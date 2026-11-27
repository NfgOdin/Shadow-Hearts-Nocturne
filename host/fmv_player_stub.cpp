#include "fmv_player.hpp"

#ifndef _WIN32

namespace fmvplayer {

void configure(const std::filesystem::path &, bool) {}
void set_extra_dirs(const std::vector<std::filesystem::path> &) {}
void set_enabled(bool) {}
bool is_enabled() { return false; }
void reload() {}
void shutdown() {}
std::shared_ptr<const VideoFrame> poll_frame(const ps2::FmvStreamInfo &) { return nullptr; }
bool self_test_backend() { return true; }
Stats stats() { return Stats{}; }

}  // namespace fmvplayer

#endif
