#include "ui/MainWindow.hpp"
#include "ui/UiHelpers.hpp"

#include <algorithm>
#include <cmath>
#include <format>

// Frame composition: titlebar, mode tabs, telemetry & settings tabs, footer and toast.

namespace cadence::ui {

void MainWindow::Render(HDC paintDc) {
    if (m_inTray) return;
    if (!m_renderer.BeginFrame()) return;

    m_hits.clear();
    const float w = m_widthDip;
    const float h = m_heightDip;

    m_renderer.Clear(ThemeTokens::kBgCanvas);

    RenderTitlebar(w);

    float y = ThemeTokens::kTitlebarHeight;
    RenderModeAndTabs(w, y);

    if (m_tab == ViewTab::Controls) {
        RenderControlsTab(w, y);
    } else if (m_tab == ViewTab::Telemetry) {
        RenderTelemetryTab(w, y);
    } else {
        RenderSettingsTab(w, y);
    }

    RenderFooter(w, h);

    if (m_profileOpen) {
        RenderProfilePopover();
    }
    if (m_updateModalOpen && m_availableUpdate) {
        RenderUpdateModal(w, h);
    }
    if (m_toastMs > 0.0f) {
        RenderToast(w, h);
    }

    m_renderer.EndFrame(paintDc);
}

void MainWindow::RenderTitlebar(float w) {
    constexpr float h = ThemeTokens::kTitlebarHeight;
    m_renderer.DrawLine(0.0f, h - 1.0f, w, h - 1.0f, ThemeTokens::kHairline);

    m_renderer.DrawCubeLogo(14.0f, 12.0f, 16.0f, ThemeTokens::kTextPrimary);
    const float brandW = std::ceil(m_renderer.MeasureTextWidth(L"Cadence", TextStyle::Title)) + 4.0f;
    const float slashX = 36.0f + brandW;
    m_renderer.DrawText(L"Cadence", {36.0f, 0.0f, slashX, h}, TextStyle::Title, ThemeTokens::kTextPrimary);
    m_renderer.DrawText(L"/", {slashX, 0.0f, slashX + 10.0f, h}, TextStyle::Body, ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);

    // Dynamically sized profile dropdown pill (leaves maximum titlebar drag room!)
    const float profLeft = slashX + 12.0f;
    const std::wstring profName = Utf8ToWide(m_profiles->ActiveProfileName());
    const float nameW = std::clamp(m_renderer.MeasureTextWidth(profName, TextStyle::Body), 36.0f, 120.0f);
    const D2D1_RECT_F profRc{profLeft, 9.0f, profLeft + nameW + 28.0f, 31.0f};
    m_profileBtnLeft  = profRc.left;
    m_profileBtnRight = profRc.right;

    if (IsHovered("profile_btn") || m_profileOpen) {
        m_renderer.FillRoundedRect(profRc, 5.0f, HexColor(0xFFFFFF, 0.06f));
    }
    m_renderer.DrawText(profName, {profRc.left + 6.0f, profRc.top, profRc.right - 20.0f, profRc.bottom},
                        TextStyle::Body,
                        (IsHovered("profile_btn") || m_profileOpen) ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary);
    m_renderer.DrawChevron(profRc.right - 11.0f, 20.5f, 7.0f, m_profileOpen, ThemeTokens::kTextSecondary);
    RegisterHit(profRc, "profile_btn", [this]() {
        m_profileOpen = !m_profileOpen;
        if (!m_profileOpen) m_namingProfile = false;
        RequestRender();
    });

    // Right controls: Pin | Min | Close
    const D2D1_RECT_F pinRc{w - 92.0f, 7.0f, w - 66.0f, 33.0f};
    if (IsHovered("pin")) m_renderer.FillRoundedRect(pinRc, 5.0f, HexColor(0xFFFFFF, 0.06f));
    const D2D1_COLOR_F pinCol = m_config.settings.alwaysOnTop
        ? ActiveAccent()
        : (IsHovered("pin") ? ThemeTokens::kTextPrimary : ThemeTokens::kTextMuted);
    m_renderer.DrawPinIcon((pinRc.left + pinRc.right) * 0.5f, (pinRc.top + pinRc.bottom) * 0.5f, pinCol);
    RegisterHit(pinRc, "pin", [this]() {
        m_config.settings.alwaysOnTop = !m_config.settings.alwaysOnTop;
        SetAlwaysOnTop(m_config.settings.alwaysOnTop);
        SaveConfigNow();
        RequestRender();
    });

    m_renderer.DrawLine(w - 60.0f, 14.0f, w - 60.0f, 26.0f, ThemeTokens::kHairline);

    const D2D1_RECT_F minRc{w - 54.0f, 8.0f, w - 30.0f, 32.0f};
    if (IsHovered("min")) m_renderer.FillRoundedRect(minRc, 5.0f, HexColor(0xFFFFFF, 0.06f));
    m_renderer.DrawMinimizeIcon((minRc.left + minRc.right) * 0.5f, (minRc.top + minRc.bottom) * 0.5f,
                                IsHovered("min") ? ThemeTokens::kTextPrimary : ThemeTokens::kTextMuted);
    RegisterHit(minRc, "min", [this]() {
        if (m_config.settings.minimizeToTray) HideToTray();
        else ShowWindow(m_hwnd, SW_MINIMIZE);
    });

    const D2D1_RECT_F closeRc{w - 28.0f, 8.0f, w - 6.0f, 32.0f};
    if (IsHovered("close")) m_renderer.FillRoundedRect(closeRc, 5.0f, HexColor(0xDC2626, 0.85f));
    m_renderer.DrawCloseIcon((closeRc.left + closeRc.right) * 0.5f, (closeRc.top + closeRc.bottom) * 0.5f,
                             IsHovered("close") ? ThemeTokens::kTextWhite : ThemeTokens::kTextMuted);
    RegisterHit(closeRc, "close", [this]() { SendMessageW(m_hwnd, WM_CLOSE, 0, 0); });
}

void MainWindow::RenderModeAndTabs(float w, float& y) {
    const float pad = ThemeTokens::kPadX;
    y += 14.0f;

    // 2-Way Tactile Neutral Segmented Mode Switcher (Mouse CPS | Keyboard KPS)
    const D2D1_RECT_F track{pad, y, w - pad, y + 32.0f};
    m_renderer.FillRoundedRect(track, 8.0f, ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(track, 8.0f, ThemeTokens::kHairline);

    const float halfW  = (track.right - track.left - 4.0f) * 0.5f;
    const float thumbL = track.left + 2.0f + m_modeAnimT * halfW;
    const D2D1_RECT_F thumb{thumbL, track.top + 2.0f, thumbL + halfW, track.bottom - 2.0f};
    m_renderer.FillRoundedRect(thumb, 6.0f, ThemeTokens::kBgElevated);
    m_renderer.DrawRoundedRect(thumb, 6.0f, ThemeTokens::kHairlineStrong);

    struct ModeTabDef { OperatingMode m; const wchar_t* label; const char* id; D2D1_COLOR_F dotCol; };
    const ModeTabDef mTabs[2] = {
        {OperatingMode::CPS, L"Mouse CPS",    "mode_cps", ThemeTokens::kAccentCps},
        {OperatingMode::KPS, L"Keyboard KPS", "mode_kps", ThemeTokens::kAccentKps}
    };

    for (int i = 0; i < 2; ++i) {
        const D2D1_RECT_F segRc{track.left + 2.0f + i * halfW, track.top, track.left + 2.0f + (i + 1) * halfW, track.bottom};
        const bool sel = (m_config.activeMode == mTabs[i].m);
        if (sel) {
            const float tw = m_renderer.MeasureTextWidth(mTabs[i].label, TextStyle::BodyMedium);
            const float cx = (segRc.left + segRc.right) * 0.5f;
            m_renderer.FillCircle(cx - tw * 0.5f - 7.0f, (segRc.top + segRc.bottom) * 0.5f, 2.8f, mTabs[i].dotCol);
        }
        m_renderer.DrawText(mTabs[i].label, {segRc.left + (sel ? 6.0f : 0.0f), segRc.top, segRc.right, segRc.bottom},
                            TextStyle::BodyMedium, sel ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary,
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(segRc, mTabs[i].id, [this, targetM = mTabs[i].m]() {
            if (m_config.activeMode != targetM) {
                if (m_editingRate) CommitRateEdit();
                m_config.activeMode = targetM;
                m_captureKind = CaptureKind::None;
                m_hotkeys.CancelCapture();
                SyncEngineAndHotkeys();
                UpdateWindowSizeForLayout();
                SaveConfigNow();
                StartAnimTimer();
            }
        });
    }

    y += 42.0f;

    struct TabDef { ViewTab t; const wchar_t* label; float x1; float x2; const char* id; };
    const TabDef tabs[] = {
        {ViewTab::Controls,  L"Controls",  pad,          pad + 56.0f,  "tab_ctrl"},
        {ViewTab::Telemetry, L"Telemetry", pad + 68.0f,  pad + 132.0f, "tab_stat"},
        {ViewTab::Settings,  L"Settings",  pad + 144.0f, pad + 198.0f, "tab_set"}
    };
    for (const auto& tb : tabs) {
        const D2D1_RECT_F tr{tb.x1, y, tb.x2, y + 22.0f};
        const bool active = (m_tab == tb.t);
        m_renderer.DrawText(tb.label, tr, active ? TextStyle::BodyMedium : TextStyle::Body,
                            active ? ThemeTokens::kTextPrimary : (IsHovered(tb.id) ? ThemeTokens::kTextSecondary : ThemeTokens::kTextMuted));
        RegisterHit(tr, tb.id, [this, t = tb.t]() {
            if (m_editingRate) CommitRateEdit();
            m_tab = t;
            UpdateWindowSizeForLayout();
            UpdateTelemetryTimer();
            RequestRender();
        });
    }

    // Pixel-perfect Status Indicator measured via DirectWrite
    const EngineState st = m_engine.State();
    D2D1_COLOR_F stDot = ThemeTokens::kTextMuted;
    D2D1_COLOR_F stTxt = ThemeTokens::kTextSecondary;
    std::wstring stLabel = L"Ready";
    if (st == EngineState::Active) {
        stDot = ThemeTokens::kEmerald; stTxt = ThemeTokens::kEmerald; stLabel = L"Running";
    } else if (st == EngineState::Armed) {
        stDot = ActiveAccent(); stTxt = ActiveAccent(); stLabel = L"Armed";
    } else if (st == EngineState::Paused) {
        stDot = ThemeTokens::kAmber; stTxt = ThemeTokens::kAmber; stLabel = L"Paused";
    }
    const float stW = m_renderer.MeasureTextWidth(stLabel, TextStyle::Body);
    m_renderer.DrawText(stLabel, {w - pad - stW - 4.0f, y, w - pad, y + 22.0f}, TextStyle::Body, stTxt, DWRITE_TEXT_ALIGNMENT_TRAILING);
    m_renderer.FillCircle(w - pad - stW - 9.0f, y + 11.5f, 3.0f, stDot);

    y += 28.0f;
    m_renderer.DrawLine(0.0f, y, w, y, ThemeTokens::kHairline);
}

void MainWindow::RenderTelemetryTab(float w, float& y) {
    const float pad = ThemeTokens::kPadX;
    const float colW = w / 3.0f;
    const bool isCps = (m_config.activeMode == OperatingMode::CPS);
    const std::wstring unit = isCps ? L"CPS" : L"KPS";

    const float statH = 62.0f;
    m_renderer.DrawText(std::format(L"Actual {}", unit), {pad, y + 10.0f, colW - 8.0f, y + 26.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);
    m_renderer.DrawText(std::format(L"{:.2f}", m_telemetry.actualRate), {pad, y + 28.0f, colW - 8.0f, y + 52.0f}, TextStyle::MonoStat, ThemeTokens::kTextPrimary);

    m_renderer.DrawLine(colW, y, colW, y + statH, ThemeTokens::kHairline);
    m_renderer.DrawText(L"P99 Error", {colW + 14.0f, y + 10.0f, colW * 2.0f - 8.0f, y + 26.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);
    m_renderer.DrawText(std::format(L"±{:.2f} ms", m_telemetry.p99ErrorMs), {colW + 14.0f, y + 28.0f, colW * 2.0f - 8.0f, y + 52.0f}, TextStyle::MonoStat, ThemeTokens::kTextPrimary);

    m_renderer.DrawLine(colW * 2.0f, y, colW * 2.0f, y + statH, ThemeTokens::kHairline);
    m_renderer.DrawText(L"Session", {colW * 2.0f + 14.0f, y + 10.0f, w - pad, y + 26.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);
    m_renderer.DrawText(std::format(L"{}", m_telemetry.sessionCount), {colW * 2.0f + 14.0f, y + 28.0f, w - pad, y + 52.0f}, TextStyle::MonoStat, ThemeTokens::kTextPrimary);

    y += statH;
    m_renderer.DrawLine(0.0f, y, w, y, ThemeTokens::kHairline);

    y += 12.0f;
    const double targetRate = isCps ? m_config.cps.rateCps : m_config.kps.rateKps;
    m_renderer.DrawText(std::format(L"{} STABILITY   σ {:.2f} ms · guard {:.0f}µs", unit, m_telemetry.jitterStdDevMs, m_engine.Timing().SpinGuard().count()),
                        {pad, y, w - pad - 82.0f, y + 16.0f}, TextStyle::MonoSmall, ThemeTokens::kTextMuted);
    m_renderer.DrawText(std::format(L"TARGET {:.1f}", targetRate),
                        {w - pad - 82.0f, y, w - pad, y + 16.0f}, TextStyle::MonoSmall, ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);

    y += 22.0f;
    const D2D1_RECT_F chartRc{pad, y, w - pad, y + 98.0f};
    std::span<const float> hist(m_telemetry.history.data(), m_telemetry.historyCount);
    m_renderer.DrawSparkline(chartRc, hist, static_cast<float>(targetRate), ActiveAccent());
    y += 110.0f;
}

void MainWindow::RenderSettingsTab(float w, float& y) {
    const float pad = ThemeTokens::kPadX;
    const D2D1_COLOR_F accent = ActiveAccent();

    DrawSettingToggle(w, y, L"Stop when Cadence is focused", L"Prevents self-clicking while window is open", &m_config.settings.stopOnSelfFocus, "set_self");
    DrawSettingToggle(w, y, L"Lock to target window", L"Only fires while selected app/game is focused", &m_config.settings.targetWindowLock, "set_tlock", true);

    if (m_config.settings.targetWindowLock) {
        const std::wstring targetTxt = m_config.settings.targetProcessName.empty()
            ? L"No target selected"
            : Utf8ToWide(m_config.settings.targetProcessName);
        m_renderer.DrawText(targetTxt, {pad, y, w - pad - 88.0f, y + 28.0f}, TextStyle::MonoSmall,
                            m_config.settings.targetProcessName.empty() ? ThemeTokens::kTextMuted : ThemeTokens::kEmerald);

        const bool pickingWin = (m_pickKind == PickKind::TargetWindow);
        const D2D1_RECT_F pickRc{w - pad - 82.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(pickRc, 5.0f, pickingWin ? accent : (IsHovered("pick_win") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed));
        m_renderer.DrawRoundedRect(pickRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(pickingWin ? L"Click app…" : L"Pick App", pickRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(pickRc, "pick_win", [this]() { BeginGlobalPick(PickKind::TargetWindow); });
        y += 28.0f;
        m_renderer.DrawLine(pad, y, w - pad, y, ThemeTokens::kHairlineSoft);
    }

    DrawSettingToggle(w, y, L"Hybrid QPC timer", L"Auto-calibrated waitable timer + µs spin", &m_config.settings.hybridQpcTimer, "set_qpc");
    DrawSettingToggle(w, y, L"Ignore Win11 throttling", L"Keeps sub-ms accuracy when minimized", &m_config.settings.disablePowerThrottle, "set_thr");
    DrawSettingToggle(w, y, L"MMCSS thread priority", L"Pro Audio scheduling while active", &m_config.settings.mmcssPriority, "set_mmcss");
    DrawSettingToggle(w, y, L"Minimize to system tray", L"Trims RAM & pauses UI when hidden", &m_config.settings.minimizeToTray, "set_tray");
    DrawSettingToggle(w, y, L"Check for updates", L"Notifies when a new GitHub release is available", &m_config.settings.checkForUpdates, "set_upd");

    // Emergency Kill Keybind
    const float rh = 42.0f;
    m_renderer.DrawText(L"Emergency kill hotkey", {pad, y, w - pad - 90.0f, y + rh}, TextStyle::Body, ThemeTokens::kTextPrimary);
    const D2D1_RECT_F pkRc{w - pad - 64.0f, y + 8.0f, w - pad, y + 34.0f};
    const bool recP = (m_captureKind == CaptureKind::PanicHotkey);
    m_renderer.DrawKeycap(pkRc, recP ? L"Press…" : FormatHotkey(m_config.settings.panicHotkey), recP, IsHovered("set_panic"), ThemeTokens::kDanger);
    RegisterHit(pkRc, "set_panic", [this]() { BeginKeyCapture(CaptureKind::PanicHotkey); });
    y += rh;
    m_renderer.DrawLine(pad, y, w - pad, y, ThemeTokens::kHairlineSoft);

    // Bottom Actions: Import | Export | Admin | Reset
    y += 10.0f;
    const D2D1_RECT_F impRc{pad,          y, pad + 82.0f,  y + 26.0f};
    const D2D1_RECT_F expRc{pad + 88.0f,  y, pad + 170.0f, y + 26.0f};
    const D2D1_RECT_F admRc{pad + 176.0f, y, w - pad - 68.0f, y + 26.0f};
    const D2D1_RECT_F rstRc{w - pad - 62.0f, y, w - pad, y + 26.0f};

    m_renderer.FillRoundedRect(impRc, 6.0f, IsHovered("set_imp") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(impRc, 6.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"Import", impRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(impRc, "set_imp", [this]() { ImportProfileDialog(); });

    m_renderer.FillRoundedRect(expRc, 6.0f, IsHovered("set_exp") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(expRc, 6.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"Export", expRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(expRc, "set_exp", [this]() { ExportActiveProfileDialog(); });

    m_renderer.FillRoundedRect(admRc, 6.0f, m_isElevated ? ThemeTokens::kBgCanvas : (IsHovered("set_adm") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed));
    m_renderer.DrawRoundedRect(admRc, 6.0f, m_isElevated ? ThemeTokens::kHairline : ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(m_isElevated ? L"Admin active" : L"Run as Admin", admRc, TextStyle::Body,
                        m_isElevated ? ThemeTokens::kEmerald : ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!m_isElevated) {
        RegisterHit(admRc, "set_adm", [this]() { RestartAsAdministrator(); });
    }

    m_renderer.FillRoundedRect(rstRc, 6.0f, IsHovered("set_rst") ? ThemeTokens::kDangerSurface : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(rstRc, 6.0f, IsHovered("set_rst") ? ThemeTokens::kDangerBorder : ThemeTokens::kHairline);
    m_renderer.DrawText(L"Reset", rstRc, TextStyle::Body, IsHovered("set_rst") ? ThemeTokens::kDangerText : ThemeTokens::kTextSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(rstRc, "set_rst", [this]() {
        m_engine.Stop();
        m_config = AppConfig{};
        m_profiles = std::make_unique<ProfileManager>(m_config);
        m_profileScroll = 0;
        SyncEngineAndHotkeys();
        SetAlwaysOnTop(false);
        UpdateWindowSizeForLayout();
        SaveConfigNow();
        StartAnimTimer();
        RequestRender();
    });
}

void MainWindow::RenderFooter(float w, float h) {
    const float pad = ThemeTokens::kPadX;
    const float footerH = 84.0f;
    const float top = h - footerH;

    m_renderer.FillRect({0.0f, top, w, h}, ThemeTokens::kBgFooter);
    m_renderer.DrawLine(0.0f, top, w, top, ThemeTokens::kHairline);

    const EngineState st = m_engine.State();
    const bool running   = (st == EngineState::Active || st == EngineState::Paused);
    const bool armed     = (st == EngineState::Armed);
    const bool isCps     = (m_config.activeMode == OperatingMode::CPS);

    const float pauseW = running ? 76.0f : 0.0f;
    const D2D1_RECT_F ctaRc{pad, top + 14.0f, w - pad - (pauseW > 0.0f ? pauseW + 8.0f : 0.0f), top + 50.0f};

    D2D1_COLOR_F ctaBg = ActiveAccent(IsHovered("cta"));
    std::wstring ctaText;
    if (running) {
        ctaBg   = IsHovered("cta") ? ThemeTokens::kDangerHover : ThemeTokens::kDanger;
        ctaText = L"Stop";
    } else if (armed) {
        ctaBg   = IsHovered("cta") ? ThemeTokens::kBgHover : ThemeTokens::kBgElevated;
        ctaText = L"Armed — switch to target window";
    } else {
        ctaText = isCps ? L"Start Clicking" : L"Start Pressing";
    }

    m_renderer.FillRoundedRect(ctaRc, ThemeTokens::kRadiusButton, ctaBg);
    m_renderer.DrawRoundedRect(ctaRc, ThemeTokens::kRadiusButton, armed ? ActiveAccent() : HexColor(0xFFFFFF, 0.14f));
    m_renderer.DrawLine(ctaRc.left + 8.0f, ctaRc.top + 1.0f, ctaRc.right - 8.0f, ctaRc.top + 1.0f, HexColor(0xFFFFFF, 0.15f));

    if (armed) {
        m_renderer.DrawText(ctaText, ctaRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        const HotkeyBinding& activeHk = isCps ? m_config.cps.hotkey : m_config.kps.hotkey;
        m_renderer.DrawText(std::format(L"{}   [{}]", ctaText, FormatHotkey(activeHk)), ctaRc, TextStyle::BodyMedium, ThemeTokens::kTextWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    RegisterHit(ctaRc, "cta", [this]() {
        if (m_editingRate) CommitRateEdit();
        if (m_engine.State() == EngineState::Idle) {
            m_engine.Start(true);
        } else {
            m_engine.Stop();
        }
        RequestRender();
    });

    if (running) {
        const D2D1_RECT_F pRc{ctaRc.right + 8.0f, ctaRc.top, w - pad, ctaRc.bottom};
        m_renderer.FillRoundedRect(pRc, ThemeTokens::kRadiusButton, IsHovered("pause_btn") ? ThemeTokens::kBgHover : ThemeTokens::kBgElevated);
        m_renderer.DrawRoundedRect(pRc, ThemeTokens::kRadiusButton, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(st == EngineState::Paused ? L"Resume" : L"Pause", pRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(pRc, "pause_btn", [this]() {
            m_engine.TogglePause();
            RequestRender();
        });
    }

    const float metaY   = top + 58.0f;
    const float centerX = w * 0.5f;
    m_renderer.DrawText(std::format(L"{:.2f}% CPU · {:.1f} MB", m_telemetry.cpuPercent, m_telemetry.memoryMb),
                        {pad, metaY, centerX - 32.0f, metaY + 18.0f}, TextStyle::MonoSmall, ThemeTokens::kTextMuted);

    // Installed version, bottom center
    static const std::wstring kVersionLabel = L"v" + Utf8ToWide(CADENCE_VERSION_STR);
    m_renderer.DrawText(kVersionLabel, {centerX - 30.0f, metaY, centerX + 30.0f, metaY + 18.0f},
                        TextStyle::MonoSmall, ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);

    const D2D1_RECT_F killRc{w - pad - 90.0f, metaY, w - pad, metaY + 18.0f};
    m_renderer.DrawText(std::format(L"Kill {}", FormatHotkey(m_config.settings.panicHotkey)),
                        killRc, TextStyle::MonoSmall,
                        IsHovered("kill_btn") ? ThemeTokens::kDangerHover : ThemeTokens::kTextMuted,
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
    RegisterHit(killRc, "kill_btn", [this]() {
        m_engine.PanicStop();
        ShowToast(L"Halted — all inputs released", true);
    });
}

void MainWindow::RenderToast(float w, float h) {
    const D2D1_RECT_F rc{16.0f, h - 118.0f, w - 16.0f, h - 88.0f};
    m_renderer.FillRoundedRect(rc, 7.0f, m_toastDanger ? ThemeTokens::kDangerSurface : ThemeTokens::kBgElevated);
    m_renderer.DrawRoundedRect(rc, 7.0f, m_toastDanger ? ThemeTokens::kDangerBorder : ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(m_toastText, {rc.left + 12.0f, rc.top, rc.right - 12.0f, rc.bottom},
                        TextStyle::Body, m_toastDanger ? ThemeTokens::kDangerText : ThemeTokens::kTextPrimary);
}

// =============================================================================
// HIT REGISTRATION & INTERACTION
// =============================================================================

} // namespace cadence::ui
