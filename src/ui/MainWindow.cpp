#include "ui/MainWindow.hpp"
#include "ui/UiHelpers.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

// Window lifecycle, tray, persistence, timers and the window procedure.
// Rendering, input, profiles and the updater live in the other MainWindow*.cpp files.

namespace cadence::ui {

namespace {

constexpr wchar_t kWindowClassName[] = L"CadenceMainWindowClass";

} // namespace

MainWindow::MainWindow() {
    m_isElevated = IsProcessElevated();

    const auto configPath = ConfigManager::DefaultConfigPath();
    if (const auto loaded = ConfigManager::LoadFromFile(configPath, m_config); !loaded) {
        if (loaded.error() != ConfigLoadError::NotFound) {
            // Keep the unreadable file: the next save would otherwise overwrite the user's settings with defaults
            const auto backup = ConfigManager::BackupFile(configPath);
            m_startupNotice = std::wstring(DescribeConfigLoadError(loaded.error())) +
                (backup ? L" — backup saved as config.json.bak" : L" — defaults loaded");
        }
    } else if (m_config.loadedVersion > kConfigVersion) {
        // Written by a newer Cadence: unknown settings would be lost on the next save
        if (ConfigManager::BackupFile(configPath)) {
            m_startupNotice = L"Settings from a newer Cadence — backup saved as config.json.bak";
        }
    }

    m_profiles = std::make_unique<ProfileManager>(m_config);
    EnsureUniqueHotkeys();
    m_modeAnimT   = static_cast<float>(m_config.activeMode);
    m_jitterAnimT = (m_config.activeMode == OperatingMode::CPS ? m_config.cps.jitterEnabled : m_config.kps.jitterEnabled) ? 1.0f : 0.0f;
}

void MainWindow::EnsureUniqueHotkeys() {
    // Guarantee Emergency Kill hotkey never collides with Mouse (CPS) or Keyboard (KPS) trigger
    auto collides = [&](const HotkeyBinding& hk) {
        return hk == m_config.cps.hotkey || hk == m_config.kps.hotkey;
    };
    if (collides(m_config.settings.panicHotkey)) {
        const uint16_t candidates[] = {VK_F12, VK_F11, VK_F10, VK_F9, VK_F8, VK_PAUSE, VK_END};
        for (uint16_t vk : candidates) {
            HotkeyBinding cand{HotkeySource::Keyboard, vk};
            if (!collides(cand)) {
                m_config.settings.panicHotkey = cand;
                break;
            }
        }
    }
}

void MainWindow::ShowToast(std::wstring text, bool isDanger) {
    m_toastText   = std::move(text);
    m_toastDanger = isDanger;
    m_toastMs     = 2200.0f;
    StartAnimTimer();
    RequestRender();
}

MainWindow::~MainWindow() {
    StopUpdateWorkers();
    RemoveTrayIcon();
    m_hotkeys.Shutdown();
    m_engine.Stop();
    SaveConfigNow();
    m_renderer.Shutdown();
    if (m_iconBig)         DestroyIcon(m_iconBig);
    if (m_iconSmallIdle)   DestroyIcon(m_iconSmallIdle);
    if (m_iconSmallActive) DestroyIcon(m_iconSmallActive);
    if (m_iconSmallArmed)  DestroyIcon(m_iconSmallArmed);
}

bool MainWindow::IsProcessElevated() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elev{};
    DWORD cb = sizeof(elev);
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, cb, &cb) != FALSE;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

void MainWindow::RestartAsAdministrator() {
    const auto exePath = UpdateChecker::CurrentExePath();
    if (exePath.empty()) return;

    SaveConfigNow();
    m_engine.Stop();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.hwnd         = m_hwnd; // UAC prompt belongs to the Cadence window
    sei.lpVerb       = L"runas";
    sei.lpFile       = exePath.c_str();
    // This instance still holds the single-instance mutex while it shuts down; tell the elevated
    // instance to wait for it instead of exiting immediately as a duplicate.
    sei.lpParameters = L"--relaunch";
    sei.nShow        = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        DestroyWindow(m_hwnd);
    }
}

HICON MainWindow::CreateAppIcon(int sizePx, uint32_t coreArgb) {
    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = sizePx;
    bi.bV5Height      = -sizePx;
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    if (!hdc) return LoadIconW(nullptr, IDI_APPLICATION);
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !bits) return LoadIconW(nullptr, IDI_APPLICATION);

    auto* px = static_cast<uint32_t*>(bits);
    const float half = static_cast<float>(sizePx) * 0.5f;
    const float rad  = static_cast<float>(sizePx) * 0.24f;

    for (int y = 0; y < sizePx; ++y) {
        for (int x = 0; x < sizePx; ++x) {
            const float fx = static_cast<float>(x) + 0.5f;
            const float fy = static_cast<float>(y) + 0.5f;
            const float dx = std::max(0.0f, std::abs(fx - half) - (half - rad));
            const float dy = std::max(0.0f, std::abs(fy - half) - (half - rad));
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist <= rad) {
                const float cd = std::abs(fx - half) + std::abs(fy - half);
                px[y * sizePx + x] = (cd < static_cast<float>(sizePx) * 0.29f) ? coreArgb : 0xFF14141A;
            } else {
                px[y * sizePx + x] = 0x00000000;
            }
        }
    }

    const size_t maskStride = ((static_cast<size_t>(sizePx) + 15u) / 16u) * 2u;
    std::vector<BYTE> maskBits(maskStride * static_cast<size_t>(sizePx), 0);
    HBITMAP hMask = CreateBitmap(sizePx, sizePx, 1, 1, maskBits.data());
    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmColor = hColor;
    ii.hbmMask  = hMask;
    HICON icon  = CreateIconIndirect(&ii);
    DeleteObject(hColor);
    DeleteObject(hMask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

bool MainWindow::Create(HINSTANCE hInstance, bool previewUpdate) {
    m_hInstance       = hInstance;
    m_iconBig         = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR));
    if (!m_iconBig)   m_iconBig = CreateAppIcon(32, 0xFF7C5CFC);
    m_iconSmallIdle   = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (!m_iconSmallIdle) m_iconSmallIdle = CreateAppIcon(16, 0xFF7C5CFC);
    m_iconSmallActive = CreateAppIcon(16, 0xFF10B981);
    m_iconSmallArmed  = CreateAppIcon(16, 0xFFF59E0B);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = m_iconBig;
    wc.hIconSm       = m_iconSmallIdle;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    m_heightDip = ComputeDesiredHeightDip();

    const int initW = static_cast<int>(m_widthDip);
    const int initH = static_cast<int>(m_heightDip);
    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClassName,
        L"Cadence",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU,
        0, 0, initW, initH,
        nullptr, nullptr, hInstance, this
    );
    if (!m_hwnd) {
        return false;
    }

    m_dpi = static_cast<float>(GetDpiForWindow(m_hwnd));
    ApplyWindowChrome();
    // Center using actual DPI-scaled pixel dimensions on the primary work area
    const int scaledW = DipToPx(m_widthDip);
    const int scaledH = DipToPx(m_heightDip);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int workW = workArea.right - workArea.left;
    const int workH = workArea.bottom - workArea.top;
    const int posX  = workArea.left + std::max(24, (workW - scaledW) / 2);
    const int posY  = workArea.top  + std::max(24, (workH - scaledH) / 2);
    SetWindowPos(m_hwnd, nullptr, posX, posY, scaledW, scaledH, SWP_NOZORDER | SWP_NOACTIVATE);

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    if (!m_renderer.Initialize(m_hwnd,
                               static_cast<uint32_t>(std::max(1L, rc.right - rc.left)),
                               static_cast<uint32_t>(std::max(1L, rc.bottom - rc.top)),
                               m_dpi)) {
        return false;
    }

    m_engine.SetAppWindow(m_hwnd);
    m_engine.SetStateChangedCallback([this](EngineState) {
        if (m_hwnd) {
            PostMessageW(m_hwnd, WM_APP_STATE_CHANGED, 0, 0);
        }
    });

    HotkeyCallbacks cbs{};
    cbs.onCpsPress   = [this]() {
        if (m_editingRate || m_namingProfile || m_updateModalOpen || m_systemDialogOpen) return;
        m_engine.HandleHotkeyPress(OperatingMode::CPS);
    };
    cbs.onCpsRelease = [this]() {
        if (m_editingRate || m_namingProfile || m_updateModalOpen || m_systemDialogOpen) return;
        m_engine.HandleHotkeyRelease(OperatingMode::CPS);
    };
    cbs.onKpsPress   = [this]() {
        if (m_editingRate || m_namingProfile || m_updateModalOpen || m_systemDialogOpen) return;
        m_engine.HandleHotkeyPress(OperatingMode::KPS);
    };
    cbs.onKpsRelease = [this]() {
        if (m_editingRate || m_namingProfile || m_updateModalOpen || m_systemDialogOpen) return;
        m_engine.HandleHotkeyRelease(OperatingMode::KPS);
    };
    cbs.onPanicStop  = [this]() {
        m_engine.PanicStop();
        ShowToast(L"Halted — all inputs released", true);
    };
    m_hotkeys.Initialize(m_hwnd, std::move(cbs));

    SyncEngineAndHotkeys();
    SetAlwaysOnTop(m_config.settings.alwaysOnTop);
    m_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    InitTrayIcon();

    m_updateShared = std::make_shared<UpdateShared>();
    m_updateShared->hwnd = m_hwnd;

    if (previewUpdate) {
        ReleaseInfo demo{};
        demo.tagName     = "v1.1.0";
        demo.version     = {1, 1, 0};
        demo.releaseUrl  = "https://github.com/solidvhdx/cadence/releases/latest";
        demo.releaseNotes = "## Preview\n- Release notes appear here\n- **Markdown** markers are stripped\n- Long notes are shortened";
        m_availableUpdate   = std::move(demo);
        m_updateModalOpen   = true;
        m_updatePreviewOnly = true;
    } else if (m_config.settings.checkForUpdates) {
        StartUpdateCheckAsync();
    }

    m_telemetry = m_engine.GetTelemetry();
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    if (!m_startupNotice.empty()) {
        ShowToast(std::exchange(m_startupNotice, {}), true);
    }
    UpdateTelemetryTimer();
    Render();
    return true;
}

void MainWindow::ApplyWindowChrome() {
    if (!m_hwnd) return;
    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    const COLORREF borderCol = RGB(36, 36, 44);
    DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &borderCol, sizeof(borderCol));
}

float MainWindow::ComputeDesiredHeightDip() const noexcept {
    // Titlebar(40) + Mode/Tabs(84) + Footer(84) = 208 base
    constexpr float base = 208.0f;
    if (m_tab == ViewTab::Telemetry) {
        return base + 206.0f;
    }
    if (m_tab == ViewTab::Settings) {
        // 7 toggles (308) + target lock subrow (28 if on) + panic row (42) + actions row (44)
        return base + 394.0f + (m_config.settings.targetWindowLock ? 28.0f : 0.0f);
    }

    const bool isCps    = (m_config.activeMode == OperatingMode::CPS);
    const bool jitterOn = isCps ? m_config.cps.jitterEnabled : m_config.kps.jitterEnabled;
    float h = base + 256.0f + (jitterOn ? 24.0f : 0.0f);
    if (m_advancedOpen) {
        h += 122.0f;
        if (isCps && m_config.cps.pattern == ClickPattern::CustomBurst) h += 56.0f; // count + cooldown
        if (isCps && m_config.cps.cursorMode == CursorMode::LockedPoint) h += 56.0f; // coords + micro-wiggle
        if ((isCps && m_config.cps.limitMode != RepeatLimitMode::Unlimited) ||
            (!isCps && m_config.kps.limitMode != RepeatLimitMode::Unlimited)) {
            h += 28.0f;
        }
    }
    return h;
}

void MainWindow::UpdateWindowSizeForLayout(bool force) {
    if (!m_hwnd) return;
    const float desiredH = ComputeDesiredHeightDip();
    if (!force && std::abs(desiredH - m_heightDip) < 0.5f) {
        return;
    }
    m_heightDip = desiredH;
    const int pxW = DipToPx(m_widthDip);
    const int pxH = DipToPx(m_heightDip);
    SetWindowPos(m_hwnd, nullptr, 0, 0, pxW, pxH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void MainWindow::SetAlwaysOnTop(bool onTop) {
    if (!m_hwnd) return;
    SetWindowPos(m_hwnd, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MainWindow::InitTrayIcon() {
    if (!m_hwnd || m_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;
    nid.hIcon            = m_iconSmallIdle;
    wcscpy_s(nid.szTip, L"Cadence — Ready");
    m_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void MainWindow::UpdateTrayIconState() {
    if (!m_hwnd || !m_trayAdded) return;
    const EngineState st = m_engine.State();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;

    if (st == EngineState::Active) {
        nid.hIcon = m_iconSmallActive;
        wcscpy_s(nid.szTip, L"Cadence — Running");
    } else if (st == EngineState::Armed || st == EngineState::Paused) {
        nid.hIcon = m_iconSmallArmed;
        wcscpy_s(nid.szTip, st == EngineState::Armed ? L"Cadence — Armed" : L"Cadence — Paused");
    } else {
        nid.hIcon = m_iconSmallIdle;
        wcscpy_s(nid.szTip, L"Cadence — Ready");
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void MainWindow::RemoveTrayIcon() {
    if (!m_hwnd || !m_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_trayAdded = false;
}

void MainWindow::HideToTray() {
    if (!m_hwnd) return;
    m_inTray = true;
    KillTimer(m_hwnd, kTimerAnim);
    KillTimer(m_hwnd, kTimerTelemetry);
    m_animActive = false;
    ShowWindow(m_hwnd, SW_HIDE);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void MainWindow::ShowFromTray() {
    if (!m_hwnd) return;
    m_inTray = false;
    ShowWindow(m_hwnd, SW_RESTORE);
    SetForegroundWindow(m_hwnd);
    UpdateTelemetryTimer();
    if (m_updateDownloading) StartAnimTimer();
    RequestRender();
}

void MainWindow::ShowTrayContextMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1001, L"Open Cadence");
    AppendMenuW(menu, MF_STRING, 1002, L"Emergency Stop");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1003, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 1001) ShowFromTray();
    else if (cmd == 1002) m_engine.PanicStop();
    else if (cmd == 1003) DestroyWindow(m_hwnd);
}

void MainWindow::SyncEngineAndHotkeys() {
    EnsureUniqueHotkeys();
    m_engine.UpdateConfig(m_config.activeMode, m_config.cps, m_config.kps, m_config.settings);
    m_hotkeys.SetBindings(m_config.cps.hotkey, m_config.kps.hotkey, m_config.settings.panicHotkey);
}

void MainWindow::SaveConfigNow() {
    if (ConfigManager::SaveToFile(ConfigManager::DefaultConfigPath(), m_config)) {
        m_saveErrorShown = false;
        return;
    }
    if (!m_saveErrorShown && m_hwnd) {
        m_saveErrorShown = true; // Tell the user once per failure streak, not on every slider tick
        ShowToast(L"Could not save settings (%APPDATA%\\Cadence)", true);
    }
}

void MainWindow::RequestRender() {
    if (m_hwnd && !m_inTray && IsWindowVisible(m_hwnd)) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void MainWindow::StartAnimTimer() {
    if (m_hwnd && !m_inTray && !m_animActive) {
        m_animActive = true;
        SetTimer(m_hwnd, kTimerAnim, 16, nullptr);
    }
}

void MainWindow::UpdateTelemetryTimer() {
    if (!m_hwnd) return;
    KillTimer(m_hwnd, kTimerTelemetry);
    if (m_inTray) return;

    const bool fast = (m_engine.State() == EngineState::Active) || (m_tab == ViewTab::Telemetry);
    SetTimer(m_hwnd, kTimerTelemetry, fast ? 140 : 1000, nullptr);
}

void MainWindow::StepAnimations() {
    bool stillMoving = false;
    const float targetMode = static_cast<float>(m_config.activeMode);
    if (m_modeAnimT != targetMode) {
        m_modeAnimT = MoveToward(m_modeAnimT, targetMode, 0.20f);
        if (m_modeAnimT != targetMode) stillMoving = true;
    }

    const bool jitterOn = (m_config.activeMode == OperatingMode::CPS) ? m_config.cps.jitterEnabled : m_config.kps.jitterEnabled;
    const float targetJitter = jitterOn ? 1.0f : 0.0f;
    if (m_jitterAnimT != targetJitter) {
        m_jitterAnimT = MoveToward(m_jitterAnimT, targetJitter, 0.18f);
        if (m_jitterAnimT != targetJitter) stillMoving = true;
    }

    if (m_toastMs > 0.0f) {
        m_toastMs = std::max(0.0f, m_toastMs - 16.0f);
        if (m_toastMs > 0.0f) stillMoving = true;
    }

    if (m_updateDownloading && m_updateShared) {
        m_updateAnimPhase += 0.022f;
        if (m_updateAnimPhase >= 1.0f) m_updateAnimPhase -= 1.0f;

        const auto st = static_cast<UpdateStage>(m_updateShared->stage.load(std::memory_order_acquire));
        const uint64_t dl  = m_updateShared->bytesDownloaded.load(std::memory_order_relaxed);
        const uint64_t tot = m_updateShared->bytesTotal.load(std::memory_order_relaxed);
        float targetProg = m_updateProgVisual;
        if (st == UpdateStage::Installing) {
            targetProg = 1.0f;
        } else if (st == UpdateStage::Downloading && tot > 0) {
            targetProg = std::clamp(static_cast<float>(static_cast<double>(dl) / static_cast<double>(tot)), 0.0f, 1.0f);
        }
        m_updateProgVisual += (targetProg - m_updateProgVisual) * 0.25f;
        if (std::abs(targetProg - m_updateProgVisual) < 0.002f) {
            m_updateProgVisual = targetProg;
        }
        stillMoving = true;
    }

    RequestRender();

    if (!stillMoving && m_animActive) {
        KillTimer(m_hwnd, kTimerAnim);
        m_animActive = false;
    }
}

D2D1_COLOR_F MainWindow::ActiveAccent(bool hover) const noexcept {
    if (m_config.activeMode == OperatingMode::CPS) {
        return hover ? ThemeTokens::kAccentCpsHover : ThemeTokens::kAccentCps;
    }
    return hover ? ThemeTokens::kAccentKpsHover : ThemeTokens::kAccentKps;
}

int MainWindow::RunMessageLoop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->WndProc(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    // Recover system tray icon automatically if explorer.exe restarts
    if (m_msgTaskbarCreated != 0 && msg == m_msgTaskbarCreated) {
        m_trayAdded = false;
        InitTrayIcon();
        UpdateTrayIconState();
        return 0;
    }

    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) return 0;
            break;

        case WM_NCLBUTTONDBLCLK:
            return 0; // Fixed-width layout: never maximize on titlebar double-click

        case WM_SYSCOMMAND: {
            const UINT cmd = static_cast<UINT>(wParam & 0xFFF0);
            if (cmd == SC_MAXIMIZE || cmd == SC_SIZE) {
                return 0; // Prevent Aero Snap maximize or manual border resizing
            }
            break;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const int pxW = DipToPx(m_widthDip);
            const int pxH = DipToPx(m_heightDip);
            mmi->ptMinTrackSize = {pxW, pxH};
            mmi->ptMaxTrackSize = {pxW, pxH};
            return 0;
        }

        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(m_hwnd, &pt);
            const float dipX = PxToDip(pt.x);
            const float dipY = PxToDip(pt.y);
            // Dynamic titlebar drag zone (full left/middle titlebar when update modal is open)
            const float dragLeft  = m_updateModalOpen ? 0.0f : (m_profileBtnRight + 4.0f);
            const float dragRight = m_updateModalOpen ? (m_widthDip - 58.0f) : (m_widthDip - 96.0f);
            if (dipY >= 0.0f && dipY <= ThemeTokens::kTitlebarHeight &&
                dipX >= dragLeft && dipX <= dragRight) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_DPICHANGED: {
            m_dpi = static_cast<float>(HIWORD(wParam));
            m_heightDip = ComputeDesiredHeightDip();
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            const int pxW = DipToPx(m_widthDip);
            const int pxH = DipToPx(m_heightDip);
            SetWindowPos(m_hwnd, nullptr,
                         suggested ? suggested->left : 0,
                         suggested ? suggested->top : 0,
                         pxW, pxH,
                         (suggested ? 0u : SWP_NOMOVE) | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            m_renderer.Resize(static_cast<uint32_t>(std::max(1L, rc.right - rc.left)),
                              static_cast<uint32_t>(std::max(1L, rc.bottom - rc.top)),
                              m_dpi);
            RequestRender();
            return 0;
        }

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) {
                if (m_config.settings.minimizeToTray) {
                    HideToTray();
                }
                return 0;
            }
            const uint32_t w = LOWORD(lParam);
            const uint32_t h = HIWORD(lParam);
            if (w > 0 && h > 0) {
                m_renderer.Resize(w, h, m_dpi);
                RequestRender();
            }
            return 0;
        }

        case WM_ACTIVATE: {
            const bool active = (LOWORD(wParam) != WA_INACTIVE);
            m_engine.NotifyAppFocusChanged(active);
            if (!active) {
                if (m_profileOpen)   { m_profileOpen = false; m_namingProfile = false; }
                if (m_editingRate)   { CommitRateEdit(); }
                RequestRender();
            }
            return 0;
        }

        case WM_INPUT:
            m_hotkeys.ProcessRawInput(lParam);
            return 0;

        case WM_HOTKEY:
            m_hotkeys.ProcessWmHotkey(wParam);
            return 0;

        case WM_APP_STATE_CHANGED:
            // Hold the global kill-key registration only while something can actually be stopped
            m_hotkeys.SetPanicFallbackEnabled(m_engine.State() != EngineState::Idle);
            UpdateTrayIconState();
            UpdateTelemetryTimer();
            RequestRender();
            return 0;

        case WM_APP_TRAY_ICON:
            if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                ShowFromTray();
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayContextMenu();
            }
            return 0;

        case WM_APP_UPDATE_AVAILABLE: {
            if (m_updateShared && m_config.settings.checkForUpdates) {
                std::scoped_lock lock(m_updateShared->mtx);
                if (m_updateShared->found) {
                    m_availableUpdate = m_updateShared->found;
                    m_updateModalOpen = true;
                    m_profileOpen     = false;
                    RequestRender();
                }
            }
            return 0;
        }

        case WM_APP_UPDATE_DONE: {
            UpdateOutcome outcome = UpdateOutcome::None;
            std::optional<UpdateError> error;
            uint64_t doneAttempt  = 0;
            if (m_updateShared) {
                std::scoped_lock lock(m_updateShared->mtx);
                outcome     = m_updateShared->outcome;
                error       = m_updateShared->error;
                doneAttempt = m_updateShared->outcomeAttemptId;
            }
            if (doneAttempt != m_updateAttemptId) {
                return 0; // Stale result from a previously cancelled attempt
            }
            m_updateDownloading = false;
            m_updateProgVisual  = 0.0f;
            if (m_updateShared) {
                m_updateShared->stage.store(static_cast<uint8_t>(UpdateStage::Idle), std::memory_order_release);
            }

            if (outcome == UpdateOutcome::Applied) {
                DestroyWindow(m_hwnd);
                return 0;
            }
            if (outcome == UpdateOutcome::Failed) {
                const UpdateError reason = error.value_or(UpdateError::Network);
                if (reason == UpdateError::ReplaceFailed) {
                    // Exe folder is read-only (e.g. Program Files): fall back to a manual download
                    if (m_availableUpdate && !m_availableUpdate->releaseUrl.empty()) {
                        const std::wstring url = Utf8ToWide(m_availableUpdate->releaseUrl);
                        ShellExecuteW(m_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    m_updateModalOpen = false;
                    ShowToast(std::wstring(DescribeUpdateError(reason)) + L" — opened GitHub release", false);
                } else {
                    ShowToast(std::wstring(DescribeUpdateError(reason)), true);
                }
            }
            RequestRender();
            return 0;
        }

        case WM_TIMER:
            if (wParam == kTimerAnim) {
                StepAnimations();
            } else if (wParam == kTimerTelemetry) {
                const auto prev = m_telemetry;
                m_telemetry = m_engine.GetTelemetry();
                const bool running = (m_engine.State() == EngineState::Active);
                const bool changed = std::abs(m_telemetry.cpuPercent - prev.cpuPercent) >= 0.05 ||
                                     std::abs(m_telemetry.memoryMb - prev.memoryMb) >= 0.1 ||
                                     m_telemetry.sessionCount != prev.sessionCount;
                if (running || m_tab == ViewTab::Telemetry || changed) {
                    RequestRender();
                }
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && (m_hoverId == "rate_edit" || m_editingRate || m_namingProfile)) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            break;

        case WM_MOUSEMOVE: {
            if (!m_trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
                m_trackingMouse = TrackMouseEvent(&tme) != FALSE;
            }
            HandleMouseMove(PxToDip(GET_X_LPARAM(lParam)), PxToDip(GET_Y_LPARAM(lParam)));
            return 0;
        }

        case WM_MOUSELEAVE:
            m_trackingMouse = false;
            if (!m_hoverId.empty()) {
                m_hoverId.clear();
                RequestRender();
            }
            return 0;

        case WM_LBUTTONDOWN:
            HandleLButtonDown(PxToDip(GET_X_LPARAM(lParam)), PxToDip(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_LBUTTONUP:
            HandleLButtonUp();
            return 0;

        case WM_CAPTURECHANGED:
            if (m_dragSlider != DragSlider::None) {
                m_dragSlider = DragSlider::None;
                SaveConfigNow();
                RequestRender();
            }
            return 0;

        case WM_MOUSEWHEEL:
            HandleMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;

        case WM_CHAR:
            HandleCharInput(static_cast<wchar_t>(wParam));
            return 0;

        case WM_KEYDOWN:
            HandleKeyDown(wParam);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(m_hwnd, &ps);
            Render(hdc);
            EndPaint(m_hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            if (m_updateShared &&
                static_cast<UpdateStage>(m_updateShared->stage.load(std::memory_order_acquire)) == UpdateStage::Installing) {
                return 0; // Allow the in-flight executable swap & relaunch to complete cleanly
            }
            DestroyWindow(m_hwnd);
            return 0;

        case WM_DESTROY:
            StopUpdateWorkers();
            m_engine.SetStateChangedCallback(nullptr);
            m_engine.SetAppWindow(nullptr);
            KillTimer(m_hwnd, kTimerAnim);
            KillTimer(m_hwnd, kTimerTelemetry);
            RemoveTrayIcon();
            m_hotkeys.Shutdown();
            m_engine.Stop();
            SaveConfigNow();
            m_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

// =============================================================================
// RENDERING (PRECISION CRAFT — ULTRA-LOW-RAM DIRECT2D + DIRECTWRITE)
// =============================================================================

} // namespace cadence::ui
