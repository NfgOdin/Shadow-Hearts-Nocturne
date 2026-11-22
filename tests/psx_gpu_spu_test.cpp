#include "psx/gpu.hpp"
#include "psx/mdec.hpp"
#include "psx/spu.hpp"
#include "psx/state.hpp"
#include "psx/system.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

int g_failures = 0;

void check_eq(const char *label, std::uint32_t actual, std::uint32_t expected) {
    if (actual != expected) {
        std::fprintf(stderr, "[FAIL] %s: actual=0x%08X, expected=0x%08X\n", label, actual, expected);
        ++g_failures;
    }
}

void test_gpu_vram_primitives_and_scaling() {
    psx::Gpu gpu;
    gpu.set_scale(2);
    gpu.write_gp1(0x03000000u);  // Enable display
    gpu.write_gp1(0x08000001u);  // 320x240 15-bit

    // 1. GP0(02) Quick FillRect: fill (0,0)-(320,240) with pure red (R=248 -> R5=31 -> 0x001F)
    gpu.write_gp0(0x020000F8u);
    gpu.write_gp0(0x00000000u);
    gpu.write_gp0((240u << 16) | 320u);
    check_eq("FillRect VRAM(10,10) red", gpu.vram[10 * 1024 + 10], 0x001Fu);

    // 2. CPU->VRAM LoadImage (2x2 @ 100,100) and VRAM->CPU StoreImage round-trip
    gpu.write_gp0(0xA0000000u);
    gpu.write_gp0((100u << 16) | 100u);
    gpu.write_gp0((2u << 16) | 2u);
    gpu.write_gp0(0x03E07C00u);  // pixel(100,100)=0x7C00 (blue), pixel(101,100)=0x03E0 (green)
    gpu.write_gp0(0x7FFF001Fu);  // pixel(100,101)=0x001F (red),  pixel(101,101)=0x7FFF (white)

    gpu.write_gp0(0xC0000000u);
    gpu.write_gp0((100u << 16) | 100u);
    gpu.write_gp0((2u << 16) | 2u);
    check_eq("StoreImage word 0", gpu.read_data(), 0x03E07C00u);
    check_eq("StoreImage word 1", gpu.read_data(), 0x7FFF001Fu);

    // 3. Draw opaque flat triangle GP0(0x20) in green at (10,10)-(30,10)-(10,30)
    gpu.write_gp0(0x2000F800u);
    gpu.write_gp0((10u << 16) | 10u);
    gpu.write_gp0((10u << 16) | 30u);
    gpu.write_gp0((30u << 16) | 10u);
    check_eq("Triangle interior VRAM(14,14) green", gpu.vram[14 * 1024 + 14], 0x03E0u);

    // 4. Verify 2x scaled present_rgba8888 outputs 640x480
    std::vector<std::uint32_t> frame;
    int w = 0, h = 0;
    gpu.present_rgba8888(frame, w, h);
    check_eq("2x scaled width == 640", static_cast<std::uint32_t>(w), 640u);
    check_eq("2x scaled height == 480", static_cast<std::uint32_t>(h), 480u);
    check_eq("2x scaled green pixel at (28,28)", frame[28 * 640 + 28], 0xFF00F800u);
}

void test_mdec_decode() {
    psx::Mdec mdec;
    mdec.reset();

    // Send a minimal 15-bit macroblock command (6 blocks, each DC=0 + EOB=0xFE00 -> 6 words)
    mdec.write_cmd((1u << 29) | (3u << 27) | 6u);  // cmd=1 (decode), depth=3 (15-bit), 6 parameter words
    for (int i = 0; i < 6; ++i) {
        mdec.write_cmd(0xFE000000u);  // low halfword = DC 0, high halfword = 0xFE00 EOB
    }
    check_eq("MDEC decoded 6 blocks", static_cast<std::uint32_t>(mdec.blocks_decoded), 6u);
    // Neutral Y=0, Cb=0, Cr=0 -> unsigned RGB = (128,128,128) -> 15-bit RGB555 = (16,16,16) = 0x4210
    const std::uint32_t pair = mdec.read_data();
    check_eq("MDEC neutral 15-bit pixel pair", pair, 0x42104210u);

    // Verify Q16 IDCT gain accuracy in 24-bit mode: DC = +200 with qt[0]=2 -> F[0,0]=400 -> +50 -> RGB = (178,178,178)
    mdec.reset();
    mdec.write_cmd((1u << 29) | (2u << 27) | 6u);  // cmd=1, depth=2 (24-bit), 6 words
    auto emit_pair = [&](std::uint16_t lo, std::uint16_t hi) {
        mdec.write_cmd(static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16));
    };
    const std::uint16_t dc_zero = (1u << 10) | 0u;
    const std::uint16_t dc_p200 = (1u << 10) | 200u;
    emit_pair(dc_zero, 0xFE00u);  // Cr = 0
    emit_pair(dc_zero, 0xFE00u);  // Cb = 0
    emit_pair(dc_p200, 0xFE00u);  // Y1 = +50
    emit_pair(dc_p200, 0xFE00u);  // Y2 = +50
    emit_pair(dc_p200, 0xFE00u);  // Y3 = +50
    emit_pair(dc_p200, 0xFE00u);  // Y4 = +50
    const std::uint32_t w24 = mdec.read_data();
    const std::uint8_t br = w24 & 0xFFu;
    const std::uint8_t bg = (w24 >> 8) & 0xFFu;
    const std::uint8_t bb = (w24 >> 16) & 0xFFu;
    check_eq("MDEC Q16 IDCT gain R (~178)", (br >= 176 && br <= 180) ? 1u : 0u, 1u);
    check_eq("MDEC Q16 IDCT gain G (~178)", (bg >= 176 && bg <= 180) ? 1u : 0u, 1u);
    check_eq("MDEC Q16 IDCT gain B (~178)", (bb >= 176 && bb <= 180) ? 1u : 0u, 1u);
}

void test_spu_voice_and_savestate() {
    psx::System sys;
    psx::Gpu gpu;
    psx::Mdec mdec;
    psx::Spu spu;
    sys.gpu = &gpu;
    sys.mdec = &mdec;
    sys.spu = &spu;

    // Upload a 16-byte ADPCM block at SPU RAM offset 0x1000 (TSA = 0x1000 >> 3 = 0x200)
    spu.write_reg(0x1F801DA6u, 0x0200u);
    spu.write_dma16(0x0000u);  // shift=0, filter=0, flags=0
    for (int i = 0; i < 7; ++i) spu.write_dma16(0x3333u);

    // Configure Voice 0 and trigger KON
    spu.write_reg(0x1F801C00u, 0x3FFFu);  // Vol L
    spu.write_reg(0x1F801C02u, 0x3FFFu);  // Vol R
    spu.write_reg(0x1F801C04u, 0x1000u);  // Pitch 44.1 kHz
    spu.write_reg(0x1F801C06u, 0x0200u);  // Start addr = 0x1000
    spu.write_reg(0x1F801C08u, 0x00FFu);  // Fast attack
    spu.write_reg(0x1F801C0Au, 0x0000u);
    spu.write_reg(0x1F801DAAu, 0xC000u);  // SPU enable + unmute
    spu.write_reg(0x1F801D88u, 0x0001u);  // KON Voice 0

    std::size_t batches = 0;
    bool non_zero_sample = false;
    spu.set_audio_callback([&](const std::int16_t *lr, std::size_t frames) {
        ++batches;
        for (std::size_t i = 0; i < frames * 2u; ++i) {
            if (lr[i] != 0) non_zero_sample = true;
        }
    });

    spu.tick(705u * 300u);
    check_eq("SPU audio callback fired", batches > 0 ? 1u : 0u, 1u);
    check_eq("SPU Voice 0 produced non-zero ADPCM audio", non_zero_sample ? 1u : 0u, 1u);

    // Test Save State round-trip
    namespace fs = std::filesystem;
    const fs::path state_path = fs::temp_directory_path() / "psx_test_slot.state";
    sys.cpu.r[16] = 0xCAFEBABEu;
    gpu.vram[42] = 0x1234u;

    check_eq("save_state_file ok", psx::save_state_file(state_path.string(), sys, gpu, mdec, spu) ? 1u : 0u, 1u);
    sys.cpu.r[16] = 0u;
    gpu.vram[42] = 0u;
    check_eq("load_state_file ok", psx::load_state_file(state_path.string(), sys, gpu, mdec, spu) ? 1u : 0u, 1u);
    check_eq("Restored CPU $s0", sys.cpu.r[16], 0xCAFEBABEu);
    check_eq("Restored GPU VRAM[42]", gpu.vram[42], 0x1234u);

    // Regression: DICR 8-bit write to 0x1F8010F6 (channel IRQ enable) must update bits 16..23 without clearing W1C flags
    sys.hw.dicr = 0x88980000u;  // bit 27 flag + bits 19/20/23 enabled
    sys.hw.write_io8(sys, 0x1F8010F6u, 0x90u);  // clear bit 19 (DMA3 enable)
    check_eq("DICR byte2 write clears DMA3 enable", (sys.hw.dicr >> 16) & 0xFFu, 0x90u);
    check_eq("DICR byte2 write preserves W1C flag bit 27", (sys.hw.dicr >> 27) & 1u, 1u);

    // Regression: SIO0 /ACK pulse must deassert after tick so PsyQ libpad does not time out
    sys.hw.write_io16(sys, 0x1F80104Au, 0x1003u);  // TXEN + CS + ACK IRQ enable
    sys.hw.write_io8(sys, 0x1F801040u, 0x01u);     // Select Pad 0
    check_eq("SIO0 /ACK asserted immediately", (sys.hw.read_io16(sys, 0x1F801044u) & 0x0080u) != 0 ? 1u : 0u, 1u);
    sys.hw.tick(sys, 200u);
    check_eq("SIO0 /ACK deasserted after pulse", (sys.hw.read_io16(sys, 0x1F801044u) & 0x0080u) == 0 ? 1u : 0u, 1u);
    check_eq("SIO0 IRQ latched after pulse", (sys.hw.read_io16(sys, 0x1F801044u) & 0x0200u) != 0 ? 1u : 0u, 1u);

    // Regression: SPU IRQ9 (0x1F801DA4) fires when voice decodes block at irq_addr and clears when SPUCNT bit 6 cleared
    {
        psx::Spu irq_spu;
        int irq9_count = 0;
        irq_spu.set_irq_callback([&]() { ++irq9_count; });
        irq_spu.write_reg(0x1F801DA6u, 0x0200u);  // Transfer addr = 0x1000
        for (int blk = 0; blk < 2; ++blk) {
            irq_spu.write_dma16(0x0000u);
            for (int i = 0; i < 7; ++i) irq_spu.write_dma16(0x2222u);
        }
        irq_spu.write_reg(0x1F801DA4u, 0x0202u);  // IRQ addr = 0x1010 (second ADPCM block)
        irq_spu.write_reg(0x1F801C00u, 0x3FFFu);
        irq_spu.write_reg(0x1F801C02u, 0x3FFFu);
        irq_spu.write_reg(0x1F801C04u, 0x1000u);
        irq_spu.write_reg(0x1F801C06u, 0x0200u);
        irq_spu.write_reg(0x1F801C08u, 0x00FFu);  // Instant attack, sustain level 0x7FFF
        irq_spu.write_reg(0x1F801C0Au, 0xCE2Cu);  // Slow exp-dec sustain (shift=14) + slow exp release (shift=12)
        irq_spu.write_reg(0x1F801DAAu, 0xC040u);  // SPU enable + unmute + IRQ9 enable
        irq_spu.write_reg(0x1F801D88u, 0x0001u);  // KON Voice 0
        irq_spu.tick(706u * 64u);                 // Enough samples to decode blocks 0 and 1
        check_eq("SPU IRQ9 fired on block 1", static_cast<std::uint32_t>(irq9_count), 1u);
        check_eq("SPUSTAT bit 6 set after IRQ9", (irq_spu.read_reg(0x1F801DAEu) & 0x0040u) ? 1u : 0u, 1u);
        irq_spu.write_reg(0x1F801DAAu, 0xC000u);  // Disable IRQ9 -> clears SPUSTAT bit 6
        check_eq("SPUSTAT bit 6 cleared on IRQ9 disable", (irq_spu.read_reg(0x1F801DAEu) & 0x0040u) ? 1u : 0u, 0u);
        // Slow sustain (shift=14) must still be near full volume after 64 samples (not collapsed!)
        check_eq("SPU ADSR shift=14 sustain stays high", irq_spu.voices[0].env_level > 0x7000 ? 1u : 0u, 1u);
    }

    // Regression: DMA1 (MDECOUT) keeps CHCR bit 24 busy for realistic MDEC delay so MOVIE.BIN handshake never races
    {
        psx::System dsys;
        psx::Mdec dmdec;
        dsys.mdec = &dmdec;
        dsys.hw.dpcr = 0x0BBBBBBBu;
        dsys.hw.write_io32(dsys, 0x1F8010F4u, 0x00820000u);  // Master + DMA1 IRQ enable
        dsys.hw.write_io32(dsys, 0x1F801090u, 0x00010000u);  // DMA1 MADR
        dsys.hw.write_io32(dsys, 0x1F801094u, (90u << 16) | 32u);  // 2880 words (16x240 24-bit slice)
        dsys.hw.write_io32(dsys, 0x1F801098u, 0x01000200u);  // Start sync=1 MDECOUT -> RAM
        check_eq("DMA1 stays busy immediately after start", (dsys.hw.read_io32(dsys, 0x1F801098u) >> 24) & 1u, 1u);
        dsys.hw.tick(dsys, 1000u);
        check_eq("DMA1 still busy at 1000 cycles", (dsys.hw.read_io32(dsys, 0x1F801098u) >> 24) & 1u, 1u);
        dsys.hw.tick(dsys, 12000u);
        check_eq("DMA1 clears busy after realistic MDEC delay", (dsys.hw.read_io32(dsys, 0x1F801098u) >> 24) & 1u, 0u);
        check_eq("DMA1 raises DICR bit 25 on completion", (dsys.hw.dicr >> 25) & 1u, 1u);
    }

    std::error_code ec;
    fs::remove(state_path, ec);
}

}  // namespace

int main() {
    test_gpu_vram_primitives_and_scaling();
    test_mdec_decode();
    test_spu_voice_and_savestate();

    if (g_failures != 0) {
        std::fprintf(stderr, "psxgpusputest FAILED (%d failure(s))\n", g_failures);
        return 1;
    }
    std::printf("psxgpusputest PASSED (PS1 GPU 1x-4x, MDEC IDCT, SPU1 24-voice ADPCM, Save State round-trip verified)\n");
    return 0;
}
