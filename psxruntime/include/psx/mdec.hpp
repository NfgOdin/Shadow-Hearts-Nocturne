#pragma once

#include "psx/system.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace psx {

// PlayStation 1 MDEC (Macroblock Decoder at 0x1F801820 / 0x1F801824).
// Decodes RLE-compressed DCT macroblocks (Cr, Cb, Y1..Y4) via 2D IDCT and
// YCbCr->RGB conversion for .STR and .MCF FMV playback.
class Mdec final : public MdecSink {
public:
    std::array<std::uint8_t, 64> iq_y = {};
    std::array<std::uint8_t, 64> iq_uv = {};
    std::array<std::int16_t, 64> scale_table = {};

    std::uint32_t cmd_header = 0;
    std::uint32_t words_remaining = 0;
    int active_cmd = 0;  // 0=idle, 1=decode, 2=set_iq, 3=set_scale

    std::vector<std::uint16_t> in_halfwords;
    std::deque<std::uint32_t> out_fifo;
    std::uint64_t blocks_decoded = 0;

    Mdec();
    ~Mdec() override = default;

    void reset() override;
    void write_cmd(std::uint32_t word) override;
    void write_ctrl(std::uint32_t word) override;
    std::uint32_t read_data() override;
    std::uint32_t read_stat() const override;

private:
    void run_command();
    bool decode_block(std::size_t &hw_pos, std::int16_t blk[64], const std::uint8_t qt[64]);
    void idct_8x8(std::int16_t blk[64]) const;
};

}  // namespace psx
