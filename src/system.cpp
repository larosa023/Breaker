// Breaker - Windows integration
#include "system.h"
#include "config.h"
#include "features.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <objbase.h>

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Breaker";

// ---------------------------------------------------------------- start with Windows
bool StartWithWindowsEnabled() {
    std::wstring v;
    return RegReadStr(HKEY_CURRENT_USER, kRunKey, L"Breaker", v) && !v.empty();
}

void SetStartWithWindows(bool on) {
    if (on) RegWriteStr(HKEY_CURRENT_USER, kRunKey, L"Breaker", L"\"" + ExePath() + L"\" --tray");
    else RegRemoveValue(HKEY_CURRENT_USER, kRunKey, L"Breaker");
}

void SetWatchdogAtSignIn(bool on) {
    if (on) RegWriteStr(HKEY_CURRENT_USER, kRunKey, L"BreakerWatchdog", L"\"" + ExePath() + L"\" --watchdog");
    else RegRemoveValue(HKEY_CURRENT_USER, kRunKey, L"BreakerWatchdog");
}

// ---------------------------------------------------------------- breaker: links
void RegisterProtocol() {
    const wchar_t* base = L"Software\\Classes\\breaker";
    std::wstring cmd = L"\"" + ExePath() + L"\" \"%1\"", cur;
    if (RegReadStr(HKEY_CURRENT_USER, L"Software\\Classes\\breaker\\shell\\open\\command", nullptr, cur) && cur == cmd) return;
    RegWriteStr(HKEY_CURRENT_USER, base, nullptr, L"URL:Breaker setting");
    RegWriteStr(HKEY_CURRENT_USER, base, L"URL Protocol", L"");
    RegWriteStr(HKEY_CURRENT_USER, L"Software\\Classes\\breaker\\DefaultIcon", nullptr, L"\"" + ExePath() + L"\",0");
    RegWriteStr(HKEY_CURRENT_USER, L"Software\\Classes\\breaker\\shell\\open\\command", nullptr, cmd);
}

// ---------------------------------------------------------------- install / uninstall / portable
static std::wstring KnownFolder(REFKNOWNFOLDERID id, DWORD flags = 0) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, flags, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring InstallDir() {
    std::wstring r = KnownFolder(FOLDERID_UserProgramFiles, KF_FLAG_CREATE);
    return r.empty() ? r : r + L"\\Breaker";
}

bool IsInstalledCopy() {
    std::wstring d = InstallDir();
    return !d.empty() && _wcsicmp(ExeDir().c_str(), d.c_str()) == 0;
}

static std::wstring ShortcutPath() {
    std::wstring r = KnownFolder(FOLDERID_Programs);
    return r.empty() ? r : r + L"\\Breaker.lnk";
}

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target) {
    IShellLinkW* link = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
        link->SetDescription(L"Breaker - every Windows setting in one fast place");
        link->SetIconLocation(target.c_str(), 0);
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    }
    if (pf) pf->Release();
    if (link) link->Release();
    return ok;
}

bool InstallBreaker(std::wstring* error) {
    std::wstring dir = InstallDir();
    if (dir.empty()) { if (error) *error = L"Couldn't find your programs folder."; return false; }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring target = dir + L"\\Breaker.exe";
    if (_wcsicmp(target.c_str(), ExePath().c_str()) != 0) {
        MoveFileExW(target.c_str(), (target + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);   // an older copy may be running
        if (!CopyFileW(ExePath().c_str(), target.c_str(), FALSE)) {
            if (error) *error = L"Couldn't copy Breaker into " + dir + L" (" + ErrorText(GetLastError()) + L").";
            return false;
        }
    }
    MakeShortcut(ShortcutPath(), target);
    RegWriteStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", L"Breaker");
    RegWriteStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", BREAKER_VERSION);
    RegWriteStr(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", L"larosa023");
    RegWriteStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", target + L",0");
    RegWriteStr(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir);
    RegWriteStr(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString", L"\"" + target + L"\" --uninstall");
    RegWriteStr(HKEY_CURRENT_USER, kUninstallKey, L"URLInfoAbout", L"https://github.com/larosa023/Breaker");
    RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1);
    RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1);
    RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize", 2048);
    // entries that point at the exe follow it to its new home
    std::wstring quoted = L"\"" + target + L"\"";
    if (StartWithWindowsEnabled()) RegWriteStr(HKEY_CURRENT_USER, kRunKey, L"Breaker", quoted + L" --tray");
    if (g_config.watchdogAtSignIn) RegWriteStr(HKEY_CURRENT_USER, kRunKey, L"BreakerWatchdog", quoted + L" --watchdog");
    RegWriteStr(HKEY_CURRENT_USER, L"Software\\Classes\\breaker\\shell\\open\\command", nullptr, quoted + L" \"%1\"");
    Log(L"Installed to %ls", dir.c_str());
    return true;
}

void UninstallBreaker(bool keepSettings) {
    SetStartWithWindows(false);
    SetWatchdogAtSignIn(false);
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\breaker");
    std::wstring dir = ExeDir(), data = DataDir();
    // delete the running exe (and settings, if asked) once this process has exited
    std::wstring cmd = L"cmd.exe /c timeout /t 2 /nobreak >nul & del /f /q \"" + ExePath() + L"\" \"" + ExePath() + L".old\" & rmdir \"" + dir + L"\"";
    if (!keepSettings && !IsPortable()) cmd += L" & rmdir /s /q \"" + data + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    Log(L"Uninstalled");
}

bool IsPortable() { return FileExists(ExeDir() + L"\\portable") || FileExists(ExeDir() + L"\\portable.txt"); }

bool SetPortable(bool on, std::wstring* error) {
    static const wchar_t* files[] = {L"config.json", L"history.json", L"desired.json", L"cache.json"};
    std::wstring marker = ExeDir() + L"\\portable", from = DataDir();
    if (on) {
        std::wstring to = ExeDir() + L"\\data";
        SHCreateDirectoryExW(nullptr, to.c_str(), nullptr);
        HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { if (error) *error = L"Breaker's folder isn't writable, so it can't run portable from there."; return false; }
        CloseHandle(h);
        for (auto f : files) CopyFileW((from + L"\\" + f).c_str(), (to + L"\\" + f).c_str(), FALSE);
    } else {
        std::wstring to = KnownFolder(FOLDERID_RoamingAppData) + L"\\Breaker";
        SHCreateDirectoryExW(nullptr, to.c_str(), nullptr);
        for (auto f : files) CopyFileW((from + L"\\" + f).c_str(), (to + L"\\" + f).c_str(), FALSE);
        DeleteFileW(marker.c_str());
        DeleteFileW((ExeDir() + L"\\portable.txt").c_str());
    }
    return true;
}

// ---------------------------------------------------------------- foreground
void ForceForeground(HWND hwnd) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else ShowWindow(hwnd, SW_SHOW);
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    if (fgThread && fgThread != me) AttachThreadInput(fgThread, me, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    if (fgThread && fgThread != me) AttachThreadInput(fgThread, me, FALSE);
}

// ---------------------------------------------------------------- tray
static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;

void TrayAdd(HWND owner) {
    if (g_trayAdded) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = owner;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_BREAKER_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(g_nid.szTip, L"Breaker");
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    if (g_trayAdded) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    }
}

void TrayRemove() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

void TrayNotify(const std::wstring& title, const std::wstring& text) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TrayOnMessage(HWND owner, LPARAM lParam) {
    UINT ev = LOWORD(lParam);
    if (ev == WM_LBUTTONUP || ev == NIN_SELECT || ev == NIN_KEYSELECT || ev == NIN_BALLOONUSERCLICK) {
        ForceForeground(owner);
        if (ev == NIN_BALLOONUSERCLICK) UiNavigate(L"watchdog");
        return;
    }
    if (ev != WM_CONTEXTMENU && ev != WM_RBUTTONUP) return;
    HMENU menu = CreatePopupMenu(), profiles = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open Breaker");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // quick toggles: the pinned on/off settings
    std::vector<std::wstring> ids;
    for (auto& id : g_config.pinned) {
        const Def* d = FindDef(id);
        std::wstring v;
        if (!d || d->ctl != CtlToggle || ids.size() >= 12) continue;
        CachedValue(id, v);
        AppendMenuW(menu, MF_STRING | (v == L"1" ? MF_CHECKED : 0), 100 + ids.size(), d->name);
        ids.push_back(id);
    }
    if (!ids.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < g_config.profiles.size() && i < 30; ++i)
        AppendMenuW(profiles, MF_STRING | (g_config.profiles[i].name == g_config.activeProfile ? MF_CHECKED : 0), 200 + i, g_config.profiles[i].name.c_str());
    if (!g_config.profiles.empty()) AppendMenuW(menu, MF_POPUP, (UINT_PTR)profiles, L"Profiles");
    AppendMenuW(menu, MF_STRING, 2, L"Windows Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"Quit Breaker");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) ForceForeground(owner);
    else if (cmd == 2) OpenTarget(L"ms-settings:");
    else if (cmd == 3) PostMessageW(owner, WM_CLOSE, 1, 0);
    else if (cmd >= 200 && cmd - 200 < (int)g_config.profiles.size()) ProfileApply(g_config.profiles[cmd - 200].name);
    else if (cmd >= 100 && cmd - 100 < (int)ids.size()) {
        std::wstring v;
        CachedValue(ids[cmd - 100], v);
        UiApply({{ids[cmd - 100], v == L"1" ? L"0" : L"1", L""}});
    }
}

// ---------------------------------------------------------------- Win+I
// A low-level keyboard hook, installed only while the option is on. It reacts to exactly one
// combination: I pressed while a Windows key is held, with no Ctrl or Alt.
static HHOOK g_hook = nullptr;
static HWND g_hookOwner = nullptr;
static bool g_swallowUp = false;

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        if (k->vkCode == 'I' && !(k->flags & LLKHF_INJECTED)) {
            bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
            bool ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000, alt = GetAsyncKeyState(VK_MENU) & 0x8000, shift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
            if (down && win && !ctrl && !alt) {
                if (!g_swallowUp) {
                    // A harmless key press, so releasing the Windows key afterwards doesn't open Start
                    INPUT in[2] = {};
                    in[0].type = in[1].type = INPUT_KEYBOARD;
                    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
                    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, in, sizeof(INPUT));
                    PostMessageW(g_hookOwner, WM_BREAKER_SHOW, shift ? 2 : 1, 0);
                }
                g_swallowUp = true;
                return 1;
            }
            if (!down && g_swallowUp) { g_swallowUp = false; return 1; }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void WinIHookSet(HWND owner, bool on) {
    g_hookOwner = owner;
    if (on && !g_hook) {
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
        Log(g_hook ? L"Win+I shortcut on" : L"Couldn't install the Win+I shortcut (error %lu)", GetLastError());
    } else if (!on && g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        Log(L"Win+I shortcut off");
    }
}
