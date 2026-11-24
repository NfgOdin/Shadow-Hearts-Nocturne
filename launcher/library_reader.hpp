#pragma once

// Shadow Hearts: Nocturne — Zero-Dependency Embedded PDF & CBZ Page Renderer
// - Renders PDF pages on-demand via Windows 10/11 built-in Windows.Data.Pdf.PdfDocument (CREATE_NO_WINDOW)
// - Extracts & naturally sorts CBZ / ZIP manga image archives
// - Caches rendered PNG/JPG pages under library/.cache/<item_id>/ for instant page turns

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace launcher {

inline bool natural_less(const std::string &a, const std::string &b) {
    std::size_t ia = 0, ib = 0;
    while (ia < a.size() && ib < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[ia])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[ib])) != 0;
        if (da && db) {
            std::size_t za = ia;
            while (za < a.size() && a[za] == '0') ++za;
            std::size_t zb = ib;
            while (zb < b.size() && b[zb] == '0') ++zb;
            std::size_t ea = za;
            while (ea < a.size() && std::isdigit(static_cast<unsigned char>(a[ea]))) ++ea;
            std::size_t eb = zb;
            while (eb < b.size() && std::isdigit(static_cast<unsigned char>(b[eb]))) ++eb;
            const std::size_t la = ea - za;
            const std::size_t lb = eb - zb;
            if (la != lb) return la < lb;
            for (std::size_t k = 0; k < la; ++k) {
                if (a[za + k] != b[zb + k]) return a[za + k] < b[zb + k];
            }
            if ((ea - ia) != (eb - ib)) return (ea - ia) < (eb - ib);
            ia = ea;
            ib = eb;
        } else {
            const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[ia])));
            const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[ib])));
            if (ca != cb) return ca < cb;
            ++ia;
            ++ib;
        }
    }
    return a.size() < b.size();
}

inline std::filesystem::path find_cached_cover_page(const std::filesystem::path &library_dir, const std::string &item_id) {
    if (item_id.empty()) return {};
    const std::filesystem::path cdir = library_dir / ".cache" / item_id;
    std::error_code ec;
    if (!std::filesystem::is_directory(cdir, ec)) return {};
    const std::filesystem::path p1 = cdir / "page_0001.png";
    if (std::filesystem::is_regular_file(p1, ec)) return p1;
    std::vector<std::filesystem::path> imgs;
    for (const auto &ent : std::filesystem::recursive_directory_iterator(cdir, ec)) {
        if (!ent.is_regular_file(ec)) continue;
        std::string ext = ent.path().extension().string();
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp") {
            imgs.push_back(ent.path());
        }
    }
    if (imgs.empty()) return {};
    std::sort(imgs.begin(), imgs.end(), [](const std::filesystem::path &a, const std::filesystem::path &b) {
        return natural_less(a.generic_string(), b.generic_string());
    });
    return imgs.front();
}

class LibraryReader {
public:
    LibraryReader() = default;
    ~LibraryReader() { close(); }

    LibraryReader(const LibraryReader &) = delete;
    LibraryReader &operator=(const LibraryReader &) = delete;

    void close() {
        ++gen_;
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lk(mu_);
        item_id_.clear();
        file_path_.clear();
        cache_dir_.clear();
        is_pdf_ = false;
        page_count_ = 0;
        busy_ = false;
        error_.clear();
        cbz_pages_.clear();
        pending_start_ = -1;
        pending_count_ = 0;
    }

    void open(const std::string &item_id, const std::filesystem::path &file_path,
              const std::filesystem::path &cache_root, int initial_page = 0) {
        close();
        const std::uint64_t g = ++gen_;
        std::string ext = file_path.extension().string();
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        {
            std::lock_guard<std::mutex> lk(mu_);
            item_id_ = item_id;
            file_path_ = file_path;
            cache_dir_ = cache_root / item_id;
            is_pdf_ = (ext == ".pdf");
            busy_ = true;
            error_.clear();
            page_count_ = 0;
            cbz_pages_.clear();
        }

        std::error_code ec;
        std::filesystem::create_directories(cache_root / item_id, ec);

        if (is_pdf_) {
            // Check if page count was already cached from an earlier open
            const int cached_n = read_cached_pdf_count(cache_root / item_id);
            if (cached_n > 0) {
                std::lock_guard<std::mutex> lk(mu_);
                page_count_ = cached_n;
            }
            const int p0 = std::max(0, initial_page);
            worker_ = std::thread([this, g, file_path, cdir = cache_root / item_id, p0]() {
                render_pdf_window(g, file_path, cdir, p0, 4);
            });
        } else {
            worker_ = std::thread([this, g, file_path, cdir = cache_root / item_id]() {
                extract_cbz_archive(g, file_path, cdir);
            });
        }
    }

    // Request a window of pages (for PDF on-demand rendering + lookahead)
    void ensure_pages(int start_page, int count = 4) {
        if (!is_pdf_) return;
        std::filesystem::path fp, cdir;
        std::uint64_t g = gen_.load();
        bool need_render = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (file_path_.empty()) return;
            fp = file_path_;
            cdir = cache_dir_;
            const int end_p = (page_count_ > 0) ? std::min(page_count_, start_page + count) : (start_page + count);
            for (int p = std::max(0, start_page); p < end_p; ++p) {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(pdf_page_path(cdir, p), ec)) {
                    need_render = true;
                    break;
                }
            }
            if (!need_render) return;
            if (busy_) {
                pending_start_ = start_page;
                pending_count_ = count;
                return;
            }
            busy_ = true;
        }
        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this, g, fp, cdir, start_page, count]() {
            render_pdf_window(g, fp, cdir, start_page, count);
        });
    }

    // Poll from UI thread to kick off any queued page-turn render after the current batch finishes
    void poll() {
        if (!is_pdf_) return;
        int ps = -1, pc = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (busy_ || pending_start_ < 0) return;
            ps = pending_start_;
            pc = pending_count_;
            pending_start_ = -1;
            pending_count_ = 0;
        }
        if (ps >= 0) ensure_pages(ps, pc);
    }

    bool is_open() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !file_path_.empty();
    }

    bool is_busy() const {
        std::lock_guard<std::mutex> lk(mu_);
        return busy_;
    }

    int page_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return page_count_;
    }

    std::string error() const {
        std::lock_guard<std::mutex> lk(mu_);
        return error_;
    }

    std::filesystem::path page_image_path(int page_idx) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (page_idx < 0) return {};
        if (!is_pdf_) {
            if (page_idx < static_cast<int>(cbz_pages_.size())) return cbz_pages_[static_cast<std::size_t>(page_idx)];
            return {};
        }
        if (page_count_ > 0 && page_idx >= page_count_) return {};
        const std::filesystem::path p = pdf_page_path(cache_dir_, page_idx);
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec) ? p : std::filesystem::path{};
    }

private:
    static std::filesystem::path pdf_page_path(const std::filesystem::path &cdir, int page_idx) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "page_%04d.png", page_idx + 1);
        return cdir / buf;
    }

    static int read_cached_pdf_count(const std::filesystem::path &cdir) {
        std::ifstream ifs(cdir / "count.txt");
        int n = 0;
        if (ifs >> n && n > 0) return n;
        return 0;
    }

    static bool is_image_ext(const std::filesystem::path &p) {
        std::string ext = p.extension().string();
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp";
    }

    static bool run_hidden_powershell(const std::filesystem::path &script_path) {
#ifdef _WIN32
        std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
                           script_path.wstring() + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
        cmdbuf.push_back(L'\0');
        if (!CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return false;
        }
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return exit_code == 0;
#else
        (void)script_path;
        return false;
#endif
    }

    static std::string ps_escape_sq(const std::string &s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '\'') out += "''";
            else out.push_back(c);
        }
        return out;
    }

    void render_pdf_window(std::uint64_t g, const std::filesystem::path &pdf_path,
                           const std::filesystem::path &cdir, int start_page, int count) {
        const std::filesystem::path ps1 = cdir / "_render_pdf.ps1";
        {
            std::ofstream ofs(ps1, std::ios::trunc);
            ofs << "$ErrorActionPreference = 'Stop'\n"
                << "Add-Type -AssemblyName System.Runtime.WindowsRuntime\n"
                << "$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]\n"
                << "function Await($WinRtTask, $ResultType) {\n"
                << "  $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)\n"
                << "  $netTask = $asTask.Invoke($null, @($WinRtTask))\n"
                << "  $netTask.Wait(-1) | Out-Null\n"
                << "  return $netTask.Result\n"
                << "}\n"
                << "$asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' })[0]\n"
                << "function AwaitAction($WinRtTask) {\n"
                << "  $netTask = $asTaskAction.Invoke($null, @($WinRtTask))\n"
                << "  $netTask.Wait(-1) | Out-Null\n"
                << "}\n"
                << "[void][Windows.Storage.StorageFile,Windows.Storage,ContentType=WindowsRuntime]\n"
                << "[void][Windows.Storage.StorageFolder,Windows.Storage,ContentType=WindowsRuntime]\n"
                << "[void][Windows.Data.Pdf.PdfDocument,Windows.Data.Pdf,ContentType=WindowsRuntime]\n"
                << "[void][Windows.Data.Pdf.PdfPageRenderOptions,Windows.Data.Pdf,ContentType=WindowsRuntime]\n"
                << "$pdfFile = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync('" << ps_escape_sq(pdf_path.string()) << "')) ([Windows.Storage.StorageFile])\n"
                << "$doc = Await ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($pdfFile)) ([Windows.Data.Pdf.PdfDocument])\n"
                << "$total = [int]$doc.PageCount\n"
                << "$outDir = '" << ps_escape_sq(cdir.string()) << "'\n"
                << "Set-Content -Path (Join-Path $outDir 'count.txt') -Value $total -Encoding ASCII\n"
                << "$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($outDir)) ([Windows.Storage.StorageFolder])\n"
                << "$start = " << std::max(0, start_page) << "\n"
                << "$end = [Math]::Min($total - 1, $start + " << std::max(1, count) - 1 << ")\n"
                << "for ($p = $start; $p -le $end; $p++) {\n"
                << "  $name = ('page_{0:D4}.png' -f ($p + 1))\n"
                << "  $finalPath = Join-Path $outDir $name\n"
                << "  if (Test-Path $finalPath) { continue }\n"
                << "  $tmpName = $name + '.tmp'\n"
                << "  $page = $doc.GetPage([uint32]$p)\n"
                << "  $opts = New-Object Windows.Data.Pdf.PdfPageRenderOptions\n"
                << "  $opts.DestinationHeight = 1600\n"
                << "  $outFile = Await ($folder.CreateFileAsync($tmpName, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])\n"
                << "  $stream = Await ($outFile.OpenAsync([Windows.Storage.FileAccessMode]::ReadWrite)) ([Windows.Storage.Streams.IRandomAccessStream])\n"
                << "  AwaitAction ($page.RenderToStreamAsync($stream, $opts))\n"
                << "  $stream.Dispose()\n"
                << "  $page.Dispose()\n"
                << "  Move-Item -Force -Path (Join-Path $outDir $tmpName) -Destination $finalPath\n"
                << "}\n";
        }

        const bool ok = run_hidden_powershell(ps1);
        std::error_code ec;
        std::filesystem::remove(ps1, ec);
        if (gen_.load() != g) return;

        const int total = read_cached_pdf_count(cdir);
        std::lock_guard<std::mutex> lk(mu_);
        if (total > 0) {
            page_count_ = total;
            error_.clear();
        } else if (!ok) {
            error_ = "Could not render PDF pages (try Open External).";
        }
        busy_ = false;
    }

    void extract_cbz_archive(std::uint64_t g, const std::filesystem::path &cbz_path,
                             const std::filesystem::path &cdir) {
        auto collect_images = [&]() {
            std::vector<std::filesystem::path> imgs;
            std::error_code ec;
            for (const auto &ent : std::filesystem::recursive_directory_iterator(cdir, ec)) {
                if (ent.is_regular_file(ec) && is_image_ext(ent.path())) {
                    imgs.push_back(ent.path());
                }
            }
            std::sort(imgs.begin(), imgs.end(), [](const std::filesystem::path &a, const std::filesystem::path &b) {
                return natural_less(a.generic_string(), b.generic_string());
            });
            return imgs;
        };

        std::vector<std::filesystem::path> imgs = collect_images();
        std::error_code ec;
        const std::filesystem::path done_marker = cdir / "_cbz_ready.txt";
        if (imgs.empty() || !std::filesystem::is_regular_file(done_marker, ec)) {
            const std::filesystem::path ps1 = cdir / "_extract_cbz.ps1";
            {
                std::ofstream ofs(ps1, std::ios::trunc);
                ofs << "$ErrorActionPreference = 'Stop'\n"
                    << "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                    << "$zip = [System.IO.Compression.ZipFile]::OpenRead('" << ps_escape_sq(cbz_path.string()) << "')\n"
                    << "$outDir = '" << ps_escape_sq(cdir.string()) << "'\n"
                    << "foreach ($e in $zip.Entries) {\n"
                    << "  if ([string]::IsNullOrEmpty($e.Name)) { continue }\n"
                    << "  $rel = $e.FullName -replace '/', '\\'\n"
                    << "  $dest = Join-Path $outDir $rel\n"
                    << "  $parent = Split-Path -Parent $dest\n"
                    << "  if (!(Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }\n"
                    << "  if (!(Test-Path $dest)) { [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true) }\n"
                    << "}\n"
                    << "$zip.Dispose()\n"
                    << "Set-Content -Path (Join-Path $outDir '_cbz_ready.txt') -Value 'ok' -Encoding ASCII\n";
            }
            run_hidden_powershell(ps1);
            std::filesystem::remove(ps1, ec);
            imgs = collect_images();
        }
        if (gen_.load() != g) return;

        std::lock_guard<std::mutex> lk(mu_);
        cbz_pages_ = std::move(imgs);
        page_count_ = static_cast<int>(cbz_pages_.size());
        if (page_count_ == 0) {
            error_ = "No readable image pages found inside CBZ/ZIP archive.";
        }
        busy_ = false;
    }

    mutable std::mutex mu_;
    std::atomic<std::uint64_t> gen_{0};
    std::thread worker_;
    std::string item_id_;
    std::filesystem::path file_path_;
    std::filesystem::path cache_dir_;
    bool is_pdf_ = false;
    int page_count_ = 0;
    bool busy_ = false;
    std::string error_;
    std::vector<std::filesystem::path> cbz_pages_;
    int pending_start_ = -1;
    int pending_count_ = 0;
};

}  // namespace launcher
