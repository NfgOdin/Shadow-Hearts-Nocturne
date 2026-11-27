#include "launcher_app.hpp"
#include "launcher_data.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cwctype>
#include <filesystem>
#include <string>

namespace {

std::string wide_to_ascii_lower(const wchar_t *ws) {
    std::string out;
    if (!ws) return out;
    for (; *ws; ++ws) {
        out.push_back(static_cast<char>(std::towlower(*ws) & 0x7F));
    }
    return out;
}

bool try_direct_play(const std::filesystem::path &root, const std::string &game_arg,
                     int load_slot, int fs_override) {
    launcher::GameId gid = launcher::GameId::Covenant;
    if (game_arg == "koudelka" || game_arg == "ps1") gid = launcher::GameId::Koudelka;
    else if (game_arg == "sh1" || game_arg == "shadowhearts1") gid = launcher::GameId::ShadowHearts1;
    else if (game_arg == "sh2" || game_arg == "sh2-covenant" || game_arg == "covenant") gid = launcher::GameId::Covenant;
    else if (game_arg == "sh3" || game_arg == "sh3-ftnw" || game_arg == "ftnw") gid = launcher::GameId::FromTheNewWorld;
    else return false;

    const launcher::LauncherPaths paths = launcher::resolve_paths(root, gid);
    const launcher::GameProfile &prof = launcher::game_profile(gid);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(paths.game_exe, ec)) {
        MessageBoxW(nullptr,
                    L"This game is not built yet. Launch Shadow Hearts Nocturne.exe without --play and run Setup first.",
                    L"Shadow Hearts: Nocturne", MB_OK | MB_ICONWARNING);
        return true;
    }

    launcher::Settings settings = launcher::load_settings(paths.keys_ini);
    const bool fullscreen = (fs_override >= 0) ? (fs_override != 0) : settings.launcher_fullscreen;

    // Prepend root_dir to PATH so shared SDL2.dll / dxcompiler.dll / dxil.dll load cleanly.
    const std::wstring root_w = paths.root_dir.wstring();
    const DWORD len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring cur_path(len > 0 ? len : 1, L'\0');
    if (len > 0) {
        const DWORD got = GetEnvironmentVariableW(L"PATH", cur_path.data(), len);
        cur_path.resize(got);
    } else {
        cur_path.clear();
    }
    if (cur_path.find(root_w) == std::wstring::npos) {
        const std::wstring updated = cur_path.empty() ? root_w : (root_w + L";" + cur_path);
        SetEnvironmentVariableW(L"PATH", updated.c_str());
    }

    auto to_wide = [](const std::string &s) -> std::wstring {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n > 0 ? static_cast<std::size_t>(n) : 0u, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    };
    SetEnvironmentVariableW(L"PS2_WINDOW_TITLE", to_wide(prof.full_title).c_str());
    SetEnvironmentVariableW(L"PS2_SCREENSHOTS_DIR", paths.screenshots_dir.wstring().c_str());
    SetEnvironmentVariableW(L"PS2_SCREENSHOT_PREFIX", to_wide(prof.screenshot_prefix).c_str());
    SetEnvironmentVariableW(L"PS2_ACTIVE_MODS", to_wide(settings.active_mods).c_str());
    SetEnvironmentVariableW(L"PS2_GPU_HW", settings.hw_rasterizer ? L"1" : L"0");
    SetEnvironmentVariableW(L"PS2_GPU_HW_DUAL", settings.dual_source_blend ? L"1" : L"0");
    SetEnvironmentVariableW(L"PS2_VRAM_GUARD", settings.vram_guard ? L"1" : L"0");
    SetEnvironmentVariableW(L"PS2_TEX_HOT_RELOAD", settings.tex_hot_reload ? L"1" : L"0");
    SetEnvironmentVariableW(L"PS2_GPU_SUBMIT_THREAD", settings.gpu_submit_thread ? L"1" : L"0");
    if (settings.vram_budget > 0) {
        SetEnvironmentVariableW(L"PS2_VRAM_BUDGET_MB", std::to_wstring(settings.vram_budget).c_str());
    }

    std::wstring cmd = launcher::build_launch_command(paths, fullscreen, load_slot);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       paths.root_dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return true;
}

}  // namespace

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
    // If launched directly from build/native-sh2-covenant/ or build/engine/, step up to repo root
    std::error_code ec;
    if (!std::filesystem::is_directory(root / "games", ec) &&
        std::filesystem::is_directory(root.parent_path().parent_path() / "games", ec)) {
        root = root.parent_path().parent_path();
    }

    // Direct game launch CLI: "Shadow Hearts Nocturne.exe --play <koudelka|sh1|sh2|sh3> [--slot <0-9|auto>] [--fullscreen|--window]"
    // In-Game NPC Duel CLI:   "Shadow Hearts Nocturne.exe --npc-duel <npc_id> --difficulty <0|1|2> [--ante <card_id>] --result-file <path>"
    std::string npc_duel_id;
    int npc_duel_diff = 1;
    int npc_duel_bet = 0;
    std::string npc_duel_ante;
    std::filesystem::path npc_duel_result;

    int argc = 0;
    if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        std::string play_id;
        int load_slot = -1;
        int fs_override = -1;
        auto wide_to_utf8 = [](const wchar_t *ws) -> std::string {
            if (!ws || !*ws) return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 1 ? static_cast<std::size_t>(n - 1) : 0u, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
            return s;
        };
        for (int i = 1; i < argc; ++i) {
            const std::string a = wide_to_ascii_lower(argv[i]);
            if (a == "--play" && i + 1 < argc) {
                play_id = wide_to_ascii_lower(argv[++i]);
            } else if ((a == "--slot" || a == "--load-slot") && i + 1 < argc) {
                const std::string sv = wide_to_ascii_lower(argv[++i]);
                load_slot = (sv == "auto" || sv == "autosave") ? 10 : std::atoi(sv.c_str());
            } else if (a == "--fullscreen") {
                fs_override = 1;
            } else if (a == "--window") {
                fs_override = 0;
            } else if (a == "--npc-duel" && i + 1 < argc) {
                npc_duel_id = wide_to_utf8(argv[++i]);
            } else if (a == "--difficulty" && i + 1 < argc) {
                npc_duel_diff = std::atoi(wide_to_ascii_lower(argv[++i]).c_str());
            } else if (a == "--bet" && i + 1 < argc) {
                npc_duel_bet = std::max(0, std::atoi(wide_to_ascii_lower(argv[++i]).c_str()));
            } else if (a == "--ante" && i + 1 < argc) {
                npc_duel_ante = wide_to_utf8(argv[++i]);
            } else if (a == "--result-file" && i + 1 < argc) {
                npc_duel_result = std::filesystem::path(argv[++i]);
            }
        }
        LocalFree(argv);
        if (!play_id.empty() && try_direct_play(root, play_id, load_slot, fs_override)) {
            if (SUCCEEDED(hr)) CoUninitialize();
            return 0;
        }
    }

    int rc = 0;
    {
        launcher::LauncherApp app(root);
        if (!npc_duel_id.empty()) {
            app.configure_npc_duel(npc_duel_id, npc_duel_diff, npc_duel_bet, npc_duel_ante, npc_duel_result);
        }
        rc = app.run(hinst, show_cmd);
    }

    if (SUCCEEDED(hr)) CoUninitialize();
    return rc;
}
