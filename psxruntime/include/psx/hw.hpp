#pragma once

#include "psx/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace psx {

class System;

// Standard PS1 16-bit digital pad button mask (active-low in hardware: 0 = pressed, 1 = released).
enum PadButton : std::uint16_t {
    kPadSelect   = 1u << 0,
    kPadL3       = 1u << 1,
    kPadR3       = 1u << 2,
    kPadStart    = 1u << 3,
    kPadUp       = 1u << 4,
    kPadRight    = 1u << 5,
    kPadDown     = 1u << 6,
    kPadLeft     = 1u << 7,
    kPadL2       = 1u << 8,
    kPadR2       = 1u << 9,
    kPadL1       = 1u << 10,
    kPadR1       = 1u << 11,
    kPadTriangle = 1u << 12,
    kPadCircle   = 1u << 13,
    kPadCross    = 1u << 14,
    kPadSquare   = 1u << 15,
};

struct PadState {
    std::uint16_t buttons = 0xFFFFu;  // active low (0 = pressed)
    std::uint8_t rx = 0x80u;
    std::uint8_t ry = 0x80u;
    std::uint8_t lx = 0x80u;
    std::uint8_t ly = 0x80u;
    bool analog_mode = false;
    bool connected = true;
};

constexpr std::uint32_t kMemcardSize = 128u * 1024u;  // 128 KiB standard PS1 memory card (1024 sectors x 128 B)

// PS1 Memory Card (128 KiB .mcr)
struct Memcard {
    std::vector<std::uint8_t> data = std::vector<std::uint8_t>(kMemcardSize, 0);
    std::string path;
    bool present = true;
    bool flag_fresh = true;  // bit 3 of flag byte (cleared on first write)
    bool dirty = false;

    void format_empty();
    bool load(const std::string &mcr_path);
    bool save() const;
};

// PS1 Core Hardware: IRQ controller, DMA channels 0..6, Root Counters 0..2, SIO0 (Pads + Memcards).
class Hardware {
public:
    // IRQ registers (0x1F801070 / 0x1F801074)
    std::uint32_t i_stat = 0;
    std::uint32_t i_mask = 0;

    // DMA channels 0..6 (0x1F801080..0x1F8010F4)
    struct DmaChannel {
        std::uint32_t madr = 0;
        std::uint32_t bcr = 0;
        std::uint32_t chcr = 0;
    };
    DmaChannel dma[7] = {};
    std::uint32_t dma_irq_delay[7] = {};
    std::uint32_t dpcr = 0x0BBBBBBBu;
    std::uint32_t dicr = 0;

    // Root Counters 0..2 (0x1F801100..0x1F80112C)
    struct Timer {
        std::uint32_t counter = 0;
        std::uint32_t mode = 0;
        std::uint32_t target = 0;
        std::uint32_t sub_cycles = 0;
        bool irq_fired = false;
    };
    Timer timers[3] = {};

    // Pads & Memory Cards (ports 0 and 1)
    PadState pads[2] = {};
    Memcard memcards[2] = {};

    // Memory control / misc registers
    std::uint32_t mem_ctrl[9] = {};
    std::uint32_t ram_size_reg = 0x00000B88u;
    std::uint32_t post_reg = 0;

    void reset();

    void raise_irq(System &sys, IrqBit bit);
    void update_cpu_irq(System &sys);

    std::uint32_t read_io32(System &sys, std::uint32_t phys);
    std::uint16_t read_io16(System &sys, std::uint32_t phys);
    std::uint8_t read_io8(System &sys, std::uint32_t phys);

    void write_io32(System &sys, std::uint32_t phys, std::uint32_t val);
    void write_io16(System &sys, std::uint32_t phys, std::uint16_t val);
    void write_io8(System &sys, std::uint32_t phys, std::uint8_t val);

    // Advance timers & SIO0 IRQ timers.
    void tick(System &sys, std::uint32_t cycles);
    void on_hblank(System &sys);
    void on_vblank(System &sys);

private:
    // SIO0 (Joypad + Memory Card serial port at 0x1F801040..0x1F80104E)
    struct Sio0 {
        std::uint8_t rx_data = 0xFFu;
        bool rx_ready = false;
        std::uint16_t mode = 0;
        std::uint16_t ctrl = 0;
        std::uint16_t baud = 0x0088u;
        bool irq = false;
        std::uint32_t irq_delay = 0;

        // Active transfer state machine
        enum class Device : std::uint8_t { None, Pad, Memcard };
        Device active_dev = Device::None;
        int port = 0;
        int step = 0;
        std::uint8_t mc_cmd = 0;
        std::uint16_t mc_sector = 0;
        std::uint8_t mc_checksum = 0;
        std::uint8_t mc_buf[128] = {};
        bool ack_level = false;
    } sio0_;

    void execute_dma(System &sys, int ch);
    void update_dicr_irq(System &sys);
    void sio0_transfer_byte(System &sys, std::uint8_t tx);
    void tick_timer(System &sys, int idx, std::uint32_t ticks);
};

}  // namespace psx
