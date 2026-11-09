#include "Sound.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <numbers>
#include <random>
#include <utility>

namespace {

constexpr int Rate = 44100;
constexpr double TwoPi = 2 * std::numbers::pi;
constexpr std::size_t MaxVoices = 40;
constexpr float Headroom = 30000.0f / 32768.0f;

using Samples = std::vector<float>;

Samples Silence(double seconds) { return Samples(static_cast<std::size_t>(seconds * Rate), 0.0f); }

void Add(Samples& target, const Samples& source, double at, float gain = 1)
{
    const auto offset = static_cast<std::size_t>(at * Rate);
    if (target.size() < offset + source.size())
        target.resize(offset + source.size(), 0.0f);
    for (std::size_t i = 0; i < source.size(); ++i)
        target[offset + i] += source[i] * gain;
}

// Scaled so that the loudest sample is `peak`.
Samples Normalized(Samples samples, float peak)
{
    float loudest = 1e-6f;
    for (float sample : samples)
        loudest = std::max(loudest, std::abs(sample));
    for (float& sample : samples)
        sample *= peak / loudest;
    return samples;
}

struct Mode {
    double frequency, gain, decay; // Hz, amplitude, seconds to fall to 1/e
};

// A struck body: a burst of noise, the higher part of it for a hard click, and damped resonances.
Samples Strike(double length, std::initializer_list<Mode> modes, double click, double clickDecay, double brightness, std::uint32_t seed)
{
    Samples samples = Silence(length);
    std::minstd_rand random(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    float low = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double t = static_cast<double>(i) / Rate;
        const float white = noise(random);
        low += (white - low) * 0.08f;
        const float bright = white - low;
        double value = click * (brightness * bright + (1 - brightness) * 3 * low) * std::exp(-t / clickDecay);
        for (const Mode& mode : modes)
            value += mode.gain * std::sin(TwoPi * mode.frequency * t) * std::exp(-t / mode.decay);
        samples[i] = static_cast<float>(value * std::min(1.0, t / 0.0003));
    }
    return samples;
}

// A small bell: inharmonic partials dying away at their own rates.
Samples Bell(double frequency, double length)
{
    Samples samples = Silence(length);
    const double partials[4][3] = {{1.0, 1.0, 1.0}, {2.0, 0.45, 0.6}, {2.76, 0.3, 0.4}, {5.4, 0.12, 0.25}};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double t = static_cast<double>(i) / Rate;
        double value = 0;
        for (const auto& [ratio, gain, decay] : partials)
            value += gain * std::sin(TwoPi * frequency * ratio * t) * std::exp(-t / (length * 0.35 * decay));
        samples[i] = static_cast<float>(value * std::min(1.0, t / 0.004));
    }
    return samples;
}

// Two hard little bodies knocking together: a die on a die, a checker on a checker.
Samples Knock(std::uint32_t seed, double pitch)
{
    return Strike(0.07, {{2900 * pitch, 0.8, 0.004}, {4700 * pitch, 0.45, 0.002}, {1700 * pitch, 0.3, 0.006}}, 1.3, 0.0006, 0.85, seed);
}

} // namespace

SoundBank::SoundBank()
{
    auto set = [&](Effect effect, Samples samples, float peak) { m_samples[std::to_underlying(effect)] = Normalized(std::move(samples), peak); };

    // Dice shaken in the hand as they leave it: quick knocks of bone on bone.
    std::minstd_rand random(23);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    Samples rattle = Silence(0.3);
    for (int i = 0; i < 7; ++i)
        Add(rattle, Knock(static_cast<std::uint32_t>(40 + i), 0.85 + 0.35 * uniform(random)), 0.025 * i + 0.015 * uniform(random),
            static_cast<float>(0.3 + 0.6 * uniform(random)));
    set(Effect::Rattle, std::move(rattle), 0.35f);

    // A die striking the board: a hard click and the wood answering under it.
    set(Effect::Die, Strike(0.13, {{2150, 0.55, 0.004}, {3900, 0.3, 0.002}, {760, 0.45, 0.014}, {330, 0.35, 0.024}}, 1.1, 0.0008, 0.7, 5), 0.6f);
    // A checker set down on the board: a short, woody knock.
    set(Effect::Checker, Strike(0.14, {{980, 0.6, 0.009}, {1720, 0.32, 0.004}, {440, 0.42, 0.018}, {260, 0.25, 0.026}}, 0.8, 0.0012, 0.5, 7), 0.55f);
    // A lone checker knocked off: the clack of checker on checker over the knock on the board.
    Samples hit = Strike(0.14, {{980, 0.5, 0.009}, {440, 0.35, 0.018}}, 0.7, 0.0012, 0.5, 9);
    Add(hit, Knock(11, 0.62), 0.0, 1.3f);
    set(Effect::Hit, std::move(hit), 0.65f);
    // A checker dropped into the felt of the tray: a dull thud.
    set(Effect::Tray, Strike(0.2, {{190, 0.8, 0.022}, {360, 0.4, 0.012}, {760, 0.15, 0.006}}, 0.45, 0.006, 0.15, 13), 0.5f);

    // The checkers set out for a new game: one after another onto the board.
    Samples setup = Silence(0.6);
    for (int i = 0; i < 10; ++i)
        Add(setup, Strike(0.1, {{980 * (0.92 + 0.16 * uniform(random)), 0.6, 0.008}, {440, 0.4, 0.016}}, 0.7, 0.001, 0.5, static_cast<std::uint32_t>(60 + i)),
            0.045 * i + 0.02 * uniform(random), static_cast<float>(0.35 + 0.5 * uniform(random)));
    set(Effect::Setup, std::move(setup), 0.4f);

    Samples click = Silence(0.03);
    for (std::size_t i = 0; i < click.size(); ++i) {
        const double t = static_cast<double>(i) / Rate;
        click[i] = static_cast<float>(std::sin(TwoPi * 2600 * t) * std::exp(-t / 0.004));
    }
    set(Effect::Click, std::move(click), 0.18f);

    Samples noMove = Silence(0.9);
    Add(noMove, Bell(392.0, 0.6), 0.0, 1.0f);
    Add(noMove, Bell(311.1, 0.7), 0.16, 1.0f);
    set(Effect::NoMove, std::move(noMove), 0.2f);

    Samples chord = Silence(1.8);
    const double notes[4] = {523.25, 659.25, 783.99, 1046.5};
    for (int i = 0; i < 4; ++i)
        Add(chord, Bell(notes[i], 1.5), i * 0.11, 0.8f);
    set(Effect::Win, std::move(chord), 0.35f);

    m_voices.reserve(MaxVoices);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "3D Nardy: no sound: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec wanted{};
    wanted.freq = Rate;
    wanted.format = AUDIO_F32SYS;
    wanted.channels = 2;
    wanted.samples = 512; // about 12 ms
    wanted.callback = &SoundBank::Mix;
    wanted.userdata = this;
    // SDL converts the samples to whatever the device takes.
    m_device = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    if (m_device == 0) {
        std::fprintf(stderr, "3D Nardy: no sound: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    SDL_PauseAudioDevice(m_device, 0);
}

SoundBank::~SoundBank()
{
    if (m_device != 0) {
        SDL_CloseAudioDevice(m_device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void SoundBank::Play(Effect effect, float volume, float pan, float pitch)
{
    if (!m_enabled || m_device == 0 || volume <= 0.001f)
        return;
    pan = std::clamp(pan, -1.0f, 1.0f);
    Voice voice;
    voice.samples = &m_samples[std::to_underlying(effect)];
    voice.rate = std::clamp(pitch, 0.25f, 4.0f);
    voice.left = volume * std::min(1.0f, 1 - pan);
    voice.right = volume * std::min(1.0f, 1 + pan);
    SDL_LockAudioDevice(m_device);
    if (m_voices.size() >= MaxVoices)
        m_voices.erase(m_voices.begin());
    m_voices.push_back(voice);
    SDL_UnlockAudioDevice(m_device);
}

void SoundBank::SetEnabled(bool enabled)
{
    m_enabled = enabled;
    // Turned off, it falls silent at once.
    if (!enabled && m_device != 0) {
        SDL_LockAudioDevice(m_device);
        m_voices.clear();
        SDL_UnlockAudioDevice(m_device);
    }
}

void SoundBank::Mix(void* bank, std::uint8_t* stream, int bytes)
{
    auto& voices = static_cast<SoundBank*>(bank)->m_voices;
    float* out = reinterpret_cast<float*>(stream);
    const std::size_t frames = static_cast<std::size_t>(bytes) / (2 * sizeof(float));
    std::fill_n(out, frames * 2, 0.0f);
    for (Voice& voice : voices) {
        const std::vector<float>& samples = *voice.samples;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto index = static_cast<std::size_t>(voice.position);
            if (index + 1 >= samples.size()) {
                voice.position = static_cast<double>(samples.size());
                break;
            }
            const float fraction = static_cast<float>(voice.position - static_cast<double>(index));
            const float sample = samples[index] + (samples[index + 1] - samples[index]) * fraction;
            out[frame * 2] += sample * voice.left;
            out[frame * 2 + 1] += sample * voice.right;
            voice.position += voice.rate;
        }
    }
    std::erase_if(voices, [](const Voice& voice) { return voice.position + 1 >= static_cast<double>(voice.samples->size()); });
    for (std::size_t i = 0; i < frames * 2; ++i) {
        // A gentle limit, so that a crowd of knocks does not crackle, and a little room below full scale.
        const float x = out[i];
        out[i] = std::clamp(x / (1 + std::abs(x) * 0.35f), -1.0f, 1.0f) * Headroom;
    }
}
