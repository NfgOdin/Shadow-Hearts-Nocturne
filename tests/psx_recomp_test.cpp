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

    // 3. Test PS1 kernel Table of Tables DCB ("bu") + TCB setup and _bu_init recovery for legacy save states
    const std::uint32_t dcb_ptr = sys.mem.ram32(0x150u);
    const std::uint32_t dcb_sz = sys.mem.ram32(0x154u);
    check_eq("DCB table size = 10 * 0x50", dcb_sz, 0x320u);
    const std::uint32_t bu_name_ptr = sys.mem.ram32(dcb_ptr & psx::kRamMask);
    check_eq("DCB[0].name[0] == 'b'", sys.mem.ram[bu_name_ptr & psx::kRamMask], static_cast<std::uint8_t>('b'));
    check_eq("DCB[0].name[1] == 'u'", sys.mem.ram[(bu_name_ptr + 1u) & psx::kRamMask], static_cast<std::uint8_t>('u'));
    check_eq("TCB table pointer non-zero", sys.mem.ram32(0x110u) != 0u ? 1u : 0u, 1u);

    // Zero [0x150] (simulating a pre-fix save state) and verify _bu_init (A0:70) restores it
    std::uint32_t zero = 0u;
    std::memcpy(sys.mem.ram + 0x150u, &zero, 4);
    sys.cpu.r[9] = 0x70u;
    sys.bios.handle_vector(sys, 0xA0u);
    check_eq("_bu_init restores DCB ptr", sys.mem.ram32(0x150u), dcb_ptr);

    // 4. Test Memory Card async SwCard events, fd=2 write (not swallowed by stderr), O_NOWAIT read/write,
    //    firstfile, erase, and fresh-card _card_info -> EvSpNEW -> _card_write(0, 63, 0) -> EvSpIOE.
    sys.hw.memcards[0].present = true;
    sys.hw.memcards[0].format_empty();

    auto open_ev = [&](std::uint32_t spec) -> std::uint32_t {
        sys.cpu.r[4] = 0xF4000001u;  // SwCard
        sys.cpu.r[5] = spec;
        sys.cpu.r[6] = 0x2000u;      // Poll mode
        sys.cpu.r[7] = 0u;
        sys.cpu.r[9] = 0x08u;
        sys.bios.handle_vector(sys, 0xB0u);
        const std::uint32_t h = sys.cpu.r[2];
        sys.cpu.r[4] = h;
        sys.cpu.r[9] = 0x0Cu;
        sys.bios.handle_vector(sys, 0xB0u);
        return h;
    };
    auto test_ev = [&](std::uint32_t h) -> std::uint32_t {
        sys.cpu.r[4] = h;
        sys.cpu.r[9] = 0x0Bu;
        sys.bios.handle_vector(sys, 0xB0u);
        return sys.cpu.r[2];
    };

    const std::uint32_t ev_ioe = open_ev(0x0004u);
    const std::uint32_t ev_new = open_ev(0x2000u);

    // Fresh card: _card_info(0) must defer EvSpNEW until next VBlank
    sys.cpu.r[4] = 0u;
    sys.cpu.r[9] = 0xABu;
    sys.bios.handle_vector(sys, 0xA0u);
    check_eq("_card_info fresh deferred before VBlank", test_ev(ev_new), 0u);
    sys.bios.on_vblank(sys);
    check_eq("_card_info fresh delivers EvSpNEW on VBlank", test_ev(ev_new), 1u);

    // PsyQ _card_clear calls _card_write(0, 63, NULL): clears fresh flag without corrupting sector 63
    sys.cpu.r[4] = 0u;
    sys.cpu.r[5] = 63u;
    sys.cpu.r[6] = 0u;
    sys.cpu.r[9] = 0x4Eu;
    sys.bios.handle_vector(sys, 0xB0u);
    sys.bios.on_vblank(sys);
    check_eq("_card_write(0,63,0) delivers EvSpIOE", test_ev(ev_ioe), 1u);
    check_eq("sector 63 preserved 'M'", sys.hw.memcards[0].data[63u * 128u], static_cast<std::uint8_t>('M'));

    // Subsequent _card_info(0) now delivers EvSpIOE
    sys.cpu.r[4] = 0u;
    sys.cpu.r[9] = 0xABu;
    sys.bios.handle_vector(sys, 0xA0u);
    sys.bios.on_vblank(sys);
    check_eq("_card_info cleared delivers EvSpIOE", test_ev(ev_ioe), 1u);

    // Create, async-write on fd=2, firstfile, async-read back, and erase
    const char kSaveName[] = "bu00:BASLUS-01051KDL00001";
    constexpr std::uint32_t kPathAddr = 0x80020000u;
    constexpr std::uint32_t kWriteBuf = 0x80020100u;
    constexpr std::uint32_t kReadBuf  = 0x80020300u;
    constexpr std::uint32_t kDirBuf   = 0x80020500u;
    std::memcpy(sys.mem.ram + (kPathAddr & psx::kRamMask), kSaveName, sizeof(kSaveName));
    for (std::uint32_t i = 0; i < 256u; ++i) {
        sys.mem.ram[(kWriteBuf + i) & psx::kRamMask] = static_cast<std::uint8_t>((i * 7u) ^ 0x5Au);
    }

    // open(O_CREAT | 1 block) -> fd = 2
    sys.cpu.r[4] = kPathAddr;
    sys.cpu.r[5] = 0x00010200u;
    sys.cpu.r[9] = 0x32u;
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("open(O_CREAT) returns fd=2", sys.cpu.r[2], 2u);
    sys.cpu.r[4] = 2u;
    sys.cpu.r[9] = 0x36u;  // close(2)
    sys.bios.handle_vector(sys, 0xB0u);

    // open(O_NOWAIT | 1) -> fd = 2, then write(2, kWriteBuf, 256)
    sys.cpu.r[4] = kPathAddr;
    sys.cpu.r[5] = 0x00008001u;
    sys.cpu.r[9] = 0x32u;
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("open(O_NOWAIT) returns fd=2", sys.cpu.r[2], 2u);

    sys.cpu.r[4] = 2u;
    sys.cpu.r[5] = kWriteBuf;
    sys.cpu.r[6] = 256u;
    sys.cpu.r[9] = 0x35u;  // write(fd=2, ...)
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("async write(fd=2) returns 0", sys.cpu.r[2], 0u);
    check_eq("async write defers EvSpIOE", test_ev(ev_ioe), 0u);
    sys.bios.on_vblank(sys);
    check_eq("async write delivers EvSpIOE on VBlank", test_ev(ev_ioe), 1u);
    check_eq("fd=2 wrote byte 0 to block 1", sys.hw.memcards[0].data[8192u], static_cast<std::uint8_t>(0x5Au));
    check_eq("fd=2 wrote byte 255 to block 1", sys.hw.memcards[0].data[8192u + 255u],
             static_cast<std::uint8_t>((255u * 7u) ^ 0x5Au));
    sys.cpu.r[4] = 2u;
    sys.cpu.r[9] = 0x36u;
    sys.bios.handle_vector(sys, 0xB0u);

    // firstfile("bu00:BASLUS-01051KDL00001") finds it
    sys.cpu.r[4] = kPathAddr;
    sys.cpu.r[5] = kDirBuf;
    sys.cpu.r[9] = 0x42u;
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("firstfile finds created save", sys.cpu.r[2], kDirBuf);

    // open(O_NOWAIT) + read(2, kReadBuf, 256) reads exact bytes back
    sys.cpu.r[4] = kPathAddr;
    sys.cpu.r[5] = 0x00008002u;
    sys.cpu.r[9] = 0x32u;
    sys.bios.handle_vector(sys, 0xB0u);
    sys.cpu.r[4] = 2u;
    sys.cpu.r[5] = kReadBuf;
    sys.cpu.r[6] = 256u;
    sys.cpu.r[9] = 0x34u;
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("async read(fd=2) returns 0", sys.cpu.r[2], 0u);
    sys.bios.on_vblank(sys);
    check_eq("async read delivers EvSpIOE", test_ev(ev_ioe), 1u);
    check_eq("read byte 0 matches", sys.mem.ram[kReadBuf & psx::kRamMask], static_cast<std::uint8_t>(0x5Au));
    check_eq("read byte 255 matches", sys.mem.ram[(kReadBuf + 255u) & psx::kRamMask],
             static_cast<std::uint8_t>((255u * 7u) ^ 0x5Au));
    sys.cpu.r[4] = 2u;
    sys.cpu.r[9] = 0x36u;
    sys.bios.handle_vector(sys, 0xB0u);

    // erase removes the file and marks directory sector 1 free (0xA0)
    sys.cpu.r[4] = kPathAddr;
    sys.cpu.r[9] = 0x45u;
    sys.bios.handle_vector(sys, 0xB0u);
    check_eq("erase returns 1", sys.cpu.r[2], 1u);
    check_eq("erased block 1 status == 0xA0", sys.hw.memcards[0].data[128u], static_cast<std::uint8_t>(0xA0u));
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
