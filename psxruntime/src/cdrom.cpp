#include "psx/cdrom.hpp"
#include "psx/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace psx {

namespace {

constexpr std::uint8_t kSyncPattern[12] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

// Cycles between sectors: 1x = 75 sectors/sec (~451,584 cyc), 2x = 150 sectors/sec (~225,792 cyc).
// We use brisk-but-realistic timings so PsyQ CdSync / CdReadSync stay snappy without missing IRQs.
constexpr std::uint32_t kCmdAckDelay = 12000u;
constexpr std::uint32_t kSeekCompleteDelay = 30000u;
constexpr std::uint32_t kSectorCycles1x = 180000u;
constexpr std::uint32_t kSectorCycles2x = 90000u;
constexpr std::uint32_t kShellOpenDuration = 1200000u;  // ~2 frames of shell-open during disc swap

std::vector<std::string> split_tab(const std::string &line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t pos = line.find('\t', start);
        if (pos == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string join_path(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

}  // namespace

std::uint8_t Cdrom::to_bcd(std::uint32_t v) noexcept {
    return static_cast<std::uint8_t>(((v / 10u) << 4) | (v % 10u));
}

std::uint32_t Cdrom::from_bcd(std::uint8_t bcd) noexcept {
    return static_cast<std::uint32_t>((bcd >> 4) * 10u + (bcd & 0x0Fu));
}

void Cdrom::lba_to_msf(std::uint32_t lba, std::uint8_t &m, std::uint8_t &s, std::uint8_t &f) noexcept {
    const std::uint32_t total = lba + 150u;
    const std::uint32_t mins = total / (60u * 75u);
    const std::uint32_t rem = total % (60u * 75u);
    const std::uint32_t secs = rem / 75u;
    const std::uint32_t frms = rem % 75u;
    m = to_bcd(mins);
    s = to_bcd(secs);
    f = to_bcd(frms);
}

std::uint32_t Cdrom::msf_to_lba(std::uint8_t m_bcd, std::uint8_t s_bcd, std::uint8_t f_bcd) noexcept {
    const std::uint32_t m = from_bcd(m_bcd);
    const std::uint32_t s = from_bcd(s_bcd);
    const std::uint32_t f = from_bcd(f_bcd);
    const std::uint32_t total = (m * 60u + s) * 75u + f;
    return (total >= 150u) ? (total - 150u) : 0u;
}

void Cdrom::reset() {
    index_ = 0;
    irq_enable_ = 0x1Fu;
    irq_flag_ = 0;
    mode_ = 0;
    filter_file_ = 0;
    filter_channel_ = 0;
    muted_ = false;
    motor_on_ = true;
    reading_ = false;
    seeking_ = false;
    playing_cdda_ = false;
    shell_was_opened_ = false;
    pending_swap_disc_ = 0;
    shell_open_cycles_ = 0;
    setloc_lba_ = 0;
    cur_lba_ = 0;
    setloc_pending_ = false;
    read_timer_ = 0;
    param_fifo_.clear();
    resp_fifo_.clear();
    irq_queue_.clear();
    data_buf_.clear();
    data_pos_ = 0;
    bfrd_enabled_ = false;
    last_raw_sector_.fill(0);
    last_loc_l_.fill(0);
    vol_ll_ = 0x80;
    vol_lr_ = 0x00;
    vol_rl_ = 0x00;
    vol_rr_ = 0x80;
}

void Cdrom::clear_discs() {
    discs_.clear();
    active_disc_ = 1;
}

void Cdrom::add_disc(DiscImage disc) {
    std::sort(disc.entries.begin(), disc.entries.end(),
              [](const Entry &a, const Entry &b) { return a.start_lba < b.start_lba; });
    for (auto &existing : discs_) {
        if (existing.disc_num == disc.disc_num) {
            existing = std::move(disc);
            return;
        }
    }
    discs_.push_back(std::move(disc));
    std::sort(discs_.begin(), discs_.end(),
              [](const DiscImage &a, const DiscImage &b) { return a.disc_num < b.disc_num; });
}

bool Cdrom::load_manifest(const std::string &disc_files_root, std::string *err_out) {
    const std::string manifest_path = join_path(disc_files_root, "disc_manifest.tsv");
    std::ifstream ifs(manifest_path);
    if (!ifs.is_open()) {
        if (err_out) *err_out = "Cannot open disc_manifest.tsv at " + manifest_path;
        return false;
    }

    clear_discs();
    root_dir_ = disc_files_root;

    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto cols = split_tab(line);
        if (cols.empty()) continue;

        if (cols[0] == "DISC" && cols.size() >= 4) {
            DiscImage img;
            img.disc_num = std::atoi(cols[1].c_str());
            img.disc_id = cols[2];
            if (cols.size() >= 7) {
                // koudelka_prepare.py format: DISC  disc_num  disc_id  boot_exe  total_sectors  meta_sectors  meta_rel_path
                img.total_sectors = static_cast<std::uint32_t>(std::strtoul(cols[4].c_str(), nullptr, 10));
                const std::uint32_t meta_secs = static_cast<std::uint32_t>(std::strtoul(cols[5].c_str(), nullptr, 10));
                if (meta_secs > 0 && !cols[6].empty()) {
                    Entry me;
                    me.start_lba = 0;
                    me.end_lba = meta_secs;
                    me.size_bytes = meta_secs * 2352u;
                    me.kind = StorageKind::Meta;
                    me.iso_path = "@ISO_META";
                    me.rel_path = cols[6];
                    img.entries.push_back(std::move(me));
                }
            } else {
                img.total_sectors = static_cast<std::uint32_t>(std::strtoul(cols[3].c_str(), nullptr, 10));
            }
            add_disc(std::move(img));
            continue;
        }

        if (cols[0] == "FILE" && cols.size() >= 8) {
            // koudelka_prepare.py format: FILE  disc  lba  sectors  size_bytes  storage_mode  iso_path  rel_path  [sha256]
            const int dnum = std::atoi(cols[1].c_str());
            if (dnum <= 0) continue;
            DiscImage *img = nullptr;
            for (auto &d : discs_) {
                if (d.disc_num == dnum) { img = &d; break; }
            }
            if (!img) {
                DiscImage created;
                created.disc_num = dnum;
                add_disc(std::move(created));
                for (auto &d : discs_) {
                    if (d.disc_num == dnum) { img = &d; break; }
                }
            }
            Entry e;
            e.start_lba = static_cast<std::uint32_t>(std::strtoul(cols[2].c_str(), nullptr, 10));
            const std::uint32_t secs = static_cast<std::uint32_t>(std::strtoul(cols[3].c_str(), nullptr, 10));
            e.end_lba = e.start_lba + secs;
            e.size_bytes = static_cast<std::uint32_t>(std::strtoul(cols[4].c_str(), nullptr, 10));
            const std::string &sm = cols[5];
            if (sm == "F1" || sm == "FORM1") {
                e.kind = StorageKind::Form1;
            } else if (sm == "RAW2352") {
                e.kind = StorageKind::Raw2352;
            } else if (sm == "META") {
                e.kind = StorageKind::Meta;
            } else if (sm.rfind("FILL:", 0) == 0) {
                e.kind = StorageKind::Fill;
                e.fill_byte = static_cast<std::uint8_t>(std::strtoul(sm.c_str() + 5, nullptr, 16));
            } else {
                continue;
            }
            e.iso_path = cols[6];
            e.rel_path = cols[7];
            if (e.end_lba > img->total_sectors) img->total_sectors = e.end_lba;
            img->entries.push_back(std::move(e));
            continue;
        }

        // Synthetic 7-column format: disc  start_lba  end_lba  size_bytes  kind  iso_path  rel_path_or_fill
        if (cols.size() >= 7) {
            const int dnum = std::atoi(cols[0].c_str());
            if (dnum <= 0) continue;
            DiscImage *img = nullptr;
            for (auto &d : discs_) {
                if (d.disc_num == dnum) { img = &d; break; }
            }
            if (!img) {
                DiscImage created;
                created.disc_num = dnum;
                add_disc(std::move(created));
                for (auto &d : discs_) {
                    if (d.disc_num == dnum) { img = &d; break; }
                }
            }
            Entry e;
            e.start_lba = static_cast<std::uint32_t>(std::strtoul(cols[1].c_str(), nullptr, 10));
            e.end_lba = static_cast<std::uint32_t>(std::strtoul(cols[2].c_str(), nullptr, 10));
            e.size_bytes = static_cast<std::uint32_t>(std::strtoul(cols[3].c_str(), nullptr, 10));
            if (cols[4] == "META") {
                e.kind = StorageKind::Meta;
            } else if (cols[4] == "FORM1") {
                e.kind = StorageKind::Form1;
            } else if (cols[4] == "RAW2352") {
                e.kind = StorageKind::Raw2352;
            } else if (cols[4] == "FILL") {
                e.kind = StorageKind::Fill;
            } else {
                continue;
            }
            e.iso_path = cols[5];
            if (e.kind == StorageKind::Fill) {
                e.fill_byte = static_cast<std::uint8_t>(std::strtoul(cols[6].c_str(), nullptr, 16));
            } else {
                e.rel_path = cols[6];
            }
            if (e.end_lba > img->total_sectors) {
                img->total_sectors = e.end_lba;
            }
            img->entries.push_back(std::move(e));
        }
    }

    for (auto &d : discs_) {
        std::sort(d.entries.begin(), d.entries.end(),
                  [](const Entry &a, const Entry &b) { return a.start_lba < b.start_lba; });
    }

    if (discs_.empty()) {
        if (err_out) *err_out = "disc_manifest.tsv contained zero discs";
        return false;
    }
    active_disc_ = discs_.front().disc_num;
    return true;
}

const Cdrom::DiscImage *Cdrom::find_disc(int disc_num) const noexcept {
    for (const auto &d : discs_) {
        if (d.disc_num == disc_num) return &d;
    }
    return nullptr;
}

std::uint32_t Cdrom::disc_total_sectors(int disc_num) const {
    const DiscImage *d = find_disc(disc_num);
    return d ? d->total_sectors : 0u;
}

bool Cdrom::set_active_disc_immediate(int disc_num) {
    if (!find_disc(disc_num)) return false;
    active_disc_ = disc_num;
    pending_swap_disc_ = 0;
    shell_open_cycles_ = 0;
    return true;
}

bool Cdrom::swap_disc(int disc_num) {
    if (!find_disc(disc_num)) return false;
    pending_swap_disc_ = disc_num;
    shell_open_cycles_ = kShellOpenDuration;
    shell_was_opened_ = true;
    reading_ = false;
    seeking_ = false;
    playing_cdda_ = false;
    return true;
}

bool Cdrom::read_sector_raw2352(int disc_num, std::uint32_t lba, std::uint8_t out[2352]) const {
    std::memset(out, 0, 2352);
    const DiscImage *disc = find_disc(disc_num);
    if (!disc || lba >= disc->total_sectors) return false;

    // Synthesize standard Mode 2 header (sync + MSF + mode=2)
    std::memcpy(out, kSyncPattern, 12);
    lba_to_msf(lba, out[12], out[13], out[14]);
    out[15] = 0x02u;  // Mode 2

    // Binary search for matching entry where start_lba <= lba < end_lba
    auto it = std::upper_bound(disc->entries.begin(), disc->entries.end(), lba,
                               [](std::uint32_t target, const Entry &e) { return target < e.start_lba; });
    if (it != disc->entries.begin()) {
        --it;
        if (lba >= it->start_lba && lba < it->end_lba) {
            const std::uint32_t rel_sec = lba - it->start_lba;
            if (it->kind == StorageKind::Fill) {
                // Default Mode 2 Form 1 subheader: 00 00 08 00 (data)
                out[16] = 0x00; out[17] = 0x00; out[18] = 0x08; out[19] = 0x00;
                std::memcpy(out + 20, out + 16, 4);
                std::memset(out + 24, it->fill_byte, 2048);
                return true;
            }

            // Use single-file cache if small/medium (<32 MB), else direct stream read
            const std::string full_path = join_path(root_dir_, it->rel_path);
            const std::uint32_t stride = (it->kind == StorageKind::Form1) ? 2048u : 2352u;
            const std::uint64_t byte_off = static_cast<std::uint64_t>(rel_sec) * stride;

            auto read_slice = [&](std::uint8_t *dst, std::uint32_t count) -> bool {
                if (it->size_bytes <= 32u * 1024u * 1024u) {
                    if (cached_rel_path_ != full_path) {
                        std::ifstream ifs(full_path, std::ios::binary | std::ios::ate);
                        if (!ifs.is_open()) return false;
                        const std::streamsize fsz = ifs.tellg();
                        cached_file_bytes_.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, fsz)));
                        ifs.seekg(0, std::ios::beg);
                        if (fsz > 0) ifs.read(reinterpret_cast<char *>(cached_file_bytes_.data()), fsz);
                        cached_rel_path_ = full_path;
                    }
                    if (byte_off >= cached_file_bytes_.size()) return false;
                    const std::size_t avail = std::min<std::size_t>(count, cached_file_bytes_.size() - static_cast<std::size_t>(byte_off));
                    std::memcpy(dst, cached_file_bytes_.data() + byte_off, avail);
                    return true;
                }
                std::ifstream ifs(full_path, std::ios::binary);
                if (!ifs.is_open()) return false;
                ifs.seekg(static_cast<std::streamoff>(byte_off), std::ios::beg);
                ifs.read(reinterpret_cast<char *>(dst), count);
                return ifs.gcount() > 0;
            };

            if (it->kind == StorageKind::Meta || it->kind == StorageKind::Raw2352) {
                return read_slice(out, 2352u);
            }

            if (it->kind == StorageKind::Form1) {
                // Mode 2 Form 1: 8-byte XA subheader + 2048 bytes user data
                const bool is_last = (lba + 1u == it->end_lba);
                const std::uint8_t submode = is_last ? 0x89u : 0x08u;  // Data (+ EOR/EOF on final sector)
                out[16] = 0x00; out[17] = 0x00; out[18] = submode; out[19] = 0x00;
                std::memcpy(out + 20, out + 16, 4);
                return read_slice(out + 24, 2048u);
            }
        }
    }

    // Gap / unreferenced padding sector: zero-filled Mode 2 Form 1
    out[16] = 0x00; out[17] = 0x00; out[18] = 0x08; out[19] = 0x00;
    std::memcpy(out + 20, out + 16, 4);
    return true;
}

bool Cdrom::read_sector_user2048(int disc_num, std::uint32_t lba, std::uint8_t out[2048]) const {
    std::uint8_t raw[2352];
    if (!read_sector_raw2352(disc_num, lba, raw)) return false;
    std::memcpy(out, raw + 24, 2048);
    return true;
}

std::uint8_t Cdrom::status_byte() const noexcept {
    std::uint8_t st = 0;
    if (motor_on_ && shell_open_cycles_ == 0) st |= 0x02u;  // Motor On
    if (shell_open_cycles_ > 0 || shell_was_opened_) st |= 0x10u;  // Shell Open
    if (reading_) st |= 0x20u;                              // Reading
    if (seeking_) st |= 0x40u;                              // Seeking
    if (playing_cdda_) st |= 0x80u;                         // Playing CD-DA
    return st;
}

void Cdrom::queue_irq(std::uint8_t type, std::initializer_list<std::uint8_t> resp, std::uint32_t delay_cycles) {
    PendingIrq p;
    p.type = type;
    p.resp.assign(resp.begin(), resp.end());
    p.delay_cycles = delay_cycles;
    irq_queue_.push_back(std::move(p));
}

void Cdrom::update_irq_line(System &sys) {
    if ((irq_flag_ & irq_enable_ & 0x1Fu) != 0) {
        sys.hw.raise_irq(sys, kIrqCdrom);
    }
}

std::uint8_t Cdrom::read_reg(std::uint32_t offset) {
    switch (offset & 3u) {
    case 0: {  // Status register (all indices)
        std::uint8_t st = index_ & 3u;
        if (param_fifo_.empty()) st |= (1u << 3);             // PRMEMPT
        if (param_fifo_.size() < 16u) st |= (1u << 4);        // PRMWRDY
        if (!resp_fifo_.empty()) st |= (1u << 5);             // RSLRRDY
        if (bfrd_enabled_ && data_pos_ < data_buf_.size()) st |= (1u << 6);  // DRQSTS
        return st;
    }
    case 1: {  // Response FIFO (all indices)
        if (resp_fifo_.empty()) return 0;
        const std::uint8_t v = resp_fifo_.front();
        resp_fifo_.pop_front();
        return v;
    }
    case 2: {  // Data FIFO (all indices)
        if (bfrd_enabled_ && data_pos_ < data_buf_.size()) {
            return data_buf_[data_pos_++];
        }
        return 0;
    }
    case 3: {
        if ((index_ & 1u) == 0) {
            return static_cast<std::uint8_t>(irq_enable_ | 0xE0u);  // Index 0/2: Interrupt Enable
        }
        return static_cast<std::uint8_t>(irq_flag_ | 0xE0u);        // Index 1/3: Interrupt Flag
    }
    default:
        return 0;
    }
}

std::uint32_t Cdrom::read_dma32() {
    std::uint32_t w = 0;
    for (int i = 0; i < 4; ++i) {
        std::uint8_t b = 0;
        if (data_pos_ < data_buf_.size()) {
            b = data_buf_[data_pos_++];
        }
        w |= static_cast<std::uint32_t>(b) << (i * 8);
    }
    return w;
}

void Cdrom::write_reg(System &sys, std::uint32_t offset, std::uint8_t val) {
    const std::uint32_t reg = offset & 3u;
    if (reg == 0) {
        index_ = val & 3u;
        return;
    }

    switch (index_) {
    case 0:
        if (reg == 1) {
            execute_command(sys, val);
        } else if (reg == 2) {
            if (param_fifo_.size() < 16u) param_fifo_.push_back(val);
        } else if (reg == 3) {
            // Request register: bit 7 = BFRD (want data)
            const bool want_data = (val & 0x80u) != 0;
            if (want_data && !bfrd_enabled_) {
                bfrd_enabled_ = true;
                data_pos_ = 0;
            } else if (!want_data) {
                bfrd_enabled_ = false;
                data_pos_ = data_buf_.size();
            }
        }
        break;

    case 1:
        if (reg == 2) {
            irq_enable_ = val & 0x1Fu;
            update_irq_line(sys);
        } else if (reg == 3) {
            if (val & 0x40u) param_fifo_.clear();
            irq_flag_ &= static_cast<std::uint8_t>(~(val & 0x1Fu));
            // If another IRQ is waiting and current flag cleared, dispatch promptly on next tick.
        }
        break;

    case 2:
        if (reg == 2) vol_ll_ = val;
        else if (reg == 3) vol_lr_ = val;
        break;

    case 3:
        if (reg == 1) vol_rr_ = val;
        else if (reg == 2) vol_rl_ = val;
        break;
    }
}

void Cdrom::execute_command(System &sys, std::uint8_t cmd) {
    auto pop_param = [&]() -> std::uint8_t {
        if (param_fifo_.empty()) return 0;
        const std::uint8_t v = param_fifo_.front();
        param_fifo_.pop_front();
        return v;
    };

    const std::uint8_t st = status_byte();

    switch (cmd) {
    case 0x01:  // Getstat
        shell_was_opened_ = false;
        queue_irq(3, {st}, kCmdAckDelay);
        break;

    case 0x02: {  // Setloc (amm, ass, asect)
        const std::uint8_t m = pop_param();
        const std::uint8_t s = pop_param();
        const std::uint8_t f = pop_param();
        setloc_lba_ = msf_to_lba(m, s, f);
        setloc_pending_ = true;
        queue_irq(3, {st}, kCmdAckDelay);
        break;
    }

    case 0x03:  // Play (CD-DA)
        if (!param_fifo_.empty()) pop_param();
        if (setloc_pending_) { cur_lba_ = setloc_lba_; setloc_pending_ = false; }
        playing_cdda_ = true;
        reading_ = false;
        seeking_ = false;
        queue_irq(3, {status_byte()}, kCmdAckDelay);
        break;

    case 0x04:  // Forward
    case 0x05:  // Backward
        queue_irq(3, {st}, kCmdAckDelay);
        break;

    case 0x06:  // ReadN
    case 0x1B:  // ReadS
        if (setloc_pending_) { cur_lba_ = setloc_lba_; setloc_pending_ = false; }
        motor_on_ = true;
        reading_ = true;
        seeking_ = false;
        playing_cdda_ = false;
        read_timer_ = (mode_ & 0x80u) ? kSectorCycles2x : kSectorCycles1x;
        queue_irq(3, {status_byte()}, kCmdAckDelay);
        break;

    case 0x07:  // MotorOn
        motor_on_ = true;
        queue_irq(3, {st}, kCmdAckDelay);
        queue_irq(2, {status_byte()}, kSeekCompleteDelay);
        break;

    case 0x08:  // Stop
        reading_ = false;
        seeking_ = false;
        playing_cdda_ = false;
        queue_irq(3, {st}, kCmdAckDelay);
        motor_on_ = false;
        queue_irq(2, {status_byte()}, kSeekCompleteDelay);
        break;

    case 0x09:  // Pause
        queue_irq(3, {st}, kCmdAckDelay);
        reading_ = false;
        seeking_ = false;
        playing_cdda_ = false;
        queue_irq(2, {status_byte()}, kSeekCompleteDelay);
        // If an automatic Koudelka disc swap was queued on completion of DISK/DISK<N>.TX8, execute it now!
        if (pending_swap_disc_ > 0 && pending_swap_disc_ != active_disc_ && shell_open_cycles_ == 0) {
            swap_disc(pending_swap_disc_);
        }
        break;

    case 0x0A:  // Init
        reading_ = false;
        seeking_ = false;
        playing_cdda_ = false;
        mode_ = 0;
        motor_on_ = true;
        queue_irq(3, {st}, kCmdAckDelay);
        queue_irq(2, {status_byte()}, kSeekCompleteDelay);
        break;

    case 0x0B:  // Mute
        muted_ = true;
        queue_irq(3, {st}, kCmdAckDelay);
        break;

    case 0x0C:  // Demute
        muted_ = false;
        queue_irq(3, {st}, kCmdAckDelay);
        break;

    case 0x0D:  // Setfilter (file, channel)
        filter_file_ = pop_param();
        filter_channel_ = pop_param();
        queue_irq(3, {st}, kCmdAckDelay);
        break;

    case 0x0E:  // Setmode (mode)
        mode_ = pop_param();
        queue_irq(3, {st}, kCmdAckDelay);
        break;

    case 0x0F:  // Getparam
        queue_irq(3, {st, mode_, 0x00u, filter_file_, filter_channel_}, kCmdAckDelay);
        break;

    case 0x10:  // GetlocL (amm, ass, asect, mode, file, channel, sm, ci)
        queue_irq(3, {last_loc_l_[0], last_loc_l_[1], last_loc_l_[2], last_loc_l_[3],
                      last_loc_l_[4], last_loc_l_[5], last_loc_l_[6], last_loc_l_[7]}, kCmdAckDelay);
        break;

    case 0x11: {  // GetlocP (track, index, mm, ss, ff, amm, ass, aff)
        std::uint8_t am = 0, as = 0, af = 0;
        lba_to_msf(cur_lba_, am, as, af);
        std::uint8_t rm = 0, rs = 0, rf = 0;
        const std::uint32_t rel = cur_lba_;
        rm = to_bcd(rel / (60u * 75u));
        rs = to_bcd((rel % (60u * 75u)) / 75u);
        rf = to_bcd(rel % 75u);
        queue_irq(3, {0x01u, 0x01u, rm, rs, rf, am, as, af}, kCmdAckDelay);
        break;
    }

    case 0x12:  // SetSession
        pop_param();
        queue_irq(3, {st}, kCmdAckDelay);
        queue_irq(2, {status_byte()}, kSeekCompleteDelay);
        break;

    case 0x13:  // GetTN (stat, first_track=1, last_track=1)
        queue_irq(3, {st, 0x01u, 0x01u}, kCmdAckDelay);
        break;

    case 0x14: {  // GetTD (track: 0=end of disc, 1=track 1 start 00:02:00)
        const std::uint8_t track = pop_param();
        if (track == 0) {
            std::uint8_t m = 0, s = 0, f = 0;
            lba_to_msf(disc_total_sectors(active_disc_), m, s, f);
            queue_irq(3, {st, m, s}, kCmdAckDelay);
        } else {
            queue_irq(3, {st, 0x00u, 0x02u}, kCmdAckDelay);
        }
        break;
    }

    case 0x15:  // SeekL
    case 0x16:  // SeekP
        if (setloc_pending_) { cur_lba_ = setloc_lba_; setloc_pending_ = false; }
        reading_ = false;
        playing_cdda_ = false;
        seeking_ = true;
        queue_irq(3, {status_byte()}, kCmdAckDelay);
        seeking_ = false;
        queue_irq(2, {status_byte()}, kSeekCompleteDelay);
        break;

    case 0x19: {  // Test
        const std::uint8_t sub = pop_param();
        if (sub == 0x20u) {
            // Standard NTSC-U SCPH-1001/5501 CD-ROM BIOS version: 95/05/16, C1
            queue_irq(3, {0x95u, 0x05u, 0x16u, 0xC1u}, kCmdAckDelay);
        } else {
            queue_irq(3, {st}, kCmdAckDelay);
        }
        break;
    }

    case 0x1A:  // GetID
        queue_irq(3, {st}, kCmdAckDelay);
        if (shell_open_cycles_ > 0 || !find_disc(active_disc_)) {
            queue_irq(5, {0x11u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}, kSeekCompleteDelay);
        } else {
            // Licensed NTSC-U Mode 2 Data disc: "SCEA"
            queue_irq(2, {0x02u, 0x00u, 0x20u, 0x00u, 'S', 'C', 'E', 'A'}, kSeekCompleteDelay);
        }
        break;

    case 0x1C:  // Init / Reset
    case 0x1E:  // ReadTOC
        queue_irq(3, {st}, kCmdAckDelay);
        queue_irq(2, {status_byte()}, kSeekCompleteDelay);
        break;

    default:
        queue_irq(5, {static_cast<std::uint8_t>(st | 0x01u), 0x40u}, kCmdAckDelay);
        break;
    }

    param_fifo_.clear();
    update_irq_line(sys);
}

void Cdrom::deliver_sector(System &sys, std::uint32_t lba) {
    if (!read_sector_raw2352(active_disc_, lba, last_raw_sector_.data())) {
        reading_ = false;
        queue_irq(5, {static_cast<std::uint8_t>(status_byte() | 0x01u), 0x80u}, 1000u);
        return;
    }

    // Automatic Koudelka 4-disc swap detection:
    // When the game finishes reading DISK/DISK<1..4>.TX8 ("Insert Disc N" prompt texture),
    // automatically queue a seamless swap to Disc N!
    if (const DiscImage *disc = find_disc(active_disc_)) {
        auto it = std::upper_bound(disc->entries.begin(), disc->entries.end(), lba,
                                   [](std::uint32_t target, const Entry &e) { return target < e.start_lba; });
        if (it != disc->entries.begin()) {
            --it;
            if (lba + 1u == it->end_lba && it->iso_path.size() == 14 &&
                it->iso_path.rfind("DISK/DISK", 0) == 0 && it->iso_path.substr(10) == ".TX8") {
                const int target_disc = it->iso_path[9] - '0';
                if (target_disc >= 1 && target_disc <= 4 && target_disc != active_disc_) {
                    pending_swap_disc_ = target_disc;
                }
            }
        }
    }

    // Populate GetlocL (header bytes 12..15 + subheader bytes 16..19)
    for (int i = 0; i < 8; ++i) last_loc_l_[i] = last_raw_sector_[12 + i];

    const std::uint8_t file = last_raw_sector_[16];
    const std::uint8_t channel = last_raw_sector_[17];
    const std::uint8_t submode = last_raw_sector_[18];
    const bool is_audio_rt = (submode & 0x44u) == 0x44u;  // Bit 6 RealTime + Bit 2 Audio

    // XA-ADPCM audio routing: if Setmode bit 6 is set and sector is RealTime Audio
    if ((mode_ & 0x40u) && is_audio_rt) {
        const bool filter_ok = !(mode_ & 0x08u) || (file == filter_file_ && channel == filter_channel_);
        if (filter_ok && !muted_ && xa_cb_) {
            xa_cb_(last_raw_sector_.data(), file, channel);
        }
        return;  // XA audio sectors are consumed by the XA decoder and do NOT trigger INT1 data IRQ!
    }

    // If XA filter is enabled on non-audio sectors, skip mismatched file/channel
    if ((mode_ & 0x08u) && (submode & 0x40u)) {
        if (file != filter_file_ || channel != filter_channel_) {
            return;
        }
    }

    // Copy either 2340 bytes (whole sector minus sync, mode bit 5) or 2048 user bytes (default)
    if (mode_ & 0x20u) {
        data_buf_.assign(last_raw_sector_.begin() + 12, last_raw_sector_.begin() + 2352);
    } else {
        data_buf_.assign(last_raw_sector_.begin() + 24, last_raw_sector_.begin() + 24 + 2048);
    }
    data_pos_ = 0;

    // Fire INT1 (Data Ready)
    if (irq_flag_ == 0) {
        resp_fifo_.clear();
        resp_fifo_.push_back(status_byte());
        irq_flag_ = 1u;
        update_irq_line(sys);
    } else {
        PendingIrq p;
        p.type = 1u;
        p.resp = {status_byte()};
        p.delay_cycles = 0;
        irq_queue_.push_back(std::move(p));
    }
}

void Cdrom::tick(System &sys, std::uint32_t cycles) {
    // Handle disc-swap lid open timer
    if (shell_open_cycles_ > 0) {
        if (cycles >= shell_open_cycles_) {
            shell_open_cycles_ = 0;
            if (pending_swap_disc_ > 0) {
                active_disc_ = pending_swap_disc_;
                pending_swap_disc_ = 0;
            }
            motor_on_ = true;
        } else {
            shell_open_cycles_ -= cycles;
        }
    }

    // Advance pending command IRQs (only pop when previous irq_flag_ has been acknowledged)
    if (!irq_queue_.empty()) {
        auto &front = irq_queue_.front();
        if (front.delay_cycles > cycles) {
            front.delay_cycles -= cycles;
        } else {
            front.delay_cycles = 0;
            if (irq_flag_ == 0) {
                resp_fifo_.clear();
                for (std::uint8_t b : front.resp) resp_fifo_.push_back(b);
                irq_flag_ = front.type;
                irq_queue_.pop_front();
                update_irq_line(sys);
            }
        }
    }

    // Advance active ReadN / ReadS streaming
    if (reading_ && shell_open_cycles_ == 0) {
        if (read_timer_ > cycles) {
            read_timer_ -= cycles;
        } else {
            const std::uint32_t period = (mode_ & 0x80u) ? kSectorCycles2x : kSectorCycles1x;
            read_timer_ = period;
            const std::uint32_t lba = cur_lba_++;
            deliver_sector(sys, lba);
        }
    }
}

}  // namespace psx
