#include "ui/MainWindow.hpp"

#include <objbase.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Prevent multiple instances conflicting over global hotkeys
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\CadenceSingleInstanceMutex");
    if (singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"CadenceMainWindowClass", nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleInstanceMutex);
        return 0;
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int exitCode = 0;
    {
        cadence::ui::MainWindow app;
        if (app.Create(hInstance)) {
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
