#pragma once

#include "ps2/video.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fmvplayer {

struct VideoFrame {
    double pts_sec = 0.0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint32_t> rgba;
};

struct Stats {
    std::size_t indexed_videos = 0;
    bool enabled = true;
    bool playing = false;
    std::uint64_t active_hash = 0;
    std::uint32_t video_w = 0;
    std::uint32_t video_h = 0;
};

// Initialize/configure the HD FMV mod directory (default "mods/fmv") and scan for <16hex>*.mp4 files.
void configure(const std::filesystem::path &mods_fmv_dir, bool enabled);
void set_extra_dirs(const std::vector<std::filesystem::path> &extra_dirs);
void set_enabled(bool enabled);
bool is_enabled();
void reload();
void shutdown();

// Called once per presented frame on the present/window thread.
// Returns a thread-safe shared_ptr<const VideoFrame> when an HD .mp4 replacement is active for `info.stream_hash`
// (eliminates any raw-pointer UAF when stop()/set_enabled(false) runs concurrently).
// Returns nullptr when no replacement is active.
std::shared_ptr<const VideoFrame> poll_frame(const ps2::FmvStreamInfo &info);

// Test helper: verify Media Foundation startup/shutdown works on this host.
bool self_test_backend();

Stats stats();

}  // namespace fmvplayer
