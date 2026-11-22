#include "core/UpdateChecker.hpp"
#include "ui/MainWindow.hpp"

#include <objbase.h>

#include <string_view>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    const std::wstring_view cmdLine = lpCmdLine ? std::wstring_view(lpCmdLine) : std::wstring_view{};
    const bool isRestartAfterUpdate = (cmdLine.find(L"--updated") != std::wstring_view::npos);
    const bool previewUpdateModal   = (cmdLine.find(L"--preview-update") != std::wstring_view::npos);

    // Clean up any leftover .old executable from a previous self-update
    cadence::UpdateChecker::CleanupOldBinary();

    // Prevent multiple instances conflicting over global hotkeys
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\CadenceSingleInstanceMutex");
    if (singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (isRestartAfterUpdate) {
            // Wait briefly for the previous process to finish shutting down after self-update
            for (int attempt = 0; attempt < 25; ++attempt) {
                CloseHandle(singleInstanceMutex);
                Sleep(100);
                singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\CadenceSingleInstanceMutex");
                if (singleInstanceMutex && GetLastError() != ERROR_ALREADY_EXISTS) {
                    break;
                }
            }
            // Retry cleanup once old process has exited
            cadence::UpdateChecker::CleanupOldBinary();
        }
        if (singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
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
