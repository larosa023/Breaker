// Breaker - My Apps: the other apps in the suite (Foyer, Strata, Faderly, Tonely, Ember, Mantel, Marq, Lumora...)
// Every suite app follows the same conventions, which is all Breaker relies on:
//   installed copy   %LOCALAPPDATA%\Programs\<App>\<App>.exe  (and an uninstall entry under HKCU)
//   settings         %APPDATA%\<App>\config.json, or <exe folder>\data\config.json in portable mode
//   releases         github.com/larosa023/<App> with <App>.exe and SHA256SUMS.txt attached
//   command line     "<App>.exe --quit" closes a running copy
#include "suite.h"
#include "config.h"
#include "json.h"
#include "updater.h"
#include <tlhelp32.h>
#include <shlobj.h>
#include <mutex>
#include <map>
#include <algorithm>

static const wchar_t* kSuite[] = {L"Foyer", L"Strata", L"Faderly", L"Tonely", L"Ember", L"Mantel", L"Marq", L"Lumora", L"Hangar"};

namespace {
struct Latest { std::wstring version, exeUrl, sumsUrl, page, notes; bool ok = false; };
}
static std::mutex g_suiteLock;
static std::map<std::wstring, Latest> g_latest;
static double g_latestAt = 0;

static Row Info(const wchar_t* group, const std::wstring& title, const std::wstring& desc, const std::wstring& valueText = L"") {
    Row r;
    r.group = group;
    r.title = title;
    r.desc = desc;
    r.valueText = valueText;
    return r;
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static std::vector<std::wstring> SuiteNames() {
    std::vector<std::wstring> names(std::begin(kSuite), std::end(kSuite));
    for (auto& extra : g_config.suiteApps)
        if (std::find_if(names.begin(), names.end(), [&](auto& n) { return _wcsicmp(n.c_str(), extra.c_str()) == 0; }) == names.end()) names.push_back(extra);
    return names;
}

static std::wstring RunningPath(const std::wstring& app, DWORD* pidOut = nullptr) {
    std::wstring exeName = app + L".exe", path;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName.c_str()) != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (h) {
            wchar_t buf[MAX_PATH * 2];
            DWORD n = (DWORD)std::size(buf);
            if (QueryFullProcessImageNameW(h, 0, buf, &n)) path = buf;
            CloseHandle(h);
        }
        if (pidOut) *pidOut = pe.th32ProcessID;
        if (path.empty()) path = L"?";
        break;
    }
    CloseHandle(snap);
    return path;
}

std::wstring SuiteExe(const std::wstring& app) {
    std::wstring loc;
    if (RegReadStr(HKEY_CURRENT_USER, (L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + app).c_str(), L"InstallLocation", loc)) {
        std::wstring p = loc + L"\\" + app + L".exe";
        if (FileExists(p)) return p;
    }
    std::wstring p = KnownFolder(FOLDERID_UserProgramFiles) + L"\\" + app + L"\\" + app + L".exe";
    if (FileExists(p)) return p;
    p = RunningPath(app);
    if (!p.empty() && p != L"?") return p;
    std::wstring run;
    if (RegReadStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", app.c_str(), run) && !run.empty()) {
        std::wstring exe = run[0] == L'"' ? run.substr(1, run.find(L'"', 1) - 1) : run.substr(0, run.find(L' '));
        if (FileExists(exe)) return exe;
    }
    return L"";
}

static std::wstring SuiteConfig(const std::wstring& app, const std::wstring& exe) {
    if (!exe.empty()) {
        std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
        if (FileExists(dir + L"\\portable") || FileExists(dir + L"\\portable.txt")) return dir + L"\\data\\config.json";
    }
    return KnownFolder(FOLDERID_RoamingAppData) + L"\\" + app + L"\\config.json";
}

static bool LoadJson(const std::wstring& path, json::Value& root) {
    std::wstring text;
    return ReadUtf8File(path, text) && json::Parser(text).Parse(root) && root.type == json::Value::Object;
}

static bool SaveJson(const std::wstring& path, const json::Value& root) {
    std::wstring text;
    json::Write(root, text);
    return WriteUtf8FileAtomic(path, text);
}

// Suite apps save their settings when they close, so a running app is closed first and reopened after.
static bool QuitApp(const std::wstring& app, const std::wstring& exe) {
    DWORD pid = 0;
    if (RunningPath(app, &pid).empty()) return false;
    ShellOpen(exe, L"--quit");
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) { WaitForSingleObject(h, 6000); CloseHandle(h); }
    else Sleep(1500);
    return true;
}

static bool WithAppClosed(const std::wstring& app, const std::function<bool()>& edit) {
    std::wstring exe = SuiteExe(app);
    bool wasRunning = !exe.empty() && QuitApp(app, exe);
    bool ok = edit();
    if (wasRunning) ShellOpen(exe);
    return ok;
}

// ============================================================ settings of one app  (suite:<App>:<key>)
namespace {
struct KeyInfo { const wchar_t* key; const wchar_t* label; const wchar_t* choices; };
}
static const KeyInfo kKeys[] = {
    {L"theme", L"Theme", L"Dark=dark|Light=light|Midnight=midnight|Glass=glass|Ember=ember|Ocean=ocean|Follow Windows=system"},
    {L"backdrop", L"Background style", L"Solid=solid|Mica=mica|Acrylic=acrylic"},
    {L"density", L"Density", L"Compact=compact|Comfortable=comfortable"},
    {L"animation", L"Animation speed", L"None=none|Fast=fast|Smooth=smooth"},
    {L"iconSize", L"Icon size", L"Small=small|Medium=medium|Large=large"},
    {L"channel", L"Release channel", L"Stable=stable|Beta=beta"},
    {L"useWindowsAccent", L"Follow the Windows accent color", nullptr},
    {L"accentColor", L"Accent color", nullptr},
    {L"opacity", L"Backdrop tint strength", nullptr},
    {L"cornerRadius", L"Corner radius", nullptr},
    {L"autoCheck", L"Check for updates automatically", nullptr},
    {L"autoInstall", L"Install updates automatically", nullptr},
    {L"checkHours", L"Hours between update checks", nullptr},
};
static const wchar_t* kSkipKeys[] = {L"lastCheck", L"notifiedVersion", L"customTheme", L"paneRatio", L"sidebarWidth", L"previewWidth", L"sortColumn"};

static std::wstring PrettyKey(const std::wstring& key) {
    for (auto& k : kKeys) if (key == k.key) return k.label;
    std::wstring out;
    for (size_t i = 0; i < key.size(); ++i) {
        wchar_t c = key[i];
        if (i == 0) out += (wchar_t)towupper(c);
        else if (iswupper(c)) { out += L' '; out += (wchar_t)towlower(c); }
        else out += c;
    }
    return out;
}

static bool SuiteSplit(const std::wstring& id, std::wstring& app, std::wstring& key) {
    size_t a = id.find(L':'), b = id.find(L':', a + 1);
    if (a == std::wstring::npos || b == std::wstring::npos) return false;
    app = id.substr(a + 1, b - a - 1);
    key = id.substr(b + 1);
    return true;
}

static bool SuiteRead(const std::wstring& id, std::wstring& out) {
    std::wstring app, key;
    json::Value root;
    if (!SuiteSplit(id, app, key) || !LoadJson(SuiteConfig(app, SuiteExe(app)), root)) return false;
    const json::Value* v = root.Get(key);
    if (!v) return false;
    if (v->type == json::Value::Bool) out = v->b ? L"1" : L"0";
    else if (v->type == json::Value::String) out = v->s;
    else if (v->type == json::Value::Number) { std::wstring t; json::Write(*v, t); out = t; }
    else return false;
    return true;
}

static WriteResult SuiteWrite(const std::wstring& id, const std::wstring& value, std::wstring& err) {
    std::wstring app, key;
    if (!SuiteSplit(id, app, key)) { err = L"Unknown app setting."; return W_FAIL; }
    bool ok = WithAppClosed(app, [&] {
        std::wstring path = SuiteConfig(app, SuiteExe(app));
        json::Value root;
        if (!LoadJson(path, root)) return false;
        const json::Value* cur = root.Get(key);
        if (!cur) return false;
        if (cur->type == json::Value::Bool) root.Set(key, json::Value::Boolean(value == L"1"));
        else if (cur->type == json::Value::Number) root.Set(key, json::Value::Num(_wtof(value.c_str())));
        else root.Set(key, json::Value::Str(value));
        return SaveJson(path, root);
    });
    if (ok) return W_OK;
    err = L"Couldn't change " + app + L"'s settings file.";
    return W_FAIL;
}

static void SuiteChoices(const std::wstring& id, Choices& out) {
    std::wstring app, key;
    if (!SuiteSplit(id, app, key)) return;
    for (auto& k : kKeys) {
        if (key != k.key || !k.choices) continue;
        for (auto& part : Split(k.choices, L"|")) {
            size_t eq = part.rfind(L'=');
            out.push_back({part.substr(0, eq), part.substr(eq + 1)});
        }
    }
}

static const Handler kSuiteHandlers[] = {{L"suite:", SuiteRead, SuiteWrite, SuiteChoices, F_NOSNAP | F_NOWATCH}};
void RegisterSuiteHandlers() { RegisterHandlers(kSuiteHandlers, 1); }

void BuildMyAppSettings(std::vector<Row>& rows) {
    std::wstring app = UiRoute().substr(7);
    std::wstring exe = SuiteExe(app), cfg = SuiteConfig(app, exe);
    Row head = Info(L"App", app, exe.empty() ? L"Not installed" : L"Version " + FileVersion(exe) + L"\n" + exe);
    if (!exe.empty()) head.actions.push_back({L"Open", [exe] { ShellOpen(exe); }});
    head.actions.push_back({L"Settings folder", [cfg] { ShellOpen(cfg.substr(0, cfg.find_last_of(L'\\'))); }});
    rows.push_back(std::move(head));
    json::Value root;
    if (!LoadJson(cfg, root)) {
        rows.push_back(Info(L"Settings", L"No settings file yet", app + L" creates its settings the first time it runs. Open it once, then come back."));
        return;
    }
    for (auto& kv : root.obj) {
        if (kv.second.type != json::Value::Bool && kv.second.type != json::Value::Number && kv.second.type != json::Value::String) continue;
        if (std::find_if(std::begin(kSkipKeys), std::end(kSkipKeys), [&](const wchar_t* s) { return kv.first == s; }) != std::end(kSkipKeys)) continue;
        if (StartsWith(kv.first, L"win") || StartsWith(kv.first, L"last")) continue;
        Row r;
        r.id = L"suite:" + app + L":" + kv.first;
        r.group = kv.second.type == json::Value::Bool ? L"Switches" : L"Options";
        r.title = PrettyKey(kv.first);
        r.label = app + L": " + r.title;
        r.detail = L"Sets \"" + kv.first + L"\" in " + cfg + L". " + app + L" saves its settings when it closes, so if it's running Breaker closes it, makes the change and opens it again.";
        r.flags = F_NOSNAP;
        if (kv.second.type == json::Value::Bool) { r.ctl = CtlToggle; r.value = kv.second.b ? L"1" : L"0"; }
        else {
            SuiteChoices(r.id, r.choices);
            r.ctl = r.choices.empty() ? CtlText : CtlChoice;
            if (kv.second.type == json::Value::String) r.value = kv.second.s;
            else json::Write(kv.second, r.value);
            if (kv.first == L"accentColor") r.flags |= F_COLOR;
        }
        rows.push_back(std::move(r));
    }
    std::stable_sort(rows.begin() + 1, rows.end(), [](const Row& a, const Row& b) { return a.group > b.group; });
}

// ============================================================ one theme for every app
static const wchar_t* kThemeKeys[] = {L"theme", L"useWindowsAccent", L"accentColor", L"backdrop", L"opacity", L"cornerRadius",
                                      L"density", L"animation", L"customTheme", L"customColors", L"fontFamily", L"fontSize"};

int SuiteApplyTheme(bool restartRunning, std::vector<std::wstring>* skipped) {
    json::Value mine;
    if (!LoadJson(ConfigPath(), mine)) return 0;
    int done = 0;
    for (auto& app : SuiteNames()) {
        std::wstring exe = SuiteExe(app), cfg = SuiteConfig(app, exe);
        if (exe.empty() || !FileExists(cfg)) continue;
        if (!restartRunning && !RunningPath(app).empty()) { if (skipped) skipped->push_back(app); continue; }
        bool ok = WithAppClosed(app, [&] {
            json::Value root;
            if (!LoadJson(cfg, root)) return false;
            for (auto key : kThemeKeys) {
                const json::Value* v = mine.Get(key);
                if (!v) continue;
                // font keys only where the app already has them; "slow" animation is Breaker-only
                if ((wcscmp(key, L"fontFamily") == 0 || wcscmp(key, L"fontSize") == 0) && !root.Get(key)) continue;
                if (wcscmp(key, L"animation") == 0 && v->s == L"slow") { root.Set(key, json::Value::Str(L"smooth")); continue; }
                root.Set(key, *v);
            }
            return SaveJson(cfg, root);
        });
        if (ok) { ++done; Log(L"Theme applied to %ls", app.c_str()); }
    }
    return done;
}

// ============================================================ updates for the other apps
static bool FetchLatest(const std::wstring& app, Latest& out) {
    std::string body;
    std::wstring err;
    if (!updater::Fetch(L"https://api.github.com/repos/larosa023/" + app + L"/releases/latest", L"application/vnd.github+json", body, 2 * 1024 * 1024, &err)) return false;
    json::Value root;
    std::wstring text = Utf8ToWide(body);
    if (!json::Parser(text).Parse(root) || root.type != json::Value::Object) return false;
    std::wstring tag = root.GetStr(L"tag_name");
    if (tag.empty()) return false;
    out.version = (tag[0] == L'v' || tag[0] == L'V') ? tag.substr(1) : tag;
    out.page = root.GetStr(L"html_url");
    out.notes = root.GetStr(L"body");
    if (const json::Value* assets = root.Get(L"assets"); assets && assets->type == json::Value::Array) {
        for (auto& a : assets->arr) {
            std::wstring name = a.GetStr(L"name"), url = a.GetStr(L"browser_download_url");
            if (_wcsicmp(name.c_str(), (app + L".exe").c_str()) == 0) out.exeUrl = url;
            else if (_wcsicmp(name.c_str(), L"SHA256SUMS.txt") == 0) out.sumsUrl = url;
        }
    }
    out.ok = true;
    return true;
}

void BuildMyAppsRemote(std::vector<Row>&) {
    {
        std::lock_guard<std::mutex> lock(g_suiteLock);
        if (NowUnix() - g_latestAt < 15 * 60) return;      // checked recently
        g_latestAt = NowUnix();
    }
    bool any = false;
    for (auto& app : SuiteNames()) {
        if (SuiteExe(app).empty()) continue;
        Latest l;
        if (!FetchLatest(app, l)) continue;
        std::lock_guard<std::mutex> lock(g_suiteLock);
        g_latest[app] = l;
        any = true;
    }
    if (any) Ui([] { if (UiRoute() == L"myapps") UiReloadPage(); });
}

static void UpdateApp(const std::wstring& app, const Latest& l) {
    auto result = std::make_shared<std::wstring>();
    UiBusy(L"Updating " + app + L"\x2026", [app, l, result] {
        std::string exeData, sums;
        std::wstring err;
        if (l.exeUrl.empty() || l.sumsUrl.empty()) { *result = app + L"'s release has no downloadable exe with a checksum."; return; }
        if (!updater::Fetch(l.sumsUrl, L"application/octet-stream", sums, 1024 * 1024, &err) ||
            !updater::Fetch(l.exeUrl, L"application/octet-stream", exeData, 200 * 1024 * 1024, &err)) { *result = L"Download failed: " + err; return; }
        std::wstring want = updater::HashFromSums(sums, app + L".exe");
        if (want.empty() || _wcsicmp(want.c_str(), updater::Sha256(exeData).c_str()) != 0) { *result = L"The download didn't match its checksum, so it was thrown away."; return; }
        std::wstring exe = SuiteExe(app);
        if (exe.empty()) { *result = app + L" isn't installed any more."; return; }
        std::wstring fresh = exe + L".new", old = exe + L".old";
        HANDLE h = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0;
        bool wrote = h != INVALID_HANDLE_VALUE && WriteFile(h, exeData.data(), (DWORD)exeData.size(), &written, nullptr) && written == exeData.size();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!wrote) { DeleteFileW(fresh.c_str()); *result = L"Couldn't write into " + app + L"'s folder."; return; }
        bool wasRunning = QuitApp(app, exe);
        bool ok = MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) && MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!ok) {
            if (!FileExists(exe)) MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);   // put the old one back
            DeleteFileW(fresh.c_str());
            *result = L"Couldn't replace " + app + L".exe. Close " + app + L" and try again.";
        } else {
            Log(L"Updated %ls to %ls", app.c_str(), l.version.c_str());
            *result = app + L" updated to " + l.version;
        }
        if (wasRunning) ShellOpen(exe);
    }, [result] { UiToast(*result); UiReloadPage(); });
}

void BuildMyApps(std::vector<Row>& rows) {
    Row theme = Info(L"One look for everything", L"Use Breaker's theme in all my apps",
                     L"Copies the theme, accent, backdrop, corners, density and animation speed to every installed suite app. They all read the same theme format.");
    theme.actions.push_back({L"Apply now", [] {
        if (!UiConfirm(L"Apply Breaker's theme to all your installed suite apps?\n\nApps that are running are closed and reopened so they pick it up.")) return;
        auto n = std::make_shared<int>(0);
        UiBusy(L"Applying the theme\x2026", [n] { *n = SuiteApplyTheme(true, nullptr); },
               [n] { UiToast(*n ? L"Theme applied to " + std::to_wstring(*n) + (*n == 1 ? L" app" : L" apps") : L"No other suite apps are installed yet"); });
    }});
    rows.push_back(std::move(theme));
    Row sync;
    sync.id = L"app.syncSuiteTheme";
    sync.group = L"One look for everything";
    sync.title = L"Keep them in sync";
    sync.desc = L"Whenever you change Breaker's look, the same change is written to the other apps (running apps pick it up the next time they start).";
    sync.ctl = CtlToggle;
    sync.flags = F_NOHIST;
    sync.known = false;
    rows.push_back(std::move(sync));

    std::vector<std::wstring> missing;
    for (auto& app : SuiteNames()) {
        std::wstring exe = SuiteExe(app);
        if (exe.empty()) { missing.push_back(app); continue; }
        std::wstring ver = FileVersion(exe);
        Latest l;
        {
            std::lock_guard<std::mutex> lock(g_suiteLock);
            auto it = g_latest.find(app);
            if (it != g_latest.end()) l = it->second;
        }
        bool newer = l.ok && !ver.empty() && updater::CompareVersions(l.version, ver) > 0;
        Row r = Info(L"Installed", app, L"Version " + (ver.empty() ? L"unknown" : ver) + (RunningPath(app).empty() ? L"" : L"  \x00B7  Running") + L"\n" + exe,
                     !l.ok ? L"" : newer ? L"Update available: " + l.version : L"Up to date");
        r.alert = newer;
        if (newer) r.actions.push_back({L"Update", [app, l] { UpdateApp(app, l); }});
        r.actions.push_back({L"Open", [exe] { ShellOpen(exe); }});
        r.actions.push_back({L"Settings", [app] { UiNavigate(L"myapps/" + app); }});
        rows.push_back(std::move(r));
    }
    Row self = Info(L"Installed", L"Breaker", std::wstring(L"Version ") + BREAKER_VERSION + L"  \x00B7  This app");
    self.actions.push_back({L"Updates", [] { UiNavigate(L"settings/updates"); }});
    rows.push_back(std::move(self));
    for (auto& app : missing) {
        Row r = Info(L"Not installed", app, L"Not found on this PC.");
        r.actions.push_back({L"Get it", [app] { OpenTarget(L"https://github.com/larosa023/" + app + L"/releases/latest"); }});
        rows.push_back(std::move(r));
    }
    Row add = Info(L"More", L"Add another app", L"For a suite app Breaker doesn't know yet. It only needs to follow the suite's conventions (same name for the exe, the settings folder and the GitHub repo).");
    add.actions.push_back({L"Add by name", [] {
        UiPrompt(L"Add a suite app", {L"App name"}, {L""}, [](const std::vector<std::wstring>& v) {
            std::wstring name = Trim(v[0]);
            if (name.empty() || name.find_first_of(L"\\/:*?\"<>| ") != std::wstring::npos) return;
            g_config.suiteApps.push_back(name);
            ConfigSave();
            UiReloadPage();
        });
    }});
    rows.push_back(std::move(add));
}
