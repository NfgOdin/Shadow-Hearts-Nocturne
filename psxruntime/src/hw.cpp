#include "psx/hw.hpp"
#include "psx/pgxp.hpp"
#include "psx/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace psx {

// ---------------- Memcard ----------------

void Memcard::format_empty() {
    data.assign(kMemcardSize, 0x00u);
    // Block 0, Sector 0: Header frame ("MC" + 125 zero bytes + XOR checksum 0x0E)
    data[0] = 'M';
    data[1] = 'C';
    data[127] = 0x0Eu;

    // Sectors 1..15: Directory entries (free = 0xA0, next_block = 0xFFFF)
    for (int i = 1; i <= 15; ++i) {
        std::uint8_t *sec = data.data() + i * 128;
        sec[0] = 0xA0u;
        sec[8] = 0xFFu;
        sec[9] = 0xFFu;
        std::uint8_t cs = 0;
        for (int b = 0; b < 127; ++b) cs ^= sec[b];
        sec[127] = cs;
    }

    // Sectors 16..35: Broken sector list (all 0xFFFFFFFF -> none broken)
    for (int i = 16; i <= 35; ++i) {
        std::uint8_t *sec = data.data() + i * 128;
        sec[0] = 0xFFu; sec[1] = 0xFFu; sec[2] = 0xFFu; sec[3] = 0xFFu;
        sec[8] = 0xFFu; sec[9] = 0xFFu;
        std::uint8_t cs = 0;
        for (int b = 0; b < 127; ++b) cs ^= sec[b];
        sec[127] = cs;
    }

    // Sector 63: Write test sector (copy of sector 0)
    std::memcpy(data.data() + 63 * 128, data.data(), 128);
    flag_fresh = true;
    dirty = false;
}

bool Memcard::load(const std::string &mcr_path) {
    path = mcr_path;
    std::ifstream ifs(mcr_path, std::ios::binary);
    if (!ifs.is_open()) {
        format_empty();
        return false;
    }
    ifs.read(reinterpret_cast<char *>(data.data()), kMemcardSize);
    if (ifs.gcount() != static_cast<std::streamsize>(kMemcardSize) || data[0] != 'M' || data[1] != 'C') {
        format_empty();
        return false;
    }
    flag_fresh = false;
    dirty = false;
    return true;
}

bool Memcard::save() const {
    if (path.empty()) return false;
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs.write(reinterpret_cast<const char *>(data.data()), kMemcardSize);
    return ofs.good();
}

// ---------------- Hardware ----------------

void Hardware::reset() {
    i_stat = 0;
    i_mask = 0;
    for (auto &d : dma) d = DmaChannel{};
    for (auto &dl : dma_irq_delay) dl = 0;
    dpcr = 0x0BBBBBBBu;
    dicr = 0;
    for (auto &t : timers) t = Timer{};
    pads[0] = PadState{};
    pads[1] = PadState{};
    pads[1].connected = false;
    for (int i = 0; i < 2; ++i) {
        if (memcards[i].data[0] != 'M' || memcards[i].data[1] != 'C') {
            memcards[i].format_empty();
        }
    }
    memcards[1].present = false;
    std::memset(mem_ctrl, 0, sizeof(mem_ctrl));
    ram_size_reg = 0x00000B88u;
    post_reg = 0;
    sio0_ = Sio0{};
}

void Hardware::raise_irq(System &sys, IrqBit bit) {
    i_stat |= (1u << static_cast<std::uint32_t>(bit));
    update_cpu_irq(sys);
}

void Hardware::update_cpu_irq(System &sys) {
    if ((i_stat & i_mask) != 0) {
        sys.cpu.cop0[kCop0Cause] |= (1u << 10);  // IP2 = hardware IRQ pending
    } else {
        sys.cpu.cop0[kCop0Cause] &= ~(1u << 10);
    }
}

void Hardware::update_dicr_irq(System &sys) {
    const std::uint32_t ch_en = (dicr >> 16) & 0x7Fu;
    const std::uint32_t ch_fl = (dicr >> 24) & 0x7Fu;
    const bool master_en = (dicr & (1u << 23)) != 0;
    const bool force_irq = (dicr & (1u << 15)) != 0;
    const bool prev_master = (dicr & (1u << 31)) != 0;
    const bool now_master = force_irq || (master_en && ((ch_en & ch_fl) != 0));
    if (now_master) {
        dicr |= (1u << 31);
        if (!prev_master) raise_irq(sys, kIrqDma);
    } else {
        dicr &= ~(1u << 31);
    }
}

void Hardware::execute_dma(System &sys, int ch) {
    auto &d = dma[ch];
    const std::uint32_t chcr = d.chcr;
    const bool to_ram = (chcr & 1u) == 0;
    const std::int32_t step = (chcr & 2u) ? -4 : 4;
    const std::uint32_t sync = (chcr >> 9) & 3u;

    std::uint32_t addr = d.madr & 0x001FFFFCu;
    std::uint32_t transferred_words = 0;

    if (ch == 6) {
        // Channel 6: OTC (Ordering Table Clear) — reverse linked list ending at 0x00FFFFFF
        std::uint32_t count = d.bcr & 0xFFFFu;
        if (count == 0) count = 0x10000u;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t next = (i + 1u == count) ? 0x00FFFFFFu : ((addr - 4u) & 0x001FFFFCu);
            std::memcpy(sys.mem.ram + (addr & kRamMask), &next, 4);
            addr = (addr - 4u) & 0x001FFFFCu;
        }
        pgxp::invalidate_ram_range(d.madr & 0x001FFFFCu, count, -4);
        sys.mem.mark_code_dirty(d.madr & 0x001FFFFCu, 4);
    } else if (sync == 2u && ch == 2) {
        // Channel 2 Linked-List Mode: GPU GP0 command packets
        pgxp::clear_recent();
        std::uint32_t safety = 0;
        while (safety++ < 1048576u) {
            const std::uint32_t header = sys.mem.ram32(addr);
            const std::uint32_t words = header >> 24;
            for (std::uint32_t i = 1; i <= words; ++i) {
                const std::uint32_t waddr = (addr + i * 4u) & 0x001FFFFCu;
                const std::uint32_t w = sys.mem.ram32(waddr);
                if (sys.gpu) {
                    pgxp::set_dma_word_addr(waddr);
                    sys.gpu->write_gp0(w);
                }
            }
            if ((header & 0x00800000u) != 0 || (header & 0x00FFFFFFu) == 0x00FFFFFFu) break;
            addr = header & 0x001FFFFCu;
        }
        pgxp::clear_dma_word_addr();
    } else {
        // Sync 0 (manual burst) or Sync 1 (request blocks)
        std::uint32_t words = 0;
        if (sync == 0u) {
            words = d.bcr & 0xFFFFu;
            if (words == 0) words = 0x10000u;
        } else {
            const std::uint32_t bs = d.bcr & 0xFFFFu;
            const std::uint32_t ba = (d.bcr >> 16) & 0xFFFFu;
            words = bs * (ba ? ba : 1u);
        }

        const std::uint32_t start_addr = addr;
        transferred_words = words;
        if (!to_ram && ch == 2) {
            pgxp::clear_recent();
        }
        if (!to_ram && ch == 2 && step == 4 && sys.gpu && sys.gpu->in_cpu_to_vram() && words > 0) {
            std::uint32_t rem = words;
            while (rem > 0 && sys.gpu->in_cpu_to_vram()) {
                const std::uint32_t off = addr & kRamMask;
                const std::uint32_t contig_words = std::min<std::uint32_t>(rem, (kRamSize - off) >> 2);
                sys.gpu->write_gp0_words(reinterpret_cast<const std::uint32_t *>(sys.mem.ram + off), contig_words);
                addr = (addr + contig_words * 4u) & 0x001FFFFCu;
                rem -= contig_words;
            }
            for (std::uint32_t i = 0; i < rem; ++i) {
                const std::uint32_t w = sys.mem.ram32(addr);
                pgxp::set_dma_word_addr(addr);
                sys.gpu->write_gp0(w);
                addr = (addr + 4u) & 0x001FFFFCu;
            }
        } else {
            for (std::uint32_t i = 0; i < words; ++i) {
                if (to_ram) {
                    std::uint32_t w = 0;
                    switch (ch) {
                    case 1: w = sys.mdec ? sys.mdec->read_data() : 0u; break;
                    case 2: w = sys.gpu ? sys.gpu->read_data() : 0u; break;
                    case 3: w = sys.cdrom.read_dma32(); break;
                    case 4:
                        if (sys.spu) {
                            const std::uint16_t lo = sys.spu->read_dma16();
                            const std::uint16_t hi = sys.spu->read_dma16();
                            w = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
                        }
                        break;
                    default: break;
                    }
                    std::memcpy(sys.mem.ram + (addr & kRamMask), &w, 4);
                } else {
                    const std::uint32_t w = sys.mem.ram32(addr);
                    switch (ch) {
                    case 0: if (sys.mdec) sys.mdec->write_cmd(w); break;
                    case 2:
                        if (sys.gpu) {
                            pgxp::set_dma_word_addr(addr);
                            sys.gpu->write_gp0(w);
                        }
                        break;
                    case 4:
                        if (sys.spu) {
                            sys.spu->write_dma16(static_cast<std::uint16_t>(w & 0xFFFFu));
                            sys.spu->write_dma16(static_cast<std::uint16_t>(w >> 16));
                        }
                        break;
                    default: break;
                    }
                }
                addr = static_cast<std::uint32_t>(static_cast<std::int32_t>(addr) + step) & 0x001FFFFCu;
            }
        }
        if (!to_ram && ch == 2) {
            pgxp::clear_dma_word_addr();
        }
        if (to_ram && words > 0) {
            pgxp::invalidate_ram_range(start_addr, words, step);
            sys.mem.mark_code_dirty(start_addr, words * 4u);
        }
        d.madr = addr;
        if (sync == 1u) d.bcr &= 0xFFFFu;  // BA decremented to 0
    }

    // Clear busy/trigger bits (DMA1 MDECOUT keeps bit 24 set until its realistic MDEC slice delay expires
    // so DecDCToutSync(1) and StCdInterrupt observe realistic MDEC busy timing).
    if (ch == 1) {
        d.chcr &= ~(1u << 28);
    } else {
        d.chcr &= ~((1u << 24) | (1u << 28));
    }

    // Schedule channel completion IRQ after a realistic DMA transfer delay.
    // For DMA1 (MDECOUT), PS1 MDEC decodes ~15 macroblocks (2,880 words) in ~11,520 cycles (~4 cyc/word);
    // firing in 512 cycles races ahead of Koudelka MOVIE.BIN's DecDCToutCallback / vlc_buf_state=2
    // handshake and causes permanent double-buffer deadlock partway through FMVs.
    std::uint32_t delay = 512u;
    if (ch == 1 && transferred_words > 0) {
        delay = std::clamp<std::uint32_t>(transferred_words * 4u, 512u, 16384u);
    }
    dma_irq_delay[ch] = delay;
}

void Hardware::sio0_transfer_byte(System &sys, std::uint8_t tx) {
    const int port = (sio0_.ctrl & (1u << 13)) ? 1 : 0;
    sio0_.rx_ready = true;
    sio0_.ack_level = false;
    sio0_.rx_data = 0xFFu;

    if ((sio0_.ctrl & 2u) == 0) {
        // Chip select inactive
        sio0_.active_dev = Sio0::Device::None;
        sio0_.step = 0;
        return;
    }

    if (sio0_.step == 0) {
        sio0_.port = port;
        if (tx == 0x01u && pads[port].connected) {
            sio0_.active_dev = Sio0::Device::Pad;
            sio0_.step = 1;
            sio0_.rx_data = 0xFFu;
            sio0_.ack_level = true;
        } else if (tx == 0x81u && memcards[port].present) {
            sio0_.active_dev = Sio0::Device::Memcard;
            sio0_.step = 1;
            sio0_.rx_data = 0xFFu;
            sio0_.ack_level = true;
        } else {
            sio0_.active_dev = Sio0::Device::None;
            sio0_.rx_data = 0xFFu;
            sio0_.ack_level = false;
        }
    } else if (sio0_.active_dev == Sio0::Device::Pad) {
        const PadState &p = pads[sio0_.port];
        const int total_steps = p.analog_mode ? 9 : 5;
        switch (sio0_.step) {
        case 1:
            if (tx == 0x42u) {
                sio0_.rx_data = p.analog_mode ? 0x73u : 0x41u;
                sio0_.ack_level = true;
            } else {
                sio0_.active_dev = Sio0::Device::None;
                sio0_.rx_data = 0xFFu;
            }
            break;
        case 2: sio0_.rx_data = 0x5Au; sio0_.ack_level = true; break;
        case 3: sio0_.rx_data = static_cast<std::uint8_t>(p.buttons & 0xFFu); sio0_.ack_level = true; break;
        case 4: sio0_.rx_data = static_cast<std::uint8_t>(p.buttons >> 8); sio0_.ack_level = p.analog_mode; break;
        case 5: sio0_.rx_data = p.rx; sio0_.ack_level = true; break;
        case 6: sio0_.rx_data = p.ry; sio0_.ack_level = true; break;
        case 7: sio0_.rx_data = p.lx; sio0_.ack_level = true; break;
        case 8: sio0_.rx_data = p.ly; sio0_.ack_level = false; break;
        default: sio0_.rx_data = 0xFFu; sio0_.ack_level = false; break;
        }
        ++sio0_.step;
        if (sio0_.step >= total_steps) {
            sio0_.active_dev = Sio0::Device::None;
        }
    } else if (sio0_.active_dev == Sio0::Device::Memcard) {
        Memcard &mc = memcards[sio0_.port];
        const std::uint8_t flag = mc.flag_fresh ? 0x08u : 0x00u;
        if (sio0_.step == 1) {
            sio0_.mc_cmd = tx;
            sio0_.rx_data = flag;
            sio0_.ack_level = (tx == 0x52u || tx == 0x57u || tx == 0x53u);
            ++sio0_.step;
        } else if (sio0_.mc_cmd == 0x53u) {  // GetID
            static constexpr std::uint8_t kIdSeq[] = {0x5Au, 0x5Du, 0x5Cu, 0x5Du, 0x04u, 0x00u, 0x00u, 0x80u};
            const int idx = sio0_.step - 2;
            if (idx >= 0 && idx < 8) {
                sio0_.rx_data = kIdSeq[idx];
                sio0_.ack_level = (idx < 7);
                ++sio0_.step;
            } else {
                sio0_.active_dev = Sio0::Device::None;
            }
        } else if (sio0_.mc_cmd == 0x52u) {  // Read sector (128 bytes)
            switch (sio0_.step) {
            case 2: sio0_.rx_data = 0x5Au; sio0_.ack_level = true; break;
            case 3: sio0_.rx_data = 0x5Du; sio0_.ack_level = true; break;
            case 4: sio0_.mc_sector = static_cast<std::uint16_t>(tx) << 8; sio0_.rx_data = 0x00u; sio0_.ack_level = true; break;
            case 5: sio0_.mc_sector |= tx; sio0_.rx_data = 0x00u; sio0_.ack_level = true; break;
            case 6: sio0_.rx_data = 0x5Cu; sio0_.ack_level = true; break;
            case 7: sio0_.rx_data = 0x5Du; sio0_.ack_level = true; break;
            case 8: sio0_.rx_data = static_cast<std::uint8_t>(sio0_.mc_sector >> 8); sio0_.ack_level = true; break;
            case 9:
                sio0_.rx_data = static_cast<std::uint8_t>(sio0_.mc_sector & 0xFFu);
                sio0_.mc_checksum = static_cast<std::uint8_t>((sio0_.mc_sector >> 8) ^ (sio0_.mc_sector & 0xFFu));
                sio0_.ack_level = true;
                break;
            default: {
                const int bidx = sio0_.step - 10;
                if (bidx >= 0 && bidx < 128) {
                    const std::uint32_t off = static_cast<std::uint32_t>(sio0_.mc_sector & 0x3FFu) * 128u + static_cast<std::uint32_t>(bidx);
                    const std::uint8_t b = mc.data[off];
                    sio0_.rx_data = b;
                    sio0_.mc_checksum ^= b;
                    sio0_.ack_level = true;
                } else if (bidx == 128) {
                    sio0_.rx_data = sio0_.mc_checksum;
                    sio0_.ack_level = true;
                } else {
                    sio0_.rx_data = 0x47u;  // 'G' = Good
                    sio0_.ack_level = false;
                    sio0_.active_dev = Sio0::Device::None;
                }
                break;
            }
            }
            ++sio0_.step;
        } else if (sio0_.mc_cmd == 0x57u) {  // Write sector (128 bytes)
            switch (sio0_.step) {
            case 2: sio0_.rx_data = 0x5Au; sio0_.ack_level = true; break;
            case 3: sio0_.rx_data = 0x5Du; sio0_.ack_level = true; break;
            case 4: sio0_.mc_sector = static_cast<std::uint16_t>(tx) << 8; sio0_.rx_data = 0x00u; sio0_.ack_level = true; break;
            case 5:
                sio0_.mc_sector |= tx;
                sio0_.mc_checksum = static_cast<std::uint8_t>((sio0_.mc_sector >> 8) ^ (sio0_.mc_sector & 0xFFu));
                sio0_.rx_data = 0x00u;
                sio0_.ack_level = true;
                break;
            default: {
                const int bidx = sio0_.step - 6;
                if (bidx >= 0 && bidx < 128) {
                    sio0_.mc_buf[bidx] = tx;
                    sio0_.mc_checksum ^= tx;
                    sio0_.rx_data = 0x00u;
                    sio0_.ack_level = true;
                } else if (bidx == 128) {
                    sio0_.rx_data = 0x00u;
                    sio0_.ack_level = true;
                    const bool ok = (tx == sio0_.mc_checksum) && (sio0_.mc_sector < 1024u);
                    if (ok) {
                        std::memcpy(mc.data.data() + sio0_.mc_sector * 128u, sio0_.mc_buf, 128);
                        mc.flag_fresh = false;
                        mc.dirty = true;
                        mc.save();
                    }
                } else if (bidx == 129) {
                    sio0_.rx_data = 0x5Cu;
                    sio0_.ack_level = true;
                } else if (bidx == 130) {
                    sio0_.rx_data = 0x5Du;
                    sio0_.ack_level = true;
                } else {
                    sio0_.rx_data = 0x47u;  // 'G' = Good
                    sio0_.ack_level = false;
                    sio0_.active_dev = Sio0::Device::None;
                }
                break;
            }
            }
            ++sio0_.step;
        }
    }

    if (sio0_.ack_level) {
        sio0_.irq_delay = 128u;
    }
    (void)sys;
}

void Hardware::tick_timer(System &sys, int idx, std::uint32_t ticks) {
    if (ticks == 0) return;
    auto &t = timers[idx];
    // Timer 2 stopped mode (sync enable + mode 0 or 3)
    if (idx == 2 && (t.mode & 1u)) {
        const std::uint32_t sync_mode = (t.mode >> 1) & 3u;
        if (sync_mode == 0u || sync_mode == 3u) return;
    }

    const std::uint32_t old_val = t.counter;
    std::uint32_t new_val = old_val + ticks;
    const bool reset_on_target = (t.mode & (1u << 3)) != 0;
    const bool irq_on_target = (t.mode & (1u << 4)) != 0;
    const bool irq_on_ffff = (t.mode & (1u << 5)) != 0;
    const bool repeat_irq = (t.mode & (1u << 6)) != 0;

    bool trigger = false;
    if (t.target != 0 && old_val < t.target && new_val >= t.target) {
        t.mode |= (1u << 11);  // Reached target
        if (irq_on_target) trigger = true;
        if (reset_on_target) {
            new_val = (t.target > 0) ? (new_val % t.target) : 0u;
        }
    }
    if (new_val >= 0x10000u) {
        t.mode |= (1u << 12);  // Reached 0xFFFF
        if (irq_on_ffff) trigger = true;
        new_val &= 0xFFFFu;
    }
    t.counter = new_val;

    if (trigger && (repeat_irq || !t.irq_fired)) {
        t.irq_fired = true;
        raise_irq(sys, static_cast<IrqBit>(kIrqTimer0 + idx));
    }
}

void Hardware::tick(System &sys, std::uint32_t cycles) {
    // Delayed DMA channel completion IRQs
    bool dma_fired = false;
    for (int ch = 0; ch < 7; ++ch) {
        if (dma_irq_delay[ch] > 0) {
            if (cycles >= dma_irq_delay[ch]) {
                dma_irq_delay[ch] = 0;
                dma[ch].chcr &= ~(1u << 24);  // Clear busy bit on completion
                if (dicr & (1u << (16 + ch))) {
                    dicr |= (1u << (24 + ch));
                    dma_fired = true;
                }
            } else {
                dma_irq_delay[ch] -= cycles;
            }
        }
    }
    if (dma_fired) {
        update_dicr_irq(sys);
    }

    // SIO0 delayed /ACK pulse end & IRQ
    if (sio0_.irq_delay > 0) {
        if (cycles >= sio0_.irq_delay) {
            sio0_.irq_delay = 0;
            sio0_.ack_level = false;
            if (sio0_.ctrl & (1u << 12)) {
                sio0_.irq = true;
                raise_irq(sys, kIrqSio0);
            }
        } else {
            sio0_.irq_delay -= cycles;
        }
    }

    // Timer 0 (dotclock or sysclk)
    tick_timer(sys, 0, cycles);

    // Timer 1 (if clock source bit 8 == 0 -> sysclk; bit 8 == 1 -> HBlank handled in on_hblank)
    if ((timers[1].mode & (1u << 8)) == 0) {
        tick_timer(sys, 1, cycles);
    }

    // Timer 2 (if clock source bit 9 == 1 -> sysclk/8; else sysclk)
    if (timers[2].mode & (1u << 9)) {
        timers[2].sub_cycles += cycles;
        const std::uint32_t t2 = timers[2].sub_cycles >> 3;
        timers[2].sub_cycles &= 7u;
        tick_timer(sys, 2, t2);
    } else {
        tick_timer(sys, 2, cycles);
    }
}

void Hardware::on_hblank(System &sys) {
    if (timers[1].mode & (1u << 8)) {
        tick_timer(sys, 1, 1u);
    }
}

void Hardware::on_vblank(System &sys) {
    raise_irq(sys, kIrqVBlank);
}

std::uint32_t Hardware::read_io32(System &sys, std::uint32_t phys) {
    if (phys >= 0x1F801000u && phys <= 0x1F801020u) {
        return mem_ctrl[(phys - 0x1F801000u) >> 2];
    }
    if (phys == 0x1F801060u) return ram_size_reg;
    if (phys == 0x1F801070u) return i_stat;
    if (phys == 0x1F801074u) return i_mask;

    // DMA registers 0x1F801080..0x1F8010F4
    if (phys >= 0x1F801080u && phys < 0x1F8010F0u) {
        const int ch = static_cast<int>((phys - 0x1F801080u) >> 4);
        const std::uint32_t reg = phys & 0xFu;
        if (ch >= 0 && ch < 7) {
            if (reg == 0) return dma[ch].madr;
            if (reg == 4) return dma[ch].bcr;
            if (reg == 8) return dma[ch].chcr;
        }
        return 0;
    }
    if (phys == 0x1F8010F0u) return dpcr;
    if (phys == 0x1F8010F4u) return dicr;

    // Timers 0x1F801100..0x1F80112F
    if (phys >= 0x1F801100u && phys < 0x1F801130u) {
        const int idx = static_cast<int>((phys - 0x1F801100u) >> 4);
        const std::uint32_t reg = phys & 0xFu;
        if (idx >= 0 && idx < 3) {
            if (reg == 0) return timers[idx].counter & 0xFFFFu;
            if (reg == 4) {
                const std::uint32_t m = timers[idx].mode;
                timers[idx].mode &= ~((1u << 11) | (1u << 12));  // Clear reached flags on read
                return m;
            }
            if (reg == 8) return timers[idx].target & 0xFFFFu;
        }
        return 0;
    }

    // GPU (0x1F801810 GPUREAD, 0x1F801814 GPUSTAT)
    if (phys == 0x1F801810u) return sys.gpu ? sys.gpu->read_data() : 0u;
    if (phys == 0x1F801814u) return sys.gpu ? sys.gpu->read_stat() : 0x1C000000u;

    // MDEC (0x1F801820 DATA, 0x1F801824 STAT)
    if (phys == 0x1F801820u) return sys.mdec ? sys.mdec->read_data() : 0u;
    if (phys == 0x1F801824u) return sys.mdec ? sys.mdec->read_stat() : 0x80040000u;

    // SIO0
    if (phys >= 0x1F801040u && phys < 0x1F801050u) {
        return read_io16(sys, phys);
    }

    // SPU (32-bit reads combine two 16-bit regs)
    if (phys >= 0x1F801C00u && phys < 0x1F802000u) {
        const std::uint32_t lo = read_io16(sys, phys);
        const std::uint32_t hi = read_io16(sys, phys + 2u);
        return lo | (hi << 16);
    }

    return 0;
}

std::uint16_t Hardware::read_io16(System &sys, std::uint32_t phys) {
    // SIO0 (0x1F801040..0x1F80104E)
    if (phys == 0x1F801040u) {
        sio0_.rx_ready = false;
        return sio0_.rx_data;
    }
    if (phys == 0x1F801044u) {
        std::uint16_t st = 0x0005u;  // TX Ready 1 & 2 always set
        if (sio0_.rx_ready) st |= 0x0002u;
        if (sio0_.ack_level) st |= 0x0080u;
        if (sio0_.irq) st |= 0x0200u;
        return st;
    }
    if (phys == 0x1F801048u) return sio0_.mode;
    if (phys == 0x1F80104Au) return sio0_.ctrl;
    if (phys == 0x1F80104Eu) return sio0_.baud;

    if (phys == 0x1F801070u) return static_cast<std::uint16_t>(i_stat);
    if (phys == 0x1F801074u) return static_cast<std::uint16_t>(i_mask);

    if (phys >= 0x1F801100u && phys < 0x1F801130u) {
        return static_cast<std::uint16_t>(read_io32(sys, phys & ~3u));
    }
    if (phys >= 0x1F801C00u && phys < 0x1F802000u) {
        return sys.spu ? sys.spu->read_reg(phys) : 0u;
    }
    return static_cast<std::uint16_t>(read_io32(sys, phys & ~3u) >> ((phys & 2u) * 8u));
}

std::uint8_t Hardware::read_io8(System &sys, std::uint32_t phys) {
    if (phys >= 0x1F801800u && phys <= 0x1F801803u) {
        return sys.cdrom.read_reg(phys - 0x1F801800u);
    }
    if (phys == 0x1F801040u) {
        sio0_.rx_ready = false;
        return sio0_.rx_data;
    }
    if (phys == 0x1F802041u) return static_cast<std::uint8_t>(post_reg);
    return static_cast<std::uint8_t>(read_io32(sys, phys & ~3u) >> ((phys & 3u) * 8u));
}

void Hardware::write_io32(System &sys, std::uint32_t phys, std::uint32_t val) {
    if (phys >= 0x1F801000u && phys <= 0x1F801020u) {
        mem_ctrl[(phys - 0x1F801000u) >> 2] = val;
        return;
    }
    if (phys == 0x1F801060u) { ram_size_reg = val; return; }
    if (phys == 0x1F801070u) {
        i_stat &= val;  // Write-0 clears IRQ bits
        update_cpu_irq(sys);
        return;
    }
    if (phys == 0x1F801074u) {
        i_mask = val & 0x7FFu;
        update_cpu_irq(sys);
        return;
    }

    // DMA registers 0x1F801080..0x1F8010F4
    if (phys >= 0x1F801080u && phys < 0x1F8010F0u) {
        const int ch = static_cast<int>((phys - 0x1F801080u) >> 4);
        const std::uint32_t reg = phys & 0xFu;
        if (ch >= 0 && ch < 7) {
            if (reg == 0) {
                dma[ch].madr = val & 0x00FFFFFFu;
            } else if (reg == 4) {
                dma[ch].bcr = val;
            } else if (reg == 8) {
                dma[ch].chcr = val;
                const bool enabled = (dpcr & (1u << (ch * 4 + 3))) != 0;
                const bool start = (val & (1u << 24)) != 0;
                const bool trigger = (val & (1u << 28)) != 0;
                const std::uint32_t sync = (val >> 9) & 3u;
                if (enabled && start && (sync != 0u || trigger)) {
                    execute_dma(sys, ch);
                }
            }
        }
        return;
    }
    if (phys == 0x1F8010F0u) { dpcr = val; return; }
    if (phys == 0x1F8010F4u) {
        // Bits 24..30 are write-1-to-clear!
        const std::uint32_t ack = (val & 0x7F000000u);
        const std::uint32_t old_flags = (dicr & 0x7F000000u) & ~ack;
        dicr = (val & 0x00FF803Fu) | old_flags;
        update_dicr_irq(sys);
        return;
    }

    // Timers 0x1F801100..0x1F80112F
    if (phys >= 0x1F801100u && phys < 0x1F801130u) {
        const int idx = static_cast<int>((phys - 0x1F801100u) >> 4);
        const std::uint32_t reg = phys & 0xFu;
        if (idx >= 0 && idx < 3) {
            if (reg == 0) {
                timers[idx].counter = val & 0xFFFFu;
            } else if (reg == 4) {
                timers[idx].mode = (val & 0x03FFu) | (1u << 10);  // Bit 10 = IRQ not yet triggered
                timers[idx].counter = 0;
                timers[idx].irq_fired = false;
            } else if (reg == 8) {
                timers[idx].target = val & 0xFFFFu;
            }
        }
        return;
    }

    // GPU
    if (phys == 0x1F801810u) { if (sys.gpu) sys.gpu->write_gp0(val); return; }
    if (phys == 0x1F801814u) { if (sys.gpu) sys.gpu->write_gp1(val); return; }

    // MDEC
    if (phys == 0x1F801820u) { if (sys.mdec) sys.mdec->write_cmd(val); return; }
    if (phys == 0x1F801824u) { if (sys.mdec) sys.mdec->write_ctrl(val); return; }

    // SPU
    if (phys >= 0x1F801C00u && phys < 0x1F802000u) {
        if (sys.spu) {
            sys.spu->write_reg(phys, static_cast<std::uint16_t>(val & 0xFFFFu));
            sys.spu->write_reg(phys + 2u, static_cast<std::uint16_t>(val >> 16));
        }
        return;
    }

    if (phys >= 0x1F801040u && phys < 0x1F801050u) {
        write_io16(sys, phys, static_cast<std::uint16_t>(val & 0xFFFFu));
    }
}

void Hardware::write_io16(System &sys, std::uint32_t phys, std::uint16_t val) {
    // SIO0
    if (phys == 0x1F801040u) {
        sio0_transfer_byte(sys, static_cast<std::uint8_t>(val & 0xFFu));
        return;
    }
    if (phys == 0x1F801048u) { sio0_.mode = val; return; }
    if (phys == 0x1F80104Au) {
        sio0_.ctrl = val;
        if (val & 0x0010u) sio0_.irq = false;   // Acknowledge IRQ
        if (val & 0x0040u) {                    // Reset SIO0
            sio0_.ctrl = 0;
            sio0_.irq = false;
            sio0_.rx_ready = false;
            sio0_.active_dev = Sio0::Device::None;
            sio0_.step = 0;
        }
        if ((val & 0x0002u) == 0) {
            sio0_.active_dev = Sio0::Device::None;
            sio0_.step = 0;
            sio0_.ack_level = false;
        }
        return;
    }
    if (phys == 0x1F80104Eu) { sio0_.baud = val; return; }

    if (phys == 0x1F801070u) {
        i_stat &= val;
        update_cpu_irq(sys);
        return;
    }
    if (phys == 0x1F801074u) {
        i_mask = val & 0x7FFu;
        update_cpu_irq(sys);
        return;
    }
    if (phys >= 0x1F801080u && phys < 0x1F8010F4u) {
        const std::uint32_t aligned = phys & ~3u;
        const std::uint32_t shift = (phys & 2u) * 8u;
        const std::uint32_t cur = read_io32(sys, aligned);
        const std::uint32_t next = (cur & ~(0xFFFFu << shift)) | (static_cast<std::uint32_t>(val) << shift);
        write_io32(sys, aligned, next);
        return;
    }
    if (phys == 0x1F8010F4u || phys == 0x1F8010F6u) {
        // DICR sub-word write: bits 24..30 are W1C (only acknowledged if upper halfword 0x1F8010F6 is written)
        const std::uint32_t shift = (phys & 2u) * 8u;
        const std::uint32_t mask = 0xFFFFu << shift;
        const std::uint32_t shifted = static_cast<std::uint32_t>(val) << shift;
        const std::uint32_t rw_mask = 0x00FF803Fu & mask;
        const std::uint32_t ack = (shifted & mask) & 0x7F000000u;
        const std::uint32_t old_flags = (dicr & 0x7F000000u) & ~ack;
        dicr = ((dicr & 0x00FF803Fu) & ~rw_mask) | (shifted & rw_mask) | old_flags;
        update_dicr_irq(sys);
        return;
    }
    if (phys >= 0x1F801100u && phys < 0x1F801130u) {
        write_io32(sys, phys & ~3u, val);
        return;
    }
    if (phys >= 0x1F801C00u && phys < 0x1F802000u) {
        if (sys.spu) sys.spu->write_reg(phys, val);
        return;
    }
}

void Hardware::write_io8(System &sys, std::uint32_t phys, std::uint8_t val) {
    if (phys >= 0x1F801800u && phys <= 0x1F801803u) {
        sys.cdrom.write_reg(sys, phys - 0x1F801800u, val);
        return;
    }
    if (phys == 0x1F801040u) {
        sio0_transfer_byte(sys, val);
        return;
    }
    if (phys >= 0x1F801070u && phys < 0x1F801074u) {
        const std::uint32_t shift = (phys & 3u) * 8u;
        const std::uint32_t mask = ~(0xFFu << shift) | (static_cast<std::uint32_t>(val) << shift);
        i_stat &= mask;
        update_cpu_irq(sys);
        return;
    }
    if (phys >= 0x1F801074u && phys < 0x1F801078u) {
        const std::uint32_t shift = (phys & 3u) * 8u;
        i_mask = ((i_mask & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(val) << shift)) & 0x7FFu;
        update_cpu_irq(sys);
        return;
    }
    if (phys >= 0x1F801080u && phys < 0x1F8010F4u) {
        const std::uint32_t aligned = phys & ~3u;
        const std::uint32_t shift = (phys & 3u) * 8u;
        const std::uint32_t cur = read_io32(sys, aligned);
        const std::uint32_t next = (cur & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(val) << shift);
        write_io32(sys, aligned, next);
        return;
    }
    if (phys >= 0x1F8010F4u && phys <= 0x1F8010F7u) {
        // DICR byte write: PsyQ libcd uses sb to 0x1F8010F6 to toggle channel IRQ enables without touching W1C flags!
        const std::uint32_t shift = (phys & 3u) * 8u;
        const std::uint32_t mask = 0xFFu << shift;
        const std::uint32_t shifted = static_cast<std::uint32_t>(val) << shift;
        const std::uint32_t rw_mask = 0x00FF803Fu & mask;
        const std::uint32_t ack = (shifted & mask) & 0x7F000000u;
        const std::uint32_t old_flags = (dicr & 0x7F000000u) & ~ack;
        dicr = ((dicr & 0x00FF803Fu) & ~rw_mask) | (shifted & rw_mask) | old_flags;
        update_dicr_irq(sys);
        return;
    }
    if (phys == 0x1F802041u) {
        post_reg = val;
    }
}

}  // namespace psx
