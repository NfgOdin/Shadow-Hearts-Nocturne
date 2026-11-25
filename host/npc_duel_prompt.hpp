#pragma once

// Shadow Hearts: Nocturne — In-Game NPC Square-to-Challenge Prompt & Launcher Bridge
// Shared by both Koudelka (host/psx_main.cpp) and SH1/SH2/SH3 (host/window.cpp).
// Flow when pressing Square near a mapped NPC:
//   1. "Are you sure you want to challenge [NPC] to Judgment Ring Duel?" + Wager info
//   2. Pick Difficulty: Strictly 3 choices (Easy / Normal / Hard)
//   3. Pauses RPG cleanly and launches Shadow Hearts Nocturne.exe --npc-duel ...
//      (Rules Grimoire ALWAYS opens first -> Opening Mulligan -> Full 3D Duel -> Return to Game)
//   4. Reads match outcome and returns wager summary to the RPG host.

#include "card_album.hpp"
#include "card_game.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace npc_duel {

struct ChallengeOutcome {
    bool played = false;
    int winner = -1;  // 0 = Player, 1 = NPC, 2 = Draw
    int difficulty = 1;
    int money_delta = 0;  // +cash on win, -cash on loss for Money Wagers
    std::string ante_card_id;
    std::string summary;
};

#ifdef _WIN32
namespace detail {

inline std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? static_cast<std::size_t>(n) : 0u, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct DiffDialogState {
    int chosen = -1;  // 0=Easy, 1=Normal, 2=Hard, -1=Cancel
    bool done = false;
};

inline LRESULT CALLBACK diff_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *st = reinterpret_cast<DiffDialogState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (st && id >= 100 && id <= 103) {
            st->chosen = (id == 103) ? -1 : (id - 100);
            st->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE:
        if (st) { st->chosen = -1; st->done = true; }
        DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// Clean native 3-button Difficulty Picker (Easy / Normal / Hard / Cancel)
inline int pick_difficulty_modal(const std::string &npc_name, const std::string &wager_line) {
    static bool registered = false;
    const HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = diff_wnd_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"NocturneDiffPickerWnd";
        RegisterClassW(&wc);
        registered = true;
    }

    DiffDialogState st{};
    const int w = 420, h = 190;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND dlg = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        L"NocturneDiffPickerWnd",
        utf8_to_wide("Judgment Ring Duel — " + npc_name).c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        sx, sy, w, h, nullptr, nullptr, hi, &st);
    if (!dlg) return 1;

    const std::wstring header = utf8_to_wide("Choose Duel Difficulty against " + npc_name + "\n" + wager_line);
    CreateWindowExW(0, L"STATIC", header.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER,
                    16, 16, w - 40, 48, dlg, nullptr, hi, nullptr);

    CreateWindowExW(0, L"BUTTON", L"Easy",   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,  24, 78, 110, 34, dlg, reinterpret_cast<HMENU>(100), hi, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Normal", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 148, 78, 110, 34, dlg, reinterpret_cast<HMENU>(101), hi, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Hard",   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 272, 78, 110, 34, dlg, reinterpret_cast<HMENU>(102), hi, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 148, 120, 110, 26, dlg, reinterpret_cast<HMENU>(103), hi, nullptr);

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);

    MSG m{};
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            st.chosen = -1;
            st.done = true;
            DestroyWindow(dlg);
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return st.chosen;
}

inline std::string pick_ante_card(const cards::Collection &col, const std::string &npc_card_id) {
    const auto *target = cards::find_card_def(npc_card_id);
    const int target_r = target ? static_cast<int>(target->rarity) : 1;
    std::string best_id;
    int best_dist = 999;
    for (const auto &c : cards::catalog()) {
        const auto *oc = col.find_owned(c.id);
        if (!oc || !oc->unlocked()) continue;
        const int dist = std::abs(static_cast<int>(c.rarity) - target_r);
        if (dist < best_dist) {
            best_dist = dist;
            best_id = c.id;
        }
    }
    return best_id;
}

}  // namespace detail
#endif

// Interactive In-Game Square Challenge Flow:
//   Step 1: "Are you sure you want to challenge [NPC]?" + Wager disclosure
//   Step 2: Pick Difficulty (Easy / Normal / Hard)
//   Step 3: Launch Shadow Hearts Nocturne.exe --npc-duel ... and wait cleanly for return
inline ChallengeOutcome prompt_and_run_challenge(const std::filesystem::path &nocturne_root,
                                                 const card_game::NpcChallenger &npc,
                                                 cards::Collection &col) {
    ChallengeOutcome out{};
#ifdef _WIN32
    col.reload_if_changed();
    std::string ante_id;
    std::string wager_desc = "Friendly Match (No Wager)";
    if (npc.wager_type == card_game::WagerType::Money) {
        wager_desc = "Money Wager: " + std::to_string(npc.wager_money) + " Cash (Easy) / " +
                     std::to_string(card_game::scaled_money_wager(npc.wager_money, card_game::AiDifficulty::Normal)) + " (Normal) / " +
                     std::to_string(card_game::scaled_money_wager(npc.wager_money, card_game::AiDifficulty::Hard)) + " (Hard)";
    } else if (npc.wager_type == card_game::WagerType::Card) {
        ante_id = detail::pick_ante_card(col, npc.wager_card_id);
        const auto *ndef = cards::find_card_def(npc.wager_card_id);
        const auto *adef = cards::find_card_def(ante_id);
        if (ante_id.empty()) {
            MessageBoxW(nullptr,
                        detail::utf8_to_wide(npc.name + " wants a Card Wager, but you don't own any cards yet!\n"
                                             "Earn your first cards through story progress or trophies, then challenge them again.").c_str(),
                        L"Judgment Ring Duel", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
            return out;
        }
        wager_desc = "CARD WAGER — Win: [" + std::string(ndef ? cards::rarity_name(ndef->rarity) : "Card") + "] " +
                     (ndef ? ndef->name : npc.wager_card_id) +
                     "\nYour Ante (Lost if Defeated!): " + (adef ? adef->name : ante_id);
    }

    // Step 1: "Are you sure you want to play?"
    const std::string ask =
        "Are you sure you want to challenge " + npc.name + " to Judgment Ring Duel?\n\n" +
        "Location: " + npc.location + "\n" + wager_desc;
    const int yn = MessageBoxW(nullptr, detail::utf8_to_wide(ask).c_str(),
                               L"Challenge NPC — Judgment Ring Duel",
                               MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    if (yn != IDYES) return out;

    // Step 2: Pick Difficulty (Strictly Easy / Normal / Hard)
    const int diff = detail::pick_difficulty_modal(npc.name, wager_desc);
    if (diff < 0 || diff > 2) return out;

    // Step 3: Launch full 3D Judgment Ring Duel in Shadow Hearts Nocturne.exe
    const std::filesystem::path launcher_exe = nocturne_root / "Shadow Hearts Nocturne.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(launcher_exe, ec)) {
        out.summary = "Launcher not found at " + launcher_exe.string();
        return out;
    }

    const std::filesystem::path res_file =
        std::filesystem::temp_directory_path(ec) /
        ("nocturne_npc_duel_" + std::to_string(GetTickCount64()) + ".txt");
    std::filesystem::remove(res_file, ec);

    std::wstring cmd = L"\"" + launcher_exe.wstring() + L"\" --npc-duel \"" +
                       detail::utf8_to_wide(npc.id) + L"\" --difficulty " +
                       std::to_wstring(diff) + L" --result-file \"" + res_file.wstring() + L"\"";
    if (!ante_id.empty()) {
        cmd += L" --ante \"" + detail::utf8_to_wide(ante_id) + L"\"";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nocturne_root.c_str(), &si, &pi)) {
        return out;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Parse match outcome written by launcher
    int winner = -1;
    if (std::ifstream ifs(res_file); ifs.is_open()) {
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.rfind("winner=", 0) == 0) {
                winner = std::atoi(line.c_str() + 7);
            }
        }
    }
    std::filesystem::remove(res_file, ec);

    if (winner < 0) return out;  // Player closed window before finishing
    out.played = true;
    out.winner = winner;
    out.difficulty = diff;
    out.ante_card_id = ante_id;

    if (npc.wager_type == card_game::WagerType::Money) {
        const int amt = card_game::scaled_money_wager(npc.wager_money, static_cast<card_game::AiDifficulty>(diff));
        if (winner == 0) {
            out.money_delta = +amt;
            out.summary = "Duel Victory vs " + npc.name + "! Won +" + std::to_string(amt) + " Cash!";
        } else if (winner == 1) {
            out.money_delta = -amt;
            out.summary = "Duel Defeat vs " + npc.name + ". Lost -" + std::to_string(amt) + " Cash.";
        } else {
            out.summary = "Duel Draw vs " + npc.name + " — Cash returned.";
        }
    } else if (npc.wager_type == card_game::WagerType::Card) {
        const auto *ndef = cards::find_card_def(npc.wager_card_id);
        const auto *adef = cards::find_card_def(ante_id);
        if (winner == 0) {
            out.summary = "Card Wager Won! Claimed " + (ndef ? ndef->name : npc.wager_card_id) + "!";
        } else if (winner == 1) {
            out.summary = "Card Wager Lost — Forfeited " + (adef ? adef->name : ante_id) + ".";
        } else {
            out.summary = "Duel Draw — Cards returned.";
        }
    } else {
        out.summary = (winner == 0) ? ("Friendly Duel Victory vs " + npc.name + "!")
                    : (winner == 1 ? ("Friendly Duel Defeat vs " + npc.name + ".")
                                   : ("Friendly Duel Draw vs " + npc.name + "."));
    }
    col.reload_if_changed();
#else
    (void)nocturne_root; (void)npc; (void)col;
#endif
    return out;
}

}  // namespace npc_duel
