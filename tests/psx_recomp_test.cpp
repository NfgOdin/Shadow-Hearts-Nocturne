#include "psx/recomp_helpers.hpp"
#include "psx/system.hpp"
#include "analysis.hpp"
#include "codegen.hpp"
#include "psx_exe.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void check_eq(const char *label, std::uint32_t actual, std::uint32_t expected) {
    if (actual != expected) {
        std::fprintf(stderr, "[FAIL] %s: actual=0x%08X, expected=0x%08X\n", label, actual, expected);
        ++g_failures;
    }
}

// Helper to build a synthetic PS-X EXE byte buffer from a vector of 32-bit MIPS instructions.
std::vector<std::uint8_t> make_psx_exe(std::uint32_t load_addr, std::uint32_t entry_pc,
                                       const std::vector<std::uint32_t> &words) {
    const std::uint32_t text_size = static_cast<std::uint32_t>(((words.size() * 4u) + 0x7FFu) & ~0x7FFu);
    std::vector<std::uint8_t> exe(0x800u + text_size, 0);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    auto put32 = [&](std::size_t off, std::uint32_t v) { std::memcpy(exe.data() + off, &v, 4); };
    put32(0x10, entry_pc);
    put32(0x14, 0x80020000u);
    put32(0x18, load_addr);
    put32(0x1C, text_size);
    put32(0x30, 0x801FFF00u);
    std::memcpy(exe.data() + 0x800u, words.data(), words.size() * 4u);
    return exe;
}

void test_load_delay_unaligned_and_div() {
    // Program at 0x80010000:
    //  0: lui  $a0, 0x8001
    //  1: ori  $a0, $a0, 0x1000        # $a0 = 0x80011000
    //  2: li   $t0, 0xAABBCCDD
    //  3: lui  $t0, 0xAABB
    //  4: ori  $t0, $t0, 0xCCDD
    //  5: sw   $t0, 0($a0)             # [0x80011000] = 0xAABBCCDD
    //  6: addiu $v0, $zero, 0x1234     # $v0 = 0x1234
    //  7: lw   $v0, 0($a0)             # delayed load into $v0 (0xAABBCCDD)
    //  8: addu $v1, $v0, $zero         # MUST see old $v0 (0x1234) due to R3000A load-delay slot!
    //  9: addu $s0, $v0, $zero         # MUST see new $v0 (0xAABBCCDD) after load-delay!
    // 10: swl  $t0, 4($a0)             # unaligned store left/right at 0x80011001..0x80011004
    // 11: swr  $t0, 1($a0)
    // 12: lwl  $s1, 4($a0)             # unaligned load back into $s1 (consecutive LWL/LWR forwarding!)
    // 13: lwr  $s1, 1($a0)
    // 14: nop
    // 15: li   $t1, -2147483648 (lui $t1, 0x8000)
    // 16: addiu $t2, $zero, -1
    // 17: div  $t1, $t2                # INT32_MIN / -1 edge case -> LO=0x80000000, HI=0
    // 18: mflo $s2
    // 19: jr   $ra
    // 20: nop
    const std::vector<std::uint32_t> code = {
        0x3C048001u,  // lui $a0, 0x8001
        0x34841000u,  // ori $a0, $a0, 0x1000
        0x3C08AABBu,  // lui $t0, 0xAABB
        0x3508CCDDu,  // ori $t0, $t0, 0xCCDD
        0xAC880000u,  // sw  $t0, 0($a0)
        0x24021234u,  // addiu $v0, $zero, 0x1234
        0x8C820000u,  // lw  $v0, 0($a0)
        0x00401821u,  // addu $v1, $v0, $zero  (load delay slot!)
        0x00408021u,  // addu $s0, $v0, $zero
        0xA8880004u,  // swl $t0, 4($a0)
        0xB8880001u,  // swr $t0, 1($a0)
        0x88910004u,  // lwl $s1, 4($a0)
        0x98910001u,  // lwr $s1, 1($a0)
        0x00000000u,  // nop
        0x3C098000u,  // lui $t1, 0x8000
        0x240AFFFFu,  // addiu $t2, $zero, -1
        0x012A001Au,  // div $t1, $t2
        0x00009012u,  // mflo $s2
        0x03E00008u,  // jr $ra
        0x00000000u,  // nop
    };

    const auto exe_bytes = make_psx_exe(0x80010000u, 0x80010000u, code);
    psx::System sys;
    if (!sys.load_psx_exe_bytes(exe_bytes.data(), exe_bytes.size())) {
        std::fprintf(stderr, "[FAIL] load_psx_exe_bytes failed\n");
        ++g_failures;
        return;
    }

    psx::call_guest_function(sys, 0x80010000u, 1000u);
    check_eq("Load-delay slot saw old $v0", sys.cpu.r[3], 0x00001234u);
    check_eq("After load-delay saw new $v0", sys.cpu.r[16], 0xAABBCCDDu);
    check_eq("SWL/SWR + LWL/LWR unaligned round-trip", sys.cpu.r[17], 0xAABBCCDDu);
    check_eq("DIV INT32_MIN / -1 LO", sys.cpu.r[18], 0x80000000u);

    // Also run psxrecomp analyzer + codegen on this binary and verify load-delay variable `_ld` is emitted!
    psxrecomp::PsxBinary bin;
    bin.name = "test.exe";
    bin.entry_pc = 0x80010000u;
    bin.load_addr = 0x80010000u;
    bin.text_size = static_cast<std::uint32_t>(code.size() * 4u);
    bin.bytes.resize(bin.text_size);
    std::memcpy(bin.bytes.data(), code.data(), bin.text_size);

    const auto analysis = psxrecomp::analyze_binary(bin);
    check_eq("psxrecomp discovered entry function", static_cast<std::uint32_t>(analysis.functions.size()), 1u);
    if (!analysis.functions.empty()) {
        const std::string cpp = psxrecomp::emit_function_cpp(bin, analysis.functions[0], analysis.function_starts);
        if (cpp.find("_ld = c.r[2]") == std::string::npos) {
            std::fprintf(stderr, "[FAIL] psxrecomp codegen did not emit load-delay preservation _ld for $v0:\n%s\n", cpp.c_str());
            ++g_failures;
        }
    }
}

void test_bios_events_memcard_and_otc_dma() {
    psx::System sys;
    sys.reset();

    // 1. Test DMA6 OTC linked-list initialization
    sys.hw.write_io32(sys, 0x1F8010E0u, 0x0001200Cu);  // MADR = 0x1200C (4 entries: 0x1200C, 0x12008, 0x12004, 0x12000)
    sys.hw.write_io32(sys, 0x1F8010E4u, 4u);           // BCR = 4 words
    sys.hw.write_io32(sys, 0x1F8010E8u, 0x11000002u);  // CHCR: start + trigger + reverse
    check_eq("DMA6 OTC entry 3 -> 2", sys.mem.ram32(0x1200Cu), 0x00012008u);
    check_eq("DMA6 OTC entry 2 -> 1", sys.mem.ram32(0x12008u), 0x00012004u);
    check_eq("DMA6 OTC entry 1 -> 0", sys.mem.ram32(0x12004u), 0x00012000u);
    check_eq("DMA6 OTC entry 0 -> end", sys.mem.ram32(0x12000u), 0x00FFFFFFu);

    // 2. Test BIOS OpenEvent / EnableEvent / DeliverEvent / TestEvent via B0 vector
    sys.cpu.r[4] = 0xF0000001u;  // VBlank class
    sys.cpu.r[5] = 0x0002u;      // IRQ spec
    sys.cpu.r[6] = 0x2000u;      // No-callback polling mode
    sys.cpu.r[7] = 0u;
    sys.cpu.r[9] = 0x08u;        // B0:08 OpenEvent
    sys.bios.handle_vector(sys, 0xB0u);
    const std::uint32_t ev = sys.cpu.r[2];
    check_eq("OpenEvent handle valid", (ev & 0xFFFF0000u), 0xF1000000u);

    sys.cpu.r[4] = ev;
    sys.cpu.r[9] = 0x0Cu;        // B0:0C EnableEvent
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("EnableEvent ok", sys.cpu.r[2], 1u);

    sys.cpu.r[4] = 0xF0000001u;
    sys.cpu.r[5] = 0x0002u;
    sys.cpu.r[9] = 0x07u;        // B0:07 DeliverEvent
    sys.bios.handle_vector(sys, 0xB0u);

    sys.cpu.r[4] = ev;
    sys.cpu.r[9] = 0x0Bu;        // B0:0B TestEvent
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("TestEvent ready after DeliverEvent", sys.cpu.r[2], 1u);

    // Second TestEvent without re-delivery should return 0
    sys.cpu.r[4] = ev;
    sys.cpu.r[9] = 0x0Bu;
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("TestEvent consumed", sys.cpu.r[2], 0u);
}

}  // namespace

int main() {
    test_load_delay_unaligned_and_div();
    test_bios_events_memcard_and_otc_dma();

    if (g_failures != 0) {
        std::fprintf(stderr, "psxrecomptest FAILED (%d failure(s))\n", g_failures);
        return 1;
    }
    std::printf("psxrecomptest PASSED (load-delay, LWL/LWR/SWL/SWR, DIV edge cases, DMA6 OTC, BIOS events verified)\n");
    return 0;
}
