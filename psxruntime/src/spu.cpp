#include "psx/spu.hpp"

#include <algorithm>
#include <cstring>

namespace psx {

namespace {

// Sony PS1 SPU / XA ADPCM filter coefficients (64-scale fixed point).
constexpr std::int32_t kAdpcmPos[5] = {0, 60, 115, 98, 122};
constexpr std::int32_t kAdpcmNeg[5] = {0, 0, -52, -55, -60};

// 33.8688 MHz / 48,000 Hz = 705.6 CPU cycles per 48 kHz output sample.
constexpr std::uint32_t kCyclesPer48kSample = 705u;
constexpr std::size_t kAudioBatchFrames = 256u;

inline std::int16_t clamp16(std::int32_t v) noexcept {
    return static_cast<std::int16_t>(std::clamp(v, -32768, 32767));
}

inline std::int16_t decode_vol(std::uint16_t raw) noexcept {
    if (raw & 0x8000u) return 0x3FFF;  // Sweep mode fallback to full volume
    return static_cast<std::int16_t>(raw << 1);
}

}  // namespace

Spu::Spu() : ram(kSpuRamSize, 0) {
    reset();
}

void Spu::reset() {
    std::fill(ram.begin(), ram.end(), 0u);
    for (auto &v : voices) v = Voice{};
    regs.fill(0u);
    main_vol_l = 0x3FFF;
    main_vol_r = 0x3FFF;
    cd_vol_l = 0x7FFF;
    cd_vol_r = 0x7FFF;
    kon = 0;
    koff = 0;
    endx = 0x00FFFFFFu;
    spucnt = 0;
    spustat = 0;
    transfer_addr = 0;
    irq_addr = 0;
    cycle_accum_ = 0;
    batch_pcm_.clear();
    batch_pcm_.reserve(kAudioBatchFrames * 2u);
    xa_queue_.clear();
    xa_s1_[0] = xa_s1_[1] = 0;
    xa_s2_[0] = xa_s2_[1] = 0;
}

void Spu::key_on_voice(int idx) {
    Voice &v = voices[idx];
    v.cur_addr = (static_cast<std::uint32_t>(v.start_addr) << 3) & kSpuRamMask & ~15u;
    v.pitch_counter = 0;
    v.phase = EnvPhase::Attack;
    v.env_level = 0;
    v.env_timer = 0;
    v.repeat_written = false;
    v.s1 = 0;
    v.s2 = 0;
    v.block_idx = 28;
    endx &= ~(1u << idx);
}

void Spu::key_off_voice(int idx) {
    Voice &v = voices[idx];
    if (v.phase != EnvPhase::Off) {
        v.phase = EnvPhase::Release;
    }
}

void Spu::step_envelope(Voice &v) {
    if (v.phase == EnvPhase::Off) {
        v.env_level = 0;
        return;
    }
    const int sustain_level = ((v.adsr1 & 0xFu) + 1) * 0x800;
    switch (v.phase) {
    case EnvPhase::Attack: {
        const int shift = (v.adsr1 >> 10) & 0x1F;
        const int step = std::max(1, (7 - ((v.adsr1 >> 8) & 3)) << std::max(0, 11 - shift));
        v.env_level = static_cast<std::int16_t>(std::min(0x7FFF, v.env_level + step));
        if (v.env_level >= 0x7FFF) v.phase = EnvPhase::Decay;
        break;
    }
    case EnvPhase::Decay: {
        const int shift = (v.adsr1 >> 4) & 0xF;
        const int step = std::max(1, (8 << std::max(0, 11 - shift)));
        const int delta = std::max(1, (step * v.env_level) >> 15);
        v.env_level = static_cast<std::int16_t>(std::max(sustain_level, v.env_level - delta));
        if (v.env_level <= sustain_level) v.phase = EnvPhase::Sustain;
        break;
    }
    case EnvPhase::Sustain: {
        const bool dec = (v.adsr2 & (1u << 14)) != 0;
        const int shift = (v.adsr2 >> 8) & 0x1F;
        if (shift >= 0x1F) break;
        const int step = std::max(1, (7 - ((v.adsr2 >> 6) & 3)) << std::max(0, 11 - shift));
        if (dec) {
            v.env_level = static_cast<std::int16_t>(std::max(0, v.env_level - (step >> 2)));
        } else {
            v.env_level = static_cast<std::int16_t>(std::min(0x7FFF, v.env_level + (step >> 2)));
        }
        break;
    }
    case EnvPhase::Release: {
        const int shift = v.adsr2 & 0x1F;
        const int step = std::max(8, 8 << std::max(0, 11 - shift));
        v.env_level = static_cast<std::int16_t>(std::max(0, v.env_level - step));
        if (v.env_level == 0) v.phase = EnvPhase::Off;
        break;
    }
    default: break;
    }
}

void Spu::decode_voice_block(Voice &v) {
    const std::uint32_t addr = v.cur_addr & kSpuRamMask & ~15u;
    const std::uint8_t shift_filter = ram[addr];
    const std::uint8_t flags = ram[addr + 1u];

    const int shift = std::min<int>(12, shift_filter & 0xFu);
    const int filter = std::min<int>(4, (shift_filter >> 4) & 0x7u);
    const std::int32_t f0 = kAdpcmPos[filter];
    const std::int32_t f1 = kAdpcmNeg[filter];

    if ((flags & 4u) && !v.repeat_written) {
        v.repeat_addr = static_cast<std::uint16_t>(addr >> 3);
    }

    for (int i = 0; i < 28; ++i) {
        const std::uint8_t byte = ram[addr + 2u + static_cast<std::uint32_t>(i >> 1)];
        const int nibble = (i & 1) ? (byte >> 4) : (byte & 0xFu);
        const std::int32_t signed_nib = static_cast<std::int16_t>(nibble << 12) >> shift;
        const std::int32_t pcm = signed_nib + ((v.s1 * f0 + v.s2 * f1 + 32) >> 6);
        const std::int16_t clamped = clamp16(pcm);
        v.block_pcm[i] = clamped;
        v.s2 = v.s1;
        v.s1 = clamped;
    }

    v.cur_addr = (addr + 16u) & kSpuRamMask;
    v.block_idx = 0;

    if (flags & 1u) {  // Loop end
        const int vidx = static_cast<int>(&v - voices.data());
        endx |= (1u << vidx);
        if (flags & 2u) {  // Loop repeat
            v.cur_addr = (static_cast<std::uint32_t>(v.repeat_addr) << 3) & kSpuRamMask & ~15u;
        } else {
            v.phase = EnvPhase::Off;
            v.env_level = 0;
        }
    }
}

void Spu::sample_one_48k(std::int16_t &out_l, std::int16_t &out_r) {
    std::int32_t mix_l = 0;
    std::int32_t mix_r = 0;

    if (spucnt & 0x8000u) {
        for (int i = 0; i < kNumVoices; ++i) {
            Voice &v = voices[i];
            if (v.phase == EnvPhase::Off) continue;
            if (v.block_idx >= 28) {
                decode_voice_block(v);
                if (v.phase == EnvPhase::Off) continue;
            }

            step_envelope(v);
            const std::int32_t s = (static_cast<std::int32_t>(v.block_pcm[v.block_idx]) * v.env_level) >> 15;
            mix_l += (s * v.vol_l) >> 15;
            mix_r += (s * v.vol_r) >> 15;

            // Advance pitch counter (scaled from 44.1 kHz to 48 kHz: * 441 / 480)
            const std::uint32_t step = (std::min<std::uint32_t>(v.pitch, 0x3FFFu) * 441u) / 480u;
            v.pitch_counter += step;
            v.block_idx += static_cast<int>(v.pitch_counter >> 12);
            v.pitch_counter &= 0xFFFu;
        }
    }

    mix_l = static_cast<std::int32_t>(mix_l * sfx_gain);
    mix_r = static_cast<std::int32_t>(mix_r * sfx_gain);

    // Mix queued XA-ADPCM stereo audio
    if (xa_queue_.size() >= 2u) {
        const std::int32_t xl = xa_queue_.front(); xa_queue_.pop_front();
        const std::int32_t xr = xa_queue_.front(); xa_queue_.pop_front();
        mix_l += static_cast<std::int32_t>(((xl * cd_vol_l) >> 15) * bgm_gain);
        mix_r += static_cast<std::int32_t>(((xr * cd_vol_r) >> 15) * bgm_gain);
    }

    mix_l = (mix_l * main_vol_l) >> 14;
    mix_r = (mix_r * main_vol_r) >> 14;
    out_l = clamp16(mix_l);
    out_r = clamp16(mix_r);
}

void Spu::tick(std::uint32_t cycles) {
    cycle_accum_ += cycles;
    while (cycle_accum_ >= kCyclesPer48kSample) {
        cycle_accum_ -= kCyclesPer48kSample;
        std::int16_t l = 0, r = 0;
        sample_one_48k(l, r);
        batch_pcm_.push_back(l);
        batch_pcm_.push_back(r);
        if (batch_pcm_.size() >= kAudioBatchFrames * 2u) {
            if (audio_cb_) {
                audio_cb_(batch_pcm_.data(), kAudioBatchFrames);
            }
            batch_pcm_.clear();
        }
    }
}

void Spu::decode_xa_sector(const std::uint8_t *raw2352) {
    if (!raw2352) return;
    const std::uint8_t coding = raw2352[19];
    const bool stereo = (coding & 3u) == 1u;
    const bool rate_18900 = ((coding >> 2) & 3u) == 1u;
    const bool bits_8 = ((coding >> 4) & 3u) == 1u;
    if (bits_8) return;  // Koudelka uses standard 4-bit XA-ADPCM

    const std::uint8_t *payload = raw2352 + 24;
    std::vector<std::int16_t> decoded_lr;
    decoded_lr.reserve(4032);

    // 18 sound groups per Mode 2 Form 2 XA sector (128 bytes each)
    for (int sg = 0; sg < 18; ++sg) {
        const std::uint8_t *grp = payload + sg * 128;
        for (int blk = 0; blk < 8; ++blk) {
            const std::uint8_t hdr = grp[4 + blk];
            const int shift = std::min<int>(12, hdr & 0xFu);
            const int filter = std::min<int>(3, (hdr >> 4) & 3u);
            const std::int32_t f0 = kAdpcmPos[filter];
            const std::int32_t f1 = kAdpcmNeg[filter];
            const int ch = stereo ? (blk & 1) : 0;

            std::int16_t unit_pcm[28];
            for (int w = 0; w < 28; ++w) {
                const std::uint8_t b = grp[16 + w * 4 + (blk >> 1)];
                const int nib = (blk & 1) ? (b >> 4) : (b & 0xFu);
                const std::int32_t sn = static_cast<std::int16_t>(nib << 12) >> shift;
                const std::int32_t p = sn + ((xa_s1_[ch] * f0 + xa_s2_[ch] * f1 + 32) >> 6);
                const std::int16_t cl = clamp16(p);
                unit_pcm[w] = cl;
                xa_s2_[ch] = xa_s1_[ch];
                xa_s1_[ch] = cl;
            }

            if (!stereo) {
                for (int w = 0; w < 28; ++w) {
                    decoded_lr.push_back(unit_pcm[w]);
                    decoded_lr.push_back(unit_pcm[w]);
                }
            } else if ((blk & 1) == 0) {
                // Temporarily store left channel in decoded_lr; right channel (blk+1) pairs with it
                for (int w = 0; w < 28; ++w) {
                    decoded_lr.push_back(unit_pcm[w]);
                    decoded_lr.push_back(0);
                }
            } else {
                const std::size_t base = decoded_lr.size() - 56u;
                for (int w = 0; w < 28; ++w) {
                    decoded_lr[base + static_cast<std::size_t>(w) * 2u + 1u] = unit_pcm[w];
                }
            }
        }
    }

    // Resample from 37,800 Hz (or 18,900 Hz) to 48,000 Hz stereo
    const int src_rate = rate_18900 ? 18900 : 37800;
    const std::size_t src_frames = decoded_lr.size() / 2u;
    if (src_frames == 0) return;
    const std::size_t dst_frames = (src_frames * kOutputRate) / static_cast<std::size_t>(src_rate);

    if (xa_queue_.size() > kOutputRate * 4u) {
        xa_queue_.erase(xa_queue_.begin(), xa_queue_.begin() + static_cast<std::ptrdiff_t>(dst_frames * 2u));
    }

    for (std::size_t i = 0; i < dst_frames; ++i) {
        const std::size_t sidx = std::min(src_frames - 1u, (i * static_cast<std::size_t>(src_rate)) / kOutputRate);
        xa_queue_.push_back(decoded_lr[sidx * 2u + 0u]);
        xa_queue_.push_back(decoded_lr[sidx * 2u + 1u]);
    }
}

std::uint16_t Spu::read_reg(std::uint32_t phys) {
    if (phys < 0x1F801C00u || phys >= 0x1F801E00u) return 0u;
    const std::uint32_t off = phys - 0x1F801C00u;

    if (off < 0x180u) {
        const int vidx = static_cast<int>(off >> 4);
        const std::uint32_t vreg = off & 0xFu;
        const Voice &v = voices[vidx];
        switch (vreg) {
        case 0x0Cu: return static_cast<std::uint16_t>(v.env_level);
        case 0x0Eu: return v.repeat_addr;
        default: return regs[off >> 1];
        }
    }

    switch (phys) {
    case 0x1F801D88u: return static_cast<std::uint16_t>(kon & 0xFFFFu);
    case 0x1F801D8Au: return static_cast<std::uint16_t>(kon >> 16);
    case 0x1F801D8Cu: return static_cast<std::uint16_t>(koff & 0xFFFFu);
    case 0x1F801D8Eu: return static_cast<std::uint16_t>(koff >> 16);
    case 0x1F801D9Cu: return static_cast<std::uint16_t>(endx & 0xFFFFu);
    case 0x1F801D9Eu: return static_cast<std::uint16_t>(endx >> 16);
    case 0x1F801DAAu: return spucnt;
    case 0x1F801DAEu: return static_cast<std::uint16_t>((spucnt & 0x3Fu) | ((spucnt & 0x30u) ? 0x80u : 0u));
    default: return regs[off >> 1];
    }
}

void Spu::write_reg(std::uint32_t phys, std::uint16_t val) {
    if (phys < 0x1F801C00u || phys >= 0x1F801E00u) return;
    const std::uint32_t off = phys - 0x1F801C00u;
    regs[off >> 1] = val;

    if (off < 0x180u) {
        const int vidx = static_cast<int>(off >> 4);
        const std::uint32_t vreg = off & 0xFu;
        Voice &v = voices[vidx];
        switch (vreg) {
        case 0x00u: v.vol_l = decode_vol(val); break;
        case 0x02u: v.vol_r = decode_vol(val); break;
        case 0x04u: v.pitch = val; break;
        case 0x06u: v.start_addr = val; break;
        case 0x08u: v.adsr1 = val; break;
        case 0x0Au: v.adsr2 = val; break;
        case 0x0Cu: v.env_level = static_cast<std::int16_t>(val); break;
        case 0x0Eu: v.repeat_addr = val; v.repeat_written = true; break;
        }
        return;
    }

    switch (phys) {
    case 0x1F801D80u: main_vol_l = decode_vol(val); break;
    case 0x1F801D82u: main_vol_r = decode_vol(val); break;
    case 0x1F801D88u:
        kon = (kon & 0xFFFF0000u) | val;
        for (int i = 0; i < 16; ++i) { if (val & (1u << i)) key_on_voice(i); }
        break;
    case 0x1F801D8Au:
        kon = (kon & 0x0000FFFFu) | (static_cast<std::uint32_t>(val) << 16);
        for (int i = 0; i < 8; ++i) { if (val & (1u << i)) key_on_voice(16 + i); }
        break;
    case 0x1F801D8Cu:
        koff = (koff & 0xFFFF0000u) | val;
        for (int i = 0; i < 16; ++i) { if (val & (1u << i)) key_off_voice(i); }
        break;
    case 0x1F801D8Eu:
        koff = (koff & 0x0000FFFFu) | (static_cast<std::uint32_t>(val) << 16);
        for (int i = 0; i < 8; ++i) { if (val & (1u << i)) key_off_voice(16 + i); }
        break;
    case 0x1F801DA4u: irq_addr = static_cast<std::uint32_t>(val) << 3; break;
    case 0x1F801DA6u: transfer_addr = (static_cast<std::uint32_t>(val) << 3) & kSpuRamMask; break;
    case 0x1F801DA8u: write_dma16(val); break;
    case 0x1F801DAAu: spucnt = val; break;
    case 0x1F801DB0u: cd_vol_l = static_cast<std::int16_t>(val); break;
    case 0x1F801DB2u: cd_vol_r = static_cast<std::int16_t>(val); break;
    default: break;
    }
}

void Spu::write_dma16(std::uint16_t val) {
    const std::uint32_t addr = transfer_addr & kSpuRamMask & ~1u;
    std::memcpy(ram.data() + addr, &val, 2);
    transfer_addr = (addr + 2u) & kSpuRamMask;
}

std::uint16_t Spu::read_dma16() {
    const std::uint32_t addr = transfer_addr & kSpuRamMask & ~1u;
    std::uint16_t val = 0;
    std::memcpy(&val, ram.data() + addr, 2);
    transfer_addr = (addr + 2u) & kSpuRamMask;
    return val;
}

}  // namespace psx
