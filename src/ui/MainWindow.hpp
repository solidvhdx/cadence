#pragma once

#include "core/ConfigManager.hpp"
#include "core/EngineController.hpp"
#include "core/HotkeyManager.hpp"
#include "core/ProfileManager.hpp"
#include "core/Types.hpp"
#include "ui/D2DRenderer.hpp"

#include <shellapi.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cadence::ui {

enum class ViewTab : uint8_t {
    Controls = 0,
    Telemetry = 1,
    Settings = 2
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE hInstance);
    int  RunMessageLoop();

    [[nodiscard]] HWND Handle() const noexcept { return m_hwnd; }

private:
    enum class DragSlider : uint8_t {
        None = 0,
        MainRate,
        Jitter
    };

    enum class CaptureKind : uint8_t {
        None = 0,
        ModeHotkey,
        KpsTargetKey,
        KpsSeqAppend,
        PanicHotkey
    };

    enum class PickKind : uint8_t {
        None = 0,
        LockedCoords,
        TargetWindow
    };

    struct HitItem {
        D2D1_RECT_F           rect{};
        std::string           id;
        std::function<void()> onClick;
        bool                  isSlider   = false;
        DragSlider            sliderKind = DragSlider::None;
        bool                  isPopover  = false;
    };

    static constexpr UINT WM_APP_STATE_CHANGED = WM_APP + 1;
    static constexpr UINT WM_APP_TRAY_ICON     = WM_APP + 2;
    static constexpr UINT_PTR kTimerAnim       = 1;
    static constexpr UINT_PTR kTimerTelemetry  = 2;

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    // Lifecycle & Window management
    void ApplyWindowChrome();
    void UpdateWindowSizeForLayout(bool force = false);
    float ComputeDesiredHeightDip() const noexcept;
    void SetAlwaysOnTop(bool onTop);
    void InitTrayIcon();
    void UpdateTrayIconState();
    void RemoveTrayIcon();
    void ShowFromTray();
    void HideToTray();
    void ShowTrayContextMenu();
    HICON CreateAppIcon(int sizePx, uint32_t coreArgb);
    static bool IsProcessElevated() noexcept;
    void RestartAsAdministrator();

    // State & Persistence
    void EnsureUniqueHotkeys();
    void ShowToast(std::wstring text, bool isDanger = true);
    void SyncEngineAndHotkeys();
    void SaveConfigNow();
    void RequestRender();
    void StartAnimTimer();
    void UpdateTelemetryTimer();
    void StepAnimations();

    // Rendering (Precision Craft)
    void Render(HDC paintDc = nullptr);
    void RenderTitlebar(float w);
    void RenderModeAndTabs(float w, float& y);
    void RenderControlsTab(float w, float& y);
    void RenderCpsControls(float w, float& y);
    void RenderKpsControls(float w, float& y);
    void RenderTelemetryTab(float w, float& y);
    void RenderSettingsTab(float w, float& y);
    void RenderFooter(float w, float h);
    void RenderProfilePopover();
    void RenderToast(float w, float h);

    // Hit-testing & Input
    void RegisterHit(D2D1_RECT_F rc, std::string id, std::function<void()> onClick, bool isPopover = false);
    void RegisterSlider(D2D1_RECT_F rc, std::string id, DragSlider kind);
    bool IsHovered(std::string_view id) const noexcept { return m_hoverId == id; }
    void UpdateSliderFromX(DragSlider kind, float dipX);
    void HandleMouseMove(float dipX, float dipY);
    void HandleLButtonDown(float dipX, float dipY);
    void HandleLButtonUp();
    void HandleMouseWheel(int delta);
    void HandleCharInput(wchar_t ch);
    void HandleKeyDown(WPARAM vk);
    void CommitRateEdit();
    void CommitProfileName();
    void BeginKeyCapture(CaptureKind kind);
    void BeginGlobalPick(PickKind kind);
    void ExportActiveProfileDialog();
    void ImportProfileDialog();

    // Coordinate conversion
    [[nodiscard]] float PxToDip(int px) const noexcept { return static_cast<float>(px) * 96.0f / m_dpi; }
    [[nodiscard]] int   DipToPx(float dip) const noexcept { return static_cast<int>(dip * m_dpi / 96.0f + 0.5f); }
    [[nodiscard]] D2D1_COLOR_F ActiveAccent(bool hover = false) const noexcept;

    HINSTANCE m_hInstance       = nullptr;
    HWND      m_hwnd            = nullptr;
    HICON     m_iconBig         = nullptr;
    HICON     m_iconSmallIdle   = nullptr;
    HICON     m_iconSmallActive = nullptr;
    HICON     m_iconSmallArmed  = nullptr;
    float     m_dpi             = 96.0f;
    float     m_widthDip        = ThemeTokens::kWindowWidth;
    float     m_heightDip       = 488.0f;
    float     m_profileBtnLeft  = 110.0f;
    float     m_profileBtnRight = 200.0f;
    bool      m_trayAdded       = false;
    bool      m_inTray          = false;
    bool      m_trackingMouse   = false;
    bool      m_isElevated      = false;

    // Core subsystems (completely decoupled from UI)
    AppConfig                       m_config{};
    std::unique_ptr<ProfileManager> m_profiles;
    EngineController                m_engine;
    HotkeyManager                   m_hotkeys;
    D2DRenderer                     m_renderer;
    TelemetrySnapshot               m_telemetry{};

    // UI transient state
    ViewTab      m_tab          = ViewTab::Controls;
    bool         m_advancedOpen = false;
    bool         m_profileOpen  = false;
    PickKind     m_pickKind     = PickKind::None;
    CaptureKind  m_captureKind  = CaptureKind::None;
    DragSlider   m_dragSlider   = DragSlider::None;
    bool         m_editingRate  = false;
    std::wstring m_rateEditBuf;
    bool         m_namingProfile = false;
    std::wstring m_profileNameBuf;

    D2D1_RECT_F m_mainSliderRc{};
    D2D1_RECT_F m_jitterSliderRc{};

    std::string          m_hoverId;
    std::vector<HitItem> m_hits;

    // Smooth animation states (0 FPS when settled!)
    float        m_modeAnimT   = 0.0f; // 0 = CPS, 1 = KPS
    float        m_jitterAnimT = 1.0f;
    float        m_toastMs     = 0.0f;
    bool         m_toastDanger = true;
    std::wstring m_toastText;
    bool         m_animActive  = false;
};

} // namespace cadence::ui
