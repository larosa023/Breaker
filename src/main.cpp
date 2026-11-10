// Breaker - entry point
//
//   Breaker.exe                       open Breaker (or bring the running window forward)
//   Breaker.exe system/display        open a page; also breaker://system/display#display.refresh
//   Breaker.exe --tray                start hidden in the tray (Start with Windows)
//   Breaker.exe --watchdog            sign-in check: stays (in the tray) only if Windows changed your settings
//   Breaker.exe --quit                close a running Breaker
//   Breaker.exe --uninstall           remove Breaker (used by Installed apps)
//   (--elevated <file> is the administrator helper; --updated / --rolled-back / --wait-old belong to the updater)
#include "common.h"
#include "config.h"
#include "window.h"
#include "system.h"
#include "suite.h"
#include "features.h"
#include <shellapi.h>
#include <ole2.h>

static HANDLE g_mutex = nullptr;

void ReleaseInstanceMutex() {
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    Log(L"CRASH: exception 0x%08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void InitEngine() {
    RegisterProviderHandlers();
    RegisterSuiteHandlers();
    RegisterAppHandlers();
    EngineInit();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
#ifndef BREAKER_NO_CRASH_HANDLER
    SetUnhandledExceptionFilter(CrashHandler);
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring link, elevatedFile;
    bool quit = false, tray = false, uninstall = false, waitOld = false, watchdog = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--quit") quit = true;
        else if (a == L"--tray") tray = true;
        else if (a == L"--watchdog") watchdog = true;
        else if (a == L"--uninstall") uninstall = true;
        else if (a == L"--wait-old") waitOld = true;
        else if (a == L"--elevated" && i + 1 < argc) elevatedFile = argv[++i];
        else if (a == L"--page" && i + 1 < argc) link = argv[++i];
        else if (a == L"--updated" || a == L"--rolled-back") ++i;   // value handled by the updater
        else if (!a.empty() && a.rfind(L"--", 0) != 0) link = a;
    }
    if (argv) LocalFree(argv);

    // The administrator helper: apply the changes it was handed, report back, exit. No window, no single instance.
    if (!elevatedFile.empty()) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        LogInit();
        InitEngine();
        return ElevatedMain(elevatedFile);
    }

    // One Breaker process: a second launch hands its page to the running window.
    bool updating = wcsstr(GetCommandLineW(), L"--updated") != nullptr;
    for (int attempt = 0; attempt < ((waitOld || updating) ? 50 : 1); ++attempt) {
        g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\Breaker.larosa023.SingleInstance");
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (!(waitOld || updating)) {
            HWND existing = FindWindowW(BREAKER_WNDCLASS, nullptr);
            if (existing && !watchdog) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existing, &pid);
                AllowSetForegroundWindow(pid);
                if (quit || uninstall) {
                    PostMessageW(existing, WM_CLOSE, 1, 0);
                    if (uninstall) Sleep(1500);
                } else {
                    COPYDATASTRUCT cds{0x4252, (DWORD)((link.size() + 1) * sizeof(wchar_t)), (PVOID)link.c_str()};
                    DWORD_PTR r = 0;
                    SendMessageTimeoutW(existing, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &r);
                }
            }
            if (g_mutex) CloseHandle(g_mutex);
            g_mutex = nullptr;
            if (!uninstall) return 0;
            break;
        }
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        Sleep(200);
    }
    if (quit) return 0;

    OleInitialize(nullptr);
    LogInit();
    Log(L"==== Breaker %ls starting (data: %ls)", BREAKER_VERSION, DataDir().c_str());
    ConfigLoad();

    if (uninstall) {
        int r = MessageBoxW(nullptr, L"Uninstall Breaker?\n\nYes removes Breaker but keeps your settings, history and profiles.\nNo removes those too.\n\nWindows settings you changed stay as they are.",
                            L"Breaker", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r != IDCANCEL) UninstallBreaker(r == IDYES);
        return 0;
    }
    InitEngine();

    int driftCount = 0;
    if (watchdog) {
        driftCount = (int)WatchdogCompute().size();
        if (!driftCount) {
            g_config.lastBuild = WindowsBuildText();
            ConfigSave();
            Log(L"Sign-in check: nothing changed");
            return 0;
        }
        Log(L"Sign-in check: %d settings changed outside Breaker", driftCount);
        tray = true;
    }
    if (IsInstalledCopy()) RegisterProtocol();

    HWND hwnd = MainCreate(inst, link, tray, GetCommandLineW());
    if (!hwnd) {
        MessageBoxW(nullptr, L"Breaker couldn't start. Details are in breaker.log.", L"Breaker", MB_ICONERROR);
        return 1;
    }
    if (driftCount) MainNotifyDrift(driftCount);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Log(L"Breaker exited normally");
    OleUninitialize();
    ReleaseInstanceMutex();
    return 0;
}
