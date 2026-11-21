// Breaker - its own options ("app." settings) and the Breaker settings pages
#include "pages.h"
#include "config.h"
#include "gfx.h"
#include "system.h"
#include "suite.h"
#include "autoupdate.h"
#include "features.h"
#include <algorithm>
#include <cmath>

void UiBackgroundChanged();   // window.cpp: tray icon, Win+I hook and timers follow the options

namespace {
struct BoolOpt { const wchar_t* id; bool Config::*m; bool look; };
struct StrOpt { const wchar_t* id; std::wstring Config::*m; bool look; };
struct NumOpt { const wchar_t* id; float Config::*m; float scale; };
}
static const BoolOpt kBools[] = {
    {L"app.keepInTray", &Config::keepInTray, false}, {L"app.startWithWindows", &Config::startWithWindows, false},
    {L"app.winI", &Config::winI, false}, {L"app.watchdogEnabled", &Config::watchdogEnabled, false},
    {L"app.watchdogAtSignIn", &Config::watchdogAtSignIn, false}, {L"app.confirmRisky", &Config::confirmRisky, false}, {L"app.restorePointFirst", &Config::restorePointFirst, false},
    {L"app.autoProfiles", &Config::autoProfiles, false}, {L"app.showRecommended", &Config::showRecommended, true},
    {L"app.useWindowsAccent", &Config::useWindowsAccent, true}, {L"app.respectHighContrast", &Config::respectHighContrast, true},
    {L"app.syncSuiteTheme", &Config::syncSuiteTheme, false}, {L"app.autoCheck", &Config::autoCheck, false},
    {L"app.autoInstall", &Config::autoInstall, false},
};
static const StrOpt kStrs[] = {
    {L"app.theme", &Config::theme, true}, {L"app.backdrop", &Config::backdrop, true}, {L"app.density", &Config::density, true},
    {L"app.animation", &Config::animation, true}, {L"app.fontFamily", &Config::fontFamily, true}, {L"app.channel", &Config::channel, false},
};
static const NumOpt kNums[] = {
    {L"app.opacity", &Config::opacity, 100}, {L"app.cornerRadius", &Config::cornerRadius, 1}, {L"app.fontSize", &Config::fontSize, 1},
};

static void SyncSuite() {
    if (!g_config.syncSuiteTheme) return;
    RunAsync([] {
        auto skipped = std::make_shared<std::vector<std::wstring>>();
        SuiteApplyTheme(false, skipped.get());
        if (!skipped->empty()) {
            std::wstring names;
            for (auto& s : *skipped) names += (names.empty() ? L"" : L", ") + s;
            Ui([names] { UiToast(names + L" will pick up the new look the next time it starts"); });
        }
    });
}

static bool AppReadUi(const std::wstring& id, std::wstring& out) {
    for (auto& o : kBools) if (id == o.id) { out = g_config.*o.m ? L"1" : L"0"; return true; }
    for (auto& o : kStrs) if (id == o.id) { out = g_config.*o.m; return true; }
    for (auto& o : kNums) if (id == o.id) { out = std::to_wstring((int)std::lround(g_config.*o.m * o.scale)); return true; }
    if (id == L"app.accentColor") { out = HexColor(g_config.accentColor); return true; }
    if (id == L"app.checkHours") { out = std::to_wstring(g_config.checkHours); return true; }
    if (id == L"app.portable") { out = IsPortable() ? L"1" : L"0"; return true; }
    if (StartsWith(id, L"app.sidebar:")) {
        auto& h = g_config.sidebarHidden;
        out = std::find(h.begin(), h.end(), id.substr(12)) == h.end() ? L"1" : L"0";
        return true;
    }
    if (StartsWith(id, L"app.profileTrigger:")) {
        Profile* p = FindProfile(id.substr(19));
        if (!p) return false;
        out = StartsWith(p->trigger, L"exe:") ? L"?exe:" : p->trigger;
        return true;
    }
    return false;
}

static WriteResult AppWriteUi(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    bool look = false, found = false;
    for (auto& o : kBools) if (id == o.id) { g_config.*o.m = v == L"1"; look = o.look; found = true; }
    for (auto& o : kStrs) if (id == o.id) { g_config.*o.m = v; look = o.look; found = true; }
    for (auto& o : kNums) if (id == o.id) { g_config.*o.m = (float)_wtof(v.c_str()) / o.scale; look = true; found = true; }
    if (id == L"app.accentColor") { g_config.accentColor = ParseHexColor(v, g_config.accentColor); g_config.useWindowsAccent = false; look = found = true; }
    else if (id == L"app.checkHours") { g_config.checkHours = std::clamp(_wtoi(v.c_str()), 1, 168); found = true; }
    else if (id == L"app.portable") {
        if (!SetPortable(v == L"1", &err)) return W_FAIL;
        UiToast(v == L"1" ? L"Portable mode on. Settings now live in the data folder next to Breaker.exe (restart Breaker to switch over)."
                          : L"Portable mode off. Settings are back in your user profile (restart Breaker to switch over).");
        return W_OK;
    } else if (StartsWith(id, L"app.sidebar:")) {
        auto& h = g_config.sidebarHidden;
        std::wstring cat = id.substr(12);
        h.erase(std::remove(h.begin(), h.end(), cat), h.end());
        if (v != L"1") h.push_back(cat);
        look = found = true;
    } else if (StartsWith(id, L"app.profileTrigger:")) {
        Profile* p = FindProfile(id.substr(19));
        if (!p) { err = L"That profile no longer exists."; return W_FAIL; }
        p->trigger = v;
        found = true;
    }
    if (!found) { err = L"Unknown option."; return W_FAIL; }

    // Options that depend on each other
    if (id == L"app.theme" && v != L"custom") { g_config.customColors.clear(); g_config.customTheme.clear(); }
    if (id == L"app.winI" && g_config.winI) g_config.keepInTray = g_config.startWithWindows = true;   // the shortcut needs Breaker running
    if (id == L"app.autoProfiles" && g_config.autoProfiles) g_config.keepInTray = true;
    if (id == L"app.keepInTray" && !g_config.keepInTray) g_config.winI = g_config.autoProfiles = false;
    if (id == L"app.startWithWindows" || id == L"app.winI") SetStartWithWindows(g_config.startWithWindows);
    if (id == L"app.watchdogAtSignIn") SetWatchdogAtSignIn(g_config.watchdogAtSignIn);
    if (id == L"app.autoCheck" || id == L"app.channel" || id == L"app.checkHours") AutoUpdateApplySettings();
    ConfigSave();
    UiBackgroundChanged();
    if (look) { UiThemeChanged(); SyncSuite(); }
    return W_OK;
}

static bool AppRead(const std::wstring& id, std::wstring& out) {
    bool ok = false;
    UiSync([&] { ok = AppReadUi(id, out); });
    return ok;
}
static WriteResult AppWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    WriteResult r = W_FAIL;
    UiSync([&] { r = AppWriteUi(id, v, err); });
    return r;
}
static void AppChoices(const std::wstring& id, Choices& out) {
    if (id == L"app.theme") {
        for (int i = 0; i < gfx::kThemePresetCount; ++i) out.push_back({gfx::kThemePresets[i].name, gfx::kThemePresets[i].id});
        out.push_back({L"Custom (imported theme)", L"custom"});
    } else if (id == L"app.backdrop") out = {{L"Solid", L"solid"}, {L"Mica", L"mica"}, {L"Acrylic (blur)", L"acrylic"}};
    else if (id == L"app.density") out = {{L"Comfortable", L"comfortable"}, {L"Compact", L"compact"}};
    else if (id == L"app.animation") out = {{L"None", L"none"}, {L"Fast", L"fast"}, {L"Smooth", L"smooth"}, {L"Slow", L"slow"}};
    else if (id == L"app.channel") out = {{L"Stable", L"stable"}, {L"Beta (early versions)", L"beta"}};
    else if (id == L"app.checkHours") out = {{L"Every hour", L"1"}, {L"Every 3 hours", L"3"}, {L"Every 6 hours", L"6"}, {L"Every 12 hours", L"12"}, {L"Once a day", L"24"}};
    else if (id == L"app.fontFamily") {
        out.push_back({L"Default (Segoe UI Variable)", L""});
        for (const wchar_t* f : {L"Segoe UI", L"Inter", L"Bahnschrift", L"Calibri", L"Verdana", L"Tahoma", L"Trebuchet MS", L"Georgia", L"Cascadia Code", L"Consolas", L"Arial"})
            if (gfx::FontAvailable(f)) out.push_back({f, f});
        out.push_back({L"Another font\x2026", L"?"});
    }
}

static const Handler kAppHandlers[] = {{L"app.", AppRead, AppWrite, AppChoices, F_NOHIST | F_NOSNAP | F_NOWATCH}};
void RegisterAppHandlers() { RegisterHandlers(kAppHandlers, 1); }

// ============================================================ pages
static Row Info(const wchar_t* group, const std::wstring& title, const std::wstring& desc, const std::wstring& valueText = L"") {
    Row r;
    r.group = group;
    r.title = title;
    r.desc = desc;
    r.valueText = valueText;
    return r;
}

static Row Opt(const wchar_t* id, const wchar_t* group, const wchar_t* title, const wchar_t* desc, Ctl ctl) {
    Row r;
    r.id = id;
    r.group = group;
    r.title = title;
    r.desc = desc;
    r.ctl = ctl;
    r.flags = F_NOHIST;
    AppReadUi(id, r.value);
    if (ctl == CtlChoice) AppChoices(id, r.choices);
    return r;
}

static Row Slider(const wchar_t* id, const wchar_t* group, const wchar_t* title, const wchar_t* desc, float lo, float hi, float step, const wchar_t* unit) {
    Row r = Opt(id, group, title, desc, CtlSlider);
    r.smin = lo;
    r.smax = hi;
    r.sstep = step;
    r.unit = unit;
    return r;
}

void BuildSettingsGeneral(std::vector<Row>& rows) {
    rows.push_back(Opt(L"app.keepInTray", L"Running in the background", L"Keep Breaker in the tray", L"Closing the window leaves a tray icon with your quick toggles and profiles. Off (the default) means Breaker runs only while its window is open.", CtlToggle));
    rows.push_back(Opt(L"app.startWithWindows", L"Running in the background", L"Start with Windows", L"Starts hidden in the tray when you sign in.", CtlToggle));
    rows.push_back(Opt(L"app.winI", L"Running in the background", L"Open Breaker with Win+I", L"Win+I opens Breaker instead of Windows Settings. Shift+Win+I still opens the real Settings app, and so does the button in Breaker's title bar. Turns on the two options above, because Breaker has to be running to hear the shortcut.", CtlToggle));
    rows.push_back(Opt(L"app.autoProfiles", L"Running in the background", L"Switch profiles automatically", L"Applies a profile when its condition starts (a game, battery power, a certain app) and puts your settings back when it ends. Needs the tray icon.", CtlToggle));
    rows.push_back(Opt(L"app.confirmRisky", L"Safety", L"Ask before risky changes", L"Shows a warning before changes that can break something or lock you out, like turning off the firewall or disabling a service.", CtlToggle));
    rows.push_back(Opt(L"app.restorePointFirst", L"Safety", L"Make a restore point before big changes", L"Before Debloat, Checkup or a snapshot changes many settings at once, Breaker asks Windows for a System Restore point. Needs System Protection to be on for your system drive; Breaker never turns it on by itself.", CtlToggle));
    rows.push_back(Opt(L"app.showRecommended", L"Safety", L"Show recommendations", L"Marks the value Breaker suggests next to settings where it helps.", CtlToggle));
    Row real = Info(L"Windows", L"Open Windows Settings", L"The real Settings app is always reachable.");
    real.ctl = CtlLink;
    real.target = L"ms-settings:";
    rows.push_back(std::move(real));
}

void BuildSettingsAppearance(std::vector<Row>& rows) {
    Row theme = Opt(L"app.theme", L"Theme", L"Theme", L"Follow Windows switches between Dark and Light with Windows' own app mode.", CtlChoice);
    if (g_config.theme == L"custom" && !g_config.customTheme.empty()) theme.desc = L"Using the imported theme \"" + g_config.customTheme + L"\".";
    rows.push_back(std::move(theme));
    rows.push_back(Opt(L"app.useWindowsAccent", L"Theme", L"Follow the Windows accent color", L"Ember and Ocean bring their own accent while this is on.", CtlToggle));
    Row accent = Opt(L"app.accentColor", L"Theme", L"Accent color", L"Used for switches, highlights and focus. Picking one turns off following Windows.", CtlText);
    accent.flags |= F_COLOR;
    rows.push_back(std::move(accent));
    rows.push_back(Opt(L"app.backdrop", L"Background", L"Background style", L"Mica and Acrylic let your wallpaper show through (Windows 11 22H2 or later). Glass always uses Acrylic.", CtlChoice));
    rows.push_back(Slider(L"app.opacity", L"Background", L"Tint strength", L"Lower lets more of the backdrop through.", 40, 100, 2, L"%"));
    rows.push_back(Slider(L"app.cornerRadius", L"Shape and size", L"Corner radius", L"0 is square.", 0, 16, 1, L" px"));
    rows.push_back(Opt(L"app.density", L"Shape and size", L"Density", L"Compact fits more on screen.", CtlChoice));
    rows.push_back(Opt(L"app.fontFamily", L"Text", L"Font", L"", CtlChoice));
    rows.push_back(Slider(L"app.fontSize", L"Text", L"Font size", L"Windows' own Text size setting (Accessibility) is added on top of this.", 11, 17, 1, L" px"));
    rows.push_back(Opt(L"app.animation", L"Motion", L"Animation speed", L"None turns off every animation and smooth scrolling.", CtlChoice));
    rows.push_back(Opt(L"app.respectHighContrast", L"Accessibility", L"Use Windows contrast themes", L"When a contrast theme is on in Windows, Breaker uses its colors.", CtlToggle));

    Row files = Info(L"Theme files", L"Import or export a theme", L"Themes are small JSON files in the format Foyer and Strata use, so one file themes the whole suite. Import also reads a Foyer or Strata config.json.");
    files.actions.push_back({L"Import", [] {
        std::wstring path, err;
        if (!UiPickFile(L"Theme files (*.json)\0*.json\0All files\0*.*\0", path)) return;
        if (ThemeImport(path, &err)) { UiThemeChanged(); SyncSuite(); UiToast(L"Theme imported"); UiReloadPage(); }
        else UiToast(err);
    }});
    files.actions.push_back({L"Export", [] {
        std::wstring path;
        if (!UiPickFile(L"Theme files (*.json)\0*.json\0", path, true, L"My theme.json", L"json")) return;
        std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
        name = name.substr(0, name.rfind(L'.'));
        UiToast(ThemeExport(path, name) ? L"Theme saved" : L"Couldn't write that file");
    }});
    rows.push_back(std::move(files));
    Row suite = Opt(L"app.syncSuiteTheme", L"Theme files", L"Use this look in all my apps", L"Writes every change on this page to Foyer, Strata and the rest of the suite too. See My Apps to apply it right now.", CtlToggle);
    rows.push_back(std::move(suite));
}

template <class V> static void MoveBy(V& v, const std::wstring& item, int delta) {
    auto it = std::find(v.begin(), v.end(), item);
    if (it == v.end()) return;
    int i = (int)(it - v.begin()), j = i + delta;
    if (j < 0 || j >= (int)v.size()) return;
    std::swap(v[i], v[j]);
}

std::vector<std::wstring> SidebarOrder();   // window.cpp: every category id in display order

void BuildSettingsLayout(std::vector<Row>& rows) {
    for (auto& id : SidebarOrder()) {
        const Category* c = FindCategory(id);
        if (!c || id == L"home" || id == L"settings") continue;
        std::wstring rowId = L"app.sidebar:" + id;
        Row r;
        r.id = rowId;
        r.group = L"Sidebar";
        r.title = c->name;
        r.ctl = CtlToggle;
        r.flags = F_NOHIST;
        AppReadUi(rowId, r.value);
        auto mover = [id](int delta) {
            return [id, delta] {
                g_config.sidebarOrder = SidebarOrder();
                MoveBy(g_config.sidebarOrder, id, delta);
                ConfigSave();
                UiThemeChanged();
                UiReloadPage();
            };
        };
        r.actions.push_back({L"\x2191", mover(-1)});
        r.actions.push_back({L"\x2193", mover(1)});
        rows.push_back(std::move(r));
    }
    Row reset = Info(L"Sidebar", L"Reset the sidebar", L"Show every category in the original order.");
    reset.actions.push_back({L"Reset", [] {
        g_config.sidebarOrder.clear();
        g_config.sidebarHidden.clear();
        ConfigSave();
        UiThemeChanged();
        UiReloadPage();
    }});
    rows.push_back(std::move(reset));
    for (auto& id : g_config.pinned) {
        std::wstring pid = id;
        Row r = Info(L"Pinned to Home", NameOf(id), L"");
        auto mover = [pid](int delta) {
            return [pid, delta] { MoveBy(g_config.pinned, pid, delta); ConfigSave(); UiReloadPage(); };
        };
        r.actions.push_back({L"\x2191", mover(-1)});
        r.actions.push_back({L"\x2193", mover(1)});
        r.actions.push_back({L"Unpin", [pid] {
            auto& p = g_config.pinned;
            p.erase(std::remove(p.begin(), p.end(), pid), p.end());
            ConfigSave();
            UiReloadPage();
        }});
        rows.push_back(std::move(r));
    }
    if (g_config.pinned.empty()) rows.push_back(Info(L"Pinned to Home", L"Nothing pinned", L"Pin a setting from its info panel (the \x24D8 button)."));
}

static std::wstring PlainNotes(const std::wstring& md) {
    std::wstring out;
    for (auto& line : Split(md, L"\n")) {
        std::wstring t = Trim(line);
        while (!t.empty() && t[0] == L'#') t = Trim(t.substr(1));
        if (StartsWith(t, L"- ") || StartsWith(t, L"* ")) t = L"\x2022 " + t.substr(2);
        t = ReplaceAll(ReplaceAll(t, L"**", L""), L"`", L"");
        if (!t.empty()) out += (out.empty() ? L"" : L"\n") + t;
    }
    return out.size() > 1400 ? out.substr(0, 1400) + L"\x2026" : out;
}

void BuildSettingsUpdates(std::vector<Row>& rows) {
    updater::Status st = updater::Get();
    std::wstring title = std::wstring(L"Breaker ") + BREAKER_VERSION, desc;
    switch (st.state) {
        case updater::State::Checking: desc = L"Checking for updates\x2026"; break;
        case updater::State::UpToDate: desc = L"You have the latest version."; break;
        case updater::State::Available: desc = L"Version " + st.release.version + L" is available."; break;
        case updater::State::Downloading: desc = L"Downloading " + st.release.version + L"\x2026 " + std::to_wstring(st.percent) + L"%"; break;
        case updater::State::Verifying: desc = L"Checking the download\x2026"; break;
        case updater::State::Ready: desc = L"Version " + st.release.version + L" is downloaded and verified."; break;
        case updater::State::Installing: desc = L"Restarting\x2026"; break;
        case updater::State::Error: desc = st.message.empty() ? L"Couldn't check for updates." : st.message; break;
        default: desc = g_config.lastCheck > 0 ? L"Last checked " + FormatWhen(g_config.lastCheck) + L"." : L"Not checked yet."; break;
    }
    Row status = Info(L"Version", title, desc);
    status.alert = st.state == updater::State::Error;
    if (st.state == updater::State::Available) status.actions.push_back({L"Update now", [] { AutoUpdateDownloadNow(); }});
    else if (st.state == updater::State::Ready) status.actions.push_back({L"Restart to update", [] { AutoUpdateInstallNow(); }});
    if (!updater::Busy()) status.actions.push_back({L"Check now", [] { AutoUpdateCheckNow(); }});
    rows.push_back(std::move(status));
    if (AutoUpdateAvailable() && !st.release.notes.empty()) {
        Row notes = Info(L"Version", L"What's new in " + st.release.version, PlainNotes(st.release.notes));
        std::wstring page = st.release.page;
        if (!page.empty()) notes.actions.push_back({L"Release page", [page] { OpenTarget(page); }});
        rows.push_back(std::move(notes));
    }
    if (!AutoUpdateFailedVersion().empty())
        rows.push_back(Info(L"Version", L"Version " + AutoUpdateFailedVersion() + L" was rolled back", L"It didn't start properly, so Breaker restored this version. \"Check now\" offers it again if you want to retry."));
    rows.push_back(Opt(L"app.autoCheck", L"Automatic updates", L"Check for updates automatically", L"On startup and every few hours, from GitHub Releases (larosa023/Breaker). Nothing else is sent.", CtlToggle));
    rows.push_back(Opt(L"app.checkHours", L"Automatic updates", L"How often", L"", CtlChoice));
    rows.push_back(Opt(L"app.autoInstall", L"Automatic updates", L"Install updates silently", L"Downloads, verifies and installs without asking, then reopens on the same page. Never while a game or full-screen app is running or while a setting is being changed.", CtlToggle));
    rows.push_back(Opt(L"app.channel", L"Automatic updates", L"Release channel", L"Beta gets new versions first and may have rough edges.", CtlChoice));
    rows.push_back(Info(L"How updates work", L"Verified and reversible", L"Each download is checked against the SHA-256 hash published with the release. If a new version fails to start, Breaker puts the previous one back by itself."));
}

void BuildSettingsAbout(std::vector<Row>& rows) {
    Row about = Info(L"Breaker", std::wstring(L"Breaker ") + BREAKER_VERSION, L"A fast replacement for Windows Settings and Control Panel. Part of the larosa023 suite.\n" + ExePath());
    about.actions.push_back({L"GitHub", [] { OpenTarget(L"https://github.com/larosa023/Breaker"); }});
    rows.push_back(std::move(about));
    Row inst = Info(L"Install", IsInstalledCopy() ? L"Breaker is installed" : L"Install Breaker",
                    IsInstalledCopy() ? L"In " + InstallDir() + L", with a Start menu entry. No administrator rights were needed."
                                      : L"Copies Breaker to " + InstallDir() + L", adds a Start menu entry and lists it under installed apps. No administrator rights needed.");
    if (!IsInstalledCopy())
        inst.actions.push_back({L"Install", [] {
            std::wstring err;
            if (!InstallBreaker(&err)) { UiToast(err); return; }
            if (UiConfirm(L"Breaker is installed.\n\nSwitch to the installed copy now?")) {
                ShellOpen(InstallDir() + L"\\Breaker.exe", L"--wait-old");
                PostMessageW(UiWindow(), WM_CLOSE, 1, 0);
            } else UiReloadPage();
        }});
    else
        inst.actions.push_back({L"Uninstall", [] {
            int r = MessageBoxW(UiWindow(), L"Uninstall Breaker?\n\nYes removes Breaker but keeps your settings, history and profiles.\nNo removes those too.\n\nWindows settings you changed stay as they are.",
                                L"Breaker", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return;
            UninstallBreaker(r == IDYES);
            PostMessageW(UiWindow(), WM_CLOSE, 1, 0);
        }});
    rows.push_back(std::move(inst));
    rows.push_back(Opt(L"app.portable", L"Install", L"Portable mode", L"Keeps settings in a data folder next to Breaker.exe instead of your user profile, so it can live on a USB stick. You can also create an empty file named \"portable\" beside the exe.", CtlToggle));
    Row data = Info(L"Troubleshooting", L"Settings and log folder", DataDir() + L"\nbreaker.log records what Breaker did and any errors, on this PC only.");
    data.actions.push_back({L"Open folder", [] { ShellOpen(DataDir()); }});
    data.actions.push_back({L"Open log", [] { ShellOpen(DataDir() + L"\\breaker.log"); }});
    rows.push_back(std::move(data));
    rows.push_back(Info(L"Keyboard", L"Shortcuts", L"Ctrl+K search   \x00B7   Arrows and Enter to move and change   \x00B7   Esc back   \x00B7   Ctrl+Z undo\nAlt+Left / Alt+Right back and forward   \x00B7   Ctrl+Tab next tab   \x00B7   I shows a setting's details   \x00B7   Ctrl+F filter a list   \x00B7   F5 refresh"));
}
