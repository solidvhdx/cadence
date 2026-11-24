#include "ui/MainWindow.hpp"
#include "ui/UiHelpers.hpp"

#include <utility>

// Reusable widgets shared by the Controls and Settings tabs.

namespace cadence::ui {

void MainWindow::ApplyConfigChange(bool resizeWindow) {
    SyncEngineAndHotkeys();
    if (resizeWindow) {
        UpdateWindowSizeForLayout();
    }
    SaveConfigNow();
    RequestRender();
}

void MainWindow::DrawStepperRow(float w, float& y, const wchar_t* label, const std::wstring& value, std::string_view idPrefix,
                                std::function<void()> onDec, std::function<void()> onInc) {
    const float pad = ThemeTokens::kPadX;
    const std::string decId = std::string(idPrefix) + "_d";
    const std::string incId = std::string(idPrefix) + "_i";

    m_renderer.DrawText(label, {pad, y, pad + 150.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
    const D2D1_RECT_F decRc{w - pad - 110.0f, y + 4.0f, w - pad - 88.0f, y + 24.0f};
    const D2D1_RECT_F incRc{w - pad - 22.0f,  y + 4.0f, w - pad,         y + 24.0f};

    m_renderer.FillRoundedRect(decRc, 4.0f, IsHovered(decId) ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(decRc, 4.0f, ThemeTokens::kHairline);
    m_renderer.DrawText(L"−", decRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(decRc, decId, std::move(onDec));

    m_renderer.DrawText(value, {decRc.right, y, incRc.left, y + 28.0f}, TextStyle::MonoSmall, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    m_renderer.FillRoundedRect(incRc, 4.0f, IsHovered(incId) ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(incRc, 4.0f, ThemeTokens::kHairline);
    m_renderer.DrawText(L"+", incRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(incRc, incId, std::move(onInc));
    y += 28.0f;
}

float MainWindow::DrawModifierChip(float rightX, float y, const wchar_t* label, bool on, const char* id, bool* field) {
    const float bw = 38.0f;
    const D2D1_RECT_F r{rightX - bw, y + 9.0f, rightX, y + 33.0f};
    m_renderer.FillRoundedRect(r, 5.0f, on ? ThemeTokens::kBgElevated : ThemeTokens::kBgCanvas);
    m_renderer.DrawRoundedRect(r, 5.0f, on ? ThemeTokens::kHairlineHover : ThemeTokens::kHairline);
    m_renderer.DrawText(label, r, TextStyle::MonoSmall, on ? ThemeTokens::kTextPrimary : ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(r, id, [this, field]() {
        *field = !(*field);
        ApplyConfigChange();
    });
    return r.left - 5.0f;
}

void MainWindow::DrawSettingToggle(float w, float& y, const wchar_t* title, const wchar_t* subtitle, bool* field, const char* id,
                                   bool resizeOnToggle) {
    const float pad = ThemeTokens::kPadX;
    const float rh  = 44.0f;
    m_renderer.DrawText(title,    {pad, y + 6.0f,  w - pad - 44.0f, y + 22.0f}, TextStyle::Body,    ThemeTokens::kTextPrimary);
    m_renderer.DrawText(subtitle, {pad, y + 22.0f, w - pad - 44.0f, y + 38.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);
    const D2D1_RECT_F sr{w - pad - 32.0f, y + 13.0f, w - pad, y + 31.0f};
    m_renderer.DrawSwitch(sr, *field ? 1.0f : 0.0f, ActiveAccent(), IsHovered(id));
    RegisterHit(sr, id, [this, field, resizeOnToggle]() {
        *field = !(*field);
        ApplyConfigChange(resizeOnToggle);
    });
    y += rh;
    m_renderer.DrawLine(pad, y, w - pad, y, ThemeTokens::kHairlineSoft);
}

} // namespace cadence::ui
