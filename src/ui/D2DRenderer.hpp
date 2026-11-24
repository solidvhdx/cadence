#pragma once

#include "ui/Theme.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cadence::ui {

enum class TextStyle : uint8_t {
    Title,       // 12.5px SemiBold
    Body,        // 12px Regular
    BodyMedium,  // 12px Medium
    Caption,     // 10.5px Medium uppercase labels
    MonoSmall,   // 11px Cascadia Mono
    MonoMedium,  // 12px Cascadia Mono
    MonoStat,    // 16px Cascadia Mono Medium
    MonoHero     // 28px Cascadia Mono SemiBold
};

// Ultra-low-RAM Direct2D + DirectWrite renderer backed by a double-buffered DIB section.
// Avoids loading heavy D3D11/GPU driver stacks (~32 MB savings) and never wakes laptop dGPUs.
class D2DRenderer {
public:
    D2DRenderer() = default;
    ~D2DRenderer();

    D2DRenderer(const D2DRenderer&)            = delete;
    D2DRenderer& operator=(const D2DRenderer&) = delete;

    bool Initialize(HWND hwnd, uint32_t widthPx, uint32_t heightPx, float dpi);
    void Shutdown();
    bool Resize(uint32_t widthPx, uint32_t heightPx, float dpi);

    bool BeginFrame();
    bool EndFrame(HDC paintDc = nullptr);

    [[nodiscard]] float Dpi() const noexcept { return m_dpi; }
    [[nodiscard]] float MeasureTextWidth(std::wstring_view text, TextStyle style);

    // Precision Craft Primitives
    void Clear(D2D1_COLOR_F color);
    void FillRect(D2D1_RECT_F rc, D2D1_COLOR_F color);
    void FillRoundedRect(D2D1_RECT_F rc, float radius, D2D1_COLOR_F color);
    void DrawRoundedRect(D2D1_RECT_F rc, float radius, D2D1_COLOR_F color, float strokeWidth = 1.0f);
    void DrawLine(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float strokeWidth = 1.0f);
    void FillCircle(float cx, float cy, float r, D2D1_COLOR_F color);
    void DrawCircle(float cx, float cy, float r, D2D1_COLOR_F color, float strokeWidth = 1.0f);

    void DrawText(std::wstring_view text,
                  D2D1_RECT_F rc,
                  TextStyle style,
                  D2D1_COLOR_F color,
                  DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // High-level custom UI components
    void DrawSlider(D2D1_RECT_F trackRc, float t, D2D1_COLOR_F accent, bool hovered, bool dragging);
    void DrawSwitch(D2D1_RECT_F rc, float animT, D2D1_COLOR_F accent, bool hovered);
    void DrawKeycap(D2D1_RECT_F rc, std::wstring_view label, bool activeOrRecording, bool hovered, D2D1_COLOR_F accent);

    // Crisp vector icons
    void DrawCubeLogo(float x, float y, float size, D2D1_COLOR_F color);
    void DrawPinIcon(float cx, float cy, D2D1_COLOR_F color);
    void DrawChevron(float cx, float cy, float size, bool up, D2D1_COLOR_F color);
    void DrawMinimizeIcon(float cx, float cy, D2D1_COLOR_F color);
    void DrawCloseIcon(float cx, float cy, D2D1_COLOR_F color);
    void DrawSparkline(D2D1_RECT_F rc, std::span<const float> values, float targetVal, D2D1_COLOR_F lineColor);

private:
    bool CreateRenderTarget();
    bool CreateBackbuffer();
    void DestroyBackbuffer();
    bool CreateTextFormats();
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F color);
    IDWriteTextFormat*    Format(TextStyle style);

    // Text layouts are expensive to build (shaping, font fallback) and most labels are identical every
    // frame, so layouts are cached by text + style + box + alignment and reused.
    struct LayoutKey {
        std::wstring               text;
        TextStyle                  style  = TextStyle::Body;
        float                      width  = 0.0f;
        float                      height = 0.0f;
        DWRITE_TEXT_ALIGNMENT      hAlign = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        bool operator==(const LayoutKey&) const = default;
    };
    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& k) const noexcept;
    };
    IDWriteTextLayout* CachedLayout(std::wstring_view text, TextStyle style, float width, float height,
                                    DWRITE_TEXT_ALIGNMENT hAlign, DWRITE_PARAGRAPH_ALIGNMENT vAlign);

    HWND     m_hwnd     = nullptr;
    uint32_t m_widthPx  = 1;
    uint32_t m_heightPx = 1;
    float    m_dpi      = 96.0f;

    HDC     m_memDc     = nullptr;
    HBITMAP m_dibBitmap = nullptr;
    HBITMAP m_oldBitmap = nullptr;

    Microsoft::WRL::ComPtr<ID2D1Factory>         m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget>  m_rt;
    Microsoft::WRL::ComPtr<IDWriteFactory>       m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle>     m_dashedStroke;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle>     m_roundStroke;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_formats[8];

    static constexpr size_t kMaxCachedLayouts = 512; // Bounded: changing numbers (CPS, %) create new entries
    std::unordered_map<LayoutKey, Microsoft::WRL::ComPtr<IDWriteTextLayout>, LayoutKeyHash> m_layoutCache;
};

} // namespace cadence::ui
