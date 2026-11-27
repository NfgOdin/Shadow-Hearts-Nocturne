#pragma once

// Shadow Hearts: Nocturne — In-Game Gothic NPC Square-to-Challenge Prompt & Launcher Bridge
// Shared by both Koudelka (host/psx_main.cpp) and SH1/SH2/SH3 (host/window.cpp).
// Flow when pressing Square:
//   1. Custom-painted Shadow Hearts Gothic prompt (NO plain white Windows MessageBox):
//      - Shows NPC Name, Location, and explicit BETTING MODE (Friendly Match / Money Bet / Card Bet)
//      - For Money Bets: displays live In-Game Cash and disables any difficulty exceeding player's Cash!
//      - Strictly 3 difficulty choices (Easy / Normal / Hard) + Decline
//   2. Pauses RPG emulation + audio cleanly and launches Shadow Hearts Nocturne.exe --npc-duel ...
//   3. Reads match outcome and returns wager summary + exact money_delta to apply to live RPG RAM!

#include "card_album.hpp"
#include "card_game.hpp"

#include <algorithm>
#include <array>
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
    int money_delta = 0;  // +cash on win, -cash on loss for Money Bets
    std::string ante_card_id;
    std::string summary;
};

inline std::filesystem::path find_nocturne_root(const std::filesystem::path &hint = {}) {
    std::error_code ec;
    auto check_chain = [&](std::filesystem::path p) -> std::filesystem::path {
        for (int i = 0; i < 4 && !p.empty(); ++i) {
            if (std::filesystem::is_regular_file(p / "Shadow Hearts Nocturne.exe", ec)) return p;
            auto par = p.parent_path();
            if (par == p) break;
            p = std::move(par);
        }
        return {};
    };
    if (!hint.empty()) {
        if (auto found = check_chain(hint); !found.empty()) return found;
    }
    if (auto found = check_chain(std::filesystem::current_path(ec)); !found.empty()) return found;
#ifdef _WIN32
    wchar_t exe_buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
        if (auto found = check_chain(std::filesystem::path(exe_buf).parent_path()); !found.empty()) return found;
    }
#endif
    return hint.empty() ? std::filesystem::current_path(ec) : hint;
}

#ifdef _WIN32
namespace detail {

inline std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? static_cast<std::size_t>(n) : 0u, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct GothicBtn {
    RECT rc{};
    std::wstring label;
    std::wstring sub;
    bool enabled = true;
    int result = -1;  // 0=Easy, 1=Normal, 2=Hard, -1=Decline
};

struct GothicPromptState {
    std::wstring title;
    std::wstring subtitle;
    std::wstring mode_badge;
    std::wstring detail_line1;
    std::wstring detail_line2;
    std::wstring warning_line;
    COLORREF badge_col = RGB(164, 214, 255);
    COLORREF badge_bg = RGB(22, 32, 54);
    std::array<GothicBtn, 4> btns{};
    int hovered = -1;
    int chosen = -1;
    bool done = false;
};

inline void draw_gothic_prompt(HWND hwnd, HDC hdc, GothicPromptState *st) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    // 1. Deep Shadow Hearts Gothic Obsidian-Crimson Backdrop
    for (int y = 0; y < H; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(std::max(H - 1, 1));
        const int r = static_cast<int>(28.0f * (1.0f - t) + 11.0f * t);
        const int g = static_cast<int>(14.0f * (1.0f - t) + 8.0f * t);
        const int b = static_cast<int>(24.0f * (1.0f - t) + 18.0f * t);
        RECT row{0, y, W, y + 1};
        HBRUSH br = CreateSolidBrush(RGB(r, g, b));
        FillRect(mem, &row, br);
        DeleteObject(br);
    }

    // Top crimson header glow band
    {
        RECT band{2, 2, W - 2, 66};
        HBRUSH br = CreateSolidBrush(RGB(44, 14, 24));
        FillRect(mem, &band, br);
        DeleteObject(br);
    }

    // Double antique-gold filigree border
    auto draw_rect_pen = [&](int x0, int y0, int x1, int y1, COLORREF col, int pw) {
        HPEN pen = CreatePen(PS_SOLID, pw, col);
        HGDIOBJ op = SelectObject(mem, pen);
        HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, x0, y0, x1, y1);
        SelectObject(mem, ob);
        SelectObject(mem, op);
        DeleteObject(pen);
    };
    draw_rect_pen(1, 1, W - 1, H - 1, RGB(214, 174, 92), 2);
    draw_rect_pen(7, 7, W - 7, H - 7, RGB(126, 94, 48), 1);

    // Subtle divider under header
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(166, 128, 66));
        HGDIOBJ op = SelectObject(mem, pen);
        MoveToEx(mem, 20, 68, nullptr);
        LineTo(mem, W - 20, 68);
        SelectObject(mem, op);
        DeleteObject(pen);
    }

    SetBkMode(mem, TRANSPARENT);
    HFONT f_title = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Georgia");
    HFONT f_sub   = CreateFontW(-13, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Georgia");
    HFONT f_badge = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT f_body  = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT f_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // Header Title & Location
    SelectObject(mem, f_title);
    SetTextColor(mem, RGB(248, 220, 146));
    RECT r_title{20, 14, W - 20, 42};
    DrawTextW(mem, st->title.c_str(), -1, &r_title, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(mem, f_sub);
    SetTextColor(mem, RGB(196, 182, 168));
    RECT r_sub{20, 42, W - 20, 62};
    DrawTextW(mem, st->subtitle.c_str(), -1, &r_sub, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // Betting Mode Banner Box
    RECT r_mode{28, 82, W - 28, 120};
    {
        HBRUSH br = CreateSolidBrush(st->badge_bg);
        FillRect(mem, &r_mode, br);
        DeleteObject(br);
        draw_rect_pen(r_mode.left, r_mode.top, r_mode.right, r_mode.bottom, st->badge_col, 1);
        SelectObject(mem, f_badge);
        SetTextColor(mem, st->badge_col);
        DrawTextW(mem, st->mode_badge.c_str(), -1, &r_mode, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // Detail lines
    SelectObject(mem, f_body);
    SetTextColor(mem, RGB(240, 232, 222));
    RECT r_d1{28, 130, W - 28, 154};
    DrawTextW(mem, st->detail_line1.c_str(), -1, &r_d1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(mem, f_small);
    SetTextColor(mem, RGB(204, 194, 186));
    RECT r_d2{28, 154, W - 28, 178};
    DrawTextW(mem, st->detail_line2.c_str(), -1, &r_d2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    if (!st->warning_line.empty()) {
        SelectObject(mem, f_body);
        SetTextColor(mem, RGB(255, 112, 122));
        RECT r_w{28, 180, W - 28, 204};
        DrawTextW(mem, st->warning_line.c_str(), -1, &r_w, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // Custom Gothic Buttons (Easy / Normal / Hard / Decline)
    for (int i = 0; i < 4; ++i) {
        const auto &b = st->btns[static_cast<std::size_t>(i)];
        const bool hov = (st->hovered == i && b.enabled);
        const bool is_decline = (i == 3);
        COLORREF bg = !b.enabled ? RGB(22, 18, 26)
                    : (is_decline ? (hov ? RGB(64, 24, 34) : RGB(36, 18, 26))
                                  : (hov ? RGB(104, 26, 42) : RGB(48, 22, 38)));
        COLORREF bdr = !b.enabled ? RGB(72, 64, 78)
                     : (hov ? RGB(252, 214, 118) : RGB(178, 138, 72));
        COLORREF fg = !b.enabled ? RGB(112, 104, 116)
                    : (hov ? RGB(255, 248, 226) : RGB(242, 224, 182));

        HBRUSH br = CreateSolidBrush(bg);
        FillRect(mem, &b.rc, br);
        DeleteObject(br);
        draw_rect_pen(b.rc.left, b.rc.top, b.rc.right, b.rc.bottom, bdr, hov ? 2 : 1);

        if (b.sub.empty()) {
            SelectObject(mem, f_badge);
            SetTextColor(mem, fg);
            RECT tr = b.rc;
            DrawTextW(mem, b.label.c_str(), -1, &tr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else {
            SelectObject(mem, f_badge);
            SetTextColor(mem, fg);
            RECT tr1{b.rc.left + 4, b.rc.top + 6, b.rc.right - 4, b.rc.top + 28};
            DrawTextW(mem, b.label.c_str(), -1, &tr1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            SelectObject(mem, f_small);
            SetTextColor(mem, b.enabled ? RGB(236, 206, 132) : RGB(140, 88, 96));
            RECT tr2{b.rc.left + 4, b.rc.top + 28, b.rc.right - 4, b.rc.bottom - 5};
            DrawTextW(mem, b.sub.c_str(), -1, &tr2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
    }

    // Bottom hint
    SelectObject(mem, f_small);
    SetTextColor(mem, RGB(144, 134, 152));
    RECT r_hint{16, H - 24, W - 16, H - 8};
    DrawTextW(mem, L"Click a Difficulty or press [1] Easy  [2] Normal  [3] Hard  •  [ESC] Return to Game",
              -1, &r_hint, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);

    DeleteObject(f_title);
    DeleteObject(f_sub);
    DeleteObject(f_badge);
    DeleteObject(f_body);
    DeleteObject(f_small);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

inline LRESULT CALLBACK gothic_prompt_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *st = reinterpret_cast<GothicPromptState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (st) draw_gothic_prompt(hwnd, hdc, st);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!st) return 0;
        const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        int nh = -1;
        for (int i = 0; i < 4; ++i) {
            if (PtInRect(&st->btns[static_cast<std::size_t>(i)].rc, pt)) { nh = i; break; }
        }
        if (nh != st->hovered) {
            st->hovered = nh;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!st) return 0;
        const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        for (int i = 0; i < 4; ++i) {
            const auto &b = st->btns[static_cast<std::size_t>(i)];
            if (b.enabled && PtInRect(&b.rc, pt)) {
                st->chosen = b.result;
                st->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (!st) return 0;
        if (wp == VK_ESCAPE) {
            st->chosen = -1;
            st->done = true;
            DestroyWindow(hwnd);
        } else if (wp == '1' && st->btns[0].enabled) {
            st->chosen = 0; st->done = true; DestroyWindow(hwnd);
        } else if (wp == '2' && st->btns[1].enabled) {
            st->chosen = 1; st->done = true; DestroyWindow(hwnd);
        } else if (wp == '3' && st->btns[2].enabled) {
            st->chosen = 2; st->done = true; DestroyWindow(hwnd);
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

inline int show_gothic_challenge_modal(GothicPromptState &st) {
    static bool registered = false;
    const HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = gothic_prompt_wnd_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.lpszClassName = L"NocturneGothicDuelPromptWnd";
        RegisterClassW(&wc);
        registered = true;
    }

    const int W = 540, H = 336;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    // Layout 3 Difficulty buttons + Decline button
    const int bw = 152, bh = 52, gap = 14;
    const int bx0 = (W - (bw * 3 + gap * 2)) / 2;
    const int by = 212;
    for (int i = 0; i < 3; ++i) {
        st.btns[static_cast<std::size_t>(i)].rc = {bx0 + i * (bw + gap), by, bx0 + i * (bw + gap) + bw, by + bh};
        st.btns[static_cast<std::size_t>(i)].result = i;
    }
    st.btns[3].rc = {(W - 180) / 2, by + bh + 12, (W + 180) / 2, by + bh + 44};
    st.btns[3].label = L"Decline";
    st.btns[3].sub.clear();
    st.btns[3].enabled = true;
    st.btns[3].result = -1;

    HWND dlg = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"NocturneGothicDuelPromptWnd",
        L"Judgment Ring Duel",
        WS_POPUP | WS_VISIBLE,
        sx, sy, W, H, nullptr, nullptr, hi, &st);
    if (!dlg) return -1;

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);
    SetFocus(dlg);

    MSG m{};
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
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

// Interactive In-Game Square Challenge Flow (Gothic Shadow Hearts UI — zero white Windows dialogs!):
//   player_cash: current live RPG wallet balance (-1 if game has no currency, e.g. Koudelka)
inline ChallengeOutcome prompt_and_run_challenge(const std::filesystem::path &nocturne_root_in,
                                                 const card_game::NpcChallenger &npc,
                                                 cards::Collection &col,
                                                 int player_cash = -1) {
    ChallengeOutcome out{};
#ifdef _WIN32
    const std::filesystem::path nocturne_root = find_nocturne_root(nocturne_root_in);
    col.reload_if_changed();

    detail::GothicPromptState st{};
    st.title = detail::utf8_to_wide("CHALLENGE " + npc.name + "?");
    st.subtitle = detail::utf8_to_wide("Judgment Ring Duel  •  " + npc.location);
    st.btns[0].label = L"Easy";
    st.btns[1].label = L"Normal";
    st.btns[2].label = L"Hard";

    std::string ante_id;
    if (npc.wager_type == card_game::WagerType::Money) {
        const int b0 = card_game::scaled_money_wager(npc.wager_money, card_game::AiDifficulty::Easy);
        const int b1 = card_game::scaled_money_wager(npc.wager_money, card_game::AiDifficulty::Normal);
        const int b2 = card_game::scaled_money_wager(npc.wager_money, card_game::AiDifficulty::Hard);
        st.mode_badge = L"BETTING MODE: MONEY BET";
        st.badge_col = RGB(255, 218, 108);
        st.badge_bg = RGB(48, 34, 14);
        if (player_cash >= 0) {
            st.detail_line1 = detail::utf8_to_wide("Your In-Game Cash: " + std::to_string(player_cash) + " Cash");
            st.detail_line2 = L"You cannot bet more Cash than you currently hold. Stakes are added or deducted live.";
            st.btns[0].enabled = (player_cash >= b0);
            st.btns[1].enabled = (player_cash >= b1);
            st.btns[2].enabled = (player_cash >= b2);
            st.btns[0].sub = detail::utf8_to_wide(st.btns[0].enabled ? (std::to_string(b0) + " Cash") : ("Need " + std::to_string(b0)));
            st.btns[1].sub = detail::utf8_to_wide(st.btns[1].enabled ? (std::to_string(b1) + " Cash") : ("Need " + std::to_string(b1)));
            st.btns[2].sub = detail::utf8_to_wide(st.btns[2].enabled ? (std::to_string(b2) + " Cash") : ("Need " + std::to_string(b2)));
            if (player_cash < b0) {
                st.warning_line = detail::utf8_to_wide("Not enough Cash! You need at least " + std::to_string(b0) + " Cash to play this Money Bet.");
            }
        } else {
            st.detail_line1 = L"Stakes scale by difficulty (Win +Cash / Lose -Cash)";
            st.detail_line2 = L"Select a difficulty to begin Judgment Ring Duel.";
            st.btns[0].sub = detail::utf8_to_wide(std::to_string(b0) + " Cash");
            st.btns[1].sub = detail::utf8_to_wide(std::to_string(b1) + " Cash");
            st.btns[2].sub = detail::utf8_to_wide(std::to_string(b2) + " Cash");
        }
    } else if (npc.wager_type == card_game::WagerType::Card) {
        ante_id = detail::pick_ante_card(col, npc.wager_card_id);
        const auto *ndef = cards::find_card_def(npc.wager_card_id);
        const auto *adef = cards::find_card_def(ante_id);
        st.mode_badge = L"BETTING MODE: CARD BET";
        st.badge_col = RGB(255, 132, 164);
        st.badge_bg = RGB(52, 16, 28);
        st.detail_line1 = detail::utf8_to_wide("Win: [" + std::string(ndef ? cards::rarity_name(ndef->rarity) : "Card") + "] " +
                                               (ndef ? ndef->name : npc.wager_card_id));
        if (ante_id.empty()) {
            st.detail_line2 = L"Earn your first cards via RPG Story & Achievements to place a Card Bet.";
            st.warning_line = L"You don't own any cards to wager yet!";
            st.btns[0].enabled = st.btns[1].enabled = st.btns[2].enabled = false;
            st.btns[0].sub = st.btns[1].sub = st.btns[2].sub = L"No Cards";
        } else {
            st.detail_line2 = detail::utf8_to_wide("Your Ante (Lost if Defeated!): " + (adef ? adef->name : ante_id));
            st.btns[0].sub = L"Card Bet";
            st.btns[1].sub = L"Card Bet";
            st.btns[2].sub = L"Card Bet";
        }
    } else {
        st.mode_badge = L"BETTING MODE: FRIENDLY MATCH (NO BET)";
        st.badge_col = RGB(164, 224, 255);
        st.badge_bg = RGB(20, 32, 52);
        st.detail_line1 = L"No Money or Cards are wagered in this match.";
        st.detail_line2 = L"Choose a difficulty below to start Judgment Ring Duel.";
        st.btns[0].sub = L"No Wager";
        st.btns[1].sub = L"No Wager";
        st.btns[2].sub = L"No Wager";
    }

    const int diff = detail::show_gothic_challenge_modal(st);
    if (diff < 0 || diff > 2) return out;

    // Launch full 3D Judgment Ring Duel in Shadow Hearts Nocturne.exe
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

    if (winner < 0) return out;
    out.played = true;
    out.winner = winner;
    out.difficulty = diff;
    out.ante_card_id = ante_id;

    if (npc.wager_type == card_game::WagerType::Money) {
        const int stake = card_game::scaled_money_wager(npc.wager_money, static_cast<card_game::AiDifficulty>(diff));
        if (winner == 0) {
            out.money_delta = stake;
            out.summary = "MONEY BET WON vs " + npc.name + "! +" + std::to_string(stake) + " Cash";
        } else if (winner == 1) {
            out.money_delta = -stake;
            out.summary = "MONEY BET LOST vs " + npc.name + "! -" + std::to_string(stake) + " Cash";
        } else {
            out.summary = "Draw vs " + npc.name + " — Cash bet returned.";
        }
    } else if (npc.wager_type == card_game::WagerType::Card) {
        const auto *ndef = cards::find_card_def(npc.wager_card_id);
        const auto *adef = cards::find_card_def(ante_id);
        if (winner == 0) {
            out.summary = "CARD BET WON! Claimed " + (ndef ? ndef->name : npc.wager_card_id) + " from " + npc.name + "!";
        } else if (winner == 1) {
            out.summary = "CARD BET LOST! Forfeited " + (adef ? adef->name : ante_id) + " to " + npc.name + ".";
        } else {
            out.summary = "Draw vs " + npc.name + " — Both cards returned.";
        }
    } else {
        out.summary = (winner == 0 ? "Friendly Victory vs " : (winner == 1 ? "Friendly Defeat vs " : "Friendly Draw vs ")) +
                      npc.name + " (No Bet).";
    }
#else
    (void)nocturne_root_in; (void)npc; (void)col; (void)player_cash;
#endif
    return out;
}

}  // namespace npc_duel
