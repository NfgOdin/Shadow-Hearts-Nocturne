#pragma once

// Windows-specific low-latency performance helpers:
// 1. MMCSS ("Games") thread priority registration via avrt.dll (dynamic load, zero link dep)
// 2. DXGI VRAM budget query for automatic low-VRAM cache sizing on <= 2 GB GPUs/iGPUs
// 3. Macro to export NvOptimusEnablement + AmdPowerXpressRequestHighPerformance from EXEs
//    so hybrid-GPU laptops automatically pick the dedicated NVIDIA/AMD GPU over Intel UHD.

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#endif

// Place once in the main translation unit of each Windows executable (nocturne_ps2, nocturne_psx).
#ifdef _WIN32
#define NOCTURNE_EXPORT_HIGH_PERF_GPU()                                              \
    extern "C" {                                                                     \
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001ul;          \
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;              \
    }
#else
#define NOCTURNE_EXPORT_HIGH_PERF_GPU()
#endif

namespace win_perf {

// RAII guard registering the calling thread with Windows Multimedia Class Scheduler Service
// ("Games" profile) so background Windows tasks never preempt emulation or audio threads.
class MmcssGuard {
public:
    explicit MmcssGuard(const wchar_t *task_name = L"Games") noexcept {
#ifdef _WIN32
        HMODULE avrt = LoadLibraryW(L"avrt.dll");
        if (!avrt) return;
        using SetCharsFn = HANDLE(WINAPI *)(LPCWSTR, LPDWORD);
        using SetPrioFn = BOOL(WINAPI *)(HANDLE, int);
        auto *set_chars = reinterpret_cast<SetCharsFn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"));
        auto *set_prio = reinterpret_cast<SetPrioFn>(GetProcAddress(avrt, "AvSetMmThreadPriority"));
        revert_fn_ = reinterpret_cast<RevertFn>(GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"));
        if (set_chars) {
            DWORD task_idx = 0;
            handle_ = set_chars(task_name ? task_name : L"Games", &task_idx);
            if (handle_ && set_prio) {
                // AVRT_PRIORITY_HIGH = 1
                set_prio(handle_, 1);
            }
        }
        avrt_mod_ = avrt;
#else
        (void)task_name;
#endif
    }

    ~MmcssGuard() noexcept {
#ifdef _WIN32
        if (handle_ && revert_fn_) {
            revert_fn_(handle_);
            handle_ = nullptr;
        }
        if (avrt_mod_) {
            FreeLibrary(avrt_mod_);
            avrt_mod_ = nullptr;
        }
#endif
    }

    MmcssGuard(const MmcssGuard &) = delete;
    MmcssGuard &operator=(const MmcssGuard &) = delete;

    bool active() const noexcept {
#ifdef _WIN32
        return handle_ != nullptr;
#else
        return false;
#endif
    }

private:
#ifdef _WIN32
    using RevertFn = BOOL(WINAPI *)(HANDLE);
    HMODULE avrt_mod_ = nullptr;
    HANDLE handle_ = nullptr;
    RevertFn revert_fn_ = nullptr;
#endif
};

// Returns dedicated VRAM in MiB of the best hardware GPU (or 0 if unavailable / non-Windows).
// Used to automatically tighten texture/override caches on <= 2 GB iGPUs & older laptops.
inline std::uint64_t query_best_gpu_vram_mb() noexcept {
#ifdef _WIN32
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return 0;
    using CreateFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
    auto *create_fn = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    std::uint64_t best_mb = 0;
    if (create_fn) {
        IDXGIFactory1 *factory = nullptr;
        if (SUCCEEDED(create_fn(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1 *adapter = nullptr;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                if (!adapter) continue;
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    std::uint64_t mb = static_cast<std::uint64_t>(desc.DedicatedVideoMemory) >> 20;
                    // Also check live local budget via IDXGIAdapter3 when available (handles shared iGPU pools)
                    IDXGIAdapter3 *a3 = nullptr;
                    if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&a3))) && a3) {
                        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                            const std::uint64_t budget_mb = info.Budget >> 20;
                            if (budget_mb > 0 && (mb == 0 || budget_mb < mb)) mb = budget_mb;
                        }
                        a3->Release();
                    }
                    if (mb > best_mb) best_mb = mb;
                }
                adapter->Release();
            }
            factory->Release();
        }
    }
    FreeLibrary(dxgi);
    return best_mb;
#else
    return 0;
#endif
}

}  // namespace win_perf
