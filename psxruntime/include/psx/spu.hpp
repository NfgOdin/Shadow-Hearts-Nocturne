#pragma once

#include "psx/system.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace psx {

// PlayStation 1 SPU1 (24 ADPCM voices, 512 KiB Sound RAM at 0x1F801C00..0x1F801DFF)
// plus CD-ROM XA-ADPCM 4-bit/8-bit stereo/mono stream decoder.
// Mixes to 48,000 Hz stereo PCM frames compatible with Nocturne's SoundPipe time-stretcher.
class Spu final : public SpuSink {
public:
    static constexpr std::uint32_t kSpuRamSize = 512u * 1024u;  // 512 KiB
    static constexpr std::uint32_t kSpuRamMask = kSpuRamSize - 1u;
    static constexpr int kNumVoices = 24;
    static constexpr int kOutputRate = 48000;

    enum class EnvPhase : std::uint8_t { Off = 0, Attack, Decay, Sustain, Release };

    struct Voice {
        std::int16_t vol_l = 0;
        std::int16_t vol_r = 0;
        std::uint16_t pitch = 0;
        std::uint16_t start_addr = 0;     // in 8-byte units
        std::uint16_t adsr1 = 0;
        std::uint16_t adsr2 = 0;
        std::int16_t env_level = 0;
        std::uint16_t repeat_addr = 0;    // in 8-byte units

        std::uint32_t cur_addr = 0;       // byte offset in SPU RAM
        std::uint32_t pitch_counter = 0;  // 16.12 fixed point
        EnvPhase phase = EnvPhase::Off;
        int env_timer = 0;
        bool repeat_written = false;
        std::int32_t s1 = 0, s2 = 0;
        std::int16_t block_pcm[28] = {};
        int block_idx = 28;
    };

    using AudioCallback = std::function<void(const std::int16_t *stereo_lr, std::size_t frames)>;

    std::vector<std::uint8_t> ram;
    std::array<Voice, kNumVoices> voices = {};
    std::array<std::uint16_t, 0x100> regs = {};

    std::int16_t main_vol_l = 0x3FFF;
    std::int16_t main_vol_r = 0x3FFF;
    std::int16_t cd_vol_l = 0x7FFF;
    std::int16_t cd_vol_r = 0x7FFF;
    std::uint32_t kon = 0;
    std::uint32_t koff = 0;
    std::uint32_t endx = 0x00FFFFFFu;
    std::uint16_t spucnt = 0;
    std::uint16_t spustat = 0;
    std::uint32_t transfer_addr = 0;
    std::uint32_t irq_addr = 0;

    float bgm_gain = 1.0f;  // XA / CD-DA gain from F1 Audio settings
    float sfx_gain = 1.0f;  // SPU voice gain from F1 Audio settings

    Spu();
    ~Spu() override = default;

    void reset() override;
    std::uint16_t read_reg(std::uint32_t phys) override;
    void write_reg(std::uint32_t phys, std::uint16_t val) override;
    void write_dma16(std::uint16_t val) override;
    std::uint16_t read_dma16() override;
    void tick(std::uint32_t cycles) override;

    // Decode a 2352-byte Mode 2 Form 2 XA-ADPCM audio sector from Cdrom.
    void decode_xa_sector(const std::uint8_t *raw2352);

    void set_audio_callback(AudioCallback cb) { audio_cb_ = std::move(cb); }

private:
    void key_on_voice(int v);
    void key_off_voice(int v);
    void decode_voice_block(Voice &v);
    void step_envelope(Voice &v);
    void sample_one_48k(std::int16_t &out_l, std::int16_t &out_r);

    std::uint32_t cycle_accum_ = 0;
    std::vector<std::int16_t> batch_pcm_;

    // XA-ADPCM resampled 48 kHz stereo queue
    std::deque<std::int16_t> xa_queue_;
    std::int32_t xa_s1_[2] = {};
    std::int32_t xa_s2_[2] = {};

    AudioCallback audio_cb_;
};

}  // namespace psx
