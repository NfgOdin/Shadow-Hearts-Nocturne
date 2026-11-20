#include "launcher_app.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <filesystem>

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int show_cmd) {
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using SetDpiCtxFn = BOOL(WINAPI *)(void *);
        if (auto fn = reinterpret_cast<SetDpiCtxFn>(GetProcAddress(u32, "SetProcessDpiAwarenessContext"))) {
            fn(reinterpret_cast<void *>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        } else {
            SetProcessDPIAware();
        }
    }
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t exe_buf[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, exe_buf, static_cast<DWORD>(std::size(exe_buf)));
    std::filesystem::path root = std::filesystem::path(exe_buf).parent_path();
    // If launched directly from build/native-sh2-covenant/, step up to repo root
    std::error_code ec;
    if (!std::filesystem::is_directory(root / "games" / "sh2-covenant", ec) &&
        std::filesystem::is_directory(root.parent_path().parent_path() / "games" / "sh2-covenant", ec)) {
        root = root.parent_path().parent_path();
    }

    int rc = 0;
    {
        launcher::LauncherApp app(root);
        rc = app.run(hinst, show_cmd);
    }

    if (SUCCEEDED(hr)) CoUninitialize();
    return rc;
}
