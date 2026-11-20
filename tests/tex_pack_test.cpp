#include "fmv_player.hpp"
#include "gs/gpu/tex_pack.hpp"
#include "ipu/ipu.hpp"
#include "ps2/png_codec.hpp"
#include "stretch.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, const char *desc) {
    ++g_checks;
    if (!cond) {
        ++g_failed;
        std::fprintf(stderr, "FAIL: %s\n", desc);
    }
}

std::vector<std::uint32_t> make_pattern(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t r = (x * 7 + seed) & 0xFFu;
            const std::uint32_t g = (y * 11 + seed * 3) & 0xFFu;
            const std::uint32_t b = ((x ^ y) * 5 + seed) & 0xFFu;
            const std::uint32_t a = ((x + y) & 1u) ? 255u : 128u;
            px[static_cast<std::size_t>(y) * w + x] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }
    return px;
}

}  // namespace

int main() {
    using namespace ps2::gs::gpu;

    const std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "sh2_tex_pack_test_tmp";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    std::filesystem::create_directories(tmp_root, ec);

    // 1. PNG codec round-trip (standard RGBA and PS2 alpha conversion)
    {
        const auto src = make_pattern(32, 16, 42);
        const auto png_path = tmp_root / "codec_rt.png";
        check(ps2::png::encode_rgba(png_path, 32, 16, src.data(), /*from_ps2_alpha=*/false), "png encode_rgba standard");
        std::uint32_t rw = 0, rh = 0;
        std::vector<std::uint32_t> dec;
        check(ps2::png::decode_rgba(png_path, rw, rh, dec, /*ps2_alpha=*/false), "png decode_rgba standard");
        check(rw == 32 && rh == 16 && dec == src, "png standard round-trip exact pixel match");

        std::vector<std::uint32_t> dec_ps2;
        check(ps2::png::decode_rgba(png_path, rw, rh, dec_ps2, /*ps2_alpha=*/true), "png decode_rgba ps2_alpha");
        check((dec_ps2[1] >> 24) == 128u && (dec_ps2[0] >> 24) == 64u, "png ps2_alpha maps 255->128 and 128->64");
    }

    // 2. CLUT-aware hashing (identical pixels + different palettes -> distinct keys)
    {
        std::vector<std::uint8_t> raw(256);
        for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>((i * 37 + 11) & 0xFFu);
        const std::uint64_t th = hash_tex_bytes(raw.data(), raw.size());
        check(th == 0x6acbf570b72e78cfull, "hash_tex_bytes bit-exact parity with scripts/sh2_extract_pkb.py");

        std::uint32_t clut_a[16], clut_b[16];
        for (int i = 0; i < 16; ++i) {
            clut_a[i] = 0x80112233u + static_cast<std::uint32_t>(i);
            clut_b[i] = clut_a[i] + (i == 5 ? 1u : 0u);
        }
        const std::uint64_t ch_a = hash_clut_rgba(clut_a, 16);
        const std::uint64_t ch_b = hash_clut_rgba(clut_b, 16);
        check(ch_a == 0xf2f4b2eb17c8bf96ull && ch_b != 0 && ch_a != ch_b, "hash_clut_rgba bit-exact parity & distinguishes palette swaps");

        TexPackKey ka{th, ch_a}, kb{th, ch_b};
        check(!(ka == kb), "TexPackKey separates identical pixels with different CLUTs");

        // Filename parsing
        const std::string fname = format_tex_filename(ka, 64, 64);
        TexPackKey parsed{};
        bool has_clut = false;
        check(parse_tex_filename(fname, parsed, has_clut) && parsed == ka && has_clut, "parse_tex_filename parses canonical <tex>_<clut>_<w>x<h>.png");

        TexPackKey parsed_legacy{};
        bool has_clut2 = true;
        check(parse_tex_filename("0123456789abcdef_icon.png", parsed_legacy, has_clut2) &&
              parsed_legacy.tex_hash == 0x0123456789abcdefull && parsed_legacy.clut_hash == 0 && !has_clut2,
              "parse_tex_filename parses palette-agnostic <tex>_suffix.png");
        check(!parse_tex_filename("not_a_hash.png", parsed, has_clut), "parse_tex_filename rejects non-hex names");
    }

    // 3. Dynamic pool sizing across GPU binding limits
    {
        const std::uint32_t reserved = (8u << 20) + (4u << 20);  // 48 MiB
        check(compute_override_pool_order(128ull << 20, reserved) == 24u, "128 MiB GPU gets 64 MiB override pool (order 24)");
        check(compute_override_pool_order(256ull << 20, reserved) == 25u, "256 MiB GPU gets 128 MiB override pool (order 25)");
        check(compute_override_pool_order(512ull << 20, reserved) == 26u, "512 MiB GPU gets 256 MiB override pool (order 26)");
        check(compute_override_pool_order(1024ull << 20, reserved) == 27u, "1 GiB GPU gets 512 MiB override pool (order 27)");
        check(compute_override_pool_order(4096ull << 20, reserved) == 27u, "4 GiB GPU caps safely at 512 MiB (within 28-bit dec_base)");
    }

    // 4. OverrideBuddyPool splitting, coalescing, LRU eviction & active-batch pinning
    {
        OverrideBuddyPool pool;
        pool.init(10);  // 1024 words total
        std::uint32_t off1 = 0, off2 = 0, off3 = 0;
        std::vector<TexPackKey> evicted;
        check(pool.allocate({1, 0}, 9, 1, off1, evicted) && off1 == 0 && evicted.empty(), "buddy alloc 512 words at 0");
        check(pool.allocate({2, 0}, 9, 2, off2, evicted) && off2 == 512 && evicted.empty(), "buddy alloc 512 words at 512");
        // Both pinned in batch 2 if we touch key 1 at batch 2 -> allocating in batch 2 must fail cleanly
        pool.touch({1, 0}, 2);
        check(!pool.allocate({3, 0}, 9, 2, off3, evicted), "buddy refuses to evict blocks pinned in current batch");
        // In batch 3, key 2 is touched -> key 1 is oldest and gets evicted
        pool.touch({2, 0}, 3);
        check(pool.allocate({3, 0}, 9, 3, off3, evicted) && evicted.size() == 1 && evicted[0] == TexPackKey{1, 0},
              "buddy LRU evicts oldest unused block (key 1)");
        // In batch 4, allocating full 1024 words evicts BOTH key 2 and key 3 and coalesces buddies back to order 10!
        check(pool.allocate({4, 0}, 10, 4, off1, evicted) && off1 == 0 && evicted.size() == 2 && pool.used_words() == 1024,
              "buddy coalesces freed halves into full order-10 block");
    }

    // 5. TexPackManager folder scanning, priority layering, disabled packs, async decode & async dump
    {
        const auto mods_dir = tmp_root / "mods" / "textures";
        const auto dumps_dir = tmp_root / "dumps" / "textures";
        const auto pack_hi = mods_dir / "PackHigh" / "ui";
        const auto pack_lo = mods_dir / "PackLow";
        const auto pack_off = mods_dir / "PackDisabled";

        const TexPackKey k_shared{0x1111222233334444ull, 0x5555666677778888ull};
        const TexPackKey k_fallback{0xaaaa0000bbbb1111ull, 0x9999888877776666ull};
        const TexPackKey k_disabled{0xdeadbeefcafebabeull, 0ull};

        // PackHigh provides 4x (64x64) for k_shared; PackLow provides 2x (32x32) for k_shared + 2x fallback for k_fallback
        const auto img_hi = make_pattern(64, 64, 100);
        const auto img_lo = make_pattern(32, 32, 200);
        const auto img_fb = make_pattern(32, 32, 55);
        const auto img_dis = make_pattern(32, 32, 77);

        check(ps2::png::encode_rgba(pack_hi / format_tex_filename(k_shared, 16, 16), 64, 64, img_hi.data(), false), "write PackHigh PNG");
        check(ps2::png::encode_rgba(pack_lo / format_tex_filename(k_shared, 16, 16), 32, 32, img_lo.data(), false), "write PackLow PNG");
        check(ps2::png::encode_rgba(pack_lo / "aaaa0000bbbb1111.png", 32, 32, img_fb.data(), false), "write fallback PNG");
        check(ps2::png::encode_rgba(pack_off / "deadbeefcafebabe.png", 32, 32, img_dis.data(), false), "write disabled PNG");

        TexPackManager mgr;
        mgr.configure_pool(18, 12u << 20);  // 256K words (1 MiB) pool
        mgr.set_directories(mods_dir, dumps_dir);
        mgr.set_pack_order({"PackHigh", "!PackDisabled", "PackLow"});
        mgr.set_overrides_enabled(true);
        mgr.set_dump_enabled(true);
        mgr.reload_packs();

        const auto st0 = mgr.stats();
        check(st0.active_packs == 2 && st0.indexed_textures == 2, "scanner loads PackHigh + PackLow and skips !PackDisabled");

        // Disabled pack must not resolve
        check(mgr.resolve_or_enqueue(k_disabled, 16, 16, 1) == 0xFFFFFFFFu, "disabled pack texture ignored");

        // First call returns 0xFFFFFFFFu immediately (zero hitch) and queues async decode
        check(mgr.resolve_or_enqueue(k_shared, 16, 16, 1) == 0xFFFFFFFFu, "first resolve is non-blocking async");
        check(mgr.resolve_or_enqueue(k_fallback, 16, 16, 1) == 0xFFFFFFFFu, "fallback resolve is non-blocking async");
        mgr.wait_idle();

        std::size_t uploaded_count = 0;
        std::uint32_t shared_hd_shift = 0;
        std::uint32_t fb_hd_shift = 0;
        mgr.drain_ready_uploads(2, [&](const ReadyTexUpload &up) {
            ++uploaded_count;
            if (up.key == k_shared) shared_hd_shift = up.hd_shift;
            if (up.key == TexPackKey{k_fallback.tex_hash, 0ull}) fb_hd_shift = up.hd_shift;
        });
        check(uploaded_count == 2, "drain_ready_uploads uploaded both decoded textures");
        check(shared_hd_shift == 2u, "PackHigh (4x -> hd_shift=2) took priority over PackLow (2x)");
        check(fb_hd_shift == 1u, "palette-agnostic fallback resolved at 2x (hd_shift=1)");

        const std::uint32_t dec_word = mgr.resolve_or_enqueue(k_shared, 16, 16, 3);
        check(dec_word != 0xFFFFFFFFu && ((dec_word >> 28) & 7u) == 2u, "subsequent resolve returns resident packed dec word");

        // Smart async PNG dump test (ui_2d vs world_3d + should_dump pre-check + dedup)
        auto dump_px1 = make_pattern(16, 16, 1);
        auto dump_px2 = make_pattern(16, 16, 2);
        auto dump_px_dup = make_pattern(16, 16, 1);
        check(mgr.should_dump({0x1001, 0x2002}), "should_dump returns true before first dump");
        check(mgr.enqueue_dump({0x1001, 0x2002}, 16, 16, true, std::move(dump_px1)), "enqueue ui_2d dump");
        check(!mgr.should_dump({0x1001, 0x2002}), "should_dump returns false immediately after enqueue (avoids CPU unswizzle)");
        check(!mgr.enqueue_dump({0x1001, 0x2002}, 16, 16, true, std::move(dump_px_dup)), "duplicate dump deduplicated");
        check(mgr.enqueue_dump({0x3003, 0}, 16, 16, false, std::move(dump_px2)), "enqueue world_3d dump");
        mgr.wait_idle();

        check(std::filesystem::is_regular_file(dumps_dir / "ui_2d" / format_tex_filename({0x1001, 0x2002}, 16, 16)), "ui_2d dump file written");
        check(std::filesystem::is_regular_file(dumps_dir / "world_3d" / format_tex_filename({0x3003, 0}, 16, 16)), "world_3d dump file written");
        check(mgr.stats().dumped_textures == 2, "dumped_textures counter matches 2");
    }

    // 6. IPU MPEG-2 FMV stream observer + .m2v & fmv_index.tsv dumper
    {
        const auto fmv_dumps = tmp_root / "dumps" / "fmv";
        ps2::ipu::set_fmv_dump_enabled(true, fmv_dumps.string().c_str());

        // Synthetic 512x448 @ 29.97 fps MPEG-2 stream (> 16384 bytes) + 3 pictures + sequence end
        std::vector<std::uint8_t> stream;
        const std::uint8_t seq_hdr[8] = {0x00, 0x00, 0x01, 0xB3, 0x20, 0x01, 0xC0, 0x24};
        stream.insert(stream.end(), seq_hdr, seq_hdr + 8);
        for (int i = 0; i < 16600; ++i) stream.push_back(static_cast<std::uint8_t>((i * 13 + 7) & 0xFFu));
        const std::uint8_t pic[4] = {0x00, 0x00, 0x01, 0x00};
        for (int p = 0; p < 3; ++p) {
            stream.insert(stream.end(), pic, pic + 4);
            stream.push_back(0x11);
            stream.push_back(0x22);
        }

        ps2::ipu::observe_mpeg2_bytes(stream.data(), stream.size());
        const auto info = ps2::ipu::fmv_stream_info();
        const std::uint64_t expected_hash = ps2::ipu::hash_mpeg2_prefix(stream.data(), stream.size());
        check(info.active && info.stream_hash == expected_hash && info.stream_hash != 0, "IPU detects & hashes MPEG-2 sequence");
        check(info.width == 512 && info.height == 448 && std::fabs(info.fps - 29.97) < 0.05, "IPU parses 512x448 @ 29.97 fps");
        check(info.frame_index == 3, "IPU counts 3 MPEG-2 picture start codes");

        const std::uint8_t seq_end[4] = {0x00, 0x00, 0x01, 0xB7};
        ps2::ipu::observe_mpeg2_bytes(seq_end, 4);
        check(!ps2::ipu::fmv_stream_info().active, "IPU ends FMV session on 00 00 01 B7");
        check(std::filesystem::is_regular_file(fmv_dumps / "fmv_index.tsv"), "FMV dumper writes fmv_index.tsv");
        char m2v_name[64];
        std::snprintf(m2v_name, sizeof m2v_name, "%016llx.m2v", static_cast<unsigned long long>(expected_hash));
        check(std::filesystem::is_regular_file(fmv_dumps / m2v_name), "FMV dumper writes <hash>.m2v");
        ps2::ipu::set_fmv_dump_enabled(false);
    }

    // 7. Windows Media Foundation backend startup/shutdown & shared_ptr poll_frame safety
    {
        check(fmvplayer::self_test_backend(), "WMF FMV decoder backend initializes & shuts down cleanly");
        ps2::FmvStreamInfo inactive{};
        auto f = fmvplayer::poll_frame(inactive);
        check(!f, "fmvplayer::poll_frame returns empty shared_ptr when no FMV is active");
    }

    // 8. Lock-free SoundPipe seqlock concurrent read/write stress test (audio render vs stats())
    {
        ps2sound::PipeConfig cfg;
        cfg.stretch = true;
        ps2sound::SoundPipe pipe(cfg);
        std::atomic<bool> stop{false};
        std::thread reader([&] {
            std::uint64_t prev_played = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto st = pipe.stats();
                if (st.played < prev_played) ++g_failed;
                prev_played = st.played;
            }
        });
        std::vector<std::int16_t> in_chunk(256 * 2, 1000);
        std::vector<std::int16_t> out_chunk(512 * 2, 0);
        double t = 0.0;
        // Pre-fill 64 ms (~3072 frames) so SoundPipe exits preroll cleanly before rendering
        for (int p = 0; p < 12; ++p) pipe.push(in_chunk.data(), 256, t);
        for (int i = 0; i < 400; ++i) {
            pipe.push(in_chunk.data(), 256, t);
            pipe.push(in_chunk.data(), 256, t + 0.005);
            pipe.frame(t + 0.010);
            pipe.render(out_chunk.data(), 512, t + 0.010);
            t += 1.0 / 60.0;
        }
        stop.store(true, std::memory_order_relaxed);
        reader.join();
        const auto final_st = pipe.stats();
        check(final_st.pushed == (12u + 800u) * 256u && final_st.played > 0u && final_st.played <= 400u * 512u,
              "SoundPipe lock-free seqlock stats match pushed/played frames monotonically without tearing");
    }

    std::filesystem::remove_all(tmp_root, ec);
    std::printf("[tex_pack_test] %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
