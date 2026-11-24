#include "ui/MainWindow.hpp"
#include "ui/UiHelpers.hpp"

#include <algorithm>
#include <cmath>
#include <format>

// Hit-testing, mouse / keyboard input, inline editing and hotkey / coordinate capture.

namespace cadence::ui {

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
        if (m_updateModalOpen && !it->isModal && it->id != "min" && it->id != "close") continue;
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
        if (m_updateModalOpen && !it->isModal && it->id != "min" && it->id != "close") continue;
        if (!m_updateModalOpen && m_profileOpen && !it->isPopover && it->id != "profile_btn") continue;
        if (PtInRectF(it->rect, dipX, dipY)) {
            hitPopover = it->isPopover || it->id == "profile_btn";
            if (m_editingRate && it->id != "rate_edit") CommitRateEdit();
            if (m_captureKind != CaptureKind::None &&
                it->id != "hk_cap" && it->id != "kps_key" && it->id != "seq_add" && it->id != "set_panic") {
                m_hotkeys.CancelCapture();
            }
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
    if (m_captureKind != CaptureKind::None) m_hotkeys.CancelCapture();
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
    if (m_updateModalOpen || m_editingRate) return;
    if (m_profileOpen) {
        m_profileScroll += (delta > 0) ? -1 : 1;
        RequestRender();
        return;
    }
    if (m_tab != ViewTab::Controls) return;
    const double cur  = (m_config.activeMode == OperatingMode::CPS) ? m_config.cps.rateCps : m_config.kps.rateKps;
    const double mag  = (cur >= 50.0) ? 5.0 : 0.5;
    const double step = (delta > 0) ? mag : -mag;
    if (m_config.activeMode == OperatingMode::CPS) {
        m_config.cps.rateCps = std::clamp(std::round((m_config.cps.rateCps + step) * 2.0) / 2.0, 0.5, kMaxRate);
    } else {
        m_config.kps.rateKps = std::clamp(std::round((m_config.kps.rateKps + step) * 2.0) / 2.0, 0.5, kMaxRate);
    }
    ApplyConfigChange();
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
        if (vk == VK_ESCAPE) {
            if (m_updateDownloading) {
                CancelUpdateDownload();
            } else {
                m_updateModalOpen = false;
                RequestRender();
            }
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
            const double parsed = std::stod(m_rateEditBuf);
            if (std::isfinite(parsed) && parsed > 0.0) {
                const bool isCps     = (m_config.activeMode == OperatingMode::CPS);
                const bool asMs      = isCps ? m_config.cps.displayAsMs : m_config.kps.displayAsMs;
                const double rate    = asMs ? (parsed > 0.5 ? 1000.0 / parsed : kMaxRate) : parsed;
                const double clamped = std::clamp(std::round(rate * 10.0) / 10.0, 0.5, kMaxRate);
                if (isCps) m_config.cps.rateCps = clamped;
                else       m_config.kps.rateKps = clamped;
                SyncEngineAndHotkeys();
                SaveConfigNow();
            }
        } catch (...) {
        }
    }
    m_rateEditBuf.clear();
    RequestRender();
}

void MainWindow::BeginKeyCapture(CaptureKind kind) {
    // While capturing, every key (including the Emergency Kill hotkey) is consumed as the new binding,
    // so the engine must never keep clicking/pressing during capture.
    m_engine.Stop();
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
            if (m_config.kps.sequenceVks.size() < kMaxSequenceKeys) {
                m_config.kps.sequenceVks.push_back(hk.code);
            }
        } else if (kind == CaptureKind::PanicHotkey) {
            if (hk == m_config.cps.hotkey || hk == m_config.kps.hotkey) {
                ShowToast(L"Kill hotkey must be unique", true);
                return;
            }
            m_config.settings.panicHotkey = hk;
        }
        ApplyConfigChange();
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
        ApplyConfigChange();
    }, [this]() {
        m_pickKind = PickKind::None;
        RequestRender();
    });
    RequestRender();
}

} // namespace cadence::ui
