#pragma once

// A game controller's analog stick as the console's pad bytes. No SDL here, so a test can run it without a window.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace stick_map {

// The stick's two axes as SDL reports them (-32768..32767, 0 in the middle) become the two pad bytes (0..255, 0x80 in the middle).
//
// A modern pad's stick moves in a round gate: pushed all the way on a diagonal it reads about 0.71 on each axis, and many physical
// sticks stop around 85% of the full radius on diagonals. A console pad reads close to the corner of the square there, and games
// that subtract a per-axis box deadzone (Shadow Hearts: Covenant subtracts 48 per axis and requires >= 95% of the remaining 80 to
// run) or test each axis on its own (Harvest Moon: A Wonderful Life) otherwise drop from running to walking on diagonals. So the
// push is stretched from the circle to the square, and the outer half of the radial travel (16384..27852) ramps to full deflection
// (32768) while the inner half (0..16384) stays untouched for walking and fine aim.
inline void to_bytes(int vx, int vy, std::uint8_t &x, std::uint8_t &y) {
    const int m = std::max(std::abs(vx), std::abs(vy));
    const double r = std::hypot(static_cast<double>(vx), static_cast<double>(vy));
    const double eff_r = r > 16384.0 ? std::min(16384.0 + (r - 16384.0) * (16384.0 / (27852.0 - 16384.0)), 32768.0) : r;
    const double scale = m ? eff_r / m : 1.0;
    const auto byte = [scale](int v) {
        const long s = std::clamp(std::lround(v * scale), -32768L, 32767L);
        return static_cast<std::uint8_t>((s + 32768) >> 8);
    };
    x = byte(vx);
    y = byte(vy);
}

}  // namespace stick_map
