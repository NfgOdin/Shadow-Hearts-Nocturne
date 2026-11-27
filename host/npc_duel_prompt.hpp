#pragma once

// Shadow Hearts: Nocturne — Controller-Friendly Gothic In-Game NPC Challenge Prompt & Bridge
// Shared by both Koudelka (host/psx_main.cpp) and SH1/SH2/SH3 (host/window.cpp).
// Features:
//   - Custom-painted Shadow Hearts obsidian-crimson & antique-gold modal (zero white Windows popups)
//   - Full Controller (XInput + WinMM DualSense/DS4), Keyboard, and Mouse support
//   - Explicit BETTING MODE display: Friendly Match / Money Bet / Card Bet
//   - Custom Money Bet amount (clamped 0 .. live in-game Cash):
//       * Easy:   No Betting Allowed (0 Cash)
//       * Normal: +0% Bonus  (Win: get bet back + 100% profit; Lose: lose bet)
//       * Hard:   +75% Bonus (Win: get bet back + 175% profit; Lose: lose bet)
//   - True Triple Triad Card Bet: Winner picks 1 card from Loser's match hand!

#include "card_album.hpp"
#include "card_game.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace npc_duel {

struct ChallengeOutcome {
    bool played = false;
    int winner = -1;      // 0 = Player, 1 = NPC, 2 = Draw
    int difficulty = 1;
    int custom_bet = 0;   // Exact Cash wagered
    int money_delta = 0;  // +profit on win (+100% Normal / +175% Hard), -bet on loss
    std::string claimed_card_id;
    std::string lost_card_id;
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

// Bitmask for universal controller polling in the Gothic prompt modal (SDL2 / XInput / WinMM DualSense)
enum ModalGpBit : std::uint32_t {
    MGP_UP    = 1u << 0,
    MGP_DOWN  = 1u << 1,
    MGP_LEFT  = 1u << 2,
    MGP_RIGHT = 1u << 3,
    MGP_A     = 1u << 4,
    MGP_B     = 1u << 5,
    MGP_Y     = 1u << 6,
    MGP_LB    = 1u << 7,
    MGP_RB    = 1u << 8,
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

struct XInputGamepadRaw {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct XInputStateRaw {
    DWORD dwPacketNumber;
    XInputGamepadRaw Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XInputStateRaw *);

inline XInputGetStateFn resolve_xinput() {
    static bool tried = false;
    static XInputGetStateFn fn = nullptr;
    if (!tried) {
        tried = true;
        for (const wchar_t *dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            if (HMODULE mod = LoadLibraryW(dll)) {
                fn = reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"));
                if (fn) break;
            }
        }
    }
    return fn;
}

inline std::uint32_t poll_modal_gamepad_mask() {
    if (auto fn = resolve_xinput()) {
        for (DWORD idx = 0; idx < 4; ++idx) {
            XInputStateRaw st{};
            if (fn(idx, &st) == ERROR_SUCCESS) {
                std::uint32_t m = 0;
                const WORD b = st.Gamepad.wButtons;
                if ((b & 0x0001) || st.Gamepad.sThumbLY >  16000) m |= MGP_UP;
                if ((b & 0x0002) || st.Gamepad.sThumbLY < -16000) m |= MGP_DOWN;
                if ((b & 0x0004) || st.Gamepad.sThumbLX < -16000) m |= MGP_LEFT;
                if ((b & 0x0008) || st.Gamepad.sThumbLX >  16000) m |= MGP_RIGHT;
                if (b & 0x1000) m |= MGP_A;
                if (b & 0x2000) m |= MGP_B;
                if (b & 0x8000) m |= MGP_Y;
                if (b & 0x0100) m |= MGP_LB;
                if (b & 0x0200) m |= MGP_RB;
                return m;
            }
        }
    }
    // Fallback: WinMM HID scan across all 16 joystick slots (PS5 DualSense, PS4 DS4, Switch, DirectInput)
    for (UINT id = 0; id < 16; ++id) {
        JOYINFOEX ji{};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &ji) == JOYERR_NOERROR) {
            std::uint32_t m = 0;
            bool is_sony = false;
            JOYCAPSW caps{};
            if (joyGetDevCapsW(id, &caps, sizeof(caps)) == JOYERR_NOERROR) {
                is_sony = (caps.wMid == 0x054Cu);
            }
            if (ji.dwPOV != 65535u && ji.dwPOV <= 36000u) {
                if (ji.dwPOV <= 4500u || ji.dwPOV >= 31500u) m |= MGP_UP;
                if (ji.dwPOV >= 4500u && ji.dwPOV <= 13500u) m |= MGP_RIGHT;
                if (ji.dwPOV >= 13500u && ji.dwPOV <= 22500u) m |= MGP_DOWN;
                if (ji.dwPOV >= 22500u && ji.dwPOV <= 31500u) m |= MGP_LEFT;
            }
            if (ji.dwYpos < 18000u) m |= MGP_UP;
            if (ji.dwYpos > 47500u) m |= MGP_DOWN;
            if (ji.dwXpos < 18000u) m |= MGP_LEFT;
            if (ji.dwXpos > 47500u) m |= MGP_RIGHT;
            const DWORD btn = ji.dwButtons;
            if (is_sony) {
                if (btn & (1u << 1)) m |= MGP_A;   // Cross
                if (btn & (1u << 2)) m |= MGP_B;   // Circle
                if (btn & (1u << 3)) m |= MGP_Y;   // Triangle
                if (btn & (1u << 4)) m |= MGP_LB;  // L1
                if (btn & (1u << 5)) m |= MGP_RB;  // R1
            } else {
                if (btn & (1u << 0)) m |= MGP_A;
                if (btn & (1u << 1)) m |= MGP_B;
                if (btn & (1u << 3)) m |= MGP_Y;
                if (btn & (1u << 4)) m |= MGP_LB;
                if (btn & (1u << 5)) m |= MGP_RB;
            }
            return m;
        }
    }
    return 0;
}

struct GothicPromptState {
    std::wstring title;
    std::wstring subtitle;
    std::wstring mode_badge;
    COLORREF badge_col = RGB(164, 214, 255);
    COLORREF badge_bg = RGB(22, 32, 54);

    card_game::WagerType wager_type = card_game::WagerType::None;
    int player_cash = -1;       // Live RPG Cash (-1 if N/A)
    int max_bet = 0;            // Clamped max cash bettable
    int custom_bet = 0;         // Current player-chosen bet
    int difficulty = 1;         // 0=Easy, 1=Normal, 2=Hard
    bool card_bet_blocked = false;
    std::wstring card_detail1;
    std::wstring card_detail2;

    // Controller / Keyboard focus:
    //   focus_row == 0 -> Difficulty selector (Easy / Normal / Hard)
    //   focus_row == 1 -> Custom Bet Adjuster (only when Money Bet && difficulty > 0)
    //   focus_row == 2 -> Action row (Start Duel / Decline)
    int focus_row = 0;
    int action_col = 0;  // 0 = Start Duel, 1 = Decline

    // Hitboxes populated during draw
    RECT rc_diff[3]{};
    RECT rc_bet_btn[5]{};  // -250, -50, +50, +250, MAX
    RECT rc_start{};
    RECT rc_decline{};
    int hovered_id = -1;  // 0..2=diff, 10..14=bet_btns, 20=start, 21=decline

    std::function<std::uint32_t()> custom_gp_poll;
    std::uint32_t prev_gp = 0;
    int hold_ticks = 0;
    bool accepted = false;
    bool done = false;

    std::uint32_t sample_gp() const {
        std::uint32_t m = custom_gp_poll ? custom_gp_poll() : 0u;
        if (m == 0u) m = poll_modal_gamepad_mask();
        return m;
    }

    bool bet_row_active() const {
        return wager_type == card_game::WagerType::Money && difficulty > 0 && max_bet > 0;
    }
    int effective_bet() const {
        if (wager_type != card_game::WagerType::Money || difficulty == 0) return 0;
        return std::clamp(custom_bet, 0, max_bet);
    }
    bool can_start() const {
        if (wager_type == card_game::WagerType::Card && card_bet_blocked) return false;
        return true;
    }
    void adjust_bet(int delta) {
        if (!bet_row_active()) return;
        custom_bet = std::clamp(custom_bet + delta, 0, max_bet);
    }
};

inline void draw_gothic_prompt(HWND hwnd, HDC hdc, GothicPromptState *st) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    // 1. Deep Shadow Hearts Gothic Obsidian-Crimson Gradient Backdrop
    for (int y = 0; y < H; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(std::max(H - 1, 1));
        const int r = static_cast<int>(28.0f * (1.0f - t) + 10.0f * t);
        const int g = static_cast<int>(14.0f * (1.0f - t) + 8.0f * t);
        const int b = static_cast<int>(26.0f * (1.0f - t) + 18.0f * t);
        RECT row{0, y, W, y + 1};
        HBRUSH br = CreateSolidBrush(RGB(r, g, b));
        FillRect(mem, &row, br);
        DeleteObject(br);
    }

    {
        RECT band{2, 2, W - 2, 66};
        HBRUSH br = CreateSolidBrush(RGB(44, 14, 24));
        FillRect(mem, &band, br);
        DeleteObject(br);
    }

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

    // Header
    SelectObject(mem, f_title);
    SetTextColor(mem, RGB(248, 220, 146));
    RECT r_title{20, 12, W - 20, 42};
    DrawTextW(mem, st->title.c_str(), -1, &r_title, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(mem, f_sub);
    SetTextColor(mem, RGB(196, 182, 168));
    RECT r_sub{20, 42, W - 20, 62};
    DrawTextW(mem, st->subtitle.c_str(), -1, &r_sub, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // Betting Mode Banner
    RECT r_mode{28, 78, W - 28, 112};
    {
        HBRUSH br = CreateSolidBrush(st->badge_bg);
        FillRect(mem, &r_mode, br);
        DeleteObject(br);
        draw_rect_pen(r_mode.left, r_mode.top, r_mode.right, r_mode.bottom, st->badge_col, 1);
        SelectObject(mem, f_badge);
        SetTextColor(mem, st->badge_col);
        DrawTextW(mem, st->mode_badge.c_str(), -1, &r_mode, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // Row 0: Difficulty Selector (Easy / Normal / Hard)
    const int d_y = 124;
    const int d_bw = 160, d_bh = 52, d_gap = 12;
    const int d_x0 = (W - (d_bw * 3 + d_gap * 2)) / 2;
    const wchar_t *d_names[3] = {L"Easy", L"Normal", L"Hard"};
    const wchar_t *d_subs[3]  = {
        st->wager_type == card_game::WagerType::Money ? L"No Betting (0 Cash)" : L"Relaxed AI",
        st->wager_type == card_game::WagerType::Money ? L"+0% Bonus (1x Profit)" : L"Balanced AI",
        st->wager_type == card_game::WagerType::Money ? L"+75% Bonus (1.75x Profit)" : L"Ruthless AI"
    };
    if (st->focus_row == 0) {
        draw_rect_pen(d_x0 - 6, d_y - 5, d_x0 + d_bw * 3 + d_gap * 2 + 6, d_y + d_bh + 5, RGB(248, 212, 112), 1);
    }
    for (int i = 0; i < 3; ++i) {
        RECT br_rc{d_x0 + i * (d_bw + d_gap), d_y, d_x0 + i * (d_bw + d_gap) + d_bw, d_y + d_bh};
        st->rc_diff[i] = br_rc;
        const bool sel = (st->difficulty == i);
        const bool hov = (st->hovered_id == i);
        COLORREF bg = sel ? RGB(104, 26, 44) : (hov ? RGB(58, 24, 42) : RGB(32, 18, 34));
        COLORREF bdr = sel ? RGB(255, 218, 116) : (hov ? RGB(204, 162, 86) : RGB(114, 88, 54));
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(mem, &br_rc, br);
        DeleteObject(br);
        draw_rect_pen(br_rc.left, br_rc.top, br_rc.right, br_rc.bottom, bdr, sel ? 2 : 1);

        SelectObject(mem, f_badge);
        SetTextColor(mem, sel ? RGB(255, 248, 224) : RGB(214, 198, 174));
        RECT tr1{br_rc.left + 4, br_rc.top + 5, br_rc.right - 4, br_rc.top + 27};
        DrawTextW(mem, d_names[i], -1, &tr1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        SelectObject(mem, f_small);
        SetTextColor(mem, sel ? RGB(252, 216, 128) : RGB(166, 152, 146));
        RECT tr2{br_rc.left + 4, br_rc.top + 27, br_rc.right - 4, br_rc.bottom - 4};
        DrawTextW(mem, d_subs[i], -1, &tr2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // Middle Section: Money Bet Custom Amount OR Card Bet Details OR Friendly Details
    const int mid_y = 190;
    if (st->wager_type == card_game::WagerType::Money) {
        const int eff = st->effective_bet();
        const int profit = card_game::money_bet_win_profit(eff, static_cast<card_game::AiDifficulty>(st->difficulty));
        const int total_ret = eff + profit;

        SelectObject(mem, f_body);
        SetTextColor(mem, RGB(248, 226, 166));
        std::wstring wallet_txt = (st->player_cash >= 0)
            ? detail::utf8_to_wide("Your In-Game Cash: " + std::to_string(st->player_cash) + " Cash")
            : L"Custom Cash Wager";
        RECT r_w{28, mid_y, W - 28, mid_y + 22};
        DrawTextW(mem, wallet_txt.c_str(), -1, &r_w, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        if (st->difficulty == 0) {
            SelectObject(mem, f_body);
            SetTextColor(mem, RGB(170, 206, 244));
            RECT r_nb{28, mid_y + 34, W - 28, mid_y + 64};
            DrawTextW(mem, L"Betting is disabled on Easy (0 Cash). Choose Normal or Hard to place a Cash Bet!",
                      -1, &r_nb, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            for (int i = 0; i < 5; ++i) st->rc_bet_btn[i] = {0, 0, 0, 0};
        } else if (st->max_bet <= 0) {
            SelectObject(mem, f_body);
            SetTextColor(mem, RGB(255, 118, 126));
            RECT r_nb{28, mid_y + 34, W - 28, mid_y + 64};
            DrawTextW(mem, L"You have 0 Cash — match will be played with 0 Cash bet.",
                      -1, &r_nb, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            for (int i = 0; i < 5; ++i) st->rc_bet_btn[i] = {0, 0, 0, 0};
        } else {
            // Custom Bet Adjuster Row: [-250] [-50]  [  500 Cash  ]  [+50] [+250] [MAX]
            const int by = mid_y + 28;
            if (st->focus_row == 1) {
                draw_rect_pen(24, by - 5, W - 24, by + 39, RGB(248, 212, 112), 1);
            }
            const wchar_t *blbls[5] = {L"-250", L"-50", L"+50", L"+250", L"MAX"};
            const int bws[5] = {56, 52, 52, 56, 54};
            const int bxs[5] = {32, 94, 344, 402, 464};
            for (int i = 0; i < 5; ++i) {
                RECT brc{bxs[i], by, bxs[i] + bws[i], by + 34};
                st->rc_bet_btn[i] = brc;
                const bool hov = (st->hovered_id == 10 + i);
                HBRUSH br = CreateSolidBrush(hov ? RGB(86, 30, 48) : RGB(40, 22, 42));
                FillRect(mem, &brc, br);
                DeleteObject(br);
                draw_rect_pen(brc.left, brc.top, brc.right, brc.bottom, hov ? RGB(252, 214, 118) : RGB(154, 122, 68), 1);
                SelectObject(mem, f_small);
                SetTextColor(mem, RGB(248, 232, 186));
                DrawTextW(mem, blbls[i], -1, &brc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
            // Center Bet Amount Box
            RECT r_amt{154, by, 336, by + 34};
            {
                HBRUSH br = CreateSolidBrush(RGB(18, 12, 28));
                FillRect(mem, &r_amt, br);
                DeleteObject(br);
                draw_rect_pen(r_amt.left, r_amt.top, r_amt.right, r_amt.bottom, RGB(255, 214, 110), 2);
                SelectObject(mem, f_badge);
                SetTextColor(mem, RGB(255, 236, 162));
                const std::wstring atxt = detail::utf8_to_wide("BET: " + std::to_string(eff) + " Cash");
                DrawTextW(mem, atxt.c_str(), -1, &r_amt, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
            // Live Payout Breakdown
            SelectObject(mem, f_small);
            SetTextColor(mem, RGB(182, 238, 188));
            const std::wstring ptxt = detail::utf8_to_wide(
                "WIN: Get " + std::to_string(total_ret) + " Cash back (+" + std::to_string(profit) +
                " profit" + (st->difficulty == 2 ? ", +75% Hard Bonus!" : ", +0% Normal") +
                ")   •   LOSE: -" + std::to_string(eff) + " Cash");
            RECT r_pay{20, by + 40, W - 20, by + 62};
            DrawTextW(mem, ptxt.c_str(), -1, &r_pay, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
    } else if (st->wager_type == card_game::WagerType::Card) {
        SelectObject(mem, f_body);
        SetTextColor(mem, RGB(255, 224, 196));
        RECT r1{24, mid_y + 6, W - 24, mid_y + 32};
        DrawTextW(mem, st->card_detail1.c_str(), -1, &r1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        SelectObject(mem, f_small);
        SetTextColor(mem, st->card_bet_blocked ? RGB(255, 118, 126) : RGB(214, 198, 224));
        RECT r2{24, mid_y + 34, W - 24, mid_y + 62};
        DrawTextW(mem, st->card_detail2.c_str(), -1, &r2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    } else {
        SelectObject(mem, f_body);
        SetTextColor(mem, RGB(216, 230, 248));
        RECT r1{24, mid_y + 12, W - 24, mid_y + 38};
        DrawTextW(mem, L"Friendly Match — No Money or Cards are wagered.", -1, &r1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(mem, f_small);
        SetTextColor(mem, RGB(178, 174, 196));
        RECT r2{24, mid_y + 40, W - 24, mid_y + 64};
        DrawTextW(mem, L"Choose a difficulty above and press Start Duel to begin!", -1, &r2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // Row 2: Start Duel & Decline Action Buttons
    const int ay = H - 80;
    st->rc_start   = {W / 2 - 206, ay, W / 2 - 10, ay + 42};
    st->rc_decline = {W / 2 + 10,  ay, W / 2 + 206, ay + 42};
    if (st->focus_row == 2) {
        RECT fbox = (st->action_col == 0) ? st->rc_start : st->rc_decline;
        draw_rect_pen(fbox.left - 4, fbox.top - 4, fbox.right + 4, fbox.bottom + 4, RGB(255, 222, 124), 2);
    }

    auto draw_action_btn = [&](const RECT &brc, const wchar_t *lbl, bool enabled, bool primary, bool hov) {
        COLORREF bg = !enabled ? RGB(24, 20, 28)
                    : (primary ? (hov ? RGB(154, 34, 56) : RGB(114, 24, 42))
                               : (hov ? RGB(58, 28, 44) : RGB(32, 20, 34)));
        COLORREF bdr = !enabled ? RGB(72, 64, 78) : (hov ? RGB(255, 222, 128) : RGB(196, 156, 84));
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(mem, &brc, br);
        DeleteObject(br);
        draw_rect_pen(brc.left, brc.top, brc.right, brc.bottom, bdr, primary ? 2 : 1);
        SelectObject(mem, f_badge);
        SetTextColor(mem, enabled ? RGB(255, 246, 224) : RGB(118, 108, 124));
        RECT tr = brc;
        DrawTextW(mem, lbl, -1, &tr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    };
    draw_action_btn(st->rc_start, L"Start Duel  [A / Enter]", st->can_start(), true,
                    st->hovered_id == 20 || (st->focus_row == 2 && st->action_col == 0));
    draw_action_btn(st->rc_decline, L"Decline  [B / Esc]", true, false,
                    st->hovered_id == 21 || (st->focus_row == 2 && st->action_col == 1));

    // Bottom Controller & Keyboard Legend
    SelectObject(mem, f_small);
    SetTextColor(mem, RGB(156, 146, 168));
    RECT r_hint{16, H - 26, W - 16, H - 8};
    DrawTextW(mem, L"[D-Pad] Select / Adjust Bet   •   [LB/RB] ±250   •   [Y] Max Bet   •   [A/Enter] Start   •   [B/Esc] Back",
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
        SetTimer(hwnd, 1, 16, nullptr);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (st) draw_gothic_prompt(hwnd, hdc, st);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER: {
        if (!st || st->done) return 0;
        const std::uint32_t m = st->sample_gp();
        const std::uint32_t p = m & ~st->prev_gp;
        const std::uint32_t held_rep = m & (MGP_LEFT | MGP_RIGHT | MGP_LB | MGP_RB);
        std::uint32_t trig = p;
        if (held_rep != 0u && (held_rep == (st->prev_gp & (MGP_LEFT | MGP_RIGHT | MGP_LB | MGP_RB)))) {
            ++st->hold_ticks;
            if (st->hold_ticks >= 18 && ((st->hold_ticks - 18) % 4) == 0) {
                trig |= held_rep;  // Smooth hold-to-repeat for adjusting custom bets!
            }
        } else {
            st->hold_ticks = 0;
        }
        st->prev_gp = m;
        if (trig == 0) return 0;

        bool dirty = false;
        if (p & MGP_B) {
            st->accepted = false; st->done = true; DestroyWindow(hwnd); return 0;
        }
        if (p & MGP_UP) {
            if (st->focus_row == 2) st->focus_row = st->bet_row_active() ? 1 : 0;
            else if (st->focus_row == 1) st->focus_row = 0;
            dirty = true;
        }
        if (p & MGP_DOWN) {
            if (st->focus_row == 0) st->focus_row = st->bet_row_active() ? 1 : 2;
            else if (st->focus_row == 1) st->focus_row = 2;
            dirty = true;
        }
        if (trig & MGP_LEFT) {
            if (st->focus_row == 0 && (p & MGP_LEFT)) {
                st->difficulty = std::max(0, st->difficulty - 1);
            } else if (st->focus_row == 1) {
                st->adjust_bet(-50);
            } else if (p & MGP_LEFT) {
                st->action_col = 0;
            }
            dirty = true;
        }
        if (trig & MGP_RIGHT) {
            if (st->focus_row == 0 && (p & MGP_RIGHT)) {
                st->difficulty = std::min(2, st->difficulty + 1);
            } else if (st->focus_row == 1) {
                st->adjust_bet(+50);
            } else if (p & MGP_RIGHT) {
                st->action_col = 1;
            }
            dirty = true;
        }
        if (trig & MGP_LB) { st->adjust_bet(-250); dirty = true; }
        if (trig & MGP_RB) { st->adjust_bet(+250); dirty = true; }
        if (p & MGP_Y)  { if (st->bet_row_active()) { st->custom_bet = st->max_bet; dirty = true; } }
        if (p & MGP_A) {
            if (st->focus_row == 2 && st->action_col == 1) {
                st->accepted = false; st->done = true; DestroyWindow(hwnd); return 0;
            }
            if (st->can_start()) {
                st->accepted = true; st->done = true; DestroyWindow(hwnd); return 0;
            }
        }
        if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!st) return 0;
        const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        int nh = -1;
        for (int i = 0; i < 3; ++i) if (PtInRect(&st->rc_diff[i], pt)) nh = i;
        if (nh < 0 && st->bet_row_active()) {
            for (int i = 0; i < 5; ++i) if (PtInRect(&st->rc_bet_btn[i], pt)) nh = 10 + i;
        }
        if (nh < 0 && PtInRect(&st->rc_start, pt)) nh = 20;
        if (nh < 0 && PtInRect(&st->rc_decline, pt)) nh = 21;
        if (nh != st->hovered_id) {
            st->hovered_id = nh;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!st) return 0;
        const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        for (int i = 0; i < 3; ++i) {
            if (PtInRect(&st->rc_diff[i], pt)) {
                st->difficulty = i;
                st->focus_row = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        if (st->bet_row_active()) {
            const int deltas[5] = {-250, -50, +50, +250, 0};
            for (int i = 0; i < 5; ++i) {
                if (PtInRect(&st->rc_bet_btn[i], pt)) {
                    if (i == 4) st->custom_bet = st->max_bet;
                    else st->adjust_bet(deltas[i]);
                    st->focus_row = 1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }
        if (PtInRect(&st->rc_start, pt) && st->can_start()) {
            st->accepted = true; st->done = true; DestroyWindow(hwnd); return 0;
        }
        if (PtInRect(&st->rc_decline, pt)) {
            st->accepted = false; st->done = true; DestroyWindow(hwnd); return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (!st) return 0;
        if (wp == VK_ESCAPE) {
            st->accepted = false; st->done = true; DestroyWindow(hwnd);
        } else if (wp == VK_RETURN) {
            if (st->focus_row == 2 && st->action_col == 1) {
                st->accepted = false; st->done = true; DestroyWindow(hwnd);
            } else if (st->can_start()) {
                st->accepted = true; st->done = true; DestroyWindow(hwnd);
            }
        } else if (wp == VK_UP) {
            if (st->focus_row == 2) st->focus_row = st->bet_row_active() ? 1 : 0;
            else if (st->focus_row == 1) st->focus_row = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_DOWN) {
            if (st->focus_row == 0) st->focus_row = st->bet_row_active() ? 1 : 2;
            else if (st->focus_row == 1) st->focus_row = 2;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_LEFT) {
            if (st->focus_row == 0) st->difficulty = std::max(0, st->difficulty - 1);
            else if (st->focus_row == 1) st->adjust_bet(-50);
            else st->action_col = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_RIGHT) {
            if (st->focus_row == 0) st->difficulty = std::min(2, st->difficulty + 1);
            else if (st->focus_row == 1) st->adjust_bet(+50);
            else st->action_col = 1;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_BACK && st->bet_row_active()) {
            st->custom_bet /= 10;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp >= '0' && wp <= '9' && st->bet_row_active()) {
            const int d = static_cast<int>(wp - '0');
            const std::int64_t next = static_cast<std::int64_t>(st->custom_bet) * 10 + d;
            st->custom_bet = static_cast<int>(std::min<std::int64_t>(next, st->max_bet));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CLOSE:
        if (st) { st->accepted = false; st->done = true; }
        DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

inline bool show_gothic_challenge_modal(GothicPromptState &st) {
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

    const int W = 550, H = 376;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    // Snapshot held controller buttons so pressing Square/A to open doesn't auto-confirm!
    st.prev_gp = st.sample_gp();

    HWND dlg = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"NocturneGothicDuelPromptWnd",
        L"Judgment Ring Duel",
        WS_POPUP | WS_VISIBLE,
        sx, sy, W, H, nullptr, nullptr, hi, &st);
    if (!dlg) return false;

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);
    SetFocus(dlg);

    MSG m{};
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return st.accepted;
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

// Interactive In-Game Square Challenge Flow (Controller-Friendly Gothic Shadow Hearts Modal):
inline ChallengeOutcome prompt_and_run_challenge(const std::filesystem::path &nocturne_root_in,
                                                 const card_game::NpcChallenger &npc,
                                                 cards::Collection &col,
                                                 int player_cash = -1,
                                                 std::function<std::uint32_t()> custom_gp_poll = {}) {
    ChallengeOutcome out{};
#ifdef _WIN32
    const std::filesystem::path nocturne_root = find_nocturne_root(nocturne_root_in);
    col.reload_if_changed();

    detail::GothicPromptState st{};
    st.custom_gp_poll = std::move(custom_gp_poll);
    st.title = detail::utf8_to_wide("CHALLENGE " + npc.name + "?");
    st.subtitle = detail::utf8_to_wide("Judgment Ring Duel  •  " + npc.location);
    st.wager_type = npc.wager_type;
    st.player_cash = player_cash;
    st.difficulty = 1;  // Default Normal

    std::string ante_id;
    if (npc.wager_type == card_game::WagerType::Money) {
        st.mode_badge = L"BETTING MODE: MONEY BET (CUSTOM AMOUNT)";
        st.badge_col = RGB(255, 218, 108);
        st.badge_bg = RGB(48, 34, 14);
        st.max_bet = (player_cash >= 0) ? player_cash : std::max(npc.wager_money, 500);
        st.custom_bet = std::clamp(npc.wager_money > 0 ? npc.wager_money : 100, 0, st.max_bet);
    } else if (npc.wager_type == card_game::WagerType::Card) {
        ante_id = detail::pick_ante_card(col, npc.wager_card_id);
        st.mode_badge = L"BETTING MODE: CARD BET (TRIPLE TRIAD)";
        st.badge_col = RGB(255, 132, 164);
        st.badge_bg = RGB(52, 16, 28);
        if (ante_id.empty()) {
            st.card_bet_blocked = true;
            st.card_detail1 = L"You don't own any cards to wager yet!";
            st.card_detail2 = L"Earn cards via RPG Story & Achievements first, then challenge again.";
        } else {
            st.card_detail1 = L"WIN: Choose 1 card directly from your opponent's match hand!";
            st.card_detail2 = L"LOSE: Opponent chooses 1 owned card from your match hand!";
        }
    } else {
        st.mode_badge = L"BETTING MODE: FRIENDLY MATCH (NO BET)";
        st.badge_col = RGB(164, 224, 255);
        st.badge_bg = RGB(20, 32, 52);
    }

    if (!detail::show_gothic_challenge_modal(st)) return out;

    const int diff = std::clamp(st.difficulty, 0, 2);
    const int chosen_bet = st.effective_bet();

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
                       std::to_wstring(diff) + L" --bet " + std::to_wstring(chosen_bet) +
                       L" --result-file \"" + res_file.wstring() + L"\"";
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
            } else if (line.rfind("claimed_card=", 0) == 0) {
                out.claimed_card_id = line.substr(13);
            } else if (line.rfind("lost_card=", 0) == 0) {
                out.lost_card_id = line.substr(10);
            }
        }
    }
    std::filesystem::remove(res_file, ec);

    if (winner < 0) return out;
    out.played = true;
    out.winner = winner;
    out.difficulty = diff;
    out.custom_bet = chosen_bet;

    if (npc.wager_type == card_game::WagerType::Money && chosen_bet > 0) {
        const int profit = card_game::money_bet_win_profit(chosen_bet, static_cast<card_game::AiDifficulty>(diff));
        if (winner == 0) {
            out.money_delta = profit;
            out.summary = "MONEY BET WON vs " + npc.name + "! +" + std::to_string(profit) + " Cash profit" +
                          (diff == 2 ? " (+75% Hard Bonus!)" : "");
        } else if (winner == 1) {
            out.money_delta = -chosen_bet;
            out.summary = "MONEY BET LOST vs " + npc.name + "! -" + std::to_string(chosen_bet) + " Cash";
        } else {
            out.summary = "Draw vs " + npc.name + " — Your " + std::to_string(chosen_bet) + " Cash bet returned.";
        }
    } else if (npc.wager_type == card_game::WagerType::Card) {
        if (winner == 0 && !out.claimed_card_id.empty()) {
            const auto *cdef = cards::find_card_def(out.claimed_card_id);
            out.summary = "CARD BET WON! Claimed " + (cdef ? cdef->name : out.claimed_card_id) + " from " + npc.name + "'s hand!";
        } else if (winner == 1 && !out.lost_card_id.empty()) {
            const auto *ldef = cards::find_card_def(out.lost_card_id);
            out.summary = "CARD BET LOST! " + npc.name + " took " + (ldef ? ldef->name : out.lost_card_id) + " from your hand!";
        } else {
            out.summary = "Draw vs " + npc.name + " — Both hands returned.";
        }
    } else {
        out.summary = (winner == 0 ? "Friendly Victory vs " : (winner == 1 ? "Friendly Defeat vs " : "Friendly Draw vs ")) +
                      npc.name + (diff == 0 && npc.wager_type == card_game::WagerType::Money ? " (Easy — No Bet)." : " (No Bet).");
    }
#else
    (void)nocturne_root_in; (void)npc; (void)col; (void)player_cash;
#endif
    return out;
}

}  // namespace npc_duel
