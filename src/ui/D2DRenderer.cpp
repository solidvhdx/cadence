#include "ui/D2DRenderer.hpp"

#include <algorithm>
#include <cmath>

namespace cadence::ui {

using Microsoft::WRL::ComPtr;

D2DRenderer::~D2DRenderer() {
    Shutdown();
}

bool D2DRenderer::Initialize(HWND hwnd, uint32_t widthPx, uint32_t heightPx, float dpi) {
    m_hwnd     = hwnd;
    m_widthPx  = std::max(1u, widthPx);
    m_heightPx = std::max(1u, heightPx);
    m_dpi      = dpi > 0.0f ? dpi : 96.0f;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &m_dwriteFactory))) {
        return false;
    }

    const float dashes[] = {3.0f, 3.0f};
    D2D1_STROKE_STYLE_PROPERTIES dashProps = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
        D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f
    );
    m_d2dFactory->CreateStrokeStyle(dashProps, dashes, 2, &m_dashedStroke);

    D2D1_STROKE_STYLE_PROPERTIES roundProps = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND
    );
    m_d2dFactory->CreateStrokeStyle(roundProps, nullptr, 0, &m_roundStroke);

    if (!CreateTextFormats()) {
        return false;
    }
    if (!CreateRenderTarget()) {
        return false;
    }
    return CreateBackbuffer();
}

void D2DRenderer::Shutdown() {
    DestroyBackbuffer();
    m_brush.Reset();
    m_rt.Reset();
    for (auto& f : m_formats) f.Reset();
    m_dashedStroke.Reset();
    m_roundStroke.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
}

bool D2DRenderer::CreateTextFormats() {
    auto pickFont = [&](const wchar_t* preferred, const wchar_t* fallback) -> const wchar_t* {
        ComPtr<IDWriteFontCollection> sysFonts;
        if (SUCCEEDED(m_dwriteFactory->GetSystemFontCollection(&sysFonts)) && sysFonts) {
            UINT32 index = 0;
            BOOL exists  = FALSE;
            if (SUCCEEDED(sysFonts->FindFamilyName(preferred, &index, &exists)) && exists) {
                return preferred;
            }
        }
        return fallback;
    };

    auto makeFmt = [&](TextStyle idx, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) -> bool {
        ComPtr<IDWriteTextFormat> fmt;
        HRESULT hr = m_dwriteFactory->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt
        );
        if (FAILED(hr)) return false;
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis))) {
            fmt->SetTrimming(&trim, ellipsis.Get());
        }
        m_formats[static_cast<size_t>(idx)] = std::move(fmt);
        return true;
    };

    const wchar_t* uiFont   = pickFont(L"Segoe UI Variable Text", L"Segoe UI");
    const wchar_t* monoFont = pickFont(L"Cascadia Mono", L"Consolas");

    return makeFmt(TextStyle::Title,      uiFont,   12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) &&
           makeFmt(TextStyle::Body,       uiFont,   12.0f, DWRITE_FONT_WEIGHT_REGULAR) &&
           makeFmt(TextStyle::BodyMedium, uiFont,   12.0f, DWRITE_FONT_WEIGHT_MEDIUM) &&
           makeFmt(TextStyle::Caption,    uiFont,   10.5f, DWRITE_FONT_WEIGHT_MEDIUM) &&
           makeFmt(TextStyle::MonoSmall,  monoFont, 11.0f, DWRITE_FONT_WEIGHT_REGULAR) &&
           makeFmt(TextStyle::MonoMedium, monoFont, 12.0f, DWRITE_FONT_WEIGHT_MEDIUM) &&
           makeFmt(TextStyle::MonoStat,   monoFont, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD) &&
           makeFmt(TextStyle::MonoHero,   monoFont, 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

bool D2DRenderer::CreateRenderTarget() {
    m_brush.Reset();
    m_rt.Reset();
    if (!m_d2dFactory) return false;

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        m_dpi, m_dpi,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT
    );

    if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_rt))) {
        return false;
    }

    m_rt->SetDpi(m_dpi, m_dpi);
    m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    m_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    return SUCCEEDED(m_rt->CreateSolidColorBrush(ThemeTokens::kTextPrimary, &m_brush));
}

void D2DRenderer::DestroyBackbuffer() {
    if (m_memDc) {
        if (m_oldBitmap) {
            SelectObject(m_memDc, m_oldBitmap);
            m_oldBitmap = nullptr;
        }
        DeleteDC(m_memDc);
        m_memDc = nullptr;
    }
    if (m_dibBitmap) {
        DeleteObject(m_dibBitmap);
        m_dibBitmap = nullptr;
    }
}

bool D2DRenderer::CreateBackbuffer() {
    DestroyBackbuffer();

    HDC screenDc = GetDC(m_hwnd);
    m_memDc = CreateCompatibleDC(screenDc);
    if (!m_memDc) {
        ReleaseDC(m_hwnd, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(m_widthPx);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(m_heightPx); // top-down DIB
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_dibBitmap = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(m_hwnd, screenDc);

    if (!m_dibBitmap) {
        DestroyBackbuffer();
        return false;
    }
    m_oldBitmap = static_cast<HBITMAP>(SelectObject(m_memDc, m_dibBitmap));
    return true;
}

bool D2DRenderer::Resize(uint32_t widthPx, uint32_t heightPx, float dpi) {
    m_widthPx  = std::max(1u, widthPx);
    m_heightPx = std::max(1u, heightPx);
    m_dpi      = dpi > 0.0f ? dpi : 96.0f;

    if (!m_d2dFactory || !m_hwnd) {
        return false;
    }
    if (m_rt) {
        m_rt->SetDpi(m_dpi, m_dpi);
    } else if (!CreateRenderTarget()) {
        return false;
    }
    return CreateBackbuffer();
}

bool D2DRenderer::BeginFrame() {
    if (!m_d2dFactory || !m_hwnd) return false;
    if (!m_rt && !CreateRenderTarget()) return false;
    if (!m_memDc && !CreateBackbuffer()) return false;

    const RECT rc{0, 0, static_cast<LONG>(m_widthPx), static_cast<LONG>(m_heightPx)};
    if (FAILED(m_rt->BindDC(m_memDc, &rc))) {
        return false;
    }
    m_rt->BeginDraw();
    m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

bool D2DRenderer::EndFrame(HDC paintDc) {
    if (!m_rt || !m_memDc) return false;
    const HRESULT hr = m_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        CreateRenderTarget();
        return false;
    }
    if (FAILED(hr)) return false;

    if (paintDc) {
        BitBlt(paintDc, 0, 0, static_cast<int>(m_widthPx), static_cast<int>(m_heightPx), m_memDc, 0, 0, SRCCOPY);
    } else if (m_hwnd) {
        HDC wdc = GetDC(m_hwnd);
        if (wdc) {
            BitBlt(wdc, 0, 0, static_cast<int>(m_widthPx), static_cast<int>(m_heightPx), m_memDc, 0, 0, SRCCOPY);
            ReleaseDC(m_hwnd, wdc);
        }
    }
    return true;
}

float D2DRenderer::MeasureTextWidth(std::wstring_view text, TextStyle style) {
    auto* fmt = Format(style);
    if (!m_dwriteFactory || !fmt || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(m_dwriteFactory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, 1000.0f, 100.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(layout->GetMetrics(&m))) {
        return m.widthIncludingTrailingWhitespace;
    }
    return 0.0f;
}

ID2D1SolidColorBrush* D2DRenderer::Brush(D2D1_COLOR_F color) {
    m_brush->SetColor(color);
    return m_brush.Get();
}

IDWriteTextFormat* D2DRenderer::Format(TextStyle style) {
    return m_formats[static_cast<size_t>(style)].Get();
}

void D2DRenderer::Clear(D2D1_COLOR_F color) {
    m_rt->Clear(color);
}

void D2DRenderer::FillRect(D2D1_RECT_F rc, D2D1_COLOR_F color) {
    m_rt->FillRectangle(rc, Brush(color));
}

void D2DRenderer::FillRoundedRect(D2D1_RECT_F rc, float radius, D2D1_COLOR_F color) {
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), Brush(color));
}

void D2DRenderer::DrawRoundedRect(D2D1_RECT_F rc, float radius, D2D1_COLOR_F color, float strokeWidth) {
    D2D1_RECT_F inset = {rc.left + 0.5f, rc.top + 0.5f, rc.right - 0.5f, rc.bottom - 0.5f};
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(inset, radius, radius), Brush(color), strokeWidth);
}

void D2DRenderer::DrawLine(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float strokeWidth) {
    const bool isHorizontal = (std::abs(y1 - y2) < 0.01f);
    const bool isVertical   = (std::abs(x1 - x2) < 0.01f);
    const float ox = isVertical   ? 0.5f : 0.0f;
    const float oy = isHorizontal ? 0.5f : 0.0f;
    m_rt->DrawLine(D2D1::Point2F(x1 + ox, y1 + oy), D2D1::Point2F(x2 + ox, y2 + oy), Brush(color), strokeWidth);
}

void D2DRenderer::FillCircle(float cx, float cy, float r, D2D1_COLOR_F color) {
    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), Brush(color));
}

void D2DRenderer::DrawCircle(float cx, float cy, float r, D2D1_COLOR_F color, float strokeWidth) {
    m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), Brush(color), strokeWidth);
}

void D2DRenderer::DrawText(std::wstring_view text,
                           D2D1_RECT_F rc,
                           TextStyle style,
                           D2D1_COLOR_F color,
                           DWRITE_TEXT_ALIGNMENT hAlign,
                           DWRITE_PARAGRAPH_ALIGNMENT vAlign) {
    auto* fmt = Format(style);
    if (!fmt || text.empty()) return;
    fmt->SetTextAlignment(hAlign);
    fmt->SetParagraphAlignment(vAlign);
    m_rt->DrawTextW(
        text.data(),
        static_cast<UINT32>(text.size()),
        fmt,
        rc,
        Brush(color),
        D2D1_DRAW_TEXT_OPTIONS_CLIP
    );
}

void D2DRenderer::DrawSlider(D2D1_RECT_F trackRc, float t, D2D1_COLOR_F accent, bool hovered, bool dragging) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float cy    = (trackRc.top + trackRc.bottom) * 0.5f;
    const float h     = 4.0f;
    const float left  = trackRc.left + 7.0f;
    const float right = trackRc.right - 7.0f;
    const float thumbX = left + (right - left) * t;

    FillRoundedRect({left, cy - h * 0.5f, right, cy + h * 0.5f}, 2.0f, ThemeTokens::kSliderTrack);
    if (thumbX > left + 1.0f) {
        FillRoundedRect({left, cy - h * 0.5f, thumbX, cy + h * 0.5f}, 2.0f, accent);
    }

    const float radius = (dragging || hovered) ? 7.5f : 6.5f;
    FillCircle(thumbX, cy + 0.8f, radius, HexColor(0x000000, 0.45f));
    FillCircle(thumbX, cy, radius, (dragging || hovered) ? ThemeTokens::kTextWhite : ThemeTokens::kTextPrimary);
    DrawCircle(thumbX, cy, radius, HexColor(0x000000, 0.35f), 1.0f);
}

void D2DRenderer::DrawSwitch(D2D1_RECT_F rc, float animT, D2D1_COLOR_F accent, bool hovered) {
    animT = std::clamp(animT, 0.0f, 1.0f);
    const float h = rc.bottom - rc.top;
    const float r = h * 0.5f;

    const D2D1_COLOR_F offBg = hovered ? HexColor(0x2B2B35) : HexColor(0x222229);
    const D2D1_COLOR_F bg{
        offBg.r + (accent.r - offBg.r) * animT,
        offBg.g + (accent.g - offBg.g) * animT,
        offBg.b + (accent.b - offBg.b) * animT,
        1.0f
    };

    FillRoundedRect(rc, r, bg);
    DrawRoundedRect(rc, r, animT > 0.5f ? HexColor(0xFFFFFF, 0.14f) : ThemeTokens::kHairline, 1.0f);

    const float knobR  = (h - 6.0f) * 0.5f;
    const float minX   = rc.left + 3.0f + knobR;
    const float maxX   = rc.right - 3.0f - knobR;
    const float knobX  = minX + (maxX - minX) * animT;
    const float knobY  = (rc.top + rc.bottom) * 0.5f;
    FillCircle(knobX, knobY, knobR, animT > 0.5f ? ThemeTokens::kTextWhite : HexColor(0xA1A1AA));
}

void D2DRenderer::DrawKeycap(D2D1_RECT_F rc, std::wstring_view label, bool activeOrRecording, bool hovered, D2D1_COLOR_F accent) {
    const D2D1_COLOR_F bg = activeOrRecording
        ? D2D1_COLOR_F{accent.r, accent.g, accent.b, 0.18f}
        : (hovered ? ThemeTokens::kBgElevated : ThemeTokens::kBgKeycap);
    const D2D1_COLOR_F border = activeOrRecording
        ? accent
        : (hovered ? ThemeTokens::kHairlineHover : ThemeTokens::kHairlineStrong);

    FillRoundedRect(rc, 6.0f, bg);
    DrawRoundedRect(rc, 6.0f, border, 1.0f);
    DrawLine(rc.left + 4.0f, rc.top + 1.0f, rc.right - 4.0f, rc.top + 1.0f, HexColor(0xFFFFFF, 0.06f), 1.0f);
    DrawText(label, rc, TextStyle::MonoSmall, activeOrRecording ? ThemeTokens::kTextWhite : ThemeTokens::kTextPrimary,
             DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void D2DRenderer::DrawCubeLogo(float x, float y, float size, D2D1_COLOR_F color) {
    const float cx = x + size * 0.5f;
    const float cy = y + size * 0.5f;
    const float r  = size * 0.46f;
    const float dx = r * 0.866f;
    const float dy = r * 0.5f;

    const D2D1_POINT_2F top = {cx, cy - r};
    const D2D1_POINT_2F tr  = {cx + dx, cy - dy};
    const D2D1_POINT_2F br  = {cx + dx, cy + dy};
    const D2D1_POINT_2F bot = {cx, cy + r};
    const D2D1_POINT_2F bl  = {cx - dx, cy + dy};
    const D2D1_POINT_2F tl  = {cx - dx, cy - dy};
    const D2D1_POINT_2F mid = {cx, cy};

    auto* b = Brush(color);
    auto* s = m_roundStroke.Get();
    m_rt->DrawLine(top, tr, b, 1.4f, s);
    m_rt->DrawLine(tr, br, b, 1.4f, s);
    m_rt->DrawLine(br, bot, b, 1.4f, s);
    m_rt->DrawLine(bot, bl, b, 1.4f, s);
    m_rt->DrawLine(bl, tl, b, 1.4f, s);
    m_rt->DrawLine(tl, top, b, 1.4f, s);
    m_rt->DrawLine(tl, mid, b, 1.25f, s);
    m_rt->DrawLine(tr, mid, b, 1.25f, s);
    m_rt->DrawLine(mid, bot, b, 1.25f, s);
}

void D2DRenderer::DrawPinIcon(float cx, float cy, D2D1_COLOR_F color) {
    auto* b = Brush(color);
    auto* s = m_roundStroke.Get();
    m_rt->DrawLine({cx - 3.5f, cy - 4.0f}, {cx + 3.5f, cy - 4.0f}, b, 1.3f, s);
    m_rt->DrawLine({cx - 2.2f, cy - 4.0f}, {cx - 2.2f, cy + 0.5f}, b, 1.3f, s);
    m_rt->DrawLine({cx + 2.2f, cy - 4.0f}, {cx + 2.2f, cy + 0.5f}, b, 1.3f, s);
    m_rt->DrawLine({cx - 4.2f, cy + 1.2f}, {cx + 4.2f, cy + 1.2f}, b, 1.3f, s);
    m_rt->DrawLine({cx, cy + 1.2f}, {cx, cy + 5.2f}, b, 1.3f, s);
}

void D2DRenderer::DrawChevron(float cx, float cy, float size, bool up, D2D1_COLOR_F color) {
    const float h = size * 0.5f;
    const float d = up ? -h * 0.55f : h * 0.55f;
    auto* b = Brush(color);
    auto* s = m_roundStroke.Get();
    m_rt->DrawLine({cx - h, cy - d}, {cx, cy + d}, b, 1.35f, s);
    m_rt->DrawLine({cx, cy + d}, {cx + h, cy - d}, b, 1.35f, s);
}

void D2DRenderer::DrawMinimizeIcon(float cx, float cy, D2D1_COLOR_F color) {
    m_rt->DrawLine({cx - 4.5f, cy + 0.5f}, {cx + 4.5f, cy + 0.5f}, Brush(color), 1.2f);
}

void D2DRenderer::DrawCloseIcon(float cx, float cy, D2D1_COLOR_F color) {
    auto* b = Brush(color);
    auto* s = m_roundStroke.Get();
    m_rt->DrawLine({cx - 4.0f, cy - 4.0f}, {cx + 4.0f, cy + 4.0f}, b, 1.25f, s);
    m_rt->DrawLine({cx + 4.0f, cy - 4.0f}, {cx - 4.0f, cy + 4.0f}, b, 1.25f, s);
}

void D2DRenderer::DrawSparkline(D2D1_RECT_F rc, std::span<const float> values, float targetVal, D2D1_COLOR_F lineColor) {
    FillRoundedRect(rc, 6.0f, HexColor(0x111115));
    DrawRoundedRect(rc, 6.0f, ThemeTokens::kHairline, 1.0f);

    const float padX = 6.0f;
    const float padY = 10.0f;
    const float w    = (rc.right - rc.left) - padX * 2.0f;
    const float h    = (rc.bottom - rc.top) - padY * 2.0f;
    if (w <= 4.0f || h <= 4.0f) return;

    float maxVal = std::max(targetVal * 1.35f, 10.0f);
    for (float v : values) maxVal = std::max(maxVal, v * 1.15f);

    const float targetY = rc.bottom - padY - (targetVal / maxVal) * h;
    m_rt->DrawLine(
        {rc.left + padX, targetY},
        {rc.right - padX, targetY},
        Brush(HexColor(0xFFFFFF, 0.10f)),
        1.0f,
        m_dashedStroke.Get()
    );

    if (values.size() < 2) return;

    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(m_d2dFactory->CreatePathGeometry(&geo))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink))) return;

    for (size_t i = 0; i < values.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(values.size() - 1);
        const float x = rc.left + padX + t * w;
        const float y = rc.bottom - padY - (std::clamp(values[i], 0.0f, maxVal) / maxVal) * h;
        if (i == 0) sink->BeginFigure({x, y}, D2D1_FIGURE_BEGIN_HOLLOW);
        else        sink->AddLine({x, y});
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    m_rt->DrawGeometry(geo.Get(), Brush(lineColor), 1.6f, m_roundStroke.Get());
}

} // namespace cadence::ui
