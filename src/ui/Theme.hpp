#pragma once

#include <d2d1.h>

#include <cstdint>

namespace cadence::ui {

[[nodiscard]] constexpr D2D1_COLOR_F HexColor(uint32_t rgb, float alpha = 1.0f) noexcept {
    return D2D1_COLOR_F{
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        alpha
    };
}

// Precision Craft Design System Tokens (Matte Obsidian & Restrained Violet)
struct ThemeTokens {
    // Matte neutral charcoal surfaces (never harsh #000000)
    static constexpr D2D1_COLOR_F kBgCanvas      = HexColor(0x0D0D10);
    static constexpr D2D1_COLOR_F kBgFooter      = HexColor(0x0B0B0E);
    static constexpr D2D1_COLOR_F kBgRecessed    = HexColor(0x131317);
    static constexpr D2D1_COLOR_F kBgElevated    = HexColor(0x1D1D23);
    static constexpr D2D1_COLOR_F kBgHover       = HexColor(0x25252D);
    static constexpr D2D1_COLOR_F kBgPopover     = HexColor(0x15151B);
    static constexpr D2D1_COLOR_F kBgKeycap      = HexColor(0x17171D);
    static constexpr D2D1_COLOR_F kSliderTrack   = HexColor(0x1F1F27);

    // Crisp 1px hairlines
    static constexpr D2D1_COLOR_F kHairline      = HexColor(0xFFFFFF, 0.07f);
    static constexpr D2D1_COLOR_F kHairlineSoft  = HexColor(0xFFFFFF, 0.05f);
    static constexpr D2D1_COLOR_F kHairlineStrong= HexColor(0xFFFFFF, 0.13f);
    static constexpr D2D1_COLOR_F kHairlineHover = HexColor(0xFFFFFF, 0.22f);

    // Off-white typography hierarchy (prevents optical halation)
    static constexpr D2D1_COLOR_F kTextPrimary   = HexColor(0xEDEDEF);
    static constexpr D2D1_COLOR_F kTextSecondary = HexColor(0x8E8E98);
    static constexpr D2D1_COLOR_F kTextMuted     = HexColor(0x5A5A64);
    static constexpr D2D1_COLOR_F kTextWhite     = HexColor(0xFFFFFF);

    // Editorial Linear-grade violet accents (CPS vs KPS subtle shift)
    static constexpr D2D1_COLOR_F kAccentCps      = HexColor(0x7C5CFC);
    static constexpr D2D1_COLOR_F kAccentCpsHover = HexColor(0x8B6EFF);
    static constexpr D2D1_COLOR_F kAccentKps      = HexColor(0x9B6BFF);
    static constexpr D2D1_COLOR_F kAccentKpsHover = HexColor(0xAA7FFF);

    // Semantic status colors
    static constexpr D2D1_COLOR_F kEmerald       = HexColor(0x10B981);
    static constexpr D2D1_COLOR_F kAmber         = HexColor(0xF59E0B);
    static constexpr D2D1_COLOR_F kDanger        = HexColor(0xDC2626);
    static constexpr D2D1_COLOR_F kDangerHover   = HexColor(0xEF4444);
    static constexpr D2D1_COLOR_F kDangerSurface = HexColor(0x1D1113);
    static constexpr D2D1_COLOR_F kDangerBorder  = HexColor(0xEF4444, 0.38f);
    static constexpr D2D1_COLOR_F kDangerText    = HexColor(0xFCA5A5);

    // Layout metrics (in DIPs)
    static constexpr float kWindowWidth          = 420.0f;
    static constexpr float kTitlebarHeight       = 40.0f;
    static constexpr float kPadX                 = 16.0f;
    static constexpr float kRadiusControl        = 7.0f;
    static constexpr float kRadiusButton         = 8.0f;
    static constexpr float kRadiusSmall          = 5.0f;
};

} // namespace cadence::ui
