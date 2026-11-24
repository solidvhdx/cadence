#include "core/UpdateChecker.hpp"
#include "ui/MainWindow.hpp"

#include <objbase.h>

#include <string_view>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    // Harden DLL search order against CWD / Downloads DLL planting
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    const std::wstring_view cmdLine = lpCmdLine ? std::wstring_view(lpCmdLine) : std::wstring_view{};
    // Set when a previous instance started us and is still shutting down: after a self-update (--updated)
    // or after "Run as Admin" (--relaunch). We then wait for it instead of exiting as a duplicate instance.
    const bool waitForPreviousInstance = (cmdLine.find(L"--updated") != std::wstring_view::npos) ||
                                         (cmdLine.find(L"--relaunch") != std::wstring_view::npos);
    const bool previewUpdateModal      = (cmdLine.find(L"--preview-update") != std::wstring_view::npos);

    // Clean up any leftover .old executable from a previous self-update
    cadence::UpdateChecker::CleanupOldBinary();

    // Prevent multiple instances conflicting over global hotkeys
    constexpr wchar_t kMutexName[] = L"Local\\CadenceSingleInstanceMutex";
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    // Capture the result right away: later calls (e.g. file cleanup) overwrite GetLastError()
    bool otherInstanceRunning = singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS;
    if (otherInstanceRunning) {
        if (waitForPreviousInstance) {
            // Wait up to 5 s for the previous process to finish shutting down
            for (int attempt = 0; attempt < 50 && otherInstanceRunning; ++attempt) {
                CloseHandle(singleInstanceMutex);
                Sleep(100);
                singleInstanceMutex  = CreateMutexW(nullptr, TRUE, kMutexName);
                otherInstanceRunning = singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS;
            }
            // Retry cleanup once old process has exited
            cadence::UpdateChecker::CleanupOldBinary();
        }
        if (otherInstanceRunning) {
            if (HWND existing = FindWindowW(L"CadenceMainWindowClass", nullptr)) {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            CloseHandle(singleInstanceMutex);
            return 0;
        }
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int exitCode = 0;
    {
        cadence::ui::MainWindow app;
        if (app.Create(hInstance, previewUpdateModal)) {
            exitCode = app.RunMessageLoop();
        } else {
            MessageBoxW(nullptr, L"Failed to initialize Direct2D / DirectWrite.", L"Cadence", MB_ICONERROR | MB_OK);
            exitCode = 1;
        }
    }

    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }
    if (singleInstanceMutex) {
        ReleaseMutex(singleInstanceMutex);
        CloseHandle(singleInstanceMutex);
    }
    return exitCode;
}
