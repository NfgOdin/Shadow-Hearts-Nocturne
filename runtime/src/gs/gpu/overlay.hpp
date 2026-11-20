#pragma once

// A picture laid over the game's picture in the window: the quick menu of the native runner (host/quickmenu.cpp). It is drawn in the
// present pass only, after the game's picture, so the game, the pictures it saves (--frames-dir, F9) and its save states never contain it.
// Called on the thread that presents (the window thread in a run with threads, the emulation thread in a run with one).

#include <cstdint>
#include <string>
#include <vector>

namespace ps2::gs::gpu {

// rgba: straight alpha, red in the low byte, `width` x `height`, its top left corner at x, y in window pixels. nullptr takes it away.
void set_overlay(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y);
// Second independent alpha-blended overlay for the top-right achievement toast (drawn above game & menu).
void set_toast_overlay(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y);
// The last picture of the game once more, with the picture look of now and the overlay: for a game that waits (the menu is open) and
// sends no new pictures. False when there is no window surface or no picture yet.
bool present_again();
// Cleanly reads back the latest displayed game picture (internal resolution, NO menu or toast overlays) for PNG screenshots.
bool capture_game_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);

// Graphics chip (adapter), graphics API, resolution limits, present mode, and live async submit thread controls for the F1 quick menu.
const std::vector<std::string> &known_adapters();
const std::string &active_adapter();
const std::string &active_backend();
std::uint32_t active_scale();
std::uint32_t max_safe_scale();
std::uint64_t estimate_vram_bytes(std::uint32_t scale);
std::string active_present_mode();
const std::vector<std::string> &supported_present_modes();
void set_present_mode(const std::string &mode);
bool submit_thread_enabled();
void set_submit_thread_enabled(bool on);
bool gpu_binning_enabled();
void set_gpu_binning_enabled(bool on);

}  // namespace ps2::gs::gpu
