#include "ui/MainWindow.hpp"

#include <commdlg.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace cadence::ui {

namespace {

constexpr wchar_t kWindowClassName[] = L"CadenceMainWindowClass";

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(std::wstring_view ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool PtInRectF(const D2D1_RECT_F& r, float x, float y) noexcept {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

float MoveToward(float cur, float target, float step) noexcept {
    if (std::abs(target - cur) <= step) return target;
    return cur + (target > cur ? step : -step);
}

constexpr double kMaxRate = 500.0;

// Piecewise slider curve: 0..50% maps 0.5..50.0 (fine control), 50..100% maps 50.0..500.0
float RateToSliderT(double rate) noexcept {
    rate = std::clamp(rate, 0.5, kMaxRate);
    if (rate <= 50.0) {
        return static_cast<float>((rate - 0.5) / 49.5 * 0.5);
    }
    return static_cast<float>(0.5 + (rate - 50.0) / (kMaxRate - 50.0) * 0.5);
}

double SliderTToRate(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= 0.5f) {
        const double v = 0.5 + (static_cast<double>(t) / 0.5) * 49.5;
        return std::round(v * 2.0) / 2.0; // 0.5 step
    }
    const double v = 50.0 + ((static_cast<double>(t) - 0.5) / 0.5) * (kMaxRate - 50.0);
    return std::round(v); // 1.0 step above 50
}

std::wstring PatternLabel(ClickPattern p) {
    switch (p) {
        case ClickPattern::Single:      return L"Single";
        case ClickPattern::Double:      return L"Double";
        case ClickPattern::TripleBurst: return L"Triple burst";
        case ClickPattern::CustomBurst: return L"Custom burst";
    }
    return L"Single";
}

std::wstring LimitLabel(RepeatLimitMode m) {
    switch (m) {
        case RepeatLimitMode::Unlimited: return L"Unlimited";
        case RepeatLimitMode::Count:     return L"Count";
        case RepeatLimitMode::Duration:  return L"Duration";
    }
    return L"Unlimited";
}

} // namespace

MainWindow::MainWindow() {
    m_isElevated = IsProcessElevated();
    ConfigManager::LoadFromFile(ConfigManager::DefaultConfigPath(), m_config);
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
    if (m_updateShared) {
        std::scoped_lock lock(m_updateShared->mtx);
        m_updateShared->hwnd = nullptr;
    }
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
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return;

    SaveConfigNow();
    m_engine.Stop();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.nShow  = SW_SHOWNORMAL;
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
        if (m_editingRate || m_namingProfile || m_updateModalOpen) return;
        m_engine.HandleHotkeyPress(OperatingMode::CPS);
    };
    cbs.onCpsRelease = [this]() {
        if (m_editingRate || m_namingProfile || m_updateModalOpen) return;
        m_engine.HandleHotkeyRelease(OperatingMode::CPS);
    };
    cbs.onKpsPress   = [this]() {
        if (m_editingRate || m_namingProfile || m_updateModalOpen) return;
        m_engine.HandleHotkeyPress(OperatingMode::KPS);
    };
    cbs.onKpsRelease = [this]() {
        if (m_editingRate || m_namingProfile || m_updateModalOpen) return;
        m_engine.HandleHotkeyRelease(OperatingMode::KPS);
    };
    cbs.onPanicStop  = [this]() {
        m_engine.PanicStop();
        ShowToast(L"Halted — all inputs released", true);
    };
    m_hotkeys.Initialize(m_hwnd, std::move(cbs));

    SyncEngineAndHotkeys();
    SetAlwaysOnTop(m_config.settings.alwaysOnTop);
    InitTrayIcon();

    m_updateShared = std::make_shared<UpdateShared>();
    m_updateShared->hwnd = m_hwnd;

    if (previewUpdate) {
        ReleaseInfo demo{};
        demo.tagName     = "v1.1.0";
        demo.version     = {1, 1, 0};
        demo.releaseUrl  = "https://github.com/solidvhdx/cadence/releases/latest";
        m_availableUpdate   = std::move(demo);
        m_updateModalOpen   = true;
        m_updatePreviewOnly = true;
    } else if (m_config.settings.checkForUpdates) {
        StartUpdateCheckAsync();
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
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
    ConfigManager::SaveToFile(ConfigManager::DefaultConfigPath(), m_config);
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
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) return 0;
            break;

        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(m_hwnd, &pt);
            const float dipX = PxToDip(pt.x);
            const float dipY = PxToDip(pt.y);
            // Dynamic titlebar drag zone: everything between the profile pill and the Pin icon
            if (dipY >= 0.0f && dipY <= ThemeTokens::kTitlebarHeight &&
                dipX >= (m_profileBtnRight + 4.0f) && dipX <= (m_widthDip - 96.0f)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_DPICHANGED: {
            m_dpi = static_cast<float>(HIWORD(wParam));
            UpdateWindowSizeForLayout(true);
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
            bool ok = false;
            std::filesystem::path dlPath;
            if (m_updateShared) {
                std::scoped_lock lock(m_updateShared->mtx);
                ok     = m_updateShared->downloadOk;
                dlPath = m_updateShared->downloadedPath;
            }
            m_updateDownloading = false;
            if (ok && !dlPath.empty()) {
                m_engine.Stop();
                SaveConfigNow();
                if (UpdateChecker::ApplyUpdateAndRelaunch(dlPath)) {
                    DestroyWindow(m_hwnd);
                    return 0;
                }
                // Fallback if current folder is read-only (e.g. Program Files without admin): open release page
                std::error_code ec;
                std::filesystem::remove(dlPath, ec);
                if (m_availableUpdate && !m_availableUpdate->releaseUrl.empty()) {
                    const std::wstring url = Utf8ToWide(m_availableUpdate->releaseUrl);
                    ShellExecuteW(m_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                m_updateModalOpen = false;
                ShowToast(L"Opened GitHub release (folder not writable)", false);
            } else {
                ShowToast(L"Update download failed", true);
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
            DestroyWindow(m_hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(m_hwnd, kTimerAnim);
            KillTimer(m_hwnd, kTimerTelemetry);
            m_engine.Stop();
            SaveConfigNow();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

// =============================================================================
// RENDERING (PRECISION CRAFT — ULTRA-LOW-RAM DIRECT2D + DIRECTWRITE)
// =============================================================================

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
    RegisterHit(closeRc, "close", [this]() { DestroyWindow(m_hwnd); });
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
        SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
    });

    m_renderer.FillRoundedRect(plusRc, 5.0f, IsHovered("rate_plus") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(plusRc, 5.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"+", plusRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(plusRc, "rate_plus", [this]() {
        const double step = (m_config.cps.rateCps >= 50.0) ? 5.0 : 0.5;
        m_config.cps.rateCps = std::clamp(std::round((m_config.cps.rateCps + step) * 2.0) / 2.0, 0.5, kMaxRate);
        SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
            SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
            SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
        auto drawStepperRow = [&](const wchar_t* label, const std::wstring& val, const char* idPrefix,
                                  std::function<void()> onDec, std::function<void()> onInc) {
            m_renderer.DrawText(label, {pad, y, pad + 150.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
            const D2D1_RECT_F decRc{w - pad - 110.0f, y + 4.0f, w - pad - 88.0f, y + 24.0f};
            const D2D1_RECT_F incRc{w - pad - 22.0f,  y + 4.0f, w - pad,         y + 24.0f};
            m_renderer.FillRoundedRect(decRc, 4.0f, IsHovered(std::string(idPrefix) + "_d") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
            m_renderer.DrawRoundedRect(decRc, 4.0f, ThemeTokens::kHairline);
            m_renderer.DrawText(L"−", decRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(decRc, std::string(idPrefix) + "_d", std::move(onDec));

            m_renderer.DrawText(val, {decRc.right, y, incRc.left, y + 28.0f}, TextStyle::MonoSmall, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

            m_renderer.FillRoundedRect(incRc, 4.0f, IsHovered(std::string(idPrefix) + "_i") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
            m_renderer.DrawRoundedRect(incRc, 4.0f, ThemeTokens::kHairline);
            m_renderer.DrawText(L"+", incRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(incRc, std::string(idPrefix) + "_i", std::move(onInc));
            y += 28.0f;
        };

        m_renderer.DrawText(L"Click pattern", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F patRc{w - pad - 120.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(patRc, 5.0f, IsHovered("pat_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(patRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(PatternLabel(c.pattern), patRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(patRc, "pat_cyc", [this]() {
            m_config.cps.pattern = static_cast<ClickPattern>((static_cast<int>(m_config.cps.pattern) + 1) % 4);
            SyncEngineAndHotkeys(); UpdateWindowSizeForLayout(); SaveConfigNow(); RequestRender();
        });
        y += 28.0f;

        if (c.pattern == ClickPattern::CustomBurst) {
            drawStepperRow(L"Burst count", std::format(L"{}×", c.burstCount), "burst",
                [this]() { m_config.cps.burstCount = std::max(2u, m_config.cps.burstCount - 1); SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender(); },
                [this]() { m_config.cps.burstCount = std::min(20u, m_config.cps.burstCount + 1); SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender(); });

            drawStepperRow(L"Burst cooldown", std::format(L"{} ms", c.burstCooldownMs), "bcd",
                [this]() { m_config.cps.burstCooldownMs = (m_config.cps.burstCooldownMs > 20) ? m_config.cps.burstCooldownMs - 10 : 10; SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender(); },
                [this]() { m_config.cps.burstCooldownMs = std::min(5000u, m_config.cps.burstCooldownMs + 10); SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender(); });
        }

        drawStepperRow(L"Press hold", std::format(L"{} ms", c.holdDurationMs), "hold",
            [this]() { m_config.cps.holdDurationMs = (m_config.cps.holdDurationMs > 2) ? m_config.cps.holdDurationMs - 2 : 1; SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender(); },
            [this]() { m_config.cps.holdDurationMs = std::min(150u, m_config.cps.holdDurationMs + 2); SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender(); });

        m_renderer.DrawText(L"Cursor", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F curRc{w - pad - 120.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(curRc, 5.0f, IsHovered("cur_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(curRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(c.cursorMode == CursorMode::FollowCursor ? L"Follow cursor" : L"Locked (X, Y)",
                            curRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(curRc, "cur_cyc", [this]() {
            m_config.cps.cursorMode = (m_config.cps.cursorMode == CursorMode::FollowCursor) ? CursorMode::LockedPoint : CursorMode::FollowCursor;
            SyncEngineAndHotkeys(); UpdateWindowSizeForLayout(); SaveConfigNow(); RequestRender();
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
                SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
            SyncEngineAndHotkeys(); UpdateWindowSizeForLayout(); SaveConfigNow(); RequestRender();
        });
        y += 28.0f;

        if (c.limitMode != RepeatLimitMode::Unlimited) {
            const std::wstring lv = (c.limitMode == RepeatLimitMode::Count)
                ? std::format(L"{} clicks", c.limitValue)
                : std::format(L"{} s", c.limitValue);
            drawStepperRow(L"Limit value", lv, "limv",
                [this]() {
                    const uint32_t step = (m_config.cps.limitMode == RepeatLimitMode::Count) ? 50u : 5u;
                    m_config.cps.limitValue = (m_config.cps.limitValue > step) ? m_config.cps.limitValue - step : 1u;
                    SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
                },
                [this]() {
                    const uint32_t step = (m_config.cps.limitMode == RepeatLimitMode::Count) ? 50u : 5u;
                    m_config.cps.limitValue += step;
                    SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
        SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
    });

    m_renderer.FillRoundedRect(plusRc, 5.0f, IsHovered("rate_plus") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(plusRc, 5.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"+", plusRc, TextStyle::BodyMedium, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterHit(plusRc, "rate_plus", [this]() {
        const double step = (m_config.kps.rateKps >= 50.0) ? 5.0 : 0.5;
        m_config.kps.rateKps = std::clamp(std::round((m_config.kps.rateKps + step) * 2.0) / 2.0, 0.5, kMaxRate);
        SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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

        auto drawMod = [&](float rightX, const wchar_t* lbl, bool on, const char* id, bool* field) -> float {
            const float bw = 38.0f;
            const D2D1_RECT_F r{rightX - bw, y + 9.0f, rightX, y + 33.0f};
            m_renderer.FillRoundedRect(r, 5.0f, on ? ThemeTokens::kBgElevated : ThemeTokens::kBgCanvas);
            m_renderer.DrawRoundedRect(r, 5.0f, on ? ThemeTokens::kHairlineHover : ThemeTokens::kHairline);
            m_renderer.DrawText(lbl, r, TextStyle::MonoSmall, on ? ThemeTokens::kTextPrimary : ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(r, id, [this, field]() { *field = !(*field); SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender(); });
            return r.left - 5.0f;
        };
        float rx = keyRc.left - 6.0f;
        rx = drawMod(rx, L"Alt",   k.modAlt,   "mod_alt",   &m_config.kps.modAlt);
        rx = drawMod(rx, L"Shift", k.modShift, "mod_shift", &m_config.kps.modShift);
        drawMod(rx, L"Ctrl",       k.modCtrl,  "mod_ctrl",  &m_config.kps.modCtrl);
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
            SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
            SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
            SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
            SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
        });
        m_renderer.DrawText(std::format(L"{} ms", k.pressDurationMs), {dRc.right, y, iRc.left, y + 28.0f}, TextStyle::MonoSmall, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        m_renderer.FillRoundedRect(iRc, 4.0f, IsHovered("kp_i") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(iRc, 4.0f, ThemeTokens::kHairline);
        m_renderer.DrawText(L"+", iRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(iRc, "kp_i", [this]() {
            m_config.kps.pressDurationMs = std::min(300u, m_config.kps.pressDurationMs + 5);
            SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
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
            SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
        });
        y += 28.0f;

        m_renderer.DrawText(L"Repeat limit", {pad, y, pad + 140.0f, y + 28.0f}, TextStyle::Body, ThemeTokens::kTextMuted);
        const D2D1_RECT_F limRc{w - pad - 128.0f, y + 3.0f, w - pad, y + 25.0f};
        m_renderer.FillRoundedRect(limRc, 5.0f, IsHovered("klim_cyc") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(limRc, 5.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(LimitLabel(k.limitMode), limRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(limRc, "klim_cyc", [this]() {
            m_config.kps.limitMode = static_cast<RepeatLimitMode>((static_cast<int>(m_config.kps.limitMode) + 1) % 3);
            SyncEngineAndHotkeys(); UpdateWindowSizeForLayout(); SaveConfigNow(); RequestRender();
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
                SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
            });
            m_renderer.DrawText(lv, {ldRc.right, y, liRc.left, y + 28.0f}, TextStyle::MonoSmall, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            m_renderer.FillRoundedRect(liRc, 4.0f, IsHovered("kl_i") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
            m_renderer.DrawRoundedRect(liRc, 4.0f, ThemeTokens::kHairline);
            m_renderer.DrawText(L"+", liRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(liRc, "kl_i", [this]() {
                const uint32_t step = (m_config.kps.limitMode == RepeatLimitMode::Count) ? 50u : 5u;
                m_config.kps.limitValue += step;
                SyncEngineAndHotkeys(); SaveConfigNow(); RequestRender();
            });
            y += 28.0f;
        }
    }
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
    m_renderer.DrawText(std::format(L"{} STABILITY   σ {:.2f} ms · guard {:.0f}µs", unit, m_telemetry.jitterStdDevMs, m_engine.Timing().SpinGuardUs()),
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

    auto drawToggle = [&](const wchar_t* title, const wchar_t* sub, bool* field, const char* id, bool resizeOnToggle = false) {
        const float rh = 44.0f;
        m_renderer.DrawText(title, {pad, y + 6.0f, w - pad - 44.0f, y + 22.0f}, TextStyle::Body, ThemeTokens::kTextPrimary);
        m_renderer.DrawText(sub,   {pad, y + 22.0f, w - pad - 44.0f, y + 38.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);
        const D2D1_RECT_F sr{w - pad - 32.0f, y + 13.0f, w - pad, y + 31.0f};
        m_renderer.DrawSwitch(sr, *field ? 1.0f : 0.0f, accent, IsHovered(id));
        RegisterHit(sr, id, [this, field, resizeOnToggle]() {
            *field = !(*field);
            SyncEngineAndHotkeys();
            if (resizeOnToggle) UpdateWindowSizeForLayout();
            SaveConfigNow();
            RequestRender();
        });
        y += rh;
        m_renderer.DrawLine(pad, y, w - pad, y, ThemeTokens::kHairlineSoft);
    };

    drawToggle(L"Stop when Cadence is focused", L"Prevents self-clicking while window is open", &m_config.settings.stopOnSelfFocus, "set_self");
    drawToggle(L"Lock to target window", L"Only fires while selected app/game is focused", &m_config.settings.targetWindowLock, "set_tlock", true);

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

    drawToggle(L"Hybrid QPC timer", L"Auto-calibrated waitable timer + µs spin", &m_config.settings.hybridQpcTimer, "set_qpc");
    drawToggle(L"Ignore Win11 throttling", L"Keeps sub-ms accuracy when minimized", &m_config.settings.disablePowerThrottle, "set_thr");
    drawToggle(L"MMCSS thread priority", L"Pro Audio scheduling while active", &m_config.settings.mmcssPriority, "set_mmcss");
    drawToggle(L"Minimize to system tray", L"Trims RAM & pauses UI when hidden", &m_config.settings.minimizeToTray, "set_tray");
    drawToggle(L"Check for updates", L"Notifies when a new GitHub release is available", &m_config.settings.checkForUpdates, "set_upd");

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
        SyncEngineAndHotkeys();
        SetAlwaysOnTop(false);
        UpdateWindowSizeForLayout();
        SaveConfigNow();
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

    const float metaY = top + 58.0f;
    m_renderer.DrawText(std::format(L"{:.2f}% CPU · {:.1f} MB", m_telemetry.cpuPercent, m_telemetry.memoryMb),
                        {pad, metaY, w - pad - 100.0f, metaY + 18.0f}, TextStyle::MonoSmall, ThemeTokens::kTextMuted);

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

void MainWindow::RenderProfilePopover() {
    const auto& list = m_profiles->Profiles();
    const float rowH = 26.0f;
    const float popH = static_cast<float>(list.size()) * rowH + (m_namingProfile ? 76.0f : 68.0f);
    const float popLeft = std::clamp(m_profileBtnLeft, 88.0f, m_widthDip - 236.0f);
    const D2D1_RECT_F rc{popLeft, 38.0f, popLeft + 222.0f, 38.0f + popH};

    m_renderer.FillRoundedRect({rc.left, rc.top + 4.0f, rc.right, rc.bottom + 6.0f}, 9.0f, HexColor(0x000000, 0.45f));
    m_renderer.FillRoundedRect(rc, 9.0f, ThemeTokens::kBgPopover);
    m_renderer.DrawRoundedRect(rc, 9.0f, ThemeTokens::kHairlineStrong);

    float py = rc.top + 6.0f;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& p = list[i];
        const bool active = (p.id == m_config.activeProfileId);
        const D2D1_RECT_F r{rc.left + 6.0f, py, rc.right - 28.0f, py + rowH};
        const std::string id = std::format("prof_{}", i);

        if (active || IsHovered(id)) {
            m_renderer.FillRoundedRect({rc.left + 6.0f, py, rc.right - 6.0f, py + rowH}, 5.0f,
                                       active ? HexColor(0xFFFFFF, 0.08f) : HexColor(0xFFFFFF, 0.04f));
        }

        m_renderer.DrawText(Utf8ToWide(p.name), {r.left + 8.0f, r.top, r.right - 52.0f, r.bottom},
                            TextStyle::Body, active ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary);
        const std::wstring pMeta = std::format(L"{:.0f} {}",
            (p.mode == OperatingMode::CPS) ? p.cps.rateCps : p.kps.rateKps,
            p.mode == OperatingMode::CPS ? L"CPS" : L"KPS");
        m_renderer.DrawText(pMeta,
                            {r.right - 56.0f, r.top, r.right, r.bottom}, TextStyle::MonoSmall, ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);

        RegisterHit(r, id, [this, pid = p.id]() {
            m_profiles->ActivateProfile(pid);
            m_profileOpen = false;
            m_namingProfile = false;
            SyncEngineAndHotkeys();
            UpdateWindowSizeForLayout();
            SaveConfigNow();
            StartAnimTimer();
        }, true);

        if (list.size() > 1) {
            const D2D1_RECT_F delRc{rc.right - 26.0f, py + 3.0f, rc.right - 8.0f, py + rowH - 3.0f};
            const std::string delId = std::format("pdel_{}", i);
            m_renderer.DrawText(L"×", delRc, TextStyle::Body, IsHovered(delId) ? ThemeTokens::kDangerHover : ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(delRc, delId, [this, pid = p.id]() {
                m_profiles->DeleteProfile(pid);
                SaveConfigNow();
                RequestRender();
            }, true);
        }
        py += rowH;
    }

    py += 4.0f;
    m_renderer.DrawLine(rc.left + 6.0f, py, rc.right - 6.0f, py, ThemeTokens::kHairline);
    py += 5.0f;

    if (m_namingProfile) {
        const D2D1_RECT_F inpRc{rc.left + 8.0f, py, rc.right - 46.0f, py + 24.0f};
        m_renderer.FillRoundedRect(inpRc, 5.0f, ThemeTokens::kBgCanvas);
        m_renderer.DrawRoundedRect(inpRc, 5.0f, ActiveAccent());
        const std::wstring disp = m_profileNameBuf.empty() ? L"Type name…|" : (m_profileNameBuf + L"|");
        m_renderer.DrawText(disp, {inpRc.left + 6.0f, inpRc.top, inpRc.right - 4.0f, inpRc.bottom},
                            TextStyle::Body, m_profileNameBuf.empty() ? ThemeTokens::kTextMuted : ThemeTokens::kTextPrimary);

        const D2D1_RECT_F okRc{rc.right - 42.0f, py, rc.right - 8.0f, py + 24.0f};
        m_renderer.FillRoundedRect(okRc, 5.0f, ActiveAccent(IsHovered("prof_ok")));
        m_renderer.DrawText(L"Save", okRc, TextStyle::BodyMedium, ThemeTokens::kTextWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(okRc, "prof_ok", [this]() { CommitProfileName(); }, true);
        py += 29.0f;
    } else {
        const D2D1_RECT_F saveNewRc{rc.left + 6.0f, py, rc.right - 6.0f, py + 25.0f};
        if (IsHovered("prof_new")) m_renderer.FillRoundedRect(saveNewRc, 5.0f, HexColor(0xFFFFFF, 0.05f));
        m_renderer.DrawText(L"+ Save as new preset…", {saveNewRc.left + 8.0f, saveNewRc.top, saveNewRc.right, saveNewRc.bottom}, TextStyle::Body, ActiveAccent());
        RegisterHit(saveNewRc, "prof_new", [this]() {
            m_namingProfile = true;
            m_profileNameBuf.clear();
            RequestRender();
        }, true);
        py += 25.0f;
    }

    const D2D1_RECT_F updRc{rc.left + 6.0f, py, rc.right - 6.0f, py + 24.0f};
    if (IsHovered("prof_upd")) m_renderer.FillRoundedRect(updRc, 5.0f, HexColor(0xFFFFFF, 0.05f));
    m_renderer.DrawText(L"Update active preset", {updRc.left + 8.0f, updRc.top, updRc.right, updRc.bottom}, TextStyle::Body, ThemeTokens::kTextSecondary);
    RegisterHit(updRc, "prof_upd", [this]() {
        m_profiles->UpdateActiveProfile();
        m_profileOpen = false;
        m_namingProfile = false;
        SaveConfigNow();
        RequestRender();
    }, true);
}

void MainWindow::RenderUpdateModal(float w, float h) {
    if (!m_availableUpdate) return;
    const auto& rel = *m_availableUpdate;
    const D2D1_COLOR_F accent = ActiveAccent();

    // Dimmed backdrop covering entire window
    m_renderer.FillRect({0.0f, 0.0f, w, h}, HexColor(0x000000, 0.64f));

    const float cardW = w - 48.0f;
    const float cardH = 216.0f;
    const float cardL = (w - cardW) * 0.5f;
    const float cardT = std::max(54.0f, (h - cardH) * 0.5f - 6.0f);
    const D2D1_RECT_F card{cardL, cardT, cardL + cardW, cardT + cardH};

    // Elevation shadow + Obsidian modal card
    m_renderer.FillRoundedRect({card.left, card.top + 6.0f, card.right, card.bottom + 10.0f}, 12.0f, HexColor(0x000000, 0.55f));
    m_renderer.FillRoundedRect(card, 12.0f, ThemeTokens::kBgPopover);
    m_renderer.DrawRoundedRect(card, 12.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawLine(card.left + 16.0f, card.top + 1.0f, card.right - 16.0f, card.top + 1.0f, HexColor(0x7C5CFC, 0.50f));

    const float pad = 20.0f;
    const float innerL = card.left + pad;
    const float innerR = card.right - pad;

    // Header: Cube logo + Title + Close X
    m_renderer.DrawCubeLogo(innerL, card.top + 19.0f, 16.0f, accent);
    m_renderer.DrawText(L"Update available", {innerL + 24.0f, card.top + 15.0f, innerR - 28.0f, card.top + 39.0f},
                        TextStyle::Title, ThemeTokens::kTextPrimary);

    const D2D1_RECT_F closeRc{innerR - 22.0f, card.top + 15.0f, innerR + 2.0f, card.top + 39.0f};
    if (!m_updateDownloading && IsHovered("upd_close")) {
        m_renderer.FillRoundedRect(closeRc, 5.0f, HexColor(0xFFFFFF, 0.06f));
    }
    m_renderer.DrawCloseIcon((closeRc.left + closeRc.right) * 0.5f, (closeRc.top + closeRc.bottom) * 0.5f,
                             IsHovered("upd_close") ? ThemeTokens::kTextPrimary : ThemeTokens::kTextMuted);
    if (!m_updateDownloading) {
        RegisterHit(closeRc, "upd_close", [this]() {
            m_updateModalOpen = false;
            RequestRender();
        }, false, true);
    }

    // Version comparison pill: v1.0.1 -> v1.1.0
    const D2D1_RECT_F verPill{innerL, card.top + 50.0f, innerR, card.top + 78.0f};
    m_renderer.FillRoundedRect(verPill, 7.0f, ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(verPill, 7.0f, ThemeTokens::kHairline);

    const std::wstring curVer = L"v" + Utf8ToWide(CADENCE_VERSION_STR);
    const std::wstring newVer = Utf8ToWide(rel.tagName);
    const float curW   = m_renderer.MeasureTextWidth(curVer, TextStyle::MonoSmall);
    const float arrowW = m_renderer.MeasureTextWidth(L" -> ", TextStyle::MonoSmall);

    m_renderer.DrawText(curVer, {verPill.left + 12.0f, verPill.top, verPill.left + 12.0f + curW + 4.0f, verPill.bottom},
                        TextStyle::MonoSmall, ThemeTokens::kTextSecondary);
    m_renderer.DrawText(L" -> ", {verPill.left + 12.0f + curW, verPill.top, verPill.left + 12.0f + curW + arrowW + 4.0f, verPill.bottom},
                        TextStyle::MonoSmall, ThemeTokens::kTextMuted);
    m_renderer.DrawText(newVer, {verPill.left + 12.0f + curW + arrowW, verPill.top, verPill.right - 92.0f, verPill.bottom},
                        TextStyle::MonoSmall, ThemeTokens::kEmerald);
    m_renderer.DrawText(L"GitHub Release", {verPill.right - 102.0f, verPill.top, verPill.right - 12.0f, verPill.bottom},
                        TextStyle::Caption, ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Description
    m_renderer.DrawText(L"A newer version of Cadence is ready.",
                        {innerL, card.top + 88.0f, innerR, card.top + 106.0f}, TextStyle::Body, ThemeTokens::kTextPrimary);
    m_renderer.DrawText(m_updateDownloading ? L"Downloading Cadence.exe from GitHub…" : L"Downloads Cadence.exe and restarts automatically.",
                        {innerL, card.top + 107.0f, innerR, card.top + 123.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);

    // Primary CTA: Update now
    const D2D1_RECT_F updRc{innerL, card.top + 134.0f, innerR, card.top + 168.0f};
    const D2D1_COLOR_F ctaBg = m_updateDownloading
        ? ThemeTokens::kBgElevated
        : ActiveAccent(IsHovered("upd_now"));
    m_renderer.FillRoundedRect(updRc, ThemeTokens::kRadiusButton, ctaBg);
    m_renderer.DrawRoundedRect(updRc, ThemeTokens::kRadiusButton, m_updateDownloading ? accent : HexColor(0xFFFFFF, 0.14f));
    if (!m_updateDownloading) {
        m_renderer.DrawLine(updRc.left + 8.0f, updRc.top + 1.0f, updRc.right - 8.0f, updRc.top + 1.0f, HexColor(0xFFFFFF, 0.15f));
    }
    m_renderer.DrawText(m_updateDownloading ? L"Downloading…" : L"Update now",
                        updRc, TextStyle::BodyMedium, ThemeTokens::kTextWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!m_updateDownloading) {
        RegisterHit(updRc, "upd_now", [this]() {
            if (m_updatePreviewOnly) {
                ShowToast(L"Preview mode — update button works!", false);
                return;
            }
            StartUpdateDownloadAsync();
        }, false, true);
    }

    // Secondary row: Later | Ignore updates
    const float btnGap = 8.0f;
    const float btnW   = (innerR - innerL - btnGap) * 0.5f;
    const D2D1_RECT_F laterRc{innerL, card.top + 176.0f, innerL + btnW, card.top + 202.0f};
    const D2D1_RECT_F ignRc  {laterRc.right + btnGap, card.top + 176.0f, innerR, card.top + 202.0f};

    m_renderer.FillRoundedRect(laterRc, 6.0f, (!m_updateDownloading && IsHovered("upd_later")) ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(laterRc, 6.0f, ThemeTokens::kHairlineStrong);
    m_renderer.DrawText(L"Later", laterRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    m_renderer.FillRoundedRect(ignRc, 6.0f, (!m_updateDownloading && IsHovered("upd_ignore")) ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
    m_renderer.DrawRoundedRect(ignRc, 6.0f, ThemeTokens::kHairline);
    m_renderer.DrawText(L"Ignore updates", ignRc, TextStyle::Body,
                        (!m_updateDownloading && IsHovered("upd_ignore")) ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary,
                        DWRITE_TEXT_ALIGNMENT_CENTER);

    if (!m_updateDownloading) {
        RegisterHit(laterRc, "upd_later", [this]() {
            m_updateModalOpen = false;
            RequestRender();
        }, false, true);

        RegisterHit(ignRc, "upd_ignore", [this]() {
            m_config.settings.checkForUpdates = false;
            m_updateModalOpen = false;
            SaveConfigNow();
            ShowToast(L"Updates ignored (re-enable in Settings)", false);
        }, false, true);
    }
}

void MainWindow::StartUpdateCheckAsync() {
    auto shared = m_updateShared;
    if (!shared) return;

    std::thread([shared]() {
        auto rel = UpdateChecker::CheckForUpdate();
        if (!rel) return;

        HWND target = nullptr;
        {
            std::scoped_lock lock(shared->mtx);
            shared->found = std::move(rel);
            target        = shared->hwnd;
        }
        if (target) {
            PostMessageW(target, WM_APP_UPDATE_AVAILABLE, 0, 0);
        }
    }).detach();
}

void MainWindow::StartUpdateDownloadAsync() {
    if (!m_availableUpdate || m_updateDownloading) return;

    if (m_availableUpdate->downloadUrl.empty()) {
        const std::wstring url = Utf8ToWide(m_availableUpdate->releaseUrl);
        ShellExecuteW(m_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        m_updateModalOpen = false;
        RequestRender();
        return;
    }

    auto shared = m_updateShared;
    if (!shared) return;

    m_updateDownloading = true;
    RequestRender();

    const std::string dlUrl = m_availableUpdate->downloadUrl;
    std::filesystem::path dest = UpdateChecker::CurrentExePath();
    if (dest.empty()) {
        m_updateDownloading = false;
        ShowToast(L"Could not locate current executable", true);
        return;
    }
    dest += L".new";

    std::thread([shared, dlUrl, dest]() {
        const bool ok = UpdateChecker::DownloadBinary(dlUrl, dest);
        HWND target = nullptr;
        {
            std::scoped_lock lock(shared->mtx);
            shared->downloadOk     = ok;
            shared->downloadedPath = ok ? dest : std::filesystem::path{};
            target                 = shared->hwnd;
        }
        if (target) {
            PostMessageW(target, WM_APP_UPDATE_DONE, 0, 0);
        }
    }).detach();
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

void MainWindow::RegisterHit(D2D1_RECT_F rc, std::string id, std::function<void()> onClick, bool isPopover, bool isModal) {
    m_hits.push_back(HitItem{rc, std::move(id), std::move(onClick), false, DragSlider::None, isPopover, isModal});
}

void MainWindow::RegisterSlider(D2D1_RECT_F rc, std::string id, DragSlider kind) {
    m_hits.push_back(HitItem{rc, std::move(id), nullptr, true, kind, false, false});
}

void MainWindow::UpdateSliderFromX(DragSlider kind, float dipX) {
    const D2D1_RECT_F rc = (kind == DragSlider::MainRate) ? m_mainSliderRc : m_jitterSliderRc;
    const float left  = rc.left + 7.0f;
    const float right = rc.right - 7.0f;
    if (right <= left) return;
    const float t = std::clamp((dipX - left) / (right - left), 0.0f, 1.0f);

    if (kind == DragSlider::MainRate) {
        const double val = SliderTToRate(t);
        if (m_config.activeMode == OperatingMode::CPS) m_config.cps.rateCps = val;
        else m_config.kps.rateKps = val;
    } else if (kind == DragSlider::Jitter) {
        const double val = std::round((0.5 + t * 24.5) * 2.0) / 2.0;
        if (m_config.activeMode == OperatingMode::CPS) m_config.cps.jitterMs = val;
        else m_config.kps.jitterMs = val;
    }
    SyncEngineAndHotkeys();
    RequestRender();
}

void MainWindow::HandleMouseMove(float dipX, float dipY) {
    if (m_dragSlider != DragSlider::None) {
        UpdateSliderFromX(m_dragSlider, dipX);
        return;
    }

    std::string newHover;
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        if (m_updateModalOpen && !it->isModal) continue;
        if (!m_updateModalOpen && m_profileOpen && !it->isPopover && it->id != "profile_btn") continue;
        if (PtInRectF(it->rect, dipX, dipY)) {
            newHover = it->id;
            break;
        }
    }
    if (newHover != m_hoverId) {
        m_hoverId = std::move(newHover);
        RequestRender();
    }
}

void MainWindow::HandleLButtonDown(float dipX, float dipY) {
    bool hitPopover = false;
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        if (m_updateModalOpen && !it->isModal) continue;
        if (!m_updateModalOpen && m_profileOpen && !it->isPopover && it->id != "profile_btn") continue;
        if (PtInRectF(it->rect, dipX, dipY)) {
            hitPopover = it->isPopover || it->id == "profile_btn";
            if (m_editingRate && it->id != "rate_edit") CommitRateEdit();
            if (it->isSlider) {
                m_dragSlider = it->sliderKind;
                SetCapture(m_hwnd);
                UpdateSliderFromX(m_dragSlider, dipX);
            } else if (it->onClick) {
                auto fn = it->onClick;
                fn();
            }
            return;
        }
    }
    if (m_updateModalOpen) return;
    if (m_editingRate) CommitRateEdit();
    if (m_profileOpen && !hitPopover) {
        m_profileOpen   = false;
        m_namingProfile = false;
        RequestRender();
    }
}

void MainWindow::HandleLButtonUp() {
    if (m_dragSlider != DragSlider::None) {
        m_dragSlider = DragSlider::None;
        ReleaseCapture();
        SaveConfigNow();
        RequestRender();
    }
}

void MainWindow::HandleMouseWheel(int delta) {
    if (m_tab != ViewTab::Controls || m_profileOpen || m_editingRate || m_updateModalOpen) return;
    const double cur  = (m_config.activeMode == OperatingMode::CPS) ? m_config.cps.rateCps : m_config.kps.rateKps;
    const double mag  = (cur >= 50.0) ? 5.0 : 0.5;
    const double step = (delta > 0) ? mag : -mag;
    if (m_config.activeMode == OperatingMode::CPS) {
        m_config.cps.rateCps = std::clamp(std::round((m_config.cps.rateCps + step) * 2.0) / 2.0, 0.5, kMaxRate);
    } else {
        m_config.kps.rateKps = std::clamp(std::round((m_config.kps.rateKps + step) * 2.0) / 2.0, 0.5, kMaxRate);
    }
    SyncEngineAndHotkeys();
    SaveConfigNow();
    RequestRender();
}

void MainWindow::HandleCharInput(wchar_t ch) {
    if (m_editingRate) {
        if (ch == L',') ch = L'.'; // Support German decimal comma
        if ((ch >= L'0' && ch <= L'9') || (ch == L'.' && m_rateEditBuf.find(L'.') == std::wstring::npos)) {
            if (m_rateEditBuf.size() < 6) {
                m_rateEditBuf.push_back(ch);
                RequestRender();
            }
        }
    } else if (m_namingProfile) {
        if (ch >= 32 && ch != 127 && m_profileNameBuf.size() < 24) {
            m_profileNameBuf.push_back(ch);
            RequestRender();
        }
    }
}

void MainWindow::HandleKeyDown(WPARAM vk) {
    if (m_updateModalOpen) {
        if (vk == VK_ESCAPE && !m_updateDownloading) {
            m_updateModalOpen = false;
            RequestRender();
        }
        return;
    }
    if (m_editingRate) {
        if (vk == VK_RETURN) {
            CommitRateEdit();
        } else if (vk == VK_ESCAPE) {
            m_editingRate = false;
            m_rateEditBuf.clear();
            RequestRender();
        } else if (vk == VK_BACK && !m_rateEditBuf.empty()) {
            m_rateEditBuf.pop_back();
            RequestRender();
        }
        return;
    }
    if (m_namingProfile) {
        if (vk == VK_RETURN) {
            CommitProfileName();
        } else if (vk == VK_ESCAPE) {
            m_namingProfile = false;
            m_profileNameBuf.clear();
            RequestRender();
        } else if (vk == VK_BACK && !m_profileNameBuf.empty()) {
            m_profileNameBuf.pop_back();
            RequestRender();
        }
    }
}

void MainWindow::CommitRateEdit() {
    m_editingRate = false;
    if (!m_rateEditBuf.empty()) {
        try {
            const double parsed  = std::stod(m_rateEditBuf);
            const bool isCps     = (m_config.activeMode == OperatingMode::CPS);
            const bool asMs      = isCps ? m_config.cps.displayAsMs : m_config.kps.displayAsMs;
            const double rate    = asMs ? (parsed > 0.5 ? 1000.0 / parsed : kMaxRate) : parsed;
            const double clamped = std::clamp(std::round(rate * 10.0) / 10.0, 0.5, kMaxRate);
            if (isCps) m_config.cps.rateCps = clamped;
            else       m_config.kps.rateKps = clamped;
            SyncEngineAndHotkeys();
            SaveConfigNow();
        } catch (...) {
        }
    }
    m_rateEditBuf.clear();
    RequestRender();
}

void MainWindow::CommitProfileName() {
    std::string nm = WideToUtf8(m_profileNameBuf);
    if (nm.empty()) {
        const bool isCps = (m_config.activeMode == OperatingMode::CPS);
        nm = isCps
            ? std::format("Mouse {:.0f} CPS", m_config.cps.rateCps)
            : std::format("Key {} {:.0f}", static_cast<char>(m_config.kps.targetVk >= 32 && m_config.kps.targetVk < 127 ? m_config.kps.targetVk : 'K'), m_config.kps.rateKps);
    }
    m_profiles->CreateFromCurrent(nm);
    m_namingProfile = false;
    m_profileOpen   = false;
    m_profileNameBuf.clear();
    SaveConfigNow();
    RequestRender();
}

void MainWindow::BeginKeyCapture(CaptureKind kind) {
    m_captureKind = kind;
    const bool allowMouse = (kind == CaptureKind::ModeHotkey || kind == CaptureKind::PanicHotkey);
    m_hotkeys.BeginCapture(allowMouse, [this, kind](HotkeyBinding hk) {
        m_captureKind = CaptureKind::None;

        if (kind == CaptureKind::ModeHotkey) {
            // Strict conflict guard: Mouse/Keyboard trigger may NOT match Emergency Kill hotkey
            if (hk == m_config.settings.panicHotkey) {
                ShowToast(L"Hotkey reserved for Emergency Kill", true);
                return;
            }
            if (m_config.activeMode == OperatingMode::CPS) m_config.cps.hotkey = hk;
            else m_config.kps.hotkey = hk;
        } else if (kind == CaptureKind::KpsTargetKey && hk.source == HotkeySource::Keyboard) {
            m_config.kps.targetVk = hk.code;
        } else if (kind == CaptureKind::KpsSeqAppend && hk.source == HotkeySource::Keyboard) {
            if (m_config.kps.sequenceVks.size() < 12) {
                m_config.kps.sequenceVks.push_back(hk.code);
            }
        } else if (kind == CaptureKind::PanicHotkey) {
            if (hk == m_config.cps.hotkey || hk == m_config.kps.hotkey) {
                ShowToast(L"Kill hotkey must be unique", true);
                return;
            }
            m_config.settings.panicHotkey = hk;
        }
        SyncEngineAndHotkeys();
        SaveConfigNow();
        RequestRender();
    }, [this]() {
        m_captureKind = CaptureKind::None;
        RequestRender();
    });
    RequestRender();
}

void MainWindow::BeginGlobalPick(PickKind kind) {
    m_pickKind = kind;
    m_hotkeys.BeginGlobalLeftClickCapture([this, kind](POINT pt, HWND targetHwnd) {
        if (kind == PickKind::LockedCoords) {
            m_config.cps.lockX = pt.x;
            m_config.cps.lockY = pt.y;
        } else if (kind == PickKind::TargetWindow) {
            if (targetHwnd && targetHwnd != m_hwnd) {
                const std::string proc = EngineController::QueryWindowProcessName(targetHwnd);
                if (!proc.empty()) {
                    m_config.settings.targetProcessName = proc;
                }
            }
        }
        m_pickKind = PickKind::None;
        SyncEngineAndHotkeys();
        SaveConfigNow();
        RequestRender();
    }, [this]() {
        m_pickKind = PickKind::None;
        RequestRender();
    });
    RequestRender();
}

void MainWindow::ExportActiveProfileDialog() {
    wchar_t fileBuf[MAX_PATH] = L"cadence_profile.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hwnd;
    ofn.lpstrFilter = L"JSON Profile (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = fileBuf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) {
        m_profiles->ExportProfile(m_config.activeProfileId, fileBuf);
    }
}

void MainWindow::ImportProfileDialog() {
    wchar_t fileBuf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hwnd;
    ofn.lpstrFilter = L"JSON Profile (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = fileBuf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        if (m_profiles->ImportProfile(fileBuf)) {
            SyncEngineAndHotkeys();
            UpdateWindowSizeForLayout();
            SaveConfigNow();
            RequestRender();
        }
    }
}

} // namespace cadence::ui
