#include "ui/MainWindow.hpp"
#include "ui/UiHelpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <stop_token>
#include <utility>

// Update modal and the background update check / download workers.

namespace cadence::ui {

namespace {

// Runs a callable when the scope ends (used to report "worker finished" on every exit path)
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F fn) : m_fn(std::move(fn)) {}
    ~ScopeExit() { m_fn(); }
    ScopeExit(const ScopeExit&)            = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F m_fn;
};

// Release notes are Markdown; the modal shows a short plain-text excerpt.
std::wstring FormatReleaseNotes(std::string_view markdown, size_t maxLines) {
    std::wstring out;
    size_t shown = 0;
    bool truncated = false;
    size_t pos = 0;
    while (pos <= markdown.size()) {
        const size_t nl = markdown.find('\n', pos);
        std::string_view line = markdown.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? markdown.size() + 1 : nl + 1;

        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        while (!line.empty() && line.back() == ' ')  line.remove_suffix(1);
        std::string_view bullet;
        while (!line.empty() && line.front() == '#') line.remove_prefix(1);           // headings
        if (line.starts_with("- ") || line.starts_with("* ")) {                        // list items
            line.remove_prefix(2);
            bullet = "\xE2\x80\xA2 ";
        }
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (line.empty()) continue;

        std::string clean(bullet);
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '`') continue;                                              // inline code
            if (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '*') { ++i; continue; } // bold
            clean.push_back(line[i]);
        }
        if (shown == maxLines) {
            truncated = true;
            break;
        }
        if (!out.empty()) out += L'\n';
        out += Utf8ToWide(clean);
        ++shown;
    }
    if (truncated) {
        out += L" …";
    }
    return out;
}

} // namespace

void MainWindow::RenderUpdateModal(float w, float h) {
    if (!m_availableUpdate) return;
    const auto& rel = *m_availableUpdate;
    const D2D1_COLOR_F accent = ActiveAccent();

    const UpdateStage stage = (m_updateDownloading && m_updateShared)
        ? static_cast<UpdateStage>(m_updateShared->stage.load(std::memory_order_acquire))
        : UpdateStage::Idle;
    const uint64_t dlBytes  = (m_updateDownloading && m_updateShared)
        ? m_updateShared->bytesDownloaded.load(std::memory_order_relaxed) : 0;
    const uint64_t totBytes = (m_updateDownloading && m_updateShared)
        ? m_updateShared->bytesTotal.load(std::memory_order_relaxed) : 0;
    const bool canCancel = m_updateDownloading && (stage != UpdateStage::Installing);

    // Dimmed backdrop below titlebar so window dragging, minimize & close remain crisp and accessible
    m_renderer.FillRect({0.0f, ThemeTokens::kTitlebarHeight, w, h}, HexColor(0x000000, 0.64f));

    // Release notes excerpt (formatted once per release, shown while the user decides)
    if (m_updateNotesTag != rel.tagName) {
        m_updateNotesTag  = rel.tagName;
        m_updateNotesText = FormatReleaseNotes(rel.releaseNotes, 4);
    }
    const bool  showNotes = !m_updateDownloading && !m_updateNotesText.empty();
    const float notesH    = 76.0f;
    const float shift     = showNotes ? notesH + 10.0f : 0.0f; // Everything below the notes moves down

    const float cardW = w - 48.0f;
    const float cardH = 216.0f + shift;
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
    const std::wstring headerTitle = !m_updateDownloading
        ? L"Update available"
        : (stage == UpdateStage::Installing ? L"Installing update" : L"Updating Cadence");
    m_renderer.DrawText(headerTitle, {innerL + 24.0f, card.top + 15.0f, innerR - 28.0f, card.top + 39.0f},
                        TextStyle::Title, ThemeTokens::kTextPrimary);

    const bool closeEnabled = (!m_updateDownloading || canCancel);
    const D2D1_RECT_F closeRc{innerR - 22.0f, card.top + 15.0f, innerR + 2.0f, card.top + 39.0f};
    if (closeEnabled && IsHovered("upd_close")) {
        m_renderer.FillRoundedRect(closeRc, 5.0f, HexColor(0xFFFFFF, 0.06f));
    }
    m_renderer.DrawCloseIcon((closeRc.left + closeRc.right) * 0.5f, (closeRc.top + closeRc.bottom) * 0.5f,
                             (closeEnabled && IsHovered("upd_close")) ? ThemeTokens::kTextPrimary : ThemeTokens::kTextMuted);
    if (closeEnabled) {
        RegisterHit(closeRc, "upd_close", [this]() {
            if (m_updateDownloading) {
                CancelUpdateDownload();
            }
            m_updateModalOpen = false;
            RequestRender();
        }, false, true);
    }

    // Version comparison pill: v1.0.2 -> v1.0.3
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

    // Description & live status line
    std::wstring primaryLine = L"A newer version of Cadence is ready.";
    std::wstring statusLine  = L"Downloads Cadence.exe and restarts automatically.";
    if (m_updateDownloading) {
        if (stage == UpdateStage::Connecting) {
            primaryLine = L"Connecting to GitHub...";
            statusLine  = L"Starting secure HTTPS download";
        } else if (stage == UpdateStage::Downloading) {
            const uint64_t dlKb = (dlBytes + 512) / 1024;
            if (totBytes > 0) {
                const uint64_t totKb = std::max<uint64_t>(1, (totBytes + 512) / 1024);
                const int pct = std::clamp(static_cast<int>((dlBytes * 100) / totBytes), 0, 100);
                primaryLine = std::format(L"Downloading Cadence.exe... {}%", pct);
                statusLine  = std::format(L"{} KB / {} KB", dlKb, totKb);
            } else {
                primaryLine = L"Downloading Cadence.exe...";
                statusLine  = std::format(L"{} KB received", dlKb);
            }
        } else if (stage == UpdateStage::Installing) {
            primaryLine = L"Installing and restarting...";
            statusLine  = L"Verifying binary and launching new version";
        }
    }

    m_renderer.DrawText(primaryLine, {innerL, card.top + 88.0f, innerR, card.top + 106.0f}, TextStyle::Body, ThemeTokens::kTextPrimary);
    m_renderer.DrawText(statusLine,  {innerL, card.top + 107.0f, innerR, card.top + 123.0f}, TextStyle::Caption, ThemeTokens::kTextMuted);

    if (showNotes) {
        const D2D1_RECT_F notesRc{innerL, card.top + 130.0f, innerR, card.top + 130.0f + notesH};
        m_renderer.FillRoundedRect(notesRc, 7.0f, ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(notesRc, 7.0f, ThemeTokens::kHairline);
        m_renderer.DrawText(m_updateNotesText,
                            {notesRc.left + 10.0f, notesRc.top + 7.0f, notesRc.right - 10.0f, notesRc.bottom - 7.0f},
                            TextStyle::Caption, ThemeTokens::kTextSecondary,
                            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    // Primary slot: CTA button when idle, live animated progress bar when downloading/installing
    const D2D1_RECT_F updRc{innerL, card.top + shift + 134.0f, innerR, card.top + shift + 168.0f};
    if (!m_updateDownloading) {
        m_renderer.FillRoundedRect(updRc, ThemeTokens::kRadiusButton, ActiveAccent(IsHovered("upd_now")));
        m_renderer.DrawRoundedRect(updRc, ThemeTokens::kRadiusButton, HexColor(0xFFFFFF, 0.14f));
        m_renderer.DrawLine(updRc.left + 8.0f, updRc.top + 1.0f, updRc.right - 8.0f, updRc.top + 1.0f, HexColor(0xFFFFFF, 0.15f));
        m_renderer.DrawText(L"Update now", updRc, TextStyle::BodyMedium, ThemeTokens::kTextWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
        RegisterHit(updRc, "upd_now", [this]() {
            if (m_updatePreviewOnly) {
                ShowToast(L"Preview mode — update button works!", false);
                return;
            }
            StartUpdateDownloadAsync();
        }, false, true);
    } else {
        m_renderer.FillRoundedRect(updRc, ThemeTokens::kRadiusButton, ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(updRc, ThemeTokens::kRadiusButton, ThemeTokens::kHairlineStrong);

        const float trackPad = 12.0f;
        const float trackH   = 8.0f;
        const float cy       = (updRc.top + updRc.bottom) * 0.5f;
        const D2D1_RECT_F trackRc{updRc.left + trackPad, cy - trackH * 0.5f, updRc.right - trackPad, cy + trackH * 0.5f};
        const float trackW = std::max(1.0f, trackRc.right - trackRc.left);
        m_renderer.FillRoundedRect(trackRc, 4.0f, HexColor(0x09090D));
        m_renderer.DrawRoundedRect(trackRc, 4.0f, ThemeTokens::kHairline);

        if (stage == UpdateStage::Downloading && totBytes > 0) {
            const float fillW = std::clamp(trackW * m_updateProgVisual, m_updateProgVisual > 0.005f ? 8.0f : 0.0f, trackW);
            if (fillW > 0.5f) {
                m_renderer.FillRoundedRect({trackRc.left, trackRc.top, trackRc.left + fillW, trackRc.bottom}, 4.0f, accent);
            }
        } else if (stage == UpdateStage::Installing) {
            m_renderer.FillRoundedRect(trackRc, 4.0f, ThemeTokens::kEmerald);
            // Smooth moving highlight glint while Windows verifies & launches the new exe
            const float segW   = trackW * 0.28f;
            const float travel = trackW - segW;
            const float wave   = 0.5f - 0.5f * std::cos(m_updateAnimPhase * 6.2831853f);
            const float segL   = trackRc.left + travel * wave;
            m_renderer.FillRoundedRect({segL, trackRc.top, segL + segW, trackRc.bottom}, 4.0f, HexColor(0xFFFFFF, 0.28f));
        } else {
            // Connecting or unknown Content-Length: smooth indeterminate gliding bar
            const float segW   = trackW * 0.34f;
            const float travel = trackW - segW;
            const float wave   = 0.5f - 0.5f * std::cos(m_updateAnimPhase * 6.2831853f);
            const float segL   = trackRc.left + travel * wave;
            m_renderer.FillRoundedRect({segL, trackRc.top, segL + segW, trackRc.bottom}, 4.0f, accent);
        }
    }

    // Secondary row: Later | Ignore updates (idle) OR Cancel (downloading) OR Restarting (installing)
    if (!m_updateDownloading) {
        const float btnGap = 8.0f;
        const float btnW   = (innerR - innerL - btnGap) * 0.5f;
        const D2D1_RECT_F laterRc{innerL, card.top + shift + 176.0f, innerL + btnW, card.top + shift + 202.0f};
        const D2D1_RECT_F ignRc  {laterRc.right + btnGap, card.top + shift + 176.0f, innerR, card.top + shift + 202.0f};

        m_renderer.FillRoundedRect(laterRc, 6.0f, IsHovered("upd_later") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(laterRc, 6.0f, ThemeTokens::kHairlineStrong);
        m_renderer.DrawText(L"Later", laterRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

        m_renderer.FillRoundedRect(ignRc, 6.0f, IsHovered("upd_ignore") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
        m_renderer.DrawRoundedRect(ignRc, 6.0f, ThemeTokens::kHairline);
        m_renderer.DrawText(L"Ignore updates", ignRc, TextStyle::Body,
                            IsHovered("upd_ignore") ? ThemeTokens::kTextPrimary : ThemeTokens::kTextSecondary,
                            DWRITE_TEXT_ALIGNMENT_CENTER);

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
    } else {
        const D2D1_RECT_F cancelRc{innerL, card.top + shift + 176.0f, innerR, card.top + shift + 202.0f};
        if (canCancel) {
            m_renderer.FillRoundedRect(cancelRc, 6.0f, IsHovered("upd_cancel") ? ThemeTokens::kBgElevated : ThemeTokens::kBgRecessed);
            m_renderer.DrawRoundedRect(cancelRc, 6.0f, ThemeTokens::kHairlineStrong);
            m_renderer.DrawText(L"Cancel", cancelRc, TextStyle::Body, ThemeTokens::kTextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            RegisterHit(cancelRc, "upd_cancel", [this]() {
                CancelUpdateDownload();
            }, false, true);
        } else {
            m_renderer.FillRoundedRect(cancelRc, 6.0f, ThemeTokens::kBgRecessed);
            m_renderer.DrawRoundedRect(cancelRc, 6.0f, ThemeTokens::kHairline);
            m_renderer.DrawText(L"Restarting Cadence...", cancelRc, TextStyle::Body, ThemeTokens::kTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

void MainWindow::StartUpdateCheckAsync() {
    auto shared = m_updateShared;
    if (!shared || m_updateCheckThread.joinable()) return; // One check per run

    {
        std::scoped_lock lock(shared->mtx);
        ++shared->activeChecks;
    }
    m_updateCheckThread = std::jthread([shared](std::stop_token stop) {
        const ScopeExit done([&shared]() {
            {
                std::scoped_lock lock(shared->mtx);
                --shared->activeChecks;
            }
            shared->cv.notify_all();
        });

        auto rel = UpdateChecker::CheckForUpdate();
        if (!rel || stop.stop_requested()) return;

        HWND target = nullptr;
        {
            std::scoped_lock lock(shared->mtx);
            shared->found = std::move(rel);
            target        = shared->hwnd;
        }
        if (target) {
            PostMessageW(target, WM_APP_UPDATE_AVAILABLE, 0, 0);
        }
    });
}

void MainWindow::StopUpdateWorkers() {
    if (!m_updateShared) return;
    const auto shared = m_updateShared;

    m_updateCheckThread.request_stop();
    m_updateDownloadThread.request_stop();
    if (m_updateCancelToken) {
        m_updateCancelToken->store(true, std::memory_order_release);
    }

    std::unique_lock lock(shared->mtx);
    shared->hwnd = nullptr; // Workers must not post to a window that is going away
    // Downloads stop within one read; a pending DNS/TLS handshake can take longer, so wait briefly only.
    const bool finished = shared->cv.wait_for(lock, std::chrono::milliseconds(1500), [&shared]() {
        return shared->activeChecks == 0 && shared->activeDownloads == 0;
    });
    lock.unlock();

    for (std::jthread* worker : {&m_updateCheckThread, &m_updateDownloadThread}) {
        if (!worker->joinable()) continue;
        if (finished) {
            worker->join();
        } else {
            worker->detach(); // Still inside WinHTTP: it only uses `shared` and exits on its own
        }
    }
}

void MainWindow::CancelUpdateDownload() {
    if (!m_updateDownloading || !m_updateShared) return;
    if (static_cast<UpdateStage>(m_updateShared->stage.load(std::memory_order_acquire)) == UpdateStage::Installing) {
        return; // Cannot cancel once binary swap is in progress
    }
    if (m_updateCancelToken) {
        m_updateCancelToken->store(true, std::memory_order_release);
        m_updateCancelToken.reset();
    }
    ++m_updateAttemptId;
    m_updateShared->attemptId.store(m_updateAttemptId, std::memory_order_release);
    m_updateShared->stage.store(static_cast<uint8_t>(UpdateStage::Idle), std::memory_order_release);
    m_updateDownloading = false;
    m_updateProgVisual  = 0.0f;
    ShowToast(L"Update cancelled", false);
}

void MainWindow::StartUpdateDownloadAsync() {
    if (!m_availableUpdate || m_updateDownloading) return;

    if (m_availableUpdate->downloadUrl.empty() || m_availableUpdate->sha256Hex.empty()) {
        const std::wstring url = Utf8ToWide(m_availableUpdate->releaseUrl);
        ShellExecuteW(m_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        m_updateModalOpen = false;
        RequestRender();
        return;
    }

    auto shared = m_updateShared;
    if (!shared) return;

    const std::string dlUrl       = m_availableUpdate->downloadUrl;
    const std::string expectedSha = m_availableUpdate->sha256Hex;
    const std::filesystem::path curExe = UpdateChecker::CurrentExePath();
    if (curExe.empty()) {
        ShowToast(L"Could not locate current executable", true);
        return;
    }

    const std::filesystem::path dest = UpdateChecker::GenerateRandomTempPath(curExe.parent_path());
    if (dest.empty()) {
        ShowToast(L"Could not allocate secure temp path", true);
        return;
    }

    // Stop clicking & persist config before downloading/applying so the background swap never touches UI state
    m_engine.Stop();
    SaveConfigNow();

    const uint64_t attempt = ++m_updateAttemptId;

    // A cancelled earlier download may still be finishing a network read: never block the UI on it
    if (m_updateDownloadThread.joinable()) {
        bool previousRunning = false;
        {
            std::scoped_lock lock(shared->mtx);
            previousRunning = shared->activeDownloads > 0;
        }
        if (previousRunning) {
            m_updateDownloadThread.detach();
        } else {
            m_updateDownloadThread.join();
        }
    }

    auto cancelToken = std::make_shared<std::atomic<bool>>(false);
    m_updateCancelToken = cancelToken;

    shared->attemptId.store(attempt, std::memory_order_release);
    shared->bytesDownloaded.store(0, std::memory_order_relaxed);
    shared->bytesTotal.store(0, std::memory_order_relaxed);
    shared->stage.store(static_cast<uint8_t>(UpdateStage::Connecting), std::memory_order_release);

    m_updateDownloading = true;
    m_updateProgVisual  = 0.0f;
    m_updateAnimPhase   = 0.0f;
    StartAnimTimer();
    RequestRender();

    {
        std::scoped_lock lock(shared->mtx);
        ++shared->activeDownloads;
    }
    m_updateDownloadThread = std::jthread([shared, cancelToken, dlUrl, expectedSha, dest, attempt](std::stop_token stop) {
        const ScopeExit done([&shared]() {
            {
                std::scoped_lock lock(shared->mtx);
                --shared->activeDownloads;
            }
            shared->cv.notify_all();
        });
        const std::stop_callback onStop(stop, [&cancelToken]() {
            cancelToken->store(true, std::memory_order_release);
        });

        auto isStaleOrCancelled = [&]() noexcept -> bool {
            return cancelToken->load(std::memory_order_acquire) ||
                   shared->attemptId.load(std::memory_order_acquire) != attempt;
        };

        const auto downloaded = UpdateChecker::DownloadBinary(
            dlUrl,
            dest,
            expectedSha,
            [shared, attempt](uint64_t dl, uint64_t tot) {
                if (shared->attemptId.load(std::memory_order_acquire) != attempt) return;
                shared->bytesDownloaded.store(dl, std::memory_order_relaxed);
                shared->bytesTotal.store(tot, std::memory_order_relaxed);
                shared->stage.store(static_cast<uint8_t>(UpdateStage::Downloading), std::memory_order_release);
            },
            cancelToken.get());

        UpdateOutcome outcome = UpdateOutcome::Failed;
        std::optional<UpdateError> error;
        if (isStaleOrCancelled()) {
            std::error_code ec;
            std::filesystem::remove(dest, ec);
            outcome = UpdateOutcome::Cancelled;
        } else if (!downloaded) {
            error = downloaded.error();
        } else {
            shared->stage.store(static_cast<uint8_t>(UpdateStage::Installing), std::memory_order_release);
            // Run file swap + locked SHA-256/PE64 re-check + CreateProcessW OFF the UI thread!
            if (const auto applied = UpdateChecker::ApplyUpdateAndRelaunch(dest, expectedSha); applied) {
                outcome = UpdateOutcome::Applied;
            } else {
                std::error_code ec;
                std::filesystem::remove(dest, ec);
                error = applied.error();
            }
        }

        HWND target = nullptr;
        {
            std::scoped_lock lock(shared->mtx);
            if (shared->attemptId.load(std::memory_order_acquire) == attempt) {
                shared->outcome          = outcome;
                shared->error            = error;
                shared->outcomeAttemptId = attempt;
                target                   = shared->hwnd;
            }
        }
        if (target) {
            PostMessageW(target, WM_APP_UPDATE_DONE, 0, 0);
        }
    });
}

} // namespace cadence::ui
