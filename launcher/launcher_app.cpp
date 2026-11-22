#include "launcher_app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <dwmapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#ifdef DrawText
#undef DrawText
#endif
#endif

namespace launcher {
namespace {

UINT window_dpi(HWND hwnd) {
    if (hwnd) {
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            using GetDpiFn = UINT(WINAPI *)(HWND);
            if (auto fn = reinterpret_cast<GetDpiFn>(GetProcAddress(u32, "GetDpiForWindow"))) {
                const UINT d = fn(hwnd);
                if (d >= 96u) return d;
            }
        }
    }
    const HDC dc = GetDC(hwnd);
    const int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return d >= 96 ? static_cast<UINT>(d) : 96u;
}

constexpr UINT kTimerAnimId = 1;
constexpr UINT kWmGameExited = WM_APP + 101;
constexpr float kWinW = 1220.0f;
constexpr float kWinH = 740.0f;
constexpr float kTitlebarH = 44.0f;
constexpr float kSidebarW = 216.0f;
constexpr float kPi = 3.14159265358979323846f;

#ifdef _WIN32
// Dynamic XInput + WinMM DualShock 4 / DualSense / DirectInput poller (zero extra link dependencies)
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

XInputGetStateFn resolve_xinput_get_state() {
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

enum GpBit : std::uint32_t {
    GP_UP    = 1u << 0,
    GP_DOWN  = 1u << 1,
    GP_LEFT  = 1u << 2,
    GP_RIGHT = 1u << 3,
    GP_A     = 1u << 4,  // Cross on PlayStation
    GP_B     = 1u << 5,  // Circle on PlayStation
    GP_Y     = 1u << 6,  // Triangle on PlayStation
    GP_LB    = 1u << 7,  // L1
    GP_RB    = 1u << 8,  // R1
    GP_START = 1u << 9,  // Options
    GP_X     = 1u << 10, // Square on PlayStation
};

struct GamepadSample {
    bool connected = false;
    bool is_sony = false;
    std::uint32_t mask = 0;
};

GamepadSample sample_system_gamepad() {
    GamepadSample out;
    // 1. Prefer XInput (Xbox, XInput adapters, Steam Input, DS4Windows)
    if (XInputGetStateFn get_state = resolve_xinput_get_state()) {
        for (DWORD idx = 0; idx < 4; ++idx) {
            XInputStateRaw st{};
            if (get_state(idx, &st) == ERROR_SUCCESS) {
                out.connected = true;
                out.is_sony = false;
                const WORD b = st.Gamepad.wButtons;
                if ((b & 0x0001u) || st.Gamepad.sThumbLY > 16000) out.mask |= GP_UP;
                if ((b & 0x0002u) || st.Gamepad.sThumbLY < -16000) out.mask |= GP_DOWN;
                if ((b & 0x0004u) || st.Gamepad.sThumbLX < -16000) out.mask |= GP_LEFT;
                if ((b & 0x0008u) || st.Gamepad.sThumbLX > 16000) out.mask |= GP_RIGHT;
                if (b & 0x1000u) out.mask |= GP_A;
                if (b & 0x2000u) out.mask |= GP_B;
                if (b & 0x4000u) out.mask |= GP_X;
                if (b & 0x8000u) out.mask |= GP_Y;
                if (b & 0x0100u) out.mask |= GP_LB;
                if (b & 0x0200u) out.mask |= GP_RB;
                if (b & 0x0010u) out.mask |= GP_START;
                return out;
            }
        }
    }
    // 2. Native WinMM HID fallback (DualShock 4, DualSense, DirectInput gamepads)
    for (UINT id = 0; id < 4; ++id) {
        JOYINFOEX ji{};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &ji) == JOYERR_NOERROR) {
            out.connected = true;
            JOYCAPSW caps{};
            if (joyGetDevCapsW(id, &caps, sizeof(caps)) == JOYERR_NOERROR) {
                out.is_sony = (caps.wMid == 0x054Cu);  // Sony Interactive Entertainment (DS4 / DualSense)
            }
            // POV hat (hundredths of a degree; 65535 = centered)
            if (ji.dwPOV != 65535u && ji.dwPOV <= 36000u) {
                if (ji.dwPOV <= 4500u || ji.dwPOV >= 31500u) out.mask |= GP_UP;
                if (ji.dwPOV >= 4500u && ji.dwPOV <= 13500u) out.mask |= GP_RIGHT;
                if (ji.dwPOV >= 13500u && ji.dwPOV <= 22500u) out.mask |= GP_DOWN;
                if (ji.dwPOV >= 22500u && ji.dwPOV <= 31500u) out.mask |= GP_LEFT;
            }
            // Left stick (0..65535, center ~32768)
            if (ji.dwYpos < 18000u) out.mask |= GP_UP;
            if (ji.dwYpos > 47500u) out.mask |= GP_DOWN;
            if (ji.dwXpos < 18000u) out.mask |= GP_LEFT;
            if (ji.dwXpos > 47500u) out.mask |= GP_RIGHT;

            const DWORD btn = ji.dwButtons;
            if (out.is_sony) {
                // DS4/DualSense Windows HID: bit0=Square, bit1=Cross, bit2=Circle, bit3=Triangle, bit4=L1, bit5=R1, bit9=Options
                if (btn & (1u << 0)) out.mask |= GP_X;
                if (btn & (1u << 1)) out.mask |= GP_A;
                if (btn & (1u << 2)) out.mask |= GP_B;
                if (btn & (1u << 3)) out.mask |= GP_Y;
                if (btn & (1u << 4)) out.mask |= GP_LB;
                if (btn & (1u << 5)) out.mask |= GP_RB;
                if (btn & (1u << 9)) out.mask |= GP_START;
            } else {
                // Standard DirectInput pad: bit0=A, bit1=B, bit2=X, bit3=Y, bit4=LB, bit5=RB, bit7=Start
                if (btn & (1u << 0)) out.mask |= GP_A;
                if (btn & (1u << 1)) out.mask |= GP_B;
                if (btn & (1u << 2)) out.mask |= GP_X;
                if (btn & (1u << 3)) out.mask |= GP_Y;
                if (btn & (1u << 4)) out.mask |= GP_LB;
                if (btn & (1u << 5)) out.mask |= GP_RB;
                if (btn & (1u << 7)) out.mask |= GP_START;
            }
            return out;
        }
    }
    return out;
}
#endif

std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

D2D1_COLOR_F col_rgba(int r, int g, int b, float a = 1.0f) {
    return D2D1::ColorF(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                        static_cast<float>(b) / 255.0f, std::clamp(a, 0.0f, 1.0f));
}

D2D1_COLOR_F from_u32(std::uint32_t c, float alpha_mul = 1.0f) {
    return D2D1::ColorF(static_cast<float>(c & 255u) / 255.0f,
                        static_cast<float>((c >> 8) & 255u) / 255.0f,
                        static_cast<float>((c >> 16) & 255u) / 255.0f,
                        std::clamp((static_cast<float>((c >> 24) & 255u) / 255.0f) * alpha_mul, 0.0f, 1.0f));
}

D2D1_COLOR_F with_a(D2D1_COLOR_F c, float a) {
    c.a = std::clamp(a, 0.0f, 1.0f);
    return c;
}

D2D1_COLOR_F lerp_col(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

bool pt_in(D2D1_RECT_F r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

void draw_arc_band(ID2D1Factory *factory, ID2D1RenderTarget *rt, float cx, float cy,
                   float r_inner, float r_outer, float a0, float a1, D2D1_COLOR_F col) {
    ID2D1PathGeometry *geo = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return;
    ID2D1GeometrySink *sink = nullptr;
    if (SUCCEEDED(geo->Open(&sink)) && sink) {
        const D2D1_POINT_2F p0 = D2D1::Point2F(cx + std::cos(a0) * r_outer, cy + std::sin(a0) * r_outer);
        const D2D1_POINT_2F p1 = D2D1::Point2F(cx + std::cos(a1) * r_outer, cy + std::sin(a1) * r_outer);
        const D2D1_POINT_2F p2 = D2D1::Point2F(cx + std::cos(a1) * r_inner, cy + std::sin(a1) * r_inner);
        const D2D1_POINT_2F p3 = D2D1::Point2F(cx + std::cos(a0) * r_inner, cy + std::sin(a0) * r_inner);
        const D2D1_ARC_SIZE arc_sz = (std::fabs(a1 - a0) > kPi) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
        sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(r_outer, r_outer), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arc_sz));
        sink->AddLine(p2);
        sink->AddArc(D2D1::ArcSegment(p3, D2D1::SizeF(r_inner, r_inner), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, arc_sz));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();

        ID2D1SolidColorBrush *br = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(col, &br)) && br) {
            rt->FillGeometry(geo, br);
            br->Release();
        }
    }
    geo->Release();
}

void fill_hgrad_rounded(ID2D1RenderTarget *rt, D2D1_ROUNDED_RECT rr, D2D1_COLOR_F c0, D2D1_COLOR_F c1) {
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, c0}, {1.0f, c1}};
    ID2D1GradientStopCollection *gsc = nullptr;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &gsc)) && gsc) {
        ID2D1LinearGradientBrush *br = nullptr;
        const auto props = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(rr.rect.left, rr.rect.top), D2D1::Point2F(rr.rect.right, rr.rect.bottom));
        if (SUCCEEDED(rt->CreateLinearGradientBrush(props, gsc, &br)) && br) {
            rt->FillRoundedRectangle(rr, br);
            br->Release();
        }
        gsc->Release();
    }
}

void fill_radial_glow(ID2D1RenderTarget *rt, float cx, float cy, float rx, float ry, D2D1_COLOR_F center, D2D1_COLOR_F edge) {
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, center}, {1.0f, edge}};
    ID2D1GradientStopCollection *gsc = nullptr;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &gsc)) && gsc) {
        ID2D1RadialGradientBrush *br = nullptr;
        const auto props = D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx, ry);
        if (SUCCEEDED(rt->CreateRadialGradientBrush(props, gsc, &br)) && br) {
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), br);
            br->Release();
        }
        gsc->Release();
    }
}

}  // namespace

LauncherApp::LauncherApp(const std::filesystem::path &root_dir) {
    state_.paths.root_dir = root_dir;
    global_cfg_ = load_global_config(root_dir);
    active_game_ = global_cfg_.last_game;
    state_.game_id = active_game_;
    trilogy_summaries_ = load_trilogy_summaries(root_dir);
    refresh_all(state_);
    audio_.set_enabled(global_cfg_.chimes);
    audio_.set_volume(state_.settings.chime_volume);
    page_ = global_cfg_.auto_enter_last_game ? Page::Home : Page::Trilogy;
    init_embers();
}

LauncherApp::~LauncherApp() {
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    release_device_resources();
    const DWRITE_TRIMMING no_trim{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    for (auto &kv : trim_signs_) {
        if (kv.first) kv.first->SetTrimming(&no_trim, nullptr);
        if (kv.second) kv.second->Release();
    }
    trim_signs_.clear();
    if (fmt_hero_) fmt_hero_->Release();
    if (fmt_roman_) fmt_roman_->Release();
    if (fmt_h1_) fmt_h1_->Release();
    if (fmt_h2_) fmt_h2_->Release();
    if (fmt_card_title_) fmt_card_title_->Release();
    if (fmt_body_) fmt_body_->Release();
    if (fmt_small_) fmt_small_->Release();
    if (fmt_pill_) fmt_pill_->Release();
    if (fmt_play_) fmt_play_->Release();
    if (wic_factory_) wic_factory_->Release();
    if (dwrite_factory_) dwrite_factory_->Release();
    if (d2d_factory_) d2d_factory_->Release();
}

void LauncherApp::init_embers() {
    embers_.clear();
    embers_.reserve(28);
    std::uint32_t seed = 0x5A1915u;
    const auto rnd = [&]() -> float {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFFu) / 65535.0f;
    };
    for (int i = 0; i < 28; ++i) {
        embers_.push_back({
            rnd() * kWinW,
            rnd() * kWinH,
            (rnd() - 0.5f) * 14.0f,
            -(10.0f + rnd() * 24.0f),
            1.4f + rnd() * 2.6f,
            0.22f + rnd() * 0.45f,
            rnd() * 6.28f
        });
    }
}

bool LauncherApp::init_factories() {
    if (d2d_factory_ && dwrite_factory_ && wic_factory_) return true;
    if (!d2d_factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_))) return false;
    if (!dwrite_factory_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                       reinterpret_cast<IUnknown **>(&dwrite_factory_)))) return false;
    if (!wic_factory_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory_));
    }
    const auto make_fmt = [&](const wchar_t *face, DWRITE_FONT_WEIGHT weight, float sz, IDWriteTextFormat **out) {
        if (*out) return;
        dwrite_factory_->CreateTextFormat(face, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, sz, L"en-us", out);
        if (*out) {
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            // Create ONE trimming sign per format BEFORE SetTrimming is ever called.
            // Calling CreateEllipsisTrimmingSign repeatedly on a format that already has a
            // trimming sign chains inline objects recursively and overflows the stack on Release!
            IDWriteInlineObject *sign = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(*out, &sign)) && sign) {
                trim_signs_[*out] = sign;
            }
        }
    };
    make_fmt(L"Georgia", DWRITE_FONT_WEIGHT_BOLD, 34.0f, &fmt_hero_);
    make_fmt(L"Georgia", DWRITE_FONT_WEIGHT_BOLD, 66.0f, &fmt_roman_);
    make_fmt(L"Georgia", DWRITE_FONT_WEIGHT_BOLD, 22.0f, &fmt_h1_);
    make_fmt(L"Georgia", DWRITE_FONT_WEIGHT_BOLD, 16.5f, &fmt_h2_);
    make_fmt(L"Georgia", DWRITE_FONT_WEIGHT_BOLD, 14.5f, &fmt_card_title_);
    make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, &fmt_body_);
    make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 11.5f, &fmt_small_);
    make_fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 10.5f, &fmt_pill_);
    make_fmt(L"Georgia", DWRITE_FONT_WEIGHT_BOLD, 20.0f, &fmt_play_);
    return true;
}

void LauncherApp::clear_bitmap_caches() {
    if (solid_brush_) {
        solid_brush_->Release();
        solid_brush_ = nullptr;
    }
    for (auto &kv : image_cache_)
        if (kv.second) kv.second->Release();
    image_cache_.clear();
    for (auto &kv : slot_thumb_cache_)
        if (kv.second) kv.second->Release();
    slot_thumb_cache_.clear();
    bitmap_cache_rt_ = nullptr;
}

ID2D1SolidColorBrush *LauncherApp::solid_brush(ID2D1RenderTarget *rt, D2D1_COLOR_F col) {
    if (!rt) return nullptr;
    if (bitmap_cache_rt_ != rt) {
        clear_bitmap_caches();
        bitmap_cache_rt_ = rt;
    }
    if (!solid_brush_) {
        if (FAILED(rt->CreateSolidColorBrush(col, &solid_brush_)) || !solid_brush_) return nullptr;
    } else {
        solid_brush_->SetColor(col);
    }
    return solid_brush_;
}

void LauncherApp::release_device_resources() {
    clear_bitmap_caches();
    if (hwnd_rt_) {
        hwnd_rt_->Release();
        hwnd_rt_ = nullptr;
    }
}

bool LauncherApp::ensure_hwnd_target() {
    if (hwnd_rt_) return true;
    if (!hwnd_ || !d2d_factory_) return false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U sz = D2D1::SizeU(std::max<UINT32>(1u, static_cast<UINT32>(rc.right - rc.left)),
                                       std::max<UINT32>(1u, static_cast<UINT32>(rc.bottom - rc.top)));
    auto rt_props = D2D1::RenderTargetProperties();
    rt_props.dpiX = 96.0f;
    rt_props.dpiY = 96.0f;
    const UINT dpi = window_dpi(hwnd_);
    if (dpi >= 96u) {
        rt_props.dpiX = static_cast<float>(dpi);
        rt_props.dpiY = static_cast<float>(dpi);
    }
    return SUCCEEDED(d2d_factory_->CreateHwndRenderTarget(rt_props, D2D1::HwndRenderTargetProperties(hwnd_, sz), &hwnd_rt_)) && hwnd_rt_;
}

LauncherApp::Palette LauncherApp::palette_for_game(GameId id, int theme_override) const {
    Palette p;
    if (theme_override == 2) {  // Judgment Ring Crimson & Brass
        p.bg_top = col_rgba(18, 11, 12);
        p.bg_bot = col_rgba(38, 12, 18);
        p.sidebar_bg = col_rgba(14, 9, 10, 0.92f);
        p.card_bg = col_rgba(28, 18, 20, 0.86f);
        p.card_hover = col_rgba(44, 26, 30, 0.92f);
        p.border = col_rgba(150, 108, 58, 0.55f);
        p.border_hi = col_rgba(246, 202, 92, 0.95f);
        p.accent = col_rgba(248, 206, 98);
        p.accent_crimson = col_rgba(196, 38, 56);
    } else if (theme_override == 3) {  // 1915 Occult Grimoire
        p.bg_top = col_rgba(11, 16, 28);
        p.bg_bot = col_rgba(20, 28, 46);
        p.sidebar_bg = col_rgba(9, 13, 24, 0.92f);
        p.card_bg = col_rgba(18, 26, 42, 0.86f);
        p.card_hover = col_rgba(28, 40, 62, 0.92f);
        p.border = col_rgba(74, 148, 196, 0.48f);
        p.border_hi = col_rgba(104, 212, 248, 0.95f);
        p.accent = col_rgba(226, 198, 134);
        p.accent_crimson = col_rgba(56, 168, 214);
    } else if (theme_override == 1) {  // Soul Graveyard Eclipse
        p.bg_top = col_rgba(12, 10, 22);
        p.bg_bot = col_rgba(32, 13, 30);
        p.sidebar_bg = col_rgba(10, 9, 19, 0.92f);
        p.card_bg = col_rgba(21, 18, 34, 0.86f);
        p.card_hover = col_rgba(35, 26, 50, 0.92f);
        p.border = col_rgba(156, 122, 68, 0.50f);
        p.border_hi = col_rgba(244, 208, 118, 0.95f);
        p.accent = col_rgba(244, 208, 120);
        p.accent_crimson = col_rgba(176, 34, 62);
    } else {
        // Default per-game signature palette (Covenant keeps the exact classic Nocturne look untouched)
        switch (id) {
        case GameId::ShadowHearts1:  // 1913 Blood-Moon & Ashen-Silver Gothic Horror
            p.bg_top = col_rgba(14, 9, 12);
            p.bg_bot = col_rgba(36, 11, 17);
            p.sidebar_bg = col_rgba(11, 8, 10, 0.93f);
            p.card_bg = col_rgba(24, 15, 19, 0.88f);
            p.card_hover = col_rgba(40, 21, 28, 0.94f);
            p.border = col_rgba(148, 112, 110, 0.52f);
            p.border_hi = col_rgba(232, 196, 178, 0.95f);
            p.accent = col_rgba(232, 208, 194);
            p.accent_crimson = col_rgba(174, 28, 44);
            break;
        case GameId::FromTheNewWorld:  // 1929 Art-Deco Midnight Indigo & Antique Brass
            p.bg_top = col_rgba(9, 14, 26);
            p.bg_bot = col_rgba(16, 30, 44);
            p.sidebar_bg = col_rgba(8, 12, 22, 0.93f);
            p.card_bg = col_rgba(16, 24, 38, 0.88f);
            p.card_hover = col_rgba(24, 38, 56, 0.94f);
            p.border = col_rgba(158, 132, 74, 0.52f);
            p.border_hi = col_rgba(242, 204, 114, 0.95f);
            p.accent = col_rgba(240, 202, 116);
            p.accent_crimson = col_rgba(44, 146, 136);
            break;
        case GameId::Koudelka:  // 1898 Nemeton Monastery Candlelit Burgundy & Tarnished Gold
            p.bg_top = col_rgba(15, 9, 14);
            p.bg_bot = col_rgba(38, 13, 24);
            p.sidebar_bg = col_rgba(12, 8, 12, 0.94f);
            p.card_bg = col_rgba(26, 14, 22, 0.88f);
            p.card_hover = col_rgba(44, 22, 34, 0.94f);
            p.border = col_rgba(168, 126, 78, 0.56f);
            p.border_hi = col_rgba(246, 204, 126, 0.96f);
            p.accent = col_rgba(244, 204, 132);
            p.accent_crimson = col_rgba(154, 32, 62);
            break;
        case GameId::Covenant:
        default:  // Classic Nocturne Obsidian, Crimson & Antique Gold (untouched)
            p.bg_top = col_rgba(12, 10, 22);
            p.bg_bot = col_rgba(32, 13, 30);
            p.sidebar_bg = col_rgba(10, 9, 19, 0.92f);
            p.card_bg = col_rgba(21, 18, 34, 0.86f);
            p.card_hover = col_rgba(35, 26, 50, 0.92f);
            p.border = col_rgba(156, 122, 68, 0.50f);
            p.border_hi = col_rgba(244, 208, 118, 0.95f);
            p.accent = col_rgba(244, 208, 120);
            p.accent_crimson = col_rgba(176, 34, 62);
            break;
        }
    }
    p.text = col_rgba(244, 242, 238);
    p.text_dim = col_rgba(164, 170, 192);
    p.good = col_rgba(104, 230, 178);
    p.warn = col_rgba(248, 178, 76);
    return p;
}

LauncherApp::Palette LauncherApp::current_palette() const {
    if (page_ == Page::Trilogy) return palette_for_game(GameId::Covenant, 0);
    return palette_for_game(active_game_, state_.settings.toast_theme);
}

void LauncherApp::select_game(GameId id, bool enter_hub) {
    const bool changed = (active_game_ != id || state_.game_id != id);
    active_game_ = id;
    state_.game_id = id;
    if (changed) {
        clear_bitmap_caches();
        refresh_all(state_);
        audio_.set_volume(state_.settings.chime_volume);
        settings_scroll_ = 0.0f;
        tools_mods_scroll_ = 0.0f;
        media_scroll_ = 0.0f;
        trophy_tier_ = -1;
        trophy_status_ = 0;
        trophy_selected_ = 0;
        trophy_scroll_ = 0;
        saves_subtab_ = 0;
        tools_subtab_ = 0;
        armed_delete_slot_ = -1;
    }
    if (global_cfg_.last_game != id) {
        global_cfg_.last_game = id;
        save_global_config(state_.paths.root_dir, global_cfg_);
    }
    if (enter_hub) {
        page_ = Page::Home;
        focused_id_ = "home_play";
    }
}

void LauncherApp::return_to_trilogy() {
    trilogy_summaries_ = load_trilogy_summaries(state_.paths.root_dir);
    page_ = Page::Trilogy;
    armed_delete_slot_ = -1;
    focused_id_ = "trilogy_card_" + std::to_string(static_cast<int>(active_game_));
}

ID2D1Bitmap *LauncherApp::load_bitmap_cached(ID2D1RenderTarget *rt, const std::filesystem::path &path, int max_w, int max_h) {
    if (!rt || !wic_factory_ || path.empty()) return nullptr;
    if (bitmap_cache_rt_ != rt) {
        clear_bitmap_caches();
        bitmap_cache_rt_ = rt;
    }
    const std::string key = path.string() + "#" + std::to_string(max_w) + "x" + std::to_string(max_h);
    auto it = image_cache_.find(key);
    if (it != image_cache_.end()) return it->second;

    ID2D1Bitmap *bmp = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    IWICFormatConverter *conv = nullptr;
    if (SUCCEEDED(wic_factory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &dec)) && dec) {
        if (SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
            UINT sw = 0, sh = 0;
            if (SUCCEEDED(frame->GetSize(&sw, &sh)) && sw > 0 && sh > 0) {
                const double sc = std::min({1.0, static_cast<double>(max_w) / sw, static_cast<double>(max_h) / sh});
                const UINT dw = std::max<UINT>(1u, static_cast<UINT>(sw * sc + 0.5));
                const UINT dh = std::max<UINT>(1u, static_cast<UINT>(sh * sc + 0.5));
                if (SUCCEEDED(wic_factory_->CreateBitmapScaler(&scaler)) && scaler &&
                    SUCCEEDED(scaler->Initialize(frame, dw, dh, WICBitmapInterpolationModeFant)) &&
                    SUCCEEDED(wic_factory_->CreateFormatConverter(&conv)) && conv &&
                    SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    rt->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
                }
            }
        }
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    image_cache_[key] = bmp;
    return bmp;
}

ID2D1Bitmap *LauncherApp::load_slot_bitmap_cached(ID2D1RenderTarget *rt, const SaveSlot &slot) {
    if (!rt || !slot.has_thumb || slot.thumb_bgra.empty()) return nullptr;
    if (bitmap_cache_rt_ != rt) {
        clear_bitmap_caches();
        bitmap_cache_rt_ = rt;
    }
    auto it = slot_thumb_cache_.find(slot.slot);
    if (it != slot_thumb_cache_.end()) return it->second;

    ID2D1Bitmap *bmp = nullptr;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(slot.thumb_w), static_cast<UINT32>(slot.thumb_h)),
                     slot.thumb_bgra.data(), static_cast<UINT32>(slot.thumb_w * 4), props, &bmp);
    slot_thumb_cache_[slot.slot] = bmp;
    return bmp;
}

void LauncherApp::draw_text(ID2D1RenderTarget *rt, const std::string &utf8, IDWriteTextFormat *fmt,
                            D2D1_RECT_F rect, D2D1_COLOR_F col, DWRITE_TEXT_ALIGNMENT align,
                            DWRITE_PARAGRAPH_ALIGNMENT valign, bool ellipsis) {
    if (!rt || !fmt || utf8.empty()) return;
    const std::wstring w = utf8_to_wide(utf8);
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(valign);
    fmt->SetWordWrapping(ellipsis ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
    IDWriteInlineObject *trim_obj = nullptr;
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    if (ellipsis) {
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        if (auto it = trim_signs_.find(fmt); it != trim_signs_.end()) trim_obj = it->second;
    }
    fmt->SetTrimming(&trim, trim_obj);
    if (ID2D1SolidColorBrush *br = solid_brush(rt, col)) {
        rt->DrawText(w.c_str(), static_cast<UINT32>(w.size()), fmt, rect, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void LauncherApp::push_hit(HitRegion hr) {
    if (has_clip_rect_) {
        hr.rect.left = std::max(hr.rect.left, active_clip_rect_.left);
        hr.rect.top = std::max(hr.rect.top, active_clip_rect_.top);
        hr.rect.right = std::min(hr.rect.right, active_clip_rect_.right);
        hr.rect.bottom = std::min(hr.rect.bottom, active_clip_rect_.bottom);
        if (hr.rect.left >= hr.rect.right || hr.rect.top >= hr.rect.bottom) return;
    }
    hits_.push_back(std::move(hr));
}

bool LauncherApp::any_non_titlebar_hit_in_y_band(float y0, float y1) const {
    for (const auto &h : hits_) {
        if (h.id.rfind("tb_", 0) == 0) continue;
        if (h.rect.bottom > y0 && h.rect.top < y1) return true;
    }
    return false;
}

void LauncherApp::draw_button(ID2D1RenderTarget *rt, const std::string &id, D2D1_RECT_F rect, const std::string &label,
                              D2D1_COLOR_F bg, D2D1_COLOR_F border, D2D1_COLOR_F text_col, bool primary,
                              std::function<void()> on_click) {
    const bool hov = (hovered_id_ == id);
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 8.0f, 8.0f);
    const D2D1_COLOR_F c0 = hov ? lerp_col(bg, col_rgba(255, 240, 200, bg.a), primary ? 0.18f : 0.12f) : bg;
    const D2D1_COLOR_F c1 = lerp_col(c0, col_rgba(8, 6, 14, c0.a), 0.25f);
    fill_hgrad_rounded(rt, rr, c0, c1);
    ID2D1SolidColorBrush *bbr = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? lerp_col(border, col_rgba(255, 246, 210), 0.45f) : border, &bbr)) && bbr) {
        rt->DrawRoundedRectangle(rr, bbr, primary ? (hov ? 2.2f : 1.6f) : (hov ? 1.5f : 1.0f));
        bbr->Release();
    }
    draw_text(rt, label, primary ? fmt_play_ : fmt_body_, rect, text_col,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    push_hit({id, rect, std::move(on_click), nullptr, false});
}

void LauncherApp::draw_toggle_row(ID2D1RenderTarget *rt, const std::string &id, float x, float y, float w,
                                  const std::string &label, const std::string &sub, bool value, const Palette &pal,
                                  std::function<void(bool)> on_change) {
    const float h = 44.0f;
    const D2D1_RECT_F row{x, y, x + w, y + h};
    const bool hov = (hovered_id_ == id);
    ID2D1SolidColorBrush *br = nullptr;
    if (hov && SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.accent, 0.08f), &br)) && br) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(row, 8.0f, 8.0f), br);
        br->Release();
    }
    draw_text(rt, label, fmt_body_, D2D1::RectF(x + 10.0f, y + 4.0f, x + w - 70.0f, y + 24.0f), pal.text);
    draw_text(rt, sub, fmt_small_, D2D1::RectF(x + 10.0f, y + 22.0f, x + w - 70.0f, y + 40.0f), pal.text_dim);

    const float sw = 44.0f, sh = 22.0f;
    const D2D1_RECT_F track{x + w - sw - 10.0f, y + (h - sh) * 0.5f, x + w - 10.0f, y + (h + sh) * 0.5f};
    const D2D1_ROUNDED_RECT trr = D2D1::RoundedRect(track, sh * 0.5f, sh * 0.5f);
    if (SUCCEEDED(rt->CreateSolidColorBrush(value ? with_a(pal.accent, 0.85f) : col_rgba(40, 44, 64, 0.9f), &br)) && br) {
        rt->FillRoundedRectangle(trr, br);
        br->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(value ? pal.border_hi : pal.border, &br)) && br) {
        rt->DrawRoundedRectangle(trr, br, 1.0f);
        br->Release();
    }
    const float knob_x = value ? (track.right - sh * 0.5f) : (track.left + sh * 0.5f);
    if (SUCCEEDED(rt->CreateSolidColorBrush(value ? col_rgba(18, 14, 10) : col_rgba(204, 210, 226), &br)) && br) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, (track.top + track.bottom) * 0.5f), 7.5f, 7.5f), br);
        br->Release();
    }
    push_hit({id, row, [on_change, value]() { on_change(!value); }, nullptr, false});
}

void LauncherApp::draw_slider_row(ID2D1RenderTarget *rt, const std::string &id, float x, float y, float w,
                                  const std::string &label, const std::string &val_text, float norm, const Palette &pal,
                                  std::function<void(float)> on_change) {
    const float h = 44.0f;
    draw_text(rt, label, fmt_body_, D2D1::RectF(x + 10.0f, y + 4.0f, x + w - 90.0f, y + 22.0f), pal.text);
    draw_text(rt, val_text, fmt_body_, D2D1::RectF(x + w - 90.0f, y + 4.0f, x + w - 10.0f, y + 22.0f), pal.accent,
              DWRITE_TEXT_ALIGNMENT_TRAILING);

    const D2D1_RECT_F track{x + 10.0f, y + 28.0f, x + w - 10.0f, y + 35.0f};
    const float clamped = std::clamp(norm, 0.0f, 1.0f);
    ID2D1SolidColorBrush *br = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(26, 28, 44, 0.95f), &br)) && br) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(track, 3.5f, 3.5f), br);
        br->Release();
    }
    const float fill_x = track.left + (track.right - track.left) * clamped;
    if (fill_x > track.left + 2.0f) {
        fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(track.left, track.top, fill_x, track.bottom), 3.5f, 3.5f),
                           pal.accent_crimson, pal.accent);
    }
    const bool active = (hovered_id_ == id || dragging_id_ == id);
    if (SUCCEEDED(rt->CreateSolidColorBrush(active ? col_rgba(255, 246, 210) : pal.accent, &br)) && br) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fill_x, (track.top + track.bottom) * 0.5f), active ? 7.5f : 6.0f, active ? 7.5f : 6.0f), br);
        br->Release();
    }
    const D2D1_RECT_F hit{track.left - 4.0f, y + 18.0f, track.right + 4.0f, y + h};
    push_hit({id, hit, nullptr, std::move(on_change), true, clamped});
}

void LauncherApp::draw_segmented_row(ID2D1RenderTarget *rt, const std::string &id_prefix, float x, float y, float w,
                                     const std::string &label, const std::vector<std::string> &opts, int active_idx,
                                     const Palette &pal, std::function<void(int)> on_change) {
    draw_text(rt, label, fmt_body_, D2D1::RectF(x + 10.0f, y + 2.0f, x + w - 10.0f, y + 20.0f), pal.text);
    const int n = static_cast<int>(opts.size());
    if (n <= 0) return;
    const float gap = 6.0f;
    const float pw = (w - 20.0f - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    const float py0 = y + 24.0f, py1 = y + 48.0f;
    for (int i = 0; i < n; ++i) {
        const float px0 = x + 10.0f + static_cast<float>(i) * (pw + gap);
        const D2D1_RECT_F pr{px0, py0, px0 + pw, py1};
        const std::string pid = id_prefix + "_" + std::to_string(i);
        const bool sel = (i == active_idx);
        const bool hov = (hovered_id_ == pid);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(pr, 6.0f, 6.0f);
        if (sel) {
            fill_hgrad_rounded(rt, rr, with_a(pal.accent_crimson, 0.85f), with_a(pal.accent, 0.72f));
        } else {
            ID2D1SolidColorBrush *br = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? pal.card_hover : col_rgba(22, 24, 38, 0.9f), &br)) && br) {
                rt->FillRoundedRectangle(rr, br);
                br->Release();
            }
        }
        ID2D1SolidColorBrush *bbr = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(sel ? pal.border_hi : pal.border, &bbr)) && bbr) {
            rt->DrawRoundedRectangle(rr, bbr, sel ? 1.4f : 1.0f);
            bbr->Release();
        }
        draw_text(rt, opts[static_cast<std::size_t>(i)], fmt_small_, pr, sel ? col_rgba(255, 252, 244) : pal.text_dim,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
        push_hit({pid, pr, [on_change, i]() { on_change(i); }, nullptr, false});
    }
}

void LauncherApp::draw_medallion_d2d(ID2D1RenderTarget *rt, float cx, float cy, float r, int tier_index,
                                     D2D1_COLOR_F gl, D2D1_COLOR_F gr, D2D1_COLOR_F bdr, D2D1_COLOR_F acc,
                                     bool unlocked, bool masked) {
    if (unlocked) fill_radial_glow(rt, cx, cy, r * 1.25f, r * 1.25f, with_a(acc, 0.30f), with_a(acc, 0.0f));
    ID2D1SolidColorBrush *br = nullptr;
    const D2D1_COLOR_F core = unlocked ? lerp_col(gl, gr, 0.4f) : col_rgba(18, 20, 32, 0.95f);
    if (SUCCEEDED(rt->CreateSolidColorBrush(core, &br)) && br) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br);
        br->Release();
    }
    const D2D1_COLOR_F ring_c = unlocked ? bdr : with_a(bdr, 0.38f);
    const D2D1_COLOR_F emb_c = unlocked ? acc : col_rgba(130, 138, 158, 0.65f);
    if (SUCCEEDED(rt->CreateSolidColorBrush(ring_c, &br)) && br) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r - 1.5f, r - 1.5f), br, std::max(1.5f, r * 0.08f));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r * 0.80f, r * 0.80f), br, 1.0f);
        br->Release();
    }
    if (!SUCCEEDED(rt->CreateSolidColorBrush(emb_c, &br)) || !br) return;

    const float e = r * 0.48f;
    const float lw = std::max(1.4f, r * 0.085f);
    if (masked) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - e * 0.2f), e * 0.36f, e * 0.36f), br, lw);
        rt->DrawLine(D2D1::Point2F(cx, cy + e * 0.16f), D2D1::Point2F(cx, cy + e * 0.48f), br, lw);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + e * 0.72f), lw * 0.7f, lw * 0.7f), br);
    } else {
        switch (std::clamp(tier_index, 0, 6)) {
        case 0:  // Mark diamond
            rt->DrawLine(D2D1::Point2F(cx, cy - e), D2D1::Point2F(cx + e * 0.9f, cy), br, lw);
            rt->DrawLine(D2D1::Point2F(cx + e * 0.9f, cy), D2D1::Point2F(cx, cy + e), br, lw);
            rt->DrawLine(D2D1::Point2F(cx, cy + e), D2D1::Point2F(cx - e * 0.9f, cy), br, lw);
            rt->DrawLine(D2D1::Point2F(cx - e * 0.9f, cy), D2D1::Point2F(cx, cy - e), br, lw);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * 0.25f, e * 0.25f), br);
            break;
        case 1:  // Sigil ring & cross
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * 0.80f, e * 0.80f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx - e, cy), D2D1::Point2F(cx + e, cy), br, lw * 0.85f);
            rt->DrawLine(D2D1::Point2F(cx, cy - e), D2D1::Point2F(cx, cy + e), br, lw * 0.85f);
            break;
        case 2:  // Crest shield
            rt->DrawLine(D2D1::Point2F(cx - e * 0.8f, cy - e * 0.6f), D2D1::Point2F(cx + e * 0.8f, cy - e * 0.6f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx - e * 0.8f, cy - e * 0.6f), D2D1::Point2F(cx - e * 0.65f, cy + e * 0.2f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx + e * 0.8f, cy - e * 0.6f), D2D1::Point2F(cx + e * 0.65f, cy + e * 0.2f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx - e * 0.65f, cy + e * 0.2f), D2D1::Point2F(cx, cy + e), br, lw);
            rt->DrawLine(D2D1::Point2F(cx + e * 0.65f, cy + e * 0.2f), D2D1::Point2F(cx, cy + e), br, lw);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * 0.25f, e * 0.25f), br);
            break;
        case 3:  // Sovereign crown
            rt->DrawLine(D2D1::Point2F(cx - e * 0.85f, cy + e * 0.55f), D2D1::Point2F(cx + e * 0.85f, cy + e * 0.55f), br, lw * 1.2f);
            rt->DrawLine(D2D1::Point2F(cx - e * 0.8f, cy + e * 0.45f), D2D1::Point2F(cx - e * 0.9f, cy - e * 0.45f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx - e * 0.9f, cy - e * 0.45f), D2D1::Point2F(cx - e * 0.35f, cy + e * 0.05f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx - e * 0.35f, cy + e * 0.05f), D2D1::Point2F(cx, cy - e * 0.75f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx, cy - e * 0.75f), D2D1::Point2F(cx + e * 0.35f, cy + e * 0.05f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx + e * 0.35f, cy + e * 0.05f), D2D1::Point2F(cx + e * 0.9f, cy - e * 0.45f), br, lw);
            rt->DrawLine(D2D1::Point2F(cx + e * 0.9f, cy - e * 0.45f), D2D1::Point2F(cx + e * 0.8f, cy + e * 0.45f), br, lw);
            break;
        case 4:  // Covenant 8-ray star
            for (int k = 0; k < 8; ++k) {
                const float ang = static_cast<float>(k) * (kPi * 0.25f);
                const float len = (k % 2 == 0) ? e * 1.05f : e * 0.72f;
                rt->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + std::cos(ang) * len, cy + std::sin(ang) * len), br, lw);
            }
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * 0.32f, e * 0.32f), br);
            break;
        case 5:  // Folly starburst
            for (int k = 0; k < 6; ++k) {
                const float ang = static_cast<float>(k) * (kPi / 3.0f) + kPi / 6.0f;
                const float px = cx + std::cos(ang) * e * 0.9f, py = cy + std::sin(ang) * e * 0.9f;
                rt->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(px, py), br, lw * 0.85f);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), lw * 0.8f, lw * 0.8f), br);
            }
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * 0.28f, e * 0.28f), br);
            break;
        case 6:  // Curiosity eye
        default:
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * 0.95f, e * 0.55f), br, lw);
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * 0.38f, e * 0.38f), br, lw);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * 0.16f, e * 0.30f), br);
            break;
        }
    }
    br->Release();
}

void LauncherApp::render_judgment_ring(ID2D1RenderTarget *rt, float cx, float cy, float radius, float alpha, const Palette &pal) {
    if (!d2d_factory_ || alpha <= 0.01f) return;
    fill_radial_glow(rt, cx, cy, radius * 1.35f, radius * 1.35f, with_a(pal.accent_crimson, 0.22f * alpha), with_a(pal.accent_crimson, 0.0f));
    fill_radial_glow(rt, cx, cy, radius * 0.72f, radius * 0.72f, with_a(pal.accent, 0.14f * alpha), with_a(pal.accent, 0.0f));

    ID2D1SolidColorBrush *brass = nullptr;
    rt->CreateSolidColorBrush(with_a(pal.accent, 0.42f * alpha), &brass);
    if (brass) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brass, 2.2f);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius * 0.82f, radius * 0.82f), brass, 1.4f);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius * 0.56f, radius * 0.56f), brass, 1.2f);
        // 36 precision bezel graduations
        const float slow_rot = time_sec_ * 0.08f;
        for (int i = 0; i < 36; ++i) {
            const float ang = slow_rot + static_cast<float>(i) * (2.0f * kPi / 36.0f);
            const float r0 = (i % 3 == 0) ? radius * 0.84f : radius * 0.90f;
            const float r1 = radius * 0.99f;
            rt->DrawLine(D2D1::Point2F(cx + std::cos(ang) * r0, cy + std::sin(ang) * r0),
                         D2D1::Point2F(cx + std::cos(ang) * r1, cy + std::sin(ang) * r1),
                         brass, (i % 9 == 0) ? 2.2f : 1.0f);
        }
        // Counter-rotating inner occult compass cross
        const float rev_rot = -time_sec_ * 0.14f;
        for (int k = 0; k < 8; ++k) {
            const float ang = rev_rot + static_cast<float>(k) * (kPi * 0.25f);
            const float r0 = radius * 0.18f;
            const float r1 = (k % 2 == 0) ? radius * 0.55f : radius * 0.40f;
            rt->DrawLine(D2D1::Point2F(cx + std::cos(ang) * r0, cy + std::sin(ang) * r0),
                         D2D1::Point2F(cx + std::cos(ang) * r1, cy + std::sin(ang) * r1),
                         brass, (k % 2 == 0) ? 1.4f : 0.9f);
        }
        brass->Release();
    }

    // Glowing Crimson Hit Zones + Radiant Golden Perfect Strike Zones
    const float zone_off = time_sec_ * 0.06f;
    const float r_in = radius * 0.58f, r_out = radius * 0.80f;
    struct Zone { float a0, a1, p0, p1; };
    const Zone zones[3] = {
        {-0.85f, -0.10f, -0.22f, -0.10f},
        {1.15f, 1.85f, 1.73f, 1.85f},
        {3.25f, 3.88f, 3.76f, 3.88f},
    };
    for (const Zone &z : zones) {
        draw_arc_band(d2d_factory_, rt, cx, cy, r_in, r_out, z.a0 + zone_off, z.a1 + zone_off, with_a(pal.accent_crimson, 0.42f * alpha));
        draw_arc_band(d2d_factory_, rt, cx, cy, r_in, r_out, z.p0 + zone_off, z.p1 + zone_off, with_a(pal.accent, 0.72f * alpha));
    }

    // Sweeping luminous Judgment Ring needle!
    const float needle = time_sec_ * 0.65f - 0.6f;
    draw_arc_band(d2d_factory_, rt, cx, cy, radius * 0.16f, radius * 0.84f, needle - 0.24f, needle, with_a(pal.accent, 0.16f * alpha));
    ID2D1SolidColorBrush *nbr = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(col_rgba(255, 250, 232), 0.88f * alpha), &nbr)) && nbr) {
        rt->DrawLine(D2D1::Point2F(cx - std::cos(needle) * radius * 0.12f, cy - std::sin(needle) * radius * 0.12f),
                     D2D1::Point2F(cx + std::cos(needle) * radius * 0.94f, cy + std::sin(needle) * radius * 0.94f), nbr, 2.4f);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius * 0.065f, radius * 0.065f), nbr);
        nbr->Release();
    }
}

void LauncherApp::render_background(ID2D1RenderTarget *rt, float w, float h, const Palette &pal) {
    // Deep nocturnal vertical gradient
    fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), 0, 0), pal.bg_top, pal.bg_bot);

    // Hybrid Home Hero: optional soft latest screenshot backdrop behind obsidian vignette
    if (page_ == Page::Home && state_.settings.launcher_hero_screenshot && !state_.hero_screenshot.empty()) {
        if (ID2D1Bitmap *hero = load_bitmap_cached(rt, state_.hero_screenshot, 1280, 720)) {
            const D2D1_SIZE_F bs = hero->GetSize();
            const float area_w = w - kSidebarW, area_h = h - kTitlebarH;
            const float sc = std::max(area_w / std::max(1.0f, bs.width), area_h / std::max(1.0f, bs.height));
            const float dw = bs.width * sc, dh = bs.height * sc;
            const float dx = kSidebarW + (area_w - dw) * 0.5f;
            const float dy = kTitlebarH + (area_h - dh) * 0.5f;
            rt->DrawBitmap(hero, D2D1::RectF(dx, dy, dx + dw, dy + dh), 0.22f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            // Smooth dark gradient fade from left so hero text stays razor-sharp
            fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(kSidebarW, kTitlebarH, w, h), 0, 0),
                               with_a(pal.bg_top, 0.92f), with_a(pal.bg_bot, 0.52f));
        }
    }

    // Animated Judgment Ring centerpiece (centered watermark on Trilogy, prominent on Home, subtle watermark on subpages)
    float ring_cx = w - 150.0f;
    float ring_cy = h - 130.0f;
    float ring_r = 190.0f;
    float ring_alpha = 0.20f;
    if (page_ == Page::Trilogy) {
        ring_cx = w * 0.5f;
        ring_cy = kTitlebarH + (h - kTitlebarH) * 0.48f;
        ring_r = 280.0f;
        ring_alpha = 0.24f;
    } else if (page_ == Page::Home) {
        ring_cx = kSidebarW + (w - kSidebarW) * 0.72f;
        ring_cy = kTitlebarH + (h - kTitlebarH) * 0.42f;
        ring_r = 215.0f;
        ring_alpha = 0.95f;
    }
    render_judgment_ring(rt, ring_cx, ring_cy, ring_r, ring_alpha, pal);

    // Subtle drifting soul embers
    ID2D1SolidColorBrush *ebr = nullptr;
    for (const Ember &em : embers_) {
        const float pulse = 0.65f + 0.35f * std::sin(time_sec_ * 2.1f + em.phase);
        const D2D1_COLOR_F ec = lerp_col(pal.accent, pal.accent_crimson, 0.35f + 0.35f * std::sin(em.phase));
        const float em_mul = (page_ == Page::Home || page_ == Page::Trilogy) ? 0.9f : 0.45f;
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(ec, em.alpha * pulse * em_mul), &ebr)) && ebr) {
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(em.x, em.y), em.radius, em.radius), ebr);
            ebr->Release();
        }
    }
}

void LauncherApp::render_titlebar(ID2D1RenderTarget *rt, float w, const Palette &pal) {
    ID2D1SolidColorBrush *br = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(10, 8, 18, 0.88f), &br)) && br) {
        rt->FillRectangle(D2D1::RectF(0, 0, w, kTitlebarH), br);
        br->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &br)) && br) {
        rt->DrawLine(D2D1::Point2F(0, kTitlebarH), D2D1::Point2F(w, kTitlebarH), br, 1.0f);
        br->Release();
    }

    // Mini Judgment Ring emblem + brand title on left
    draw_medallion_d2d(rt, 24.0f, kTitlebarH * 0.5f, 11.0f, 4, pal.bg_top, pal.accent_crimson, pal.accent, pal.accent, true, false);
    draw_text(rt, "SHADOW HEARTS: NOCTURNE", fmt_card_title_, D2D1::RectF(44.0f, 0.0f, 302.0f, kTitlebarH),
              pal.accent, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const std::string tb_sub = (page_ == Page::Trilogy)
        ? std::string("•  Trilogy Collection  •  Native PC Ports")
        : (std::string("•  ") + game_profile(active_game_).full_title + "  •  " + game_profile(active_game_).serials);
    draw_text(rt, tb_sub, fmt_small_, D2D1::RectF(308.0f, 1.0f, 700.0f, kTitlebarH),
              pal.text_dim, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Top-right quick controls: [Theme (in hub)] [Chimes On/Off] [—] [×]
    float rx = w - 12.0f;
    const auto win_btn = [&](const std::string &id, float bw, const std::string &lbl, bool danger, std::function<void()> fn) {
        rx -= bw;
        const D2D1_RECT_F r{rx, 8.0f, rx + bw, kTitlebarH - 8.0f};
        const bool hov = (hovered_id_ == id);
        const D2D1_COLOR_F bg = hov ? (danger ? col_rgba(186, 36, 54, 0.92f) : pal.card_hover) : col_rgba(24, 22, 38, 0.65f);
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(bg, &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? pal.border_hi : with_a(pal.border, 0.45f), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), b, 1.0f);
            b->Release();
        }
        draw_text(rt, lbl, fmt_small_, r, hov ? col_rgba(255, 252, 245) : pal.text_dim,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        hits_.push_back({id, r, std::move(fn), nullptr, false});
        rx -= 8.0f;
    };

    win_btn("tb_close", 34.0f, "X", true, [this]() {
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    });
    win_btn("tb_min", 34.0f, "-", false, [this]() {
        if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
    });
    win_btn("tb_chime", 96.0f, global_cfg_.chimes ? "Chimes: ON" : "Chimes: OFF", false, [this]() {
        global_cfg_.chimes = !global_cfg_.chimes;
        state_.settings.launcher_chimes = global_cfg_.chimes;
        audio_.set_enabled(global_cfg_.chimes);
        save_global_config(state_.paths.root_dir, global_cfg_);
        if (page_ != Page::Trilogy) persist_settings();
        show_toast(global_cfg_.chimes ? "Launcher chimes enabled" : "Launcher chimes muted");
    });
    if (page_ != Page::Trilogy) {
        win_btn("tb_theme", 168.0f, std::string("Theme: ") + achievements::catalog_toast_theme_name(state_.settings.toast_theme), false, [this]() {
            state_.settings.toast_theme = (state_.settings.toast_theme + 1) % static_cast<int>(achievements::ToastTheme::Count);
            persist_settings(std::string("Theme: ") + achievements::catalog_toast_theme_name(state_.settings.toast_theme));
        });
    }

    // Subtle startup "Update Available" pill when a newer GitHub commit exists on the active channel
    UpdateCheckResult uinfo;
    {
        std::lock_guard<std::mutex> lk(update_mu_);
        uinfo = update_info_;
    }
    if (uinfo.checked && uinfo.ok && uinfo.update_available && !uinfo.remote_short.empty()) {
        const std::string ulbl = "Update • " + uinfo.remote_short;
        const float uw = 122.0f;
        rx -= uw;
        const D2D1_RECT_F ur{rx, 8.0f, rx + uw, kTitlebarH - 8.0f};
        const bool hov = (hovered_id_ == "tb_update");
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? with_a(pal.accent_crimson, 0.92f) : with_a(pal.accent_crimson, 0.58f), &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(ur, 6.0f, 6.0f), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? col_rgba(255, 224, 150) : pal.accent, &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(ur, 6.0f, 6.0f), b, 1.1f);
            b->Release();
        }
        draw_text(rt, ulbl, fmt_small_, ur, col_rgba(255, 245, 225),
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        hits_.push_back({"tb_update", ur, [this]() {
            if (page_ == Page::Trilogy) {
                select_game(active_game_, false);
            }
            page_ = Page::Tools;
            tools_subtab_ = 2;
            audio_.play(SoundCue::TabSwitch);
        }, nullptr, false});
    }
}

void LauncherApp::render_trilogy(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const float w = area.right - area.left;

    // Centered Gothic Header
    const float hdr_y = area.top + 18.0f;
    const D2D1_RECT_F badge{(w - 292.0f) * 0.5f, hdr_y, (w + 292.0f) * 0.5f, hdr_y + 22.0f};
    fill_hgrad_rounded(rt, D2D1::RoundedRect(badge, 11, 11), with_a(pal.accent_crimson, 0.45f), with_a(pal.accent, 0.24f));
    ID2D1SolidColorBrush *br = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border_hi, 0.85f), &br)) && br) {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(badge, 11, 11), br, 1.0f);
        br->Release();
    }
    draw_text(rt, "SHADOW HEARTS  •  NATIVE PC TRILOGY", fmt_pill_, badge, col_rgba(255, 244, 214),
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    draw_text(rt, "CHOOSE YOUR CHRONICLE", fmt_h1_,
              D2D1::RectF(40.0f, hdr_y + 28.0f, w - 40.0f, hdr_y + 60.0f),
              pal.accent, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    draw_text(rt, "Select a chapter to enter its dedicated command hub, settings, saves, and trophy gallery",
              fmt_small_, D2D1::RectF(40.0f, hdr_y + 58.0f, w - 40.0f, hdr_y + 78.0f),
              pal.text_dim, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 3-Panel Vertical Gothic Triptych
    const float pad_x = 36.0f;
    const float gap = 22.0f;
    const float card_top = hdr_y + 92.0f;
    const float footer_reserve = 38.0f;
    const float card_bot = area.bottom - footer_reserve;
    const float card_w = (w - pad_x * 2.0f - gap * 2.0f) / 3.0f;
    const float card_h = std::max(380.0f, card_bot - card_top);

    // Compact Prequel Toast Pill pinned top-left directly above Card #1 (0 · Koudelka, 1898 PS1)
    {
        const GameId kgid = GameId::Koudelka;
        const TrilogyCardSummary &ksum = trilogy_summaries_[static_cast<std::size_t>(kgid)];
        const Palette kp = palette_for_game(kgid, 0);
        const std::string kcid = "trilogy_card_" + std::to_string(static_cast<int>(kgid));
        const bool khov = (hovered_id_ == kcid || (gamepad_nav_active_ && focused_id_ == kcid));
        const float klift = khov ? -2.0f : 0.0f;
        const float kw = std::min(card_w, 294.0f);
        const float kh = 54.0f;
        const float ky = hdr_y + 12.0f + klift;
        const D2D1_RECT_F kr{pad_x, ky, pad_x + kw, ky + kh};
        const D2D1_ROUNDED_RECT krr = D2D1::RoundedRect(kr, 12.0f, 12.0f);

        if (khov || ksum.ready_to_play || ksum.disc1_ok) {
            fill_radial_glow(rt, kr.left + 44.0f, (kr.top + kr.bottom) * 0.5f, 120.0f, 38.0f,
                             with_a(kp.accent_crimson, khov ? 0.36f : 0.18f), with_a(kp.accent_crimson, 0.0f));
        }
        fill_hgrad_rounded(rt, krr,
                           khov ? lerp_col(kp.bg_top, kp.card_hover, 0.65f) : with_a(kp.bg_top, 0.94f),
                           khov ? lerp_col(kp.bg_bot, kp.accent_crimson, 0.30f) : with_a(kp.bg_bot, 0.92f));
        if (SUCCEEDED(rt->CreateSolidColorBrush(khov ? kp.border_hi : with_a(kp.border, 0.72f), &br)) && br) {
            rt->DrawRoundedRectangle(krr, br, khov ? 1.8f : 1.0f);
            br->Release();
        }

        // Left gilded circular medallion ("0")
        const float mx_c = kr.left + 26.0f;
        const float my_c = (kr.top + kr.bottom) * 0.5f;
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(kp.accent_crimson, 0.42f), &br)) && br) {
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx_c, my_c), 15.0f, 15.0f), br);
            br->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(khov ? kp.border_hi : with_a(kp.accent, 0.78f), &br)) && br) {
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mx_c, my_c), 15.0f, 15.0f), br, 1.2f);
            br->Release();
        }
        draw_text(rt, "0", fmt_card_title_,
                  D2D1::RectF(mx_c - 15.0f, my_c - 15.0f, mx_c + 15.0f, my_c + 15.0f),
                  kp.accent, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Text stack: eyebrow + title
        const float tx0 = kr.left + 49.0f;
        const float tx1 = kr.right - 92.0f;
        draw_text(rt, "1898 PREQUEL  •  PS1", fmt_pill_,
                  D2D1::RectF(tx0, kr.top + 8.0f, tx1, kr.top + 23.0f), kp.accent);
        draw_text(rt, "KOUDELKA", fmt_card_title_,
                  D2D1::RectF(tx0, kr.top + 23.0f, tx1, kr.bottom - 7.0f), pal.text);

        // Right live status pill (READY / 4 DISCS / PREQUEL)
        std::string kst_lbl = "PREQUEL";
        D2D1_COLOR_F kst_c = kp.accent;
        D2D1_COLOR_F kst_bg = col_rgba(24, 16, 26, 0.90f);
        if (ksum.ready_to_play) {
            kst_lbl = "READY";
            kst_c = pal.good;
            kst_bg = col_rgba(14, 52, 40, 0.92f);
        } else if (ksum.disc1_ok) {
            kst_lbl = "4 DISCS";
            kst_c = pal.warn;
            kst_bg = col_rgba(62, 36, 14, 0.92f);
        }
        const D2D1_RECT_F ksp{kr.right - 84.0f, my_c - 11.0f, kr.right - 12.0f, my_c + 11.0f};
        if (SUCCEEDED(rt->CreateSolidColorBrush(kst_bg, &br)) && br) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(ksp, 10.0f, 10.0f), br);
            br->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(kst_c, 0.80f), &br)) && br) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(ksp, 10.0f, 10.0f), br, 1.0f);
            br->Release();
        }
        draw_text(rt, kst_lbl, fmt_pill_, ksp, kst_c,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        hits_.push_back({kcid, kr, [this]() {
            audio_.play(SoundCue::TabSwitch);
            select_game(GameId::Koudelka, true);
        }, nullptr, false});
    }

    for (int i = 0; i < 3; ++i) {
        const GameId gid = static_cast<GameId>(i);
        const GameProfile &prof = game_profile(gid);
        const TrilogyCardSummary &sum = trilogy_summaries_[static_cast<std::size_t>(i)];
        const Palette gp = palette_for_game(gid, 0);

        const std::string cid = "trilogy_card_" + std::to_string(i);
        const bool hov = (hovered_id_ == cid || (gamepad_nav_active_ && focused_id_ == cid));
        const float lift = hov ? -4.0f : 0.0f;

        const float cx0 = pad_x + static_cast<float>(i) * (card_w + gap);
        const D2D1_RECT_F cr{cx0, card_top + lift, cx0 + card_w, card_top + card_h + lift};
        const D2D1_ROUNDED_RECT crr = D2D1::RoundedRect(cr, 14.0f, 14.0f);

        // Ambient glow behind highlighted card
        if (hov || sum.ready_to_play) {
            fill_radial_glow(rt, (cr.left + cr.right) * 0.5f, (cr.top + cr.bottom) * 0.5f,
                             card_w * 0.62f, card_h * 0.56f,
                             with_a(gp.accent_crimson, hov ? 0.30f : 0.14f),
                             with_a(gp.accent_crimson, 0.0f));
        }

        // Card base gradient
        fill_hgrad_rounded(rt, crr,
                           hov ? lerp_col(gp.bg_top, gp.card_hover, 0.55f) : with_a(gp.bg_top, 0.94f),
                           hov ? lerp_col(gp.bg_bot, gp.card_hover, 0.35f) : with_a(gp.bg_bot, 0.94f));

        // Top artwork showcase region (upper 53% of the portrait card)
        const float art_h = card_h * 0.53f;
        const D2D1_RECT_F art_r{cr.left + 8.0f, cr.top + 8.0f, cr.right - 8.0f, cr.top + art_h};
        const D2D1_ROUNDED_RECT art_rr = D2D1::RoundedRect(art_r, 10.0f, 10.0f);

        // Layered artwork: drop-in cover.png/.jpg -> latest screenshot -> pure procedural Gothic crest
        const std::filesystem::path art_path = !sum.cover_image.empty() ? sum.cover_image : sum.hero_screenshot;
        const bool is_cover = !sum.cover_image.empty();
        ID2D1Bitmap *art_bmp = !art_path.empty() ? load_bitmap_cached(rt, art_path, 512, 512) : nullptr;

        rt->PushAxisAlignedClip(art_r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        fill_hgrad_rounded(rt, art_rr, gp.bg_top, lerp_col(gp.bg_bot, gp.accent_crimson, 0.22f));
        if (art_bmp) {
            const D2D1_SIZE_F bs = art_bmp->GetSize();
            const float aw = art_r.right - art_r.left, ah = art_r.bottom - art_r.top;
            const float sc = std::max(aw / std::max(1.0f, bs.width), ah / std::max(1.0f, bs.height));
            const float dw = bs.width * sc, dh = bs.height * sc;
            const float dx = art_r.left + (aw - dw) * 0.5f;
            const float dy = art_r.top + (ah - dh) * 0.5f;
            rt->DrawBitmap(art_bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh),
                           is_cover ? (hov ? 0.92f : 0.82f) : (hov ? 0.44f : 0.32f),
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            fill_hgrad_rounded(rt, art_rr, with_a(gp.bg_top, is_cover ? 0.14f : 0.45f), with_a(gp.bg_bot, is_cover ? 0.72f : 0.88f));
        }

        // Procedural Gothic Judgment Ring crest + atmospheric Roman Numeral watermark
        const float mid_x = (art_r.left + art_r.right) * 0.5f;
        const float mid_y = (art_r.top + art_r.bottom) * 0.50f;
        if (!is_cover) {
            fill_radial_glow(rt, mid_x, mid_y, 118.0f, 118.0f,
                             with_a(gp.accent_crimson, hov ? 0.34f : 0.22f), with_a(gp.accent_crimson, 0.0f));
            // Outer delicate Judgment Ring ornament
            if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(gp.border_hi, hov ? 0.55f : 0.32f), &br)) && br) {
                rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mid_x, mid_y), 86.0f, 86.0f), br, 1.4f);
                rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mid_x, mid_y), 66.0f, 66.0f), br, 0.9f);
                br->Release();
            }
            const float spin = time_sec_ * (0.16f + static_cast<float>(i) * 0.04f);
            draw_arc_band(d2d_factory_, rt, mid_x, mid_y, 67.0f, 85.0f, spin, spin + 0.52f, with_a(gp.accent_crimson, hov ? 0.52f : 0.32f));
            draw_arc_band(d2d_factory_, rt, mid_x, mid_y, 67.0f, 85.0f, spin + 2.4f, spin + 2.75f, with_a(gp.accent, hov ? 0.48f : 0.28f));

            // Translucent gilded Roman numeral (I / II / III)
            draw_text(rt, prof.roman, fmt_roman_,
                      D2D1::RectF(art_r.left, mid_y - 44.0f, art_r.right, mid_y + 44.0f),
                      with_a(gp.accent, hov ? 0.92f : 0.72f),
                      DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            // Subtle Roman numeral corner medallion when custom cover art is present
            const D2D1_RECT_F rbadge{art_r.left + 10.0f, art_r.top + 10.0f, art_r.left + 48.0f, art_r.top + 34.0f};
            if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(10, 8, 18, 0.82f), &br)) && br) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(rbadge, 7, 7), br);
                br->Release();
            }
            draw_text(rt, prof.roman, fmt_card_title_, rbadge, gp.accent,
                      DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Top-right status pill inside artwork frame
        std::string status_lbl = "COMING SOON";
        D2D1_COLOR_F st_bg = col_rgba(18, 16, 28, 0.88f);
        D2D1_COLOR_F st_bdr = with_a(gp.border_hi, 0.75f);
        D2D1_COLOR_F st_txt = gp.accent;
        if (sum.ready_to_play) {
            status_lbl = "READY TO PLAY";
            st_bg = col_rgba(14, 52, 40, 0.92f);
            st_bdr = pal.good;
            st_txt = pal.good;
        } else if (sum.exe_present) {
            status_lbl = "SETUP DISC";
            st_bg = col_rgba(64, 38, 14, 0.92f);
            st_bdr = pal.warn;
            st_txt = pal.warn;
        }
        const float spw = 112.0f;
        const D2D1_RECT_F sp{art_r.right - spw - 10.0f, art_r.top + 10.0f, art_r.right - 10.0f, art_r.top + 32.0f};
        if (SUCCEEDED(rt->CreateSolidColorBrush(st_bg, &br)) && br) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(sp, 11, 11), br);
            br->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(st_bdr, &br)) && br) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(sp, 11, 11), br, 1.1f);
            br->Release();
        }
        draw_text(rt, status_lbl, fmt_pill_, sp, st_txt,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Subtle inner filigree frame on art showcase
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(gp.border, 0.42f), &br)) && br) {
            rt->DrawRoundedRectangle(art_rr, br, 1.0f);
            br->Release();
        }
        rt->PopAxisAlignedClip();

        // Lower Metadata & Chronicle Section
        float my = art_r.bottom + 14.0f;
        const float mx = cr.left + 18.0f;
        const float mw = card_w - 36.0f;

        draw_text(rt, prof.era_subtitle, fmt_pill_,
                  D2D1::RectF(mx, my, mx + mw, my + 16.0f), gp.accent);
        my += 18.0f;

        draw_text(rt, prof.full_title, fmt_h2_,
                  D2D1::RectF(mx, my, mx + mw, my + 28.0f), pal.text);
        my += 26.0f;

        draw_text(rt, std::string("PS2 NTSC-U  •  ") + prof.serials + (prof.disc_count >= 2 ? "  •  2 Discs" : "  •  1 Disc"),
                  fmt_small_, D2D1::RectF(mx, my, mx + mw, my + 18.0f), pal.text_dim);
        my += 24.0f;

        // Gilded hairline divider
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(gp.border, 0.45f), &br)) && br) {
            rt->DrawLine(D2D1::Point2F(mx, my), D2D1::Point2F(mx + mw, my), br, 1.0f);
            br->Release();
        }
        my += 12.0f;

        // Live chronicle stats
        if (sum.trophies_total > 0) {
            const int pct = (sum.trophies_unlocked * 100) / sum.trophies_total;
            std::string tline = "Trophies: " + std::to_string(sum.trophies_unlocked) + " / " + std::to_string(sum.trophies_total) +
                                " (" + std::to_string(pct) + "%)";
            if (sum.playtime_seconds > 0.0) {
                tline += "  •  " + format_playtime(static_cast<float>(sum.playtime_seconds));
            }
            draw_text(rt, tline, fmt_body_, D2D1::RectF(mx, my, mx + mw, my + 20.0f), pal.text);
            my += 22.0f;
            std::string line2 = std::to_string(sum.occupied_slots) + " Save Slots";
            if (sum.has_autosave) line2 += " + Autosave";
            if (!sum.latest_area.empty()) line2 += "  •  " + sum.latest_area;
            else if (sum.disc1_ok) line2 += (sum.disc2_ok ? "  •  Discs 1 & 2 Ready" : "  •  Disc 1 Ready");
            draw_text(rt, line2, fmt_small_, D2D1::RectF(mx, my, mx + mw, my + 18.0f), pal.text_dim);
        } else if (sum.ready_to_play) {
            draw_text(rt, "Native PC Port Ready to Play",
                      fmt_body_, D2D1::RectF(mx, my, mx + mw, my + 20.0f), pal.text);
            my += 22.0f;
            std::string line2 = std::to_string(sum.occupied_slots) + " Save Slots";
            if (sum.has_autosave) line2 += " + Autosave";
            line2 += "  •  Unpacked SSD Disc Ready";
            draw_text(rt, line2, fmt_small_, D2D1::RectF(mx, my, mx + mw, my + 18.0f), pal.text_dim);
        } else {
            draw_text(rt, gid == GameId::ShadowHearts1 ? "Next PC Port In Active Development" : "Planned Future Native PC Port",
                      fmt_body_, D2D1::RectF(mx, my, mx + mw, my + 20.0f), pal.text);
            my += 22.0f;
            const std::string disc_note = sum.disc1_ok
                ? "PS2 Disc staged & detected — ready for porting"
                : (std::string("Hub open for pre-config & disc staging (games/") + prof.slug + "/)");
            draw_text(rt, disc_note, fmt_small_, D2D1::RectF(mx, my, mx + mw, my + 18.0f),
                      sum.disc1_ok ? pal.good : pal.text_dim);
        }

        // Bottom CTA pill inside the card
        const D2D1_RECT_F cta{mx, cr.bottom - 52.0f, mx + mw, cr.bottom - 16.0f};
        const D2D1_ROUNDED_RECT cta_rr = D2D1::RoundedRect(cta, 9.0f, 9.0f);
        if (sum.ready_to_play) {
            fill_hgrad_rounded(rt, cta_rr,
                               hov ? lerp_col(gp.accent_crimson, gp.accent, 0.20f) : with_a(gp.accent_crimson, 0.82f),
                               hov ? with_a(gp.accent, 0.65f) : with_a(gp.accent_crimson, 0.45f));
        } else {
            fill_hgrad_rounded(rt, cta_rr,
                               hov ? gp.card_hover : col_rgba(18, 16, 28, 0.85f),
                               hov ? with_a(gp.accent_crimson, 0.35f) : col_rgba(14, 12, 22, 0.90f));
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? gp.border_hi : with_a(gp.border, 0.65f), &br)) && br) {
            rt->DrawRoundedRectangle(cta_rr, br, hov ? 1.5f : 1.0f);
            br->Release();
        }
        std::string cta_name = "COVENANT";
        if (gid == GameId::ShadowHearts1) cta_name = "SHADOW HEARTS";
        else if (gid == GameId::FromTheNewWorld) cta_name = "NEW WORLD";
        const std::string cta_lbl = sum.ready_to_play
            ? ("ENTER " + cta_name + " HUB   ->")
            : "OPEN HUB (COMING SOON)   ->";
        draw_text(rt, cta_lbl, fmt_card_title_, cta,
                  (sum.ready_to_play || hov) ? col_rgba(255, 250, 236) : gp.accent,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Card outer gilded border
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? gp.border_hi : with_a(gp.border, 0.60f), &br)) && br) {
            rt->DrawRoundedRectangle(crr, br, hov ? 2.2f : 1.1f);
            br->Release();
        }

        hits_.push_back({cid, cr, [this, gid]() {
            audio_.play(SoundCue::TabSwitch);
            select_game(gid, true);
        }, nullptr, false});
    }

    // Subtle bottom navigation & controller footer
    const std::string foot = (gamepad_connected_ || gamepad_nav_active_)
        ? (gamepad_is_sony_
               ? "D-Pad / Stick: Choose Chronicle   •   ×: Enter Game Hub   •   Shadow Hearts: Nocturne Trilogy Hub"
               : "D-Pad / Stick: Choose Chronicle   •   A: Enter Game Hub   •   Shadow Hearts: Nocturne Trilogy Hub")
        : "Click a Chronicle or use ← / → and Enter   •   Drop optional cover.png into games/<game>/ for custom box art";
    draw_text(rt, foot, fmt_small_,
              D2D1::RectF(pad_x, area.bottom - 30.0f, w - pad_x, area.bottom - 8.0f),
              pal.text_dim, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void LauncherApp::render_sidebar(ID2D1RenderTarget *rt, float h, const Palette &pal) {
    const GameProfile &prof = game_profile(active_game_);
    ID2D1SolidColorBrush *br = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.sidebar_bg, &br)) && br) {
        rt->FillRectangle(D2D1::RectF(0, kTitlebarH, kSidebarW, h), br);
        br->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.45f), &br)) && br) {
        rt->DrawLine(D2D1::Point2F(kSidebarW, kTitlebarH), D2D1::Point2F(kSidebarW, h), br, 1.0f);
        br->Release();
    }

    // Top "<- Trilogy" return button
    {
        const D2D1_RECT_F tr{14.0f, kTitlebarH + 14.0f, kSidebarW - 14.0f, kTitlebarH + 48.0f};
        const bool hov = (hovered_id_ == "nav_trilogy" || (gamepad_nav_active_ && focused_id_ == "nav_trilogy"));
        const D2D1_ROUNDED_RECT trr = D2D1::RoundedRect(tr, 8.0f, 8.0f);
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? pal.card_hover : col_rgba(20, 18, 32, 0.78f), &br)) && br) {
            rt->FillRoundedRectangle(trr, br);
            br->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? pal.border_hi : with_a(pal.border, 0.65f), &br)) && br) {
            rt->DrawRoundedRectangle(trr, br, hov ? 1.4f : 1.0f);
            br->Release();
        }
        draw_text(rt, "<-  All Games (Trilogy)", fmt_body_, tr, hov ? col_rgba(255, 250, 236) : pal.accent,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        hits_.push_back({"nav_trilogy", tr, [this]() {
            audio_.play(SoundCue::TabSwitch);
            return_to_trilogy();
        }, nullptr, false});
    }

    const std::string trophy_sub = prof.has_achievement_catalog
        ? (std::to_string(state_.trophies.size()) + " Achievements")
        : std::string("Chronicle Pending");
    struct NavItem { Page p; const char *label; const char *sub; };
    const NavItem nav[7] = {
        {Page::Home, "Home", "Play & Command Hub"},
        {Page::Settings, "Settings", "Graphics, Lighting, Audio"},
        {Page::Trophies, "Trophy Gallery", trophy_sub.c_str()},
        {Page::SavesMedia, "Saves & Media", "Slots 0-9 & Screenshots"},
        {Page::Tools, "Tools", "Discs, Mods, Backups"},
        {Page::Coop, "Co-op Lobby", "2-4P Online & LAN"},
        {Page::RingDojo, "Ring Dojo", "0 ms Practice & Timing"},
    };

    float y = kTitlebarH + 54.0f;
    for (int i = 0; i < 7; ++i) {
        const bool active = (page_ == nav[i].p);
        const std::string id = "nav_" + std::to_string(i);
        const bool hov = (hovered_id_ == id);
        const D2D1_RECT_F r{14.0f, y, kSidebarW - 14.0f, y + 40.0f};
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 8.0f, 8.0f);
        if (active) {
            fill_hgrad_rounded(rt, rr, with_a(pal.accent_crimson, 0.45f), with_a(pal.accent, 0.20f));
            if (SUCCEEDED(rt->CreateSolidColorBrush(pal.border_hi, &br)) && br) {
                rt->DrawRoundedRectangle(rr, br, 1.3f);
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 2, r.top + 6, r.left + 6, r.bottom - 6), 2, 2), br);
                br->Release();
            }
        } else if (hov) {
            if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_hover, &br)) && br) {
                rt->FillRoundedRectangle(rr, br);
                br->Release();
            }
        }
        draw_text(rt, nav[i].label, fmt_card_title_, D2D1::RectF(r.left + 16.0f, r.top + 3.0f, r.right - 10.0f, r.top + 21.0f),
                  active ? pal.accent : pal.text);
        draw_text(rt, nav[i].sub, fmt_small_, D2D1::RectF(r.left + 16.0f, r.top + 20.0f, r.right - 10.0f, r.bottom - 3.0f),
                  active ? col_rgba(232, 224, 212) : pal.text_dim);
        const Page target = nav[i].p;
        hits_.push_back({id, r, [this, target]() {
            if (page_ != target) {
                audio_.play(SoundCue::TabSwitch);
                page_ = target;
                armed_delete_slot_ = -1;
                coop_edit_field_ = 0;
                if (target == Page::Coop && !coop_net_.is_open()) {
                    coop_net_.open(0, true);
                }
                if (target == Page::RingDojo) {
                    ring_dojo_.set_best_streak(global_cfg_.dojo_best_streak, global_cfg_.dojo_best_perfect);
                    dojo_last_tick_ms_ = GetTickCount64();
                }
            }
        }, nullptr, false});
        y += 44.0f;
    }

    // Compact Controller Legend pill above the Progress card when a gamepad is connected or active
    if (gamepad_connected_ || gamepad_nav_active_) {
        const D2D1_RECT_F gp{14.0f, h - 186.0f, kSidebarW - 14.0f, h - 144.0f};
        const D2D1_ROUNDED_RECT grr = D2D1::RoundedRect(gp, 8.0f, 8.0f);
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.accent_crimson, 0.24f), &br)) && br) {
            rt->FillRoundedRectangle(grr, br);
            br->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border_hi, 0.70f), &br)) && br) {
            rt->DrawRoundedRectangle(grr, br, 1.0f);
            br->Release();
        }
        draw_text(rt, gamepad_is_sony_ ? "CONTROLLER (PS)" : "CONTROLLER READY", fmt_pill_,
                  D2D1::RectF(gp.left + 10, gp.top + 5, gp.right - 10, gp.top + 19), pal.accent);
        draw_text(rt, gamepad_is_sony_ ? "L1/R1 Tabs • × OK • ○ Back" : "LB/RB Tabs • A OK • B Back",
                  fmt_small_, D2D1::RectF(gp.left + 8, gp.top + 20, gp.right - 8, gp.bottom - 4), pal.text_dim);
    }

    // Bottom sidebar compact Trophy & Port Status card
    const D2D1_RECT_F card{14.0f, h - 134.0f, kSidebarW - 14.0f, h - 16.0f};
    const D2D1_ROUNDED_RECT crr = D2D1::RoundedRect(card, 10.0f, 10.0f);
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &br)) && br) {
        rt->FillRoundedRectangle(crr, br);
        br->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &br)) && br) {
        rt->DrawRoundedRectangle(crr, br, 1.0f);
        br->Release();
    }
    const int tot = static_cast<int>(state_.trophies.size());
    const int pct = tot > 0 ? (state_.trophies_unlocked * 100 / tot) : 0;
    draw_text(rt, prof.progress_heading, fmt_pill_, D2D1::RectF(card.left + 12, card.top + 10, card.right - 12, card.top + 26), pal.accent);
    if (tot > 0) {
        draw_text(rt, std::to_string(state_.trophies_unlocked) + " / " + std::to_string(tot) + " Trophies (" + std::to_string(pct) + "%)",
                  fmt_body_, D2D1::RectF(card.left + 12, card.top + 28, card.right - 12, card.top + 48), pal.text);
    } else {
        draw_text(rt, "Chronicle In Progress",
                  fmt_body_, D2D1::RectF(card.left + 12, card.top + 28, card.right - 12, card.top + 48), pal.text_dim);
    }
    const D2D1_RECT_F pb{card.left + 12, card.top + 54, card.right - 12, card.top + 62};
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(24, 24, 40), &br)) && br) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(pb, 4, 4), br);
        br->Release();
    }
    if (tot > 0 && state_.trophies_unlocked > 0) {
        const float fw = std::max(8.0f, (pb.right - pb.left) * static_cast<float>(state_.trophies_unlocked) / static_cast<float>(tot));
        fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(pb.left, pb.top, pb.left + fw, pb.bottom), 4, 4), pal.accent_crimson, pal.accent);
    }
    const std::string status_line = state_.ready_to_play
        ? (std::string("Ready: ") + prof.short_name)
        : (state_.exe_present ? "Setup disc (see Tools)" : "Coming Soon — Port Pending");
    draw_text(rt, status_line,
              fmt_small_, D2D1::RectF(card.left + 12, card.top + 74, card.right - 12, card.top + 94),
              state_.ready_to_play ? pal.good : (state_.exe_present ? pal.warn : pal.accent));
    const std::string side_sub = format_playtime(static_cast<float>(state_.playtime.effective_total_seconds)) + "  •  " +
                                 std::to_string(state_.occupied_slots) + " Slots  •  " +
                                 std::to_string(state_.screenshots.size()) + " Shots";
    draw_text(rt, side_sub,
              fmt_small_, D2D1::RectF(card.left + 12, card.top + 92, card.right - 12, card.bottom - 8), pal.text_dim);
}

void LauncherApp::render_home(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const GameProfile &prof = game_profile(active_game_);
    const float x = area.left + 32.0f;
    const float w = area.right - area.left - 64.0f;
    float y = area.top + 36.0f;

    // Eyebrow badge
    const D2D1_RECT_F badge{x, y, x + 236.0f, y + 24.0f};
    fill_hgrad_rounded(rt, D2D1::RoundedRect(badge, 12, 12), with_a(pal.accent_crimson, 0.55f), with_a(pal.accent, 0.30f));
    ID2D1SolidColorBrush *br = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.border_hi, &br)) && br) {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(badge, 12, 12), br, 1.0f);
        br->Release();
    }
    draw_text(rt, state_.exe_present ? "NATIVE WINDOWS PC RECOMPILATION" : "UPCOMING NATIVE PC PORT", fmt_pill_, badge, col_rgba(255, 244, 212),
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    y += 38.0f;

    // Hero Title & Subtitle
    draw_text(rt, prof.title_top, fmt_hero_, D2D1::RectF(x, y, x + 680.0f, y + 42.0f), pal.text);
    y += 40.0f;
    draw_text(rt, prof.title_main, fmt_hero_, D2D1::RectF(x, y, x + 680.0f, y + 44.0f), pal.accent);
    y += 52.0f;
    draw_text(rt, prof.tagline,
              fmt_body_, D2D1::RectF(x, y, x + 680.0f, y + 26.0f), pal.text_dim);
    y += 38.0f;

    // Status Pills Row
    const auto pill = [&](float px, const std::string &txt, D2D1_COLOR_F accent_c) -> float {
        const float pw = std::max(96.0f, static_cast<float>(txt.size()) * 7.2f + 22.0f);
        const D2D1_RECT_F pr{px, y, px + pw, y + 26.0f};
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(16, 16, 28, 0.85f), &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(pr, 13, 13), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(accent_c, 0.7f), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pr, 13, 13), b, 1.0f);
            b->Release();
        }
        draw_text(rt, txt, fmt_small_, pr, accent_c, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return px + pw + 10.0f;
    };

    float px = x;
    const std::string d1_ok_lbl = (prof.disc_count == 4) ? "4 Discs Verified"
                                : (prof.disc_count == 2) ? "Disc 1 Verified"
                                : "Disc Verified";
    const std::string d1_miss_lbl = (prof.disc_count == 4) ? "4 Discs Missing"
                                  : (prof.disc_count == 2) ? "Disc 1 Missing"
                                  : "Disc Missing";
    px = pill(px, !state_.paths.disc1_path.empty() ? d1_ok_lbl : (state_.exe_present ? d1_miss_lbl : "Disc Not Staged"),
              !state_.paths.disc1_path.empty() ? pal.good : (state_.exe_present ? pal.warn : pal.text_dim));
    if (prof.disc_count == 2) {
        px = pill(px, !state_.paths.disc2_path.empty() ? "Disc 2 Verified" : "Disc 2 Optional",
                  !state_.paths.disc2_path.empty() ? pal.good : pal.text_dim);
    }
    const std::string api_lbl = (active_game_ == GameId::Koudelka)
        ? "Direct3D 11 + PGXP"
        : (state_.settings.gpu_api == "vulkan" ? "Vulkan" : "Direct3D 12");
    px = pill(px, api_lbl + "  •  " + std::to_string(state_.settings.scale) + "x  •  " + state_.settings.fps_cap + " FPS", pal.accent);
    px = pill(px, "Playtime: " + format_playtime(static_cast<float>(state_.playtime.effective_total_seconds)), pal.good);
    y += 48.0f;

    // Primary PLAY / COMING SOON Button + Optional Continue Slot / Autosave + Launch Mode Toggle + Quick Screenshots
    const bool has_continue = state_.ready_to_play &&
                              ((state_.latest_slot >= 0 && state_.latest_slot < 10 &&
                                state_.slots[static_cast<std::size_t>(state_.latest_slot)].exists) ||
                               (state_.latest_slot == 10 && state_.autosave_slot.exists));
    const float play_w = has_continue ? 248.0f : 288.0f;
    const D2D1_RECT_F play_r{x, y, x + play_w, y + 58.0f};
    if (state_.ready_to_play) {
        fill_radial_glow(rt, (play_r.left + play_r.right) * 0.5f, (play_r.top + play_r.bottom) * 0.5f,
                         175.0f, 52.0f, with_a(pal.accent_crimson, 0.42f), with_a(pal.accent_crimson, 0.0f));
    }
    const std::string play_lbl = game_running_
        ? "RUNNING..."
        : (state_.ready_to_play ? std::string(prof.play_button_label)
                                : (state_.exe_present ? "SETUP DISC" : "COMING SOON"));
    const D2D1_COLOR_F play_bg = state_.ready_to_play
        ? pal.accent_crimson
        : (state_.exe_present ? col_rgba(52, 46, 56) : lerp_col(pal.card_hover, pal.accent_crimson, 0.22f));
    const D2D1_COLOR_F play_txt = state_.ready_to_play ? col_rgba(255, 250, 236) : pal.accent;
    draw_button(rt, "home_play", play_r, play_lbl, play_bg,
                pal.border_hi, play_txt, true, [this]() {
                    if (state_.ready_to_play) {
                        launch_game(-1);
                    } else if (!state_.exe_present) {
                        audio_.play(SoundCue::Click);
                        show_toast(game_profile(active_game_).coming_soon_msg);
                    } else {
                        audio_.play(SoundCue::TabSwitch);
                        page_ = Page::Tools;
                    }
                });

    float next_x = play_r.right + 12.0f;
    if (has_continue) {
        const int cslot = state_.latest_slot;
        const std::string cont_lbl = (cslot == 10) ? "CONTINUE AUTOSAVE" : ("CONTINUE SLOT " + std::to_string(cslot));
        const D2D1_RECT_F cont_r{next_x, y + 4.0f, next_x + 216.0f, y + 54.0f};
        draw_button(rt, "home_continue", cont_r,
                    cont_lbl,
                    with_a(pal.accent_crimson, 0.62f), pal.border_hi, col_rgba(255, 246, 224), false,
                    [this, cslot]() { launch_game(cslot); });
        next_x = cont_r.right + 12.0f;
    }

    const D2D1_RECT_F mode_r{next_x, y + 6.0f, next_x + 154.0f, y + 52.0f};
    draw_button(rt, "home_mode", mode_r,
                state_.settings.launcher_fullscreen ? "Mode: Fullscreen" : "Mode: Windowed",
                pal.card_bg, pal.border, pal.text, false, [this]() {
                    state_.settings.launcher_fullscreen = !state_.settings.launcher_fullscreen;
                    persist_settings(state_.settings.launcher_fullscreen ? "Launch mode: Fullscreen" : "Launch mode: Windowed");
                });

    const D2D1_RECT_F dir_r{mode_r.right + 12.0f, y + 6.0f, mode_r.right + 142.0f, y + 52.0f};
    draw_button(rt, "home_shots", dir_r, "Screenshots", pal.card_bg, pal.border, pal.text_dim, false, [this]() {
        audio_.play(SoundCue::TabSwitch);
        page_ = Page::SavesMedia;
        saves_subtab_ = 1;
    });

    // Bottom 3 Glass Command Cards
    const float cards_y = area.bottom - 188.0f;
    const float cards_h = 158.0f;
    const float gap = 16.0f;
    const float cw = (w - gap * 2.0f) / 3.0f;

    const auto draw_card_frame = [&](const std::string &id, float cx0, const std::string &eyebrow,
                                     const std::string &headline, const std::string &line1, const std::string &line2,
                                     std::function<void()> on_click) {
        const D2D1_RECT_F cr{cx0, cards_y, cx0 + cw, cards_y + cards_h};
        const bool hov = (hovered_id_ == id);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(cr, 11.0f, 11.0f);
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? pal.card_hover : pal.card_bg, &b)) && b) {
            rt->FillRoundedRectangle(rr, b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? pal.border_hi : pal.border, &b)) && b) {
            rt->DrawRoundedRectangle(rr, b, hov ? 1.5f : 1.0f);
            b->Release();
        }
        draw_text(rt, eyebrow, fmt_pill_, D2D1::RectF(cr.left + 16, cr.top + 14, cr.right - 16, cr.top + 30), pal.accent);
        draw_text(rt, headline, fmt_h2_, D2D1::RectF(cr.left + 16, cr.top + 36, cr.right - 16, cr.top + 64), pal.text);
        draw_text(rt, line1, fmt_body_, D2D1::RectF(cr.left + 16, cr.top + 72, cr.right - 16, cr.top + 96), pal.text_dim);
        draw_text(rt, line2, fmt_small_, D2D1::RectF(cr.left + 16, cr.top + 98, cr.right - 16, cr.top + 122), pal.text_dim);
        draw_text(rt, "Open ->", fmt_small_, D2D1::RectF(cr.left + 16, cr.bottom - 28, cr.right - 16, cr.bottom - 10),
                  hov ? pal.accent : pal.text_dim, DWRITE_TEXT_ALIGNMENT_TRAILING);
        hits_.push_back({id, cr, std::move(on_click), nullptr, false});
    };

    const int tot = static_cast<int>(state_.trophies.size());
    if (tot > 0) {
        draw_card_frame("home_card_trophies", x, "TROPHY GALLERY",
                        std::to_string(state_.trophies_unlocked) + " / " + std::to_string(tot) + " Unlocked",
                        "Best Ring Streak: " + std::to_string(state_.best_ring_streak) + "  •  Perfects: " + std::to_string(state_.best_perfect_streak),
                        "Mark " + std::to_string(state_.tier_unlocked[0]) + "/" + std::to_string(state_.tier_total[0]) +
                        "  •  Curiosities " + std::to_string(state_.tier_unlocked[6]) + "/" + std::to_string(state_.tier_total[6]),
                        [this]() { audio_.play(SoundCue::TabSwitch); page_ = Page::Trophies; });
    } else {
        draw_card_frame("home_card_trophies", x, "TROPHY GALLERY",
                        "Chronicle In Progress",
                        "Achievements awaken with the native PC port",
                        std::string("Era: ") + prof.era_subtitle + "  •  " + prof.serials,
                        [this]() { audio_.play(SoundCue::TabSwitch); page_ = Page::Trophies; });
    }

    std::string save_head = "No Save States Yet";
    std::string save_l1 = "Press F5 in-game to save Slots 0-9";
    std::string save_l2 = "Memory card: " + state_.paths.memcard_path.filename().string() + " ready";
    if (state_.latest_slot >= 0) {
        const auto &sl = (state_.latest_slot == 10) ? state_.autosave_slot : state_.slots[static_cast<std::size_t>(state_.latest_slot)];
        save_head = (state_.latest_slot == 10 ? "Autosave" : ("Slot " + std::to_string(sl.slot))) +
                    (sl.area.empty() ? "" : (" — " + sl.area));
        save_l1 = "Disc " + std::to_string(sl.disc) + (sl.max_level > 0 ? ("  •  Lv " + std::to_string(sl.max_level)) : "") +
                  "  •  " + format_playtime(sl.playtime_sec);
        save_l2 = sl.timestamp + "  (" + format_bytes(sl.bytes) + ")";
    }
    draw_card_frame("home_card_saves", x + cw + gap, "SAVES & MOMENTS", save_head, save_l1, save_l2,
                    [this]() { audio_.play(SoundCue::TabSwitch); page_ = Page::SavesMedia; });

    static const char *const kAspectLbl[7] = {"4:3 Classic", "16:9 Wide", "Stretch", "Auto Window", "16:10 Wide", "21:9 Ultrawide", "32:9 Superwide"};
    const std::string light_summary = (state_.settings.ssao || state_.settings.bloom || state_.settings.ssgi || state_.settings.filmic)
        ? ("SSAO " + std::to_string(state_.settings.ssao) + "% • Bloom " + std::to_string(state_.settings.bloom) + "%")
        : "Authentic PS2 Lighting (Post-FX Off)";
    draw_card_frame("home_card_gfx", x + (cw + gap) * 2.0f, "VISUAL PROFILE",
                    std::to_string(state_.settings.scale) + "x Internal  •  " + kAspectLbl[std::clamp(state_.settings.aspect, 0, 6)],
                    api_lbl + " • " + state_.settings.present_mode + " • " + state_.settings.fps_cap + " FPS",
                    light_summary,
                    [this]() { audio_.play(SoundCue::TabSwitch); page_ = Page::Settings; });
}

void LauncherApp::render_settings(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const float pad = 26.0f;
    const float x = area.left + pad;
    const float w = area.right - area.left - pad * 2.0f;
    const float header_h = 56.0f;

    draw_text(rt, "Pre-Launch Settings", fmt_h1_, D2D1::RectF(x, area.top + 14, x + 420, area.top + 44), pal.accent);
    draw_text(rt, "Changes sync immediately with games/" + std::string(game_profile(active_game_).slug) + "/keys.ini (comments & keybinds preserved).",
              fmt_small_, D2D1::RectF(x, area.top + 42, x + 640, area.top + 60), pal.text_dim);

    draw_button(rt, "set_edit_ini", D2D1::RectF(area.right - pad - 282.0f, area.top + 18.0f, area.right - pad - 160.0f, area.top + 50.0f),
                "Edit keys.ini", pal.card_bg, pal.border, pal.accent, false, [this]() {
                    persist_settings();
                    ShellExecuteW(hwnd_, L"open", state_.paths.keys_ini.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                });

    draw_button(rt, "set_reset", D2D1::RectF(area.right - pad - 150.0f, area.top + 18.0f, area.right - pad, area.top + 50.0f),
                "Reset Defaults", pal.card_bg, pal.border, pal.text_dim, false, [this]() {
                    state_.settings = Settings{};
                    audio_.set_enabled(state_.settings.launcher_chimes);
                    audio_.set_volume(state_.settings.chime_volume);
                    persist_settings("Settings reset to defaults");
                });

    const D2D1_RECT_F view{x, area.top + header_h + 14.0f, x + w - 14.0f, area.bottom - 18.0f};
    const float view_h = view.bottom - view.top;
    const float content_h = 1650.0f;
    settings_max_scroll_ = std::max(0.0f, content_h - view_h);
    settings_scroll_ = std::clamp(settings_scroll_, 0.0f, settings_max_scroll_);

    rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    has_clip_rect_ = true;
    active_clip_rect_ = view;

    const float col_gap = 18.0f;
    const float full_w = view.right - view.left;
    const float cw = (full_w - col_gap) * 0.5f;
    const float lx = view.left;
    const float rx = view.left + cw + col_gap;
    const float preset_y = view.top - settings_scroll_;
    const float base_y = preset_y + 92.0f;

    const auto panel = [&](float px, float py, float pw, float ph, const std::string &title) {
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(px, py, px + pw, py + ph), 11, 11);
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &b)) && b) {
            rt->FillRoundedRectangle(rr, b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &b)) && b) {
            rt->DrawRoundedRectangle(rr, b, 1.0f);
            b->Release();
        }
        draw_text(rt, title, fmt_pill_, D2D1::RectF(px + 14, py + 12, px + pw - 14, py + 28), pal.accent);
    };

    // Top 1-Click Performance Presets Bar (78px)
    panel(lx, preset_y, full_w, 78.0f, "1-CLICK PERFORMANCE PRESETS");
    {
        static const char *const kPresetLabels[4] = {"Nocturne Turbo", "Balanced 1080p", "Authentic PS2", "4K Ultra"};
        const float pgap = 10.0f;
        const float bw = (full_w - 28.0f - pgap * 3.0f) / 4.0f;
        const auto apply_preset = [this](int idx) {
            auto &s = state_.settings;
            s.gpu_api = "vulkan";
            s.fps_cap = "60";
            s.fps_speed_sync = true;
            s.gpu_binning = true;
            s.deinterlace = true;
            s.audio_stretch = true;
            s.scanlines = 0;
            s.ssgi = 0;
            s.color_temp = 0;
            s.film_grain = 0;
            s.crt_mask = 0;
            s.brightness = 0;
            s.contrast = 100;
            s.gamma = 100;
            if (idx == 0) {
                s.present_mode = "Immediate"; s.scale = 4; s.aspect = 1; s.true_widescreen = true;
                s.look = "sharp_edges"; s.sharp_2d = "sharpest"; s.sharp_3d = false;
                s.fsr_upscale = 1; s.cas_sharpen = 35; s.aa_mode = 1; s.lod_bias = -5;
                s.ssao = 20; s.bloom = 0; s.filmic = false; s.vignette = 0; s.saturation = 110;
                s.adaptive_scale = true; s.perf_hud = true;
                persist_settings("Applied preset: Nocturne Turbo (Vulkan 4x + FSR Quality + CAS)");
            } else if (idx == 1) {
                s.present_mode = "Mailbox"; s.scale = 3; s.aspect = 1; s.true_widescreen = true;
                s.look = "sharp_edges"; s.sharp_2d = "sharpest"; s.sharp_3d = false;
                s.fsr_upscale = 0; s.cas_sharpen = 25; s.aa_mode = 1; s.lod_bias = -5;
                s.ssao = 0; s.bloom = 0; s.filmic = false; s.vignette = 0; s.saturation = 100;
                s.adaptive_scale = true; s.perf_hud = false;
                persist_settings("Applied preset: Balanced 1080p (Vulkan 3x + FXAA Fast)");
            } else if (idx == 2) {
                s.present_mode = "Mailbox"; s.scale = 1; s.aspect = 0; s.true_widescreen = false;
                s.look = "soft"; s.sharp_2d = "soft"; s.sharp_3d = false;
                s.fsr_upscale = 0; s.cas_sharpen = 0; s.aa_mode = 0; s.lod_bias = 0;
                s.ssao = 0; s.bloom = 0; s.filmic = false; s.vignette = 0; s.saturation = 100;
                s.adaptive_scale = false; s.perf_hud = false;
                persist_settings("Applied preset: Authentic PS2 (4:3 1x)");
            } else {
                s.present_mode = "Mailbox"; s.scale = 6; s.aspect = 1; s.true_widescreen = true;
                s.look = "sharp_edges"; s.sharp_2d = "sharpest"; s.sharp_3d = true;
                s.fsr_upscale = 2; s.cas_sharpen = 40; s.aa_mode = 2; s.lod_bias = -8;
                s.ssao = 30; s.bloom = 20; s.filmic = true; s.vignette = 15; s.saturation = 105;
                s.adaptive_scale = true; s.perf_hud = false;
                persist_settings("Applied preset: 4K Ultra (Vulkan 6x + FSR Ultra + FXAA Quality)");
            }
        };
        for (int i = 0; i < 4; ++i) {
            const float bx = lx + 14.0f + static_cast<float>(i) * (bw + pgap);
            const D2D1_RECT_F br{bx, preset_y + 34.0f, bx + bw, preset_y + 66.0f};
            const bool is_turbo = (i == 0);
            draw_button(rt, "set_preset_" + std::to_string(i), br, kPresetLabels[i],
                        is_turbo ? with_a(pal.accent_crimson, 0.65f) : pal.card_hover,
                        is_turbo ? pal.border_hi : pal.border,
                        is_turbo ? col_rgba(255, 248, 232) : pal.text, false,
                        [apply_preset, i]() { apply_preset(i); });
        }
    }

    // Left Top: Display & Graphics (760px)
    const float lt_h = 760.0f;
    panel(lx, base_y, cw, lt_h, "DISPLAY & GRAPHICS");
    float cy = base_y + 34.0f;
    draw_slider_row(rt, "set_scale", lx + 6, cy, cw - 12, "Internal Resolution Scale",
                    std::to_string(state_.settings.scale) + "x", static_cast<float>(state_.settings.scale - 1) / 7.0f, pal,
                    [this](float t) {
                        const int v = std::clamp(static_cast<int>(t * 7.0f + 0.5f) + 1, 1, 8);
                        if (v != state_.settings.scale) { state_.settings.scale = v; persist_settings(); }
                    });
    cy += 50.0f;
    {
        static constexpr int kAspectOrder[7] = {3, 0, 4, 1, 5, 6, 2};  // Auto, 4:3, 16:10, 16:9, 21:9, 32:9, Stretch
        int ai = 3;
        for (int i = 0; i < 7; ++i)
            if (kAspectOrder[i] == state_.settings.aspect) ai = i;
        draw_segmented_row(rt, "set_aspect", lx + 6, cy, cw - 12, "Aspect Ratio",
                           {"Auto", "4:3", "16:10", "16:9", "21:9", "32:9", "Fill"}, ai, pal, [this](int idx) {
                               state_.settings.aspect = kAspectOrder[std::clamp(idx, 0, 6)];
                               persist_settings("Aspect ratio updated");
                           });
    }
    cy += 54.0f;
    draw_toggle_row(rt, "set_true_ws", lx + 6, cy, cw - 12, "True 16:9 Camera (Hor+)", "Widens 3D camera FOV & frustum cleanly without stretching",
                    state_.settings.true_widescreen, pal, [this](bool v) {
                        state_.settings.true_widescreen = v;
                        persist_settings(v ? "True 16:9 Camera (Hor+): ON" : "True 16:9 Camera (Hor+): OFF");
                    });
    cy += 48.0f;
    static const std::vector<std::string> kFpsOpts = {"30", "60", "120", "144", "240", "display", "unlimited"};
    static const std::vector<std::string> kFpsLabels = {"30", "60", "120", "144", "240", "Disp", "Max"};
    int fps_i = 1;
    for (int i = 0; i < static_cast<int>(kFpsOpts.size()); ++i)
        if (state_.settings.fps_cap == kFpsOpts[static_cast<std::size_t>(i)]) fps_i = i;
    draw_segmented_row(rt, "set_fps", lx + 6, cy, cw - 12, "Frame Rate Cap", kFpsLabels, fps_i, pal, [this](int idx) {
        state_.settings.fps_cap = kFpsOpts[static_cast<std::size_t>(idx)];
        persist_settings("FPS cap set to " + state_.settings.fps_cap);
    });
    cy += 56.0f;
    draw_segmented_row(rt, "set_api", lx + 6, cy, cw - 12, "Graphics API (Vulkan recommended on Windows)", {"Vulkan", "Direct3D 12"},
                       state_.settings.gpu_api == "dx12" ? 1 : 0, pal, [this](int idx) {
                           state_.settings.gpu_api = (idx == 1) ? "dx12" : "vulkan";
                           persist_settings("Graphics API updated");
                       });
    cy += 56.0f;
    int pm_i = state_.settings.present_mode == "Fifo" ? 1 : (state_.settings.present_mode == "Immediate" ? 2 : 0);
    draw_segmented_row(rt, "set_pm", lx + 6, cy, cw - 12, "VSync / Present Mode", {"Mailbox", "Fifo", "Immediate"}, pm_i, pal, [this](int idx) {
        static const char *const kPM[3] = {"Mailbox", "Fifo", "Immediate"};
        state_.settings.present_mode = kPM[idx];
        persist_settings("Present mode updated");
    });
    cy += 56.0f;
    draw_toggle_row(rt, "set_bin", lx + 6, cy, cw - 12, "GPU Tile Binning", "2.5x faster 4x+ rendering in GPU shared memory",
                    state_.settings.gpu_binning, pal, [this](bool v) {
                        state_.settings.gpu_binning = v;
                        persist_settings("GPU Tile Binning updated");
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_adapt", lx + 6, cy, cw - 12, "Adaptive Battle Scale", "Live GPU 4x -> 3x step-down only under heavy battle load",
                    state_.settings.adaptive_scale, pal, [this](bool v) {
                        state_.settings.adaptive_scale = v;
                        persist_settings(v ? "Adaptive Battle Scale: ON" : "Adaptive Battle Scale: OFF");
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_hud", lx + 6, cy, cw - 12, "Fullscreen Corner FPS HUD (F10)", "Compact FPS, frame-time, scale & Turbo pill in top-left corner",
                    state_.settings.perf_hud, pal, [this](bool v) {
                        state_.settings.perf_hud = v;
                        persist_settings(v ? "Corner FPS HUD: ON" : "Corner FPS HUD: OFF");
                    });
    cy += 46.0f;
    if (active_game_ == GameId::Koudelka) {
        draw_segmented_row(rt, "set_kdl_rend", lx + 6, cy, cw - 12, "Koudelka Renderer",
                           {"D3D11 HW", "CPU Soft"}, state_.settings.renderer == 0 ? 1 : 0, pal,
                           [this](int idx) {
                               state_.settings.renderer = (idx == 1) ? 0 : 1;
                               persist_settings(idx == 1 ? "Koudelka Renderer: CPU Software" : "Koudelka Renderer: D3D11 Hardware");
                           });
        cy += 54.0f;
        draw_toggle_row(rt, "set_kdl_pgxp", lx + 6, cy, cw - 12, "PGXP Geometry Precision",
                        "Sub-pixel floating-point vertex snapping eliminates PS1 polygon wobble",
                        state_.settings.pgxp, pal, [this](bool v) {
                            state_.settings.pgxp = v;
                            persist_settings(v ? "PGXP Precision: ON" : "PGXP Precision: OFF");
                        });
        cy += 46.0f;
        draw_segmented_row(rt, "set_kdl_f3d", lx + 6, cy, cw - 12, "3D Texture Filter",
                           {"Nearest", "Bilinear", "Sharp"}, std::clamp(state_.settings.filter_3d, 0, 2), pal,
                           [this](int idx) {
                               state_.settings.filter_3d = std::clamp(idx, 0, 2);
                               persist_settings("Koudelka 3D Filter updated");
                           });
        cy += 54.0f;
        draw_segmented_row(rt, "set_kdl_preset", lx + 6, cy, cw - 12, "Koudelka Visual Preset",
                           {"Raw PS1", "Noir", "Crisp", "Vibrant"}, std::clamp(state_.settings.visual_preset, 0, 3), pal,
                           [this](int idx) {
                               state_.settings.visual_preset = idx;
                               persist_settings("Koudelka Visual Preset updated");
                           });
        cy += 54.0f;
        draw_toggle_row(rt, "set_kdl_dedither", lx + 6, cy, cw - 12, "24-Bit Smooth De-Dither",
                        "Removes PS1 15-bit checkerboard dithering for smooth HD gradients",
                        state_.settings.dedither, pal, [this](bool v) {
                            state_.settings.dedither = v;
                            persist_settings(v ? "24-Bit De-Dither: ON" : "24-Bit De-Dither: OFF");
                        });
    } else {
        draw_segmented_row(rt, "set_hud_anchor", lx + 6, cy, cw - 12, "2D HUD & Judgment Ring Pillar-Anchor",
                           {"Off", "Ultrawide", "All Wide"}, std::clamp(state_.settings.hud_anchor, 0, 2), pal,
                           [this](int idx) {
                               state_.settings.hud_anchor = idx;
                               persist_settings("2D HUD & Judgment Ring Pillar-Anchor updated");
                           });
        cy += 54.0f;
        const int ai = (state_.settings.anisotropy >= 16) ? 3 : (state_.settings.anisotropy >= 8 ? 2 : (state_.settings.anisotropy >= 4 ? 1 : 0));
        draw_segmented_row(rt, "set_aniso", lx + 6, cy, cw - 12, "Anisotropic Texture Filtering",
                           {"Off", "4x", "8x", "16x"}, ai, pal, [this](int idx) {
                               static constexpr int kAniso[4] = {1, 4, 8, 16};
                               state_.settings.anisotropy = kAniso[std::clamp(idx, 0, 3)];
                               persist_settings("Anisotropic Filtering updated");
                           });
        cy += 54.0f;
        char lod_buf[24];
        std::snprintf(lod_buf, sizeof(lod_buf), "%+.1f", static_cast<double>(state_.settings.lod_bias) * 0.1);
        draw_slider_row(rt, "set_lod_bias", lx + 6, cy, cw - 12, "GS Mipmap LOD Bias",
                        lod_buf, static_cast<float>(state_.settings.lod_bias + 20) / 40.0f, pal,
                        [this](float t) {
                            state_.settings.lod_bias = std::clamp(-20 + static_cast<int>(t * 40.0f + 0.5f), -20, 20);
                            persist_settings();
                        });
        cy += 48.0f;
        draw_toggle_row(rt, "set_gpu_submit", lx + 6, cy, cw - 12, "Async GPU Submit Thread",
                        "Decouples EE emulation from GPU frame presentation",
                        state_.settings.gpu_submit_thread, pal, [this](bool v) {
                            state_.settings.gpu_submit_thread = v;
                            persist_settings(v ? "Async GPU Submit: ON" : "Async GPU Submit: OFF");
                        });
        cy += 46.0f;
        draw_toggle_row(rt, "set_title_perf", lx + 6, cy, cw - 12, "Titlebar Performance Telemetry",
                        "Show live FPS & GPU timing metrics in the window titlebar",
                        state_.settings.title_perf, pal, [this](bool v) {
                            state_.settings.title_perf = v;
                            persist_settings(v ? "Titlebar Telemetry: ON" : "Titlebar Telemetry: OFF");
                        });
    }

    // Left Middle: Launcher, Turbo & Disc Controls (424px)
    const float lb_y = base_y + lt_h + 16.0f;
    panel(lx, lb_y, cw, 424.0f, "LAUNCHER, TURBO & DISC BEHAVIOR");
    cy = lb_y + 34.0f;
    draw_toggle_row(rt, "set_hide", lx + 6, cy, cw - 12, "Hide Launcher While Playing", "0% CPU/GPU during gameplay, auto-restores on exit",
                    state_.settings.launcher_hide_on_play, pal, [this](bool v) {
                        state_.settings.launcher_hide_on_play = v;
                        persist_settings(v ? "Launcher will hide & auto-restore" : "Launcher will close on Play");
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_auto_enter", lx + 6, cy, cw - 12, "Auto-Enter Last Game on Startup", "OFF (default) always greets you with the Trilogy Triptych",
                    global_cfg_.auto_enter_last_game, pal, [this](bool v) {
                        global_cfg_.auto_enter_last_game = v;
                        save_global_config(state_.paths.root_dir, global_cfg_);
                        show_toast(v ? "Startup: Auto-enter last selected game" : "Startup: Always show Trilogy Triptych");
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_fast", lx + 6, cy, cw - 12, "Fast SSD Disc Loading", "~16x faster area loads (OFF for authentic PS2 DVD speed)",
                    state_.settings.fast_disc, pal, [this](bool v) {
                        state_.settings.fast_disc = v;
                        persist_settings("Fast disc loading updated");
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_autosave", lx + 6, cy, cw - 12, "Area-Transition Autosave", "Rolling safety save on new area entry (never overwrites Slots 0-9)",
                    state_.settings.autosave, pal, [this](bool v) {
                        state_.settings.autosave = v;
                        persist_settings(v ? "Area-Transition Autosave: ON" : "Area-Transition Autosave: OFF");
                    });
    cy += 46.0f;
    {
        static constexpr int kSp[5] = {15, 2, 3, 4, 0};
        int ti = 2;
        for (int i = 0; i < 5; ++i)
            if (kSp[i] == state_.settings.turbo_speed) ti = i;
        draw_segmented_row(rt, "set_turbo", lx + 6, cy, cw - 12, "Turbo Speed (Hold Tab / F3 / Select+R2)", {"1.5x", "2x", "3x", "4x", "Max"}, ti, pal, [this](int idx) {
            state_.settings.turbo_speed = kSp[std::clamp(idx, 0, 4)];
            persist_settings("Turbo speed updated");
        });
    }
    cy += 56.0f;
    draw_toggle_row(rt, "set_herobg", lx + 6, cy, cw - 12, "Screenshot Hero Backdrop", "Crossfade latest captured screenshot softly on Home",
                    state_.settings.launcher_hero_screenshot, pal, [this](bool v) {
                        state_.settings.launcher_hero_screenshot = v;
                        persist_settings("Hero backdrop preference saved");
                    });
    cy += 46.0f;
    draw_segmented_row(rt, "set_theme", lx + 6, cy, cw - 12, "Nocturne Palette & Toast Theme",
                       {"Tier", "Eclipse", "Crimson", "Grimoire"}, state_.settings.toast_theme, pal, [this](int idx) {
                           state_.settings.toast_theme = idx;
                           persist_settings(std::string("Palette: ") + achievements::catalog_toast_theme_name(idx));
                       });

    // Left Bottom: Advanced Engine & GPU Expert Controls (304px)
    const float le_y = lb_y + 424.0f + 16.0f;
    panel(lx, le_y, cw, 304.0f, "ADVANCED ENGINE & GPU EXPERT CONTROLS");
    cy = le_y + 34.0f;
    draw_toggle_row(rt, "set_hw_rast", lx + 6, cy, cw - 12, "Hardware Rasterizer (PS2_GPU_HW)",
                    "GPU-accelerated triangle rasterizer (OFF falls back to compute)",
                    state_.settings.hw_rasterizer, pal, [this](bool v) {
                        state_.settings.hw_rasterizer = v;
                        persist_settings(v ? "Hardware Rasterizer: ON" : "Hardware Rasterizer: OFF");
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_dual_blend", lx + 6, cy, cw - 12, "Dual-Source Alpha Blending",
                    "Exact PS2 GS alpha-blend math on supporting Vulkan/D3D12 GPUs",
                    state_.settings.dual_source_blend, pal, [this](bool v) {
                        state_.settings.dual_source_blend = v;
                        persist_settings(v ? "Dual-Source Blending: ON" : "Dual-Source Blending: OFF");
                    });
    cy += 46.0f;
    {
        static constexpr int kVramMB[5] = {0, 512, 1024, 2048, 4096};
        int vi = 0;
        for (int i = 0; i < 5; ++i)
            if (kVramMB[i] == state_.settings.vram_budget) vi = i;
        draw_segmented_row(rt, "set_vram_budget", lx + 6, cy, cw - 12, "VRAM Texture Cache Budget",
                           {"Auto", "512M", "1G", "2G", "4G"}, vi, pal, [this](int idx) {
                               state_.settings.vram_budget = kVramMB[std::clamp(idx, 0, 4)];
                               persist_settings("VRAM Texture Cache Budget updated");
                           });
    }
    cy += 56.0f;
    draw_toggle_row(rt, "set_vram_guard", lx + 6, cy, cw - 12, "VRAM Budget Guard",
                    "Evicts stale HD textures proactively to prevent OOM stutters",
                    state_.settings.vram_guard, pal, [this](bool v) {
                        state_.settings.vram_guard = v;
                        persist_settings(v ? "VRAM Budget Guard: ON" : "VRAM Budget Guard: OFF");
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_hot_reload", lx + 6, cy, cw - 12, "Live Texture Pack Hot-Reload",
                    "Automatically reloads edited PNG replacement textures while playing",
                    state_.settings.tex_hot_reload, pal, [this](bool v) {
                        state_.settings.tex_hot_reload = v;
                        persist_settings(v ? "Texture Hot-Reload: ON" : "Texture Hot-Reload: OFF");
                    });

    // Right Top: Next-Gen Upscaling & Anti-Aliasing (256px)
    const float rt_h = 256.0f;
    panel(rx, base_y, cw, rt_h, "NEXT-GEN UPSCALING & ANTI-ALIASING");
    cy = base_y + 34.0f;
    draw_segmented_row(rt, "set_fsr", rx + 6, cy, cw - 12, "AMD FSR 1.0 Spatial Upscaling (EASU)",
                       {"Off", "Quality", "Ultra"}, std::clamp(state_.settings.fsr_upscale, 0, 2), pal, [this](int idx) {
                           state_.settings.fsr_upscale = idx;
                           persist_settings("AMD FSR 1.0 updated");
                       });
    cy += 56.0f;
    draw_slider_row(rt, "set_cas", rx + 6, cy, cw - 12, "AMD CAS Adaptive Sharpening",
                    std::to_string(state_.settings.cas_sharpen) + "%", static_cast<float>(state_.settings.cas_sharpen) / 100.0f, pal,
                    [this](float t) { state_.settings.cas_sharpen = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    draw_segmented_row(rt, "set_aa", rx + 6, cy, cw - 12, "FXAA Edge Anti-Aliasing",
                       {"Off", "Fast", "Quality"}, std::clamp(state_.settings.aa_mode, 0, 2), pal, [this](int idx) {
                           state_.settings.aa_mode = idx;
                           persist_settings("FXAA Anti-Aliasing updated");
                       });
    cy += 56.0f;
    draw_toggle_row(rt, "set_s3d", rx + 6, cy, cw - 12, "Sharp-Bilinear 3D Textures", "Crisper texels on 3D characters & environments (zero blur)",
                    state_.settings.sharp_3d, pal, [this](bool v) { state_.settings.sharp_3d = v; persist_settings("Sharp-Bilinear 3D updated"); });

    // Right Middle: Depth-Aware Lighting & Image (740px)
    const float rm_y = base_y + rt_h + 16.0f;
    const float rm_h = 740.0f;
    panel(rx, rm_y, cw, rm_h, "DEPTH-AWARE LIGHTING & IMAGE");
    cy = rm_y + 34.0f;
    draw_slider_row(rt, "set_ssao", rx + 6, cy, cw - 12, "SSAO Ambient Occlusion",
                    std::to_string(state_.settings.ssao) + "%", static_cast<float>(state_.settings.ssao) / 100.0f, pal,
                    [this](float t) { state_.settings.ssao = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    draw_slider_row(rt, "set_bloom", rx + 6, cy, cw - 12, "HDR Bloom Glow",
                    std::to_string(state_.settings.bloom) + "%", static_cast<float>(state_.settings.bloom) / 100.0f, pal,
                    [this](float t) { state_.settings.bloom = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    draw_slider_row(rt, "set_ssgi", rx + 6, cy, cw - 12, "SSGI Light Bounce",
                    std::to_string(state_.settings.ssgi) + "%", static_cast<float>(state_.settings.ssgi) / 100.0f, pal,
                    [this](float t) { state_.settings.ssgi = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    draw_slider_row(rt, "set_vig", rx + 6, cy, cw - 12, "Gothic Vignette",
                    std::to_string(state_.settings.vignette) + "%", static_cast<float>(state_.settings.vignette) / 100.0f, pal,
                    [this](float t) { state_.settings.vignette = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    char brt_buf[16];
    std::snprintf(brt_buf, sizeof(brt_buf), "%+d", state_.settings.brightness);
    draw_slider_row(rt, "set_bright", rx + 6, cy, cw - 12, "Brightness",
                    brt_buf, static_cast<float>(state_.settings.brightness + 50) / 100.0f, pal,
                    [this](float t) { state_.settings.brightness = std::clamp(-50 + static_cast<int>(t * 20.0f + 0.5f) * 5, -50, 50); persist_settings(); });
    cy += 48.0f;
    draw_slider_row(rt, "set_contrast", rx + 6, cy, cw - 12, "Contrast",
                    std::to_string(state_.settings.contrast) + "%", static_cast<float>(state_.settings.contrast - 50) / 100.0f, pal,
                    [this](float t) { state_.settings.contrast = std::clamp(50 + static_cast<int>(t * 20.0f + 0.5f) * 5, 50, 150); persist_settings(); });
    cy += 48.0f;
    char gam_buf[16];
    std::snprintf(gam_buf, sizeof(gam_buf), "%.2f", static_cast<double>(state_.settings.gamma) * 0.01);
    draw_slider_row(rt, "set_gamma", rx + 6, cy, cw - 12, "Gamma",
                    gam_buf, static_cast<float>(state_.settings.gamma - 50) / 150.0f, pal,
                    [this](float t) {
                        state_.settings.gamma = std::clamp(50 + static_cast<int>(t * 30.0f + 0.5f) * 5, 50, 200);
                        persist_settings();
                    });
    cy += 48.0f;
    draw_slider_row(rt, "set_sat", rx + 6, cy, cw - 12, "Color Saturation",
                    std::to_string(state_.settings.saturation) + "%", static_cast<float>(state_.settings.saturation) / 200.0f, pal,
                    [this](float t) { state_.settings.saturation = std::clamp(static_cast<int>(t * 40.0f + 0.5f) * 5, 0, 200); persist_settings(); });
    cy += 48.0f;
    char tmp_buf[24];
    std::snprintf(tmp_buf, sizeof(tmp_buf), "%+d", state_.settings.color_temp);
    draw_slider_row(rt, "set_temp", rx + 6, cy, cw - 12, "Gothic Color Temperature",
                    tmp_buf, static_cast<float>(state_.settings.color_temp + 50) / 100.0f, pal,
                    [this](float t) { state_.settings.color_temp = std::clamp(-50 + static_cast<int>(t * 20.0f + 0.5f) * 5, -50, 50); persist_settings(); });
    cy += 48.0f;
    draw_slider_row(rt, "set_grain", rx + 6, cy, cw - 12, "Animated Film Grain",
                    std::to_string(state_.settings.film_grain) + "%", static_cast<float>(state_.settings.film_grain) / 100.0f, pal,
                    [this](float t) { state_.settings.film_grain = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    draw_segmented_row(rt, "set_crt_mask", rx + 6, cy, cw - 12, "CRT Phosphor Mask",
                       {"Off", "Aperture", "Shadow"}, std::clamp(state_.settings.crt_mask, 0, 2), pal, [this](int idx) {
                           state_.settings.crt_mask = idx;
                           persist_settings("CRT Phosphor Mask updated");
                       });
    cy += 56.0f;
    draw_toggle_row(rt, "set_filmic", rx + 6, cy, cw - 12, "Filmic Tone Mapping", "Rich cinematic contrast curve in blit.wgsl",
                    state_.settings.filmic, pal, [this](bool v) { state_.settings.filmic = v; persist_settings("Filmic tone map updated"); });
    cy += 46.0f;
    int s2d_i = state_.settings.sharp_2d == "soft" ? 0 : (state_.settings.sharp_2d == "sharp" ? 1 : 2);
    draw_segmented_row(rt, "set_s2d", rx + 6, cy, cw - 12, "2D Text & UI Filtering", {"Soft", "Sharp", "Sharpest"}, s2d_i, pal, [this](int idx) {
        static const char *const kS2D[3] = {"soft", "sharp", "sharpest"};
        state_.settings.sharp_2d = kS2D[idx];
        persist_settings("2D filtering updated");
    });
    cy += 56.0f;
    draw_toggle_row(rt, "set_deint", rx + 6, cy, cw - 12, "Interlaced Field Blend", "Smooths 480i interlaced flicker across odd/even fields",
                    state_.settings.deinterlace, pal, [this](bool v) { state_.settings.deinterlace = v; persist_settings("Deinterlace updated"); });

    // Right Bottom: Audio & Chimes (276px)
    const float rb_y = rm_y + rm_h + 16.0f;
    panel(rx, rb_y, cw, 276.0f, "AUDIO & HARMONIC CHIMES");
    cy = rb_y + 34.0f;
    draw_slider_row(rt, "set_mvol", rx + 6, cy, cw - 12, "Master Volume",
                    std::to_string(state_.settings.master_volume) + "%", static_cast<float>(state_.settings.master_volume) / 100.0f, pal,
                    [this](float t) { state_.settings.master_volume = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    draw_slider_row(rt, "set_bgm", rx + 6, cy, cw - 12, "Music & Voices (SPU2 Core 0)",
                    std::to_string(state_.settings.bgm_volume) + "%", static_cast<float>(state_.settings.bgm_volume) / 100.0f, pal,
                    [this](float t) { state_.settings.bgm_volume = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    draw_slider_row(rt, "set_sfx", rx + 6, cy, cw - 12, "Sound Effects (SPU2 Core 1)",
                    std::to_string(state_.settings.sfx_volume) + "%", static_cast<float>(state_.settings.sfx_volume) / 100.0f, pal,
                    [this](float t) { state_.settings.sfx_volume = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100); persist_settings(); });
    cy += 48.0f;
    draw_slider_row(rt, "set_cvol", rx + 6, cy, cw - 12, "Achievement & Launcher Chimes",
                    std::to_string(state_.settings.chime_volume) + "%", static_cast<float>(state_.settings.chime_volume) / 100.0f, pal,
                    [this](float t) {
                        state_.settings.chime_volume = std::clamp(static_cast<int>(t * 20.0f + 0.5f) * 5, 0, 100);
                        audio_.set_volume(state_.settings.chime_volume);
                        persist_settings();
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_sync", rx + 6, cy, cw - 12, "High-FPS Audio Speed Sync", "Keeps 48 kHz audio real-time above 60 FPS",
                    state_.settings.fps_speed_sync, pal, [this](bool v) { state_.settings.fps_speed_sync = v; persist_settings("Speed sync updated"); });

    has_clip_rect_ = false;
    rt->PopAxisAlignedClip();

    // Subtle scrollbar on the right of Settings when scrollable
    if (settings_max_scroll_ > 1.0f) {
        const D2D1_RECT_F sb{x + w - 8.0f, view.top, x + w, view.bottom};
        ID2D1SolidColorBrush *sbr = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(22, 24, 38, 0.85f), &sbr)) && sbr) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(sb, 4, 4), sbr);
            sbr->Release();
        }
        const float th = std::max(36.0f, view_h * (view_h / content_h));
        const float ty = view.top + (view_h - th) * (settings_scroll_ / settings_max_scroll_);
        fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(sb.left + 1, ty, sb.right - 1, ty + th), 3, 3),
                           pal.accent_crimson, pal.accent);
    }
}

void LauncherApp::render_trophies(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const float pad = 22.0f;
    const float x = area.left + pad;
    const float w = area.right - area.left - pad * 2.0f;
    float y = area.top + 14.0f;

    // Header + Status Filter Pills + Reveal Secrets
    const int tot = static_cast<int>(state_.trophies.size());
    if (tot == 0) {
        const GameProfile &prof = game_profile(active_game_);
        draw_text(rt, "Trophy Gallery", fmt_h1_, D2D1::RectF(x, y, x + 280, y + 30), pal.accent);
        draw_text(rt, std::string(prof.full_title) + "  •  Chronicle Pending",
                  fmt_body_, D2D1::RectF(x + 186, y + 6, x + 640, y + 28), pal.text_dim);
        y += 56.0f;

        const float card_w = std::min(680.0f, w);
        const float card_h = 330.0f;
        const float cx0 = x + (w - card_w) * 0.5f;
        const float cy0 = y + std::max(0.0f, (area.bottom - y - card_h) * 0.35f);
        const D2D1_RECT_F cr{cx0, cy0, cx0 + card_w, cy0 + card_h};
        const D2D1_ROUNDED_RECT crr = D2D1::RoundedRect(cr, 14.0f, 14.0f);
        fill_hgrad_rounded(rt, crr, pal.card_bg, lerp_col(pal.card_bg, pal.bg_bot, 0.45f));
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border_hi, 0.72f), &b)) && b) {
            rt->DrawRoundedRectangle(crr, b, 1.4f);
            b->Release();
        }
        const float mcx = (cr.left + cr.right) * 0.5f;
        const float mcy = cr.top + 78.0f;
        fill_radial_glow(rt, mcx, mcy, 96.0f, 96.0f, with_a(pal.accent_crimson, 0.30f), with_a(pal.accent_crimson, 0.0f));
        draw_medallion_d2d(rt, mcx, mcy, 42.0f, 4, pal.bg_top, pal.accent_crimson, pal.border_hi, pal.accent, false, false);

        draw_text(rt, "CHRONICLE IN PROGRESS", fmt_pill_,
                  D2D1::RectF(cr.left + 24, mcy + 54, cr.right - 24, mcy + 72), pal.accent,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        draw_text(rt, std::string(prof.full_title) + " Achievements", fmt_h1_,
                  D2D1::RectF(cr.left + 24, mcy + 76, cr.right - 24, mcy + 110), pal.text,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        draw_text(rt, std::string("The custom achievement chronicle for ") + prof.full_title +
                      " (" + prof.era_subtitle + ") will awaken alongside its native PC port. "
                      "You can already pre-configure graphics, audio, and disc staging in this hub.",
                  fmt_body_, D2D1::RectF(cr.left + 44, mcy + 118, cr.right - 44, mcy + 178), pal.text_dim,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

        const float bw = 200.0f;
        draw_button(rt, "tr_back_home", D2D1::RectF(mcx - bw - 8.0f, cr.bottom - 56.0f, mcx - 8.0f, cr.bottom - 18.0f),
                    "Back to Home", pal.card_hover, pal.border, pal.text, false, [this]() {
                        audio_.play(SoundCue::TabSwitch);
                        page_ = Page::Home;
                    });
        draw_button(rt, "tr_back_trilogy", D2D1::RectF(mcx + 8.0f, cr.bottom - 56.0f, mcx + bw + 8.0f, cr.bottom - 18.0f),
                    "<- Return to Trilogy", with_a(pal.accent_crimson, 0.55f), pal.border_hi, col_rgba(255, 248, 230), false, [this]() {
                        audio_.play(SoundCue::TabSwitch);
                        return_to_trilogy();
                    });
        return;
    }

    const int pct = tot > 0 ? (state_.trophies_unlocked * 100 / tot) : 0;
    draw_text(rt, "Trophy Gallery", fmt_h1_, D2D1::RectF(x, y, x + 280, y + 30), pal.accent);
    draw_text(rt, std::to_string(state_.trophies_unlocked) + " / " + std::to_string(tot) + " Unlocked  (" + std::to_string(pct) + "%)",
              fmt_body_, D2D1::RectF(x + 186, y + 6, x + 440, y + 28), pal.text_dim);

    float rx = x + w;
    const auto top_pill = [&](const std::string &id, float pw, const std::string &lbl, bool active, D2D1_COLOR_F active_c, std::function<void()> fn) {
        rx -= pw;
        const D2D1_RECT_F r{rx, y + 2, rx + pw, y + 28};
        const bool hov = (hovered_id_ == id);
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(active ? with_a(active_c, 0.35f) : (hov ? pal.card_hover : pal.card_bg), &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, 13, 13), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(active ? active_c : pal.border, &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 13, 13), b, active ? 1.4f : 1.0f);
            b->Release();
        }
        draw_text(rt, lbl, fmt_small_, r, active ? col_rgba(255, 252, 244) : pal.text_dim,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        hits_.push_back({id, r, std::move(fn), nullptr, false});
        rx -= 8.0f;
    };

    top_pill("tr_sec", 122.0f, reveal_secrets_ ? "Secrets: Shown" : "Reveal Secrets", reveal_secrets_, col_rgba(110, 231, 200), [this]() {
        reveal_secrets_ = !reveal_secrets_;
    });
    static const char *const kStLbl[3] = {"All", "Unlocked", "Locked"};
    for (int st = 2; st >= 0; --st) {
        top_pill("tr_st_" + std::to_string(st), st == 0 ? 52.0f : 78.0f, kStLbl[st], trophy_status_ == st, pal.accent, [this, st]() {
            trophy_status_ = st;
            trophy_selected_ = 0;
            trophy_scroll_ = 0;
        });
    }
    y += 40.0f;

    // 8-Tab Tier Strip (All -> Mark -> Sigil -> Crest -> Sovereign -> Follies -> Curiosities -> Covenant)
    const float tgap = 6.0f;
    const float tw = (w - tgap * 7.0f) / 8.0f;
    for (int t = 0; t < 8; ++t) {
        const auto &tab_spec = achievements::kGalleryTierTabs[static_cast<std::size_t>(t)];
        const float tx = x + static_cast<float>(t) * (tw + tgap);
        const D2D1_RECT_F tr{tx, y, tx + tw, y + 32.0f};
        const bool active = (trophy_tier_ == tab_spec.tier_filter);
        const std::string tid = "tr_tier_" + std::to_string(t);
        std::uint32_t gl_u, gr_u, bdr_u, acc_u;
        if (tab_spec.tier_filter < 0) {
            gl_u = achievements::catalog_rgba(56, 40, 24);
            gr_u = achievements::catalog_rgba(136, 94, 40);
            bdr_u = achievements::catalog_rgba(244, 208, 120);
            acc_u = achievements::catalog_rgba(255, 230, 150);
        } else {
            achievements::catalog_theme_colors(static_cast<achievements::Tier>(tab_spec.tier_filter), state_.settings.toast_theme, gl_u, gr_u, bdr_u, acc_u);
        }
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(tr, 7, 7);
        if (active) {
            fill_hgrad_rounded(rt, rr, from_u32(gl_u), from_u32(gr_u));
        } else {
            ID2D1SolidColorBrush *b = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(hovered_id_ == tid ? pal.card_hover : pal.card_bg, &b)) && b) {
                rt->FillRoundedRectangle(rr, b);
                b->Release();
            }
        }
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(active ? from_u32(bdr_u) : with_a(pal.border, 0.5f), &b)) && b) {
            rt->DrawRoundedRectangle(rr, b, active ? 1.5f : 1.0f);
            b->Release();
        }
        const int cu = (tab_spec.tier_filter < 0) ? state_.trophies_unlocked : state_.tier_unlocked[tab_spec.tier_filter];
        const int ct = (tab_spec.tier_filter < 0) ? tot : state_.tier_total[tab_spec.tier_filter];
        draw_text(rt, std::string(tab_spec.label) + " (" + std::to_string(cu) + "/" + std::to_string(ct) + ")",
                  fmt_pill_, tr, active ? col_rgba(255, 252, 242) : lerp_col(from_u32(acc_u), pal.text_dim, 0.6f),
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const int target_filter = tab_spec.tier_filter;
        hits_.push_back({tid, tr, [this, target_filter]() {
            trophy_tier_ = target_filter;
            trophy_selected_ = 0;
            trophy_scroll_ = 0;
        }, nullptr, false});
    }
    y += 42.0f;

    // Build filtered list
    std::vector<std::size_t> filtered;
    for (std::size_t i = 0; i < state_.trophies.size(); ++i) {
        const auto &e = state_.trophies[i];
        if (trophy_tier_ >= 0 && static_cast<int>(e.tier) != trophy_tier_) continue;
        if (trophy_status_ == 1 && !e.unlocked) continue;
        if (trophy_status_ == 2 && e.unlocked) continue;
        filtered.push_back(i);
    }
    const int n = static_cast<int>(filtered.size());
    const int vis = 7;
    trophy_selected_ = n > 0 ? std::clamp(trophy_selected_, 0, n - 1) : 0;
    trophy_scroll_ = n > vis ? std::clamp(trophy_scroll_, 0, n - vis) : 0;

    // Two-column Trophy Deck + Right Showcase Inspector
    const float body_h = area.bottom - y - 16.0f;
    const float col_gap = 16.0f;
    const float left_w = (w - col_gap) * 0.58f;
    const float right_x = x + left_w + col_gap;
    const float right_w = w - left_w - col_gap;
    const float sb_w = 8.0f;
    const float card_w = left_w - sb_w - 8.0f;
    const float card_gap = 8.0f;
    const float card_h = (body_h - card_gap * static_cast<float>(vis - 1)) / static_cast<float>(vis);

    if (n == 0) {
        draw_text(rt, "No trophies match the current filter.", fmt_h2_, D2D1::RectF(x + 16, y + 24, x + card_w, y + 60), pal.text_dim);
    } else {
        for (int r = 0; r < std::min(n, vis); ++r) {
            const int idx = trophy_scroll_ + r;
            const auto &e = state_.trophies[filtered[static_cast<std::size_t>(idx)]];
            const bool sel = (idx == trophy_selected_);
            const bool masked = e.secret && !e.unlocked && !reveal_secrets_;
            const float cy = y + static_cast<float>(r) * (card_h + card_gap);
            const D2D1_RECT_F cr{x, cy, x + card_w, cy + card_h};
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(cr, 9, 9);
            const std::string cid = "tr_card_" + std::to_string(r);

            std::uint32_t gl_u, gr_u, bdr_u, acc_u;
            achievements::catalog_theme_colors(e.tier, state_.settings.toast_theme, gl_u, gr_u, bdr_u, acc_u);
            const D2D1_COLOR_F gl = from_u32(gl_u), gr = from_u32(gr_u), bdr = from_u32(bdr_u), acc = from_u32(acc_u);

            if (sel) {
                fill_hgrad_rounded(rt, rr, lerp_col(gl, pal.card_hover, 0.25f), lerp_col(gr, pal.card_hover, 0.25f));
            } else if (e.unlocked) {
                fill_hgrad_rounded(rt, rr, with_a(gl, 0.72f), with_a(gr, 0.58f));
            } else {
                ID2D1SolidColorBrush *b = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(hovered_id_ == cid ? pal.card_hover : pal.card_bg, &b)) && b) {
                    rt->FillRoundedRectangle(rr, b);
                    b->Release();
                }
            }
            ID2D1SolidColorBrush *b = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(sel ? pal.border_hi : (e.unlocked ? with_a(bdr, 0.75f) : with_a(pal.border, 0.45f)), &b)) && b) {
                rt->DrawRoundedRectangle(rr, b, sel ? 1.8f : 1.0f);
                b->Release();
            }

            // Medallion
            const float mr = std::min(22.0f, card_h * 0.36f);
            draw_medallion_d2d(rt, cr.left + 14.0f + mr, (cr.top + cr.bottom) * 0.5f, mr,
                               static_cast<int>(e.tier), gl, gr, bdr, acc, e.unlocked, masked);

            // Title & Description
            const float tx = cr.left + 26.0f + mr * 2.0f;
            float right_limit = cr.right - 12.0f;
            if (e.unlocked) {
                const std::string dt = e.unlocked_at.size() >= 10 ? e.unlocked_at.substr(0, 10) : "UNLOCKED";
                const D2D1_RECT_F dr{right_limit - 82.0f, cr.top + 8.0f, right_limit, cr.top + 26.0f};
                if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(20, 74, 56, 0.9f), &b)) && b) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(dr, 9, 9), b);
                    b->Release();
                }
                draw_text(rt, dt, fmt_pill_, dr, pal.good, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                right_limit -= 88.0f;
            } else if (e.missable && !masked) {
                const D2D1_RECT_F mr_rect{right_limit - 74.0f, cr.top + 8.0f, right_limit, cr.top + 26.0f};
                if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(92, 46, 16, 0.92f), &b)) && b) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(mr_rect, 9, 9), b);
                    b->Release();
                }
                draw_text(rt, "MISSABLE", fmt_pill_, mr_rect, pal.warn, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                right_limit -= 80.0f;
            } else if (masked) {
                const D2D1_RECT_F sr{right_limit - 62.0f, cr.top + 8.0f, right_limit, cr.top + 26.0f};
                if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(18, 56, 54, 0.9f), &b)) && b) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(sr, 9, 9), b);
                    b->Release();
                }
                draw_text(rt, "SECRET", fmt_pill_, sr, col_rgba(162, 245, 222), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                right_limit -= 68.0f;
            }

            draw_text(rt, masked ? "Secret Achievement" : e.title, fmt_card_title_,
                      D2D1::RectF(tx, cr.top + 8.0f, right_limit, cr.top + 28.0f),
                      sel ? col_rgba(255, 252, 240) : (e.unlocked ? acc : pal.text));
            draw_text(rt, masked ? "Hidden Curiosity — click Reveal Secrets to view hint." : e.description, fmt_small_,
                      D2D1::RectF(tx, cr.top + 30.0f, cr.right - 14.0f, cr.bottom - 8.0f),
                      sel ? col_rgba(232, 232, 242) : pal.text_dim);

            hits_.push_back({cid, cr, [this, idx]() { trophy_selected_ = idx; }, nullptr, false});
        }
    }

    // Scrollbar
    const D2D1_RECT_F sb{x + card_w + 8.0f, y, x + card_w + 8.0f + sb_w, y + body_h};
    ID2D1SolidColorBrush *sbr = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(22, 24, 38, 0.88f), &sbr)) && sbr) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(sb, 4, 4), sbr);
        sbr->Release();
    }
    if (n > vis) {
        const float th = std::max(28.0f, body_h * static_cast<float>(vis) / static_cast<float>(n));
        const float ty = y + (body_h - th) * static_cast<float>(trophy_scroll_) / static_cast<float>(n - vis);
        fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(sb.left + 1, ty, sb.right - 1, ty + th), 3, 3), pal.accent_crimson, pal.accent);
        hits_.push_back({"tr_scroll", D2D1::RectF(sb.left - 4, sb.top, sb.right + 4, sb.bottom), nullptr, [this, n](float t) {
            trophy_scroll_ = std::clamp(static_cast<int>(t * static_cast<float>(n - 7) + 0.5f), 0, std::max(0, n - 7));
        }, true});
    }

    // Right Showcase Inspector Panel
    const D2D1_RECT_F insp{right_x, y, right_x + right_w, y + body_h};
    const D2D1_ROUNDED_RECT irr = D2D1::RoundedRect(insp, 11, 11);
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &sbr)) && sbr) {
        rt->FillRoundedRectangle(irr, sbr);
        sbr->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.65f), &sbr)) && sbr) {
        rt->DrawRoundedRectangle(irr, sbr, 1.2f);
        sbr->Release();
    }

    if (n > 0) {
        const auto &e = state_.trophies[filtered[static_cast<std::size_t>(trophy_selected_)]];
        const bool masked = e.secret && !e.unlocked && !reveal_secrets_;
        std::uint32_t gl_u, gr_u, bdr_u, acc_u;
        achievements::catalog_theme_colors(e.tier, state_.settings.toast_theme, gl_u, gr_u, bdr_u, acc_u);
        const D2D1_COLOR_F gl = from_u32(gl_u), gr = from_u32(gr_u), bdr = from_u32(bdr_u), acc = from_u32(acc_u);

        const float mcx = (insp.left + insp.right) * 0.5f;
        const float mcy = insp.top + 64.0f;
        draw_medallion_d2d(rt, mcx, mcy, 38.0f, static_cast<int>(e.tier), gl, gr, bdr, acc, e.unlocked, masked);

        draw_text(rt, masked ? "Secret Achievement" : e.title, fmt_h2_,
                  D2D1::RectF(insp.left + 16, mcy + 48, insp.right - 16, mcy + 76),
                  e.unlocked ? acc : pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);

        const std::string sub_meta = std::string(achievements::catalog_tier_name(e.tier)) + "  •  " +
                                     (e.unlocked ? ("Unlocked " + e.unlocked_at) : (e.missable && !masked ? "Locked (Missable)" : "Locked"));
        draw_text(rt, sub_meta, fmt_small_, D2D1::RectF(insp.left + 16, mcy + 78, insp.right - 16, mcy + 98),
                  e.unlocked ? pal.good : (e.missable && !masked ? pal.warn : pal.text_dim), DWRITE_TEXT_ALIGNMENT_CENTER);

        draw_text(rt, masked ? "Explore 1915 Europe & Japan or click Reveal Secrets to unveil its unlock condition." : e.description,
                  fmt_body_, D2D1::RectF(insp.left + 20, mcy + 108, insp.right - 20, mcy + 178), pal.text,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

        // Captured Screenshot OR Tier Mastery Breakdown
        const float bot_y = mcy + 190.0f;
        ID2D1Bitmap *shot = (e.unlocked && !e.screenshot_path.empty())
            ? load_bitmap_cached(rt, e.screenshot_path, 360, 220) : nullptr;
        if (shot) {
            draw_text(rt, "CAPTURED MOMENT", fmt_pill_, D2D1::RectF(insp.left + 20, bot_y, insp.right - 20, bot_y + 18), pal.accent);
            const D2D1_RECT_F fr{insp.left + 20, bot_y + 22, insp.right - 20, insp.bottom - 18};
            rt->DrawBitmap(shot, fr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (SUCCEEDED(rt->CreateSolidColorBrush(bdr, &sbr)) && sbr) {
                rt->DrawRectangle(fr, sbr, 1.4f);
                sbr->Release();
            }
        } else {
            draw_text(rt, "TIER MASTERY BREAKDOWN", fmt_pill_, D2D1::RectF(insp.left + 20, bot_y, insp.right - 20, bot_y + 18), pal.accent);
            static constexpr int kOrd[6] = {0, 1, 2, 3, 5, 6};
            static const char *const kNam[6] = {"Mark", "Sigil", "Crest", "Sovereign", "Follies", "Curiosities"};
            float my = bot_y + 26.0f;
            for (int k = 0; k < 6; ++k) {
                const int ti = kOrd[k];
                std::uint32_t mgl, mgr, mbdr, macc;
                achievements::catalog_theme_colors(static_cast<achievements::Tier>(ti), state_.settings.toast_theme, mgl, mgr, mbdr, macc);
                draw_text(rt, kNam[k], fmt_small_, D2D1::RectF(insp.left + 20, my, insp.left + 96, my + 18), from_u32(macc));
                const D2D1_RECT_F mb{insp.left + 98, my + 5, insp.right - 66, my + 13};
                if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(20, 22, 36), &sbr)) && sbr) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(mb, 4, 4), sbr);
                    sbr->Release();
                }
                const float rat = state_.tier_total[ti] > 0 ? static_cast<float>(state_.tier_unlocked[ti]) / static_cast<float>(state_.tier_total[ti]) : 0.0f;
                if (rat > 0.001f) {
                    const float fw = std::max(6.0f, (mb.right - mb.left) * rat);
                    fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(mb.left, mb.top, mb.left + fw, mb.bottom), 4, 4),
                                       from_u32(mbdr), from_u32(macc));
                }
                draw_text(rt, std::to_string(state_.tier_unlocked[ti]) + "/" + std::to_string(state_.tier_total[ti]),
                          fmt_small_, D2D1::RectF(insp.right - 62, my, insp.right - 20, my + 18), pal.text_dim, DWRITE_TEXT_ALIGNMENT_TRAILING);
                my += 26.0f;
            }
        }
    }
}

void LauncherApp::render_saves_media(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const float pad = 24.0f;
    const float x = area.left + pad;
    const float w = area.right - area.left - pad * 2.0f;
    const float y0 = area.top + 14.0f;

    draw_text(rt, "Saves & Media", fmt_h1_, D2D1::RectF(x, y0, x + 280, y0 + 30), pal.accent);

    // Sub-tabs: [Save States (0-9)]  [Screenshots Gallery]
    draw_button(rt, "sm_tab_0", D2D1::RectF(x + 210, y0, x + 382, y0 + 32),
                "Save States (" + std::to_string(state_.occupied_slots) + "/10)",
                saves_subtab_ == 0 ? with_a(pal.accent_crimson, 0.72f) : pal.card_bg,
                saves_subtab_ == 0 ? pal.border_hi : pal.border, pal.text, false, [this]() {
                    saves_subtab_ = 0; media_scroll_ = 0.0f; armed_delete_slot_ = -1;
                });
    draw_button(rt, "sm_tab_1", D2D1::RectF(x + 392, y0, x + 572, y0 + 32),
                "Screenshots (" + std::to_string(state_.screenshots.size()) + ")",
                saves_subtab_ == 1 ? with_a(pal.accent_crimson, 0.72f) : pal.card_bg,
                saves_subtab_ == 1 ? pal.border_hi : pal.border, pal.text, false, [this]() {
                    saves_subtab_ = 1; media_scroll_ = 0.0f; armed_delete_slot_ = -1;
                });
    draw_button(rt, "sm_export_bundle", D2D1::RectF(x + w - 316, y0, x + w - 158, y0 + 32),
                "Export .zip Bundle", with_a(pal.accent_crimson, 0.45f), pal.border_hi, pal.text, false, [this]() {
                    std::string msg;
                    export_save_bundle(state_.paths, nullptr, &msg);
                    show_toast(msg);
                });
    draw_button(rt, "sm_open", D2D1::RectF(x + w - 150, y0, x + w, y0 + 32),
                "Open Folder", pal.card_bg, pal.border, pal.text_dim, false, [this]() {
                    open_in_explorer(saves_subtab_ == 0 ? state_.paths.states_dir : state_.paths.screenshots_dir);
                });

    const D2D1_RECT_F view{x, y0 + 46.0f, x + w, area.bottom - 18.0f};
    const float view_h = view.bottom - view.top;
    rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    has_clip_rect_ = true;
    active_clip_rect_ = view;

    if (saves_subtab_ == 0) {
        const float gap = 12.0f;
        const float cw = (w - gap) * 0.5f;
        const float ch = 112.0f;
        const bool show_auto = state_.autosave_slot.exists;
        const float auto_h = show_auto ? 104.0f : 0.0f;
        const float auto_off = show_auto ? (auto_h + gap) : 0.0f;
        const float total_h = auto_off + 5.0f * (ch + gap);
        media_max_scroll_ = std::max(0.0f, total_h - view_h);
        media_scroll_ = std::clamp(media_scroll_, 0.0f, media_max_scroll_);

        if (show_auto) {
            const auto &as = state_.autosave_slot;
            const D2D1_RECT_F acr{view.left, view.top - media_scroll_, view.left + w, view.top - media_scroll_ + auto_h};
            const D2D1_ROUNDED_RECT arr = D2D1::RoundedRect(acr, 10, 10);
            fill_hgrad_rounded(rt, arr, with_a(pal.accent_crimson, 0.22f), pal.card_bg);
            ID2D1SolidColorBrush *ab = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(pal.border_hi, &ab)) && ab) {
                rt->DrawRoundedRectangle(arr, ab, 1.3f);
                ab->Release();
            }
            const D2D1_RECT_F atr{acr.left + 12, acr.top + 10, acr.left + 140, acr.bottom - 10};
            if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(10, 10, 18), &ab)) && ab) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(atr, 6, 6), ab);
                ab->Release();
            }
            if (ID2D1Bitmap *tb = load_slot_bitmap_cached(rt, as)) {
                rt->DrawBitmap(tb, atr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                draw_text(rt, "Autosave", fmt_small_, atr, pal.accent,
                          DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            const float atx = atr.right + 16.0f;
            draw_text(rt, "AREA AUTOSAVE" + (as.area.empty() ? std::string() : ("  —  " + as.area)),
                      fmt_card_title_, D2D1::RectF(atx, acr.top + 12, acr.right - 230, acr.top + 32), pal.accent);
            const std::string astats = "Disc " + std::to_string(as.disc) +
                                       (as.max_level > 0 ? ("  •  Lv " + std::to_string(as.max_level)) : "") +
                                       "  •  " + format_playtime(as.playtime_sec) + "  •  Rolling safety checkpoint";
            draw_text(rt, astats, fmt_body_, D2D1::RectF(atx, acr.top + 36, acr.right - 230, acr.top + 56), pal.text);
            draw_text(rt, as.timestamp + "  •  " + format_bytes(as.bytes), fmt_small_,
                      D2D1::RectF(atx, acr.top + 60, acr.right - 230, acr.top + 80), pal.text_dim);
            draw_button(rt, "play_slot_10", D2D1::RectF(acr.right - 216, acr.top + 32, acr.right - 122, acr.top + 68),
                        "Play Auto", state_.ready_to_play ? pal.accent_crimson : col_rgba(44, 40, 50),
                        pal.border_hi, col_rgba(255, 248, 232), false, [this]() {
                            if (state_.ready_to_play) launch_game(10);
                            else show_toast("Game executable missing — run Builder in Tools");
                        });
            const bool a_armed = (armed_delete_slot_ == 10);
            draw_button(rt, "del_slot_10", D2D1::RectF(acr.right - 114, acr.top + 32, acr.right - 12, acr.top + 68),
                        a_armed ? "Confirm Del" : "Delete",
                        a_armed ? col_rgba(168, 28, 46, 0.9f) : col_rgba(36, 24, 34, 0.85f),
                        a_armed ? pal.warn : pal.border, pal.text, false, [this]() {
                            if (armed_delete_slot_ != 10) {
                                armed_delete_slot_ = 10;
                                show_toast("Click 'Confirm Del' again to delete Area Autosave");
                                return;
                            }
                            armed_delete_slot_ = -1;
                            if (delete_save_slot(state_.paths, 10)) {
                                clear_bitmap_caches();
                                load_save_slots(state_);
                                show_toast("Deleted Area Autosave");
                            }
                        });
        }

        for (int s = 0; s < 10; ++s) {
            const auto &sl = state_.slots[static_cast<std::size_t>(s)];
            const int col = s % 2, row = s / 2;
            const float cx = view.left + static_cast<float>(col) * (cw + gap);
            const float cy = view.top + auto_off - media_scroll_ + static_cast<float>(row) * (ch + gap);
            const D2D1_RECT_F cr{cx, cy, cx + cw, cy + ch};
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(cr, 10, 10);

            ID2D1SolidColorBrush *b = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &b)) && b) {
                rt->FillRoundedRectangle(rr, b);
                b->Release();
            }
            if (SUCCEEDED(rt->CreateSolidColorBrush(sl.exists ? pal.border : with_a(pal.border, 0.35f), &b)) && b) {
                rt->DrawRoundedRectangle(rr, b, 1.0f);
                b->Release();
            }

            // Thumbnail box (128x90)
            const D2D1_RECT_F tr{cr.left + 12, cr.top + 11, cr.left + 140, cr.bottom - 11};
            if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(10, 10, 18), &b)) && b) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(tr, 6, 6), b);
                b->Release();
            }
            if (ID2D1Bitmap *tb = load_slot_bitmap_cached(rt, sl)) {
                rt->DrawBitmap(tb, tr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                draw_text(rt, sl.exists ? "No Preview" : "Empty", fmt_small_, tr, pal.text_dim,
                          DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            const float tx = tr.right + 14.0f;
            draw_text(rt, "Slot " + std::to_string(s) + (sl.exists && !sl.area.empty() ? ("  —  " + sl.area) : ""),
                      fmt_card_title_, D2D1::RectF(tx, cr.top + 12, cr.right - 14, cr.top + 32),
                      sl.exists ? pal.accent : pal.text_dim);
            if (sl.exists) {
                const std::string stats = "Disc " + std::to_string(sl.disc) +
                                          (sl.max_level > 0 ? ("  •  Lv " + std::to_string(sl.max_level)) : "") +
                                          "  •  " + format_playtime(sl.playtime_sec);
                draw_text(rt, stats, fmt_body_, D2D1::RectF(tx, cr.top + 36, cr.right - 14, cr.top + 56), pal.text);
                draw_text(rt, sl.timestamp + "  •  " + format_bytes(sl.bytes), fmt_small_,
                          D2D1::RectF(tx, cr.top + 58, cr.right - 14, cr.top + 76), pal.text_dim);

                draw_button(rt, "play_slot_" + std::to_string(s), D2D1::RectF(cr.right - 200, cr.bottom - 34, cr.right - 122, cr.bottom - 10),
                            "Play",
                            state_.ready_to_play ? with_a(pal.accent_crimson, 0.80f) : col_rgba(44, 40, 50),
                            pal.border_hi, col_rgba(255, 248, 232), false, [this, s]() {
                                if (state_.ready_to_play) launch_game(s);
                                else show_toast("Game executable missing — run Builder in Tools");
                            });

                const bool armed = (armed_delete_slot_ == s);
                draw_button(rt, "del_slot_" + std::to_string(s), D2D1::RectF(cr.right - 114, cr.bottom - 34, cr.right - 12, cr.bottom - 10),
                            armed ? "Confirm Del" : "Delete",
                            armed ? col_rgba(168, 28, 46, 0.9f) : col_rgba(36, 24, 34, 0.85f),
                            armed ? pal.warn : pal.border, pal.text, false, [this, s]() {
                                if (armed_delete_slot_ != s) {
                                    armed_delete_slot_ = s;
                                    show_toast("Click 'Confirm Del' again to delete Slot " + std::to_string(s));
                                    return;
                                }
                                armed_delete_slot_ = -1;
                                if (delete_save_slot(state_.paths, s)) {
                                    clear_bitmap_caches();
                                    load_save_slots(state_);
                                    show_toast("Deleted Save Slot " + std::to_string(s));
                                }
                            });
            } else {
                draw_text(rt, "Press F5 in-game to save into this slot.", fmt_small_,
                          D2D1::RectF(tx, cr.top + 42, cr.right - 14, cr.top + 66), pal.text_dim);
            }
        }
    } else {
        if (state_.screenshots.empty()) {
            draw_text(rt, "No screenshots captured yet. Press F12 in-game or unlock an achievement!",
                      fmt_h2_, D2D1::RectF(view.left + 12, view.top + 30, view.right - 12, view.top + 70), pal.text_dim);
            media_max_scroll_ = 0.0f;
        } else {
            const int cols = 3;
            const float gap = 14.0f;
            const float cw = (w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            const float ch = 196.0f;
            const int rows = (static_cast<int>(state_.screenshots.size()) + cols - 1) / cols;
            media_max_scroll_ = std::max(0.0f, static_cast<float>(rows) * (ch + gap) - view_h);
            media_scroll_ = std::clamp(media_scroll_, 0.0f, media_max_scroll_);

            for (std::size_t i = 0; i < state_.screenshots.size(); ++i) {
                const int r = static_cast<int>(i) / cols, c = static_cast<int>(i) % cols;
                const float cx = view.left + static_cast<float>(c) * (cw + gap);
                const float cy = view.top - media_scroll_ + static_cast<float>(r) * (ch + gap);
                if (cy + ch < view.top - 20.0f || cy > view.bottom + 20.0f) continue;

                const auto &sh = state_.screenshots[i];
                const D2D1_RECT_F cr{cx, cy, cx + cw, cy + ch};
                const std::string sid = "shot_" + std::to_string(i);
                const bool hov = (hovered_id_ == sid);
                ID2D1SolidColorBrush *b = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? pal.card_hover : pal.card_bg, &b)) && b) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(cr, 9, 9), b);
                    b->Release();
                }
                const D2D1_RECT_F ir{cr.left + 8, cr.top + 8, cr.right - 8, cr.bottom - 44};
                if (ID2D1Bitmap *bmp = load_bitmap_cached(rt, sh.path, 340, 200)) {
                    rt->DrawBitmap(bmp, ir, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                if (SUCCEEDED(rt->CreateSolidColorBrush(hov ? pal.border_hi : pal.border, &b)) && b) {
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(cr, 9, 9), b, hov ? 1.6f : 1.0f);
                    b->Release();
                }
                draw_text(rt, sh.display_name, fmt_small_, D2D1::RectF(cr.left + 10, cr.bottom - 38, cr.right - 10, cr.bottom - 20),
                          sh.is_achievement ? pal.accent : pal.text);
                draw_text(rt, sh.date_text, fmt_pill_, D2D1::RectF(cr.left + 10, cr.bottom - 20, cr.right - 10, cr.bottom - 4), pal.text_dim);
                push_hit({sid, cr, [this, sh]() {
                    lightbox_image_ = sh.path;
                    lightbox_title_ = sh.display_name + "  (" + sh.date_text + ")";
                }, nullptr, false});
            }
        }
    }

    has_clip_rect_ = false;
    rt->PopAxisAlignedClip();
}

void LauncherApp::start_update_check() {
    {
        std::lock_guard<std::mutex> lk(update_mu_);
        if (update_info_.checking) return;
        update_info_.checking = true;
        update_info_.channel = (global_cfg_.update_channel == "nightly") ? "nightly" : "main";
        update_info_.error.clear();
    }
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    const std::filesystem::path root_dir = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
    const std::filesystem::path script = state_.paths.update_script;
    const std::string channel = (global_cfg_.update_channel == "nightly") ? "nightly" : "main";
    HWND target_hwnd = hwnd_;

    update_thread_ = std::thread([this, root_dir, script, channel, target_hwnd]() {
        UpdateCheckResult res;
        res.checked = true;
        res.checking = false;
        res.channel = channel;

        std::error_code ec;
        if (script.empty() || !std::filesystem::is_regular_file(script, ec)) {
            res.ok = false;
            res.error = "Updater script (scripts/nocturne_update.py) not found in this folder.";
        } else {
            const std::filesystem::path portable_py = root_dir / "tools" / "portable" / "python" / "python.exe";
            const std::wstring py_exe = std::filesystem::is_regular_file(portable_py, ec) ? portable_py.wstring() : L"python.exe";
            const std::wstring wch = (channel == "nightly") ? L"nightly" : L"main";
            std::wstring cmd = L"\"" + py_exe + L"\" \"" + script.wstring() + L"\" --check --json --channel " + wch;

            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof sa;
            sa.bInheritHandle = TRUE;
            HANDLE h_read = nullptr, h_write = nullptr;
            if (CreatePipe(&h_read, &h_write, &sa, 0)) {
                SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW si{};
                si.cb = sizeof si;
                si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
                si.hStdOutput = h_write;
                si.hStdError = h_write;
                si.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION pi{};
                std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
                cmd_buf.push_back(L'\0');
                if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, root_dir.wstring().c_str(), &si, &pi)) {
                    CloseHandle(h_write);
                    h_write = nullptr;
                    std::string output;
                    char buf[1024];
                    DWORD got = 0;
                    while (ReadFile(h_read, buf, sizeof buf, &got, nullptr) && got > 0) {
                        output.append(buf, got);
                    }
                    WaitForSingleObject(pi.hProcess, 15000);
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    res = parse_update_check_json(output);
                } else {
                    res.ok = false;
                    res.error = "Could not launch Python to check GitHub updates.";
                }
                if (h_write) CloseHandle(h_write);
                CloseHandle(h_read);
            } else {
                res.ok = false;
                res.error = "Could not create pipe for update checker.";
            }
        }
        {
            std::lock_guard<std::mutex> lk(update_mu_);
            update_info_ = std::move(res);
        }
        if (target_hwnd && IsWindow(target_hwnd)) {
            PostMessageW(target_hwnd, WM_PAINT, 0, 0);
            InvalidateRect(target_hwnd, nullptr, FALSE);
        }
    });
}

void LauncherApp::render_tools(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const float pad = 26.0f;
    const float x = area.left + pad;
    const float w = area.right - area.left - pad * 2.0f;
    float y = area.top + 14.0f;

    draw_text(rt, "Tools, Health & Updates", fmt_h1_, D2D1::RectF(x, y, x + 420, y + 32), pal.accent);
    draw_text(rt, "Check GitHub updates (Stable or Nightly), manage HD packs, verify discs, or back up saves.",
              fmt_small_, D2D1::RectF(x, y + 30, x + 620, y + 48), pal.text_dim);

    // Subtab switcher (System Health | HD Packs & Mods | Updates) + Refresh button
    draw_button(rt, "tool_tab_health", D2D1::RectF(x + w - 462, y + 4, x + w - 344, y + 36),
                "System Health", tools_subtab_ == 0 ? with_a(pal.accent_crimson, 0.60f) : pal.card_bg,
                tools_subtab_ == 0 ? pal.border_hi : pal.border, pal.text, false, [this]() { tools_subtab_ = 0; });
    draw_button(rt, "tool_tab_mods", D2D1::RectF(x + w - 336, y + 4, x + w - 208, y + 36),
                "HD Packs & Mods", tools_subtab_ == 1 ? with_a(pal.accent_crimson, 0.60f) : pal.card_bg,
                tools_subtab_ == 1 ? pal.border_hi : pal.border, pal.text, false, [this]() { tools_subtab_ = 1; });
    draw_button(rt, "tool_tab_updates", D2D1::RectF(x + w - 200, y + 4, x + w - 108, y + 36),
                "Updates", tools_subtab_ == 2 ? with_a(pal.accent_crimson, 0.60f) : pal.card_bg,
                tools_subtab_ == 2 ? pal.border_hi : pal.border, pal.text, false, [this]() {
                    tools_subtab_ = 2;
                    bool need_check = false;
                    {
                        std::lock_guard<std::mutex> lk(update_mu_);
                        need_check = !update_info_.checked && !update_info_.checking;
                    }
                    if (need_check) start_update_check();
                });
    draw_button(rt, "tool_refresh", D2D1::RectF(x + w - 100, y + 4, x + w, y + 36),
                "Refresh", pal.card_bg, pal.border, pal.text, false, [this]() {
                    clear_bitmap_caches();
                    refresh_all(state_);
                    if (tools_subtab_ == 2) start_update_check();
                    show_toast("Status refreshed");
                });
    y += 62.0f;

    const float gap = 18.0f;
    const float left_w = (w - gap) * 0.56f;
    const float right_x = x + left_w + gap;
    const float right_w = w - left_w - gap;
    const float panel_h = area.bottom - y - 20.0f;

    if (tools_subtab_ == 2) {
        UpdateCheckResult uinfo;
        {
            std::lock_guard<std::mutex> lk(update_mu_);
            uinfo = update_info_;
        }
        const bool is_nightly = (global_cfg_.update_channel == "nightly");

        // Left: Release Channel & Build Status Card
        const D2D1_RECT_F uc{x, y, x + left_w, y + panel_h};
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(uc, 11, 11), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(uc, 11, 11), b, 1.0f);
            b->Release();
        }
        draw_text(rt, "RELEASE CHANNEL & GITHUB BUILD STATUS", fmt_pill_,
                  D2D1::RectF(uc.left + 16, uc.top + 14, uc.right - 16, uc.top + 30), pal.accent);

        float uy = uc.top + 42.0f;
        draw_segmented_row(rt, "upd_chan", uc.left + 12, uy, left_w - 24, "Update Channel",
                           {"Stable (main)", "Nightly (Opt-In)"}, is_nightly ? 1 : 0, pal, [this](int idx) {
                               const std::string next = (idx == 1) ? "nightly" : "main";
                               if (global_cfg_.update_channel != next) {
                                   global_cfg_.update_channel = next;
                                   save_global_config(state_.paths.root_dir, global_cfg_);
                                   // Mirror choice into install_config.json via nocturne_update.py --set-channel
                                   std::error_code ec;
                                   const std::filesystem::path sr = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
                                   if (std::filesystem::is_regular_file(state_.paths.update_script, ec)) {
                                       const std::filesystem::path py = sr / "tools" / "portable" / "python" / "python.exe";
                                       const std::wstring py_exe = std::filesystem::is_regular_file(py, ec) ? py.wstring() : L"python.exe";
                                       const std::wstring params = L"\"" + state_.paths.update_script.wstring() + L"\" --set-channel " +
                                                                   (next == "nightly" ? L"nightly" : L"main");
                                       ShellExecuteW(nullptr, L"open", py_exe.c_str(), params.c_str(),
                                                     sr.c_str(), SW_HIDE);
                                   }
                                   show_toast(next == "nightly"
                                                  ? "Opted into Nightly builds — checking GitHub..."
                                                  : "Switched to Stable (main) — checking GitHub...");
                                   start_update_check();
                               }
                           });
        uy += 62.0f;
        draw_text(rt, is_nightly
                          ? "Nightly receives the latest Koudelka, SH1/SH3, and Launcher updates as soon as they pass tests."
                          : "Stable tracks verified main-branch releases. Opt into Nightly anytime for early fixes.",
                  fmt_small_, D2D1::RectF(uc.left + 18, uy, uc.right - 18, uy + 24), pal.text_dim);
        uy += 36.0f;

        // Status readout box
        const D2D1_RECT_F sbox{uc.left + 16, uy, uc.right - 16, uy + 152.0f};
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.card_hover, 0.72f), &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(sbox, 9, 9), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(uinfo.update_available ? pal.border_hi : pal.border, 0.7f), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(sbox, 9, 9), b, 1.0f);
            b->Release();
        }

        std::string status_text = "Click 'Check for Updates' to compare against GitHub.";
        D2D1_COLOR_F status_col = pal.text_dim;
        if (uinfo.checking) {
            status_text = "Checking GitHub (" + uinfo.channel + ")...";
            status_col = pal.accent;
        } else if (uinfo.checked && !uinfo.ok) {
            status_text = uinfo.error.empty() ? "Could not check GitHub updates." : uinfo.error;
            status_col = pal.warn;
        } else if (uinfo.checked && uinfo.ok && uinfo.update_available) {
            status_text = "UPDATE AVAILABLE on " + uinfo.channel + " (" + uinfo.remote_short + ")";
            status_col = pal.accent;
        } else if (uinfo.checked && uinfo.ok) {
            status_text = "UP TO DATE on " + uinfo.channel + " (" + uinfo.local_short + ")";
            status_col = pal.good;
        }
        draw_text(rt, status_text, fmt_card_title_,
                  D2D1::RectF(sbox.left + 14, sbox.top + 12, sbox.right - 14, sbox.top + 36), status_col);

        const std::string loc_str = uinfo.local_short.empty()
                                        ? "Installed Build: (click Check for Updates)"
                                        : ("Installed Build:  " + uinfo.local_short +
                                           (uinfo.local_date.empty() ? "" : ("  (" + uinfo.local_date + ")")));
        const std::string rem_str = uinfo.remote_short.empty()
                                        ? "Latest on GitHub:  —"
                                        : ("Latest on GitHub:  " + uinfo.remote_short +
                                           (uinfo.remote_date.empty() ? "" : ("  (" + uinfo.remote_date + ")")));
        draw_text(rt, loc_str, fmt_body_, D2D1::RectF(sbox.left + 14, sbox.top + 48, sbox.right - 14, sbox.top + 70), pal.text);
        draw_text(rt, rem_str, fmt_body_, D2D1::RectF(sbox.left + 14, sbox.top + 74, sbox.right - 14, sbox.top + 96), pal.text);
        if (!uinfo.remote_subject.empty()) {
            draw_text(rt, "Latest change: " + uinfo.remote_subject, fmt_small_,
                      D2D1::RectF(sbox.left + 14, sbox.top + 106, sbox.right - 14, sbox.top + 136), pal.text_dim);
        }
        if (!state_.paths.scripts_root.empty() && state_.paths.scripts_root != state_.paths.root_dir) {
            draw_text(rt, "Standalone Install: " + state_.paths.root_dir.string(), fmt_small_,
                      D2D1::RectF(uc.left + 18, sbox.bottom + 14, uc.right - 18, sbox.bottom + 34), pal.text_dim);
            draw_text(rt, "Source / Tools Repo: " + state_.paths.scripts_root.string(), fmt_small_,
                      D2D1::RectF(uc.left + 18, sbox.bottom + 34, uc.right - 18, sbox.bottom + 54), pal.text_dim);
        }

        // Right: Update & Setup Actions Card
        const D2D1_RECT_F ac{right_x, y, right_x + right_w, y + panel_h};
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(ac, 11, 11), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(ac, 11, 11), b, 1.0f);
            b->Release();
        }
        draw_text(rt, "UPDATE & INSTALLER ACTIONS", fmt_pill_,
                  D2D1::RectF(ac.left + 16, ac.top + 14, ac.right - 16, ac.top + 30), pal.accent);

        float ay = ac.top + 42.0f;
        const float bw = right_w - 32.0f;
        draw_button(rt, "upd_check_now", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 42),
                    uinfo.checking ? "Checking GitHub..." : "Check for Updates Now",
                    pal.card_hover, pal.border_hi, pal.text, false, [this]() {
                        start_update_check();
                        show_toast("Checking GitHub (" + global_cfg_.update_channel + ")...");
                    });
        ay += 54.0f;

        const bool hi_upd = uinfo.checked && uinfo.ok && uinfo.update_available;
        const std::string upd_lbl = hi_upd
            ? ("Download & Install Update (" + uinfo.remote_short + ") — Closes Launcher")
            : ("Update Installed Games (" + global_cfg_.update_channel + ") — Closes Launcher");
        draw_button(rt, "upd_install_now", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 44),
                    upd_lbl,
                    hi_upd ? with_a(pal.accent_crimson, 0.80f) : with_a(pal.accent_crimson, 0.40f),
                    hi_upd ? pal.accent : pal.border_hi, pal.text, hi_upd, [this]() {
                        std::error_code ec;
                        if (state_.paths.build_cmd.empty() || !std::filesystem::is_regular_file(state_.paths.build_cmd, ec)) {
                            show_toast("Shadow Hearts Nocturne Setup.cmd not found in this folder.");
                            return;
                        }
                        const std::wstring wch = (global_cfg_.update_channel == "nightly") ? L"nightly" : L"main";
                        const std::filesystem::path sr = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
                        const std::wstring params = L"/c \"timeout /t 1 /nobreak >nul & call \"" +
                                                    state_.paths.build_cmd.wstring() + L"\" update --channel " + wch + L"\"";
                        ShellExecuteW(nullptr, L"open", L"cmd.exe", params.c_str(), sr.c_str(), SW_SHOWNORMAL);
                        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    });
        ay += 58.0f;

        draw_button(rt, "upd_open_wizard", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 40),
                    "Run 6-Step Setup Walkthrough (Closes Launcher)",
                    pal.card_hover, pal.border, pal.text, false, [this]() {
                        std::error_code ec;
                        if (state_.paths.build_cmd.empty() || !std::filesystem::is_regular_file(state_.paths.build_cmd, ec)) {
                            show_toast("Shadow Hearts Nocturne Setup.cmd not found in this folder.");
                            return;
                        }
                        const std::filesystem::path sr = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
                        const std::wstring params = L"/c \"timeout /t 1 /nobreak >nul & call \"" +
                                                    state_.paths.build_cmd.wstring() + L"\" wizard\"";
                        ShellExecuteW(nullptr, L"open", L"cmd.exe", params.c_str(), sr.c_str(), SW_SHOWNORMAL);
                        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    });
        ay += 54.0f;
        draw_text(rt, "Updates only rebuild the games you already installed. Memory cards, save states, keys.ini, mods, and unpacked discs are never deleted.",
                  fmt_small_, D2D1::RectF(ac.left + 16, ay, ac.right - 16, ay + 48), pal.text_dim);
        return;
    }

    if (tools_subtab_ == 1) {
        // Left: 3-Mode Content Manager: Texture Packs (texture_packs/) | Video Packs (video_packs/) | Gameplay Mods (mods/)
        const D2D1_RECT_F pc{x, y, x + left_w, y + panel_h};
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(pc, 11, 11), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pc, 11, 11), b, 1.0f);
            b->Release();
        }

        // Top 3-way subtab selector inside left card
        const float ptw = (left_w - 36.0f) / 3.0f;
        const std::string t0_lbl = "Texture Packs (" + std::to_string(state_.texture_packs.size()) + ")";
        const std::string t1_lbl = "Video Packs (" + std::to_string(state_.video_packs.size()) + ")";
        const std::string t2_lbl = "Gameplay Mods (" + std::to_string(state_.mods.size()) + ")";
        draw_button(rt, "pkm_tab_0", D2D1::RectF(pc.left + 12, pc.top + 10, pc.left + 12 + ptw, pc.top + 38),
                    t0_lbl, packs_view_mode_ == 0 ? with_a(pal.accent_crimson, 0.70f) : pal.card_hover,
                    packs_view_mode_ == 0 ? pal.border_hi : pal.border, pal.text, false,
                    [this]() { packs_view_mode_ = 0; tools_mods_scroll_ = 0.0f; });
        draw_button(rt, "pkm_tab_1", D2D1::RectF(pc.left + 18 + ptw, pc.top + 10, pc.left + 18 + ptw * 2.0f, pc.top + 38),
                    t1_lbl, packs_view_mode_ == 1 ? with_a(pal.accent_crimson, 0.70f) : pal.card_hover,
                    packs_view_mode_ == 1 ? pal.border_hi : pal.border, pal.text, false,
                    [this]() { packs_view_mode_ = 1; tools_mods_scroll_ = 0.0f; });
        draw_button(rt, "pkm_tab_2", D2D1::RectF(pc.left + 24 + ptw * 2.0f, pc.top + 10, pc.right - 12, pc.top + 38),
                    t2_lbl, packs_view_mode_ == 2 ? with_a(pal.accent_crimson, 0.70f) : pal.card_hover,
                    packs_view_mode_ == 2 ? pal.border_hi : pal.border, pal.text, false,
                    [this]() { packs_view_mode_ = 2; tools_mods_scroll_ = 0.0f; });

        const std::size_t active_list_count = (packs_view_mode_ == 0) ? state_.texture_packs.size()
                                            : (packs_view_mode_ == 1) ? state_.video_packs.size()
                                            : state_.mods.size();

        if (active_list_count == 0) {
            if (packs_view_mode_ == 0) {
                draw_text(rt, "No HD texture packs found in texture_packs/ yet.", fmt_body_,
                          D2D1::RectF(pc.left + 18, pc.top + 60, pc.right - 18, pc.top + 84), pal.text);
                draw_text(rt, "Drop pack folders into games/" + std::string(game_profile(active_game_).slug) +
                              "/texture_packs/<PackName>/, or click '1-Click: Dump All Disc Textures & AI Upscale (4x)' on the right.",
                          fmt_small_, D2D1::RectF(pc.left + 18, pc.top + 88, pc.right - 18, pc.top + 156), pal.text_dim,
                          DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            } else if (packs_view_mode_ == 1) {
                draw_text(rt, "No HD video packs found in video_packs/ yet.", fmt_body_,
                          D2D1::RectF(pc.left + 18, pc.top + 60, pc.right - 18, pc.top + 84), pal.text);
                draw_text(rt, "Drop video pack folders or <hash>.mp4 cutscenes into games/" + std::string(game_profile(active_game_).slug) +
                              "/video_packs/, or click '1-Click: Dump All Disc FMVs & AI Upscale' on the right.",
                          fmt_small_, D2D1::RectF(pc.left + 18, pc.top + 88, pc.right - 18, pc.top + 156), pal.text_dim,
                          DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            } else {
                draw_text(rt, "No gameplay mods installed in mods/ yet.", fmt_body_,
                          D2D1::RectF(pc.left + 18, pc.top + 60, pc.right - 18, pc.top + 84), pal.text);
                draw_text(rt, "Drop gameplay mods into games/" + std::string(game_profile(active_game_).slug) +
                              "/mods/<ModName>/ with mod.json, files/ (Layer 1 disc overrides), patches/*.pnach (Layer 2 memory patches), or scripts/*.lua (Layer 3 event hooks).",
                          fmt_small_, D2D1::RectF(pc.left + 18, pc.top + 88, pc.right - 18, pc.top + 166), pal.text_dim,
                          DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            }
            tools_mods_max_scroll_ = 0.0f;
        } else {
            const D2D1_RECT_F list_view{pc.left + 8.0f, pc.top + 46.0f, pc.right - 16.0f, pc.bottom - 10.0f};
            const float list_h = std::max(1.0f, list_view.bottom - list_view.top);
            const float row_pitch = (packs_view_mode_ == 2) ? 74.0f : 60.0f;
            const float total_h = static_cast<float>(active_list_count) * row_pitch;
            tools_mods_max_scroll_ = std::max(0.0f, total_h - list_h);
            tools_mods_scroll_ = std::clamp(tools_mods_scroll_, 0.0f, tools_mods_max_scroll_);

            rt->PushAxisAlignedClip(list_view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            has_clip_rect_ = true;
            active_clip_rect_ = list_view;

            float py = list_view.top + 4.0f - tools_mods_scroll_;
            for (std::size_t i = 0; i < active_list_count; ++i) {
                const bool is_mod = (packs_view_mode_ == 2);
                const bool en = (packs_view_mode_ == 0) ? state_.texture_packs[i].enabled
                              : (packs_view_mode_ == 1) ? state_.video_packs[i].enabled
                              : state_.mods[i].enabled;
                const bool has_conflict = is_mod && !state_.mods[i].conflicts_with.empty();
                const float rh = is_mod ? 66.0f : 52.0f;
                const D2D1_RECT_F row{pc.left + 12.0f, py, list_view.right - 4.0f, py + rh};
                if (row.bottom >= list_view.top - 4.0f && row.top <= list_view.bottom + 4.0f) {
                    if (SUCCEEDED(rt->CreateSolidColorBrush(en ? with_a(pal.card_hover, 0.9f) : with_a(pal.card_bg, 0.5f), &b)) && b) {
                        rt->FillRoundedRectangle(D2D1::RoundedRect(row, 8, 8), b);
                        b->Release();
                    }
                    const D2D1_COLOR_F bdr_c = has_conflict ? pal.warn : (en ? with_a(pal.border_hi, 0.5f) : with_a(pal.border, 0.35f));
                    if (SUCCEEDED(rt->CreateSolidColorBrush(bdr_c, &b)) && b) {
                        rt->DrawRoundedRectangle(D2D1::RoundedRect(row, 8, 8), b, has_conflict ? 1.5f : 1.0f);
                        b->Release();
                    }
                    draw_button(rt, "pk_tog_" + std::to_string(i), D2D1::RectF(row.left + 8, row.top + 11, row.left + 58, row.top + 41),
                                en ? "ON" : "OFF", en ? with_a(pal.good, 0.30f) : pal.card_bg,
                                en ? pal.good : pal.border, en ? pal.text : pal.text_dim, false, [this, i]() {
                                    if (packs_view_mode_ == 0 && toggle_texture_pack(state_, i)) persist_settings("Updated HD texture pack state");
                                    else if (packs_view_mode_ == 1 && toggle_video_pack(state_, i)) persist_settings("Updated HD video pack state");
                                    else if (packs_view_mode_ == 2 && toggle_mod(state_, i)) persist_settings("Updated gameplay mod state");
                                });

                    if (packs_view_mode_ == 0) {
                        const auto &pk = state_.texture_packs[i];
                        draw_text(rt, pk.name, fmt_body_, D2D1::RectF(row.left + 68, row.top + 7, row.right - 92, row.top + 27),
                                  en ? pal.text : pal.text_dim);
                        const std::string meta = std::to_string(pk.png_count) + " textures  |  " + format_bytes(pk.bytes);
                        draw_text(rt, meta, fmt_small_, D2D1::RectF(row.left + 68, row.top + 27, row.right - 92, row.top + 45), pal.text_dim);
                    } else if (packs_view_mode_ == 1) {
                        const auto &vp = state_.video_packs[i];
                        draw_text(rt, vp.name, fmt_body_, D2D1::RectF(row.left + 68, row.top + 7, row.right - 92, row.top + 27),
                                  en ? pal.text : pal.text_dim);
                        const std::string meta = std::to_string(vp.mp4_count) + " cutscenes (.mp4)  |  " + format_bytes(vp.bytes);
                        draw_text(rt, meta, fmt_small_, D2D1::RectF(row.left + 68, row.top + 27, row.right - 92, row.top + 45), pal.text_dim);
                    } else {
                        const auto &m = state_.mods[i];
                        const std::string title = m.name + "  v" + m.version + "  by " + m.author;
                        draw_text(rt, title, fmt_body_, D2D1::RectF(row.left + 68, row.top + 6, row.right - 92, row.top + 25),
                                  en ? pal.text : pal.text_dim);
                        draw_text(rt, m.description, fmt_small_, D2D1::RectF(row.left + 68, row.top + 25, row.right - 92, row.top + 43), pal.text_dim);
                        std::string layer_line;
                        if (has_conflict) {
                            layer_line = "CONFLICT with: " + m.conflicts_with.front() + " (top priority wins)";
                        } else {
                            layer_line = "Files: " + std::to_string(m.file_override_count) +
                                         "  •  Patches: " + std::to_string(m.pnach_patch_count) +
                                         "  •  Lua Hooks: " + std::to_string(m.lua_hook_count);
                        }
                        draw_text(rt, layer_line, fmt_pill_, D2D1::RectF(row.left + 68, row.top + 44, row.right - 92, row.top + 62),
                                  has_conflict ? pal.warn : pal.accent);
                    }

                    draw_button(rt, "pk_up_" + std::to_string(i), D2D1::RectF(row.right - 82, row.top + 11, row.right - 46, row.top + 41),
                                "Up", pal.card_bg, pal.border, i > 0 ? pal.text : pal.text_dim, false, [this, i]() {
                                    if (packs_view_mode_ == 0 && move_texture_pack(state_, i, -1)) persist_settings("Moved texture pack priority up");
                                    else if (packs_view_mode_ == 1 && move_video_pack(state_, i, -1)) persist_settings("Moved video pack priority up");
                                    else if (packs_view_mode_ == 2 && move_mod(state_, i, -1)) persist_settings("Moved mod priority up");
                                });
                    draw_button(rt, "pk_dn_" + std::to_string(i), D2D1::RectF(row.right - 42, row.top + 11, row.right - 6, row.top + 41),
                                "Dn", pal.card_bg, pal.border, (i + 1 < active_list_count) ? pal.text : pal.text_dim, false, [this, i]() {
                                    if (packs_view_mode_ == 0 && move_texture_pack(state_, i, +1)) persist_settings("Moved texture pack priority down");
                                    else if (packs_view_mode_ == 1 && move_video_pack(state_, i, +1)) persist_settings("Moved video pack priority down");
                                    else if (packs_view_mode_ == 2 && move_mod(state_, i, +1)) persist_settings("Moved mod priority down");
                                });
                }
                py += row_pitch;
            }
            has_clip_rect_ = false;
            rt->PopAxisAlignedClip();

            if (tools_mods_max_scroll_ > 1.0f) {
                const D2D1_RECT_F sb{pc.right - 12.0f, list_view.top, pc.right - 6.0f, list_view.bottom};
                if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(22, 24, 38, 0.85f), &b)) && b) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(sb, 3, 3), b);
                    b->Release();
                }
                const float th = std::max(30.0f, list_h * (list_h / std::max(1.0f, total_h)));
                const float ty = list_view.top + (list_h - th) * (tools_mods_scroll_ / tools_mods_max_scroll_);
                fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(sb.left + 1, ty, sb.right - 1, ty + th), 2, 2),
                                   pal.accent_crimson, pal.accent);
            }
        }

        // Right: HD Toggles, FMV Summary & Batch Upscaler Actions
        const D2D1_RECT_F mc{right_x, y, right_x + right_w, y + panel_h};
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &b)) && b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(mc, 11, 11), b);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(mc, 11, 11), b, 1.0f);
            b->Release();
        }
        draw_text(rt, "CONTENT PACKS, MOD ENGINE & AI UPSCALERS", fmt_pill_, D2D1::RectF(mc.left + 16, mc.top + 12, mc.right - 16, mc.top + 28), pal.accent);

        float my = mc.top + 34.0f;
        const float mw = right_w - 32.0f;
        draw_toggle_row(rt, "mod_hd_tex", mc.left + 16, my, mw, "HD Texture Packs (texture_packs/)",
                        "Async zero-hitch 2x/4x/8x texture loading", state_.settings.hd_textures, pal, [this](bool v) {
                            state_.settings.hd_textures = v;
                            persist_settings(v ? "HD Texture Packs enabled" : "HD Texture Packs disabled");
                        });
        my += 46.0f;
        const std::string fmv_sub = state_.fmv_mods.mp4_count > 0
            ? (std::to_string(state_.fmv_mods.mp4_count) + " .mp4 cutscene(s) in video_packs/ (" + format_bytes(state_.fmv_mods.bytes) + ")")
            : "Drop <hash>.mp4 cutscenes in video_packs/ (WMF HW decode)";
        draw_toggle_row(rt, "mod_hd_fmv", mc.left + 16, my, mw, "HD Video Packs (video_packs/)",
                        fmv_sub, state_.settings.hd_fmv, pal, [this](bool v) {
                            state_.settings.hd_fmv = v;
                            persist_settings(v ? "HD Video Packs enabled" : "HD Video Packs disabled");
                        });
        my += 46.0f;
        draw_toggle_row(rt, "mod_tex_dump", mc.left + 16, my, mw, "Runtime GPU Texture Dumping",
                        "Saves static textures to dumps/textures/ (" + std::to_string(state_.dumped_tex_count) + " PNGs)",
                        state_.settings.tex_dump, pal, [this](bool v) {
                            state_.settings.tex_dump = v;
                            persist_settings(v ? "Texture dumping ON (skips render targets & FMVs)" : "Texture dumping OFF");
                        });
        my += 46.0f;
        draw_toggle_row(rt, "mod_fmv_dump", mc.left + 16, my, mw, "Log & Dump FMV Streams",
                        "Saves cutscene dumps in dumps/fmv/ (" + std::to_string(state_.dumped_fmv_count) + " saved)",
                        state_.settings.fmv_dump, pal, [this](bool v) {
                            state_.settings.fmv_dump = v;
                            persist_settings(v ? "FMV stream dumping ON" : "FMV stream dumping OFF");
                        });
        my += 52.0f;

        auto resolve_disc1_and_py = [this](std::filesystem::path &disc_layer0, std::wstring &py_exe) -> bool {
            std::error_code ec;
            if (state_.paths.extract_pkb_script.empty() || !std::filesystem::is_regular_file(state_.paths.extract_pkb_script, ec)) {
                show_toast(game_profile(active_game_).coming_soon_msg);
                return false;
            }
            disc_layer0 = state_.paths.game_dir / "disc1_files" / "layer0";
            if (!std::filesystem::is_directory(disc_layer0, ec)) {
                disc_layer0 = state_.paths.disc1_path / "layer0";
            }
            if (!std::filesystem::is_directory(disc_layer0, ec)) {
                show_toast("Unpacked disc1_files/layer0 not found — unpack Disc 1 first");
                return false;
            }
            const std::filesystem::path sr = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
            const std::filesystem::path portable_py = sr / "tools" / "portable" / "python" / "python.exe";
            py_exe = std::filesystem::is_regular_file(portable_py, ec)
                ? (L"\"" + portable_py.wstring() + L"\"")
                : std::wstring(L"python");
            return true;
        };

        const std::string hd_pack_name = (active_game_ == GameId::ShadowHearts1) ? "SH1_HD_Pack"
                                       : (active_game_ == GameId::FromTheNewWorld) ? "SH3_HD_Pack"
                                       : (active_game_ == GameId::Koudelka) ? "Koudelka_HD_Pack"
                                       : "SH2_HD_Pack";
        const std::wstring hd_pack_wname(hd_pack_name.begin(), hd_pack_name.end());

        auto launch_disc_extract = [this, resolve_disc1_and_py, hd_pack_name, hd_pack_wname](bool also_upscale) {
            std::error_code ec;
            std::filesystem::create_directories(state_.paths.texture_packs_dir, ec);
            std::filesystem::create_directories(state_.paths.dumps_textures_dir, ec);
            std::filesystem::path disc_layer0;
            std::wstring py_exe;
            if (!resolve_disc1_and_py(disc_layer0, py_exe)) return;
            std::wstring cmd = py_exe + L" \"" + state_.paths.extract_pkb_script.wstring() +
                               L"\" --disc \"" + disc_layer0.wstring() +
                               L"\" --extract-tex \"" + state_.paths.dumps_textures_dir.wstring() +
                               L"\" --clean";
            if (also_upscale) {
                cmd += L" --upscale --output \"" + (state_.paths.texture_packs_dir / hd_pack_wname).wstring() +
                       L"\" --scale 4";
                if (!state_.settings.hd_textures) {
                    state_.settings.hd_textures = true;
                    persist_settings("HD Texture Packs enabled for upcoming " + hd_pack_name);
                }
            }
            const std::wstring params = L"/k \"" + cmd + L"\"";
            ShellExecuteW(hwnd_, L"open", L"cmd.exe", params.c_str(), state_.paths.root_dir.c_str(), SW_SHOWNORMAL);
            show_toast(also_upscale ? "Started 1-Click Full-Disc Texture Dump & 4x AI Upscale -> texture_packs/"
                                    : "Started Full-Disc Texture Dump (~20s)");
        };

        auto launch_fmv_extract = [this, resolve_disc1_and_py](bool also_upscale) {
            std::error_code ec;
            std::filesystem::create_directories(state_.paths.video_packs_dir, ec);
            std::filesystem::create_directories(state_.paths.dumps_fmv_dir, ec);
            std::filesystem::path disc_layer0;
            std::wstring py_exe;
            if (!resolve_disc1_and_py(disc_layer0, py_exe)) return;
            const int sc = (fmv_upscale_scale_ == 3 || fmv_upscale_scale_ == 4) ? fmv_upscale_scale_ : 2;
            std::wstring cmd = py_exe + L" \"" + state_.paths.extract_pkb_script.wstring() +
                               L"\" --disc \"" + disc_layer0.wstring() +
                               L"\" --extract-fmv \"" + state_.paths.dumps_fmv_dir.wstring() +
                               L"\" --clean";
            if (also_upscale) {
                cmd += L" --upscale-fmv --fmv-output \"" + state_.paths.video_packs_dir.wstring() +
                       L"\" --fmv-scale " + std::to_wstring(sc);
                if (!state_.settings.hd_fmv) {
                    state_.settings.hd_fmv = true;
                    persist_settings("HD Video Packs enabled for upcoming AI upscale");
                }
            }
            const std::wstring params = L"/k \"" + cmd + L"\"";
            ShellExecuteW(hwnd_, L"open", L"cmd.exe", params.c_str(), state_.paths.root_dir.c_str(), SW_SHOWNORMAL);
            show_toast(also_upscale
                ? ("Started 1-Click Full-Disc FMV Dump & " + std::to_string(sc) + "x AI Upscale -> video_packs/")
                : "Started Full-Disc FMV Dump (Playable MP4 + 48kHz Audio)");
        };

        // --- Textures Section ---
        const char *tex_hdr = (game_profile(active_game_).disc_count >= 2) ? "TEXTURES (DISC 1 & 2)" : "TEXTURES (DISC 1)";
        draw_text(rt, tex_hdr, fmt_pill_, D2D1::RectF(mc.left + 16, my, mc.right - 16, my + 16), pal.text_dim);
        my += 18.0f;

        draw_button(rt, "mod_oneclick_all", D2D1::RectF(mc.left + 16, my, mc.left + 16 + mw, my + 36),
                    "1-Click: Dump All Disc Textures & AI Upscale (4x)", with_a(pal.accent_crimson, 0.65f), pal.border_hi, pal.text, false,
                    [launch_disc_extract]() { launch_disc_extract(true); });
        my += 42.0f;

        const float hw = (mw - 10.0f) * 0.5f;
        draw_button(rt, "mod_dump_all_disc", D2D1::RectF(mc.left + 16, my, mc.left + 16 + hw, my + 32),
                    "Dump All Disc Textures Only", pal.card_hover, pal.border_hi, pal.text, false,
                    [launch_disc_extract]() { launch_disc_extract(false); });
        draw_button(rt, "mod_run_upscaler", D2D1::RectF(mc.left + 26 + hw, my, mc.left + 16 + mw, my + 32),
                    "AI Upscale Texture Dumps", pal.card_hover, pal.border_hi, pal.text, false, [this, hd_pack_wname]() {
                        std::error_code ec;
                        std::filesystem::create_directories(state_.paths.texture_packs_dir, ec);
                        std::filesystem::create_directories(state_.paths.dumps_textures_dir, ec);
                        const std::filesystem::path sr = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
                        const std::filesystem::path portable_py = sr / "tools" / "portable" / "python" / "python.exe";
                        const std::wstring py_exe = std::filesystem::is_regular_file(portable_py, ec)
                            ? (L"\"" + portable_py.wstring() + L"\"")
                            : std::wstring(L"python");
                        const std::wstring params = L"/k \"" + py_exe + L" \"" + state_.paths.upscale_script.wstring() + L"\" --input \"" +
                                                    state_.paths.dumps_textures_dir.wstring() + L"\" --output \"" +
                                                    (state_.paths.texture_packs_dir / hd_pack_wname).wstring() + L"\" --scale 4 --auto-setup\"";
                        ShellExecuteW(hwnd_, L"open", L"cmd.exe", params.c_str(), sr.c_str(), SW_SHOWNORMAL);
                        show_toast("Launched AI Texture Upscaler console");
                    });
        my += 42.0f;

        // --- FMV Cutscenes Section ---
        const char *fmv_hdr = (game_profile(active_game_).disc_count >= 2) ? "FMV CUTSCENES (DISC 1 & 2)" : "FMV CUTSCENES (DISC 1)";
        draw_text(rt, fmv_hdr, fmt_pill_, D2D1::RectF(mc.left + 16, my + 4, mc.left + 16 + mw - 180, my + 22), pal.text_dim);
        draw_text(rt, "AI Scale:", fmt_small_, D2D1::RectF(mc.right - 196, my + 3, mc.right - 134, my + 22), pal.text_dim,
                  DWRITE_TEXT_ALIGNMENT_TRAILING);
        const int fmv_scales[3] = {2, 3, 4};
        for (int si = 0; si < 3; ++si) {
            const int sc = fmv_scales[si];
            const float px = mc.right - 130.0f + si * 38.0f;
            const bool active = (fmv_upscale_scale_ == sc);
            draw_button(rt, "mod_fmv_scale_" + std::to_string(sc), D2D1::RectF(px, my, px + 34.0f, my + 24.0f),
                        std::to_string(sc) + "x",
                        active ? with_a(pal.accent_crimson, 0.70f) : pal.card_hover,
                        active ? pal.border_hi : pal.border,
                        active ? pal.text : pal.text_dim, false, [this, sc]() {
                            fmv_upscale_scale_ = sc;
                            show_toast("FMV AI upscale scale set to " + std::to_string(sc) + "x");
                        });
        }
        my += 30.0f;

        draw_button(rt, "mod_fmv_oneclick", D2D1::RectF(mc.left + 16, my, mc.left + 16 + mw, my + 36),
                    "1-Click: Dump All Disc FMVs & AI Upscale (" + std::to_string(fmv_upscale_scale_) + "x)",
                    with_a(pal.accent_crimson, 0.65f), pal.border_hi, pal.text, false,
                    [launch_fmv_extract]() { launch_fmv_extract(true); });
        my += 42.0f;

        draw_button(rt, "mod_fmv_dump_all", D2D1::RectF(mc.left + 16, my, mc.left + 16 + hw, my + 32),
                    "Dump All Disc FMVs (MP4)", pal.card_hover, pal.border_hi, pal.text, false,
                    [launch_fmv_extract]() { launch_fmv_extract(false); });
        draw_button(rt, "mod_fmv_upscale", D2D1::RectF(mc.left + 26 + hw, my, mc.left + 16 + mw, my + 32),
                    "AI Upscale Dumped FMVs (" + std::to_string(fmv_upscale_scale_) + "x)", pal.card_hover, pal.border_hi, pal.text, false, [this]() {
                        std::error_code ec;
                        std::filesystem::create_directories(state_.paths.video_packs_dir, ec);
                        std::filesystem::create_directories(state_.paths.dumps_fmv_dir, ec);
                        const std::filesystem::path sr = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
                        const std::filesystem::path portable_py = sr / "tools" / "portable" / "python" / "python.exe";
                        const std::wstring py_exe = std::filesystem::is_regular_file(portable_py, ec)
                            ? (L"\"" + portable_py.wstring() + L"\"")
                            : std::wstring(L"python");
                        const int sc = (fmv_upscale_scale_ == 3 || fmv_upscale_scale_ == 4) ? fmv_upscale_scale_ : 2;
                        const std::wstring params = L"/k \"" + py_exe + L" \"" + state_.paths.upscale_fmv_script.wstring() +
                                                    L"\" --input \"" + state_.paths.dumps_fmv_dir.wstring() +
                                                    L"\" --output \"" + state_.paths.video_packs_dir.wstring() +
                                                    L"\" --scale " + std::to_wstring(sc) + L" --auto-setup\"";
                        ShellExecuteW(hwnd_, L"open", L"cmd.exe", params.c_str(), sr.c_str(), SW_SHOWNORMAL);
                        show_toast("Launched " + std::to_string(sc) + "x AI FMV Upscaler console");
                    });
        my += 42.0f;

        const float qw = (mw - 16.0f) / 3.0f;
        draw_button(rt, "mod_dir_texpacks", D2D1::RectF(mc.left + 16, my, mc.left + 16 + qw, my + 32),
                    "texture_packs/", pal.card_hover, pal.border, pal.text_dim, false, [this]() {
                        open_in_explorer(state_.paths.texture_packs_dir);
                    });
        draw_button(rt, "mod_dir_vidpacks", D2D1::RectF(mc.left + 24 + qw, my, mc.left + 24 + qw * 2.0f, my + 32),
                    "video_packs/", pal.card_hover, pal.border, pal.text_dim, false, [this]() {
                        open_in_explorer(state_.paths.video_packs_dir);
                    });
        draw_button(rt, "mod_dir_mods", D2D1::RectF(mc.left + 32 + qw * 2.0f, my, mc.left + 16 + mw, my + 32),
                    "mods/ & dumps/", pal.card_hover, pal.border, pal.text_dim, false, [this]() {
                        open_in_explorer(packs_view_mode_ == 2 ? state_.paths.mods_dir : state_.paths.game_dir / "dumps");
                    });
        my += 38.0f;
        const float iw = (mw - 8.0f) * 0.5f;
        draw_button(rt, "mod_import_zip", D2D1::RectF(mc.left + 16, my, mc.left + 16 + iw, my + 32),
                    "Import .zip / .nocturne-mod", with_a(pal.accent_crimson, 0.52f), pal.border_hi, pal.accent, false, [this]() {
                        IFileOpenDialog *dlg = nullptr;
                        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
                            COMDLG_FILTERSPEC filters[2] = {
                                {L"Nocturne Mod / Pack Archives (*.zip;*.nocturne-mod)", L"*.zip;*.nocturne-mod"},
                                {L"All Files (*.*)", L"*.*"}
                            };
                            dlg->SetFileTypes(2, filters);
                            dlg->SetTitle(L"Import Mod or HD Pack Archive");
                            if (SUCCEEDED(dlg->Show(hwnd_))) {
                                IShellItem *item = nullptr;
                                if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                                    PWSTR wpath = nullptr;
                                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath)) && wpath) {
                                        ArchivePackKind kind = ArchivePackKind::Unknown;
                                        std::string msg;
                                        if (import_mod_or_pack_archive(state_.paths, std::filesystem::path(wpath), &kind, &msg)) {
                                            refresh_all(state_);
                                            if (kind == ArchivePackKind::GameplayMod) packs_view_mode_ = 2;
                                            else if (kind == ArchivePackKind::VideoPack) packs_view_mode_ = 1;
                                            else if (kind == ArchivePackKind::TexturePack) packs_view_mode_ = 0;
                                        }
                                        show_toast(msg);
                                        CoTaskMemFree(wpath);
                                    }
                                    item->Release();
                                }
                            }
                            dlg->Release();
                        }
                    });
        draw_button(rt, "mod_new_scaffold", D2D1::RectF(mc.left + 24 + iw, my, mc.left + 16 + mw, my + 32),
                    "New Mod Scaffold", pal.card_hover, pal.border, pal.text, false, [this]() {
                        std::string msg;
                        if (create_mod_scaffold(state_.paths, "MyCustomMod", &msg)) {
                            refresh_all(state_);
                            packs_view_mode_ = 2;
                        }
                        show_toast(msg);
                    });
        return;
    }

    // Left: Health Checklist Card
    const D2D1_RECT_F hc{x, y, x + left_w, y + panel_h};
    ID2D1SolidColorBrush *b = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &b)) && b) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(hc, 11, 11), b);
        b->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &b)) && b) {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(hc, 11, 11), b, 1.0f);
        b->Release();
    }
    draw_text(rt, "INSTALLATION & DISC HEALTH", fmt_pill_, D2D1::RectF(hc.left + 16, hc.top + 14, hc.right - 16, hc.top + 30), pal.accent);

    float hy = hc.top + 38.0f;
    const float row_pitch = state_.health.size() >= 8 ? 48.0f : 56.0f;
    for (const auto &item : state_.health) {
        if (SUCCEEDED(rt->CreateSolidColorBrush(item.ok ? pal.good : pal.warn, &b)) && b) {
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hc.left + 24, hy + 10), 4.5f, 4.5f), b);
            b->Release();
        }
        draw_text(rt, item.label, fmt_body_, D2D1::RectF(hc.left + 38, hy, hc.right - 16, hy + 19), pal.text);
        draw_text(rt, item.detail, fmt_small_, D2D1::RectF(hc.left + 38, hy + 19, hc.right - 16, hy + 36), pal.text_dim);
        hy += row_pitch;
    }

    // Right: Maintenance Actions & Quick Folders
    const D2D1_RECT_F ac{right_x, y, right_x + right_w, y + panel_h};
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &b)) && b) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(ac, 11, 11), b);
        b->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.55f), &b)) && b) {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(ac, 11, 11), b, 1.0f);
        b->Release();
    }
    draw_text(rt, "MAINTENANCE ACTIONS", fmt_pill_, D2D1::RectF(ac.left + 16, ac.top + 14, ac.right - 16, ac.top + 30), pal.accent);

    float ay = ac.top + 38.0f;
    const float bw = right_w - 32.0f;
    const bool has_unbaked_vu1 = state_.vu1_cache.unbaked_count > 0;
    const std::string vu_btn_lbl = has_unbaked_vu1
        ? ("Bake Captured VU1 Programs into EXE (+" + std::to_string(state_.vu1_cache.unbaked_count) + " new)")
        : ("Pre-Compile Captured VU1 Programs (" + std::to_string(state_.vu1_cache.baked_programs) + " baked)");
    draw_button(rt, "tool_bake_vu1", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 40),
                vu_btn_lbl,
                has_unbaked_vu1 ? with_a(pal.accent_crimson, 0.72f) : pal.card_hover,
                has_unbaked_vu1 ? pal.border_hi : pal.border, pal.text, false, [this]() {
                    std::error_code ec;
                    if (state_.paths.precompile_vu1_script.empty() || !std::filesystem::is_regular_file(state_.paths.precompile_vu1_script, ec)) {
                        show_toast(game_profile(active_game_).coming_soon_msg);
                        return;
                    }
                    const std::filesystem::path sr = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
                    const std::filesystem::path portable_py = sr / "tools" / "portable" / "python" / "python.exe";
                    const std::wstring py_exe = std::filesystem::is_regular_file(portable_py, ec) ? portable_py.wstring() : L"python";
                    const std::wstring params = L"\"" + state_.paths.precompile_vu1_script.wstring() + L"\" --pause";
                    ShellExecuteW(hwnd_, L"open", py_exe.c_str(), params.c_str(), sr.c_str(), SW_SHOWNORMAL);
                    show_toast("Baking captured VU1 programs into EXE (click Refresh when done)...");
                });
    ay += 48.0f;
    draw_button(rt, "tool_export_bundle", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 40),
                "Export Full Save & State Bundle (.nocturne-save.zip)", with_a(pal.accent_crimson, 0.55f), pal.border_hi, pal.text, false, [this]() {
                    std::string msg;
                    export_save_bundle(state_.paths, nullptr, &msg);
                    show_toast(msg);
                });
    ay += 48.0f;
    draw_button(rt, "tool_backup", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 40),
                (active_game_ == GameId::Koudelka ? "Backup PS1 Memory Card (memcard.mcr)" : "Backup PS2 Memory Card (memcard.ps2)"), with_a(pal.accent_crimson, 0.35f), pal.border_hi, pal.text, false, [this]() {
                    std::string msg;
                    backup_memcard(state_.paths, nullptr, &msg);
                    show_toast(msg);
                });
    ay += 48.0f;
    draw_button(rt, "tool_cache", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 40),
                "Clear Live Recompiler Caches (ee-live / vu1-live)", pal.card_hover, pal.border, pal.text, false, [this]() {
                    const int n = clear_live_caches(state_.paths);
                    refresh_all(state_);
                    show_toast("Cleared " + std::to_string(n) + " cached file(s)");
                });
    ay += 48.0f;
    draw_button(rt, "tool_rebuild", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 40),
                "Run Builder / Verify Discs (Closes Launcher)", pal.card_hover, pal.border, pal.text, false, [this]() {
                    std::error_code ec;
                    if (state_.paths.build_cmd.empty() || !std::filesystem::is_regular_file(state_.paths.build_cmd, ec)) {
                        show_toast(game_profile(active_game_).coming_soon_msg);
                        return;
                    }
                    // Wait 1 second so Shadow Hearts Nocturne.exe exits & unlocks before the builder checks file locks.
                    const std::wstring build_arg = (active_game_ == GameId::ShadowHearts1) ? L"build-sh1"
                                                 : (active_game_ == GameId::FromTheNewWorld) ? L"build-sh3"
                                                 : (active_game_ == GameId::Koudelka) ? L"build-koudelka"
                                                 : L"build-sh2";
                    const std::filesystem::path sr = state_.paths.scripts_root.empty() ? state_.paths.root_dir : state_.paths.scripts_root;
                    const std::wstring params = L"/c \"timeout /t 1 /nobreak >nul & call \"" + state_.paths.build_cmd.wstring() + L"\" " + build_arg + L"\"";
                    ShellExecuteW(nullptr, L"open", L"cmd.exe", params.c_str(), sr.c_str(), SW_SHOWNORMAL);
                    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                });
    ay += 52.0f;

    draw_text(rt, "QUICK OPEN FOLDERS", fmt_pill_, D2D1::RectF(ac.left + 16, ay, ac.right - 16, ay + 18), pal.accent);
    ay += 28.0f;
    const float hw = (bw - 10.0f) * 0.5f;
    draw_button(rt, "dir_game", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + hw, ay + 36),
                "Game Data", pal.card_hover, pal.border, pal.text_dim, false, [this]() { open_in_explorer(state_.paths.game_dir); });
    draw_button(rt, "dir_shots", D2D1::RectF(ac.left + 26 + hw, ay, ac.left + 16 + bw, ay + 36),
                "Screenshots", pal.card_hover, pal.border, pal.text_dim, false, [this]() { open_in_explorer(state_.paths.screenshots_dir); });
    ay += 46.0f;
    draw_button(rt, "dir_states", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + hw, ay + 36),
                "Save States", pal.card_hover, pal.border, pal.text_dim, false, [this]() { open_in_explorer(state_.paths.states_dir); });
    draw_button(rt, "dir_logs", D2D1::RectF(ac.left + 26 + hw, ay, ac.left + 16 + bw, ay + 36),
                "Logs", pal.card_hover, pal.border, pal.text_dim, false, [this]() { open_in_explorer(state_.paths.logs_dir); });
}

void LauncherApp::render_focus_ring(ID2D1RenderTarget *rt, const Palette &pal) {
    if (!gamepad_nav_active_ || focused_id_.empty() || !lightbox_image_.empty()) return;
    for (const auto &h : hits_) {
        if (h.id != focused_id_) continue;
        const float pulse = 0.72f + 0.28f * std::sin(time_sec_ * 4.5f);
        const D2D1_RECT_F outer{h.rect.left - 3.5f, h.rect.top - 3.5f, h.rect.right + 3.5f, h.rect.bottom + 3.5f};
        const D2D1_RECT_F inner{h.rect.left - 1.5f, h.rect.top - 1.5f, h.rect.right + 1.5f, h.rect.bottom + 1.5f};
        ID2D1SolidColorBrush *b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.accent_crimson, 0.36f * pulse), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(outer, 10.0f, 10.0f), b, 3.2f);
            b->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(col_rgba(248, 218, 142), 0.92f * pulse), &b)) && b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(inner, 8.5f, 8.5f), b, 2.0f);
            b->Release();
        }
        return;
    }
}

void LauncherApp::render_status_toast(ID2D1RenderTarget *rt, float w, float h, const Palette &pal) {
    if (status_toast_.empty() || time_sec_ > status_toast_until_) return;
    const float tw = std::min(520.0f, std::max(240.0f, static_cast<float>(status_toast_.size()) * 7.5f + 42.0f));
    const D2D1_RECT_F r{w - tw - 24.0f, h - 54.0f, w - 24.0f, h - 18.0f};
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 10, 10);
    fill_hgrad_rounded(rt, rr, col_rgba(24, 18, 36, 0.96f), col_rgba(52, 22, 38, 0.96f));
    ID2D1SolidColorBrush *b = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.border_hi, &b)) && b) {
        rt->DrawRoundedRectangle(rr, b, 1.4f);
        b->Release();
    }
    draw_text(rt, status_toast_, fmt_body_, r, pal.text, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void LauncherApp::render_lightbox(ID2D1RenderTarget *rt, float w, float h, const Palette &pal) {
    if (lightbox_image_.empty()) return;
    ID2D1SolidColorBrush *b = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(4, 4, 10, 0.88f), &b)) && b) {
        rt->FillRectangle(D2D1::RectF(0, 0, w, h), b);
        b->Release();
    }
    const D2D1_RECT_F box{64, 58, w - 64, h - 58};
    if (ID2D1Bitmap *bmp = load_bitmap_cached(rt, lightbox_image_, 1120, 660)) {
        const D2D1_SIZE_F bs = bmp->GetSize();
        const float bw = box.right - box.left, bh = box.bottom - box.top - 42.0f;
        const float sc = std::min(bw / std::max(1.0f, bs.width), bh / std::max(1.0f, bs.height));
        const float dw = bs.width * sc, dh = bs.height * sc;
        const float dx = (box.left + box.right - dw) * 0.5f;
        const float dy = box.top + (bh - dh) * 0.5f;
        rt->DrawBitmap(bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.border_hi, &b)) && b) {
            rt->DrawRectangle(D2D1::RectF(dx, dy, dx + dw, dy + dh), b, 1.8f);
            b->Release();
        }
    }
    draw_text(rt, lightbox_title_ + "   (Click anywhere or press Esc to close)", fmt_body_,
              D2D1::RectF(box.left, box.bottom - 34, box.right, box.bottom), pal.accent,
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hits_.clear();
    hits_.push_back({"lightbox_close", D2D1::RectF(0, 0, w, h), [this]() { lightbox_image_.clear(); }, nullptr, false});
}

void LauncherApp::render_frame(ID2D1RenderTarget *rt, float w, float h) {
    hits_.clear();
    has_clip_rect_ = false;
    const Palette pal = current_palette();
    rt->BeginDraw();
    render_background(rt, w, h, pal);
    render_titlebar(rt, w, pal);

    if (page_ == Page::Trilogy) {
        render_trilogy(rt, D2D1::RectF(0.0f, kTitlebarH, w, h), pal);
    } else {
        render_sidebar(rt, h, pal);
        const D2D1_RECT_F content{kSidebarW, kTitlebarH, w, h};
        switch (page_) {
        case Page::Home: render_home(rt, content, pal); break;
        case Page::Settings: render_settings(rt, content, pal); break;
        case Page::Trophies: render_trophies(rt, content, pal); break;
        case Page::SavesMedia: render_saves_media(rt, content, pal); break;
        case Page::Tools: render_tools(rt, content, pal); break;
        case Page::Coop: render_coop_lobby(rt, content, pal); break;
        case Page::RingDojo: render_ring_dojo(rt, content, pal); break;
        default: break;
        }
    }

    render_focus_ring(rt, pal);
    render_status_toast(rt, w, h, pal);
    render_lightbox(rt, w, h, pal);

    // Subtle outer gilded hairline border
    ID2D1SolidColorBrush *b = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.72f), &b)) && b) {
        rt->DrawRectangle(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), b, 1.0f);
        b->Release();
    }
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        release_device_resources();
    }
}

void LauncherApp::persist_settings(const std::string &toast_msg) {
    save_settings(state_.paths.keys_ini, state_.settings);
    if (!toast_msg.empty()) show_toast(toast_msg);
}

void LauncherApp::show_toast(const std::string &msg) {
    status_toast_ = msg;
    status_toast_until_ = time_sec_ + 3.4f;
}

void LauncherApp::open_in_explorer(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void LauncherApp::start_anim_timer() {
    if (hwnd_ && !anim_running_ && !game_running_) {
        SetTimer(hwnd_, kTimerAnimId, 16, nullptr);
        anim_running_ = true;
    }
}

void LauncherApp::stop_anim_timer() {
    if (hwnd_ && anim_running_) {
        KillTimer(hwnd_, kTimerAnimId);
        anim_running_ = false;
    }
}

void LauncherApp::poll_coop_lobby() {
    if (!coop_net_.is_open()) return;
    coop_net_.poll();
    // Prune stale LAN beacons (> 8s old)
    const std::uint64_t now = coop::steady_now_ms();
    coop_lan_rooms_.erase(
        std::remove_if(coop_lan_rooms_.begin(), coop_lan_rooms_.end(),
                       [now](const coop::LanRoomInfo &r) { return now - r.last_seen_ms > 8000; }),
        coop_lan_rooms_.end());
}

void LauncherApp::render_coop_lobby(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const float pad = 26.0f;
    const float x0 = area.left + pad;
    const float x1 = area.right - pad;
    const float w = x1 - x0;
    const GameProfile &prof = game_profile(active_game_);
    const int max_p = (active_game_ == GameId::Koudelka) ? 3 : 4;
    coop_settings_.max_players = static_cast<std::uint8_t>(max_p);
    coop_settings_.campaign_name = global_cfg_.coop_campaign.empty() ? "nocturne_coop" : global_cfg_.coop_campaign;
    coop_settings_.game = (active_game_ == GameId::Koudelka) ? coop::GameKind::Koudelka
                        : (active_game_ == GameId::ShadowHearts1) ? coop::GameKind::SH1
                        : (active_game_ == GameId::FromTheNewWorld) ? coop::GameKind::SH3
                        : coop::GameKind::SH2;

    // Ensure callbacks are bound
    auto bind_coop_callbacks = [&]() {
        coop::TransportCallbacks cb;
        cb.on_room_created = [this](const std::string &code, std::uint32_t) {
            show_toast("Co-op Room Ready — Code: " + coop::format_room_code_pretty(code));
        };
        cb.on_joined_room = [this](std::uint8_t slot, std::uint32_t) {
            coop_in_lobby_ = true;
            coop_hosting_ = false;
            show_toast("Joined Co-op Lobby as Player " + std::to_string(unsigned(slot) + 1u));
        };
        cb.on_error = [this](const std::string &why) {
            show_toast("Co-op: " + why);
        };
        cb.on_peer_joined = [this](std::uint8_t slot, const coop::PeerInfo &p) {
            audio_.play(SoundCue::TabSwitch);
            show_toast(p.name + " joined as P" + std::to_string(unsigned(slot) + 1u));
        };
        cb.on_lobby_config = [this](const coop::LobbySettings &cfg) {
            coop_settings_ = cfg;
        };
        cb.on_launch_together = [this](const coop::LobbySettings &cfg, std::uint16_t game_port) {
            coop::LaunchConfig lc;
            lc.enabled = true;
            lc.is_host = false;
            lc.player_name = global_cfg_.coop_player_name;
            lc.relay_addr = global_cfg_.coop_relay_url;
            lc.room_code = coop_net_.room_code();
            lc.direct_addr = coop_net_.peers()[0].direct_ep.valid()
                                 ? (coop::Endpoint{coop_net_.peers()[0].direct_ep.ipv4, game_port}.to_string())
                                 : coop_join_input_;
            lc.settings = cfg;
            coop_net_.close();
            coop_in_lobby_ = false;
            launch_game_with_args(-1, coop::build_coop_cli_args(lc), true);
        };
        cb.on_lan_room_found = [this](const coop::LanRoomInfo &info) {
            for (auto &r : coop_lan_rooms_) {
                if (r.endpoint == info.endpoint) { r = info; return; }
            }
            if (coop_lan_rooms_.size() < 6) coop_lan_rooms_.push_back(info);
        };
        coop_net_.set_callbacks(std::move(cb));
    };

    auto draw_coop_card = [&](D2D1_RECT_F r) {
        ID2D1SolidColorBrush *br = nullptr;
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 10.0f, 10.0f);
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &br)) && br) {
            rt->FillRoundedRectangle(rr, br);
            br->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.border, &br)) && br) {
            rt->DrawRoundedRectangle(rr, br, 1.1f);
            br->Release();
        }
    };
    auto coop_btn = [&](const std::string &id, const std::string &label, D2D1_RECT_F rect,
                        bool primary, std::function<void()> on_click) {
        draw_button(rt, id, rect, label,
                    primary ? with_a(pal.accent_crimson, 0.72f) : pal.card_bg,
                    primary ? pal.border_hi : pal.border,
                    primary ? pal.accent : pal.text, primary, std::move(on_click));
    };

    float y = area.top + 18.0f;
    draw_text(rt, "NATIVE TURN-AUTHORITY CO-OP LOBBY", fmt_h1_, D2D1::RectF(x0, y, x1, y + 32.0f), pal.accent);
    y += 32.0f;
    draw_text(rt, std::string(prof.full_title) + "  •  Up to " + std::to_string(max_p) +
                  " Players  •  0 ms Local Judgment Ring  •  Isolated Auto-Mirrored Co-op Saves",
              fmt_small_, D2D1::RectF(x0, y, x1, y + 20.0f), pal.text_dim);
    y += 28.0f;

    const float col_gap = 18.0f;
    const float left_w = (w - col_gap) * 0.50f;
    const float rx0 = x0 + left_w + col_gap;
    const float right_w = x1 - rx0;

    // --- LEFT COLUMN: Connection, Room Code, Profile & LAN Discovery ---
    const D2D1_RECT_F conn_card{x0, y, x0 + left_w, y + 252.0f};
    draw_coop_card(conn_card);
    float ly = conn_card.top + 14.0f;
    draw_text(rt, coop_in_lobby_ ? (coop_hosting_ ? "HOSTING CO-OP ROOM" : "CONNECTED TO CO-OP ROOM") : "HOST OR JOIN SESSION",
              fmt_card_title_, D2D1::RectF(x0 + 16, ly, conn_card.right - 16, ly + 22), pal.accent);
    ly += 28.0f;

    auto draw_field = [&](const std::string &id, const char *lbl, const std::string &val, int field_idx, float fy) {
        const bool editing = (coop_edit_field_ == field_idx);
        draw_text(rt, lbl, fmt_small_, D2D1::RectF(x0 + 16, fy, x0 + 136, fy + 26), pal.text_dim);
        const D2D1_RECT_F box{x0 + 138, fy, conn_card.right - 16, fy + 26};
        ID2D1SolidColorBrush *br = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(editing ? col_rgba(32, 24, 48, 0.96f) : col_rgba(14, 13, 24, 0.88f), &br)) && br) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 5, 5), br);
            br->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(editing ? pal.accent : pal.border, &br)) && br) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 5, 5), br, editing ? 1.5f : 1.0f);
            br->Release();
        }
        std::string disp = val.empty() ? (field_idx == 3 ? "(Optional: host:47191 for Room Codes)" : "(click to type)") : val;
        if (editing) disp += "|";
        draw_text(rt, disp, fmt_small_, D2D1::RectF(box.left + 8, box.top + 4, box.right - 8, box.bottom - 2),
                  val.empty() && !editing ? pal.text_dim : pal.text);
        hits_.push_back({id, box, [this, field_idx]() {
            coop_edit_field_ = (coop_edit_field_ == field_idx) ? 0 : field_idx;
            if (coop_edit_field_ == 0) save_global_config(state_.paths.root_dir, global_cfg_);
        }, nullptr, false});
    };

    draw_field("coop_f_name", "Player Name", global_cfg_.coop_player_name, 2, ly); ly += 32.0f;
    draw_field("coop_f_camp", "Campaign Save", global_cfg_.coop_campaign, 4, ly); ly += 32.0f;
    draw_field("coop_f_relay", "Relay Server", global_cfg_.coop_relay_url, 3, ly); ly += 32.0f;
    draw_field("coop_f_join", "Code or IP:Port", coop_join_input_, 1, ly); ly += 38.0f;

    const float btn_w = (left_w - 44.0f) * 0.5f;
    if (!coop_in_lobby_) {
        coop_btn("coop_host_btn", "Host Co-op Room", D2D1::RectF(x0 + 16, ly, x0 + 16 + btn_w, ly + 36), true,
                 [this, bind_coop_callbacks]() {
                     save_global_config(state_.paths.root_dir, global_cfg_);
                     bind_coop_callbacks();
                     if (coop_net_.host_room(global_cfg_.coop_player_name, coop_settings_, global_cfg_.coop_relay_url)) {
                         coop_in_lobby_ = true;
                         coop_hosting_ = true;
                         coop_ready_ = true;
                         show_toast("Hosting Co-op Room: " + coop::format_room_code_pretty(coop_net_.room_code()));
                     } else {
                         show_toast("Failed to bind Co-op UDP port.");
                     }
                 });
        coop_btn("coop_join_btn", "Join Room / IP", D2D1::RectF(x0 + 28 + btn_w, ly, conn_card.right - 16, ly + 36), false,
                 [this, bind_coop_callbacks]() {
                     save_global_config(state_.paths.root_dir, global_cfg_);
                     bind_coop_callbacks();
                     const std::string norm = coop::normalize_room_code(coop_join_input_);
                     const bool looks_code = (norm.size() == 6 && coop_join_input_.find('.') == std::string::npos &&
                                              coop_join_input_.find(':') == std::string::npos);
                     if (looks_code && !global_cfg_.coop_relay_url.empty()) {
                         coop_net_.join_by_code(global_cfg_.coop_player_name, norm, global_cfg_.coop_relay_url);
                         show_toast("Joining Room " + coop::format_room_code_pretty(norm) + "...");
                     } else {
                         coop_net_.join_direct(global_cfg_.coop_player_name, coop_join_input_);
                         show_toast("Connecting to " + coop_join_input_ + "...");
                     }
                 });
    } else {
        const std::string code_lbl = "Code: " + coop::format_room_code_pretty(coop_net_.room_code()) +
                                     "  (Port " + std::to_string(coop_net_.local_port()) + ")";
        coop_btn("coop_copy_code", code_lbl, D2D1::RectF(x0 + 16, ly, x0 + 16 + btn_w, ly + 36), false,
                 [this]() {
                     const std::string txt = coop::format_room_code_pretty(coop_net_.room_code());
                     if (OpenClipboard(hwnd_)) {
                         EmptyClipboard();
                         HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, txt.size() + 1);
                         if (hg) {
                             std::memcpy(GlobalLock(hg), txt.c_str(), txt.size() + 1);
                             GlobalUnlock(hg);
                             SetClipboardData(CF_TEXT, hg);
                         }
                         CloseClipboard();
                     }
                     show_toast("Copied Room Code: " + txt);
                 });
        coop_btn("coop_leave_btn", "Leave Room", D2D1::RectF(x0 + 28 + btn_w, ly, conn_card.right - 16, ly + 36), false,
                 [this]() {
                     coop_net_.close();
                     coop_in_lobby_ = false;
                     coop_hosting_ = false;
                     coop_ready_ = false;
                     show_toast("Left Co-op Lobby.");
                 });
    }

    // --- LEFT BOTTOM: LAN Auto-Discovery ---
    const D2D1_RECT_F lan_card{x0, conn_card.bottom + 14.0f, x0 + left_w, area.bottom - 18.0f};
    draw_coop_card(lan_card);
    float dly = lan_card.top + 14.0f;
    draw_text(rt, "LAN AUTO-DISCOVERY (" + std::to_string(coop_lan_rooms_.size()) + " FOUND)",
              fmt_card_title_, D2D1::RectF(x0 + 16, dly, lan_card.right - 16, dly + 22), pal.accent);
    dly += 28.0f;
    if (coop_lan_rooms_.empty()) {
        draw_text(rt, "Listening on UDP 47190 for nearby Nocturne hosts...\nFor worldwide play, enter a Relay Server URL above or Direct IP / Tailscale.",
                  fmt_small_, D2D1::RectF(x0 + 16, dly, lan_card.right - 16, dly + 52), pal.text_dim);
    } else {
        for (std::size_t i = 0; i < coop_lan_rooms_.size() && dly + 34.0f < lan_card.bottom - 10.0f; ++i) {
            const auto &rm = coop_lan_rooms_[i];
            const std::string lbl = rm.host_name + "  •  " + rm.campaign_name + " (" +
                                    std::to_string(rm.player_count) + "/" + std::to_string(rm.max_players) + ")  " +
                                    rm.endpoint.to_string();
            coop_btn("coop_lan_" + std::to_string(i), lbl,
                     D2D1::RectF(x0 + 16, dly, lan_card.right - 16, dly + 30.0f), false,
                     [this, rm, bind_coop_callbacks]() {
                         coop_join_input_ = rm.endpoint.to_string();
                         bind_coop_callbacks();
                         coop_net_.join_direct(global_cfg_.coop_player_name, coop_join_input_);
                         show_toast("Joining LAN host " + rm.host_name + "...");
                     });
            dly += 36.0f;
        }
    }

    // --- RIGHT COLUMN: Party Roster, Slot Split, Rules & Launch Together ---
    const D2D1_RECT_F right_card{rx0, y, rx0 + right_w, area.bottom - 18.0f};
    draw_coop_card(right_card);
    float ry = right_card.top + 14.0f;
    draw_text(rt, "PARTY ROSTER & TURN-AUTHORITY SPLIT",
              fmt_card_title_, D2D1::RectF(rx0 + 16, ry, right_card.right - 16, ry + 22), pal.accent);
    ry += 28.0f;

    const auto &peers = coop_net_.peers();
    for (int i = 0; i < max_p; ++i) {
        const bool conn = coop_in_lobby_ ? peers[i].connected : (i == 0);
        const std::string pname = conn ? (coop_in_lobby_ ? peers[i].name : global_cfg_.coop_player_name) : "Open Slot";
        const std::string meta = conn
            ? ((i == 0 ? "HOST" : (peers[i].ready ? "READY" : "WAITING")) +
               (peers[i].rtt_ms > 0 ? ("  •  " + std::to_string(peers[i].rtt_ms) + " ms") : std::string()))
            : "Waiting for partner...";
        const D2D1_RECT_F pr{rx0 + 16, ry, right_card.right - 16, ry + 32.0f};
        ID2D1SolidColorBrush *br = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(conn ? col_rgba(26, 22, 42, 0.90f) : col_rgba(14, 13, 22, 0.55f), &br)) && br) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(pr, 6, 6), br);
            br->Release();
        }
        draw_text(rt, "P" + std::to_string(i + 1) + "  " + pname, fmt_body_,
                  D2D1::RectF(pr.left + 10, pr.top + 6, pr.right - 140, pr.bottom - 4), conn ? pal.text : pal.text_dim);
        draw_text(rt, meta, fmt_small_,
                  D2D1::RectF(pr.right - 140, pr.top + 8, pr.right - 10, pr.bottom - 4), conn ? pal.accent : pal.text_dim);
        ry += 38.0f;
    }

    ry += 6.0f;
    draw_text(rt, "BATTLE PARTY SLOT OWNERSHIP (CLICK TO ASSIGN)", fmt_small_,
              D2D1::RectF(rx0 + 16, ry, right_card.right - 16, ry + 18), pal.text_dim);
    ry += 22.0f;
    const float sw = (right_w - 32.0f - (max_p - 1) * 8.0f) / static_cast<float>(max_p);
    for (int s = 0; s < max_p; ++s) {
        const float sx = rx0 + 16.0f + s * (sw + 8.0f);
        const std::string slbl = "Slot " + std::to_string(s + 1) + ": P" + std::to_string((coop_settings_.slot_owner[s] % max_p) + 1);
        coop_btn("coop_slot_" + std::to_string(s), slbl, D2D1::RectF(sx, ry, sx + sw, ry + 30.0f), false,
                 [this, s, max_p]() {
                     coop_settings_.slot_owner[s] = static_cast<std::uint8_t>((coop_settings_.slot_owner[s] + 1) % max_p);
                     if (coop_in_lobby_ && coop_hosting_) coop_net_.broadcast_lobby_config(coop_settings_);
                 });
    }
    ry += 40.0f;

    // Signature Co-op feature toggles
    auto draw_coop_toggle = [&](const std::string &id, const char *lbl, bool &flag) {
        const D2D1_RECT_F tr{rx0 + 16, ry, right_card.right - 16, ry + 26.0f};
        draw_text(rt, lbl, fmt_small_, D2D1::RectF(tr.left, tr.top + 3, tr.right - 70, tr.bottom), pal.text);
        coop_btn(id, flag ? "ON" : "OFF", D2D1::RectF(tr.right - 64, tr.top, tr.right, tr.bottom), flag,
                 [this, &flag]() {
                     flag = !flag;
                     if (coop_in_lobby_ && coop_hosting_) coop_net_.broadcast_lobby_config(coop_settings_);
                 });
        ry += 32.0f;
    };
    draw_coop_toggle("coop_tog_auto", "Auto Battle-Turn Authority", coop_settings_.auto_battle_turn);
    draw_coop_toggle("coop_tog_relay", "Relay Combos (Mid-Combo Ring Baton Pass)", coop_settings_.relay_combos);
    draw_coop_toggle("coop_tog_hud", "Turn Spotlight HUD & Partner Callouts", coop_settings_.show_hud);
    draw_coop_toggle("coop_tog_mc", "Auto-Mirror Isolated Co-op Memory Card", coop_settings_.mirror_memcard);

    // Bottom primary action: Launch Together (Host) or Toggle Ready (Guest)
    const D2D1_RECT_F launch_r{rx0 + 16, right_card.bottom - 52.0f, right_card.right - 16, right_card.bottom - 14.0f};
    if (!coop_in_lobby_ || coop_hosting_) {
        coop_btn("coop_launch_btn",
                 coop_in_lobby_ ? "LAUNCH CO-OP TOGETHER" : "START HOSTED CO-OP SESSION",
                 launch_r, true,
                 [this]() {
                     save_global_config(state_.paths.root_dir, global_cfg_);
                     const std::uint16_t game_port = coop::kDefaultGamePort;
                     if (coop_in_lobby_ && coop_hosting_) {
                         coop_net_.send_launch_together(coop_settings_, game_port);
                     }
                     coop::LaunchConfig lc;
                     lc.enabled = true;
                     lc.is_host = true;
                     lc.player_name = global_cfg_.coop_player_name;
                     lc.relay_addr = global_cfg_.coop_relay_url;
                     lc.local_port = game_port;
                     lc.settings = coop_settings_;
                     coop_net_.close();
                     coop_in_lobby_ = false;
                     launch_game_with_args(-1, coop::build_coop_cli_args(lc), true);
                 });
    } else {
        coop_btn("coop_ready_btn", coop_ready_ ? "READY! (Waiting for Host)" : "CLICK TO READY UP",
                 launch_r, coop_ready_,
                 [this]() {
                     coop_ready_ = !coop_ready_;
                     coop_net_.set_ready(coop_ready_);
                 });
    }
}

void LauncherApp::trigger_ring_dojo_press() {
    const DojoGrade g = ring_dojo_.trigger_press();
    if (g == DojoGrade::Perfect) audio_.play(SoundCue::LaunchPlay);
    else if (g == DojoGrade::Hit) audio_.play(SoundCue::TabSwitch);
    if (ring_dojo_.best_streak() > global_cfg_.dojo_best_streak ||
        ring_dojo_.best_perfect_streak() > global_cfg_.dojo_best_perfect) {
        global_cfg_.dojo_best_streak = std::max(global_cfg_.dojo_best_streak, ring_dojo_.best_streak());
        global_cfg_.dojo_best_perfect = std::max(global_cfg_.dojo_best_perfect, ring_dojo_.best_perfect_streak());
        save_global_config(state_.paths.root_dir, global_cfg_);
    }
}

void LauncherApp::render_ring_dojo(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const float pad = 26.0f;
    const float x0 = area.left + pad;
    const float x1 = area.right - pad;
    const float w = x1 - x0;
    float y = area.top + 18.0f;

    draw_text(rt, "JUDGMENT RING DOJO", fmt_h1_, D2D1::RectF(x0, y, x1, y + 32.0f), pal.accent);
    y += 32.0f;
    draw_text(rt, "0 ms Native Practice Simulator  •  Click Ring, press SPACE / ENTER, or Gamepad A to spin & strike",
              fmt_small_, D2D1::RectF(x0, y, x1, y + 20.0f), pal.text_dim);
    y += 28.0f;

    const float col_gap = 18.0f;
    const float left_w = (w - col_gap) * 0.52f;
    const float rx0 = x0 + left_w + col_gap;
    const float right_w = x1 - rx0;

    auto draw_panel = [&](D2D1_RECT_F r) {
        ID2D1SolidColorBrush *br = nullptr;
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 11.0f, 11.0f);
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_bg, &br)) && br) {
            rt->FillRoundedRectangle(rr, br);
            br->Release();
        }
        if (SUCCEEDED(rt->CreateSolidColorBrush(pal.border, &br)) && br) {
            rt->DrawRoundedRectangle(rr, br, 1.1f);
            br->Release();
        }
    };

    // --- LEFT PANEL: Interactive Direct2D Judgment Ring ---
    const D2D1_RECT_F ring_card{x0, y, x0 + left_w, area.bottom - 18.0f};
    draw_panel(ring_card);

    const float cx = (ring_card.left + ring_card.right) * 0.5f;
    const float cy = ring_card.top + (ring_card.bottom - ring_card.top - 96.0f) * 0.5f + 10.0f;
    const float outer_r = std::min(left_w * 0.36f, (ring_card.bottom - ring_card.top - 150.0f) * 0.48f);
    const float inner_r = outer_r * 0.24f;
    constexpr float kPi = 3.1415926535f;

    auto deg_pt = [&](float deg, float r) -> D2D1_POINT_2F {
        const float a = (deg - 90.0f) * (kPi / 180.0f);
        return D2D1::Point2F(cx + std::cos(a) * r, cy + std::sin(a) * r);
    };

    auto fill_wedge = [&](float a0, float a1, D2D1_COLOR_F col) {
        if (a1 <= a0 || !d2d_factory_) return;
        ID2D1PathGeometry *geo = nullptr;
        if (FAILED(d2d_factory_->CreatePathGeometry(&geo)) || !geo) return;
        ID2D1GeometrySink *sink = nullptr;
        if (SUCCEEDED(geo->Open(&sink)) && sink) {
            const float rad_w = outer_r - 6.0f;
            sink->BeginFigure(D2D1::Point2F(cx, cy), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(deg_pt(a0, rad_w));
            sink->AddArc(D2D1::ArcSegment(
                deg_pt(a1, rad_w), D2D1::SizeF(rad_w, rad_w), 0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                (a1 - a0 > 180.0f) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            ID2D1SolidColorBrush *br = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(col, &br)) && br) {
                rt->FillGeometry(geo, br);
                br->Release();
            }
        }
        geo->Release();
    };

    ID2D1SolidColorBrush *br = nullptr;
    // Obsidian base disc + brass rim
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(11, 10, 20, 0.96f), &br)) && br) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), outer_r, outer_r), br);
        br->Release();
    }

    // Draw Hit (emerald) and Perfect Strike (crimson) wedges
    const float zalpha = ring_dojo_.zone_alpha();
    for (const auto &z : ring_dojo_.zones()) {
        float a = z.resolved ? 0.35f : zalpha;
        D2D1_COLOR_F hit_col = col_rgba(46, 172, 118, 0.62f * a);
        D2D1_COLOR_F str_col = col_rgba(220, 52, 62, 0.88f * a);
        if (z.resolved && z.result == DojoGrade::Perfect) {
            hit_col = col_rgba(240, 198, 74, 0.55f);
            str_col = col_rgba(255, 224, 96, 0.92f);
        } else if (z.resolved && z.result == DojoGrade::Miss) {
            hit_col = col_rgba(120, 42, 46, 0.35f);
            str_col = col_rgba(180, 42, 46, 0.55f);
        }
        fill_wedge(z.start_deg, z.strike_deg, hit_col);
        fill_wedge(z.strike_deg, z.end_deg, str_col);
    }

    // Radial tick marks every 30 degrees
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.45f), &br)) && br) {
        for (int deg = 0; deg < 360; deg += 30) {
            rt->DrawLine(deg_pt(static_cast<float>(deg), outer_r - 12.0f),
                         deg_pt(static_cast<float>(deg), outer_r - 2.0f), br, 1.2f);
        }
        br->Release();
    }

    // Double gilded brass outer ring
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.accent, &br)) && br) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), outer_r, outer_r), br, 3.0f);
        br->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border_hi, 0.60f), &br)) && br) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), outer_r - 6.0f, outer_r - 6.0f), br, 1.2f);
        br->Release();
    }

    // Rotating glowing golden needle
    const D2D1_POINT_2F tip = deg_pt(ring_dojo_.needle_deg(), outer_r - 3.0f);
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(255, 226, 128, 0.38f), &br)) && br) {
        rt->DrawLine(D2D1::Point2F(cx, cy), tip, br, 7.0f);
        br->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(255, 242, 188, 1.0f), &br)) && br) {
        rt->DrawLine(D2D1::Point2F(cx, cy), tip, br, 2.6f);
        br->Release();
    }

    // Center gothic hub medallion
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(22, 16, 32, 0.98f), &br)) && br) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), inner_r, inner_r), br);
        br->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.accent, &br)) && br) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), inner_r, inner_r), br, 2.0f);
        br->Release();
    }

    // Clickable hit region over the entire Judgment Ring
    const D2D1_RECT_F ring_hit{cx - outer_r, cy - outer_r, cx + outer_r, cy + outer_r};
    hits_.push_back({"dojo_ring_click", ring_hit, [this]() { trigger_ring_dojo_press(); }, nullptr, false});

    // Bottom readout banner inside left panel
    const D2D1_COLOR_F grade_col = (ring_dojo_.last_grade() == DojoGrade::Perfect) ? col_rgba(255, 218, 88)
                                 : (ring_dojo_.last_grade() == DojoGrade::Hit) ? col_rgba(92, 224, 156)
                                 : (ring_dojo_.last_grade() == DojoGrade::Miss) ? col_rgba(240, 88, 92)
                                 : pal.accent;
    draw_text(rt, ring_dojo_.last_label(), fmt_card_title_,
              D2D1::RectF(ring_card.left + 16, ring_card.bottom - 86, ring_card.right - 16, ring_card.bottom - 58),
              grade_col, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_button(rt, "dojo_spin_btn",
                D2D1::RectF(ring_card.left + 24, ring_card.bottom - 50, ring_card.right - 24, ring_card.bottom - 14),
                ring_dojo_.sweeping() ? "STRIKE NOW!  (SPACE / CLICK / A)" : "SPIN JUDGMENT RING  (SPACE / CLICK / A)",
                with_a(pal.accent_crimson, 0.72f), pal.border_hi, pal.accent, true,
                [this]() { trigger_ring_dojo_press(); });

    // --- RIGHT PANEL: 6 Modes, Millisecond Timing Graph & Live Game HUD Toggles ---
    const D2D1_RECT_F right_card{rx0, y, rx0 + right_w, area.bottom - 18.0f};
    draw_panel(right_card);
    float ry = right_card.top + 14.0f;

    draw_text(rt, "PRACTICE RING MODE", fmt_card_title_, D2D1::RectF(rx0 + 16, ry, right_card.right - 16, ry + 22), pal.accent);
    ry += 28.0f;
    const float mw = (right_w - 40.0f) * 0.5f;
    for (int m = 0; m < static_cast<int>(DojoMode::Count); ++m) {
        const auto dm = static_cast<DojoMode>(m);
        const bool sel = (ring_dojo_.mode() == dm);
        const float bx = rx0 + 16.0f + (m % 2) * (mw + 8.0f);
        const float by = ry + (m / 2) * 34.0f;
        draw_button(rt, "dojo_mode_" + std::to_string(m), D2D1::RectF(bx, by, bx + mw, by + 28.0f),
                    dojo_mode_name(dm), sel ? with_a(pal.accent_crimson, 0.65f) : pal.card_hover,
                    sel ? pal.border_hi : pal.border, sel ? pal.accent : pal.text, false,
                    [this, dm]() { ring_dojo_.set_mode(dm); audio_.play(SoundCue::TabSwitch); });
    }
    ry += 108.0f;
    draw_text(rt, dojo_mode_desc(ring_dojo_.mode()), fmt_small_,
              D2D1::RectF(rx0 + 16, ry, right_card.right - 16, ry + 34.0f), pal.text_dim);
    ry += 40.0f;

    // Session & Personal Best telemetry
    char stat1[128], stat2[128];
    std::snprintf(stat1, sizeof(stat1), "Hit Rate: %.0f%% (%d/%d)   •   Perfects: %d (%.0f%%)",
                  static_cast<double>(ring_dojo_.hit_rate_pct()), ring_dojo_.hits(), ring_dojo_.attempts(),
                  ring_dojo_.perfects(), static_cast<double>(ring_dojo_.perfect_rate_pct()));
    std::snprintf(stat2, sizeof(stat2), "Streak: %d  (Best: %d)   •   Perfect Streak: %d  (Best: %d)",
                  ring_dojo_.streak(), ring_dojo_.best_streak(),
                  ring_dojo_.perfect_streak(), ring_dojo_.best_perfect_streak());
    draw_text(rt, "PRECISION TELEMETRY", fmt_card_title_, D2D1::RectF(rx0 + 16, ry, right_card.right - 16, ry + 20), pal.accent);
    ry += 24.0f;
    draw_text(rt, stat1, fmt_small_, D2D1::RectF(rx0 + 16, ry, right_card.right - 16, ry + 18), pal.text); ry += 20.0f;
    draw_text(rt, stat2, fmt_small_, D2D1::RectF(rx0 + 16, ry, right_card.right - 16, ry + 18), pal.text_dim); ry += 24.0f;

    // Visual Timing Offset History Graph (-40 ms Early <-- 0 ms Perfect --> +40 ms Late)
    const D2D1_RECT_F gbox{rx0 + 16, ry, right_card.right - 16, ry + 78.0f};
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(12, 11, 22, 0.90f), &br)) && br) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(gbox, 6, 6), br);
        br->Release();
    }
    const float mid_y = (gbox.top + gbox.bottom) * 0.5f;
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.accent, 0.55f), &br)) && br) {
        rt->DrawLine(D2D1::Point2F(gbox.left + 8, mid_y), D2D1::Point2F(gbox.right - 8, mid_y), br, 1.0f);
        br->Release();
    }
    const auto &hist = ring_dojo_.history();
    const float slot_w = (gbox.right - gbox.left - 24.0f) / 14.0f;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        const float bx = gbox.left + 12.0f + static_cast<float>(i) * slot_w;
        const float norm = std::clamp(hist[i].delta_ms / 45.0f, -1.0f, 1.0f);
        const float h_px = std::max(4.0f, std::fabs(norm) * ((gbox.bottom - gbox.top) * 0.42f));
        const float y0 = (norm < 0.0f) ? (mid_y - h_px) : mid_y;
        const D2D1_COLOR_F bc = (hist[i].grade == DojoGrade::Perfect) ? col_rgba(255, 218, 88)
                              : (hist[i].grade == DojoGrade::Hit) ? col_rgba(88, 216, 148)
                              : col_rgba(232, 74, 82);
        if (SUCCEEDED(rt->CreateSolidColorBrush(bc, &br)) && br) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx + 2, y0, bx + slot_w - 4, y0 + h_px), 2, 2), br);
            br->Release();
        }
    }
    draw_text(rt, "EARLY (-ms)", fmt_small_, D2D1::RectF(gbox.left + 8, gbox.top + 3, gbox.left + 110, gbox.top + 18), pal.text_dim);
    draw_text(rt, "LATE (+ms)", fmt_small_, D2D1::RectF(gbox.left + 8, gbox.bottom - 18, gbox.left + 110, gbox.bottom - 3), pal.text_dim);
    ry += 90.0f;

    // Live In-Game Battle Ring HUD & Strict Ring Challenge toggles
    draw_toggle_row(rt, "dojo_ring_hud", rx0 + 10, ry, right_w - 20, "In-Game Battle Ring Streak HUD",
                    "Shows live Hit % & Perfect count in the corner during SH2/SH3 battles",
                    state_.settings.ring_hud, pal, [this](bool v) {
                        state_.settings.ring_hud = v;
                        persist_settings(v ? "Battle Ring Streak HUD: ON" : "Battle Ring Streak HUD: OFF");
                    });
    ry += 46.0f;
    draw_toggle_row(rt, "dojo_strict_ring", rx0 + 10, ry, right_w - 20, "Strict Ring Challenge Mode",
                    "Tracks an unbroken zero-miss Judgment Ring challenge badge live in-game",
                    state_.settings.strict_ring_challenge, pal, [this](bool v) {
                        state_.settings.strict_ring_challenge = v;
                        persist_settings(v ? "Strict Ring Challenge: ON" : "Strict Ring Challenge: OFF");
                    });
    ry += 50.0f;
    draw_button(rt, "dojo_reset_btn", D2D1::RectF(rx0 + 16, ry, right_card.right - 16, ry + 30.0f),
                "Reset Session Stats", pal.card_hover, pal.border, pal.text_dim, false,
                [this]() { ring_dojo_.reset_stats(); });
}

void LauncherApp::launch_game(int load_slot) {
    launch_game_with_args(load_slot, "", false);
}

void LauncherApp::launch_game_with_args(int load_slot, const std::string &extra_args, bool is_coop) {
    if (game_running_ || !state_.ready_to_play) return;
    game_launched_coop_ = is_coop;
    audio_.play(SoundCue::LaunchPlay);
    persist_settings();

    const GameProfile &prof = game_profile(active_game_);
    // Ensure root_dir (which holds shared SDL2.dll, dxcompiler.dll, dxil.dll, libc++.dll, etc.)
    // is at the front of PATH so per-game subfolder EXEs (games/<slug>/SH2C.exe) load cleanly with zero DLL copies.
    {
        const std::wstring root_w = state_.paths.root_dir.wstring();
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
    }
    SetEnvironmentVariableW(L"PS2_WINDOW_TITLE", utf8_to_wide(prof.full_title).c_str());
    SetEnvironmentVariableW(L"PS2_SCREENSHOTS_DIR", state_.paths.screenshots_dir.wstring().c_str());
    SetEnvironmentVariableW(L"PS2_SCREENSHOT_PREFIX", utf8_to_wide(prof.screenshot_prefix).c_str());
    SetEnvironmentVariableW(L"PS2_ACTIVE_MODS", utf8_to_wide(state_.settings.active_mods).c_str());
    SetEnvironmentVariableW(L"PS2_GPU_HW", state_.settings.hw_rasterizer ? L"1" : L"0");
    SetEnvironmentVariableW(L"PS2_GPU_HW_DUAL", state_.settings.dual_source_blend ? L"1" : L"0");
    SetEnvironmentVariableW(L"PS2_VRAM_GUARD", state_.settings.vram_guard ? L"1" : L"0");
    SetEnvironmentVariableW(L"PS2_TEX_HOT_RELOAD", state_.settings.tex_hot_reload ? L"1" : L"0");
    SetEnvironmentVariableW(L"PS2_GPU_SUBMIT_THREAD", state_.settings.gpu_submit_thread ? L"1" : L"0");
    if (state_.settings.vram_budget > 0) {
        SetEnvironmentVariableW(L"PS2_VRAM_BUDGET_MB", std::to_wstring(state_.settings.vram_budget).c_str());
    } else {
        SetEnvironmentVariableW(L"PS2_VRAM_BUDGET_MB", nullptr);
    }
    std::wstring cmd = build_launch_command(state_.paths, state_.settings.launcher_fullscreen, load_slot, extra_args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        state_.paths.root_dir.c_str(), &si, &pi)) {
        show_toast(std::string("Failed to launch ") + prof.exe_name);
        return;
    }
    CloseHandle(pi.hThread);

    if (!state_.settings.launcher_hide_on_play) {
        CloseHandle(pi.hProcess);
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }

    game_running_ = true;
    game_launch_time_sec_ = static_cast<double>(GetTickCount64()) / 1000.0;
    stop_anim_timer();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    HWND target_hwnd = hwnd_;
    HANDLE hproc = pi.hProcess;
    std::thread([target_hwnd, hproc]() {
        WaitForSingleObject(hproc, INFINITE);
        CloseHandle(hproc);
        if (target_hwnd) PostMessageW(target_hwnd, kWmGameExited, 0, 0);
    }).detach();
}

namespace {
bool is_navigable_hit_id(const std::string &id) {
    if (id.empty()) return false;
    if (id == "win_min" || id == "win_close" || id == "lightbox_close") return false;
    if (id.size() >= 7 && id.compare(id.size() - 7, 7, "_scroll") == 0) return false;
    return true;
}
}  // namespace

void LauncherApp::ensure_valid_focus() {
    for (const auto &h : hits_) {
        if (h.id == focused_id_ && is_navigable_hit_id(h.id)) return;
    }
    if (page_ == Page::Trilogy) {
        const std::string want = "trilogy_card_" + std::to_string(static_cast<int>(active_game_));
        for (const auto &h : hits_) {
            if (h.id == want) { focused_id_ = h.id; return; }
        }
    }
    // Prefer primary hero action on Home
    if (page_ == Page::Home) {
        for (const auto &h : hits_) {
            if (h.id == "home_play") { focused_id_ = h.id; return; }
        }
    }
    // On other pages, prefer the first content item (not sidebar nav_*) as default initial focus
    for (const auto &h : hits_) {
        if (is_navigable_hit_id(h.id) && h.id.rfind("nav_", 0) != 0) {
            focused_id_ = h.id;
            return;
        }
    }
    for (const auto &h : hits_) {
        if (is_navigable_hit_id(h.id)) {
            focused_id_ = h.id;
            return;
        }
    }
    focused_id_.clear();
}

void LauncherApp::gamepad_switch_page(int delta) {
    if (!lightbox_image_.empty()) return;
    gamepad_nav_active_ = true;
    if (page_ == Page::Trilogy) {
        int cur_idx = static_cast<int>(active_game_);
        if (focused_id_.rfind("trilogy_card_", 0) == 0 && focused_id_.size() > 13) {
            cur_idx = std::atoi(focused_id_.c_str() + 13);
        }
        const int next_idx = ((cur_idx + delta) % 4 + 4) % 4;
        focused_id_ = "trilogy_card_" + std::to_string(next_idx);
        audio_.play(SoundCue::Hover);
        return;
    }
    const int n = static_cast<int>(Page::Count);
    const int cur = static_cast<int>(page_);
    const int next = ((cur + delta) % n + n) % n;
    if (next != cur) {
        page_ = static_cast<Page>(next);
        armed_delete_slot_ = -1;
        focused_id_.clear();
        audio_.play(SoundCue::TabSwitch);
    }
}

void LauncherApp::gamepad_navigate(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    if (!lightbox_image_.empty()) return;
    gamepad_nav_active_ = true;
    ensure_valid_focus();

    const HitRegion *cur = nullptr;
    for (const auto &h : hits_) {
        if (h.id == focused_id_) { cur = &h; break; }
    }
    if (!cur) return;

    // Horizontal input on a focused slider adjusts its value directly
    if (dx != 0 && dy == 0 && cur->is_slider && cur->on_drag) {
        const float step = (cur->id == "set_scale") ? (1.0f / 7.0f) : 0.05f;
        const float next_val = std::clamp(cur->slider_val + static_cast<float>(dx) * step, 0.0f, 1.0f);
        auto fn = cur->on_drag;
        fn(next_val);
        audio_.play(SoundCue::Hover);
        return;
    }

    const float cx = (cur->rect.left + cur->rect.right) * 0.5f;
    const float cy = (cur->rect.top + cur->rect.bottom) * 0.5f;
    const HitRegion *best = nullptr;
    float best_score = 1e9f;
    for (const auto &h : hits_) {
        if (&h == cur || !is_navigable_hit_id(h.id)) continue;
        const float tx = (h.rect.left + h.rect.right) * 0.5f;
        const float ty = (h.rect.top + h.rect.bottom) * 0.5f;
        const float vx = tx - cx;
        const float vy = ty - cy;
        float primary = 0.0f, secondary = 0.0f;
        if (dx != 0) {
            primary = vx * static_cast<float>(dx);
            secondary = std::fabs(vy);
        } else {
            primary = vy * static_cast<float>(dy);
            secondary = std::fabs(vx);
        }
        if (primary <= 4.0f) continue;
        const float score = primary + secondary * 2.4f;
        if (score < best_score) {
            best_score = score;
            best = &h;
        }
    }

    if (best) {
        focused_id_ = best->id;
        audio_.play(SoundCue::Hover);
        return;
    }

    // No neighbor in direction: scroll active scrollable page if vertical input
    if (dy != 0) {
        const float step = dy > 0 ? 72.0f : -72.0f;
        if (page_ == Page::Settings) {
            settings_scroll_ = std::clamp(settings_scroll_ + step, 0.0f, settings_max_scroll_);
        } else if (page_ == Page::Trophies) {
            trophy_scroll_ = std::max(0, trophy_scroll_ + (dy > 0 ? 2 : -2));
        } else if (page_ == Page::SavesMedia) {
            media_scroll_ = std::clamp(media_scroll_ + step, 0.0f, media_max_scroll_);
        } else if (page_ == Page::Tools && tools_subtab_ == 1) {
            tools_mods_scroll_ = std::clamp(tools_mods_scroll_ + step, 0.0f, tools_mods_max_scroll_);
        }
    }
}

bool LauncherApp::gamepad_activate() {
    gamepad_nav_active_ = true;
    if (!lightbox_image_.empty()) {
        lightbox_image_.clear();
        audio_.play(SoundCue::Click);
        return true;
    }
    ensure_valid_focus();
    HitRegion matched;
    bool found = false;
    for (const auto &h : hits_) {
        if (h.id == focused_id_) { matched = h; found = true; break; }
    }
    if (!found || !matched.on_click) return false;
    const bool silent_click = (matched.id == "home_play" || matched.id == "home_continue" ||
                               matched.id.rfind("play_slot_", 0) == 0 || matched.id.rfind("nav_", 0) == 0 ||
                               matched.id.rfind("trilogy_card_", 0) == 0);
    if (!silent_click) audio_.play(SoundCue::Click);
    matched.on_click();
    return true;
}

void LauncherApp::gamepad_back() {
    gamepad_nav_active_ = true;
    if (!lightbox_image_.empty()) {
        lightbox_image_.clear();
        audio_.play(SoundCue::Click);
        return;
    }
    if (armed_delete_slot_ >= 0) {
        armed_delete_slot_ = -1;
        audio_.play(SoundCue::Click);
        return;
    }
    if (page_ == Page::Home) {
        return_to_trilogy();
        audio_.play(SoundCue::TabSwitch);
    } else if (page_ != Page::Trilogy) {
        page_ = Page::Home;
        focused_id_ = "home_play";
        audio_.play(SoundCue::TabSwitch);
    }
}

void LauncherApp::set_gamepad_nav_preview(bool active, const std::string &focus_id, bool sony) {
    gamepad_nav_active_ = active;
    gamepad_connected_ = active;
    gamepad_is_sony_ = sony;
    focused_id_ = focus_id;
}

void LauncherApp::poll_gamepad() {
#ifdef _WIN32
    if (!hwnd_ || game_running_ || GetForegroundWindow() != hwnd_) {
        gp_prev_mask_ = 0;
        gp_repeat_dx_ = gp_repeat_dy_ = 0;
        return;
    }
    const GamepadSample sample = sample_system_gamepad();
    gamepad_connected_ = sample.connected;
    gamepad_is_sony_ = sample.is_sony;
    if (!sample.connected) {
        gp_prev_mask_ = 0;
        gp_repeat_dx_ = gp_repeat_dy_ = 0;
        return;
    }

    const std::uint32_t m = sample.mask;
    const std::uint32_t pressed = m & ~gp_prev_mask_;
    gp_prev_mask_ = m;

    bool dirty = false;
    if (pressed & GP_LB) { gamepad_switch_page(-1); dirty = true; }
    if (pressed & GP_RB) { gamepad_switch_page(+1); dirty = true; }
    if (pressed & GP_A) { if (gamepad_activate()) dirty = true; }
    if (pressed & GP_B) { gamepad_back(); dirty = true; }
    if (pressed & GP_Y) {
        gamepad_nav_active_ = true;
        if (page_ == Page::SavesMedia) {
            saves_subtab_ = 1 - saves_subtab_;
            media_scroll_ = 0.0f;
            armed_delete_slot_ = -1;
            focused_id_.clear();
            audio_.play(SoundCue::TabSwitch);
            dirty = true;
        } else if (page_ == Page::Tools) {
            tools_subtab_ = (tools_subtab_ + 1) % 3;
            focused_id_.clear();
            audio_.play(SoundCue::TabSwitch);
            dirty = true;
        } else if (page_ == Page::Trophies) {
            trophy_status_ = (trophy_status_ + 1) % 3;
            trophy_scroll_ = 0;
            audio_.play(SoundCue::Click);
            dirty = true;
        }
    }
    if (pressed & GP_X) {
        gamepad_nav_active_ = true;
        if (page_ == Page::Tools && tools_subtab_ == 1) {
            packs_view_mode_ = (packs_view_mode_ + 1) % 3;
            tools_mods_scroll_ = 0.0f;
            focused_id_.clear();
            audio_.play(SoundCue::TabSwitch);
            dirty = true;
        } else if (page_ == Page::Trophies) {
            reveal_secrets_ = !reveal_secrets_;
            audio_.play(SoundCue::Click);
            dirty = true;
        }
    }
    if (pressed & GP_START) {
        gamepad_nav_active_ = true;
        if (page_ == Page::Trilogy) {
            if (gamepad_activate()) dirty = true;
        } else if (state_.ready_to_play) {
            if (focused_id_ == "home_continue" && state_.latest_slot >= 0) launch_game(state_.latest_slot);
            else launch_game(-1);
            dirty = true;
        }
    }

    // D-pad / Left Stick with clean initial delay + repeat rate
    const int dx = ((m & GP_RIGHT) ? 1 : 0) - ((m & GP_LEFT) ? 1 : 0);
    const int dy = ((m & GP_DOWN) ? 1 : 0) - ((m & GP_UP) ? 1 : 0);
    if (dx != 0 || dy != 0) {
        if (dx != gp_repeat_dx_ || dy != gp_repeat_dy_) {
            gp_repeat_dx_ = dx;
            gp_repeat_dy_ = dy;
            gp_repeat_next_ = time_sec_ + 0.30f;
            gamepad_navigate(dx, dy);
            dirty = true;
        } else if (time_sec_ >= gp_repeat_next_) {
            gp_repeat_next_ = time_sec_ + 0.11f;
            gamepad_navigate(dx, dy);
            dirty = true;
        }
    } else {
        gp_repeat_dx_ = gp_repeat_dy_ = 0;
    }

    if (dirty && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
#endif
}

void LauncherApp::set_page(Page p) { page_ = p; }
void LauncherApp::set_trophy_tier(int tier_filter) { trophy_tier_ = std::clamp(tier_filter, -1, 6); }
void LauncherApp::set_reveal_secrets(bool show) { reveal_secrets_ = show; }
void LauncherApp::set_saves_subtab(int subtab) { saves_subtab_ = std::clamp(subtab, 0, 1); }
void LauncherApp::set_tools_subtab(int subtab) { tools_subtab_ = std::clamp(subtab, 0, 2); }

bool LauncherApp::render_to_png(const std::filesystem::path &out_png, int width, int height) {
    if (!init_factories() || !wic_factory_) return false;
    IWICBitmap *wic_bmp = nullptr;
    ID2D1RenderTarget *rt = nullptr;
    bool ok = false;

    if (SUCCEEDED(wic_factory_->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(height),
                                             GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wic_bmp)) && wic_bmp) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (SUCCEEDED(d2d_factory_->CreateWicBitmapRenderTarget(wic_bmp, props, &rt)) && rt) {
            time_sec_ = 1.8f;
            render_frame(rt, static_cast<float>(width), static_cast<float>(height));

            IWICStream *stream = nullptr;
            IWICBitmapEncoder *enc = nullptr;
            IWICBitmapFrameEncode *frame = nullptr;
            if (SUCCEEDED(wic_factory_->CreateStream(&stream)) && stream &&
                SUCCEEDED(stream->InitializeFromFilename(out_png.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(wic_factory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && enc &&
                SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && frame &&
                SUCCEEDED(frame->Initialize(nullptr)) &&
                SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
                WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&pf);
                if (SUCCEEDED(frame->WriteSource(wic_bmp, nullptr)) &&
                    SUCCEEDED(frame->Commit()) &&
                    SUCCEEDED(enc->Commit())) {
                    ok = true;
                }
            }
            if (frame) frame->Release();
            if (enc) enc->Release();
            if (stream) stream->Release();
            clear_bitmap_caches();
            rt->Release();
        }
        wic_bmp->Release();
    }
    return ok;
}

bool LauncherApp::render_stress_frames(int home_frames, int trophy_frames, int width, int height) {
    if (!init_factories() || !wic_factory_) return false;
    IWICBitmap *wic_bmp = nullptr;
    ID2D1RenderTarget *rt = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic_factory_->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(height),
                                             GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wic_bmp)) && wic_bmp) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (SUCCEEDED(d2d_factory_->CreateWicBitmapRenderTarget(wic_bmp, props, &rt)) && rt) {
            page_ = Page::Home;
            for (int i = 0; i < home_frames; ++i) {
                time_sec_ += 0.016f;
                render_frame(rt, static_cast<float>(width), static_cast<float>(height));
            }
            page_ = Page::Trophies;
            for (int i = 0; i < trophy_frames; ++i) {
                trophy_tier_ = (i % 8) - 1;
                trophy_status_ = (i / 8) % 3;
                reveal_secrets_ = ((i % 2) == 1);
                time_sec_ += 0.016f;
                render_frame(rt, static_cast<float>(width), static_cast<float>(height));
            }
            clear_bitmap_caches();
            rt->Release();
            ok = true;
        }
        wic_bmp->Release();
    }
    return ok;
}

LRESULT CALLBACK LauncherApp::wnd_proc_static(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LauncherApp *self = reinterpret_cast<LauncherApp *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        self = static_cast<LauncherApp *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->wnd_proc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT LauncherApp::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const auto to_logical = [&](int px, int py) -> D2D1_POINT_2F {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float cw = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
        const float ch = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));
        return D2D1::Point2F(static_cast<float>(px) * kWinW / cw, static_cast<float>(py) * kWinH / ch);
    };

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        if (!in_render_ && ensure_hwnd_target()) {
            in_render_ = true;
            const D2D1_SIZE_F sz = hwnd_rt_->GetSize();
            const float sx = std::max(0.1f, sz.width / kWinW);
            const float sy = std::max(0.1f, sz.height / kWinH);
            hwnd_rt_->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy));
            render_frame(hwnd_rt_, kWinW, kWinH);
            if (hwnd_rt_) hwnd_rt_->SetTransform(D2D1::Matrix3x2F::Identity());
            in_render_ = false;
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            stop_anim_timer();
        } else {
            if (hwnd_rt_) {
                hwnd_rt_->Resize(D2D1::SizeU(std::max<UINT32>(1u, LOWORD(lp)), std::max<UINT32>(1u, HIWORD(lp))));
            }
            start_anim_timer();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case 0x02E0: {  // WM_DPICHANGED
        const float dpi = static_cast<float>(LOWORD(wp) >= 96 ? LOWORD(wp) : 96);
        if (hwnd_rt_) hwnd_rt_->SetDpi(dpi, dpi);
        if (const auto *suggested = reinterpret_cast<const RECT *>(lp)) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerAnimId) {
            time_sec_ += 0.016f;
            for (Ember &em : embers_) {
                em.x += em.vx * 0.016f;
                em.y += em.vy * 0.016f;
                if (em.y < -10.0f) { em.y = kWinH + 10.0f; em.x = std::fmod(em.x + 173.0f, kWinW); }
                if (em.x < -10.0f) em.x = kWinW + 10.0f;
                if (em.x > kWinW + 10.0f) em.x = -10.0f;
            }
            poll_gamepad();
            poll_coop_lobby();
            if (page_ == Page::RingDojo) {
                const std::uint64_t now_ms = GetTickCount64();
                const float dt = (dojo_last_tick_ms_ > 0 && now_ms >= dojo_last_tick_ms_)
                                     ? std::clamp(static_cast<float>(now_ms - dojo_last_tick_ms_) / 1000.0f, 0.001f, 0.05f)
                                     : 0.016f;
                dojo_last_tick_ms_ = now_ms;
                ring_dojo_.update(dt);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CHAR: {
        if (page_ == Page::Coop && coop_edit_field_ != 0) {
            std::string *target = nullptr;
            std::size_t max_len = 48;
            if (coop_edit_field_ == 1) { target = &coop_join_input_; max_len = 40; }
            else if (coop_edit_field_ == 2) { target = &global_cfg_.coop_player_name; max_len = 24; }
            else if (coop_edit_field_ == 3) { target = &global_cfg_.coop_relay_url; max_len = 64; }
            else if (coop_edit_field_ == 4) { target = &global_cfg_.coop_campaign; max_len = 32; }
            if (target) {
                const wchar_t ch = static_cast<wchar_t>(wp);
                if (ch == L'\b') {
                    if (!target->empty()) target->pop_back();
                } else if (ch == L'\r' || ch == 27) {
                    coop_edit_field_ = 0;
                    save_global_config(state_.paths.root_dir, global_cfg_);
                } else if (ch >= 32 && ch < 127 && target->size() < max_len) {
                    target->push_back(static_cast<char>(ch));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const D2D1_POINT_2F d = to_logical(pt.x, pt.y);
        if (d.y < kTitlebarH && lightbox_image_.empty()) {
            bool over_btn = false;
            for (const auto &h : hits_)
                if (pt_in(h.rect, d.x, d.y)) { over_btn = true; break; }
            if (!over_btn) return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        const D2D1_POINT_2F d = to_logical(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (!dragging_id_.empty()) {
            std::function<void(float)> drag_fn;
            const std::string drag_id = dragging_id_;
            const D2D1_RECT_F drag_rect = dragging_rect_;
            for (const auto &h : hits_) {
                if (h.id == drag_id && h.on_drag) {
                    drag_fn = h.on_drag;
                    break;
                }
            }
            if (drag_fn) {
                const float norm = (drag_rect.right > drag_rect.left)
                    ? (d.x - drag_rect.left) / (drag_rect.right - drag_rect.left) : 0.0f;
                const float vnorm = (drag_rect.bottom > drag_rect.top)
                    ? (d.y - drag_rect.top) / (drag_rect.bottom - drag_rect.top) : 0.0f;
                drag_fn(drag_id == "tr_scroll" ? std::clamp(vnorm, 0.0f, 1.0f) : std::clamp(norm, 0.0f, 1.0f));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        std::string now_hov;
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (pt_in(it->rect, d.x, d.y)) { now_hov = it->id; break; }
        }
        if (now_hov != hovered_id_) {
            hovered_id_ = now_hov;
            if (!hovered_id_.empty()) {
                gamepad_nav_active_ = false;
                audio_.play(SoundCue::Hover);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const D2D1_POINT_2F d = to_logical(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        HitRegion matched;
        bool found = false;
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (!pt_in(it->rect, d.x, d.y)) continue;
            matched = *it;
            found = true;
            break;
        }
        if (found) {
            if (matched.is_slider && matched.on_drag) {
                dragging_id_ = matched.id;
                dragging_rect_ = matched.rect;
                SetCapture(hwnd);
                const float norm = (matched.id == "tr_scroll")
                    ? (d.y - matched.rect.top) / std::max(1.0f, matched.rect.bottom - matched.rect.top)
                    : (d.x - matched.rect.left) / std::max(1.0f, matched.rect.right - matched.rect.left);
                matched.on_drag(std::clamp(norm, 0.0f, 1.0f));
                audio_.play(SoundCue::Click);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (matched.on_click) {
                if (matched.id != "home_play" && matched.id.rfind("nav_", 0) != 0 && matched.id.rfind("trilogy_card_", 0) != 0)
                    audio_.play(SoundCue::Click);
                matched.on_click();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (!dragging_id_.empty()) {
            dragging_id_.clear();
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (page_ == Page::Settings) {
            settings_scroll_ = std::clamp(settings_scroll_ - (delta > 0 ? 48.0f : -48.0f), 0.0f, settings_max_scroll_);
        } else if (page_ == Page::Trophies) {
            trophy_scroll_ = std::max(0, trophy_scroll_ + (delta > 0 ? -1 : 1));
        } else if (page_ == Page::SavesMedia) {
            media_scroll_ = std::clamp(media_scroll_ - (delta > 0 ? 64.0f : -64.0f), 0.0f, media_max_scroll_);
        } else if (page_ == Page::Tools && tools_subtab_ == 1) {
            tools_mods_scroll_ = std::clamp(tools_mods_scroll_ - (delta > 0 ? 60.0f : -60.0f), 0.0f, tools_mods_max_scroll_);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (page_ == Page::RingDojo && (wp == VK_SPACE || wp == VK_RETURN)) {
            trigger_ring_dojo_press();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            gamepad_back();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_UP) {
            gamepad_navigate(0, -1);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_DOWN) {
            gamepad_navigate(0, +1);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_LEFT) {
            gamepad_navigate(-1, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_RIGHT) {
            gamepad_navigate(+1, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_RETURN || wp == VK_SPACE) {
            gamepad_activate();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_TAB) {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            gamepad_switch_page(shift ? -1 : +1);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kWmGameExited:
        game_running_ = false;
        if (game_launch_time_sec_ > 0.0) {
            const double now_sec = static_cast<double>(GetTickCount64()) / 1000.0;
            if (now_sec > game_launch_time_sec_) {
                record_playtime_session(state_.paths, now_sec - game_launch_time_sec_, game_launched_coop_);
            }
            game_launched_coop_ = false;
            game_launch_time_sec_ = 0.0;
        }
        clear_bitmap_caches();
        refresh_all(state_);
        trilogy_summaries_ = load_trilogy_summaries(state_.paths.root_dir);
        audio_.set_enabled(global_cfg_.chimes);
        audio_.set_volume(state_.settings.chime_volume);
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        if (!state_.trophies.empty()) {
            show_toast("Welcome back!  " + std::to_string(state_.trophies_unlocked) + " / " +
                       std::to_string(state_.trophies.size()) + " Trophies unlocked");
        } else {
            show_toast(std::string("Welcome back to ") + game_profile(active_game_).full_title);
        }
        start_anim_timer();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        stop_anim_timer();
        release_device_resources();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int LauncherApp::run(HINSTANCE hinst, int show_cmd) {
    if (!init_factories()) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc_static;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hinst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"ShadowHeartsNocturneLauncherWnd";
    RegisterClassExW(&wc);

    // Create window, then size & center accurately for the active monitor's DPI and work area
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, wc.lpszClassName, L"Shadow Hearts: Nocturne",
        WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(kWinW), static_cast<int>(kWinH),
        nullptr, nullptr, hinst, this);
    if (!hwnd) return 1;

    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    }
    const float dpi_scale = static_cast<float>(window_dpi(hwnd)) / 96.0f;
    const float work_w = static_cast<float>(std::max<LONG>(640, work.right - work.left));
    const float work_h = static_cast<float>(std::max<LONG>(480, work.bottom - work.top));
    const float fit_scale = std::min({dpi_scale, (work_w * 0.94f) / kWinW, (work_h * 0.94f) / kWinH});
    const int win_w = static_cast<int>(kWinW * fit_scale + 0.5f);
    const int win_h = static_cast<int>(kWinH * fit_scale + 0.5f);
    const int pos_x = work.left + std::max(0, static_cast<int>(work_w - static_cast<float>(win_w)) / 2);
    const int pos_y = work.top + std::max(0, static_cast<int>(work_h - static_cast<float>(win_h)) / 2);
    SetWindowPos(hwnd, nullptr, pos_x, pos_y, win_w, win_h, SWP_NOZORDER | SWP_NOACTIVATE);

    // Enable Windows 11 native rounded corners + subtle shadow
    const DWORD corner_pref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner_pref, sizeof(corner_pref));
    const MARGINS shadow{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &shadow);

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);
    start_anim_timer();
    start_update_check();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace launcher
