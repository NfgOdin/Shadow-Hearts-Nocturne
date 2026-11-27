#pragma once

// A picture laid over the game's picture in the window: the quick menu of the native runner (host/quickmenu.cpp). It is drawn in the
// present pass only, after the game's picture, so the game, the pictures it saves (--frames-dir, F9) and its save states never contain it.
// Called on the thread that presents (the window thread in a run with threads, the emulation thread in a run with one).

#include <cstdint>
#include <filesystem>
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
// Non-blocking async achievement screenshot request & poll (zero GPU stall on the render thread):
bool request_async_game_frame();
bool poll_async_game_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);

// Graphics chip (adapter), graphics API, resolution limits, present mode, and live async submit thread controls for the F1 quick menu.
const std::vector<std::string> &known_adapters();
const std::string &active_adapter();
const std::string &active_backend();
std::uint32_t active_scale();
std::uint32_t allocated_scale_limit();
bool set_live_scale(std::uint32_t new_scale);
double recent_submit_ms();
std::uint32_t max_safe_scale();
std::uint64_t estimate_vram_bytes(std::uint32_t scale);
std::string active_present_mode();
const std::vector<std::string> &supported_present_modes();
void set_present_mode(const std::string &mode);
bool submit_thread_enabled();
void set_submit_thread_enabled(bool on);
bool gpu_binning_enabled();
void set_gpu_binning_enabled(bool on);

// HD Texture Pack Engine, Smart Runtime GPU Dumper, and HD FMV Present Layer
struct TexPackHudStats {
    std::size_t indexed_textures = 0;
    std::size_t active_packs = 0;
    std::size_t resident_textures = 0;
    std::size_t pending_decodes = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t pool_capacity_bytes = 0;
    std::size_t dumped_textures = 0;
    bool overrides_enabled = false;
    bool dump_enabled = false;
};
void configure_tex_packs(const std::string &mods_textures_dir, const std::string &dumps_textures_dir,
                         const std::vector<std::string> &pack_order_spec, bool overrides_enabled, bool dump_enabled);
void set_mod_tex_dirs(const std::vector<std::filesystem::path> &extra_dirs);
void set_tex_overrides_enabled(bool on);
bool tex_overrides_enabled();
void set_tex_dump_enabled(bool on);
bool tex_dump_enabled();
void reload_tex_packs();
TexPackHudStats tex_pack_stats();

// HD FMV video frame composited into the active viewport in present() (beneath Quick Menu & Trophy Toasts).
// Pass rgba == nullptr to disable and show the standard PS2 display blit.
void set_fmv_frame(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height, float opacity = 1.0f);

}  // namespace ps2::gs::gpu
