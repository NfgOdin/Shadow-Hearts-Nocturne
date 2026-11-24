#include "psx/spu.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace psx {

namespace {

// Sony PS1 SPU / XA ADPCM filter coefficients (64-scale fixed point).
constexpr std::int32_t kAdpcmPos[5] = {0, 60, 115, 98, 122};
constexpr std::int32_t kAdpcmNeg[5] = {0, 0, -52, -55, -60};

// 33.8688 MHz / 48,000 Hz = 705.6 CPU cycles per 48 kHz output sample (exact ratio: 3528 / 5).
constexpr std::uint32_t kCyclesNum = 5u;
constexpr std::uint32_t kCyclesPer48kScaled = 3528u;
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
    active_voice_mask_ = 0u;
    regs.fill(0u);
    main_vol_l = 0x7FFF;
    main_vol_r = 0x7FFF;
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
    env_44k_accum_ = 0;
    batch_pcm_.clear();
    batch_pcm_.reserve(kAudioBatchFrames * 2u);
    voice_prev_pcm_.fill(0);
    for (auto &h : voice_hist_) h.fill(0);
    reverb_buf_l_.fill(0.0f);
    reverb_buf_r_.fill(0.0f);
    reverb_pos_ = 0;
    xa_queue_.clear();
    xa_s1_[0] = xa_s1_[1] = 0;
    xa_s2_[0] = xa_s2_[1] = 0;
    xa_last_lr_[0] = xa_last_lr_[1] = 0;
    xa_last_file_ = -1;
    xa_last_channel_ = -1;
}

void Spu::check_irq(std::uint32_t byte_addr) {
    if ((spucnt & 0x0040u) == 0) return;
    if ((byte_addr & kSpuRamMask & ~15u) == (irq_addr & kSpuRamMask & ~15u)) {
        if ((spustat & 0x0040u) == 0) {
            spustat |= 0x0040u;
            if (irq_cb_) irq_cb_();
        }
    }
}

void Spu::key_on_voice(int idx) {
    Voice &v = voices[idx];
    v.cur_addr = (static_cast<std::uint32_t>(v.start_addr) << 3) & kSpuRamMask & ~15u;
    v.pitch_counter = 0;
    v.phase = EnvPhase::Attack;
    active_voice_mask_ |= (1u << idx);
    v.env_level = 0;
    v.env_timer = 0;
    v.repeat_written = false;
    v.end_after_block = false;
    v.s1 = 0;
    v.s2 = 0;
    std::memset(v.block_pcm, 0, sizeof(v.block_pcm));
    v.block_idx = 28;
    voice_prev_pcm_[idx] = 0;
    voice_hist_[idx].fill(0);
    endx &= ~(1u << idx);
}

void Spu::key_off_voice(int idx) {
    Voice &v = voices[idx];
    if (v.phase != EnvPhase::Off) {
        v.phase = EnvPhase::Release;
        v.env_timer = 0;
    }
}

void Spu::step_envelope(Voice &v) {
    if (v.phase == EnvPhase::Off) {
        v.env_level = 0;
        return;
    }
    const int sustain_level = std::min<int>(0x7FFF, ((v.adsr1 & 0xF) + 1) * 0x800);
    switch (v.phase) {
    case EnvPhase::Attack: {
        const bool exp_mode = (v.adsr1 & 0x8000u) != 0;
        const int shift = (v.adsr1 >> 10) & 0x1F;
        const int step_idx = (v.adsr1 >> 8) & 3;
        int cycles = (shift > 11) ? (1 << std::min(20, shift - 11)) : 1;
        if (exp_mode && v.env_level > 0x6000) cycles <<= 2;
        if (++v.env_timer < cycles) return;
        v.env_timer = 0;
        const int step = (7 - step_idx) << std::max(0, 11 - shift);
        v.env_level = static_cast<std::int16_t>(std::min(0x7FFF, v.env_level + step));
        if (v.env_level >= 0x7FFF) {
            v.phase = EnvPhase::Decay;
            v.env_timer = 0;
        }
        break;
    }
    case EnvPhase::Decay: {
        const int shift = (v.adsr1 >> 4) & 0xF;
        const int cycles = (shift > 11) ? (1 << (shift - 11)) : 1;
        if (++v.env_timer < cycles) return;
        v.env_timer = 0;
        const int base = 8 << std::max(0, 11 - shift);
        const int delta = std::max(1, (base * v.env_level) >> 15);
        v.env_level = static_cast<std::int16_t>(std::max(sustain_level, v.env_level - delta));
        if (v.env_level <= sustain_level) {
            v.phase = EnvPhase::Sustain;
            v.env_timer = 0;
        }
        break;
    }
    case EnvPhase::Sustain: {
        const bool exp_mode = (v.adsr2 & 0x8000u) != 0;
        const bool dec = (v.adsr2 & 0x4000u) != 0;
        const int shift = (v.adsr2 >> 8) & 0x1F;
        const int step_idx = (v.adsr2 >> 6) & 3;
        if (shift == 0x1F && step_idx == 3) break;  // Infinite sustain hold
        int cycles = (shift > 11) ? (1 << std::min(20, shift - 11)) : 1;
        if (!dec && exp_mode && v.env_level > 0x6000) cycles <<= 2;
        if (++v.env_timer < cycles) return;
        v.env_timer = 0;
        if (dec) {
            const int base = (8 - step_idx) << std::max(0, 11 - shift);
            const int delta = exp_mode ? std::max(1, (base * v.env_level) >> 15) : base;
            v.env_level = static_cast<std::int16_t>(std::max(0, v.env_level - delta));
        } else {
            const int step = (7 - step_idx) << std::max(0, 11 - shift);
            v.env_level = static_cast<std::int16_t>(std::min(0x7FFF, v.env_level + step));
        }
        break;
    }
    case EnvPhase::Release: {
        const bool exp_mode = (v.adsr2 & 0x0020u) != 0;
        const int shift = v.adsr2 & 0x1F;
        const int cycles = (shift > 11) ? (1 << std::min(20, shift - 11)) : 1;
        if (++v.env_timer < cycles) return;
        v.env_timer = 0;
        const int base = 8 << std::max(0, 11 - shift);
        const int delta = exp_mode ? std::max(1, (base * v.env_level) >> 15) : base;
        v.env_level = static_cast<std::int16_t>(std::max(0, v.env_level - delta));
        if (v.env_level == 0) v.phase = EnvPhase::Off;
        break;
    }
    default: break;
    }
}

void Spu::decode_voice_block(Voice &v) {
    const std::uint32_t addr = v.cur_addr & kSpuRamMask & ~15u;
    check_irq(addr);

    const std::uint8_t shift_filter = ram[addr];
    const std::uint8_t flags = ram[addr + 1u];

    int shift = shift_filter & 0xFu;
    if (shift > 12) shift = 9;  // Real PS1 SPU hardware treats shifts 13..15 as 9
    const int filter = std::min<int>(4, (shift_filter >> 4) & 0x7u);
    const std::int32_t f0 = kAdpcmPos[filter];
    const std::int32_t f1 = kAdpcmNeg[filter];

    if ((flags & 4u) && !v.repeat_written) {
        v.repeat_addr = static_cast<std::uint16_t>(addr >> 3);
    }

    const int vidx = static_cast<int>(&v - voices.data());
    voice_hist_[vidx][0] = v.block_pcm[25];
    voice_hist_[vidx][1] = v.block_pcm[26];
    voice_hist_[vidx][2] = v.block_pcm[27];
    voice_prev_pcm_[vidx] = v.block_pcm[27];

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

    if (flags & 1u) {  // Loop end
        endx |= (1u << vidx);
        if (flags & 2u) {  // Loop repeat
            v.cur_addr = (static_cast<std::uint32_t>(v.repeat_addr) << 3) & kSpuRamMask & ~15u;
        } else {
            v.end_after_block = true;
        }
    }
}

void Spu::sample_one_48k(std::int16_t &out_l, std::int16_t &out_r) {
    std::int32_t mix_l = 0;
    std::int32_t mix_r = 0;
    std::int32_t rev_in_l = 0;
    std::int32_t rev_in_r = 0;

    // EON (Reverb Enable per-voice bitmask at 0x1F801D98..1D9A -> regs[0xCC..0xCD])
    const std::uint32_t eon_mask = static_cast<std::uint32_t>(regs[0xCC]) | (static_cast<std::uint32_t>(regs[0xCD]) << 16);
    const bool reverb_master = (spucnt & 0x0080u) != 0;

    // Tick ADSR envelopes at exact PS1 44.1 kHz cadence (441 / 480 ratio)
    env_44k_accum_ += 441u;
    const bool tick_env = (env_44k_accum_ >= 480u);
    if (tick_env) env_44k_accum_ -= 480u;

    if ((spucnt & 0x8000u) && active_voice_mask_ != 0u) {
        std::uint32_t mask = active_voice_mask_;
        while (mask != 0u) {
            const int i = std::countr_zero(mask);
            mask &= (mask - 1u);
            Voice &v = voices[i];
            if (v.phase == EnvPhase::Off) {
                active_voice_mask_ &= ~(1u << i);
                continue;
            }
            while (v.block_idx >= 28) {
                if (v.end_after_block) {
                    v.phase = EnvPhase::Off;
                    v.env_level = 0;
                    v.end_after_block = false;
                    active_voice_mask_ &= ~(1u << i);
                    break;
                }
                decode_voice_block(v);
                v.block_idx -= 28;
            }
            if (v.phase == EnvPhase::Off) {
                active_voice_mask_ &= ~(1u << i);
                continue;
            }

            if (tick_env) {
                step_envelope(v);
                if (v.phase == EnvPhase::Off) active_voice_mask_ &= ~(1u << i);
            }

            // 4-tap smooth PS1 Gaussian-style cubic interpolation using 3-sample block history
            const int idx = std::clamp(v.block_idx, 0, 27);
            auto sample_at = [&](int k) -> std::int32_t {
                if (k >= 0) return v.block_pcm[k];
                const int hk = 3 + k;
                return (hk >= 0 && hk < 3) ? voice_hist_[i][hk] : 0;
            };
            const std::int32_t xm1 = sample_at(idx - 2);
            const std::int32_t x0  = sample_at(idx - 1);
            const std::int32_t x1  = sample_at(idx);
            const std::int32_t x2  = (idx + 1 < 28) ? v.block_pcm[idx + 1] : x1;
            const float t = static_cast<float>(v.pitch_counter & 0xFFFu) * (1.0f / 4096.0f);
            // Smooth 4-tap B-spline (authentic warm PS1 Gaussian low-pass rolloff, zero overshoot)
            const float omt = 1.0f - t;
            const float w0 = (omt * omt * omt) * (1.0f / 6.0f);
            const float w1 = (3.0f * t * t * t - 6.0f * t * t + 4.0f) * (1.0f / 6.0f);
            const float w2 = (-3.0f * t * t * t + 3.0f * t * t + 3.0f * t + 1.0f) * (1.0f / 6.0f);
            const float w3 = (t * t * t) * (1.0f / 6.0f);
            const std::int32_t interp = static_cast<std::int32_t>(w0 * xm1 + w1 * x0 + w2 * x1 + w3 * x2);

            const std::int32_t s = (interp * v.env_level) >> 15;
            const std::int32_t vl = (s * v.vol_l) >> 15;
            const std::int32_t vr = (s * v.vol_r) >> 15;
            mix_l += vl;
            mix_r += vr;
            if (reverb_master && (eon_mask & (1u << i))) {
                rev_in_l += vl;
                rev_in_r += vr;
            }

            // Advance pitch counter (scaled from 44.1 kHz to 48 kHz: * 441 / 480 = * 147 / 160)
            const std::uint32_t step = (std::min<std::uint32_t>(v.pitch, 0x3FFFu) * 147u) / 160u;
            v.pitch_counter += step;
            v.block_idx += static_cast<int>(v.pitch_counter >> 12);
            v.pitch_counter &= 0xFFFu;
        }
    }

    // SPU1 Hardware Reverb output (vLOUT = regs[0xC2] @ 0x1F801D84, vROUT = regs[0xC3] @ 0x1F801D86)
    const std::int16_t rev_vol_l = static_cast<std::int16_t>(regs[0xC2]);
    const std::int16_t rev_vol_r = static_cast<std::int16_t>(regs[0xC3]);
    if (reverb_master && (rev_vol_l != 0 || rev_vol_r != 0)) {
        // Multi-tap gothic hall feedback delay (prime tap spacings @ 48 kHz for lush, metallic-free decay)
        const std::size_t p = reverb_pos_;
        const float tap1_l = reverb_buf_l_[(p + kReverbDelaySize - 1789) & (kReverbDelaySize - 1)];
        const float tap2_l = reverb_buf_l_[(p + kReverbDelaySize - 3571) & (kReverbDelaySize - 1)];
        const float tap3_l = reverb_buf_l_[(p + kReverbDelaySize - 5923) & (kReverbDelaySize - 1)];
        const float tap1_r = reverb_buf_r_[(p + kReverbDelaySize - 1931) & (kReverbDelaySize - 1)];
        const float tap2_r = reverb_buf_r_[(p + kReverbDelaySize - 3847) & (kReverbDelaySize - 1)];
        const float tap3_r = reverb_buf_r_[(p + kReverbDelaySize - 6317) & (kReverbDelaySize - 1)];
        const float wet_l = (tap1_l * 0.45f + tap2_l * 0.35f + tap3_l * 0.20f);
        const float wet_r = (tap1_r * 0.45f + tap2_r * 0.35f + tap3_r * 0.20f);
        // Cross-coupled low-passed feedback (prevents runaway and gives warm stone-monastery tail)
        reverb_buf_l_[p] = std::clamp(static_cast<float>(rev_in_l) * 0.55f + wet_r * 0.48f, -32768.0f, 32767.0f);
        reverb_buf_r_[p] = std::clamp(static_cast<float>(rev_in_r) * 0.55f + wet_l * 0.48f, -32768.0f, 32767.0f);
        reverb_pos_ = (p + 1u) & (kReverbDelaySize - 1);
        mix_l += (static_cast<std::int32_t>(wet_l) * rev_vol_l) >> 15;
        mix_r += (static_cast<std::int32_t>(wet_r) * rev_vol_r) >> 15;
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

    mix_l = (mix_l * main_vol_l) >> 15;
    mix_r = (mix_r * main_vol_r) >> 15;
    out_l = clamp16(mix_l);
    out_r = clamp16(mix_r);
}

void Spu::tick(std::uint32_t cycles) {
    cycle_accum_ += cycles * kCyclesNum;
    while (cycle_accum_ >= kCyclesPer48kScaled) {
        cycle_accum_ -= kCyclesPer48kScaled;
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
    const int file = static_cast<int>(raw2352[16]);
    const int channel = static_cast<int>(raw2352[17]);
    if (file != xa_last_file_ || channel != xa_last_channel_) {
        xa_s1_[0] = xa_s1_[1] = 0;
        xa_s2_[0] = xa_s2_[1] = 0;
        xa_last_lr_[0] = xa_last_lr_[1] = 0;
        xa_last_file_ = file;
        xa_last_channel_ = channel;
    }

    const std::uint8_t coding = raw2352[19];
    const bool stereo = (coding & 3u) == 1u;
    const bool rate_18900 = ((coding >> 2) & 3u) == 1u;
    const bool bits_8 = ((coding >> 4) & 3u) == 1u;

    const std::uint8_t *payload = raw2352 + 24;
    std::vector<std::int16_t> decoded_lr;
    decoded_lr.reserve(8064);

    // 18 sound groups per Mode 2 Form 2 XA sector (128 bytes each)
    const int num_blks = bits_8 ? 4 : 8;
    for (int sg = 0; sg < 18; ++sg) {
        const std::uint8_t *grp = payload + sg * 128;
        for (int blk = 0; blk < num_blks; ++blk) {
            const std::uint8_t hdr = grp[4 + blk];
            int shift = hdr & 0xFu;
            if (shift > 12) shift = 9;
            const int filter = std::min<int>(3, (hdr >> 4) & 3u);
            const std::int32_t f0 = kAdpcmPos[filter];
            const std::int32_t f1 = kAdpcmNeg[filter];
            const int ch = stereo ? (blk & 1) : 0;

            std::int16_t unit_pcm[28];
            for (int w = 0; w < 28; ++w) {
                std::int32_t sn = 0;
                if (bits_8) {
                    const std::int8_t sb = static_cast<std::int8_t>(grp[16 + w * 4 + blk]);
                    sn = (static_cast<std::int32_t>(sb) << 8) >> shift;
                } else {
                    const std::uint8_t b = grp[16 + w * 4 + (blk >> 1)];
                    const int nib = (blk & 1) ? (b >> 4) : (b & 0xFu);
                    sn = static_cast<std::int16_t>(nib << 12) >> shift;
                }
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

    // Linear-interpolated resample from 37,800 Hz (or 18,900 Hz) to 48,000 Hz stereo
    const int src_rate = rate_18900 ? 18900 : 37800;
    const std::size_t src_frames = decoded_lr.size() / 2u;
    if (src_frames == 0) return;
    const std::size_t dst_frames = (src_frames * kOutputRate) / static_cast<std::size_t>(src_rate);

    if (xa_queue_.size() > kOutputRate * 4u) {
        xa_queue_.erase(xa_queue_.begin(), xa_queue_.begin() + static_cast<std::ptrdiff_t>(dst_frames * 2u));
    }

    for (std::size_t i = 0; i < dst_frames; ++i) {
        const std::uint64_t pos_fp = (static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(src_rate) << 12) / kOutputRate;
        const std::size_t sidx = static_cast<std::size_t>(pos_fp >> 12);
        const std::int32_t frac = static_cast<std::int32_t>(pos_fp & 0xFFFu);
        const std::int32_t l0 = (sidx == 0) ? xa_last_lr_[0] : decoded_lr[std::min(sidx - 1u, src_frames - 1u) * 2u + 0u];
        const std::int32_t r0 = (sidx == 0) ? xa_last_lr_[1] : decoded_lr[std::min(sidx - 1u, src_frames - 1u) * 2u + 1u];
        const std::int32_t l1 = decoded_lr[std::min(sidx, src_frames - 1u) * 2u + 0u];
        const std::int32_t r1 = decoded_lr[std::min(sidx, src_frames - 1u) * 2u + 1u];
        xa_queue_.push_back(clamp16(l0 + (((l1 - l0) * frac) >> 12)));
        xa_queue_.push_back(clamp16(r0 + (((r1 - r0) * frac) >> 12)));
    }
    xa_last_lr_[0] = decoded_lr[(src_frames - 1u) * 2u + 0u];
    xa_last_lr_[1] = decoded_lr[(src_frames - 1u) * 2u + 1u];
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
    case 0x1F801DAEu:
        return static_cast<std::uint16_t>((spucnt & 0x3Fu) | (spustat & 0x0040u) | ((spucnt & 0x30u) ? 0x80u : 0u));
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
    case 0x1F801DAAu:
        spucnt = val;
        if ((val & 0x0040u) == 0) {
            spustat &= ~0x0040u;  // Disabling IRQ9 acknowledges/clears SPUSTAT bit 6
        }
        break;
    case 0x1F801DB0u: cd_vol_l = static_cast<std::int16_t>(val); break;
    case 0x1F801DB2u: cd_vol_r = static_cast<std::int16_t>(val); break;
    default: break;
    }
}

void Spu::write_dma16(std::uint16_t val) {
    const std::uint32_t addr = transfer_addr & kSpuRamMask & ~1u;
    check_irq(addr);
    std::memcpy(ram.data() + addr, &val, 2);
    transfer_addr = (addr + 2u) & kSpuRamMask;
}

std::uint16_t Spu::read_dma16() {
    const std::uint32_t addr = transfer_addr & kSpuRamMask & ~1u;
    check_irq(addr);
    std::uint16_t val = 0;
    std::memcpy(&val, ram.data() + addr, 2);
    transfer_addr = (addr + 2u) & kSpuRamMask;
    return val;
}

}  // namespace psx
