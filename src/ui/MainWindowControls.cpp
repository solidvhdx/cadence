#include "ui/MainWindow.hpp"
#include "ui/UiHelpers.hpp"

#include <algorithm>
#include <cmath>
#include <format>

// Controls tab: mouse (CPS) and keyboard (KPS) settings.

namespace cadence::ui {

void MainWindow::RenderControlsTab(float w, float& y) {
    if (m_config.activeMode == OperatingMode::CPS) {
        RenderCpsControls(w, y);
    } else {
        RenderKpsControls(w, y);
    }
}

void MainWindow::RenderCpsControls(float w, float& y) {
    auto& c = m_config.cps;
    const float pad = ThemeTokens::kPadX;
    const D2D1_COLOR_F accent = ActiveAccent();

    // 1. RATE CONTROL SECTION
    y += 14.0f;
    m_renderer.DrawText(L"CLICK RATE", {pad, y, pad + 140.0f, y + 14.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);

    const D2D1_RECT_F minusRc{w - pad - 52.0f, y, w - pad - 28.0f, y + 22.0f};
    const D2D1_RECT_F plusRc {w - pad - 24.0f, y, w - pad,        y + 22.0f};
    m_renderer.FillRoundedRect(minusRc, 5.0f, IsHovered("rate_minus") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(minusRc, 5.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"−", minusRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(minusRc, "rate_minus", [this]() {
        const double step = (m_config.cps.rateCps > 50.0) ? 5.0 : 0.5;
        m_config.cps.rateCps = std::clamp(std::round((m_config.cps.rateCps - step) * 2.0) / 2.0, 0.5, kMaxRate);
        ApplyConfigChange();
    });

    m_renderer.FillRoundedRect(plusRc, 5.0f, IsHovered("rate_plus") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(plusRc, 5.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"+", plusRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(plusRc, "rate_plus", [this]() {
        const double step = (m_config.cps.rateCps >= 50.0) ? 5.0 : 0.5;
        m_config.cps.rateCps = std::clamp(std::round((m_config.cps.rateCps + step) * 2.0) / 2.0, 0.5, kMaxRate);
        ApplyConfigChange();
    });

    const double intervalMs = 1000.0 / std::max(0.5, c.rateCps);
    const std::wstring mainVal  = m_editingRate
        ? (m_rateEditBuf + L"|")
        : (c.displayAsMs ? std::format(L"{:.1f}", intervalMs) : std::format(L"{:.1f}", c.rateCps));
    const std::wstring mainUnit = c.displayAsMs ? L"ms" : L"CPS";
    const std::wstring subMeta  = c.displayAsMs ? std::format(L"{:.1f} CPS", c.rateCps) : std::format(L"{:.1f} ms", intervalMs);

    const float measuredValW = std::clamp(m_renderer.MeasureTextWidth(mainVal, TextStyle::MonoHero), 58.0f, 126.0f);
    const D2D1_RECT_F valRc{pad, y + 16.0f, pad + measuredValW + 16.0f, y + 50.0f};
    m_renderer.FillRoundedRect(valRc, 6.0f, m_editingRate ? ThemeTokens::kBgElevated : (IsHovered("rate_edit") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed));
    m_renderer.DrawRoundedRect(valRc, 6.0f, m_editingRate ? accent : (IsHovered("rate_edit") ? ThemeTokens::kHairlineHover : ThemeTokens::kHairline));
    m_renderer.DrawText(mainVal, {valRc.left + 8.0f, valRc.top, valRc.right - 6.0f, valRc.bottom}, TextStyle::MonoHero, ThemeTokens::kTextPrimary);
    RegisterHit(valRc, "rate_edit", [this]() {
        m_editingRate = true;
        m_rateEditBuf.clear();
        RequestRender();
    });

    const D2D1_RECT_F unitRc{valRc.right + 8.0f, y + 28.0f, valRc.right + 52.0f, y + 46.0f};
    m_renderer.DrawText(mainUnit, unitRc, TextStyle::BodyMedium, IsHovered("unit_toggle") ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary);
    RegisterHit(unitRc, "unit_toggle", [this]() {
        if (m_editingRate) CommitRateEdit();
        m_config.cps.displayAsMs = !m_config.cps.displayAsMs;
        SaveConfigNow(); RequestRender();
    });

    std::wstring rightMeta = subMeta;
    if (m_engine.State() == EngineState::Active && m_telemetry.actualRate > 0.05) {
        rightMeta = std::format(L"{:.1f} live", m_telemetry.actualRate);
    }
    m_renderer.DrawText(rightMeta, {w - pad - 110.0f, y + 28.0f, w - pad, y + 46.0f},
                        TextStyle::MonoSmall,
                        m_engine.State() == EngineState::Active ? ThemeTokens::kEmerald : ThemeTokens::kTextMuted,
                        DWRITE_TEXT_ALIGNMENT_TRAILING);

    y += 58.0f;
    m_mainSliderRc = {pad, y, w - pad, y + 18.0f};
    const float rateT = RateToSliderT(c.rateCps);
    m_renderer.DrawSlider(m_mainSliderRc, rateT, accent, IsHovered("main_slider"), m_dragSlider == DragSlider::MainRate);
    RegisterSlider(m_mainSliderRc, "main_slider", DragSlider::MainRate);

    y += 28.0f;
    m_renderer.DrawLine(0.0f, y, w, y, ThemeTokens::kHairline);

    // 2. CORE ROWS
    const float rowH = 42.0f;
    m_renderer.DrawText(L"Button", {pad, y, pad + 110.0f, y + rowH}, TextStyle::Body, ThemeTokens::kTextSecondary);
    const D2D1_RECT_F btnTrack{w - pad - 186.0f, y + 8.0f, w - pad, y + 34.0f};
    m_renderer.FillRoundedRect(btnTrack, 7.0f, ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(btnTrack, 7.0f, ThemeTokens::kHairline);
    const wchar_t* btnNames[3] = {L"Left", L"Right", L"Middle"};
    const float segW = (btnTrack.right - btnTrack.left - 4.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F sr{btnTrack.left + 2.0f + i * segW, btnTrack.top + 2.0f, btnTrack.left + 2.0f + (i + 1) * segW, btnTrack.bottom - 2.0f};
        const bool sel = (static_cast<int>(c.button) == i);
        if (sel) {
            m_renderer.FillRoundedRect(sr, 5.0f, ThemeTokens::kBgElevated);
            m_renderer.DrawRoundedRect(sr, 5.0f, ThemeTokens::kHairlineStrong);
        }
        m_renderer.DrawText(btnNames[i], sr, TextStyle::BodyMedium, sel ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(sr, std::format("mb_{}", i), [this, i]() {
            m_config.cps.button = static_cast<MouseButton>(i);
            ApplyConfigChange();
        });
    }
    y += rowH;
    m_renderer.DrawLine(pad, y, w - pad, y, ThemeTokens::kHairlineSoft);

    // Activation + Hotkey
    m_renderer.DrawText(L"Activation", {pad, y, pad + 110.0f, y + rowH}, TextStyle::Body, ThemeTokens::kTextSecondary);
    const D2D1_RECT_F actTrack{w - pad - 186.0f, y + 8.0f, w - pad - 72.0f, y + 34.0f};
    m_renderer.FillRoundedRect(actTrack, 7.0f, ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(actTrack, 7.0f, ThemeTokens::kHairline);
    const float actW = (actTrack.right - actTrack.left - 4.0f) * 0.5f;
    const wchar_t* actNames[2] = {L"Toggle", L"Hold"};
    for (int i = 0; i < 2; ++i) {
        const D2D1_RECT_F sr{actTrack.left + 2.0f + i * actW, actTrack.top + 2.0f, actTrack.left + 2.0f + (i + 1) * actW, actTrack.bottom - 2.0f};
        const bool sel = (static_cast<int>(c.activation) == i);
        if (sel) {
            m_renderer.FillRoundedRect(sr, 5.0f, ThemeTokens::kBgElevated);
            m_renderer.DrawRoundedRect(sr, 5.0f, ThemeTokens::kHairlineStrong);
        }
        m_renderer.DrawText(actNames[i], sr, TextStyle::BodyMedium, sel ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(sr, std::format("act_{}", i), [this, i]() {
            m_config.cps.activation = static_cast<ActivationMode>(i);
            ApplyConfigChange();
        });
    }

    const D2D1_RECT_F hkRc{w - pad - 64.0f, y + 8.0f, w - pad, y + 34.0f};
    const bool recHk = (m_captureKind == CaptureKind::ModeHotkey);
    m_renderer.DrawKeycap(hkRc, recHk ? L"Press…" : FormatHotkey(c.hotkey), recHk, IsHovered("hk_cap"), accent);
    RegisterHit(hkRc, "hk_cap", [this]() { BeginKeyCapture(CaptureKind::ModeHotkey); });

    y += rowH;
    m_renderer.DrawLine(pad, y, w - pad, y, ThemeTokens::kHairlineSoft);

    // Jitter
    m_renderer.DrawText(L"Jitter", {pad, y, pad + 110.0f, y + 38.0f}, TextStyle::Body, ThemeTokens::kTextSecondary);
    const std::wstring jitStr = c.jitterEnabled ? std::format(L"±{:.1f} ms", c.jitterMs) : L"Off";
    m_renderer.DrawText(jitStr, {w - pad - 120.0f, y, w - pad - 42.0f, y + 38.0f}, TextStyle::MonoSmall,
                        c.jitterEnabled ? ThemeTokens::kTextPrimary : ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    const D2D1_RECT_F swRc{w - pad - 32.0f, y + 10.0f, w - pad, y + 28.0f};
    m_renderer.DrawSwitch(swRc, m_jitterAnimT, accent, IsHovered("jit_sw"));
    RegisterHit(swRc, "jit_sw", [this]() {
        m_config.cps.jitterEnabled = !m_config.cps.jitterEnabled;
        SyncEngineAndHotkeys(); UpdateWindowSizeForLayout(); SaveConfigNow(); StartAnimTimer();
    });

    y += 38.0f;
    if (c.jitterEnabled) {
        m_jitterSliderRc = {pad, y - 2.0f, w - pad, y + 16.0f};
        const float jt = static_cast<float>((c.jitterMs - 0.5) / 24.5);
        m_renderer.DrawSlider(m_jitterSliderRc, jt, accent, IsHovered("jit_slider"), m_dragSlider == DragSlider::Jitter);
        RegisterSlider(m_jitterSliderRc, "jit_slider", DragSlider::Jitter);
        y += 24.0f;
    }

    m_renderer.DrawLine(0.0f, y, w, y, ThemeTokens::kHairline);

    // 3. COLLAPSIBLE ADVANCED DRAWER
    const D2D1_RECT_F advHdr{0.0f, y, w, y + 36.0f};
    m_renderer.DrawText(L"Pattern, hold & position", {pad, y, w - pad - 24.0f, y + 36.0f}, TextStyle::Body,
                        IsHovered("adv_hdr") ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary);
    m_renderer.DrawChevron(w - pad - 6.0f, y + 18.0f, 8.0f, m_advancedOpen, ThemeTokens::kTextSecondary);
    RegisterHit(advHdr, "adv_hdr", [this]() {
        m_advancedOpen = !m_advancedOpen;
        UpdateWindowSizeForLayout();
        RequestRender();
    });
    y += 36.0f;

    if (m_advancedOpen) {
        m_renderer.DrawText(L"Click pattern", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F patRc{w - pad - 120.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(patRc, 5.0f, IsHovered("pat_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(patRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(PatternLabel(c.pattern), patRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(patRc, "pat_cyc", [this]() {
            m_config.cps.pattern = static_cast<ClickPattern>((static_cast<int>(m_config.cps.pattern) + 1) % 4);
            ApplyConfigChange(true);
        });
        y += 28.0f;

        if (c.pattern == ClickPattern::CustomBurst) {
            DrawStepperRow(w, y, L"Burst count", std::format(L"{}×", c.burstCount), "burst",
                [this]() { m_config.cps.burstCount = std::max(2u, m_config.cps.burstCount - 1); ApplyConfigChange(); },
                [this]() { m_config.cps.burstCount = std::min(20u, m_config.cps.burstCount + 1); ApplyConfigChange(); });

            DrawStepperRow(w, y, L"Burst cooldown", std::format(L"{} ms", c.burstCooldownMs), "bcd",
                [this]() { m_config.cps.burstCooldownMs = (m_config.cps.burstCooldownMs > 20) ? m_config.cps.burstCooldownMs - 10 : 10; ApplyConfigChange(); },
                [this]() { m_config.cps.burstCooldownMs = std::min(5000u, m_config.cps.burstCooldownMs + 10); ApplyConfigChange(); });
        }

        DrawStepperRow(w, y, L"Press hold", std::format(L"{} ms", c.holdDurationMs), "hold",
            [this]() { m_config.cps.holdDurationMs = (m_config.cps.holdDurationMs > 2) ? m_config.cps.holdDurationMs - 2 : 1; ApplyConfigChange(); },
            [this]() { m_config.cps.holdDurationMs = std::min(150u, m_config.cps.holdDurationMs + 2); ApplyConfigChange(); });

        m_renderer.DrawText(L"Cursor", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F curRc{w - pad - 120.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(curRc, 5.0f, IsHovered("cur_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(curRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(c.cursorMode == CursorMode::FollowCursor ? L"Follow cursor" : L"Locked (X, Y)",
                            curRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(curRc, "cur_cyc", [this]() {
            m_config.cps.cursorMode = (m_config.cps.cursorMode == CursorMode::FollowCursor) ? CursorMode::LockedPoint : CursorMode::FollowCursor;
            ApplyConfigChange(true);
        });
        y += 28.0f;

        if (c.cursorMode == CursorMode::LockedPoint) {
            m_renderer.DrawText(std::format(L"X: {}   Y: {}", c.lockX, c.lockY), {pad, y, w - pad - 80.0f, y + 28.0f}, TextStyle::MonoSmall, ThemeTokens::kTextSecondary);
            const bool picking = (m_pickKind == PickKind::LockedCoords);
            const D2D1_RECT_F pickRc{w - pad - 74.0f, y + 3.0f, w - pad, y + 25.0f};
            m_renderer.FillRoundedRect(pickRc, 5.0f, picking ? accent : (IsHovered("pick_xy") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed));
            m_renderer.DrawRoundedRect(pickRc, 5.0f, ThemeTokens::kHairlineStrong);
            m_renderer.DrawText(picking ? L"Click…" : L"Pick", pickRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(pickRc, "pick_xy", [this]() { BeginGlobalPick(PickKind::LockedCoords); });
            y += 28.0f;

            // Roblox / Game UI Micro-wiggle toggle
            m_renderer.DrawText(L"UI hover wiggle (±1px)", {pad, y, w - pad - 44.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
            const D2D1_RECT_F wigSw{w - pad - 32.0f, y + 5.0f, w - pad, y + 23.0f};
            m_renderer.DrawSwitch(wigSw, c.lockedMicroWiggle ? 1.0f : 0.0f, accent, IsHovered("wig_sw"));
            RegisterHit(wigSw, "wig_sw", [this]() {
                m_config.cps.lockedMicroWiggle = !m_config.cps.lockedMicroWiggle;
                ApplyConfigChange();
            });
            y += 28.0f;
        }

        m_renderer.DrawText(L"Repeat limit", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F limRc{w - pad - 120.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(limRc, 5.0f, IsHovered("lim_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(limRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(LimitLabel(c.limitMode), limRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(limRc, "lim_cyc", [this]() {
            m_config.cps.limitMode = static_cast<RepeatLimitMode>((static_cast<int>(m_config.cps.limitMode) + 1) % 3);
            ApplyConfigChange(true);
        });
        y += 28.0f;

        if (c.limitMode != RepeatLimitMode::Unlimited) {
            const std::wstring lv = (c.limitMode == RepeatLimitMode::Count)
                ? std::format(L"{} clicks", c.limitValue)
                : std::format(L"{} s", c.limitValue);
            DrawStepperRow(w, y, L"Limit value", lv, "limv",
                [this]() {
                    const uint32_t step = (m_config.cps.limitMode == RepeatLimitMode::Count) ? 50u : 5u;
                    m_config.cps.limitValue = (m_config.cps.limitValue > step) ? m_config.cps.limitValue - step : 1u;
                    ApplyConfigChange();
                },
                [this]() {
                    const uint32_t step = (m_config.cps.limitMode == RepeatLimitMode::Count) ? 50u : 5u;
                    m_config.cps.limitValue += step;
                    ApplyConfigChange();
                });
        }
    }
}

void MainWindow::RenderKpsControls(float w, float& y) {
    auto& k = m_config.kps;
    const float pad = ThemeTokens::kPadX;
    const D2D1_COLOR_F accent = ActiveAccent();

    y += 14.0f;
    m_renderer.DrawText(L"KEY PRESS RATE", {pad, y, pad + 140.0f, y + 14.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);

    const D2D1_RECT_F minusRc{w - pad - 52.0f, y, w - pad - 28.0f, y + 22.0f};
    const D2D1_RECT_F plusRc {w - pad - 24.0f, y, w - pad,        y + 22.0f};
    m_renderer.FillRoundedRect(minusRc, 5.0f, IsHovered("rate_minus") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(minusRc, 5.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"−", minusRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(minusRc, "rate_minus", [this]() {
        const double step = (m_config.kps.rateKps > 50.0) ? 5.0 : 0.5;
        m_config.kps.rateKps = std::clamp(std::round((m_config.kps.rateKps - step) * 2.0) / 2.0, 0.5, kMaxRate);
        ApplyConfigChange();
    });

    m_renderer.FillRoundedRect(plusRc, 5.0f, IsHovered("rate_plus") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(plusRc, 5.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"+", plusRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(plusRc, "rate_plus", [this]() {
        const double step = (m_config.kps.rateKps >= 50.0) ? 5.0 : 0.5;
        m_config.kps.rateKps = std::clamp(std::round((m_config.kps.rateKps + step) * 2.0) / 2.0, 0.5, kMaxRate);
        ApplyConfigChange();
    });

    const double intervalMs = 1000.0 / std::max(0.5, k.rateKps);
    const std::wstring mainVal  = m_editingRate
        ? (m_rateEditBuf + L"|")
        : (k.displayAsMs ? std::format(L"{:.1f}", intervalMs) : std::format(L"{:.1f}", k.rateKps));
    const std::wstring mainUnit = k.displayAsMs ? L"ms" : L"KPS";
    const std::wstring subMeta  = k.displayAsMs ? std::format(L"{:.1f} KPS", k.rateKps) : std::format(L"{:.1f} ms", intervalMs);

    const float measuredValW = std::clamp(m_renderer.MeasureTextWidth(mainVal, TextStyle::MonoHero), 58.0f, 126.0f);
    const D2D1_RECT_F valRc{pad, y + 16.0f, pad + measuredValW + 16.0f, y + 50.0f};
    m_renderer.FillRoundedRect(valRc, 6.0f, m_editingRate ? ThemeTokens::kBgElevated : (IsHovered("rate_edit") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed));
    m_renderer.DrawRoundedRect(valRc, 6.0f, m_editingRate ? accent : (IsHovered("rate_edit") ? ThemeTokens::kHairlineHover : ThemeTokens::kHairline));
    m_renderer.DrawText(mainVal, {valRc.left + 8.0f, valRc.top, valRc.right - 6.0f, valRc.bottom}, TextStyle::MonoHero, ThemeTokens::kTextPrimary);
    RegisterHit(valRc, "rate_edit", [this]() {
        m_editingRate = true;
        m_rateEditBuf.clear();
        RequestRender();
    });

    const D2D1_RECT_F unitRc{valRc.right + 8.0f, y + 28.0f, valRc.right + 52.0f, y + 46.0f};
    m_renderer.DrawText(mainUnit, unitRc, TextStyle::BodyMedium, IsHovered("unit_toggle") ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary);
    RegisterHit(unitRc, "unit_toggle", [this]() {
        if (m_editingRate) CommitRateEdit();
        m_config.kps.displayAsMs = !m_config.kps.displayAsMs;
        SaveConfigNow(); RequestRender();
    });

    std::wstring rightMeta = subMeta;
    if (m_engine.State() == EngineState::Active && m_telemetry.actualRate > 0.05) {
        rightMeta = std::format(L"{:.1f} live", m_telemetry.actualRate);
    }
    m_renderer.DrawText(rightMeta, {w - pad - 110.0f, y + 28.0f, w - pad, y + 46.0f},
                        TextStyle::MonoSmall,
                        m_engine.State() == EngineState::Active ? ThemeTokens::kEmerald : ThemeTokens::kTextMuted,
                        DWRITE_TEXT_ALIGNMENT_TRAILING);

    y += 58.0f;
    m_mainSliderRc = {pad, y, w - pad, y + 18.0f};
    const float rateT = RateToSliderT(k.rateKps);
    m_renderer.DrawSlider(m_mainSliderRc, rateT, accent, IsHovered("main_slider"), m_dragSlider == DragSlider::MainRate);
    RegisterSlider(m_mainSliderRc, "main_slider", DragSlider::MainRate);

    y += 28.0f;
    m_renderer.DrawLine(0.0f, y, w, y, ThemeTokens::kHairline);

    const float rowH = 42.0f;
    if (k.inputMode == KeyInputMode::SingleOrChord) {
        m_renderer.DrawText(L"Target Key", {pad, y, pad + 90.0f, y + rowH}, TextStyle::Body, ThemeTokens::kTextSecondary);

        const D2D1_RECT_F keyRc{w - pad - 60.0f, y + 8.0f, w - pad, y + 34.0f};
        const bool recKey = (m_captureKind == CaptureKind::KpsTargetKey);
        m_renderer.DrawKeycap(keyRc, recKey ? L"Press…" : FormatVkName(k.targetVk), recKey, IsHovered("kps_key"), accent);
        RegisterHit(keyRc, "kps_key", [this]() { BeginKeyCapture(CaptureKind::KpsTargetKey); });

        float rx = keyRc.left - 6.0f;
        rx = DrawModifierChip(rx, y, L"Alt",   k.modAlt,   "mod_alt",   &m_config.kps.modAlt);
        rx = DrawModifierChip(rx, y, L"Shift", k.modShift, "mod_shift", &m_config.kps.modShift);
        DrawModifierChip(rx, y, L"Ctrl",       k.modCtrl,  "mod_ctrl",  &m_config.kps.modCtrl);
    } else {
        m_renderer.DrawText(L"Sequence", {pad, y, pad + 80.0f, y + rowH}, TextStyle::Body, ThemeTokens::kTextSecondary);
        std::wstring seqTxt;
        for (size_t i = 0; i < k.sequenceVks.size(); ++i) {
            if (i > 0) seqTxt += L"→";
            seqTxt += FormatVkName(k.sequenceVks[i]);
        }
        m_renderer.DrawText(seqTxt, {pad + 82.0f, y, w - pad - 62.0f, y + rowH}, TextStyle::MonoSmall, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_TRAILING);

        const D2D1_RECT_F addRc{w - pad - 56.0f, y + 9.0f, w - pad - 28.0f, y + 33.0f};
        const bool recSeq = (m_captureKind == CaptureKind::KpsSeqAppend);
        m_renderer.DrawKeycap(addRc, recSeq ? L"…" : L"+", recSeq, IsHovered("seq_add"), accent);
        RegisterHit(addRc, "seq_add", [this]() { BeginKeyCapture(CaptureKind::KpsSeqAppend); });

        const D2D1_RECT_F popRc{w - pad - 24.0f, y + 9.0f, w - pad, y + 33.0f};
        m_renderer.DrawKeycap(popRc, L"×", false, IsHovered("seq_pop"), accent);
        RegisterHit(popRc, "seq_pop", [this]() {
            if (m_config.kps.sequenceVks.size() > 1) m_config.kps.sequenceVks.pop_back();
            ApplyConfigChange();
        });
    }

    y += rowH;
    m_renderer.DrawLine(pad, y, w - pad, y, ThemeTokens::kHairlineSoft);

    m_renderer.DrawText(L"Activation", {pad, y, pad + 110.0f, y + rowH}, TextStyle::Body, ThemeTokens::kTextSecondary);
    const D2D1_RECT_F actTrack{w - pad - 186.0f, y + 8.0f, w - pad - 72.0f, y + 34.0f};
    m_renderer.FillRoundedRect(actTrack, 7.0f, ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(actTrack, 7.0f, ThemeTokens::kHairline);
    const float actW = (actTrack.right - actTrack.left - 4.0f) * 0.5f;
    const wchar_t* actNames[2] = {L"Toggle", L"Hold"};
    for (int i = 0; i < 2; ++i) {
        const D2D1_RECT_F sr{actTrack.left + 2.0f + i * actW, actTrack.top + 2.0f, actTrack.left + 2.0f + (i + 1) * actW, actTrack.bottom - 2.0f};
        const bool sel = (static_cast<int>(k.activation) == i);
        if (sel) {
            m_renderer.FillRoundedRect(sr, 5.0f, ThemeTokens::kBgElevated);
            m_renderer.DrawRoundedRect(sr, 5.0f, ThemeTokens::kHairlineStrong);
        }
        m_renderer.DrawText(actNames[i], sr, TextStyle::BodyMedium, sel ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(sr, std::format("kact_{}", i), [this, i]() {
            m_config.kps.activation = static_cast<ActivationMode>(i);
            ApplyConfigChange();
        });
    }

    const D2D1_RECT_F hkRc{w - pad - 64.0f, y + 8.0f, w - pad, y + 34.0f};
    const bool recHk = (m_captureKind == CaptureKind::ModeHotkey);
    m_renderer.DrawKeycap(hkRc, recHk ? L"Press…" : FormatHotkey(k.hotkey), recHk, IsHovered("hk_cap"), accent);
    RegisterHit(hkRc, "hk_cap", [this]() { BeginKeyCapture(CaptureKind::ModeHotkey); });

    y += rowH;
    m_renderer.DrawLine(pad, y, w - pad, y, ThemeTokens::kHairlineSoft);

    m_renderer.DrawText(L"Jitter", {pad, y, pad + 110.0f, y + 38.0f}, TextStyle::Body, ThemeTokens::kTextSecondary);
    const std::wstring jitStr = k.jitterEnabled ? std::format(L"±{:.1f} ms", k.jitterMs) : L"Off";
    m_renderer.DrawText(jitStr, {w - pad - 120.0f, y, w - pad - 42.0f, y + 38.0f}, TextStyle::MonoSmall,
                        k.jitterEnabled ? ThemeTokens::kTextPrimary : ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    const D2D1_RECT_F swRc{w - pad - 32.0f, y + 10.0f, w - pad, y + 28.0f};
    m_renderer.DrawSwitch(swRc, m_jitterAnimT, accent, IsHovered("jit_sw"));
    RegisterHit(swRc, "jit_sw", [this]() {
        m_config.kps.jitterEnabled = !m_config.kps.jitterEnabled;
        SyncEngineAndHotkeys(); UpdateWindowSizeForLayout(); SaveConfigNow(); StartAnimTimer();
    });

    y += 38.0f;
    if (k.jitterEnabled) {
        m_jitterSliderRc = {pad, y - 2.0f, w - pad, y + 16.0f};
        const float jt = static_cast<float>((k.jitterMs - 0.5) / 24.5);
        m_renderer.DrawSlider(m_jitterSliderRc, jt, accent, IsHovered("jit_slider"), m_dragSlider == DragSlider::Jitter);
        RegisterSlider(m_jitterSliderRc, "jit_slider", DragSlider::Jitter);
        y += 24.0f;
    }

    m_renderer.DrawLine(0.0f, y, w, y, ThemeTokens::kHairline);

    const D2D1_RECT_F advHdr{0.0f, y, w, y + 36.0f};
    m_renderer.DrawText(L"Hold duration & dispatch", {pad, y, w - pad - 24.0f, y + 36.0f}, TextStyle::Body,
                        IsHovered("adv_hdr") ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary);
    m_renderer.DrawChevron(w - pad - 6.0f, y + 18.0f, 8.0f, m_advancedOpen, ThemeTokens::kTextSecondary);
    RegisterHit(advHdr, "adv_hdr", [this]() {
        m_advancedOpen = !m_advancedOpen;
        UpdateWindowSizeForLayout();
        RequestRender();
    });
    y += 36.0f;

    if (m_advancedOpen) {
        m_renderer.DrawText(L"Key mode", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F kmRc{w - pad - 128.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(kmRc, 5.0f, IsHovered("km_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(kmRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(k.inputMode == KeyInputMode::SingleOrChord ? L"Single / Chord" : L"Sequence",
                            kmRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(kmRc, "km_cyc", [this]() {
            m_config.kps.inputMode = (m_config.kps.inputMode == KeyInputMode::SingleOrChord) ? KeyInputMode::Sequence : KeyInputMode::SingleOrChord;
            ApplyConfigChange();
        });
        y += 28.0f;

        m_renderer.DrawText(L"Press duration", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F dRc{w - pad - 110.0f, y + 4.0f, w - pad - 88.0f, y + 24.0f};
        const D2D1_RECT_F iRc{w - pad - 22.0f,  y + 4.0f, w - pad,         y + 24.0f};
        m_renderer.FillRoundedRect(dRc, 4.0f, IsHovered("kp_d") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(dRc, 4.0f, ThemeTokens::kHairline);
        m_renderer.DrawText(L"−", dRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(dRc, "kp_d", [this]() {
            m_config.kps.pressDurationMs = (m_config.kps.pressDurationMs > 5) ? m_config.kps.pressDurationMs - 5 : 1;
            ApplyConfigChange();
        });
        m_renderer.DrawText(std::format(L"{} ms", k.pressDurationMs), {dRc.right, y, iRc.left, y + 28.0f}, TextStyle::MonoSmall, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        m_renderer.FillRoundedRect(iRc, 4.0f, IsHovered("kp_i") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(iRc, 4.0f, ThemeTokens::kHairline);
        m_renderer.DrawText(L"+", iRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(iRc, "kp_i", [this]() {
            m_config.kps.pressDurationMs = std::min(300u, m_config.kps.pressDurationMs + 5);
            ApplyConfigChange();
        });
        y += 28.0f;

        m_renderer.DrawText(L"Dispatch method", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F dmRc{w - pad - 128.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(dmRc, 5.0f, IsHovered("dm_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(dmRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(k.dispatchMethod == KeyDispatchMethod::ScanCode ? L"Scan Code (Games)" : L"Virtual Key",
                            dmRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(dmRc, "dm_cyc", [this]() {
            m_config.kps.dispatchMethod = (m_config.kps.dispatchMethod == KeyDispatchMethod::ScanCode) ? KeyDispatchMethod::VirtualKey : KeyDispatchMethod::ScanCode;
            ApplyConfigChange();
        });
        y += 28.0f;

        m_renderer.DrawText(L"Repeat limit", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F limRc{w - pad - 128.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(limRc, 5.0f, IsHovered("klim_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(limRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(LimitLabel(k.limitMode), limRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(limRc, "klim_cyc", [this]() {
            m_config.kps.limitMode = static_cast<RepeatLimitMode>((static_cast<int>(m_config.kps.limitMode) + 1) % 3);
            ApplyConfigChange(true);
        });
        y += 28.0f;

        if (k.limitMode != RepeatLimitMode::Unlimited) {
            const std::wstring lv = (k.limitMode == RepeatLimitMode::Count)
                ? std::format(L"{} presses", k.limitValue)
                : std::format(L"{} s", k.limitValue);
            m_renderer.DrawText(L"Limit value", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
            const D2D1_RECT_F ldRc{w - pad - 110.0f, y + 4.0f, w - pad - 88.0f, y + 24.0f};
            const D2D1_RECT_F liRc{w - pad - 22.0f,  y + 4.0f, w - pad,         y + 24.0f};
            m_renderer.FillRoundedRect(ldRc, 4.0f, IsHovered("kl_d") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
            m_renderer.DrawRoundedRect(ldRc, 4.0f, ThemeTokens::kHairline);
            m_renderer.DrawText(L"−", ldRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(ldRc, "kl_d", [this]() {
                const uint32_t step = (m_config.kps.limitMode == RepeatLimitMode::Count) ? 50u : 5u;
                m_config.kps.limitValue = (m_config.kps.limitValue > step) ? m_config.kps.limitValue - step : 1u;
                ApplyConfigChange();
            });
            m_renderer.DrawText(lv, {ldRc.right, y, liRc.left, y + 28.0f}, TextStyle::MonoSmall, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            m_renderer.FillRoundedRect(liRc, 4.0f, IsHovered("kl_i") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
            m_renderer.DrawRoundedRect(liRc, 4.0f, ThemeTokens::kHairline);
            m_renderer.DrawText(L"+", liRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(liRc, "kl_i", [this]() {
                const uint32_t step = (m_config.kps.limitMode == RepeatLimitMode::Count) ? 50u : 5u;
                m_config.kps.limitValue += step;
                ApplyConfigChange();
            });
            y += 28.0f;
        }
    }
}

} // namespace cadence::ui
