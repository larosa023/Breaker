// Breaker - Screen time: the shared data store (files, dates, categories) and control of the tracker
#include "tally.h"
#include "config.h"
#include <shlobj.h>
#include <algorithm>
#include <set>

namespace tally {

const wchar_t* const kCategoryNames[] = {L"Games", L"Work", L"Browsing", L"Media", L"Chat", L"Other"};

std::wstring Dir() {
    static std::wstring dir;
    if (dir.empty()) {
        dir = DataDir() + L"\\screentime";
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    }
    return dir;
}

// ============================================================ dates
static int DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int ToSerial(int ymd) { return DaysFromCivil(ymd / 10000, ymd / 100 % 100, ymd % 100); }
static int FromSerial(int z) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
    return (y + (m <= 2)) * 10000 + m * 100 + d;
}

int Today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}
int AddDays(int ymd, int days) { return FromSerial(ToSerial(ymd) + days); }
int DaysBetween(int from, int to) { return ToSerial(to) - ToSerial(from); }
int Weekday(int ymd) { return ((ToSerial(ymd) % 7) + 11) % 7; }   // 1970-01-01 was a Thursday

int WeekStart(int ymd) {
    wchar_t buf[4] = L"6";   // LOCALE_IFIRSTDAYOFWEEK: 0 = Monday ... 6 = Sunday
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, buf, 4);
    int first = (_wtoi(buf) + 1) % 7;   // as 0 = Sunday
    return AddDays(ymd, -((Weekday(ymd) - first + 7) % 7));
}
int MonthStart(int ymd) { return ymd / 100 * 100 + 1; }
int MonthEnd(int ymd) {
    int y = ymd / 10000, m = ymd / 100 % 100;
    int next = (m == 12 ? (y + 1) * 10000 + 101 : y * 10000 + (m + 1) * 100 + 1);
    return AddDays(next, -1);
}

std::wstring DayLabel(int ymd, bool longForm) {
    static const wchar_t* wd[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (!longForm) return wd[Weekday(ymd)];
    return std::wstring(wd[Weekday(ymd)]) + L", " + mon[(ymd / 100 % 100 + 11) % 12] + L" " + std::to_wstring(ymd % 100);
}

// ============================================================ files
static std::wstring MonthPath(int y, int m) {
    wchar_t b[32];
    swprintf(b, 32, L"\\%04d-%02d.tsv", y, m);
    return Dir() + b;
}

static std::wstring Clean(std::wstring s) {
    for (auto& c : s) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

bool LoadMonth(int year, int month, DayMap& out) {
    std::wstring text;
    if (!ReadUtf8File(MonthPath(year, month), text)) return false;
    for (auto& line : Split(text, L"\n")) {
        if (line.empty() || line[0] == L'#') continue;
        auto f = Split(line, L"\t", true);
        if (f.size() < 3) continue;
        int day = _wtoi(f[0].c_str());
        long long secs = _wtoi64(Trim(f[2]).c_str());
        if (day < 1 || day > 31 || secs <= 0 || f[1].empty()) continue;
        out[year * 10000 + month * 100 + day][f[1]] += secs;
    }
    return true;
}

bool SaveMonth(int year, int month, const DayMap& all) {
    std::wstring text = L"# Breaker screen time: day, app, seconds\n";
    int from = year * 10000 + month * 100, to = from + 99;
    bool any = false;
    for (auto it = all.lower_bound(from); it != all.end() && it->first <= to; ++it)
        for (auto& kv : it->second) {
            if (kv.second <= 0) continue;
            text += std::to_wstring(it->first % 100) + L"\t" + Clean(kv.first) + L"\t" + std::to_wstring(kv.second) + L"\n";
            any = true;
        }
    std::wstring path = MonthPath(year, month);
    if (!any) { DeleteFileW(path.c_str()); return true; }
    return WriteUtf8FileAtomic(path, text);
}

void LoadRange(int from, int to, DayMap& out) {
    DayMap tmp;
    for (int ym = from / 100; ym <= to / 100;) {
        LoadMonth(ym / 100, ym % 100, tmp);
        ym = ym % 100 == 12 ? (ym / 100 + 1) * 100 + 1 : ym + 1;
    }
    for (auto it = tmp.lower_bound(from); it != tmp.end() && it->first <= to; ++it) out[it->first] = std::move(it->second);
}

std::map<std::wstring, AppInfo> LoadApps() {
    std::map<std::wstring, AppInfo> apps;
    std::wstring text;
    if (!ReadUtf8File(Dir() + L"\\apps.tsv", text)) return apps;
    for (auto& line : Split(text, L"\n")) {
        if (line.empty() || line[0] == L'#') continue;
        auto f = Split(line, L"\t", true);
        if (f.size() < 3 || f[0].empty()) continue;
        AppInfo a;
        a.key = f[0];
        a.name = f[1];
        a.game = Trim(f[2]) == L"1";
        if (f.size() > 3) a.path = Trim(f[3]);
        apps[a.key] = a;
    }
    return apps;
}

bool SaveApps(const std::map<std::wstring, AppInfo>& apps) {
    std::wstring text = L"# Breaker screen time apps: key, name, game, path\n";
    for (auto& kv : apps)
        text += Clean(kv.first) + L"\t" + Clean(kv.second.name) + L"\t" + (kv.second.game ? L"1" : L"0") + L"\t" + Clean(kv.second.path) + L"\n";
    return WriteUtf8FileAtomic(Dir() + L"\\apps.tsv", text);
}

bool ClearRange(int from, int to, const std::wstring& key) {
    bool ok = true;
    std::vector<int> months;   // only the month files that exist
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((Dir() + L"\\????" L"-??.tsv").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            int y = 0, m = 0;
            if (swscanf(fd.cFileName, L"%d-%d.tsv", &y, &m) == 2 && y * 100 + m >= from / 100 && y * 100 + m <= to / 100) months.push_back(y * 100 + m);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    for (int ym : months) {
        int y = ym / 100, m = ym % 100;
        DayMap days;
        if (LoadMonth(y, m, days)) {
            for (auto& d : days) {
                if (d.first < from || d.first > to) continue;
                if (key.empty()) d.second.clear();
                else d.second.erase(key);
            }
            ok = SaveMonth(y, m, days) && ok;
        }
    }
    return ok;
}

// ============================================================ categories
static bool In(const std::wstring& exe, std::initializer_list<const wchar_t*> list) {
    for (auto s : list) if (exe == s) return true;
    return false;
}

std::wstring AutoCategory(const std::wstring& key, bool game) {
    if (game || StartsWith(key, L"retroarch|")) return L"Games";
    const std::wstring& k = key;
    if (In(k, {L"steam.exe", L"steamwebhelper.exe", L"epicgameslauncher.exe", L"galaxyclient.exe", L"battle.net.exe", L"eadesktop.exe",
               L"upc.exe", L"ubisoftconnect.exe", L"xboxpcapp.exe", L"gamebar.exe", L"playnite.desktopapp.exe", L"playnite.fullscreenapp.exe",
               L"retroarch.exe", L"heroic.exe", L"rockstarlauncher.exe"}))
        return L"Games";
    if (In(k, {L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"vivaldi.exe", L"arc.exe", L"zen.exe", L"librewolf.exe",
               L"waterfox.exe", L"iexplore.exe", L"chromium.exe", L"thorium.exe", L"floorp.exe", L"duckduckgo.exe"}))
        return L"Browsing";
    if (In(k, {L"spotify.exe", L"vlc.exe", L"mpc-hc64.exe", L"mpc-hc.exe", L"mpc-be64.exe", L"mpv.exe", L"potplayermini64.exe", L"wmplayer.exe",
               L"microsoft.media.player.exe", L"applemusic.exe", L"appletv.exe", L"itunes.exe", L"plex.exe", L"plexamp.exe", L"foobar2000.exe",
               L"musicbee.exe", L"tidal.exe", L"deezer.exe", L"kodi.exe", L"stremio.exe", L"obs64.exe", L"photos.exe", L"microsoft.photos.exe",
               L"netflix.exe", L"faderly.exe", L"tonely.exe", L"lumora.exe"}))
        return L"Media";
    if (In(k, {L"discord.exe", L"slack.exe", L"ms-teams.exe", L"teams.exe", L"whatsapp.exe", L"whatsapp.root.exe", L"telegram.exe", L"signal.exe",
               L"zoom.exe", L"skype.exe", L"messenger.exe", L"element.exe", L"phoneexperiencehost.exe", L"beeper.exe"}))
        return L"Chat";
    if (In(k, {L"code.exe", L"code - insiders.exe", L"devenv.exe", L"winword.exe", L"excel.exe", L"powerpnt.exe", L"outlook.exe", L"olk.exe",
               L"onenote.exe", L"msaccess.exe", L"mspub.exe", L"visio.exe", L"winproj.exe", L"acrobat.exe", L"acrord32.exe", L"revu.exe",
               L"planswift.exe", L"ost.exe", L"acad.exe", L"revit.exe", L"sketchup.exe", L"notepad.exe", L"notepad++.exe", L"sublime_text.exe",
               L"windowsterminal.exe", L"wt.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"idea64.exe", L"pycharm64.exe", L"rider64.exe",
               L"clion64.exe", L"webstorm64.exe", L"cursor.exe", L"zed.exe", L"figma.exe", L"photoshop.exe", L"illustrator.exe", L"indesign.exe",
               L"afterfx.exe", L"adobe premiere pro.exe", L"blender.exe", L"githubdesktop.exe", L"postman.exe", L"claude.exe", L"chatgpt.exe",
               L"thunderbird.exe", L"obsidian.exe", L"notion.exe", L"todoist.exe", L"calc.exe", L"calculatorapp.exe"}))
        return L"Work";
    return L"Other";
}

std::wstring CategoryOf(const std::wstring& key, const std::map<std::wstring, AppInfo>& apps) {
    for (auto& kv : g_config.tallyCats) if (kv.first == key) return kv.second;
    auto it = apps.find(key);
    return AutoCategory(key, it != apps.end() && it->second.game);
}

std::wstring NameOf(const std::wstring& key, const std::map<std::wstring, AppInfo>& apps) {
    auto it = apps.find(key);
    if (it != apps.end() && !it->second.name.empty()) return it->second.name;
    std::wstring n = key;
    size_t bar = n.find(L'|');
    if (bar != std::wstring::npos) return n.substr(bar + 1);
    if (n.size() > 4 && n.compare(n.size() - 4, 4, L".exe") == 0) n.resize(n.size() - 4);
    if (!n.empty()) n[0] = (wchar_t)towupper(n[0]);
    return n;
}

int LimitFor(const std::wstring& limitKey) {
    for (auto& kv : g_config.tallyLimits) if (kv.first == limitKey) return kv.second;
    return 0;
}

std::wstring Dur(long long s) {
    if (s < 60) return s <= 0 ? L"0m" : L"<1m";
    long long h = s / 3600, m = (s % 3600) / 60;
    wchar_t b[32];
    if (h) swprintf(b, 32, L"%lldh %02lldm", h, m);
    else swprintf(b, 32, L"%lldm", m);
    return b;
}

std::wstring Csv(const std::wstring& s) {
    if (s.find_first_of(L",\"\n") == std::wstring::npos) return s;
    return L"\"" + ReplaceAll(s, L"\"", L"\"\"") + L"\"";
}

bool ExportCsv(const std::wstring& path) {
    auto apps = LoadApps();
    DayMap all;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((Dir() + L"\\????" L"-??.tsv").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            int y = 0, m = 0;
            if (swscanf(fd.cFileName, L"%d-%d.tsv", &y, &m) == 2) LoadMonth(y, m, all);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::wstring text = L"\xFEFF" L"Date,App,Name,Category,Game,Minutes\r\n";
    for (auto& d : all)
        for (auto& kv : d.second) {
            wchar_t date[16], mins[32];
            swprintf(date, 16, L"%04d-%02d-%02d", d.first / 10000, d.first / 100 % 100, d.first % 100);
            swprintf(mins, 32, L"%.1f", kv.second / 60.0);
            auto a = apps.find(kv.first);
            text += std::wstring(date) + L"," + Csv(kv.first) + L"," + Csv(NameOf(kv.first, apps)) + L"," + CategoryOf(kv.first, apps) + L"," +
                    (a != apps.end() && a->second.game ? L"yes" : L"no") + L"," + mins + L"\r\n";
        }
    return WriteUtf8FileAtomic(path, text);
}

// ============================================================ the tracker process
HWND FindTracker() { return FindWindowW(kTrackerClass, nullptr); }

bool FlushTracker() {
    HWND t = FindTracker();
    if (!t) return false;
    DWORD_PTR r = 0;
    return SendMessageTimeoutW(t, WM_TALLY_FLUSH, 0, 0, SMTO_ABORTIFHUNG, 2000, &r) && r == 1;
}

void NotifyTrackerReload() {
    if (HWND t = FindTracker()) PostMessageW(t, WM_TALLY_RELOAD, 0, 0);
}

bool ClearHistory(int from, int to, const std::wstring& key) {
    HWND t = FindTracker();
    if (t) {
        std::wstring cmd = std::to_wstring(from) + L" " + std::to_wstring(to) + (key.empty() ? L"" : L" " + key);
        COPYDATASTRUCT cds{kCopyClear, (DWORD)((cmd.size() + 1) * sizeof(wchar_t)), (PVOID)cmd.c_str()};
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(t, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &r) && r == 1) return true;
    }
    return ClearRange(from, to, key);
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static void StopAndWait(HWND t) {
    DWORD pid = 0;
    GetWindowThreadProcessId(t, &pid);
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(t, WM_CLOSE, 0, 0);
    if (h) { WaitForSingleObject(h, 4000); CloseHandle(h); }
}

void EnsureTracker(bool enabled) {
    HWND t = FindTracker();
    if (!enabled) {
        RegRemoveValue(HKEY_CURRENT_USER, kRunKey, L"BreakerTracker");
        if (t) StopAndWait(t);
        return;
    }
    RegWriteStr(HKEY_CURRENT_USER, kRunKey, L"BreakerTracker", L"\"" + ExePath() + L"\" --tracker");
    if (t) {
        // The tracker's window title carries its version and exe; an old copy (after an update) is replaced
        wchar_t title[600] = {};
        GetWindowTextW(t, title, 600);
        std::wstring want = std::wstring(L"Breaker tracker ") + BREAKER_VERSION + L" " + ExePath();
        if (_wcsicmp(title, want.c_str()) == 0) return;
        Log(L"Restarting the screen time tracker (%ls)", title);
        StopAndWait(t);
    }
    ShellOpen(ExePath(), L"--tracker");
}

void StopTracker() {
    RegRemoveValue(HKEY_CURRENT_USER, kRunKey, L"BreakerTracker");
    if (HWND t = FindTracker()) StopAndWait(t);
}

}  // namespace tally
