#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace ui {

enum class ThemeId : int {
    Fluent = 0,     // Direction A: Windows 11 Fluent Craft
    Editorial = 1,  // Direction B: Warm Editorial Ink & Paper
    Monolith = 2    // Direction C: Refined Heritage Monolith
};

struct ThemeTokens {
    ThemeId id;
    const char* key;           // "fluent", "editorial", "monolith"
    const wchar_t* display_en; // L"Fluent Craft", L"Warm Editorial", L"Refined Monolith"

    // Surfaces
    UINT32 surface_canvas;        // A: 0x1C1D21, B: 0x161615, C: 0x0B0F17
    float  surface_canvas_alpha;  // A: 0.96f,    B: 0.98f,    C: 0.97f
    UINT32 surface_elevated;      // A: 0x25272C, B: 0x222220, C: 0x131924
    UINT32 surface_card;          // A: 0x202227, B: 0x1C1C1A, C: 0x101620
    UINT32 surface_hover;         // A: 0x2F3238, B: 0x2D2D2A, C: 0x1C2433
    UINT32 surface_pressed;       // A: 0x18191C, B: 0x121211, C: 0x080B11

    // Borders & Dividers
    UINT32 border_outer;          // A: 0x0A0B0D, B: 0x080807, C: 0x05070B
    float  border_outer_alpha;    // A: 0.65f,    B: 0.75f,    C: 0.75f
    UINT32 border_inner;          // A: 0xFFFFFF, B: 0xFFFFFF, C: 0xFFFFFF
    float  border_inner_alpha;    // A: 0.08f,    B: 0.06f,    C: 0.07f
    UINT32 border_subtle;         // A: 0xFFFFFF, B: 0x33332F, C: 0x232D3F
    float  border_subtle_alpha;   // A: 0.08f,    B: 1.00f,    C: 1.00f
    UINT32 border_hover;          // A: 0x60CDFF, B: 0xFF5500, C: 0xC5A869
    float  border_hover_alpha;    // A: 0.75f,    B: 0.85f,    C: 0.70f

    // Text Hierarchy
    UINT32 text_primary;          // A: 0xF3F4F6, B: 0xF4F1EA, C: 0xECE7DC
    UINT32 text_secondary;        // A: 0xA1A6B0, B: 0xA8A49C, C: 0x9A968E
    UINT32 text_muted;            // A: 0x6E737D, B: 0x6E6B65, C: 0x686A70
    UINT32 text_dim;              // A: 0x52565E, B: 0x52504B, C: 0x4B505B

    // Accents & Semantic States
    UINT32 accent_primary;        // A: 0x60CDFF, B: 0xFF5500, C: 0xC5A869
    UINT32 accent_subtle;         // A: 0x3898EC, B: 0xCC4400, C: 0x9E8550
    UINT32 state_ready;           // A: 0x6CCB5F, B: 0x4E9A51, C: 0x4E9F76
    UINT32 state_busy;            // A: 0xFCE100, B: 0xE09F3E, C: 0xD49B4B
    UINT32 state_off;             // A: 0x6E737D, B: 0x6E6B65, C: 0x5F6672
    UINT32 state_danger;          // A: 0xFF99A4, B: 0xD64545, C: 0xC85A53
    UINT32 danger_fill;           // A: 0x3B1A1E, B: 0x361919, C: 0x33181A
    UINT32 danger_hover;          // A: 0x522329, B: 0x4A2222, C: 0x472023
    UINT32 danger_stroke;         // A: 0xC42B1C, B: 0xB83B3B, C: 0xA34842
    UINT32 danger_ink;            // A: 0xFFCCD2, B: 0xF5C6C6, C: 0xEED0CD

    // Geometry & Typography
    float badge_radius;           // A: 6.0f,  B: 5.0f, C: 7.0f
    float card_radius;            // A: 10.0f, B: 8.0f, C: 10.0f
    float control_radius;         // A: 5.0f,  B: 4.0f, C: 5.0f
    const wchar_t* font_ui;       // L"Segoe UI"
    const wchar_t* font_mono;     // L"Consolas"
    bool use_mono_tags;           // A: false, B: true, C: false
};

inline D2D1_COLOR_F ColorFromHex(UINT32 hex, float alpha = 1.0f) noexcept {
    return D2D1::ColorF(hex, alpha);
}

inline ThemeId ParseThemeId(std::string_view key) noexcept {
    while (!key.empty() && (key.front() == ' ' || key.front() == '\t' || key.front() == '\r' || key.front() == '\n')) {
        key.remove_prefix(1);
    }
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r' || key.back() == '\n')) {
        key.remove_suffix(1);
    }
    auto iequals = [](std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
            const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
            if (ca != cb) return false;
        }
        return true;
    };
    if (iequals(key, "editorial") || iequals(key, "b")) {
        return ThemeId::Editorial;
    }
    if (iequals(key, "monolith") || iequals(key, "c")) {
        return ThemeId::Monolith;
    }
    return ThemeId::Fluent;
}

inline const char* ThemeIdToKey(ThemeId id) noexcept {
    switch (id) {
        case ThemeId::Editorial: return "editorial";
        case ThemeId::Monolith:  return "monolith";
        case ThemeId::Fluent:
        default:                 return "fluent";
    }
}

inline const ThemeTokens& GetThemeTokens(ThemeId id) noexcept {
    static constexpr ThemeTokens kFluent = {
        ThemeId::Fluent,
        "fluent",
        L"Fluent Craft",
        // Surfaces
        0x1C1D21, 0.96f,
        0x25272C,
        0x202227,
        0x2F3238,
        0x18191C,
        // Borders & Dividers
        0x0A0B0D, 0.65f,
        0xFFFFFF, 0.08f,
        0xFFFFFF, 0.08f,
        0x60CDFF, 0.75f,
        // Text Hierarchy
        0xF3F4F6,
        0xA1A6B0,
        0x6E737D,
        0x52565E,
        // Accents & Semantic States
        0x60CDFF,
        0x3898EC,
        0x6CCB5F,
        0xFCE100,
        0x6E737D,
        0xFF99A4,
        0x3B1A1E,
        0x522329,
        0xC42B1C,
        0xFFCCD2,
        // Geometry & Typography
        6.0f, 10.0f, 5.0f,
        L"Segoe UI",
        L"Consolas",
        false
    };

    static constexpr ThemeTokens kEditorial = {
        ThemeId::Editorial,
        "editorial",
        L"Warm Editorial",
        // Surfaces
        0x161615, 0.98f,
        0x222220,
        0x1C1C1A,
        0x2D2D2A,
        0x121211,
        // Borders & Dividers
        0x080807, 0.75f,
        0xFFFFFF, 0.06f,
        0x33332F, 1.00f,
        0xFF5500, 0.85f,
        // Text Hierarchy
        0xF4F1EA,
        0xA8A49C,
        0x6E6B65,
        0x52504B,
        // Accents & Semantic States
        0xFF5500,
        0xCC4400,
        0x4E9A51,
        0xE09F3E,
        0x6E6B65,
        0xD64545,
        0x361919,
        0x4A2222,
        0xB83B3B,
        0xF5C6C6,
        // Geometry & Typography
        5.0f, 8.0f, 4.0f,
        L"Segoe UI",
        L"Consolas",
        true
    };

    static constexpr ThemeTokens kMonolith = {
        ThemeId::Monolith,
        "monolith",
        L"Refined Monolith",
        // Surfaces
        0x0B0F17, 0.97f,
        0x131924,
        0x101620,
        0x1C2433,
        0x080B11,
        // Borders & Dividers
        0x05070B, 0.75f,
        0xFFFFFF, 0.07f,
        0x232D3F, 1.00f,
        0xC5A869, 0.70f,
        // Text Hierarchy
        0xECE7DC,
        0x9A968E,
        0x686A70,
        0x4B505B,
        // Accents & Semantic States
        0xC5A869,
        0x9E8550,
        0x4E9F76,
        0xD49B4B,
        0x5F6672,
        0xC85A53,
        0x33181A,
        0x472023,
        0xA34842,
        0xEED0CD,
        // Geometry & Typography
        7.0f, 10.0f, 5.0f,
        L"Segoe UI",
        L"Consolas",
        false
    };

    switch (id) {
        case ThemeId::Editorial: return kEditorial;
        case ThemeId::Monolith:  return kMonolith;
        case ThemeId::Fluent:
        default:                 return kFluent;
    }
}

inline const ThemeTokens& GetThemeTokens(std::string_view key) noexcept {
    return GetThemeTokens(ParseThemeId(key));
}

namespace detail {
inline std::atomic<int>& ActiveThemeIdStorage() noexcept {
    static std::atomic<int> s_id{static_cast<int>(ThemeId::Fluent)};
    return s_id;
}
} // namespace detail

inline ThemeId GetActiveThemeId() noexcept {
    const int raw = detail::ActiveThemeIdStorage().load(std::memory_order_relaxed);
    if (raw == static_cast<int>(ThemeId::Editorial)) return ThemeId::Editorial;
    if (raw == static_cast<int>(ThemeId::Monolith))  return ThemeId::Monolith;
    return ThemeId::Fluent;
}

inline const ThemeTokens& GetActiveTheme() noexcept {
    return GetThemeTokens(GetActiveThemeId());
}

inline void SetActiveTheme(ThemeId id) noexcept {
    detail::ActiveThemeIdStorage().store(static_cast<int>(id), std::memory_order_relaxed);
}

inline void SetActiveTheme(std::string_view key) noexcept {
    SetActiveTheme(ParseThemeId(key));
}

inline void DrawElevatedCard(ID2D1RenderTarget* rt,
                             const D2D1_RECT_F& card_rect,
                             float radius_dip,
                             float scale,
                             const ThemeTokens& t,
                             float master_alpha = 1.0f,
                             float shadow_pad_dip = 0.0f) noexcept {
    if (!rt || master_alpha <= 0.0f) return;

    const float s = (scale > 0.0f) ? scale : 1.0f;
    const float r = (std::max)(0.0f, radius_dip * s);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.0f), &brush)) || !brush) {
        return;
    }

    // Optional multi-pass soft outer shadow when shadow_pad_dip > 0.0f.
    if (shadow_pad_dip > 0.0f) {
        const float max_expand = shadow_pad_dip * s;
        struct ShadowLayer {
            float expand_ratio;
            float offset_y_dip;
            float alpha;
        };
        constexpr ShadowLayer kLayers[3] = {
            { 0.90f, 1.2f, 0.06f },
            { 0.55f, 0.8f, 0.11f },
            { 0.25f, 0.4f, 0.16f },
        };
        for (const auto& layer : kLayers) {
            const float exp = max_expand * layer.expand_ratio;
            const float oy  = (std::min)(layer.offset_y_dip * s, max_expand - exp);
            const D2D1_RECT_F sr = D2D1::RectF(
                card_rect.left   - exp,
                card_rect.top    - exp + oy,
                card_rect.right  + exp,
                card_rect.bottom + exp + oy
            );
            brush->SetColor(ColorFromHex(t.border_outer, layer.alpha * master_alpha));
            rt->FillRoundedRectangle(D2D1::RoundedRect(sr, r + exp, r + exp), brush.Get());
        }
    }

    // 1. Fill card_rect with surface_canvas.
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(card_rect, r, r);
    brush->SetColor(ColorFromHex(t.surface_canvas, t.surface_canvas_alpha * master_alpha));
    rt->FillRoundedRectangle(rr, brush.Get());

    // 2. Inset 1.0f * scale rounded bezel with border_inner (replaces flat DrawLine top rim).
    const float inset = 1.0f * s;
    if ((card_rect.right - card_rect.left) > inset * 2.0f &&
        (card_rect.bottom - card_rect.top) > inset * 2.0f) {
        const D2D1_RECT_F inner_rect = D2D1::RectF(
            card_rect.left   + inset,
            card_rect.top    + inset,
            card_rect.right  - inset,
            card_rect.bottom - inset
        );
        const float inner_r = (std::max)(0.0f, r - inset);
        brush->SetColor(ColorFromHex(t.border_inner, t.border_inner_alpha * master_alpha));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(inner_rect, inner_r, inner_r), brush.Get(), inset);
    }

    // 3. Stroke card_rect with border_outer.
    brush->SetColor(ColorFromHex(t.border_outer, t.border_outer_alpha * master_alpha));
    rt->DrawRoundedRectangle(rr, brush.Get(), 1.0f * s);
}

inline void DrawCopyIcon(ID2D1RenderTarget* rt,
                         const D2D1_RECT_F& bounds,
                         ID2D1Brush* brush,
                         float stroke_width) noexcept {
    if (!rt || !brush) return;
    const float w = bounds.right - bounds.left;
    const float h = bounds.bottom - bounds.top;
    if (w <= 0.0f || h <= 0.0f) return;

    const float sw = (stroke_width > 0.0f) ? stroke_width : 1.5f;
    const float r  = (std::max)(1.0f, (std::min)(w, h) * 0.10f);

    // Front sheet (lower-right rounded rectangle).
    const D2D1_RECT_F front = D2D1::RectF(
        bounds.left  + w * 0.28f,
        bounds.top   + h * 0.28f,
        bounds.right - w * 0.10f,
        bounds.bottom - h * 0.10f
    );
    rt->DrawRoundedRectangle(D2D1::RoundedRect(front, r, r), brush, sw);

    // Back sheet (upper-left peek without crossing the front sheet interior).
    const float back_left   = bounds.left + w * 0.10f;
    const float back_top    = bounds.top  + h * 0.10f;
    const float back_right  = bounds.right - w * 0.28f;
    const float back_bottom = bounds.bottom - h * 0.28f;

    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    rt->GetFactory(&factory);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(&path)) && path) {
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(path->Open(&sink)) && sink) {
            sink->BeginFigure(D2D1::Point2F(back_left, back_bottom), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(back_left, back_top));
            sink->AddLine(D2D1::Point2F(back_right, back_top));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close())) {
                rt->DrawGeometry(path.Get(), brush, sw);
                return;
            }
        }
    }

    rt->DrawLine(D2D1::Point2F(back_left, back_bottom), D2D1::Point2F(back_left, back_top), brush, sw);
    rt->DrawLine(D2D1::Point2F(back_left, back_top), D2D1::Point2F(back_right, back_top), brush, sw);
}

inline void DrawCheckIcon(ID2D1RenderTarget* rt,
                          const D2D1_RECT_F& bounds,
                          ID2D1Brush* brush,
                          float stroke_width) noexcept {
    if (!rt || !brush) return;
    const float w = bounds.right - bounds.left;
    const float h = bounds.bottom - bounds.top;
    if (w <= 0.0f || h <= 0.0f) return;

    const float sw = (stroke_width > 0.0f) ? stroke_width : 1.5f;
    const D2D1_POINT_2F p0 = D2D1::Point2F(bounds.left + w * 0.16f, bounds.top + h * 0.52f);
    const D2D1_POINT_2F p1 = D2D1::Point2F(bounds.left + w * 0.42f, bounds.top + h * 0.78f);
    const D2D1_POINT_2F p2 = D2D1::Point2F(bounds.left + w * 0.85f, bounds.top + h * 0.24f);

    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    rt->GetFactory(&factory);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(&path)) && path) {
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(path->Open(&sink)) && sink) {
            sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(p1);
            sink->AddLine(p2);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close())) {
                rt->DrawGeometry(path.Get(), brush, sw);
                return;
            }
        }
    }

    rt->DrawLine(p0, p1, brush, sw);
    rt->DrawLine(p1, p2, brush, sw);
}

inline void DrawSpeakerIcon(ID2D1RenderTarget* rt,
                            const D2D1_RECT_F& bounds,
                            ID2D1Brush* brush,
                            float stroke_width,
                            bool active_waves = true) noexcept {
    if (!rt || !brush) return;
    const float w = bounds.right - bounds.left;
    const float h = bounds.bottom - bounds.top;
    if (w <= 0.0f || h <= 0.0f) return;

    const float sw = (stroke_width > 0.0f) ? stroke_width : 1.5f;

    // Speaker cone vertices.
    const D2D1_POINT_2F c0 = D2D1::Point2F(bounds.left + w * 0.12f, bounds.top + h * 0.38f);
    const D2D1_POINT_2F c1 = D2D1::Point2F(bounds.left + w * 0.28f, bounds.top + h * 0.38f);
    const D2D1_POINT_2F c2 = D2D1::Point2F(bounds.left + w * 0.50f, bounds.top + h * 0.20f);
    const D2D1_POINT_2F c3 = D2D1::Point2F(bounds.left + w * 0.50f, bounds.top + h * 0.80f);
    const D2D1_POINT_2F c4 = D2D1::Point2F(bounds.left + w * 0.28f, bounds.top + h * 0.62f);
    const D2D1_POINT_2F c5 = D2D1::Point2F(bounds.left + w * 0.12f, bounds.top + h * 0.62f);

    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    rt->GetFactory(&factory);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(&path)) && path) {
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(path->Open(&sink)) && sink) {
            sink->BeginFigure(c0, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(c1);
            sink->AddLine(c2);
            sink->AddLine(c3);
            sink->AddLine(c4);
            sink->AddLine(c5);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);

            // Inner acoustic wave arc.
            const D2D1_POINT_2F w1_start = D2D1::Point2F(bounds.left + w * 0.64f, bounds.top + h * 0.34f);
            const D2D1_POINT_2F w1_end   = D2D1::Point2F(bounds.left + w * 0.64f, bounds.top + h * 0.66f);
            sink->BeginFigure(w1_start, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(
                w1_end,
                D2D1::SizeF(w * 0.18f, h * 0.22f),
                0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                D2D1_ARC_SIZE_SMALL
            ));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);

            if (active_waves) {
                const D2D1_POINT_2F w2_start = D2D1::Point2F(bounds.left + w * 0.76f, bounds.top + h * 0.22f);
                const D2D1_POINT_2F w2_end   = D2D1::Point2F(bounds.left + w * 0.76f, bounds.top + h * 0.78f);
                sink->BeginFigure(w2_start, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(
                    w2_end,
                    D2D1::SizeF(w * 0.28f, h * 0.34f),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL
                ));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
            }

            if (SUCCEEDED(sink->Close())) {
                rt->DrawGeometry(path.Get(), brush, sw);
                return;
            }
        }
    }

    rt->DrawLine(c0, c1, brush, sw);
    rt->DrawLine(c1, c2, brush, sw);
    rt->DrawLine(c2, c3, brush, sw);
    rt->DrawLine(c3, c4, brush, sw);
    rt->DrawLine(c4, c5, brush, sw);
    rt->DrawLine(c5, c0, brush, sw);
}

inline void DrawSwapIcon(ID2D1RenderTarget* rt,
                         const D2D1_RECT_F& bounds,
                         ID2D1Brush* brush,
                         float stroke_width) noexcept {
    if (!rt || !brush) return;
    const float w = bounds.right - bounds.left;
    const float h = bounds.bottom - bounds.top;
    if (w <= 0.0f || h <= 0.0f) return;

    const float sw = (stroke_width > 0.0f) ? stroke_width : 1.5f;
    const float x_left  = bounds.left  + w * 0.15f;
    const float x_right = bounds.right - w * 0.15f;
    const float y_top   = bounds.top   + h * 0.34f;
    const float y_bot   = bounds.top   + h * 0.66f;
    const float head_dx = w * 0.18f;
    const float head_dy = h * 0.16f;

    // Top arrow pointing right.
    rt->DrawLine(D2D1::Point2F(x_left, y_top), D2D1::Point2F(x_right, y_top), brush, sw);
    rt->DrawLine(D2D1::Point2F(x_right - head_dx, y_top - head_dy), D2D1::Point2F(x_right, y_top), brush, sw);
    rt->DrawLine(D2D1::Point2F(x_right - head_dx, y_top + head_dy), D2D1::Point2F(x_right, y_top), brush, sw);

    // Bottom arrow pointing left.
    rt->DrawLine(D2D1::Point2F(x_right, y_bot), D2D1::Point2F(x_left, y_bot), brush, sw);
    rt->DrawLine(D2D1::Point2F(x_left + head_dx, y_bot - head_dy), D2D1::Point2F(x_left, y_bot), brush, sw);
    rt->DrawLine(D2D1::Point2F(x_left + head_dx, y_bot + head_dy), D2D1::Point2F(x_left, y_bot), brush, sw);
}

} // namespace ui

namespace emebalachat::ui {
using ::ui::ThemeId;
using ::ui::ThemeTokens;
using ::ui::ColorFromHex;
using ::ui::ParseThemeId;
using ::ui::ThemeIdToKey;
using ::ui::GetThemeTokens;
using ::ui::GetActiveThemeId;
using ::ui::GetActiveTheme;
using ::ui::SetActiveTheme;
using ::ui::DrawElevatedCard;
using ::ui::DrawCopyIcon;
using ::ui::DrawCheckIcon;
using ::ui::DrawSpeakerIcon;
using ::ui::DrawSwapIcon;
} // namespace emebalachat::ui
