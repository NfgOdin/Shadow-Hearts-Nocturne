#include "launcher_app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <dwmapi.h>
#include <shellapi.h>
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
    refresh_all(state_);
    audio_.set_enabled(state_.settings.launcher_chimes);
    audio_.set_volume(state_.settings.chime_volume);
    init_embers();
}

LauncherApp::~LauncherApp() {
    release_device_resources();
    const DWRITE_TRIMMING no_trim{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    for (auto &kv : trim_signs_) {
        if (kv.first) kv.first->SetTrimming(&no_trim, nullptr);
        if (kv.second) kv.second->Release();
    }
    trim_signs_.clear();
    if (fmt_hero_) fmt_hero_->Release();
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

LauncherApp::Palette LauncherApp::current_palette() const {
    Palette p;
    switch (state_.settings.toast_theme) {
    case 2:  // Judgment Ring Crimson & Brass
        p.bg_top = col_rgba(18, 11, 12);
        p.bg_bot = col_rgba(38, 12, 18);
        p.sidebar_bg = col_rgba(14, 9, 10, 0.92f);
        p.card_bg = col_rgba(28, 18, 20, 0.86f);
        p.card_hover = col_rgba(44, 26, 30, 0.92f);
        p.border = col_rgba(150, 108, 58, 0.55f);
        p.border_hi = col_rgba(246, 202, 92, 0.95f);
        p.accent = col_rgba(248, 206, 98);
        p.accent_crimson = col_rgba(196, 38, 56);
        break;
    case 3:  // 1915 Occult Grimoire
        p.bg_top = col_rgba(11, 16, 28);
        p.bg_bot = col_rgba(20, 28, 46);
        p.sidebar_bg = col_rgba(9, 13, 24, 0.92f);
        p.card_bg = col_rgba(18, 26, 42, 0.86f);
        p.card_hover = col_rgba(28, 40, 62, 0.92f);
        p.border = col_rgba(74, 148, 196, 0.48f);
        p.border_hi = col_rgba(104, 212, 248, 0.95f);
        p.accent = col_rgba(226, 198, 134);
        p.accent_crimson = col_rgba(56, 168, 214);
        break;
    case 1:  // Soul Graveyard Eclipse
    case 0:  // Tier-Tinted / Nocturne Default
    default:
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
    p.text = col_rgba(244, 242, 238);
    p.text_dim = col_rgba(164, 170, 192);
    p.good = col_rgba(104, 230, 178);
    p.warn = col_rgba(248, 178, 76);
    return p;
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
    hits_.push_back({id, rect, std::move(on_click), nullptr, false});
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
    hits_.push_back({id, row, [on_change, value]() { on_change(!value); }, nullptr, false});
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
    hits_.push_back({id, hit, nullptr, std::move(on_change), true});
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
        hits_.push_back({pid, pr, [on_change, i]() { on_change(i); }, nullptr, false});
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

    // Animated Judgment Ring centerpiece (prominent on Home, subtle watermark on subpages)
    const float ring_cx = (page_ == Page::Home) ? (kSidebarW + (w - kSidebarW) * 0.72f) : (w - 150.0f);
    const float ring_cy = (page_ == Page::Home) ? (kTitlebarH + (h - kTitlebarH) * 0.42f) : (h - 130.0f);
    const float ring_r = (page_ == Page::Home) ? 215.0f : 190.0f;
    render_judgment_ring(rt, ring_cx, ring_cy, ring_r, (page_ == Page::Home) ? 0.95f : 0.20f, pal);

    // Subtle drifting soul embers
    ID2D1SolidColorBrush *ebr = nullptr;
    for (const Ember &em : embers_) {
        const float pulse = 0.65f + 0.35f * std::sin(time_sec_ * 2.1f + em.phase);
        const D2D1_COLOR_F ec = lerp_col(pal.accent, pal.accent_crimson, 0.35f + 0.35f * std::sin(em.phase));
        if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(ec, em.alpha * pulse * (page_ == Page::Home ? 0.9f : 0.45f)), &ebr)) && ebr) {
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
    draw_text(rt, "•  Native PC Launcher  •  SLUS-21041 / 21044", fmt_small_, D2D1::RectF(308.0f, 1.0f, 640.0f, kTitlebarH),
              pal.text_dim, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Top-right quick controls: [Theme] [Chimes On/Off] [—] [×]
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
    win_btn("tb_chime", 96.0f, state_.settings.launcher_chimes ? "Chimes: ON" : "Chimes: OFF", false, [this]() {
        state_.settings.launcher_chimes = !state_.settings.launcher_chimes;
        audio_.set_enabled(state_.settings.launcher_chimes);
        persist_settings(state_.settings.launcher_chimes ? "Launcher chimes enabled" : "Launcher chimes muted");
    });
    win_btn("tb_theme", 168.0f, std::string("Theme: ") + achievements::catalog_toast_theme_name(state_.settings.toast_theme), false, [this]() {
        state_.settings.toast_theme = (state_.settings.toast_theme + 1) % static_cast<int>(achievements::ToastTheme::Count);
        persist_settings(std::string("Theme: ") + achievements::catalog_toast_theme_name(state_.settings.toast_theme));
    });
}

void LauncherApp::render_sidebar(ID2D1RenderTarget *rt, float h, const Palette &pal) {
    ID2D1SolidColorBrush *br = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(pal.sidebar_bg, &br)) && br) {
        rt->FillRectangle(D2D1::RectF(0, kTitlebarH, kSidebarW, h), br);
        br->Release();
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(with_a(pal.border, 0.45f), &br)) && br) {
        rt->DrawLine(D2D1::Point2F(kSidebarW, kTitlebarH), D2D1::Point2F(kSidebarW, h), br, 1.0f);
        br->Release();
    }

    struct NavItem { Page p; const char *label; const char *sub; };
    const NavItem nav[5] = {
        {Page::Home, "Home", "Play & Command Hub"},
        {Page::Settings, "Settings", "Graphics, Lighting, Audio"},
        {Page::Trophies, "Trophy Gallery", "110 Achievements"},
        {Page::SavesMedia, "Saves & Media", "Slots 0-9 & Screenshots"},
        {Page::Tools, "Tools", "Discs, Backups, Cache"},
    };

    float y = kTitlebarH + 22.0f;
    for (int i = 0; i < 5; ++i) {
        const bool active = (page_ == nav[i].p);
        const std::string id = "nav_" + std::to_string(i);
        const bool hov = (hovered_id_ == id);
        const D2D1_RECT_F r{14.0f, y, kSidebarW - 14.0f, y + 54.0f};
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 9.0f, 9.0f);
        if (active) {
            fill_hgrad_rounded(rt, rr, with_a(pal.accent_crimson, 0.45f), with_a(pal.accent, 0.20f));
            if (SUCCEEDED(rt->CreateSolidColorBrush(pal.border_hi, &br)) && br) {
                rt->DrawRoundedRectangle(rr, br, 1.3f);
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 2, r.top + 10, r.left + 6, r.bottom - 10), 2, 2), br);
                br->Release();
            }
        } else if (hov) {
            if (SUCCEEDED(rt->CreateSolidColorBrush(pal.card_hover, &br)) && br) {
                rt->FillRoundedRectangle(rr, br);
                br->Release();
            }
        }
        draw_text(rt, nav[i].label, fmt_card_title_, D2D1::RectF(r.left + 16.0f, r.top + 8.0f, r.right - 10.0f, r.top + 28.0f),
                  active ? pal.accent : pal.text);
        draw_text(rt, nav[i].sub, fmt_small_, D2D1::RectF(r.left + 16.0f, r.top + 28.0f, r.right - 10.0f, r.bottom - 6.0f),
                  active ? col_rgba(232, 224, 212) : pal.text_dim);
        const Page target = nav[i].p;
        hits_.push_back({id, r, [this, target]() {
            if (page_ != target) {
                audio_.play(SoundCue::TabSwitch);
                page_ = target;
                armed_delete_slot_ = -1;
            }
        }, nullptr, false});
        y += 62.0f;
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
    draw_text(rt, "COVENANT PROGRESS", fmt_pill_, D2D1::RectF(card.left + 12, card.top + 10, card.right - 12, card.top + 26), pal.accent);
    draw_text(rt, std::to_string(state_.trophies_unlocked) + " / " + std::to_string(tot) + " Trophies (" + std::to_string(pct) + "%)",
              fmt_body_, D2D1::RectF(card.left + 12, card.top + 28, card.right - 12, card.top + 48), pal.text);
    const D2D1_RECT_F pb{card.left + 12, card.top + 54, card.right - 12, card.top + 62};
    if (SUCCEEDED(rt->CreateSolidColorBrush(col_rgba(24, 24, 40), &br)) && br) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(pb, 4, 4), br);
        br->Release();
    }
    if (tot > 0 && state_.trophies_unlocked > 0) {
        const float fw = std::max(8.0f, (pb.right - pb.left) * static_cast<float>(state_.trophies_unlocked) / static_cast<float>(tot));
        fill_hgrad_rounded(rt, D2D1::RoundedRect(D2D1::RectF(pb.left, pb.top, pb.left + fw, pb.bottom), 4, 4), pal.accent_crimson, pal.accent);
    }
    draw_text(rt, state_.ready_to_play ? "Ready: Shadow Hearts Covenant" : "Setup required (see Tools)",
              fmt_small_, D2D1::RectF(card.left + 12, card.top + 74, card.right - 12, card.top + 94),
              state_.ready_to_play ? pal.good : pal.warn);
    draw_text(rt, std::to_string(state_.occupied_slots) + " Save Slots  •  " + std::to_string(state_.screenshots.size()) + " Shots",
              fmt_small_, D2D1::RectF(card.left + 12, card.top + 92, card.right - 12, card.bottom - 8), pal.text_dim);
}

void LauncherApp::render_home(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
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
    draw_text(rt, "NATIVE WINDOWS PC RECOMPILATION", fmt_pill_, badge, col_rgba(255, 244, 212),
              DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    y += 38.0f;

    // Hero Title & Subtitle
    draw_text(rt, "SHADOW HEARTS:", fmt_hero_, D2D1::RectF(x, y, x + 640.0f, y + 42.0f), pal.text);
    y += 40.0f;
    draw_text(rt, "COVENANT", fmt_hero_, D2D1::RectF(x, y, x + 640.0f, y + 44.0f), pal.accent);
    y += 52.0f;
    draw_text(rt, "1915 Europe & Japan  •  60+ FPS Native Port  •  Seamless 2-Disc Auto-Swap  •  110 Trophies",
              fmt_body_, D2D1::RectF(x, y, x + 650.0f, y + 26.0f), pal.text_dim);
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
    px = pill(px, !state_.paths.disc1_path.empty() ? "Disc 1 Verified" : "Disc 1 Missing",
              !state_.paths.disc1_path.empty() ? pal.good : pal.warn);
    px = pill(px, !state_.paths.disc2_path.empty() ? "Disc 2 Verified" : "Disc 2 Optional",
              !state_.paths.disc2_path.empty() ? pal.good : pal.text_dim);
    const std::string api_lbl = (state_.settings.gpu_api == "vulkan" ? "Vulkan" : "Direct3D 12");
    px = pill(px, api_lbl + "  •  " + std::to_string(state_.settings.scale) + "x  •  " + state_.settings.fps_cap + " FPS", pal.accent);
    if (state_.settings.fast_disc) pill(px, "Fast SSD Load", pal.good);
    y += 48.0f;

    // Primary PLAY Button + Launch Mode Toggle + Quick Open Game Folder
    const D2D1_RECT_F play_r{x, y, x + 266.0f, y + 58.0f};
    if (state_.ready_to_play) {
        fill_radial_glow(rt, (play_r.left + play_r.right) * 0.5f, (play_r.top + play_r.bottom) * 0.5f,
                         175.0f, 52.0f, with_a(pal.accent_crimson, 0.42f), with_a(pal.accent_crimson, 0.0f));
    }
    draw_button(rt, "home_play", play_r,
                game_running_ ? "RUNNING..." : (state_.ready_to_play ? "PLAY COVENANT" : "BUILD REQUIRED"),
                state_.ready_to_play ? pal.accent_crimson : col_rgba(52, 46, 56),
                pal.border_hi, col_rgba(255, 250, 236), true, [this]() {
                    if (state_.ready_to_play) launch_game();
                    else {
                        audio_.play(SoundCue::TabSwitch);
                        page_ = Page::Tools;
                    }
                });

    const D2D1_RECT_F mode_r{play_r.right + 14.0f, y + 6.0f, play_r.right + 172.0f, y + 52.0f};
    draw_button(rt, "home_mode", mode_r,
                state_.settings.launcher_fullscreen ? "Mode: Fullscreen" : "Mode: Windowed",
                pal.card_bg, pal.border, pal.text, false, [this]() {
                    state_.settings.launcher_fullscreen = !state_.settings.launcher_fullscreen;
                    persist_settings(state_.settings.launcher_fullscreen ? "Launch mode: Fullscreen" : "Launch mode: Windowed");
                });

    const D2D1_RECT_F dir_r{mode_r.right + 12.0f, y + 6.0f, mode_r.right + 156.0f, y + 52.0f};
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
    draw_card_frame("home_card_trophies", x, "TROPHY GALLERY",
                    std::to_string(state_.trophies_unlocked) + " / " + std::to_string(tot) + " Unlocked",
                    "Best Ring Streak: " + std::to_string(state_.best_ring_streak) + "  •  Perfects: " + std::to_string(state_.best_perfect_streak),
                    "Mark " + std::to_string(state_.tier_unlocked[0]) + "/" + std::to_string(state_.tier_total[0]) +
                    "  •  Curiosities " + std::to_string(state_.tier_unlocked[6]) + "/" + std::to_string(state_.tier_total[6]),
                    [this]() { audio_.play(SoundCue::TabSwitch); page_ = Page::Trophies; });

    std::string save_head = "No Save States Yet";
    std::string save_l1 = "Press F5 in-game to save Slots 0-9";
    std::string save_l2 = "Memory card: memcard.ps2 ready";
    if (state_.latest_slot >= 0) {
        const auto &sl = state_.slots[static_cast<std::size_t>(state_.latest_slot)];
        save_head = "Slot " + std::to_string(sl.slot) + (sl.area.empty() ? "" : (" — " + sl.area));
        save_l1 = "Disc " + std::to_string(sl.disc) + (sl.max_level > 0 ? ("  •  Lv " + std::to_string(sl.max_level)) : "") +
                  "  •  " + format_playtime(sl.playtime_sec);
        save_l2 = sl.timestamp + "  (" + format_bytes(sl.bytes) + ")";
    }
    draw_card_frame("home_card_saves", x + cw + gap, "SAVES & MOMENTS", save_head, save_l1, save_l2,
                    [this]() { audio_.play(SoundCue::TabSwitch); page_ = Page::SavesMedia; });

    static const char *const kAspectLbl[3] = {"4:3 Classic", "16:9 Wide", "Stretch"};
    const std::string light_summary = (state_.settings.ssao || state_.settings.bloom || state_.settings.ssgi || state_.settings.filmic)
        ? ("SSAO " + std::to_string(state_.settings.ssao) + "% • Bloom " + std::to_string(state_.settings.bloom) + "%")
        : "Authentic PS2 Lighting (Post-FX Off)";
    draw_card_frame("home_card_gfx", x + (cw + gap) * 2.0f, "VISUAL PROFILE",
                    std::to_string(state_.settings.scale) + "x Internal  •  " + kAspectLbl[std::clamp(state_.settings.aspect, 0, 2)],
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
    draw_text(rt, "Changes sync immediately with games/sh2-covenant/keys.ini (comments & keybinds preserved).",
              fmt_small_, D2D1::RectF(x, area.top + 42, x + 640, area.top + 60), pal.text_dim);

    draw_button(rt, "set_reset", D2D1::RectF(area.right - pad - 150.0f, area.top + 18.0f, area.right - pad, area.top + 50.0f),
                "Reset Defaults", pal.card_bg, pal.border, pal.text_dim, false, [this]() {
                    state_.settings = Settings{};
                    audio_.set_enabled(state_.settings.launcher_chimes);
                    audio_.set_volume(state_.settings.chime_volume);
                    persist_settings("Settings reset to defaults");
                });

    const D2D1_RECT_F view{x, area.top + header_h + 14.0f, x + w - 14.0f, area.bottom - 18.0f};
    const float view_h = view.bottom - view.top;
    const float content_h = 680.0f;
    settings_max_scroll_ = std::max(0.0f, content_h - view_h);
    settings_scroll_ = std::clamp(settings_scroll_, 0.0f, settings_max_scroll_);

    rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const float col_gap = 18.0f;
    const float cw = (view.right - view.left - col_gap) * 0.5f;
    const float lx = view.left;
    const float rx = view.left + cw + col_gap;
    const float base_y = view.top - settings_scroll_;

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

    // Left Top: Display & Performance (360px)
    panel(lx, base_y, cw, 370.0f, "DISPLAY & GRAPHICS");
    float cy = base_y + 34.0f;
    draw_slider_row(rt, "set_scale", lx + 6, cy, cw - 12, "Internal Resolution Scale",
                    std::to_string(state_.settings.scale) + "x", static_cast<float>(state_.settings.scale - 1) / 7.0f, pal,
                    [this](float t) {
                        const int v = std::clamp(static_cast<int>(t * 7.0f + 0.5f) + 1, 1, 8);
                        if (v != state_.settings.scale) { state_.settings.scale = v; persist_settings(); }
                    });
    cy += 50.0f;
    draw_segmented_row(rt, "set_aspect", lx + 6, cy, cw - 12, "Aspect Ratio", {"4:3 Classic", "16:9 Wide", "Stretch"},
                       state_.settings.aspect, pal, [this](int idx) {
                           state_.settings.aspect = idx;
                           persist_settings("Aspect ratio updated");
                       });
    cy += 56.0f;
    static const std::vector<std::string> kFpsOpts = {"30", "60", "120", "144", "display", "unlimited"};
    static const std::vector<std::string> kFpsLabels = {"30", "60", "120", "144", "Display", "Max"};
    int fps_i = 1;
    for (int i = 0; i < static_cast<int>(kFpsOpts.size()); ++i)
        if (state_.settings.fps_cap == kFpsOpts[static_cast<std::size_t>(i)]) fps_i = i;
    draw_segmented_row(rt, "set_fps", lx + 6, cy, cw - 12, "Frame Rate Cap", kFpsLabels, fps_i, pal, [this](int idx) {
        state_.settings.fps_cap = kFpsOpts[static_cast<std::size_t>(idx)];
        persist_settings("FPS cap set to " + state_.settings.fps_cap);
    });
    cy += 56.0f;
    draw_segmented_row(rt, "set_api", lx + 6, cy, cw - 12, "Graphics API", {"Direct3D 12", "Vulkan"},
                       state_.settings.gpu_api == "vulkan" ? 1 : 0, pal, [this](int idx) {
                           state_.settings.gpu_api = (idx == 1) ? "vulkan" : "dx12";
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

    // Left Bottom: Launcher & Disc Controls (280px)
    const float lb_y = base_y + 386.0f;
    panel(lx, lb_y, cw, 276.0f, "LAUNCHER & DISC BEHAVIOR");
    cy = lb_y + 34.0f;
    draw_toggle_row(rt, "set_hide", lx + 6, cy, cw - 12, "Hide Launcher While Playing", "0% CPU/GPU during gameplay, auto-restores on exit",
                    state_.settings.launcher_hide_on_play, pal, [this](bool v) {
                        state_.settings.launcher_hide_on_play = v;
                        persist_settings(v ? "Launcher will hide & auto-restore" : "Launcher will close on Play");
                    });
    cy += 46.0f;
    draw_toggle_row(rt, "set_fast", lx + 6, cy, cw - 12, "Fast SSD Disc Loading", "~16x faster area loads (OFF for authentic PS2 DVD speed)",
                    state_.settings.fast_disc, pal, [this](bool v) {
                        state_.settings.fast_disc = v;
                        persist_settings("Fast disc loading updated");
                    });
    cy += 46.0f;
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

    // Right Top: Depth-Aware Lighting & Post-Processing (370px)
    panel(rx, base_y, cw, 370.0f, "DEPTH-AWARE LIGHTING & IMAGE");
    cy = base_y + 34.0f;
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
    draw_toggle_row(rt, "set_filmic", rx + 6, cy, cw - 12, "Filmic Tone Mapping", "Rich cinematic contrast curve in blit.wgsl",
                    state_.settings.filmic, pal, [this](bool v) { state_.settings.filmic = v; persist_settings("Filmic tone map updated"); });
    cy += 48.0f;
    int s2d_i = state_.settings.sharp_2d == "soft" ? 0 : (state_.settings.sharp_2d == "sharp" ? 1 : 2);
    draw_segmented_row(rt, "set_s2d", rx + 6, cy, cw - 12, "2D Text & UI Filtering", {"Soft", "Sharp", "Sharpest"}, s2d_i, pal, [this](int idx) {
        static const char *const kS2D[3] = {"soft", "sharp", "sharpest"};
        state_.settings.sharp_2d = kS2D[idx];
        persist_settings("2D filtering updated");
    });

    // Right Bottom: Audio & Chimes (276px)
    panel(rx, lb_y, cw, 276.0f, "AUDIO & HARMONIC CHIMES");
    cy = lb_y + 34.0f;
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

    // 8-Tab Tier Strip
    static const char *const kTabShort[8] = {"All", "Mark", "Sigil", "Crest", "Sovereign", "Covenant", "Follies", "Curiosities"};
    const float tgap = 6.0f;
    const float tw = (w - tgap * 7.0f) / 8.0f;
    for (int t = 0; t < 8; ++t) {
        const float tx = x + static_cast<float>(t) * (tw + tgap);
        const D2D1_RECT_F tr{tx, y, tx + tw, y + 32.0f};
        const bool active = (trophy_tier_ == t - 1);
        const std::string tid = "tr_tier_" + std::to_string(t);
        std::uint32_t gl_u, gr_u, bdr_u, acc_u;
        if (t == 0) {
            gl_u = achievements::catalog_rgba(56, 40, 24);
            gr_u = achievements::catalog_rgba(136, 94, 40);
            bdr_u = achievements::catalog_rgba(244, 208, 120);
            acc_u = achievements::catalog_rgba(255, 230, 150);
        } else {
            achievements::catalog_theme_colors(static_cast<achievements::Tier>(t - 1), state_.settings.toast_theme, gl_u, gr_u, bdr_u, acc_u);
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
        const int cu = (t == 0) ? state_.trophies_unlocked : state_.tier_unlocked[t - 1];
        const int ct = (t == 0) ? tot : state_.tier_total[t - 1];
        draw_text(rt, std::string(kTabShort[t]) + " (" + std::to_string(cu) + "/" + std::to_string(ct) + ")",
                  fmt_pill_, tr, active ? col_rgba(255, 252, 242) : lerp_col(from_u32(acc_u), pal.text_dim, 0.6f),
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        hits_.push_back({tid, tr, [this, t]() {
            trophy_tier_ = t - 1;
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
    draw_button(rt, "sm_open", D2D1::RectF(x + w - 150, y0, x + w, y0 + 32),
                "Open Folder", pal.card_bg, pal.border, pal.text_dim, false, [this]() {
                    open_in_explorer(saves_subtab_ == 0 ? state_.paths.states_dir : state_.paths.screenshots_dir);
                });

    const D2D1_RECT_F view{x, y0 + 46.0f, x + w, area.bottom - 18.0f};
    const float view_h = view.bottom - view.top;
    rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (saves_subtab_ == 0) {
        const float gap = 12.0f;
        const float cw = (w - gap) * 0.5f;
        const float ch = 112.0f;
        const float total_h = 5.0f * (ch + gap);
        media_max_scroll_ = std::max(0.0f, total_h - view_h);
        media_scroll_ = std::clamp(media_scroll_, 0.0f, media_max_scroll_);

        for (int s = 0; s < 10; ++s) {
            const auto &sl = state_.slots[static_cast<std::size_t>(s)];
            const int col = s % 2, row = s / 2;
            const float cx = view.left + static_cast<float>(col) * (cw + gap);
            const float cy = view.top - media_scroll_ + static_cast<float>(row) * (ch + gap);
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
                hits_.push_back({sid, cr, [this, sh]() {
                    lightbox_image_ = sh.path;
                    lightbox_title_ = sh.display_name + "  (" + sh.date_text + ")";
                }, nullptr, false});
            }
        }
    }

    rt->PopAxisAlignedClip();
}

void LauncherApp::render_tools(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal) {
    const float pad = 26.0f;
    const float x = area.left + pad;
    const float w = area.right - area.left - pad * 2.0f;
    float y = area.top + 14.0f;

    draw_text(rt, "Tools & System Health", fmt_h1_, D2D1::RectF(x, y, x + 420, y + 32), pal.accent);
    draw_text(rt, "Verify your unpacked discs, back up your PS2 memory card, or manage local caches.",
              fmt_small_, D2D1::RectF(x, y + 30, x + 640, y + 48), pal.text_dim);
    draw_button(rt, "tool_refresh", D2D1::RectF(x + w - 134, y + 4, x + w, y + 36),
                "Refresh Status", pal.card_bg, pal.border, pal.text, false, [this]() {
                    clear_bitmap_caches();
                    refresh_all(state_);
                    show_toast("Status refreshed");
                });
    y += 62.0f;

    const float gap = 18.0f;
    const float left_w = (w - gap) * 0.56f;
    const float right_x = x + left_w + gap;
    const float right_w = w - left_w - gap;
    const float panel_h = area.bottom - y - 20.0f;

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

    float hy = hc.top + 42.0f;
    for (const auto &item : state_.health) {
        if (SUCCEEDED(rt->CreateSolidColorBrush(item.ok ? pal.good : pal.warn, &b)) && b) {
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hc.left + 24, hy + 11), 5.0f, 5.0f), b);
            b->Release();
        }
        draw_text(rt, item.label, fmt_body_, D2D1::RectF(hc.left + 38, hy, hc.right - 16, hy + 20), pal.text);
        draw_text(rt, item.detail, fmt_small_, D2D1::RectF(hc.left + 38, hy + 20, hc.right - 16, hy + 38), pal.text_dim);
        hy += 56.0f;
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

    float ay = ac.top + 42.0f;
    const float bw = right_w - 32.0f;
    draw_button(rt, "tool_backup", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 42),
                "Backup PS2 Memory Card (memcard.ps2)", with_a(pal.accent_crimson, 0.55f), pal.border_hi, pal.text, false, [this]() {
                    std::string msg;
                    backup_memcard(state_.paths, nullptr, &msg);
                    show_toast(msg);
                });
    ay += 54.0f;
    draw_button(rt, "tool_cache", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 42),
                "Clear Live Recompiler Caches (ee-live / vu1-live)", pal.card_hover, pal.border, pal.text, false, [this]() {
                    const int n = clear_live_caches(state_.paths);
                    show_toast("Cleared " + std::to_string(n) + " cached file(s)");
                });
    ay += 54.0f;
    draw_button(rt, "tool_rebuild", D2D1::RectF(ac.left + 16, ay, ac.left + 16 + bw, ay + 42),
                "Run Builder / Verify Discs (Build .cmd)", pal.card_hover, pal.border, pal.text, false, [this]() {
                    ShellExecuteW(hwnd_, L"open", state_.paths.build_cmd.c_str(), nullptr, state_.paths.root_dir.c_str(), SW_SHOWNORMAL);
                    show_toast("Opened Build Shadow Hearts Covenant.cmd");
                });
    ay += 68.0f;

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
    const Palette pal = current_palette();
    rt->BeginDraw();
    render_background(rt, w, h, pal);
    render_sidebar(rt, h, pal);
    render_titlebar(rt, w, pal);

    const D2D1_RECT_F content{kSidebarW, kTitlebarH, w, h};
    switch (page_) {
    case Page::Home: render_home(rt, content, pal); break;
    case Page::Settings: render_settings(rt, content, pal); break;
    case Page::Trophies: render_trophies(rt, content, pal); break;
    case Page::SavesMedia: render_saves_media(rt, content, pal); break;
    case Page::Tools: render_tools(rt, content, pal); break;
    default: break;
    }

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

void LauncherApp::launch_game() {
    if (game_running_ || !state_.ready_to_play) return;
    audio_.play(SoundCue::LaunchPlay);
    persist_settings();

    SetEnvironmentVariableW(L"PS2_WINDOW_TITLE", L"Shadow Hearts: Covenant");
    std::wstring cmd = build_launch_command(state_.paths, state_.settings.launcher_fullscreen);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        state_.paths.root_dir.c_str(), &si, &pi)) {
        show_toast("Failed to launch Shadow Hearts Covenant.exe");
        return;
    }
    CloseHandle(pi.hThread);

    if (!state_.settings.launcher_hide_on_play) {
        CloseHandle(pi.hProcess);
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }

    game_running_ = true;
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

void LauncherApp::set_page(Page p) { page_ = p; }
void LauncherApp::set_trophy_tier(int tier_filter) { trophy_tier_ = std::clamp(tier_filter, -1, 6); }
void LauncherApp::set_reveal_secrets(bool show) { reveal_secrets_ = show; }
void LauncherApp::set_saves_subtab(int subtab) { saves_subtab_ = std::clamp(subtab, 0, 1); }

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
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
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
            if (!hovered_id_.empty()) audio_.play(SoundCue::Hover);
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
                if (matched.id != "home_play" && matched.id.rfind("nav_", 0) != 0) audio_.play(SoundCue::Click);
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
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !lightbox_image_.empty()) {
            lightbox_image_.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kWmGameExited:
        game_running_ = false;
        clear_bitmap_caches();
        refresh_all(state_);
        audio_.set_enabled(state_.settings.launcher_chimes);
        audio_.set_volume(state_.settings.chime_volume);
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        show_toast("Welcome back!  " + std::to_string(state_.trophies_unlocked) + " / " +
                   std::to_string(state_.trophies.size()) + " Trophies unlocked");
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

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace launcher
