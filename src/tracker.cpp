// Breaker - Screen time tracker ("Breaker.exe --tracker").
//
// A tiny headless process that starts with Windows: one hidden window, three timers, no graphics,
// no settings engine. Every few seconds it notes which app is in front and adds that time to today.
// Time doesn't count while the PC is locked, asleep, or idle (no input for a couple of minutes, unless a
// game or video is keeping the screen on). Totals are written to <data>\screentime every few minutes.
#include "tally.h"
#include "config.h"
#include "json.h"
#include <wtsapi32.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <powrprof.h>
#include <algorithm>
#include <set>

using namespace tally;

namespace {
struct ProcInfo {
    std::wstring key, name, path, exe;
    bool game = false, skip = false, retro = false, knownApp = false;
};
}

constexpr UINT_PTR T_SAMPLE = 1, T_FLUSH = 2, T_MINUTE = 3, T_BALLOON = 4;
#ifdef BREAKER_TALLY_FAST
constexpr UINT kSampleMs = 1000;           // test builds
#else
constexpr UINT kSampleMs = 5000;
#endif
constexpr UINT WM_TRACKER_TRAY = WM_APP + 22;

static HWND g_wnd = nullptr;
static HANDLE g_mutex = nullptr;
static std::wstring g_startPath;
static DayMap g_days;                          // the current month, in seconds
static int g_month = 0;                        // yyyymm held in g_days
static std::map<std::wstring, long long> g_ms; // leftover milliseconds per app
static std::map<std::wstring, AppInfo> g_apps;
static bool g_dirty = false, g_appsDirty = false, g_locked = false, g_asleep = false, g_iconShown = false;
static ULONGLONG g_lastTick = 0;
static std::map<DWORD, ProcInfo> g_procs;
static std::wstring g_fullKey;
static int g_fullStreak = 0, g_minutes = 0;
static std::wstring g_balloonLink;
// notifications already shown: "limit|<day>|<key>|<level>", "week|<weekstart>"
static std::set<std::wstring> g_shown;

// ============================================================ helpers
static std::wstring Lower(std::wstring s) { return ToLower(std::move(s)); }

static std::wstring FileName(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

static std::wstring VersionString(const std::wstring& path, const wchar_t* field) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return L"";
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return L"";
    struct LangCp { WORD lang, cp; }* tr = nullptr;
    UINT len = 0;
    std::vector<std::wstring> blocks;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && tr && len >= sizeof(LangCp)) {
        wchar_t b[64];
        swprintf(b, 64, L"\\StringFileInfo\\%04x%04x\\", tr->lang, tr->cp);
        blocks.push_back(b);
    }
    blocks.push_back(L"\\StringFileInfo\\040904B0\\");
    blocks.push_back(L"\\StringFileInfo\\040904E4\\");
    for (auto& b : blocks) {
        void* str = nullptr;
        if (VerQueryValueW(buf.data(), (b + field).c_str(), &str, &len) && str && len > 1) return Trim((const wchar_t*)str);
    }
    return L"";
}

static std::wstring Stem(const std::wstring& exe) {
    std::wstring n = exe;
    if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".exe") == 0) n.resize(n.size() - 4);
    if (!n.empty()) n[0] = (wchar_t)towupper(n[0]);
    return n;
}

static std::wstring ProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = (DWORD)std::size(buf);
    std::wstring r = QueryFullProcessImageNameW(h, 0, buf, &n) ? std::wstring(buf, n) : L"";
    CloseHandle(h);
    return r;
}

static std::wstring WindowTitle(HWND w) {
    wchar_t b[512] = {};
    GetWindowTextW(w, b, 512);
    return Trim(b);
}

// ============================================================ game names
// Steam: steamapps\appmanifest_*.acf maps an install folder to the game's name
static std::wstring SteamName(const std::wstring& steamapps, const std::wstring& folder) {
    static std::map<std::wstring, std::map<std::wstring, std::wstring>> cache;
    std::wstring dirKey = Lower(steamapps);
    auto it = cache.find(dirKey);
    if (it == cache.end()) {
        std::map<std::wstring, std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((steamapps + L"\\appmanifest_*.acf").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring text, name, dir;
                if (!ReadUtf8File(steamapps + L"\\" + fd.cFileName, text)) continue;
                for (auto& line : Split(text, L"\n")) {
                    auto q = Split(Trim(line), L"\"");   // "key"  "value" -> key, ws, value
                    if (q.size() < 3) continue;
                    if (q[0] == L"name" && name.empty()) name = q[2];
                    else if (q[0] == L"installdir" && dir.empty()) dir = q[2];
                }
                if (!dir.empty() && !name.empty()) names[Lower(dir)] = name;
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        it = cache.emplace(dirKey, std::move(names)).first;
    }
    auto n = it->second.find(Lower(folder));
    return n == it->second.end() ? L"" : n->second;
}

// Epic: ProgramData\Epic\EpicGamesLauncher\Data\Manifests\*.item (JSON with InstallLocation and DisplayName)
static std::wstring EpicName(const std::wstring& lowerPath) {
    static std::vector<std::pair<std::wstring, std::wstring>> games;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::wstring dir = ExpandEnv(L"%ProgramData%\\Epic\\EpicGamesLauncher\\Data\\Manifests");
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"\\*.item").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring text;
                json::Value v;
                if (!ReadUtf8File(dir + L"\\" + fd.cFileName, text) || !json::Parser(text).Parse(v)) continue;
                std::wstring loc = Lower(v.GetStr(L"InstallLocation")), name = v.GetStr(L"DisplayName");
                if (!loc.empty() && !name.empty()) games.push_back({loc, name});
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    for (auto& g : games) if (lowerPath.compare(0, g.first.size(), g.first) == 0) return g.second;
    return L"";
}

// GOG: HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<id> with "path" and "gameName"
static std::wstring GogName(const std::wstring& lowerPath) {
    static std::vector<std::pair<std::wstring, std::wstring>> games;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", 0, KEY_READ, &h) == ERROR_SUCCESS) {
            wchar_t sub[128];
            for (DWORD i = 0;; ++i) {
                DWORD n = (DWORD)std::size(sub);
                if (RegEnumKeyExW(h, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                std::wstring key = std::wstring(L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\") + sub, path, name;
                RegReadStr(HKEY_LOCAL_MACHINE, key.c_str(), L"path", path);
                if (!RegReadStr(HKEY_LOCAL_MACHINE, key.c_str(), L"gameName", name)) RegReadStr(HKEY_LOCAL_MACHINE, key.c_str(), L"GAMENAME", name);
                if (!path.empty() && !name.empty()) games.push_back({Lower(path), name});
            }
            RegCloseKey(h);
        }
    }
    for (auto& g : games) if (lowerPath.compare(0, g.first.size(), g.first) == 0) return g.second;
    return L"";
}

static std::wstring FolderAfter(const std::wstring& path, size_t at) {
    size_t end = path.find(L'\\', at);
    return path.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
}

static bool Generic(const std::wstring& s) {
    std::wstring l = Lower(s);
    return l.empty() || l.find(L"unity") != std::wstring::npos || l.find(L"unreal") != std::wstring::npos || l == L"game" ||
           l.find(L"launcher") != std::wstring::npos || l.find(L"bootstrap") != std::wstring::npos || l.find(L"win64") != std::wstring::npos ||
           l.find(L"shipping") != std::wstring::npos;
}

// Is it a game, and what is it really called?
static bool GameFromPath(const std::wstring& path, std::wstring& name) {
    std::wstring lp = Lower(path);
    size_t s = lp.find(L"\\steamapps\\common\\");
    if (s != std::wstring::npos) {
        std::wstring folder = FolderAfter(path, s + 18);
        name = SteamName(path.substr(0, s + 10), folder);
        if (name.empty()) name = folder;
        return true;
    }
    if (!(name = EpicName(lp)).empty()) return true;
    if (!(name = GogName(lp)).empty()) return true;
    for (const wchar_t* marker : {L"\\epic games\\", L"\\gog galaxy\\games\\", L"\\gog games\\", L"\\xboxgames\\"}) {
        size_t p = lp.find(marker);
        if (p != std::wstring::npos) { name = FolderAfter(path, p + wcslen(marker)); return true; }
    }
    return false;
}

static std::wstring GameNameFallback(const std::wstring& path, HWND w) {
    std::wstring n = VersionString(path, L"ProductName");
    if (Generic(n)) n = VersionString(path, L"FileDescription");
    if (Generic(n)) n = WindowTitle(w);
    if (Generic(n)) {
        std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
        n = FileName(dir);
    }
    return n.empty() ? Stem(FileName(path)) : n;
}

// Launched by a game store? (Steam, Epic, GOG Galaxy or RetroArch up the process tree)
static bool LaunchedByStore(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    std::map<DWORD, std::pair<DWORD, std::wstring>> procs;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) procs[pe.th32ProcessID] = {pe.th32ParentProcessID, Lower(pe.szExeFile)};
    CloseHandle(snap);
    DWORD cur = pid;
    for (int depth = 0; depth < 4; ++depth) {
        auto it = procs.find(cur);
        if (it == procs.end()) return false;
        auto parent = procs.find(it->second.first);
        if (parent == procs.end() || parent->first == cur) return false;
        const std::wstring& p = parent->second.second;
        if (p == L"steam.exe" || p == L"epicgameslauncher.exe" || p == L"galaxyclient.exe" || p == L"retroarch.exe") return true;
        cur = parent->first;
    }
    return false;
}

// ============================================================ which app is in front
static DWORD UwpChild(HWND host, DWORD hostPid) {   // ApplicationFrameHost hosts Store apps; find the real process
    struct Ctx { DWORD host, found; } ctx{hostPid, 0};
    EnumChildWindows(host, [](HWND c, LPARAM lp) -> BOOL {
        auto* x = (Ctx*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(c, &pid);
        if (pid && pid != x->host) { x->found = pid; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

static ProcInfo* Identify(HWND fg, DWORD pid) {
    auto it = g_procs.find(pid);
    if (it != g_procs.end() && !it->second.retro) return &it->second;
    if (g_procs.size() > 300) g_procs.clear();
    ProcInfo p;
    p.path = ProcessPath(pid);
    p.exe = Lower(FileName(p.path));
    if (p.path.empty()) { p.skip = true; return &(g_procs[pid] = p); }
    if (p.exe == L"applicationframehost.exe") {
        DWORD child = UwpChild(fg, pid);
        if (child) {
            std::wstring cp = ProcessPath(child);
            if (!cp.empty()) { p.path = cp; p.exe = Lower(FileName(cp)); }
        }
        if (p.exe == L"applicationframehost.exe") { p.key = L"windows-app"; p.name = L"Windows app"; }
    }
    if (p.exe == L"lockapp.exe" || p.exe == L"logonui.exe" || (p.exe.size() > 4 && p.exe.compare(p.exe.size() - 4, 4, L".scr") == 0)) p.skip = true;
    else if (p.exe == L"searchhost.exe" || p.exe == L"startmenuexperiencehost.exe" || p.exe == L"shellexperiencehost.exe" ||
             p.exe == L"textinputhost.exe" || p.exe == L"searchapp.exe" || p.exe == L"shellhost.exe") {
        p.key = L"windows-shell";
        p.name = L"Windows (Start and search)";
    } else if (p.exe == L"explorer.exe") {
        p.key = L"explorer.exe";
        p.name = L"File Explorer";
    } else if (p.exe == L"retroarch.exe") {
        p.retro = true;
        std::wstring t = WindowTitle(fg);
        size_t bar = t.rfind(L"||");
        std::wstring content = bar == std::wstring::npos ? L"" : Trim(t.substr(bar + 2));
        p.key = content.empty() ? L"retroarch.exe" : L"retroarch|" + Lower(content);
        p.name = content.empty() ? L"RetroArch" : content;
        p.game = true;
    } else if (p.key.empty()) {
        p.key = p.exe;
        std::wstring gameName;
        p.knownApp = AutoCategory(p.key, false) != L"Other";
        if (!p.knownApp && GameFromPath(p.path, gameName)) { p.game = true; p.name = gameName; }
        else if (!p.knownApp && LaunchedByStore(pid)) { p.game = true; p.name = GameNameFallback(p.path, fg); }
        else {
            p.name = VersionString(p.path, L"FileDescription");
            if (p.name.empty() || p.name.size() > 60) p.name = VersionString(p.path, L"ProductName");
            if (p.name.empty() || p.name.size() > 60) p.name = Stem(FileName(p.path));
        }
    }
    if (!p.skip) {
        // remember what we learned; a game stays a game
        AppInfo& a = g_apps[p.key];
        bool changed = a.key.empty() || (p.game && !a.game) || (a.name != p.name && !p.name.empty() && (p.game || !a.game));
        if (a.game && !p.game) { p.game = true; p.name = a.name; }
        if (changed) {
            a.key = p.key;
            a.name = p.name;
            a.game = a.game || p.game;
            a.path = p.path;
            g_appsDirty = true;
        }
    }
    return &(g_procs[pid] = p);
}

static bool IsFullscreen(HWND fg) {
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) && (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE)) return true;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT wr;
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(fg, &wr) || !GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

static bool DisplayKeptOn() {   // a video or game asked Windows to keep the screen on
    EXECUTION_STATE es = 0;
    return CallNtPowerInformation(SystemExecutionState, nullptr, 0, &es, sizeof(es)) == 0 && (es & ES_DISPLAY_REQUIRED);
}

// ============================================================ storage
static void LoadCurrentMonth() {
    int today = Today();
    g_month = today / 100;
    g_days.clear();
    LoadMonth(g_month / 100, g_month % 100, g_days);
}

static void Flush() {
    if (g_dirty) {
        if (SaveMonth(g_month / 100, g_month % 100, g_days)) g_dirty = false;
        else Log(L"Screen time: couldn't save %d (error %lu)", g_month, GetLastError());
    }
    if (g_appsDirty && SaveApps(g_apps)) g_appsDirty = false;
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void LoadState() {
    std::wstring text;
    if (!ReadUtf8File(Dir() + L"\\state.tsv", text)) return;
    int keepFrom = AddDays(Today(), -14);
    for (auto& line : Split(text, L"\n")) {
        std::wstring t = Trim(line);
        if (t.empty() || t[0] == L'#') continue;
        auto f = Split(t, L"|", true);
        if (f.size() >= 2 && _wtoi(f[1].c_str()) >= keepFrom) g_shown.insert(t);
    }
}

static void SaveState() {
    std::wstring text = L"# Breaker screen time: notifications already shown\n";
    for (auto& s : g_shown) text += s + L"\n";
    WriteUtf8FileAtomic(Dir() + L"\\state.tsv", text);
}

// ============================================================ notifications
static void Notify(const std::wstring& title, const std::wstring& text, const std::wstring& link) {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g_wnd;
    n.uID = 1;
    if (!g_iconShown) {
        n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        n.uCallbackMessage = WM_TRACKER_TRAY;
        n.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        wcscpy_s(n.szTip, L"Breaker screen time");
        g_iconShown = Shell_NotifyIconW(NIM_ADD, &n) != FALSE;
        n.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &n);
    }
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
    g_balloonLink = link;
    SetTimer(g_wnd, T_BALLOON, 60000, nullptr);   // the icon only exists to carry the message
    Log(L"Screen time notification: %ls", text.c_str());
}

static void RemoveIcon() {
    if (!g_iconShown) return;
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g_wnd;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
    g_iconShown = false;
}

static void CheckLimits() {
    if (!g_config.tallyLimitNotify || g_config.tallyLimits.empty()) return;
    int today = Today();
    auto dayIt = g_days.find(today);
    if (dayIt == g_days.end()) return;
    for (auto& lim : g_config.tallyLimits) {
        if (lim.second <= 0) continue;
        long long used = 0;
        std::wstring label;
        if (StartsWith(lim.first, L"cat:")) {
            std::wstring cat = lim.first.substr(4);
            for (auto& kv : dayIt->second) if (CategoryOf(kv.first, g_apps) == cat) used += kv.second;
            label = cat;
        } else {
            auto u = dayIt->second.find(lim.first);
            used = u == dayIt->second.end() ? 0 : u->second;
            label = tally::NameOf(lim.first, g_apps);
        }
        long long limit = lim.second * 60LL;
        int level = used >= limit ? 100 : used * 100 >= limit * 80 ? 80 : 0;
        if (!level) continue;
        std::wstring id = L"limit|" + std::to_wstring(today) + L"|" + lim.first + L"|" + std::to_wstring(level);
        if (g_shown.count(id)) continue;
        g_shown.insert(id);
        if (level == 100) g_shown.insert(L"limit|" + std::to_wstring(today) + L"|" + lim.first + L"|80");
        std::wstring link = StartsWith(lim.first, L"cat:") ? L"screentime/today" : L"screentime/app/" + lim.first;
        if (level == 80)
            Notify(L"Screen time", L"You've used 80% of today's limit for " + label + L" (" + Dur(used) + L" of " + Dur(limit) + L").", link);
        else
            Notify(L"Screen time", L"You've reached today's " + Dur(limit) + L" limit for " + label + L". Nothing will be closed; this is just a heads-up.", link);
        SaveState();
    }
}

static void CheckWeekly() {
    if (!g_config.tallyWeekly) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    int today = Today(), week = WeekStart(today);
    std::wstring id = L"week|" + std::to_wstring(week);
    if (g_shown.count(id) || st.wHour < 9) return;
    g_shown.insert(id);
    SaveState();
    Flush();
    DayMap days;
    LoadRange(AddDays(week, -7), AddDays(week, -1), days);
    std::map<std::wstring, long long> per;
    long long total = 0, gaming = 0;
    for (auto& d : days)
        for (auto& kv : d.second) {
            per[kv.first] += kv.second;
            total += kv.second;
            if (CategoryOf(kv.first, g_apps) == L"Games") gaming += kv.second;
        }
    if (total < 60) return;
    auto top = std::max_element(per.begin(), per.end(), [](auto& a, auto& b) { return a.second < b.second; });
    Notify(L"Your week in screen time",
           Dur(total) + L" in total, " + Dur(gaming) + L" gaming. Most used: " + tally::NameOf(top->first, g_apps) + L" (" + Dur(top->second) + L").",
           L"screentime/week");
}

// ============================================================ sampling
static void Add(const std::wstring& key, ULONGLONG ms) {
    int today = Today();
    if (today / 100 != g_month) { Flush(); LoadCurrentMonth(); }
    long long& rem = g_ms[key];
    rem += (long long)ms;
    if (rem >= 1000) {
        g_days[today][key] += rem / 1000;
        rem %= 1000;
        g_dirty = true;
    }
}

static void Sample() {
    ULONGLONG now = GetTickCount64(), elapsed = now - g_lastTick;
    g_lastTick = now;
    if (g_locked || g_asleep || !g_config.tallyEnabled) return;
    if (elapsed > kSampleMs * 3) elapsed = kSampleMs;   // the timer was held up (sleep we missed): count one beat only
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desk) return;                                   // lock screen, sign-in or UAC prompt
    CloseDesktop(desk);
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    bool full = IsFullscreen(fg);
    LASTINPUTINFO li{sizeof(li)};
    GetLastInputInfo(&li);
    DWORD idle = GetTickCount() - li.dwTime;
    DWORD idleLimit = (DWORD)std::max(1, g_config.tallyIdleMin) * 60000u;
    if (idle >= idleLimit) {
        // away, unless a game or a video is keeping things going (but not for hours on end)
        if (!(full || DisplayKeptOn()) || idle > 3u * 3600000u) return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid || pid == GetCurrentProcessId()) return;
    ProcInfo* p = Identify(fg, pid);
    if (!p || p->skip || p->key.empty()) return;
    for (auto& n : g_config.tallyNever) if (n == p->key) return;
    // Something unknown running full screen for a while is a game
    if (full && !p->game && !p->knownApp && p->key == p->exe && p->exe != L"explorer.exe" &&
        Lower(p->path).find(L"\\windows\\") == std::wstring::npos) {
        g_fullStreak = g_fullKey == p->key ? g_fullStreak + 1 : 1;
        g_fullKey = p->key;
        if (g_fullStreak * kSampleMs >= 15000) {
            p->game = true;
            AppInfo& a = g_apps[p->key];
            a.key = p->key;
            a.game = true;
            a.name = p->name = GameNameFallback(p->path, fg);
            a.path = p->path;
            g_appsDirty = true;
            Log(L"Screen time: %ls looks like a game (full screen)", p->name.c_str());
        }
    } else if (!full) {
        g_fullStreak = 0;
    }
    Add(p->key, elapsed);
}

// ============================================================ window
static void Quit() {
    Flush();
    RemoveIcon();
    DestroyWindow(g_wnd);
}

// Breaker was updated (by itself or by Hangar): hand over to the new exe. The file is compared by size and
// time rather than its version: asking Windows for the version of a path this process was started from can
// answer with the copy already in memory.
static WIN32_FILE_ATTRIBUTE_DATA g_startFile{};

static bool SameFile(const WIN32_FILE_ATTRIBUTE_DATA& a, const WIN32_FILE_ATTRIBUTE_DATA& b) {
    return a.nFileSizeLow == b.nFileSizeLow && a.nFileSizeHigh == b.nFileSizeHigh &&
           a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime && a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime;
}

static void CheckForNewExe() {
    WIN32_FILE_ATTRIBUTE_DATA now{};
    if (!GetFileAttributesExW(g_startPath.c_str(), GetFileExInfoStandard, &now)) {
        Log(L"Screen time tracker: Breaker.exe is gone, stopping");
        Quit();
        return;
    }
    if (SameFile(now, g_startFile)) return;
    Log(L"Screen time tracker: Breaker.exe changed (updated), restarting on the new copy");
    Quit();
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = nullptr; }
    ShellOpen(g_startPath, L"--tracker");
}

static LRESULT CALLBACK TrackerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == T_SAMPLE) Sample();
            else if (wp == T_FLUSH) Flush();
            else if (wp == T_MINUTE) {
                CheckLimits();
                if (++g_minutes % 10 == 1) { CheckWeekly(); CheckForNewExe(); }
            } else if (wp == T_BALLOON) {
                KillTimer(hwnd, T_BALLOON);
                RemoveIcon();
            }
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_DISCONNECT) { g_locked = true; Flush(); }
            else if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT || wp == WTS_SESSION_LOGON) {
                g_locked = false;
                g_lastTick = GetTickCount64();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) { g_asleep = true; Flush(); }
            else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) { g_asleep = false; g_lastTick = GetTickCount64(); }
            return TRUE;
        case WM_QUERYENDSESSION:
            Flush();
            return TRUE;
        case WM_ENDSESSION:
            if (wp) Flush();
            return 0;
        case tally::WM_TALLY_FLUSH:
            Flush();
            return 1;
        case tally::WM_TALLY_RELOAD:
            ConfigLoad();
            if (!g_config.tallyEnabled) Quit();
            return 0;
        case WM_COPYDATA: {
            auto* cds = (COPYDATASTRUCT*)lp;
            if (!cds || cds->dwData != kCopyClear || !cds->lpData) return 0;
            std::wstring cmd((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
            while (!cmd.empty() && cmd.back() == 0) cmd.pop_back();
            auto f = Split(cmd, L" ");
            if (f.size() < 2) return 0;
            Flush();
            bool ok = ClearRange(_wtoi(f[0].c_str()), _wtoi(f[1].c_str()), f.size() > 2 ? f[2] : L"");
            if (f.size() > 2) g_ms.erase(f[2]); else g_ms.clear();
            LoadCurrentMonth();
            Log(L"Screen time history cleared: %ls", cmd.c_str());
            return ok ? 1 : 0;
        }
        case WM_TRACKER_TRAY:
            if (LOWORD(lp) == NIN_BALLOONUSERCLICK || LOWORD(lp) == WM_LBUTTONUP) {
                ShellOpen(g_startPath, L"\"" + (g_balloonLink.empty() ? std::wstring(L"screentime") : g_balloonLink) + L"\"");
                RemoveIcon();
            } else if (LOWORD(lp) == NIN_BALLOONTIMEOUT || LOWORD(lp) == NIN_BALLOONHIDE) {
                RemoveIcon();
            }
            return 0;
        case WM_CLOSE:
            Quit();
            return 0;
        case WM_DESTROY:
            WTSUnRegisterSessionNotification(hwnd);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int TrackerMain() {
    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\Breaker.larosa023.Tracker");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    LogInit();
    ConfigLoad();
    if (!g_config.tallyEnabled) return 0;
    g_startPath = ExePath();
    GetFileAttributesExW(g_startPath.c_str(), GetFileExInfoStandard, &g_startFile);

    // Stay out of the way: low priority, and let Windows run it on efficient cores at low clock speed
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    struct { ULONG version, mask, state; } throttle{1, 1, 1};   // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
    using SetInfoFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    if (auto fn = (SetInfoFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")) fn(GetCurrentProcess(), 4, &throttle, sizeof(throttle));

    WNDCLASSW wc{};
    wc.lpfnWndProc = TrackerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTrackerClass;
    RegisterClassW(&wc);
    std::wstring title = std::wstring(L"Breaker tracker ") + BREAKER_VERSION + L" " + g_startPath;
    // A hidden top-level window (not message-only) so it hears lock, sleep and shutdown
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrackerClass, title.c_str(), WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_wnd) return 1;
    WTSRegisterSessionNotification(g_wnd, NOTIFY_FOR_THIS_SESSION);

    LoadCurrentMonth();
    g_apps = LoadApps();
    LoadState();
    g_lastTick = GetTickCount64();
    SetTimer(g_wnd, T_SAMPLE, kSampleMs, nullptr);
    SetTimer(g_wnd, T_FLUSH, 5 * 60 * 1000, nullptr);
    SetTimer(g_wnd, T_MINUTE, 60 * 1000, nullptr);
    Log(L"Screen time tracker %ls started", BREAKER_VERSION);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    if (g_mutex) CloseHandle(g_mutex);
    Log(L"Screen time tracker stopped");
    return 0;
}
