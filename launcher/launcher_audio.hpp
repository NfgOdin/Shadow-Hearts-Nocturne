#pragma once

// Lightweight zero-dependency 8-voice polyphonic Gothic UI & Card Duel synthesizer (WinMM waveOut).
// Pre-synthesizes harmonic crystal, brass, parchment, and Duel SFX PCM buffers in memory
// and mixes up to 8 overlapping voices cleanly without cutting off rapid sweep chimes.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace launcher {

enum class SoundCue : int {
    Hover = 0,
    Click = 1,
    TabSwitch = 2,
    LaunchPlay = 3,
    PageTurn = 4,
    CardSlam = 5,
    CardFuse = 6,
    ClashHit = 7,
    ChainBell = 8,
    WardBlock = 9,
    DuelVictory = 10,
    Count = 11
};

class ChimeAudio {
public:
    ChimeAudio();
    ~ChimeAudio();
    ChimeAudio(const ChimeAudio &) = delete;
    ChimeAudio &operator=(const ChimeAudio &) = delete;

    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }
    void set_volume(int percent);  // 0..100
    void play(SoundCue cue);

private:
    struct ActiveVoice {
        int cue_idx = -1;
        std::size_t pos = 0;
    };

    void synthesize_all();
    void ensure_mixer_started();
    void stop_mixer();
    void mixer_loop();

    bool enabled_ = true;
    int volume_ = 75;
    std::vector<std::int16_t> pcm_buffers_[static_cast<int>(SoundCue::Count)];
    std::mutex voice_mu_;
    std::vector<ActiveVoice> voices_;
    std::atomic<bool> mixer_running_{false};
    std::thread mixer_thread_;
};

}  // namespace launcher
