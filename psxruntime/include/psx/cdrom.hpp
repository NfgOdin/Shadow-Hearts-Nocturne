#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace psx {

class System;

// Virtual 4-Disc PS1 CD-ROM Controller (0x1F801800..0x1F801803).
// Serves Mode 2 Form 1 (2048 B), Mode 2 Form 2 / Real-Time RAW2352 (XA/STR),
// ISO9660 metadata (iso_meta.bin), and constant-byte FILL ranges directly from
// games/koudelka/disc_files/disc_manifest.tsv with zero ISO files needed.
class Cdrom {
public:
    enum class StorageKind : std::uint8_t {
        Meta = 0,
        Form1 = 1,
        Raw2352 = 2,
        Fill = 3,
    };

    struct Entry {
        std::uint32_t start_lba = 0;
        std::uint32_t end_lba = 0;      // exclusive
        std::uint32_t size_bytes = 0;
        StorageKind kind = StorageKind::Form1;
        std::uint8_t fill_byte = 0;
        std::string iso_path;
        std::string rel_path;           // relative to disc_files_root
    };

    struct DiscImage {
        int disc_num = 0;               // 1..4
        std::string disc_id;            // e.g. SLUS_01051
        std::uint32_t total_sectors = 0;
        std::vector<Entry> entries;     // sorted by start_lba
    };

    using XaAudioCallback = std::function<void(const std::uint8_t *raw2352, std::uint8_t file, std::uint8_t channel)>;

    void reset();

    // Load unified disc_manifest.tsv (discs 1..4) from a virtual disc root directory.
    bool load_manifest(const std::string &disc_files_root, std::string *err_out = nullptr);

    // Load a synthetic in-memory disc (for unit tests).
    void clear_discs();
    void set_disc_root(const std::string &root) { root_dir_ = root; }
    void add_disc(DiscImage disc);

    // Active disc (1-based: 1..4) and seamless 4-disc swap (simulates lid open -> close -> spinup).
    int active_disc() const noexcept { return active_disc_; }
    int disc_count() const noexcept { return static_cast<int>(discs_.size()); }
    bool set_active_disc_immediate(int disc_num);
    bool swap_disc(int disc_num);
    bool lid_open() const noexcept { return shell_open_cycles_ > 0; }

    // Direct sector read (2352 raw bytes or 2048 user bytes) for verification, DMA, or tests.
    bool read_sector_raw2352(int disc_num, std::uint32_t lba, std::uint8_t out[2352]) const;
    bool read_sector_user2048(int disc_num, std::uint32_t lba, std::uint8_t out[2048]) const;
    std::uint32_t disc_total_sectors(int disc_num) const;

    // Register interface at 0x1F801800..0x1F801803.
    std::uint8_t read_reg(std::uint32_t offset);
    void write_reg(System &sys, std::uint32_t offset, std::uint8_t val);

    // DMA3 (CD-ROM -> RAM) word read.
    std::uint32_t read_dma32();
    bool data_ready() const noexcept { return data_pos_ < data_buf_.size(); }

    // Advance controller state machine by CPU cycles (fires IRQ2 via sys when responses/sectors arrive).
    void tick(System &sys, std::uint32_t cycles);

    // Optional XA-ADPCM audio callback (hooked by Milestone 3 SPU1/XA mixer).
    void set_xa_callback(XaAudioCallback cb) { xa_cb_ = std::move(cb); }

    void serialize_state(std::ostream &os) const;
    bool deserialize_state(std::istream &is);

    // BCD / MSF helpers (PS1 MSF has 00:02:00 = 150 sector pregap).
    static std::uint8_t to_bcd(std::uint32_t v) noexcept;
    static std::uint32_t from_bcd(std::uint8_t bcd) noexcept;
    static void lba_to_msf(std::uint32_t lba, std::uint8_t &m, std::uint8_t &s, std::uint8_t &f) noexcept;
    static std::uint32_t msf_to_lba(std::uint8_t m_bcd, std::uint8_t s_bcd, std::uint8_t f_bcd) noexcept;

private:
    struct PendingIrq {
        std::uint8_t type = 0;          // 1=INT1(data), 2=INT2(complete), 3=INT3(ack), 5=INT5(error)
        std::vector<std::uint8_t> resp;
        std::uint32_t delay_cycles = 0;
        bool is_sector = false;
        std::uint32_t sector_lba = 0;
    };

    const DiscImage *find_disc(int disc_num) const noexcept;
    std::uint8_t status_byte() const noexcept;
    void queue_irq(std::uint8_t type, std::initializer_list<std::uint8_t> resp, std::uint32_t delay_cycles);
    void execute_command(System &sys, std::uint8_t cmd);
    void deliver_sector(System &sys, std::uint32_t lba);
    void update_irq_line(System &sys);

    std::string root_dir_;
    std::vector<DiscImage> discs_;
    int active_disc_ = 1;
    int pending_swap_disc_ = 0;
    std::uint32_t shell_open_cycles_ = 0;

    // Hardware register state
    std::uint8_t index_ = 0;
    std::uint8_t irq_enable_ = 0x1Fu;
    std::uint8_t irq_flag_ = 0;
    std::uint8_t mode_ = 0;             // Setmode: bit7=2x speed, bit6=XA-ADPCM, bit5=sector size(2340), bit3=XA filter
    std::uint8_t filter_file_ = 0;
    std::uint8_t filter_channel_ = 0;
    bool muted_ = false;
    bool motor_on_ = true;
    bool reading_ = false;
    bool seeking_ = false;
    bool playing_cdda_ = false;
    bool shell_was_opened_ = false;

    std::uint32_t setloc_lba_ = 0;
    std::uint32_t cur_lba_ = 0;
    std::uint32_t read_start_lba_ = 0;
    bool setloc_pending_ = false;
    std::uint32_t read_timer_ = 0;

    std::deque<std::uint8_t> param_fifo_;
    std::deque<std::uint8_t> resp_fifo_;
    std::deque<PendingIrq> irq_queue_;

    // Current sector data buffer exposed to CPU/DMA3 when BFRD (0x80) is written to 0x1F801803.0
    std::array<std::uint8_t, 2352> last_raw_sector_ = {};
    std::vector<std::uint8_t> data_buf_;
    std::size_t data_pos_ = 0;
    bool bfrd_enabled_ = false;

    // Last sector header/subheader for GetlocL
    std::array<std::uint8_t, 8> last_loc_l_ = {};

    // Volume/XA mixing registers (Index 2/3)
    std::uint8_t vol_ll_ = 0x80, vol_lr_ = 0x00, vol_rl_ = 0x00, vol_rr_ = 0x80;

    // Cached file buffer to avoid reopening std::ifstream on consecutive sector reads
    mutable std::string cached_rel_path_;
    mutable std::vector<std::uint8_t> cached_file_bytes_;

    XaAudioCallback xa_cb_;
};

}  // namespace psx
