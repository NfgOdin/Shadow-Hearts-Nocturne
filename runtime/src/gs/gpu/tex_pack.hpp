#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ps2::gs::gpu {

struct TexPackKey {
    std::uint64_t tex_hash = 0;
    std::uint64_t clut_hash = 0;

    bool operator==(const TexPackKey &o) const { return tex_hash == o.tex_hash && clut_hash == o.clut_hash; }
};

struct TexPackKeyHash {
    std::size_t operator()(const TexPackKey &k) const noexcept {
        std::uint64_t h = k.tex_hash ^ (k.clut_hash + 0x9E3779B97F4A7C15ull + (k.tex_hash << 6) + (k.tex_hash >> 2));
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
        return static_cast<std::size_t>(h ^ (h >> 27));
    }
};

struct TexPackStats {
    std::size_t indexed_textures = 0;
    std::size_t active_packs = 0;
    std::size_t resident_textures = 0;
    std::size_t pending_decodes = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t pool_capacity_bytes = 0;
    std::size_t dumped_textures = 0;
    bool overrides_enabled = false;
    bool dump_enabled = false;
};

struct ReadyTexUpload {
    TexPackKey key{};
    std::uint32_t gpu_offset_words = 0;  // relative to start of override region
    std::uint32_t hd_shift = 0;          // 0=1x, 1=2x, 2=4x, 3=8x
    std::uint32_t orig_w = 0;
    std::uint32_t orig_h = 0;
    std::uint32_t png_w = 0;
    std::uint32_t png_h = 0;
    std::vector<std::uint32_t> words;    // PS2-alpha RGBA32 texels
};

// Deterministic 64-bit content hashes shared by runtime loader, runtime dumper, and offline extractors.
std::uint64_t hash_tex_bytes(const std::uint8_t *p, std::size_t n);
std::uint64_t hash_clut_rgba(const std::uint32_t *rgba, std::size_t count);

// Parse a pack filename stem into a TexPackKey:
//   "<16hex>_<16hex>[_anything].png" -> {tex_hash, clut_hash}, has_clut = true
//   "<16hex>[_anything].png"         -> {tex_hash, 0},         has_clut = false
bool parse_tex_filename(const std::string &filename, TexPackKey &out_key, bool &has_clut);

// Format canonical dump filename: "<16hex>_<16hex>_<w>x<h>.png" (or "<16hex>_0000000000000000_<w>x<h>.png" for direct color).
std::string format_tex_filename(TexPackKey key, std::uint32_t w, std::uint32_t h);

// Compute a safe power-of-two override pool order (words = 1u << order) given the GPU's storage buffer limit
// and the words already reserved for kCachedWords + kDecodedWords.
std::uint32_t compute_override_pool_order(std::uint64_t max_binding_bytes, std::uint32_t reserved_words);

// Power-of-two buddy + LRU allocator for the GPU override pool (pure CPU logic, directly unit-testable).
class OverrideBuddyPool {
public:
    void init(std::uint32_t max_order);
    void reset();

    std::uint32_t capacity_words() const { return max_order_ ? (1u << max_order_) : 0u; }
    std::uint32_t used_words() const { return used_words_; }
    std::size_t resident_count() const { return resident_.size(); }

    // Allocate 2^order words for `key`, evicting oldest entries with last_used_submit < current_submit if needed.
    // Pinning by submit serial guarantees no HD texture used in an unsubmitted batch can be overwritten mid-frame.
    bool allocate(TexPackKey key, std::uint32_t order, std::uint64_t current_submit,
                  std::uint32_t &out_offset, std::vector<TexPackKey> &out_evicted);

    void touch(TexPackKey key, std::uint64_t current_submit);
    void release(TexPackKey key);

private:
    struct Block {
        std::uint32_t offset = 0;
        std::uint32_t order = 0;
        std::uint64_t last_used_batch = 0;
    };

    bool take(std::uint32_t order, std::uint32_t &out_offset);
    void give_back(std::uint32_t offset, std::uint32_t order);

    std::uint32_t max_order_ = 0;
    std::uint32_t used_words_ = 0;
    std::vector<std::vector<std::uint32_t>> free_lists_;
    std::unordered_map<TexPackKey, Block, TexPackKeyHash> resident_;
};

// High-level HD Texture Pack Manager:
// - Folder-based mod pack scanner (mods/textures/<pack>/) + legacy textures_index.tsv
// - Async background PNG decoding worker pool (zero render-thread hitches)
// - Dynamic buddy + LRU GPU allocation tracking
// - Smart async runtime PNG dumper (dumps/textures/{ui_2d,world_3d}/)
class TexPackManager {
public:
    TexPackManager();
    ~TexPackManager();
    TexPackManager(const TexPackManager &) = delete;
    TexPackManager &operator=(const TexPackManager &) = delete;

    // Configure pool size and root directories (mods_dir defaults to "mods/textures", dump_dir defaults to "dumps/textures").
    void configure_pool(std::uint32_t pool_order, std::uint32_t base_offset_words);
    void set_directories(const std::filesystem::path &mods_textures_dir, const std::filesystem::path &dumps_textures_dir);
    void set_pack_order(const std::vector<std::string> &pack_order_spec);
    void set_overrides_enabled(bool on);
    void set_dump_enabled(bool on);
    void set_legacy_index_override(const std::string &env_opt, bool override_all, bool hot_reload);

    // Scan mods/textures/<pack>/ folders + legacy textures_index.tsv and reset GPU residency.
    void reload_packs();

    // Returns true if any override entries are indexed and overrides are enabled.
    bool overrides_active() const;
    bool dump_active() const;

    // Query whether a texture key has an available HD override.
    // - If already GPU-resident: touches LRU, returns packed `dec` word ((base_offset_words + pool_off) | (hd_shift << 28)).
    // - If indexed on disk but not resident: enqueues async PNG decode and returns 0xFFFFFFFFu (zero hitch).
    // - If not indexed or incompatible: returns 0xFFFFFFFFu.
    std::uint32_t resolve_or_enqueue(TexPackKey key, std::uint32_t orig_w, std::uint32_t orig_h, std::uint64_t current_batch);

    // Drain completed async PNG decodes, allocate buddy+LRU slots, and invoke `upload_fn(ReadyTexUpload)`
    // so the GPU layer can call wgpuQueueWriteBuffer before the next command submission.
    // Returns number of textures uploaded.
    std::size_t drain_ready_uploads(std::uint64_t current_batch, const std::function<void(const ReadyTexUpload &)> &upload_fn);

    // Optional hot-reload check (legacy textures_index.tsv or modified PNGs, called periodically at frame end).
    void poll_hot_reload();

    // Fast O(1) pre-check before CPU-unswizzling a texture for dumping (avoids per-frame decode waste).
    bool should_dump(TexPackKey key) const;

    // Enqueue a texture for smart async PNG dumping if not already dumped.
    // `is_ui_2d` classifies into ui_2d/ vs world_3d/.
    bool enqueue_dump(TexPackKey key, std::uint32_t w, std::uint32_t h, bool is_ui_2d, std::vector<std::uint32_t> &&ps2_rgba);

    // Wait for all pending decode/dump background tasks (used by tests and clean shutdown).
    void wait_idle();

    TexPackStats stats() const;

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

}  // namespace ps2::gs::gpu
