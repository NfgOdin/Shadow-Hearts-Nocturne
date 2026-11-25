#include "launcher_audio.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
constexpr std::size_t kMaxPolyVoices = 8;

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
    const double duration_s = 0.175;
    const std::size_t n = static_cast<std::size_t>(duration_s * kSampleRate);
    std::vector<std::int16_t> pcm(n);
    std::uint32_t rng = 0x5A17B00Du;
    double lp1 = 0.0, lp2 = 0.0, hp = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
        const double u = t / duration_s;
        const double lift = std::sin(3.1415926535 * std::min(1.0, u * 1.45));
        const double settle = (u > 0.45) ? 0.55 * std::sin(3.1415926535 * (u - 0.45) / 0.55) : 0.0;
        const double env = std::max(0.0, lift * lift * 0.85 + settle * settle);

        rng = rng * 1664525u + 1013904223u;
        const double white = (static_cast<double>((rng >> 8) & 0xFFFFu) / 32768.0) - 1.0;
        lp1 += 0.24 * (white - lp1);
        lp2 += 0.14 * (lp1 - lp2);
        hp += 0.04 * (lp2 - hp);
        const double band = lp2 - hp;
        const double v = std::clamp(band * env * gain * 2.2, -0.95, 0.95);
        pcm[i] = static_cast<std::int16_t>(v * 32767.0);
    }
    return pcm;
}

std::vector<std::int16_t> synth_arpeggio(double step_s, double gain,
                                         const std::vector<double> &notes,
                                         double decay_rate = 10.0) {
    const double total_s = step_s * static_cast<double>(notes.size()) + 0.24;
    const std::size_t n = static_cast<std::size_t>(total_s * kSampleRate);
    std::vector<std::int16_t> pcm(n, 0);
    for (std::size_t idx = 0; idx < notes.size(); ++idx) {
        const double start_t = static_cast<double>(idx) * step_s;
        const double freq = notes[idx];
        for (std::size_t i = static_cast<std::size_t>(start_t * kSampleRate); i < n; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(kSampleRate) - start_t;
            const double env = std::min(1.0, t / 0.004) * std::exp(-t * decay_rate);
            const double s = (0.65 * std::sin(kTwoPi * freq * t) + 0.35 * std::sin(kTwoPi * freq * 2.0 * t)) * env * gain;
            const double cur = static_cast<double>(pcm[i]) / 32767.0 + s;
            pcm[i] = static_cast<std::int16_t>(std::clamp(cur, -0.95, 0.95) * 32767.0);
        }
    }
    return pcm;
}

std::vector<std::int16_t> synth_impact_slam(double duration_s, double gain, double f_start, double f_end, double chime_f) {
    const std::size_t n = static_cast<std::size_t>(duration_s * kSampleRate);
    std::vector<std::int16_t> pcm(n);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
        const double u = t / duration_s;
        const double f = f_start + (f_end - f_start) * std::min(1.0, u * 2.2);
        phase += kTwoPi * f / static_cast<double>(kSampleRate);
        const double thud = std::sin(phase) * std::exp(-t * 18.0);
        const double chime = 0.35 * std::sin(kTwoPi * chime_f * t) * std::exp(-t * 22.0);
        const double v = std::clamp((thud + chime) * gain, -0.95, 0.95);
        pcm[i] = static_cast<std::int16_t>(v * 32767.0);
    }
    return pcm;
}

}  // namespace

ChimeAudio::ChimeAudio() { synthesize_all(); }

ChimeAudio::~ChimeAudio() { stop_mixer(); }

void ChimeAudio::set_volume(int percent) {
    const int clamped = std::clamp(percent, 0, 100);
    if (clamped == volume_) return;
    std::lock_guard<std::mutex> lk(voice_mu_);
    volume_ = clamped;
    voices_.clear();
    synthesize_all();
}

void ChimeAudio::synthesize_all() {
    const double vol = static_cast<double>(volume_) / 100.0;
    pcm_buffers_[0]  = synth_chord(0.055, 0.10 * vol, {{1174.66, 0.65}, {2349.32, 0.35}}, 0.002, 48.0);
    pcm_buffers_[1]  = synth_chord(0.095, 0.18 * vol, {{659.25, 0.60}, {987.77, 0.30}, {1975.53, 0.10}}, 0.002, 28.0);
    pcm_buffers_[2]  = synth_chord(0.135, 0.17 * vol, {{440.0, 0.50}, {659.25, 0.35}, {1318.51, 0.15}}, 0.004, 18.0);
    pcm_buffers_[3]  = synth_chord(0.460, 0.28 * vol, {{293.66, 0.38}, {440.0, 0.28}, {659.25, 0.20}, {880.0, 0.14}}, 0.012, 6.2);
    pcm_buffers_[4]  = synth_page_turn(0.24 * vol);
    pcm_buffers_[5]  = synth_impact_slam(0.16, 0.32 * vol, 185.0, 58.0, 880.0);
    pcm_buffers_[6]  = synth_arpeggio(0.055, 0.24 * vol, {329.63, 440.0, 554.37, 659.25, 880.0}, 8.5);
    pcm_buffers_[7]  = synth_impact_slam(0.18, 0.34 * vol, 260.0, 72.0, 1174.66);
    pcm_buffers_[8]  = synth_chord(0.24, 0.25 * vol, {{1046.50, 0.45}, {1567.98, 0.35}, {2093.0, 0.20}}, 0.002, 11.0);
    pcm_buffers_[9]  = synth_chord(0.20, 0.22 * vol, {{1318.51, 0.50}, {1975.53, 0.32}, {2637.02, 0.18}}, 0.002, 15.0);
    pcm_buffers_[10] = synth_arpeggio(0.085, 0.26 * vol, {293.66, 369.99, 440.0, 587.33, 739.99}, 5.2);
}

void ChimeAudio::ensure_mixer_started() {
#ifdef _WIN32
    bool expected = false;
    if (mixer_running_.compare_exchange_strong(expected, true)) {
        mixer_thread_ = std::thread(&ChimeAudio::mixer_loop, this);
    }
#endif
}

void ChimeAudio::stop_mixer() {
#ifdef _WIN32
    if (mixer_running_.exchange(false)) {
        if (mixer_thread_.joinable()) mixer_thread_.join();
    }
#endif
}

void ChimeAudio::mixer_loop() {
#ifdef _WIN32
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = kSampleRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = kSampleRate * 2;

    HWAVEOUT hwo = nullptr;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR || !hwo) {
        mixer_running_.store(false);
        return;
    }

    constexpr int kBufCount = 4;
    constexpr int kChunkSamples = 512;  // ~11.6ms latency per chunk
    std::array<std::array<std::int16_t, kChunkSamples>, kBufCount> bufs{};
    std::array<WAVEHDR, kBufCount> hdrs{};
    for (int i = 0; i < kBufCount; ++i) {
        hdrs[static_cast<std::size_t>(i)].lpData = reinterpret_cast<LPSTR>(bufs[static_cast<std::size_t>(i)].data());
        hdrs[static_cast<std::size_t>(i)].dwBufferLength = kChunkSamples * sizeof(std::int16_t);
        waveOutPrepareHeader(hwo, &hdrs[static_cast<std::size_t>(i)], sizeof(WAVEHDR));
        hdrs[static_cast<std::size_t>(i)].dwFlags |= WHDR_DONE;
    }

    int cur = 0;
    while (mixer_running_.load(std::memory_order_relaxed)) {
        WAVEHDR &h = hdrs[static_cast<std::size_t>(cur)];
        if ((h.dwFlags & WHDR_DONE) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        auto &out = bufs[static_cast<std::size_t>(cur)];
        bool had_audio = false;
        {
            std::lock_guard<std::mutex> lk(voice_mu_);
            if (!voices_.empty()) {
                had_audio = true;
                for (int s = 0; s < kChunkSamples; ++s) {
                    int acc = 0;
                    for (auto &v : voices_) {
                        const auto &src = pcm_buffers_[v.cue_idx];
                        if (v.pos < src.size()) acc += src[v.pos++];
                    }
                    out[static_cast<std::size_t>(s)] = static_cast<std::int16_t>(std::clamp(acc, -32767, 32767));
                }
                voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](const ActiveVoice &v) {
                    return v.cue_idx < 0 || v.pos >= pcm_buffers_[v.cue_idx].size();
                }), voices_.end());
            }
        }
        if (had_audio) {
            waveOutWrite(hwo, &h, sizeof(WAVEHDR));
            cur = (cur + 1) % kBufCount;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }

    waveOutReset(hwo);
    for (int i = 0; i < kBufCount; ++i) {
        waveOutUnprepareHeader(hwo, &hdrs[static_cast<std::size_t>(i)], sizeof(WAVEHDR));
    }
    waveOutClose(hwo);
#endif
}

void ChimeAudio::play(SoundCue cue) {
    if (!enabled_ || volume_ <= 0) return;
    const int idx = static_cast<int>(cue);
    if (idx < 0 || idx >= static_cast<int>(SoundCue::Count)) return;
#ifdef _WIN32
    ensure_mixer_started();
    std::lock_guard<std::mutex> lk(voice_mu_);
    if (voices_.size() >= kMaxPolyVoices) voices_.erase(voices_.begin());
    voices_.push_back({idx, 0});
#else
    (void)idx;
#endif
}

}  // namespace launcher
