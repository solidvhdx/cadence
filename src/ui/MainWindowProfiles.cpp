#include "ui/MainWindow.hpp"
#include "ui/UiHelpers.hpp"

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <format>

// Profile popover, profile naming and JSON import / export.

namespace cadence::ui {

namespace {

// Keys that let a shared profile leave the game: open Start / context menus or confirm typed commands
bool IsRiskyMacroKey(uint16_t vk) noexcept {
    switch (vk) {
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_RETURN:
            return true;
        default:
            return false;
    }
}

bool ProfileUsesRiskyKeys(const Profile& p) noexcept {
    if (p.mode != OperatingMode::KPS) {
        return false; // ActivateProfile only applies the KPS part of KPS profiles
    }
    if (p.kps.inputMode == KeyInputMode::Sequence) {
        return std::any_of(p.kps.sequenceVks.begin(), p.kps.sequenceVks.end(), IsRiskyMacroKey);
    }
    return IsRiskyMacroKey(p.kps.targetVk);
}

// Plain-text summary of what an imported profile will press, shown before it is added
std::wstring DescribeImportedProfile(const Profile& p, bool risky) {
    std::wstring name = Utf8ToWide(p.name);
    if (name.size() > 48) {
        name.resize(48);
        if (IS_HIGH_SURROGATE(name.back())) name.pop_back();
        name += L"…";
    }

    std::wstring text = L"Name: " + name + L"\n";
    if (p.mode == OperatingMode::CPS) {
        static constexpr const wchar_t* kButtons[] = {L"Left", L"Right", L"Middle"};
        text += std::format(L"Mode: Mouse, {:.1f} clicks per second ({} button)\n",
                            p.cps.rateCps, kButtons[std::min<size_t>(static_cast<size_t>(p.cps.button), 2)]);
    } else {
        text += std::format(L"Mode: Keyboard, {:.1f} presses per second\n", p.kps.rateKps);
        std::wstring keys;
        if (p.kps.inputMode == KeyInputMode::Sequence) {
            for (size_t i = 0; i < p.kps.sequenceVks.size(); ++i) {
                if (i > 0) keys += L" → ";
                keys += FormatVkName(p.kps.sequenceVks[i]);
            }
        } else {
            if (p.kps.modCtrl)  keys += L"Ctrl + ";
            if (p.kps.modShift) keys += L"Shift + ";
            if (p.kps.modAlt)   keys += L"Alt + ";
            keys += FormatVkName(p.kps.targetVk);
        }
        text += L"Keys: " + keys + L"\n";
    }
    text += L"Hotkey: " + FormatHotkey(p.mode == OperatingMode::CPS ? p.cps.hotkey : p.kps.hotkey) + L"\n";

    if (risky) {
        text += L"\nWARNING: This profile presses Windows / Menu / Enter keys. A profile from someone else "
                L"can use them to open programs and type commands on your PC. Only import it if you trust the source.\n";
    }
    text += L"\nImport and activate this profile?";
    return text;
}

} // namespace

void MainWindow::RenderProfilePopover() {
    const auto& list = m_profiles->Profiles();
    const float rowH = 26.0f;
    const float footerH = m_namingProfile ? 76.0f : 68.0f;
    const int totalRows = static_cast<int>(list.size());
    const int maxVisRows = std::max(3, static_cast<int>((m_heightDip - 52.0f - footerH) / rowH));
    const int visRows = std::min(totalRows, maxVisRows);
    const int maxScroll = std::max(0, totalRows - visRows);
    m_profileScroll = std::clamp(m_profileScroll, 0, maxScroll);

    const float popH = static_cast<float>(visRows) * rowH + footerH;
    const float popLeft = std::clamp(m_profileBtnLeft, 88.0f, m_widthDip - 236.0f);
    const D2D1_RECT_F rc{popLeft, 38.0f, popLeft + 222.0f, 38.0f + popH};

    m_renderer.FillRoundedRect({rc.left, rc.top + 4.0f, rc.right, rc.bottom + 6.0f}, 9.0f, HexColor(0x000000, 0.45f));
    m_renderer.FillRoundedRect(rc, 9.0f, ThemeTokens::kBgPopover);
    m_renderer.DrawRoundedRect(rc, 9.0f, ThemeTokens::kHairlineStrong);

    float py = rc.top + 6.0f;
    const float listTop = py;
    const float rightInset = (maxScroll > 0) ? 10.0f : 6.0f;
    for (int row = 0; row < visRows; ++row) {
        const size_t i = static_cast<size_t>(m_profileScroll + row);
        const auto& p = list[i];
        const bool active = (p.id == m_config.activeProfileId);
        const D2D1_RECT_F r{rc.left + 6.0f, py, rc.right - (rightInset + 22.0f), py + rowH};
        const std::string id = std::format("prof_{}", i);

        if (active || IsHovered(id)) {
            m_renderer.FillRoundedRect({rc.left + 6.0f, py, rc.right - rightInset, py + rowH}, 5.0f,
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
            const D2D1_RECT_F delRc{rc.right - (rightInset + 20.0f), py + 3.0f, rc.right - (rightInset + 2.0f), py + rowH - 3.0f};
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

    if (maxScroll > 0) {
        const float trackH = static_cast<float>(visRows) * rowH;
        const float thumbH = std::max(16.0f, trackH * (static_cast<float>(visRows) / static_cast<float>(totalRows)));
        const float thumbT = static_cast<float>(m_profileScroll) / static_cast<float>(maxScroll);
        const float thumbY = listTop + thumbT * (trackH - thumbH);
        m_renderer.FillRoundedRect({rc.right - 6.0f, thumbY, rc.right - 3.0f, thumbY + thumbH}, 1.5f, HexColor(0xFFFFFF, 0.22f));
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

void MainWindow::ExportActiveProfileDialog() {
    wchar_t fileBuf[MAX_PATH] = L"cadence_profile.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hwnd;
    ofn.lpstrFilter = L"JSON Profile (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = fileBuf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    m_engine.Stop();
    m_systemDialogOpen = true;
    const BOOL picked = GetSaveFileNameW(&ofn);
    m_systemDialogOpen = false;
    if (picked) {
        if (m_profiles->ExportProfile(m_config.activeProfileId, fileBuf)) {
            ShowToast(L"Profile exported", false);
        } else {
            ShowToast(L"Failed to export profile", true);
        }
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
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    m_engine.Stop();
    m_systemDialogOpen = true;
    const BOOL picked = GetOpenFileNameW(&ofn);
    m_systemDialogOpen = false;
    if (!picked) {
        return;
    }

    Profile imported{};
    if (!ProfileManager::ReadProfileFile(fileBuf, imported)) {
        ShowToast(L"Invalid profile file", true);
        return;
    }

    // An imported profile is a click/key macro from a possibly untrusted source:
    // show exactly what it will press and only add + activate it after explicit confirmation.
    const bool risky = ProfileUsesRiskyKeys(imported);
    const std::wstring summary = DescribeImportedProfile(imported, risky);
    const UINT flags = static_cast<UINT>(MB_YESNO | MB_DEFBUTTON2 | (risky ? MB_ICONWARNING : MB_ICONQUESTION));
    m_systemDialogOpen = true;
    const int answer = MessageBoxW(m_hwnd, summary.c_str(), L"Import profile", flags);
    m_systemDialogOpen = false;
    if (answer != IDYES) {
        ShowToast(L"Import cancelled", false);
        return;
    }

    m_profiles->ActivateProfile(m_profiles->AddImportedProfile(std::move(imported)));
    SyncEngineAndHotkeys();
    UpdateWindowSizeForLayout();
    SaveConfigNow();
    StartAnimTimer();
    ShowToast(L"Profile imported", false);
}

} // namespace cadence::ui
