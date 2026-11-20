#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2::png {

// Decode a PNG file on disk (8-bit RGB, RGBA, Gray, Gray+Alpha, or Indexed PLTE+tRNS) into RGBA32 words
// laid out as R | (G << 8) | (B << 16) | (A << 24).
// When ps2_alpha == true, alpha is converted from 0..255 into PS2 GS 0..128 range ((a * 128 + 127) / 255).
bool decode_rgba(const std::filesystem::path &path, std::uint32_t &out_w, std::uint32_t &out_h,
                 std::vector<std::uint32_t> &out_words, bool ps2_alpha = false);

bool decode_rgba_bytes(const std::uint8_t *data, std::size_t len, std::uint32_t &out_w, std::uint32_t &out_h,
                       std::vector<std::uint32_t> &out_words, bool ps2_alpha = false);

// Encode RGBA32 words (R | (G << 8) | (B << 16) | (A << 24)) into a PNG byte buffer or file on disk.
// When from_ps2_alpha == true, alpha is converted from PS2 GS 0..128 range into standard 0..255 (min(255, (a * 255 + 64) / 128)).
// Writes atomically via a temporary sibling file + rename.
bool encode_rgba_bytes(std::uint32_t width, std::uint32_t height, const std::uint32_t *rgba,
                       std::vector<std::uint8_t> &out_png, bool from_ps2_alpha = false);

bool encode_rgba(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height,
                 const std::uint32_t *rgba, bool from_ps2_alpha = false);

}  // namespace ps2::png
