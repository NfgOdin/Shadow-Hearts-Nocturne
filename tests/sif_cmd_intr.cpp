// No game data: exercise the real SIFCMD imports through a synthetic IOP module.
#include "hw.hpp"
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", message); }
}
std::vector<std::uint8_t> module() {
    std::vector<std::uint8_t> elf(0x180);
    const auto put = [&](std::size_t at, auto value) { std::memcpy(elf.data() + at, &value, sizeof value); };
    std::memcpy(elf.data(), "\177ELF\1\1\1", 7);
    put(16, std::uint16_t{0xFF80}); put(18, std::uint16_t{8}); put(20, 1u);
    put(28, 52u); put(40, std::uint16_t{52}); put(42, std::uint16_t{32}); put(44, std::uint16_t{2});
    put(52, 0x70000080u); put(56, 0x80u);
    put(84, 1u); put(88, 0x100u); put(100, 0x80u); put(104, 0x80u);
    put(0x84, 0x60u); put(0x8C, 0x80u);
    put(0x100, 0x41E00000u); put(0x108, 0x101u);
    std::memcpy(elf.data() + 0x10C, "sifcmd", 6);
    const std::uint32_t exports[5] = {12u, 13u, 28u, 29u, 10u};
    for (unsigned i = 0; i < 5; ++i) {
        put(0x114 + i * 8, 0x03E00008u);
        put(0x118 + i * 8, 0x24000000u | exports[i]);
    }
    put(0x160, 0x03E00008u); // module start: jr ra; nop
    return elf;
}
}

int main() {
    namespace iop = ps2::iop;
    namespace kernel = ps2::iop::kernel;
    iop::reset();
    const int id = iop::load_module_buffer(module(), "sifcmd-test.irx", "");
    check(id > 0, "synthetic IOP module loads");
    if (id <= 0) return 1;
    const auto base = kernel::modules().at(id).base;
    iop::run(8192); // finish the synthetic module's start thread
    constexpr std::uint32_t packet = 0x18000, payload = 0x18100, stack = 0x18200, dest = 0x100000;
    int calls = 0;
    const auto callback = kernel::hle_stub([&](iop::Cpu &c) {
        ++calls;
        check(c.r[4] == 0x12345678u, "callback receives its stack-passed user argument");
        check(kernel::in_interrupt(), "completion runs in interrupt context");
        check(ps2::ld32(dest) == 0xAABBCCDDu, "payload is copied before completion");
    });
    for (unsigned which = 0; which < 4; ++which) {
        const auto opcode = iop::ram32(base + 20 + which * 8);
        check(opcode >> 26 == iop::kHleOpcode, "SIFCMD import links to its HLE handler");
        for (bool with_callback : {false, true}) {
            iop::Cpu c{};
            c.r[4] = 0x1234; c.r[5] = packet; c.r[6] = 16; c.r[7] = payload; c.r[29] = stack;
            iop::set_ram32(stack + 16, dest); iop::set_ram32(stack + 20, 4);
            iop::set_ram32(stack + 24, with_callback ? callback : 0);
            iop::set_ram32(stack + 28, 0x12345678u);
            iop::set_ram32(payload, 0xAABBCCDDu); ps2::st32(dest, 0);
            const int before = calls;
            iop::call_hle(c, opcode & 0x03FFFFFFu);
            check(c.r[2] != 0, "send returns a successful DMA identifier");
            check(ps2::ld32(dest) == 0xAABBCCDDu, "send copies the extra payload");
            check(calls == before, "completion is deferred until after send returns");
            iop::run(8192);
            const int expected = before + (which >= 2 && with_callback ? 1 : 0);
            check(calls == expected, "only Intr variants invoke a non-null callback");
            iop::run(8192);
            check(calls == expected, "completion runs exactly once");
        }
    }
    // Verify that an immediate isceSifSendCmd reply inside a guest IOP SIF command handler
    // (invoked synchronously by EE sceSifSetDma) is NOT delivered before sceSifSetDma returns,
    // and IS delivered once the short SIF round-trip delay elapses.
    {
        ps2::sif::reset();
        iop::reset();
        const int id2 = iop::load_module_buffer(module(), "sifcmd-reply-test.irx", "");
        check(id2 > 0, "second synthetic IOP module loads");
        if (id2 > 0) {
            const auto base2 = kernel::modules().at(id2).base;
            iop::run(8192);
            const auto isend_op = iop::ram32(base2 + 20 + 1 * 8);
            const auto add_op = iop::ram32(base2 + 20 + 4 * 8);
            check(isend_op >> 26 == iop::kHleOpcode && add_op >> 26 == iop::kHleOpcode,
                  "isceSifSendCmd and sceSifAddCmdHandler link to HLE");

            // Set EE receive buffer via system command 0x80000002 (opt = 0).
            constexpr std::uint32_t ee_recv = 0x120000, ee_pkt = 0x121000, ee_dma = 0x122000;
            constexpr std::uint32_t iop_reply_pkt = 0x18300, iop_reply_stack = 0x18400;
            std::memset(ps2::g_ram + ee_recv, 0, 0x40);
            ps2::st32(ee_pkt + 0, 24u);
            ps2::st32(ee_pkt + 4, 0u);
            ps2::st32(ee_pkt + 8, 0x80000002u);
            ps2::st32(ee_pkt + 12, 0u);
            ps2::st32(ee_pkt + 16, ee_recv);
            ps2::st32(ee_dma + 0, ee_pkt);
            ps2::st32(ee_dma + 4, ps2::sif::get_reg(2));
            ps2::st32(ee_dma + 8, 24u);
            ps2::st32(ee_dma + 12, 0u);
            ps2::sif::set_dma(ee_dma, 1);

            ps2::hw::dmac_ack(5);
            ps2::hw::dmac_set_mask(5, true);

            // Register IOP command handler for cid 1 that immediately calls isceSifSendCmd back to EE.
            int handler_ran = 0;
            const auto cmd_handler = kernel::hle_stub([&](iop::Cpu &) {
                ++handler_ran;
                iop::set_ram32(iop_reply_pkt + 16, 0xCAFEBABEu);
                iop::Cpu hc{};
                hc.r[4] = 1u;
                hc.r[5] = iop_reply_pkt;
                hc.r[6] = 24u;
                hc.r[7] = 0u;
                hc.r[29] = iop_reply_stack;
                iop::set_ram32(iop_reply_stack + 16, 0u);
                iop::set_ram32(iop_reply_stack + 20, 0u);
                iop::call_hle(hc, isend_op & 0x03FFFFFFu);
            });
            iop::Cpu reg_c{};
            reg_c.r[4] = 1u;
            reg_c.r[5] = cmd_handler;
            reg_c.r[6] = 0u;
            iop::call_hle(reg_c, add_op & 0x03FFFFFFu);

            ps2::set_budget(ps2::g_cpu, 10000);
            ps2::st32(ee_pkt + 0, 24u);
            ps2::st32(ee_pkt + 4, 0u);
            ps2::st32(ee_pkt + 8, 1u);
            ps2::st32(ee_pkt + 12, 0u);
            ps2::st32(ee_pkt + 16, 0x11223344u);
            ps2::sif::set_dma(ee_dma, 1);

            check(handler_ran == 1, "EE SIF command invokes IOP guest handler");
            check((ps2::hw::dmac_pending() & (1u << 5)) == 0, "immediate guest SIF reply is not delivered synchronously inside set_dma");
            check(ps2::ld32(ee_recv + 16) == 0, "EE receive buffer is untouched before SIF round-trip elapses");

            // Even if pump() runs only 100 EE cycles later, reply stays deferred.
            ps2::g_cpu.budget -= 100;
            ps2::sif::pump();
            check((ps2::hw::dmac_pending() & (1u << 5)) == 0, "guest SIF reply stays deferred at 100 EE cycles");

            // After the ~14 us round-trip elapses, pump() delivers the reply and raises DMAC CH5.
            ps2::g_cpu.budget -= 5000;
            ps2::sif::pump();
            check((ps2::hw::dmac_pending() & (1u << 5)) != 0, "guest SIF reply raises DMAC CH5 after round-trip elapses");
            check(ps2::ld32(ee_recv + 16) == 0xCAFEBABEu, "guest SIF reply payload reaches EE receive buffer");
        }
    }

    std::printf("SIFCMD completion: %d failed\n", failed);
    return failed ? 1 : 0;
}
