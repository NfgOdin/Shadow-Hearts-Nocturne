#pragma once

// Lightweight zero-dependency harmonic UI chime synthesizer (WinMM waveOut).
// Pre-synthesizes subtle Gothic crystal & brass cues in memory at startup.

#include <cstdint>
#include <vector>

namespace launcher {

enum class SoundCue : int {
    Hover = 0,
    Click = 1,
    TabSwitch = 2,
    LaunchPlay = 3,
    Count = 4
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
    void synthesize_all();
    bool enabled_ = true;
    int volume_ = 75;
    std::vector<std::uint8_t> wav_buffers_[static_cast<int>(SoundCue::Count)];
};

}  // namespace launcher
