#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace screenshot {

// Resolves "%USERPROFILE%\Pictures\Game Screenshots" on Windows (creating the folder if needed).
std::filesystem::path default_folder();

// Writes a 24-bit lossless PNG from packed RGBA8 (red in low byte, `width` x `height`).
bool write_png(const std::filesystem::path &path, const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height);

// Saves a clean game-only screenshot as "ShadowHeartsCovenant_YYYY-MM-DD_HH-MM-SS.png" inside default_folder().
// Returns the saved path (empty on failure). Runs PNG encoding synchronously or asynchronously via save_game_async.
std::filesystem::path save_game(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height);

// Saves an achievement screenshot asynchronously with the toast RGBA composited in the top-right corner
// as "Achievement_<SanitizedName>_<YYYY-MM-DD>.png" inside default_folder().
void save_achievement_async(const std::string &achievement_name,
                            std::vector<std::uint32_t> game_rgba, std::uint32_t game_w, std::uint32_t game_h,
                            std::vector<std::uint32_t> toast_rgba, std::uint32_t toast_w, std::uint32_t toast_h,
                            std::uint32_t win_w, std::uint32_t win_h, std::uint32_t toast_x, std::uint32_t toast_y);

// Waits for any in-flight background screenshot writes to finish cleanly before process exit.
void wait_pending();

}  // namespace screenshot
