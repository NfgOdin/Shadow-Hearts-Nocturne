#include "tex_pack.hpp"

#include "ps2/os.hpp"
#include "ps2/png_codec.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ps2::gs::gpu {

namespace {

inline std::uint64_t mix64_tp(std::uint64_t a, std::uint64_t b) {
    std::uint64_t z = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

bool parse_hex16(const char *s, std::uint64_t &out) {
    std::uint64_t v = 0;
    for (int i = 0; i < 16; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint64_t d = 0;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

bool is_pow2(std::uint32_t v) { return v != 0 && (v & (v - 1u)) == 0; }

std::uint32_t log2_pow2(std::uint32_t v) {
    std::uint32_t r = 0;
    while (v > 1u) {
        v >>= 1;
        ++r;
    }
    return r;
}

std::string to_lower_ascii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::uint64_t hash_tex_bytes(const std::uint8_t *p, std::size_t n) {
    if (!p || n == 0) return 0;
    std::uint64_t h = static_cast<std::uint64_t>(n) * 0x9E3779B97F4A7C15ull;
    const auto mix = [&h](std::uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xBF58476D1CE4E5B9ull;
    };
    const std::size_t step = std::max<std::size_t>(8, (n / 256) & ~static_cast<std::size_t>(7));
    std::uint64_t v;
    for (std::size_t i = 0; i + 8 <= n; i += step) {
        std::memcpy(&v, p + i, 8);
        mix(v);
    }
    v = 0;
    const std::size_t tail = std::min<std::size_t>(n, 8);
    std::memcpy(&v, p + n - tail, tail);
    mix(v);
    return h ? h : 2ull;
}

std::uint64_t hash_clut_rgba(const std::uint32_t *rgba, std::size_t count) {
    if (!rgba || count == 0) return 0;
    std::uint64_t h = mix64_tp(0xC107CAFEull, count);
    for (std::size_t i = 0; i < count; ++i) h = mix64_tp(h, rgba[i]);
    return h ? h : 1ull;
}

bool parse_tex_filename(const std::string &filename, TexPackKey &out_key, bool &has_clut) {
    // Strip directory and verify .png extension
    std::filesystem::path p(filename);
    if (to_lower_ascii(p.extension().string()) != ".png") return false;
    const std::string stem = p.stem().string();
    std::size_t off = 0;
    // Accept both standard "<16hex>[_<16hex>][_<w>x<h>].png" and
    // extractor-prefixed "tex_<16hex>[_clut_<16hex>][_<w>x<h>].png".
    if (stem.size() >= 20 &&
        (stem.compare(0, 4, "tex_") == 0 || stem.compare(0, 4, "TEX_") == 0 ||
         stem.compare(0, 4, "tex-") == 0 || stem.compare(0, 4, "TEX-") == 0)) {
        off = 4;
    }
    if (stem.size() < off + 16) return false;
    std::uint64_t th = 0;
    if (!parse_hex16(stem.c_str() + off, th)) return false;
    out_key.tex_hash = th;
    out_key.clut_hash = 0;
    has_clut = false;
    off += 16;
    if (off == stem.size()) return true;
    if (stem[off] != '_' && stem[off] != '-') return false;
    ++off;
    if (stem.size() >= off + 5 &&
        (stem.compare(off, 5, "clut_") == 0 || stem.compare(off, 5, "CLUT_") == 0 ||
         stem.compare(off, 5, "clut-") == 0 || stem.compare(off, 5, "CLUT-") == 0)) {
        off += 5;
    }
    if (stem.size() >= off + 16) {
        std::uint64_t ch = 0;
        if (parse_hex16(stem.c_str() + off, ch) &&
            (stem.size() == off + 16 || stem[off + 16] == '_' || stem[off + 16] == '-')) {
            out_key.clut_hash = ch;
            has_clut = (ch != 0);
            return true;
        }
    }
    return true;
}

std::string format_tex_filename(TexPackKey key, std::uint32_t w, std::uint32_t h) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%016llx_%016llx_%ux%u.png",
                  static_cast<unsigned long long>(key.tex_hash),
                  static_cast<unsigned long long>(key.clut_hash), w, h);
    return buf;
}

std::uint32_t compute_override_pool_order(std::uint64_t max_binding_bytes, std::uint32_t reserved_words) {
    // Shader packs dec_base in low 28 bits -> max total g_decoded words = 2^28 (1024 MiB).
    constexpr std::uint64_t kMaxTotalWords = 1ull << 28;
    const std::uint64_t hw_words = max_binding_bytes / 4ull;
    const std::uint64_t capped_total = std::min<std::uint64_t>(hw_words, kMaxTotalWords);
    if (capped_total <= reserved_words + (1ull << 20)) return 20;  // 4 MiB minimum fallback
    // Keep headroom so g_decoded stays comfortably within max_binding_bytes (up to 512 MiB = 128 Mi words = order 27).
    const std::uint64_t avail = std::min<std::uint64_t>(capped_total - reserved_words, 1ull << 27);
    std::uint32_t order = 20;
    while (order < 27 && (1ull << (order + 1u)) <= avail) ++order;
    return order;
}

// ---- OverrideBuddyPool ----------------------------------------------------------------------------------------------------

void OverrideBuddyPool::init(std::uint32_t max_order) {
    max_order_ = max_order;
    reset();
}

void OverrideBuddyPool::reset() {
    resident_.clear();
    used_words_ = 0;
    free_lists_.assign(max_order_ + 1u, {});
    if (max_order_ > 0) free_lists_[max_order_].push_back(0u);
}

bool OverrideBuddyPool::take(std::uint32_t order, std::uint32_t &out_offset) {
    if (order > max_order_) return false;
    std::uint32_t k = order;
    while (k <= max_order_ && free_lists_[k].empty()) ++k;
    if (k > max_order_) return false;
    std::uint32_t off = free_lists_[k].back();
    free_lists_[k].pop_back();
    while (k > order) {
        --k;
        free_lists_[k].push_back(off + (1u << k));
    }
    out_offset = off;
    return true;
}

void OverrideBuddyPool::give_back(std::uint32_t offset, std::uint32_t order) {
    while (order < max_order_) {
        auto &list = free_lists_[order];
        const std::uint32_t buddy = offset ^ (1u << order);
        const auto it = std::find(list.begin(), list.end(), buddy);
        if (it == list.end()) break;
        *it = list.back();
        list.pop_back();
        offset &= ~(1u << order);
        ++order;
    }
    free_lists_[order].push_back(offset);
}

bool OverrideBuddyPool::allocate(TexPackKey key, std::uint32_t order, std::uint64_t current_batch,
                                 std::uint32_t &out_offset, std::vector<TexPackKey> &out_evicted) {
    out_evicted.clear();
    if (order > max_order_) return false;
    if (auto it = resident_.find(key); it != resident_.end()) {
        if (it->second.order == order) {
            it->second.last_used_batch = current_batch;
            out_offset = it->second.offset;
            return true;
        }
        give_back(it->second.offset, it->second.order);
        used_words_ -= (1u << it->second.order);
        resident_.erase(it);
    }

    if (take(order, out_offset)) {
        resident_[key] = Block{out_offset, order, current_batch};
        used_words_ += (1u << order);
        return true;
    }

    // Collect eviction candidates not used in the current batch, sorted oldest-first.
    struct Cand {
        TexPackKey key;
        std::uint64_t used;
        std::uint32_t offset;
        std::uint32_t order;
    };
    std::vector<Cand> cands;
    cands.reserve(resident_.size());
    for (const auto &kv : resident_) {
        if (kv.second.last_used_batch < current_batch)
            cands.push_back({kv.first, kv.second.last_used_batch, kv.second.offset, kv.second.order});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.used < b.used; });

    for (const Cand &c : cands) {
        give_back(c.offset, c.order);
        used_words_ -= (1u << c.order);
        resident_.erase(c.key);
        out_evicted.push_back(c.key);
        if (take(order, out_offset)) {
            resident_[key] = Block{out_offset, order, current_batch};
            used_words_ += (1u << order);
            return true;
        }
    }
    return false;
}

void OverrideBuddyPool::touch(TexPackKey key, std::uint64_t current_batch) {
    auto it = resident_.find(key);
    if (it != resident_.end()) it->second.last_used_batch = current_batch;
}

void OverrideBuddyPool::release(TexPackKey key) {
    auto it = resident_.find(key);
    if (it == resident_.end()) return;
    give_back(it->second.offset, it->second.order);
    used_words_ -= (1u << it->second.order);
    resident_.erase(it);
}

// ---- TexPackManager -------------------------------------------------------------------------------------------------------

struct TexPackManager::Impl {
    enum class State { Unloaded, Loading, Resident, Failed };

    struct Entry {
        std::filesystem::path png_path;
        std::uint32_t legacy_orig_w = 0;  // non-zero only for legacy textures_index.tsv entries
        std::uint32_t legacy_orig_h = 0;
        std::uint64_t legacy_mtime_ns = 0;
        std::uint64_t legacy_size = 0;
        bool is_legacy = false;
        bool legacy_modified = true;  // folder packs are always active overrides

        State state = State::Unloaded;
        std::uint32_t cached_dec = 0xFFFFFFFFu;
        std::uint32_t pool_order = 0;
        std::uint64_t disk_mtime_sec = 0;
        std::uint64_t disk_size = 0;
    };

    struct DecodeJob {
        TexPackKey key{};
        std::filesystem::path path;
        std::uint32_t orig_w = 0;
        std::uint32_t orig_h = 0;
        std::uint64_t generation = 0;
    };

    struct DecodedResult {
        TexPackKey key{};
        std::uint32_t orig_w = 0;
        std::uint32_t orig_h = 0;
        std::uint32_t png_w = 0;
        std::uint32_t png_h = 0;
        std::uint32_t hd_shift = 0;
        std::uint64_t generation = 0;
        bool ok = false;
        std::vector<std::uint32_t> words;
    };

    struct DumpJob {
        std::filesystem::path out_path;
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        std::vector<std::uint32_t> rgba;
    };

    std::filesystem::path mods_dir = "mods/textures";
    std::filesystem::path dumps_dir = "dumps/textures";
    std::vector<std::string> pack_order_spec;

    std::string legacy_env_opt;
    bool legacy_override_all = false;
    bool hot_reload = false;

    std::atomic<bool> overrides_enabled{true};
    std::atomic<bool> dump_enabled{false};
    std::atomic<std::size_t> dumped_count{0};

    std::uint32_t base_offset_words = 0;
    OverrideBuddyPool pool;
    std::uint64_t generation = 1;
    std::size_t active_pack_count = 0;

    mutable std::mutex state_mtx;
    std::unordered_map<TexPackKey, Entry, TexPackKeyHash> index;
    std::unordered_set<TexPackKey, TexPackKeyHash> dumped_seen;

    // Async decode thread pool (2 workers)
    std::mutex decode_mtx;
    std::condition_variable decode_cv;
    std::condition_variable decode_idle_cv;
    std::deque<DecodeJob> decode_queue;
    std::deque<DecodedResult> ready_queue;
    std::size_t decode_in_flight = 0;
    bool stop_workers = false;
    std::vector<std::thread> decode_threads;

    // Async dump writer thread (1 worker, bounded queue)
    std::mutex dump_mtx;
    std::condition_variable dump_cv;
    std::condition_variable dump_idle_cv;
    std::deque<DumpJob> dump_queue;
    std::size_t dump_in_flight = 0;
    std::thread dump_thread;

    Impl() {
        for (int i = 0; i < 2; ++i) decode_threads.emplace_back([this] { decode_worker(); });
        dump_thread = std::thread([this] { dump_worker(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk1(decode_mtx);
            std::lock_guard<std::mutex> lk2(dump_mtx);
            stop_workers = true;
        }
        decode_cv.notify_all();
        dump_cv.notify_all();
        for (auto &t : decode_threads)
            if (t.joinable()) t.join();
        if (dump_thread.joinable()) dump_thread.join();
    }

    void decode_worker() {
        for (;;) {
            DecodeJob job;
            {
                std::unique_lock<std::mutex> lk(decode_mtx);
                decode_cv.wait(lk, [this] { return stop_workers || !decode_queue.empty(); });
                if (stop_workers && decode_queue.empty()) return;
                job = std::move(decode_queue.front());
                decode_queue.pop_front();
                ++decode_in_flight;
            }

            DecodedResult res;
            res.key = job.key;
            res.orig_w = job.orig_w;
            res.orig_h = job.orig_h;
            res.generation = job.generation;

            std::vector<std::uint32_t> words;
            std::uint32_t pw = 0, ph = 0;
            if (ps2::png::decode_rgba(job.path, pw, ph, words, /*ps2_alpha=*/true)) {
                std::uint32_t shift = 0;
                bool dim_ok = false;
                if (pw == job.orig_w && ph == job.orig_h) {
                    shift = 0; dim_ok = true;
                } else if (pw == job.orig_w * 2 && ph == job.orig_h * 2) {
                    shift = 1; dim_ok = true;
                } else if (pw == job.orig_w * 4 && ph == job.orig_h * 4) {
                    shift = 2; dim_ok = true;
                } else if (pw == job.orig_w * 8 && ph == job.orig_h * 8) {
                    shift = 3; dim_ok = true;
                }
                if (dim_ok && is_pow2(pw) && is_pow2(ph)) {
                    res.png_w = pw;
                    res.png_h = ph;
                    res.hd_shift = shift;
                    res.words = std::move(words);
                    res.ok = true;
                }
            }

            {
                std::lock_guard<std::mutex> lk(decode_mtx);
                --decode_in_flight;
                ready_queue.push_back(std::move(res));
                if (decode_queue.empty() && decode_in_flight == 0) decode_idle_cv.notify_all();
            }
        }
    }

    void dump_worker() {
        for (;;) {
            DumpJob job;
            {
                std::unique_lock<std::mutex> lk(dump_mtx);
                dump_cv.wait(lk, [this] { return stop_workers || !dump_queue.empty(); });
                if (stop_workers && dump_queue.empty()) return;
                job = std::move(dump_queue.front());
                dump_queue.pop_front();
                ++dump_in_flight;
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(job.out_path, ec)) {
                if (ps2::png::encode_rgba(job.out_path, job.w, job.h, job.rgba.data(), /*from_ps2_alpha=*/true)) {
                    dumped_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
            {
                std::lock_guard<std::mutex> lk(dump_mtx);
                --dump_in_flight;
                if (dump_queue.empty() && dump_in_flight == 0) dump_idle_cv.notify_all();
            }
        }
    }

    static bool stat_file(const std::filesystem::path &p, std::uint64_t &size_out, std::uint64_t &mtime_sec_out, std::uint64_t &mtime_ns_out) {
#if defined(_WIN32)
        struct _stat64 st = {};
        if (::_wstat64(p.c_str(), &st) != 0) return false;
        size_out = static_cast<std::uint64_t>(st.st_size);
        mtime_sec_out = static_cast<std::uint64_t>(st.st_mtime);
        mtime_ns_out = mtime_sec_out * 1000000000ULL;
        return true;
#else
        struct stat st = {};
        if (::stat(p.c_str(), &st) != 0) return false;
        size_out = static_cast<std::uint64_t>(st.st_size);
        mtime_sec_out = static_cast<std::uint64_t>(st.st_mtime);
#if defined(__linux__)
        mtime_ns_out = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
#else
        mtime_ns_out = mtime_sec_out * 1000000000ULL;
#endif
        return true;
#endif
    }

    void index_folder_pack(const std::filesystem::path &pack_root) {
        std::error_code ec;
        if (!std::filesystem::is_directory(pack_root, ec)) return;
        std::vector<std::filesystem::path> pngs;
        for (const auto &it : std::filesystem::recursive_directory_iterator(pack_root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (!it.is_regular_file(ec)) continue;
            if (to_lower_ascii(it.path().extension().string()) == ".png") pngs.push_back(it.path());
        }
        std::sort(pngs.begin(), pngs.end());
        for (const auto &p : pngs) {
            TexPackKey key{};
            bool has_clut = false;
            if (!parse_tex_filename(p.filename().string(), key, has_clut)) continue;
            // Higher-priority packs are scanned first: only insert if not already provided.
            if (index.find(key) != index.end()) continue;
            Entry e;
            e.png_path = p;
            e.is_legacy = false;
            e.legacy_modified = true;
            std::uint64_t mns = 0;
            stat_file(p, e.disk_size, e.disk_mtime_sec, mns);
            index.emplace(key, std::move(e));
        }
    }

    void index_legacy_tsv(const std::filesystem::path &tsv_path) {
        std::ifstream f(tsv_path);
        if (!f) return;
        const std::filesystem::path base_dir = tsv_path.parent_path();
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            unsigned long long key = 0, mtime_ns = 0, fsize = 0;
            unsigned int w = 0, h = 0;
            char rel_buf[1024] = {};
            if (std::sscanf(line.c_str(), "%llx\t%u\t%u\t%llu\t%llu\t%1023[^\r\n]", &key, &w, &h, &mtime_ns, &fsize, rel_buf) == 6) {
                const TexPackKey k{static_cast<std::uint64_t>(key), 0ull};
                if (index.find(k) != index.end()) continue;
                const std::filesystem::path full = base_dir / os::path(rel_buf);
                std::uint64_t cur_sz = 0, cur_ms = 0, cur_mns = 0;
                if (!stat_file(full, cur_sz, cur_ms, cur_mns)) continue;
                const bool mod = legacy_override_all || cur_sz != fsize || (cur_ms != (mtime_ns / 1000000000ULL));
                Entry e;
                e.png_path = full;
                e.legacy_orig_w = w;
                e.legacy_orig_h = h;
                e.legacy_mtime_ns = mtime_ns;
                e.legacy_size = fsize;
                e.is_legacy = true;
                e.legacy_modified = mod;
                e.disk_size = cur_sz;
                e.disk_mtime_sec = cur_ms;
                index.emplace(k, std::move(e));
            }
        }
    }
};

TexPackManager::TexPackManager() : impl_(new Impl()) {}
TexPackManager::~TexPackManager() { delete impl_; }

void TexPackManager::configure_pool(std::uint32_t pool_order, std::uint32_t base_offset_words) {
    std::lock_guard<std::mutex> slk(impl_->state_mtx);
    impl_->base_offset_words = base_offset_words;
    impl_->pool.init(pool_order);
}

void TexPackManager::set_directories(const std::filesystem::path &mods_textures_dir, const std::filesystem::path &dumps_textures_dir) {
    std::lock_guard<std::mutex> slk(impl_->state_mtx);
    impl_->mods_dir = mods_textures_dir;
    impl_->dumps_dir = dumps_textures_dir;
}

void TexPackManager::set_pack_order(const std::vector<std::string> &pack_order_spec) {
    std::lock_guard<std::mutex> slk(impl_->state_mtx);
    impl_->pack_order_spec = pack_order_spec;
}

void TexPackManager::set_overrides_enabled(bool on) { impl_->overrides_enabled.store(on, std::memory_order_relaxed); }
void TexPackManager::set_dump_enabled(bool on) { impl_->dump_enabled.store(on, std::memory_order_relaxed); }

void TexPackManager::set_legacy_index_override(const std::string &env_opt, bool override_all, bool hot_reload) {
    std::lock_guard<std::mutex> slk(impl_->state_mtx);
    impl_->legacy_env_opt = env_opt;
    impl_->legacy_override_all = override_all;
    impl_->hot_reload = hot_reload;
}

void TexPackManager::reload_packs() {
    std::lock_guard<std::mutex> slk(impl_->state_mtx);
    {
        std::lock_guard<std::mutex> lk(impl_->decode_mtx);
        ++impl_->generation;
        impl_->decode_queue.clear();
        impl_->ready_queue.clear();
    }
    impl_->pool.reset();
    impl_->index.clear();
    impl_->active_pack_count = 0;

    // 1. Discover pack folders under dedicated texture_packs/ (primary) and legacy mods/textures/ (fallback)
    std::error_code ec;
    std::vector<std::filesystem::path> roots;
    if (!impl_->mods_dir.empty()) {
        // If caller passed .../mods/textures, also check sibling .../texture_packs first
        if (impl_->mods_dir.filename() == "textures" && impl_->mods_dir.parent_path().filename() == "mods") {
            const std::filesystem::path dedicated = impl_->mods_dir.parent_path().parent_path() / "texture_packs";
            if (std::filesystem::is_directory(dedicated, ec)) roots.push_back(dedicated);
            if (std::filesystem::is_directory(impl_->mods_dir, ec)) roots.push_back(impl_->mods_dir);
        } else {
            if (std::filesystem::is_directory(impl_->mods_dir, ec)) roots.push_back(impl_->mods_dir);
            if (impl_->mods_dir.filename() == "texture_packs") {
                const std::filesystem::path legacy = impl_->mods_dir.parent_path() / "mods" / "textures";
                if (std::filesystem::is_directory(legacy, ec)) roots.push_back(legacy);
            }
        }
    }

    if (!roots.empty()) {
        std::unordered_set<std::string> disabled;
        std::vector<std::string> ordered;
        std::unordered_set<std::string> seen;

        for (const std::string &tok : impl_->pack_order_spec) {
            if (tok.empty()) continue;
            if (tok[0] == '!') {
                if (tok.size() > 1) disabled.insert(tok.substr(1));
            } else if (seen.insert(tok).second) {
                ordered.push_back(tok);
            }
        }

        // Gather all subdirectories across primary texture_packs/ and legacy mods/textures/
        std::unordered_map<std::string, std::filesystem::path> pack_paths;
        std::vector<std::string> discovered;
        for (const auto &root : roots) {
            for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
                if (entry.is_directory(ec)) {
                    const std::string name = entry.path().filename().string();
                    if (pack_paths.find(name) == pack_paths.end()) {
                        pack_paths.emplace(name, entry.path());
                        discovered.push_back(name);
                    }
                }
            }
        }
        std::sort(discovered.begin(), discovered.end());
        for (const std::string &name : discovered) {
            if (!disabled.count(name) && seen.insert(name).second) ordered.push_back(name);
        }

        for (const std::string &pack_name : ordered) {
            if (disabled.count(pack_name)) continue;
            auto pit = pack_paths.find(pack_name);
            if (pit != pack_paths.end() && std::filesystem::is_directory(pit->second, ec)) {
                const std::size_t before = impl_->index.size();
                impl_->index_folder_pack(pit->second);
                if (impl_->index.size() > before) ++impl_->active_pack_count;
            }
        }

        // Also support loose PNGs placed directly in texture_packs/ or mods/textures/ at lowest folder priority
        for (const auto &root : roots) {
            for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
                if (!entry.is_regular_file(ec) || to_lower_ascii(entry.path().extension().string()) != ".png") continue;
                TexPackKey key{};
                bool has_clut = false;
                if (!parse_tex_filename(entry.path().filename().string(), key, has_clut)) continue;
                if (impl_->index.find(key) != impl_->index.end()) continue;
                Impl::Entry e;
                e.png_path = entry.path();
                std::uint64_t mns = 0;
                Impl::stat_file(entry.path(), e.disk_size, e.disk_mtime_sec, mns);
                impl_->index.emplace(key, std::move(e));
            }
        }
    }

    // 2. Legacy textures_index.tsv support (explicit env override or auto-discovered)
    if (!impl_->legacy_env_opt.empty() && impl_->legacy_env_opt != "0") {
        if (impl_->legacy_env_opt != "1") {
            std::filesystem::path p = os::path(impl_->legacy_env_opt);
            if (std::filesystem::is_directory(p, ec)) p /= "textures_index.tsv";
            if (std::filesystem::is_regular_file(p, ec)) impl_->index_legacy_tsv(p);
        } else {
            for (const char *cand : {"assets_unpacked/textures_index.tsv", "mods/textures_index.tsv", "../assets_unpacked/textures_index.tsv"}) {
                const std::filesystem::path p = os::path(cand);
                if (std::filesystem::is_regular_file(p, ec)) {
                    impl_->index_legacy_tsv(p);
                    break;
                }
            }
        }
    }

    if (!impl_->index.empty()) {
        std::fprintf(stderr, "[texpack] indexed %zu HD texture overrides across %zu active pack(s) (pool %u MiB)\n",
                     impl_->index.size(), impl_->active_pack_count,
                     static_cast<unsigned>((static_cast<std::uint64_t>(impl_->pool.capacity_words()) * 4ull) >> 20));
    }
}

bool TexPackManager::overrides_active() const {
    if (!impl_->overrides_enabled.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lk(impl_->state_mtx);
    return !impl_->index.empty() && impl_->pool.capacity_words() > 0;
}

bool TexPackManager::dump_active() const {
    return impl_->dump_enabled.load(std::memory_order_relaxed);
}

std::uint32_t TexPackManager::resolve_or_enqueue(TexPackKey key, std::uint32_t orig_w, std::uint32_t orig_h, std::uint64_t current_submit) {
    if (!impl_->overrides_enabled.load(std::memory_order_relaxed) || key.tex_hash == 0) return 0xFFFFFFFFu;
    std::filesystem::path job_path;
    TexPackKey matched_key = key;
    std::uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> slk(impl_->state_mtx);
        if (impl_->index.empty() || impl_->pool.capacity_words() == 0) return 0xFFFFFFFFu;
        auto it = impl_->index.find(key);
        if (it == impl_->index.end() && key.clut_hash != 0) {
            matched_key = TexPackKey{key.tex_hash, 0ull};
            it = impl_->index.find(matched_key);
        }
        if (it == impl_->index.end()) return 0xFFFFFFFFu;

        Impl::Entry &e = it->second;
        if (e.is_legacy && (!e.legacy_modified || orig_w != e.legacy_orig_w || orig_h != e.legacy_orig_h)) return 0xFFFFFFFFu;
        if (e.state == Impl::State::Resident && e.cached_dec != 0xFFFFFFFFu) {
            impl_->pool.touch(matched_key, current_submit);
            return e.cached_dec;
        }
        if (e.state == Impl::State::Loading || e.state == Impl::State::Failed) return 0xFFFFFFFFu;
        e.state = Impl::State::Loading;
        job_path = e.png_path;
        gen = impl_->generation;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->decode_mtx);
        impl_->decode_queue.push_back({matched_key, std::move(job_path), orig_w, orig_h, gen});
    }
    impl_->decode_cv.notify_one();
    return 0xFFFFFFFFu;
}

std::size_t TexPackManager::drain_ready_uploads(std::uint64_t current_submit, const std::function<void(const ReadyTexUpload &)> &upload_fn) {
    std::deque<Impl::DecodedResult> local;
    {
        std::lock_guard<std::mutex> lk(impl_->decode_mtx);
        if (impl_->ready_queue.empty()) return 0;
        local.swap(impl_->ready_queue);
    }

    std::size_t uploaded = 0;
    std::vector<TexPackKey> evicted;
    for (auto &res : local) {
        ReadyTexUpload up;
        bool do_upload = false;
        {
            std::lock_guard<std::mutex> slk(impl_->state_mtx);
            if (res.generation != impl_->generation) continue;
            auto it = impl_->index.find(res.key);
            if (it == impl_->index.end()) continue;
            Impl::Entry &e = it->second;
            if (!res.ok || res.words.empty()) {
                e.state = Impl::State::Failed;
                continue;
            }
            const std::uint32_t words_n = static_cast<std::uint32_t>(res.words.size());
            if (!is_pow2(words_n)) {
                e.state = Impl::State::Failed;
                continue;
            }
            const std::uint32_t order = log2_pow2(words_n);
            std::uint32_t pool_off = 0;
            evicted.clear();
            if (!impl_->pool.allocate(res.key, order, current_submit, pool_off, evicted)) {
                e.state = Impl::State::Unloaded;
                continue;
            }
            for (const TexPackKey &ev : evicted) {
                auto eit = impl_->index.find(ev);
                if (eit != impl_->index.end()) {
                    eit->second.state = Impl::State::Unloaded;
                    eit->second.cached_dec = 0xFFFFFFFFu;
                }
            }
            up.key = res.key;
            up.gpu_offset_words = pool_off;
            up.hd_shift = res.hd_shift;
            up.orig_w = res.orig_w;
            up.orig_h = res.orig_h;
            up.png_w = res.png_w;
            up.png_h = res.png_h;
            up.words = std::move(res.words);
            e.cached_dec = (impl_->base_offset_words + pool_off) | (res.hd_shift << 28);
            e.pool_order = order;
            e.state = Impl::State::Resident;
            do_upload = true;
        }
        if (do_upload) {
            if (upload_fn) upload_fn(up);
            ++uploaded;
        }
    }
    return uploaded;
}

void TexPackManager::poll_hot_reload() {
    if (!impl_->hot_reload || !overrides_active()) return;
    std::lock_guard<std::mutex> slk(impl_->state_mtx);
    for (auto &kv : impl_->index) {
        Impl::Entry &e = kv.second;
        if (e.state != Impl::State::Resident && !e.is_legacy) continue;
        std::uint64_t sz = 0, ms = 0, mns = 0;
        if (!Impl::stat_file(e.png_path, sz, ms, mns)) continue;
        if (sz != e.disk_size || ms != e.disk_mtime_sec) {
            e.disk_size = sz;
            e.disk_mtime_sec = ms;
            if (e.is_legacy) e.legacy_modified = true;
            if (e.state == Impl::State::Resident) impl_->pool.release(kv.first);
            e.cached_dec = 0xFFFFFFFFu;
            e.state = Impl::State::Unloaded;
        }
    }
}

bool TexPackManager::should_dump(TexPackKey key) const {
    if (!dump_active() || key.tex_hash == 0) return false;
    std::lock_guard<std::mutex> slk(impl_->state_mtx);
    return impl_->dumped_seen.find(key) == impl_->dumped_seen.end();
}

bool TexPackManager::enqueue_dump(TexPackKey key, std::uint32_t w, std::uint32_t h, bool is_ui_2d, std::vector<std::uint32_t> &&ps2_rgba) {
    if (!dump_active() || key.tex_hash == 0 || w == 0 || h == 0 || ps2_rgba.size() < static_cast<std::size_t>(w) * h) return false;
    std::filesystem::path out_path;
    {
        std::lock_guard<std::mutex> slk(impl_->state_mtx);
        if (!impl_->dumped_seen.insert(key).second) return false;
        const std::filesystem::path sub = impl_->dumps_dir / (is_ui_2d ? "ui_2d" : "world_3d");
        out_path = sub / format_tex_filename(key, w, h);
    }

    {
        std::lock_guard<std::mutex> lk(impl_->dump_mtx);
        if (impl_->dump_queue.size() >= 64) {
            std::lock_guard<std::mutex> slk(impl_->state_mtx);
            impl_->dumped_seen.erase(key);
            return false;
        }
        impl_->dump_queue.push_back({std::move(out_path), w, h, std::move(ps2_rgba)});
    }
    impl_->dump_cv.notify_one();
    return true;
}

void TexPackManager::wait_idle() {
    {
        std::unique_lock<std::mutex> lk(impl_->decode_mtx);
        impl_->decode_idle_cv.wait(lk, [this] { return impl_->decode_queue.empty() && impl_->decode_in_flight == 0; });
    }
    {
        std::unique_lock<std::mutex> lk(impl_->dump_mtx);
        impl_->dump_idle_cv.wait(lk, [this] { return impl_->dump_queue.empty() && impl_->dump_in_flight == 0; });
    }
}

TexPackStats TexPackManager::stats() const {
    TexPackStats s;
    {
        std::lock_guard<std::mutex> slk(impl_->state_mtx);
        s.indexed_textures = impl_->index.size();
        s.active_packs = impl_->active_pack_count;
        s.resident_textures = impl_->pool.resident_count();
        s.resident_bytes = static_cast<std::uint64_t>(impl_->pool.used_words()) * 4ull;
        s.pool_capacity_bytes = static_cast<std::uint64_t>(impl_->pool.capacity_words()) * 4ull;
    }
    s.dumped_textures = impl_->dumped_count.load(std::memory_order_relaxed);
    s.overrides_enabled = impl_->overrides_enabled.load(std::memory_order_relaxed);
    s.dump_enabled = impl_->dump_enabled.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(impl_->decode_mtx);
        s.pending_decodes = impl_->decode_queue.size() + impl_->decode_in_flight;
    }
    return s;
}

}  // namespace ps2::gs::gpu
