#include "launcher_audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace launcher {
namespace {

constexpr int kSampleRate = 44100;
constexpr double kTwoPi = 6.28318530717958647692;

std::vector<std::uint8_t> build_wav(const std::vector<std::int16_t> &samples) {
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t riff_size = 36u + data_bytes;
    std::vector<std::uint8_t> wav(44u + data_bytes);
    auto put32 = [&](std::size_t off, std::uint32_t v) { std::memcpy(wav.data() + off, &v, 4); };
    auto put16 = [&](std::size_t off, std::uint16_t v) { std::memcpy(wav.data() + off, &v, 2); };

    std::memcpy(wav.data() + 0, "RIFF", 4);
    put32(4, riff_size);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put32(16, 16u);
    put16(20, 1u);   // PCM
    put16(22, 1u);   // Mono
    put32(24, static_cast<std::uint32_t>(kSampleRate));
    put32(28, static_cast<std::uint32_t>(kSampleRate * 2));
    put16(32, 2u);
    put16(34, 16u);
    std::memcpy(wav.data() + 36, "data", 4);
    put32(40, data_bytes);
    if (data_bytes > 0) std::memcpy(wav.data() + 44, samples.data(), data_bytes);
    return wav;
}

std::vector<std::int16_t> synth_chord(double duration_s, double gain,
                                      const std::vector<std::pair<double, double>> &partials,
                                      double attack_s = 0.004, double decay_rate = 14.0) {
    const std::size_t n = static_cast<std::size_t>(duration_s * kSampleRate);
    std::vector<std::int16_t> pcm(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
        const double env_a = attack_s > 0.0 ? std::min(1.0, t / attack_s) : 1.0;
        const double env_d = std::exp(-t * decay_rate);
        const double tail = (duration_s - t < 0.01) ? std::max(0.0, (duration_s - t) / 0.01) : 1.0;
        double s = 0.0;
        for (const auto &[freq, amp] : partials) s += amp * std::sin(kTwoPi * freq * t);
        const double v = std::clamp(s * env_a * env_d * tail * gain, -0.95, 0.95);
        pcm[i] = static_cast<std::int16_t>(v * 32767.0);
    }
    return pcm;
}

std::vector<std::int16_t> synth_page_turn(double gain) {
    // Realistic 170ms parchment page-rustle: warm band-passed noise with a soft paper-lift swell
    const double duration_s = 0.175;
    const std::size_t n = static_cast<std::size_t>(duration_s * kSampleRate);
    std::vector<std::int16_t> pcm(n);
    std::uint32_t rng = 0x5A17B00Du;
    double lp1 = 0.0, lp2 = 0.0, hp = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
        const double u = t / duration_s;
        // Two-peak organic paper-flip envelope (lift + settle)
        const double lift = std::sin(3.1415926535 * std::min(1.0, u * 1.45));
        const double settle = (u > 0.45) ? 0.55 * std::sin(3.1415926535 * (u - 0.45) / 0.55) : 0.0;
        const double env = std::max(0.0, lift * lift * 0.85 + settle * settle);

        rng = rng * 1664525u + 1013904223u;
        const double white = (static_cast<double>((rng >> 8) & 0xFFFFu) / 32768.0) - 1.0;
        // Warm paper band-pass (~900 Hz - 3.2 kHz)
        lp1 += 0.24 * (white - lp1);
        lp2 += 0.14 * (lp1 - lp2);
        hp += 0.04 * (lp2 - hp);
        const double band = lp2 - hp;
        const double v = std::clamp(band * env * gain * 2.2, -0.95, 0.95);
        pcm[i] = static_cast<std::int16_t>(v * 32767.0);
    }
    return pcm;
}

}  // namespace

ChimeAudio::ChimeAudio() { synthesize_all(); }

ChimeAudio::~ChimeAudio() {
#ifdef _WIN32
    PlaySoundW(nullptr, nullptr, 0);
#endif
}

void ChimeAudio::set_volume(int percent) {
    const int clamped = std::clamp(percent, 0, 100);
    if (clamped == volume_) return;
#ifdef _WIN32
    // Stop any in-flight async SND_MEMORY playback before reallocating wav_buffers_.
    PlaySoundW(nullptr, nullptr, 0);
#endif
    volume_ = clamped;
    synthesize_all();
}

void ChimeAudio::synthesize_all() {
    const double vol = static_cast<double>(volume_) / 100.0;
    // 0. Hover: ultra-subtle crystalline tick (55ms)
    wav_buffers_[0] = build_wav(synth_chord(0.055, 0.10 * vol, {{1174.66, 0.65}, {2349.32, 0.35}}, 0.002, 48.0));
    // 1. Click: warm brass-bell tap (90ms)
    wav_buffers_[1] = build_wav(synth_chord(0.095, 0.18 * vol, {{659.25, 0.60}, {987.77, 0.30}, {1975.53, 0.10}}, 0.002, 28.0));
    // 2. TabSwitch: subtle Gothic resonant fifth (130ms)
    wav_buffers_[2] = build_wav(synth_chord(0.135, 0.17 * vol, {{440.0, 0.50}, {659.25, 0.35}, {1318.51, 0.15}}, 0.004, 18.0));
    // 3. LaunchPlay: rich harmonic D-minor9/open-fifth Judgment Ring swell (460ms)
    wav_buffers_[3] = build_wav(synth_chord(0.460, 0.28 * vol, {{293.66, 0.38}, {440.0, 0.28}, {659.25, 0.20}, {880.0, 0.14}}, 0.012, 6.2));
    // 4. PageTurn: soft parchment page-rustle (175ms)
    wav_buffers_[4] = build_wav(synth_page_turn(0.24 * vol));
}

void ChimeAudio::play(SoundCue cue) {
    if (!enabled_ || volume_ <= 0) return;
    const int idx = static_cast<int>(cue);
    if (idx < 0 || idx >= static_cast<int>(SoundCue::Count)) return;
#ifdef _WIN32
    const auto &buf = wav_buffers_[idx];
    if (buf.size() >= 44) {
        PlaySoundW(reinterpret_cast<LPCWSTR>(buf.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
#else
    (void)idx;
#endif
}

}  // namespace launcher
