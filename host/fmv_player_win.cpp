#include "fmv_player.hpp"

#ifdef _WIN32

#include "ps2/os.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <propvarutil.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fmvplayer {
namespace {

std::string to_lower_ascii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_hex16_prefix(const std::string &stem, std::uint64_t &out) {
    if (stem.size() < 16) return false;
    if (stem.size() > 16 && stem[16] != '_' && stem[16] != '-') return false;
    std::uint64_t v = 0;
    for (int i = 0; i < 16; ++i) {
        const unsigned char c = static_cast<unsigned char>(stem[i]);
        std::uint64_t d = 0;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return v != 0;
}

// Explicit Media Foundation startup/shutdown (managed by fmvplayer::shutdown(), never during atexit
// static teardown where Windows loader lock or a live MfSession worker thread could deadlock/crash).
std::atomic<bool> g_mf_started{false};
std::mutex g_mf_init_mtx;

bool ensure_mf_started() {
    if (g_mf_started.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lk(g_mf_init_mtx);
    if (g_mf_started.load(std::memory_order_relaxed)) return true;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        g_mf_started.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

void shutdown_mf_if_started() {
    std::lock_guard<std::mutex> lk(g_mf_init_mtx);
    if (g_mf_started.exchange(false, std::memory_order_acq_rel)) {
        MFShutdown();
    }
}

class MfSession {
public:
    MfSession() = default;
    ~MfSession() { stop(); }

    bool start(const std::filesystem::path &mp4_path, std::uint64_t hash, double initial_pts_sec) {
        stop();
        path_ = mp4_path;
        hash_ = hash;
        stop_flag_.store(false, std::memory_order_relaxed);
        eos_.store(false, std::memory_order_relaxed);
        failed_.store(false, std::memory_order_relaxed);
        seek_pending_.store(initial_pts_sec > 0.25, std::memory_order_relaxed);
        seek_target_sec_.store(initial_pts_sec, std::memory_order_relaxed);
        worker_ = std::thread([this] { decode_loop(); });
        return true;
    }

    void stop() {
        stop_flag_.store(true, std::memory_order_relaxed);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
        pool_.clear();
        current_.reset();
        hash_ = 0;
    }

    std::uint64_t hash() const { return hash_; }
    bool failed() const { return failed_.load(std::memory_order_relaxed); }

    std::shared_ptr<const VideoFrame> fetch_for_pts(double target_sec) {
        std::unique_lock<std::mutex> lk(mtx_);
        // If save-state load jumped PTS backward or far forward (> 1.0s), request a Media Foundation seek.
        // Skip forward seeks once EOS is reached so an HD MP4 that ends slightly early doesn't thrash seeks every second.
        const bool past_eos = eos_.load(std::memory_order_relaxed) && current_ && target_sec >= current_->pts_sec;
        if (current_ && !past_eos && std::fabs(target_sec - current_->pts_sec) > 1.0 && !seek_pending_.load(std::memory_order_relaxed)) {
            seek_target_sec_.store(std::max(0.0, target_sec), std::memory_order_relaxed);
            seek_pending_.store(true, std::memory_order_release);
            for (auto &f : queue_) recycle_locked(std::move(f));
            queue_.clear();
            cv_.notify_one();
            return current_;
        }
        while (!queue_.empty()) {
            const bool need_first = !current_;
            if (need_first || queue_.front()->pts_sec <= target_sec + 0.016) {
                if (current_ && current_.use_count() == 1) recycle_locked(std::move(current_));
                current_ = std::move(queue_.front());
                queue_.pop_front();
                cv_.notify_one();
                if (need_first && current_->pts_sec >= target_sec) break;
            } else {
                break;
            }
        }
        return current_;
    }

    void current_dims(std::uint32_t &w, std::uint32_t &h) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (current_) {
            w = current_->width;
            h = current_->height;
        } else {
            w = h = 0;
        }
    }

private:
    void recycle_locked(std::shared_ptr<VideoFrame> &&f) {
        if (f && f.use_count() == 1 && pool_.size() < 6) pool_.push_back(std::move(f));
    }

    std::shared_ptr<VideoFrame> acquire_frame_locked(std::uint32_t w, std::uint32_t h) {
        std::shared_ptr<VideoFrame> f;
        if (!pool_.empty()) {
            f = std::move(pool_.back());
            pool_.pop_back();
        } else {
            f = std::make_shared<VideoFrame>();
        }
        f->width = w;
        f->height = h;
        const std::size_t need = static_cast<std::size_t>(w) * h;
        if (f->rgba.size() != need) f->rgba.resize(need);
        return f;
    }

    void decode_loop() {
        const HRESULT hr_co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool co_init = (hr_co == S_OK || hr_co == S_FALSE);
        const bool mf_init = ensure_mf_started();

        IMFSourceReader *reader = nullptr;
        IMFAttributes *attrs = nullptr;
        bool opened = false;
        if (mf_init) {
            if (SUCCEEDED(MFCreateAttributes(&attrs, 2))) {
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
                attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            }
            const std::wstring wpath = path_.wstring();
            HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs, &reader);
            if (FAILED(hr) && attrs) {
                attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
                hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs, &reader);
            }
            if (attrs) {
                attrs->Release();
                attrs = nullptr;
            }
            if (SUCCEEDED(hr) && reader) {
                reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
                reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

                IMFMediaType *out_type = nullptr;
                if (SUCCEEDED(MFCreateMediaType(&out_type))) {
                    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                    hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out_type);
                    out_type->Release();
                }

                std::uint32_t width = 0, height = 0;
                LONG stride = 0;
                IMFMediaType *cur_type = nullptr;
                if (SUCCEEDED(hr) && SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur_type))) {
                    UINT32 w32 = 0, h32 = 0;
                    if (SUCCEEDED(MFGetAttributeSize(cur_type, MF_MT_FRAME_SIZE, &w32, &h32))) {
                        width = w32;
                        height = h32;
                    }
                    UINT32 raw_stride = 0;
                    if (SUCCEEDED(cur_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
                        stride = static_cast<LONG>(raw_stride);
                    } else {
                        stride = static_cast<LONG>(width * 4u);
                    }
                    cur_type->Release();
                }

                if (SUCCEEDED(hr) && width > 0 && height > 0) {
                    opened = true;
                    std::fprintf(stderr, "[fmv] playing HD replacement %s (%ux%u)\n", ps2::os::utf8(path_).c_str(), width, height);

                    while (!stop_flag_.load(std::memory_order_relaxed)) {
                        if (seek_pending_.exchange(false, std::memory_order_acquire)) {
                            const double sec = std::max(0.0, seek_target_sec_.load(std::memory_order_relaxed));
                            PROPVARIANT pv;
                            PropVariantInit(&pv);
                            pv.vt = VT_I8;
                            pv.hVal.QuadPart = static_cast<LONGLONG>(sec * 1e7);
                            reader->SetCurrentPosition(GUID_NULL, pv);
                            PropVariantClear(&pv);
                            eos_.store(false, std::memory_order_relaxed);
                        }

                        {
                            std::unique_lock<std::mutex> lk(mtx_);
                            cv_.wait(lk, [this] {
                                return stop_flag_.load(std::memory_order_relaxed) ||
                                       seek_pending_.load(std::memory_order_relaxed) ||
                                       queue_.size() < 4;
                            });
                            if (stop_flag_.load(std::memory_order_relaxed)) break;
                            if (seek_pending_.load(std::memory_order_relaxed)) continue;
                        }

                        DWORD stream_idx = 0, flags = 0;
                        LONGLONG ts = 0;
                        IMFSample *sample = nullptr;
                        const HRESULT rhr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream_idx, &flags, &ts, &sample);
                        if (FAILED(rhr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                            if (sample) sample->Release();
                            eos_.store(true, std::memory_order_relaxed);
                            // Wait for either stop or a backward seek (e.g. save-state load)
                            std::unique_lock<std::mutex> lk(mtx_);
                            cv_.wait(lk, [this] { return stop_flag_.load(std::memory_order_relaxed) || seek_pending_.load(std::memory_order_relaxed); });
                            continue;
                        }
                        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                            IMFMediaType *nt = nullptr;
                            if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &nt))) {
                                UINT32 w32 = 0, h32 = 0;
                                if (SUCCEEDED(MFGetAttributeSize(nt, MF_MT_FRAME_SIZE, &w32, &h32))) {
                                    width = w32;
                                    height = h32;
                                }
                                UINT32 rs = 0;
                                stride = SUCCEEDED(nt->GetUINT32(MF_MT_DEFAULT_STRIDE, &rs)) ? static_cast<LONG>(rs) : static_cast<LONG>(width * 4u);
                                nt->Release();
                            }
                        }
                        if (!sample) continue;

                        IMFMediaBuffer *buf = nullptr;
                        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                            BYTE *ptr = nullptr;
                            DWORD max_len = 0, cur_len = 0;
                            if (SUCCEEDED(buf->Lock(&ptr, &max_len, &cur_len)) && ptr) {
                                const std::size_t abs_stride = static_cast<std::size_t>(stride >= 0 ? stride : -stride);
                                if (abs_stride >= static_cast<std::size_t>(width) * 4u && cur_len >= abs_stride * height) {
                                    std::shared_ptr<VideoFrame> f;
                                    {
                                        std::lock_guard<std::mutex> lk(mtx_);
                                        f = acquire_frame_locked(width, height);
                                    }
                                    f->pts_sec = static_cast<double>(ts) * 1e-7;
                                    const bool bottom_up = (stride < 0);
                                    for (std::uint32_t y = 0; y < height; ++y) {
                                        const std::uint32_t sy = bottom_up ? (height - 1u - y) : y;
                                        const BYTE *src_row = ptr + static_cast<std::size_t>(sy) * abs_stride;
                                        std::uint32_t *dst_row = f->rgba.data() + static_cast<std::size_t>(y) * width;
                                        for (std::uint32_t x = 0; x < width; ++x) {
                                            const std::uint32_t b = src_row[x * 4u + 0u];
                                            const std::uint32_t g = src_row[x * 4u + 1u];
                                            const std::uint32_t r = src_row[x * 4u + 2u];
                                            dst_row[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
                                        }
                                    }
                                    std::lock_guard<std::mutex> lk(mtx_);
                                    queue_.push_back(std::move(f));
                                }
                                buf->Unlock();
                            }
                            buf->Release();
                        }
                        sample->Release();
                    }
                }
            }
            if (reader) reader->Release();
        }
        if (!opened) {
            failed_.store(true, std::memory_order_relaxed);
            std::fprintf(stderr, "[fmv] warning: failed to open HD FMV %s via Media Foundation\n", ps2::os::utf8(path_).c_str());
        }
        if (co_init) CoUninitialize();
    }

    std::filesystem::path path_;
    std::uint64_t hash_ = 0;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> eos_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> seek_pending_{false};
    std::atomic<double> seek_target_sec_{0.0};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<VideoFrame>> queue_;
    std::vector<std::shared_ptr<VideoFrame>> pool_;
    std::shared_ptr<VideoFrame> current_;
    std::thread worker_;
};

std::mutex g_mtx;
std::filesystem::path g_mods_dir = "mods/fmv";
std::vector<std::filesystem::path> g_extra_dirs;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_session_active{false};
std::unordered_map<std::uint64_t, std::filesystem::path> g_index;
MfSession g_session;
std::uint64_t g_failed_hash = 0;

void scan_locked() {
    g_index.clear();
    std::error_code ec;
    std::vector<std::filesystem::path> roots;
    // Layer 1c active mod video/ override directories (highest priority, in mod load order)
    for (const auto &ed : g_extra_dirs) {
        if (!ed.empty() && std::filesystem::is_directory(ed, ec)) roots.push_back(ed);
    }
    if (!g_mods_dir.empty()) {
        if (g_mods_dir.filename() == "fmv" && g_mods_dir.parent_path().filename() == "mods") {
            const std::filesystem::path dedicated = g_mods_dir.parent_path().parent_path() / "video_packs";
            if (std::filesystem::is_directory(dedicated, ec)) roots.push_back(dedicated);
            if (std::filesystem::is_directory(g_mods_dir, ec)) roots.push_back(g_mods_dir);
        } else {
            if (std::filesystem::is_directory(g_mods_dir, ec)) roots.push_back(g_mods_dir);
            if (g_mods_dir.filename() == "video_packs") {
                const std::filesystem::path legacy = g_mods_dir.parent_path() / "mods" / "fmv";
                if (std::filesystem::is_directory(legacy, ec)) roots.push_back(legacy);
            }
        }
    }
    for (const auto &root : roots) {
        for (const auto &it : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (!it.is_regular_file(ec)) continue;
            const std::string ext = to_lower_ascii(ps2::os::utf8(it.path().extension()));
            if (ext != ".mp4" && ext != ".m4v" && ext != ".mov") continue;
            std::uint64_t h = 0;
            if (!parse_hex16_prefix(ps2::os::utf8(it.path().stem()), h)) continue;
            if (g_index.find(h) == g_index.end()) g_index.emplace(h, it.path());
        }
    }
    if (!g_index.empty()) {
        std::fprintf(stderr, "[fmv] indexed %zu HD FMV replacement video(s) from %s\n", g_index.size(), ps2::os::utf8(g_mods_dir).c_str());
    }
}

}  // namespace

void configure(const std::filesystem::path &mods_fmv_dir, bool enabled) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_mods_dir = mods_fmv_dir.empty() ? std::filesystem::path("mods/fmv") : mods_fmv_dir;
    g_enabled.store(enabled, std::memory_order_relaxed);
    g_failed_hash = 0;
    scan_locked();
}

void set_extra_dirs(const std::vector<std::filesystem::path> &extra_dirs) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_extra_dirs = extra_dirs;
    g_session.stop();
    g_session_active.store(false, std::memory_order_release);
    g_failed_hash = 0;
    scan_locked();
}

void set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        g_session.stop();
        g_session_active.store(false, std::memory_order_release);
    }
}

bool is_enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void reload() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_session.stop();
    g_session_active.store(false, std::memory_order_release);
    g_failed_hash = 0;
    scan_locked();
}

void shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_session.stop();
        g_session_active.store(false, std::memory_order_release);
    }
    shutdown_mf_if_started();
}

std::shared_ptr<const VideoFrame> poll_frame(const ps2::FmvStreamInfo &info) {
    if (!g_enabled.load(std::memory_order_relaxed) || !info.active || info.stream_hash == 0) {
        if (!g_session_active.load(std::memory_order_acquire)) return nullptr;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_session.hash() != 0) g_session.stop();
        g_session_active.store(false, std::memory_order_release);
        g_failed_hash = 0;
        return nullptr;
    }

    const double fps = info.fps > 1.0 ? info.fps : (30000.0 / 1001.0);
    const double target_sec = static_cast<double>(info.frame_index) / fps;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_session.hash() == info.stream_hash && g_session.failed()) {
            g_failed_hash = info.stream_hash;
            g_session.stop();
            g_session_active.store(false, std::memory_order_release);
            return nullptr;
        }
        if (g_session.hash() != info.stream_hash) {
            if (g_failed_hash == info.stream_hash) return nullptr;
            auto it = g_index.find(info.stream_hash);
            if (it == g_index.end()) {
                g_failed_hash = info.stream_hash;
                if (g_session.hash() != 0) {
                    g_session.stop();
                    g_session_active.store(false, std::memory_order_release);
                }
                return nullptr;
            }
            g_session.start(it->second, info.stream_hash, target_sec);
            g_session_active.store(true, std::memory_order_release);
        }
        return g_session.fetch_for_pts(target_sec);
    }
}

bool self_test_backend() {
    const HRESULT hr_co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_init = (hr_co == S_OK || hr_co == S_FALSE);
    const bool ok = ensure_mf_started();
    if (co_init) CoUninitialize();
    return ok;
}

Stats stats() {
    std::lock_guard<std::mutex> lk(g_mtx);
    Stats s;
    s.indexed_videos = g_index.size();
    s.enabled = g_enabled.load(std::memory_order_relaxed);
    s.active_hash = g_session.hash();
    s.playing = s.active_hash != 0;
    if (s.playing) g_session.current_dims(s.video_w, s.video_h);
    return s;
}

}  // namespace fmvplayer

#endif
