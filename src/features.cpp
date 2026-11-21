// Breaker - Home, Debloat, Profiles, Undo history, Update watchdog, Snapshots and universal search
#include "features.h"
#include "config.h"
#include "json.h"
#include "autoupdate.h"
#include <tlhelp32.h>
#include <shellapi.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <set>

static Row Info(const wchar_t* group, const std::wstring& title, const std::wstring& desc, const std::wstring& valueText = L"") {
    Row r;
    r.group = group;
    r.title = title;
    r.desc = desc;
    r.valueText = valueText;
    return r;
}

std::wstring WindowsBuildText() {
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring build;
    DWORD ubr = 0;
    RegReadStr(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild", build);
    RegReadDword(HKEY_LOCAL_MACHINE, nt, L"UBR", ubr);
    return build + L"." + std::to_wstring(ubr);
}

static std::wstring PageWhere(const std::wstring& route) {
    const PageInfo* p = FindPage(route);
    const Category* c = FindCategory(CategoryOf(route));
    if (!p || !c) return L"";
    return wcscmp(c->name, p->title) == 0 ? std::wstring(c->name) : std::wstring(c->name) + L"  \x203A  " + p->title;
}

// ============================================================ universal search
static const wchar_t* kSynonyms[] = {
    L"dark night theme black", L"wifi wi-fi wireless internet network", L"sound audio volume speaker", L"screen display monitor",
    L"ads ad suggestions promotions bloat junk", L"privacy tracking telemetry spying", L"startup boot autostart", L"mouse pointer cursor",
    L"uninstall remove delete", L"apps programs applications software", L"update updates patch", L"power battery sleep energy",
    L"wallpaper background desktop", L"mic microphone", L"camera webcam", L"color colour accent", L"taskbar tray", L"fast speed performance",
    L"notification notifications alerts toast", L"file explorer folder", L"keyboard typing keys", L"game gaming games", L"account user login",
    L"pc computer device", L"clean cleanup storage space",
};

namespace {
struct SearchItem { const Def* def; std::wstring name, keys, rest; };
}

static std::vector<SearchItem>& SearchIndex() {
    static std::vector<SearchItem> items;
    if (!items.empty()) return items;
    for (int i = 0; i < kDefCount; ++i) {
        const Def& d = kDefs[i];
        SearchItem it;
        it.def = &d;
        it.name = ToLower(d.name);
        it.keys = ToLower(d.keys ? d.keys : L"");
        it.rest = ToLower(std::wstring(d.desc ? d.desc : L"") + L" " + d.group + L" " + PageWhere(d.page));
        items.push_back(std::move(it));
    }
    return items;
}

// Edit distance (insert, delete, change, swap neighbours), giving up early once it passes `limit`
static int EditDistance(const std::wstring& a, const std::wstring& b, int limit) {
    int n = (int)a.size(), m = (int)b.size();
    if (std::abs(n - m) > limit) return limit + 1;
    std::vector<int> prev2(m + 1), prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int best = cur[0];
        for (int j = 1; j <= m; ++j) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) cur[j] = std::min(cur[j], prev2[j - 2] + 1);
            best = std::min(best, cur[j]);
        }
        if (best > limit) return limit + 1;
        prev2.swap(prev);
        prev.swap(cur);
    }
    return prev[m];
}

// "blutooth", "telemtry", "wallpapr": a word close to what was typed still matches, a little lower
static bool NearWord(const std::wstring& text, const std::wstring& tok) {
    if (tok.size() < 4) return false;
    int limit = tok.size() >= 7 ? 2 : 1;
    for (auto& w : Split(text, L" ")) {
        if (w.size() + limit < tok.size()) continue;
        // compare against the start of longer words too, so a half-typed word with a typo still finds it
        std::wstring head = w.size() > tok.size() + 1 ? w.substr(0, tok.size()) : w;
        if (EditDistance(tok, head, limit) <= limit) return true;
    }
    return false;
}

static int ScoreToken(const SearchItem& it, const std::wstring& tok) {
    size_t p = it.name.find(tok);
    if (p == 0) return 100;
    if (p != std::wstring::npos) return it.name[p - 1] == L' ' ? 70 : 45;
    p = it.keys.find(tok);
    if (p != std::wstring::npos) return (p == 0 || it.keys[p - 1] == L' ') ? 30 : 18;
    if (it.rest.find(tok) != std::wstring::npos) return 10;
    if (NearWord(it.name, tok)) return 24;
    if (NearWord(it.keys, tok)) return 12;
    return 0;
}

void SearchRows(const std::wstring& query, std::vector<Row>& out) {
    std::vector<std::wstring> tokens = Split(ToLower(Trim(query)), L" ");
    if (tokens.empty()) return;
    std::vector<std::vector<std::wstring>> alts(tokens.size());
    for (size_t t = 0; t < tokens.size(); ++t) {
        for (auto syn : kSynonyms) {
            auto words = Split(syn, L" ");
            if (std::find(words.begin(), words.end(), tokens[t]) == words.end()) continue;
            for (auto& w : words) if (w != tokens[t]) alts[t].push_back(w);
        }
    }
    std::vector<std::pair<int, const Def*>> hits;
    for (auto& it : SearchIndex()) {
        int total = 0;
        bool all = true;
        for (size_t t = 0; t < tokens.size() && all; ++t) {
            int s = ScoreToken(it, tokens[t]);
            for (auto& a : alts[t]) s = std::max(s, ScoreToken(it, a) * 6 / 10);
            if (!s) all = false;
            total += s;
        }
        if (all) hits.push_back({total + (it.def->ctl == CtlLink ? 0 : 5), it.def});
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
    // Pages match too ("power" finds System > Power)
    std::wstring q = ToLower(Trim(query));
    for (int i = 0; i < kPageCount; ++i) {
        if (ToLower(kPages[i].title).find(q) != 0) continue;
        Row r = Info(L"Pages", kPages[i].title, PageWhere(kPages[i].route));
        r.ctl = CtlLink;
        r.target = std::wstring(L"breaker:") + kPages[i].route;
        out.push_back(std::move(r));
    }
    for (size_t i = 0; i < hits.size() && i < 60; ++i) {
        Row r = RowFromDef(*hits[i].second);
        r.group = L"Settings";
        r.where = wcscmp(hits[i].second->page, L"~") == 0 ? L"Breaker" : PageWhere(hits[i].second->page);
        out.push_back(std::move(r));
    }
}

// ============================================================ Home
void BuildHome(std::vector<Row>& rows) {
    // ---- alerts
    const auto& drift = WatchdogDrift();
    if (!drift.empty()) {
        std::wstring names;
        for (size_t i = 0; i < drift.size() && i < 3; ++i) names += (i ? L", " : L"") + drift[i].name;
        if (drift.size() > 3) names += L" and " + std::to_wstring(drift.size() - 3) + L" more";
        std::wstring note = WatchdogBuildNote();
        Row r = Info(L"Needs your attention", drift.size() == 1 ? L"Windows changed one of your settings" : L"Windows changed " + std::to_wstring(drift.size()) + L" of your settings",
                     (note.empty() ? L"" : note + L". ") + names);
        r.alert = true;
        r.actions.push_back({L"Review", [] { UiNavigate(L"watchdog"); }});
        r.actions.push_back({L"Restore all", [] {
            std::vector<Change> changes;
            for (auto& d : WatchdogDrift()) changes.push_back({d.id, d.wanted, d.name});
            UiApply(changes, L"watchdog");
        }});
        rows.push_back(std::move(r));
    }
    updater::Status st = updater::Get();
    if (AutoUpdateAvailable()) {
        Row r = Info(L"Needs your attention", L"Breaker " + st.release.version + L" is available", st.state == updater::State::Ready ? L"Downloaded and verified. Ready to install." : L"See what's new and update in one click.");
        r.actions.push_back({L"What's new", [] { UiNavigate(L"settings/updates"); }});
        rows.push_back(std::move(r));
    }
    if (!AutoUpdateFailedVersion().empty()) {
        Row r = Info(L"Needs your attention", L"The last update was rolled back", L"Version " + AutoUpdateFailedVersion() + L" didn't start properly, so Breaker went back to this version. It won't be installed again automatically.");
        r.alert = true;
        rows.push_back(std::move(r));
    }

    if (CheckupCount() > 0) {
        Row r = Info(L"Needs your attention", CheckupCount() == 1 ? L"1 recommendation isn't applied" : std::to_wstring(CheckupCount()) + L" recommendations aren't applied",
                     L"PC Checkup lists them in one place: ads, tracking and a few safety settings.");
        r.actions.push_back({L"Review", [] { UiNavigate(L"checkup"); }});
        rows.push_back(std::move(r));
    }

    // ---- quick toggles
    for (auto& id : g_config.pinned) {
        const Def* d = FindDef(id);
        if (!d) continue;
        Row r = RowFromDef(*d);
        r.group = L"Quick toggles";
        r.tile = true;
        rows.push_back(std::move(r));
    }
    if (g_config.pinned.empty())
        rows.push_back(Info(L"Quick toggles", L"Pin the settings you use most", L"Open any setting's info panel (the \x24D8 button) and choose Pin to Home."));

    // ---- profiles
    if (!g_config.profiles.empty()) {
        Row r = Info(L"Profile", g_config.activeProfile.empty() ? L"No profile active" : g_config.activeProfile,
                     L"Switch a whole group of settings in one click.");
        for (auto& p : g_config.profiles) {
            std::wstring name = p.name;
            if (name != g_config.activeProfile) r.actions.push_back({name, [name] { ProfileApply(name); }});
            if (r.actions.size() >= 4) break;
        }
        rows.push_back(std::move(r));
    }

    // ---- recent changes
    auto hist = HistoryList();
    int shown = 0;
    for (auto& e : hist) {
        if (shown >= 6) break;
        ++shown;
        Row r = Info(L"Recent changes", e.name, DescribeValue(e.id, e.was) + L"  \x2192  " + DescribeValue(e.id, e.after) + L"   \x00B7   " + FormatWhen(e.time),
                     e.reverted ? L"Undone" : L"");
        uint64_t seq = e.seq;
        if (!e.reverted && e.source != L"undo" && e.source != L"restore")
            r.actions.push_back({L"Undo", [seq] { UiBusy(L"", [seq] { HistoryRevert(seq); }, [] { UiToast(L"Change undone"); UiReloadPage(); }); }});
        rows.push_back(std::move(r));
    }
    if (hist.empty()) rows.push_back(Info(L"Recent changes", L"Nothing changed yet", L"Every change you make in Breaker is listed here with an Undo button."));
    else {
        Row more = Info(L"Recent changes", L"Full undo history", L"");
        more.ctl = CtlLink;
        more.target = L"breaker:history";
        rows.push_back(std::move(more));
    }
}

// ============================================================ Debloat
static const wchar_t* DebloatGroup(const Def& d) {
    std::wstring id = d.id;
    if (id == L"privacy.recall" || StartsWith(id, L"taskbar.") || id == L"debloat.copilot" || id == L"debloat.edgeBackground" || id == L"wu.p2p" ||
        id == L"debloat.askCopilot" || id == L"debloat.clickToDo" || id == L"debloat.edgeSidebar")
        return L"AI and background features";
    if (StartsWith(id, L"privacy.")) return L"Tracking and telemetry";
    if (id == L"start.bing" || id == L"start.highlights") return L"Search";
    return L"Ads and suggestions";
}

void BuildDebloat(std::vector<Row>& rows) {
    std::vector<Change> pending;
    for (int i = 0; i < kDefCount; ++i) {
        const Def& d = kDefs[i];
        if (!(d.flags & F_DEBLOAT) || !d.rec) continue;
        std::wstring cur;
        if (CachedValue(d.id, cur) && cur != d.rec) pending.push_back({d.id, d.rec, L""});
    }
    Row top = Info(L"One click", pending.empty() ? L"Everything recommended is already applied" : std::to_wstring(pending.size()) + L" recommended changes available",
                   L"Applies every recommended switch below in one go, with a single administrator prompt. Preinstalled apps are never removed automatically.");
    if (!pending.empty())
        top.actions.push_back({L"Apply recommended", [pending] {
            if (!UiConfirm(L"Turn off " + std::to_wstring(pending.size()) + L" ads, suggestions and background features?\n\nEvery change is recorded and can be undone from this page or the undo history.")) return;
            std::vector<Change> changes = pending;
            PrependRestorePoint(changes, L"Before Breaker Debloat");
            UiApply(changes, L"debloat");
        }});
    top.actions.push_back({L"Undo Debloat changes", [] {
        std::vector<uint64_t> seqs;
        for (auto& e : HistoryList()) if (e.source == L"debloat" && !e.reverted) seqs.push_back(e.seq);
        if (seqs.empty()) { UiToast(L"Debloat hasn't changed anything"); return; }
        if (!UiConfirm(L"Put back the " + std::to_wstring(seqs.size()) + L" settings Debloat changed?")) return;
        UiBusy(L"Putting settings back\x2026", [seqs] { for (auto s : seqs) HistoryRevert(s); }, [] { UiToast(L"Debloat changes undone"); UiReloadPage(); });
    }});
    rows.push_back(std::move(top));
    for (const wchar_t* group : {L"Ads and suggestions", L"Search", L"Tracking and telemetry", L"AI and background features"}) {
        for (int i = 0; i < kDefCount; ++i) {
            const Def& d = kDefs[i];
            if (!(d.flags & F_DEBLOAT) || wcscmp(DebloatGroup(d), group) != 0) continue;
            Row r = RowFromDef(d);
            r.group = group;
            rows.push_back(std::move(r));
        }
    }
}

// Preinstalled Store apps people commonly remove. rec = removal is recommended (pure promotion).
static const struct { const wchar_t* name; const wchar_t* label; const wchar_t* what; bool rec; } kBloat[] = {
    {L"king.com.CandyCrushSaga", L"Candy Crush Saga", L"Promoted game.", true},
    {L"king.com.CandyCrushSodaSaga", L"Candy Crush Soda Saga", L"Promoted game.", true},
    {L"BytedancePte.Ltd.TikTok", L"TikTok", L"Promoted app.", true},
    {L"Disney.37853FC22B2CE", L"Disney+", L"Promoted app.", true},
    {L"SpotifyAB.SpotifyMusic", L"Spotify", L"Promoted app. Keep it if you use it.", false},
    {L"AmazonVideo.PrimeVideo", L"Prime Video", L"Promoted app.", true},
    {L"7EE7776C.LinkedInforWindows", L"LinkedIn", L"Promoted app.", true},
    {L"FACEBOOK.FACEBOOK", L"Facebook", L"Promoted app.", true},
    {L"Facebook.InstagramBeta", L"Instagram", L"Promoted app.", true},
    {L"Microsoft.BingNews", L"News", L"Microsoft's news feed app.", true},
    {L"Microsoft.BingWeather", L"Weather", L"Microsoft's weather app.", false},
    {L"Microsoft.BingSearch", L"Bing Search", L"Adds Bing to Windows search.", true},
    {L"Microsoft.MicrosoftSolitaireCollection", L"Solitaire & Casual Games", L"Card games with ads.", true},
    {L"Microsoft.Getstarted", L"Tips", L"Tutorials for Windows.", true},
    {L"Microsoft.GetHelp", L"Get Help", L"Microsoft's support chat app.", false},
    {L"Microsoft.WindowsFeedbackHub", L"Feedback Hub", L"Send feedback to Microsoft.", true},
    {L"Microsoft.MicrosoftOfficeHub", L"Microsoft 365 (Office) hub", L"A launcher that promotes Microsoft 365. Office itself is separate.", true},
    {L"Clipchamp.Clipchamp", L"Clipchamp", L"Video editor.", false},
    {L"Microsoft.Todos", L"Microsoft To Do", L"To-do lists.", false},
    {L"Microsoft.PowerAutomateDesktop", L"Power Automate", L"Automation tool.", false},
    {L"Microsoft.People", L"People", L"Contacts app.", true},
    {L"Microsoft.WindowsMaps", L"Maps", L"Microsoft's maps app.", true},
    {L"Microsoft.ZuneVideo", L"Movies & TV", L"Microsoft's video store and player.", false},
    {L"Microsoft.549981C3F5F10", L"Cortana", L"Retired voice assistant.", true},
    {L"Microsoft.MixedReality.Portal", L"Mixed Reality Portal", L"For Windows Mixed Reality headsets.", true},
    {L"Microsoft.Microsoft3DViewer", L"3D Viewer", L"Views 3D models.", true},
    {L"Microsoft.SkypeApp", L"Skype", L"Retired by Microsoft.", true},
    {L"MicrosoftTeams", L"Microsoft Teams (personal)", L"The consumer chat app, not Teams for work.", false},
    {L"MSTeams", L"Microsoft Teams", L"Remove only if you don't use Teams.", false},
    {L"Microsoft.OutlookForWindows", L"Outlook (new)", L"The new Outlook app Windows installs by itself.", false},
    {L"Microsoft.Copilot", L"Copilot app", L"The standalone Copilot app.", false},
    {L"Microsoft.Windows.DevHome", L"Dev Home", L"Dashboard for developers.", false},
    {L"Microsoft.YourPhone", L"Phone Link", L"Connects your phone to the PC.", false},
    {L"Microsoft.GamingApp", L"Xbox app", L"Game Pass and the Xbox store. Keep it if you play Game Pass games.", false},
};

void BuildDebloatApps(std::vector<Row>& rows) {
    std::set<std::wstring> installed;
    if (!InstalledAppx(installed, 30000)) return;
    for (auto& b : kBloat) {
        if (!installed.count(ToLower(b.name))) continue;
        Row r;
        r.id = std::wstring(L"appx:") + b.name;
        r.group = L"Preinstalled apps";
        r.title = b.label;
        r.label = b.label;
        r.desc = std::wstring(b.what) + L" Turn off to remove it for your account.";
        r.detail = std::wstring(L"Runs Remove-AppxPackage for ") + b.name + L". Undo re-registers it from the copy Windows keeps; if Windows has already deleted that copy, reinstall the app from the Microsoft Store.";
        r.ctl = CtlToggle;
        r.value = L"1";
        r.rec = b.rec ? L"0" : L"";
        rows.push_back(std::move(r));
    }
    if (rows.empty()) rows.push_back(Info(L"Preinstalled apps", L"No preinstalled junk found", L"None of the commonly removed Store apps are installed for your account."));
}

// ============================================================ Profiles
static std::wstring TriggerText(const std::wstring& t) {
    if (t == L"game") return L"Switches on while a full-screen game is running";
    if (t == L"battery") return L"Switches on while running on battery";
    if (StartsWith(t, L"exe:")) return L"Switches on while " + ReplaceAll(t.substr(4), L";", L", ") + L" is running";
    if (StartsWith(t, L"time:")) return L"Switches on between " + ReplaceAll(t.substr(5), L"-", L" and ");
    return L"Applied when you choose it";
}

void ProfileApply(const std::wstring& name) {
    Profile* p = FindProfile(name);
    if (!p) return;
    std::vector<Change> changes;
    for (auto& kv : p->values) changes.push_back({kv.first, kv.second, L""});
    g_config.activeProfile = name;
    ConfigSave();
    if (changes.empty()) { UiToast(name + L" has no settings yet. Add some from any setting's info panel."); UiReloadPage(); return; }
    UiApply(changes, L"profile " + name);
}

void ProfileAddSetting(const std::wstring& profile, const std::wstring& id, const std::wstring& value) {
    Profile* p = FindProfile(profile);
    if (!p) return;
    bool found = false;
    for (auto& kv : p->values) if (kv.first == id) { kv.second = value; found = true; }
    if (!found) p->values.push_back({id, value});
    ConfigSave();
    UiToast(NameOf(id) + L" (" + DescribeValue(id, value) + L") added to " + profile);
}

void BuildProfiles(std::vector<Row>& rows) {
    Row add = Info(L"Profiles", L"New profile", L"Create a profile, then add settings to it from any setting's info panel (the \x24D8 button > Add to profile).");
    add.actions.push_back({L"New profile", [] {
        UiPrompt(L"New profile", {L"Name"}, {L""}, [](const std::vector<std::wstring>& v) {
            std::wstring name = Trim(v[0]);
            if (name.empty() || FindProfile(name)) return;
            g_config.profiles.push_back({name, L"", {}});
            ConfigSave();
            UiReloadPage();
        });
    }});
    rows.push_back(std::move(add));
    for (auto& p : g_config.profiles) {
        std::wstring name = p.name;
        Row head = Info(L"", name, std::to_wstring(p.values.size()) + (p.values.size() == 1 ? L" setting.  " : L" settings.  ") + TriggerText(p.trigger),
                        name == g_config.activeProfile ? L"Active" : L"");
        head.group = name;
        head.actions.push_back({L"Apply", [name] { ProfileApply(name); }});
        head.actions.push_back({L"Use current values", [name] {
            Profile* pr = FindProfile(name);
            if (!pr) return;
            std::vector<std::wstring> ids;
            for (auto& kv : pr->values) ids.push_back(kv.first);
            auto vals = std::make_shared<std::vector<std::wstring>>(ids.size());
            UiBusy(L"", [ids, vals] { for (size_t i = 0; i < ids.size(); ++i) ReadValue(ids[i], (*vals)[i]); }, [name, ids, vals] {
                Profile* q = FindProfile(name);
                if (!q) return;
                for (size_t i = 0; i < ids.size(); ++i)
                    for (auto& kv : q->values) if (kv.first == ids[i] && !(*vals)[i].empty()) kv.second = (*vals)[i];
                ConfigSave();
                UiToast(name + L" now matches how this PC is set");
                UiReloadPage();
            });
        }});
        head.actions.push_back({L"Delete", [name] {
            if (!UiConfirm(L"Delete the profile \"" + name + L"\"?\n\nYour Windows settings stay as they are.")) return;
            auto& ps = g_config.profiles;
            ps.erase(std::remove_if(ps.begin(), ps.end(), [&](const Profile& x) { return x.name == name; }), ps.end());
            if (g_config.activeProfile == name) g_config.activeProfile.clear();
            ConfigSave();
            UiReloadPage();
        }});
        rows.push_back(std::move(head));

        Row trig;
        trig.id = L"app.profileTrigger:" + name;
        trig.group = name;
        trig.title = L"Switch automatically";
        trig.desc = L"Breaker applies this profile when the condition starts and puts your settings back when it ends. Needs \"Switch profiles automatically\" in Breaker settings.";
        trig.ctl = CtlChoice;
        trig.choices = {{L"Never (manual only)", L""}, {L"While a full-screen game runs", L"game"}, {L"While on battery", L"battery"}, {L"While certain apps run\x2026", L"?exe:"}, {L"During certain hours\x2026", L"?time:"}};
        trig.value = StartsWith(p.trigger, L"exe:") ? L"?exe:" : StartsWith(p.trigger, L"time:") ? L"?time:" : p.trigger;
        trig.flags = F_NOHIST;
        rows.push_back(std::move(trig));

        for (auto& kv : p.values) {
            std::wstring id = kv.first;
            Row r = Info(L"", NameOf(id), L"", DescribeValue(id, kv.second));
            r.group = name;
            if (const Def* d = FindDef(id)) r.desc = d->desc ? d->desc : L"";
            r.actions.push_back({L"Remove", [name, id] {
                Profile* q = FindProfile(name);
                if (!q) return;
                q->values.erase(std::remove_if(q->values.begin(), q->values.end(), [&](auto& x) { return x.first == id; }), q->values.end());
                ConfigSave();
                UiReloadPage();
            }});
            rows.push_back(std::move(r));
        }
    }
}

// ---- automatic switching
static bool ProcessRunning(const std::vector<std::wstring>& names) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        for (auto& n : names) if (_wcsicmp(pe.szExeFile, n.c_str()) == 0) found = true;
    CloseHandle(snap);
    return found;
}

static bool FullScreenGame() {
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN) return true;
    HWND fg = GetForegroundWindow();
    if (!fg || fg == UiWindow() || fg == GetDesktopWindow() || fg == GetShellWindow()) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT wr;
    MONITORINFO mi{sizeof(mi)};
    return GetWindowRect(fg, &wr) && GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi) && wr.left <= mi.rcMonitor.left &&
           wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

static bool TriggerActive(const std::wstring& t) {
    if (t == L"game") return FullScreenGame();
    if (t == L"battery") {
        SYSTEM_POWER_STATUS s{};
        return GetSystemPowerStatus(&s) && s.ACLineStatus == 0;
    }
    if (StartsWith(t, L"exe:")) return ProcessRunning(Split(t.substr(4), L";"));
    if (StartsWith(t, L"time:")) {           // "time:22:00-07:00", which may wrap past midnight
        int h1, m1, h2, m2;
        if (swscanf(t.c_str() + 5, L"%d:%d-%d:%d", &h1, &m1, &h2, &m2) != 4) return false;
        SYSTEMTIME st;
        GetLocalTime(&st);
        int now = st.wHour * 60 + st.wMinute, a = h1 * 60 + m1, b = h2 * 60 + m2;
        return a <= b ? (now >= a && now < b) : (now >= a || now < b);
    }
    return false;
}

static std::wstring g_autoProfile;                       // profile applied by a trigger, "" when none
static std::vector<Change> g_autoRestore;                // what to put back when the trigger ends
static std::atomic<bool> g_autoBusy{false};

void ProfilesTick() {
    if (!g_config.autoProfiles || g_autoBusy) return;
    std::wstring want;
    for (auto& p : g_config.profiles) if (!p.trigger.empty() && TriggerActive(p.trigger)) { want = p.name; break; }
    if (want == g_autoProfile) return;
    g_autoBusy = true;
    std::vector<Change> restore = g_autoRestore, apply;
    if (Profile* p = FindProfile(want)) for (auto& kv : p->values) apply.push_back({kv.first, kv.second, L""});
    std::wstring previous = g_autoProfile;
    RunAsync([want, previous, restore, apply] {
        if (!previous.empty() && !restore.empty()) ApplyChanges(restore, L"profile " + previous + L" ended");
        std::vector<Change> saved;
        for (auto& c : apply) {
            std::wstring cur;
            if (ReadValue(c.id, cur) && cur != c.value) saved.push_back({c.id, cur, L""});
        }
        if (!apply.empty()) ApplyChanges(apply, L"profile " + want + L" (automatic)");
        Ui([want, saved] {
            g_autoProfile = want;
            g_autoRestore = saved;
            g_config.activeProfile = want;
            ConfigSave();
            g_autoBusy = false;
            UiToast(want.empty() ? L"Profile ended; your settings are back" : L"Switched to the " + want + L" profile");
            UiReloadPage();
        });
    });
}

// ============================================================ Undo history
static double DayStart(int daysAgo) {
    time_t now = time(nullptr);
    struct tm t{};
    localtime_s(&t, &now);
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    return (double)mktime(&t) - daysAgo * 86400.0;
}

static void RestoreTo(double when, const std::wstring& label) {
    int n = 0;
    for (auto& e : HistoryList()) if (e.time >= when && !e.reverted) ++n;
    if (!n) { UiToast(L"Nothing has changed since " + label); return; }
    if (!UiConfirm(L"Put back everything changed since " + label + L"?\n\n" + std::to_wstring(n) + (n == 1 ? L" change" : L" changes") + L" will be undone.")) return;
    auto rep = std::make_shared<ApplyReport>();
    UiBusy(L"Restoring settings\x2026", [when, rep] { *rep = HistoryRestoreTo(when); }, [rep, label] {
        UiToast(rep->ok ? L"Settings restored to " + label + (rep->failed ? L" (" + std::to_wstring(rep->failed) + L" couldn't be restored)" : L"") : (rep->error.empty() ? L"Nothing was restored" : rep->error));
        UiReloadPage();
    });
}

void BuildHistory(std::vector<Row>& rows) {
    Row top = Info(L"Restore", L"Restore everything to how it was", L"Undoes every change Breaker made since the moment you pick.");
    top.actions.push_back({L"This morning", [] { RestoreTo(DayStart(0), L"this morning"); }});
    top.actions.push_back({L"Yesterday", [] { RestoreTo(DayStart(1), L"yesterday morning"); }});
    top.actions.push_back({L"A week ago", [] { RestoreTo(DayStart(7), L"a week ago"); }});
    top.actions.push_back({L"Pick a date", [] {
        UiPrompt(L"Restore to the start of which day?", {L"Date (YYYY-MM-DD)"}, {L""}, [](const std::vector<std::wstring>& v) {
            struct tm t{};
            if (swscanf(v[0].c_str(), L"%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) { UiToast(L"That isn't a date like 2026-11-09"); return; }
            t.tm_year -= 1900;
            t.tm_mon -= 1;
            t.tm_isdst = -1;
            RestoreTo((double)mktime(&t), v[0]);
        });
    }});
    rows.push_back(std::move(top));
    auto hist = HistoryList();
    if (hist.empty()) { rows.push_back(Info(L"Changes", L"No changes yet", L"When you change a setting in Breaker it appears here.")); return; }
    double today = DayStart(0);
    int shown = 0;
    for (auto& e : hist) {
        if (++shown > 400) break;
        std::wstring day = e.time >= today ? L"Today" : e.time >= today - 86400 ? L"Yesterday" : FormatWhen(e.time).substr(0, FormatWhen(e.time).rfind(L','));
        std::wstring by = e.source == L"user" ? L"" : L"   \x00B7   " + e.source;
        Row r = Info(L"", e.name, DescribeValue(e.id, e.was) + L"  \x2192  " + DescribeValue(e.id, e.after) + L"   \x00B7   " + FormatWhen(e.time) + by, e.reverted ? L"Undone" : L"");
        r.group = day;
        uint64_t seq = e.seq;
        if (!e.reverted) {
            auto rep = std::make_shared<ApplyReport>();
            r.actions.push_back({L"Undo", [seq, rep] {
                UiBusy(L"", [seq, rep] { *rep = HistoryRevert(seq); }, [rep] { UiToast(rep->ok ? L"Change undone" : rep->error); UiReloadPage(); });
            }});
        }
        rows.push_back(std::move(r));
    }
    Row clear = Info(L"Housekeeping", L"Clear the history", L"Forgets the list. Your settings stay as they are, but these changes can no longer be undone.");
    clear.actions.push_back({L"Clear", [] { if (UiConfirm(L"Clear the undo history?\n\nYou won't be able to undo these changes afterwards.")) { HistoryClear(); UiReloadPage(); } }});
    rows.push_back(std::move(clear));
}

// ============================================================ Update watchdog
static std::vector<Drift> g_drift;
static std::wstring g_buildNote;

std::vector<Drift> WatchdogCompute() {
    std::vector<Drift> out;
    auto desired = DesiredList();
    bool anyAppx = false;
    for (auto& kv : desired) if (StartsWith(kv.first, L"appx:")) anyAppx = true;
    std::set<std::wstring> appx;
    bool appxOk = anyAppx && InstalledAppx(appx, 30000);
    for (auto& kv : desired) {
        std::wstring now;
        if (StartsWith(kv.first, L"appx:")) {
            if (!appxOk) continue;
            now = appx.count(ToLower(kv.first.substr(5))) ? L"1" : L"0";
        } else if (!ReadValue(kv.first, now)) {
            continue;
        }
        if (now == kv.second) continue;
        if (StartsWith(kv.first, L"assoc:") && now.empty()) continue;
        out.push_back({kv.first, StartsWith(kv.first, L"assoc:") ? L"Default app for " + kv.first.substr(6) : NameOf(kv.first), kv.second, now});
    }
    return out;
}

const std::vector<Drift>& WatchdogDrift() { return g_drift; }
std::wstring WatchdogBuildNote() { return g_buildNote; }

void WatchdogRefresh(bool toastWhenClean) {
    RunAsync([toastWhenClean] {
        auto drift = std::make_shared<std::vector<Drift>>(WatchdogCompute());
        std::wstring build = WindowsBuildText();
        Ui([drift, build, toastWhenClean] {
            g_drift = *drift;
            if (!g_config.lastBuild.empty() && g_config.lastBuild != build && !g_drift.empty())
                g_buildNote = L"Windows updated from build " + g_config.lastBuild + L" to " + build;
            if (g_drift.empty()) g_buildNote.clear();
            if (g_config.lastBuild != build && g_drift.empty()) { g_config.lastBuild = build; ConfigSave(); }
            if (toastWhenClean) UiToast(g_drift.empty() ? L"All your settings are still how you left them" : g_drift.size() == 1 ? L"One setting was changed outside Breaker" : std::to_wstring(g_drift.size()) + L" settings were changed outside Breaker");
            std::wstring route = UiRoute();
            if (route == L"home" || route == L"watchdog") UiReloadPage();
        });
    });
}

static void DriftResolved() {
    g_config.lastBuild = WindowsBuildText();
    ConfigSave();
    WatchdogRefresh(false);
}

void BuildWatchdog(std::vector<Row>& rows) {
    Row check = Info(L"Watchdog", g_drift.empty() ? L"Everything is how you left it" : g_drift.size() == 1 ? L"One setting was changed outside Breaker" : std::to_wstring(g_drift.size()) + L" settings were changed outside Breaker",
                     g_buildNote.empty() ? L"Breaker compares the settings you changed here with what Windows has now. It checks when Breaker starts, and at sign-in if you turn that on." : g_buildNote + L".");
    check.alert = !g_drift.empty();
    if (g_drift.empty()) check.actions.push_back({L"Check now", [] { WatchdogRefresh(true); }});
    if (!g_drift.empty()) {
        check.actions.push_back({L"Restore all", [] {
            std::vector<Change> changes;
            for (auto& d : g_drift) if (!StartsWith(d.id, L"assoc:")) changes.push_back({d.id, d.wanted, d.name});
            UiApply(changes, L"watchdog");
        }});
        check.actions.push_back({L"Keep Windows' values", [] {
            for (auto& d : g_drift) DesiredSet(d.id, d.now);
            DriftResolved();
        }});
        check.actions.push_back({L"Check again", [] { WatchdogRefresh(true); }});
    }
    rows.push_back(std::move(check));
    for (auto& d : g_drift) {
        bool assoc = StartsWith(d.id, L"assoc:");
        Row r = Info(L"Changed outside Breaker", d.name, L"You chose " + DescribeValue(d.id, d.wanted) + L". It's now " + DescribeValue(d.id, d.now) + L".");
        r.alert = true;
        Drift copy = d;
        if (assoc) r.actions.push_back({L"Fix in Windows", [] { OpenTarget(L"ms-settings:defaultapps"); }});
        else r.actions.push_back({L"Restore", [copy] { UiApply({{copy.id, copy.wanted, copy.name}}, L"watchdog"); }});
        r.actions.push_back({L"Keep", [copy] { DesiredSet(copy.id, copy.now); DriftResolved(); }});
        r.actions.push_back({L"Stop watching", [copy] { DesiredForget(copy.id); DriftResolved(); }});
        rows.push_back(std::move(r));
    }
    for (const wchar_t* id : {L"app.watchdogEnabled", L"app.watchdogAtSignIn"}) {
        Row r;
        r.id = id;
        r.group = L"When to check";
        r.ctl = CtlToggle;
        r.flags = F_NOHIST;
        r.known = false;
        bool signIn = wcscmp(id, L"app.watchdogAtSignIn") == 0;
        r.title = signIn ? L"Also check when I sign in to Windows" : L"Check when Breaker starts";
        r.desc = signIn ? L"A quick check right after sign-in. Breaker only stays open if something changed; otherwise it exits straight away."
                        : L"Takes a moment in the background and never slows the window down.";
        rows.push_back(std::move(r));
    }
    auto desired = DesiredList();
    int shown = 0;
    for (auto& kv : desired) {
        if (++shown > 250) break;
        std::wstring id = kv.first;
        Row r = Info(L"Watching", StartsWith(id, L"assoc:") ? L"Default app for " + id.substr(6) : NameOf(id), L"", DescribeValue(id, kv.second));
        r.actions.push_back({L"Stop watching", [id] { DesiredForget(id); UiReloadPage(); }});
        rows.push_back(std::move(r));
    }
    if (desired.empty()) rows.push_back(Info(L"Watching", L"Nothing yet", L"Settings you change in Breaker are watched automatically from then on."));
}

// ============================================================ Snapshots
static bool SnapshotWorthy(const std::wstring& id) {
    unsigned f = FlagsOf(id);
    return !(f & (F_NOSNAP | F_NOHIST));
}

bool SnapshotWrite(const std::wstring& path, std::wstring* err) {
    json::Value settings = json::Value::MakeObject();
    int n = 0;
    for (int i = 0; i < kDefCount; ++i) {
        const Def& d = kDefs[i];
        if (d.ctl == CtlLink || d.ctl == CtlInfo || !SnapshotWorthy(d.id)) continue;
        std::wstring v;
        if (ReadValue(d.id, v) && !v.empty()) { settings.obj.emplace_back(d.id, json::Value::Str(v)); ++n; }
    }
    // Things found on this PC that the user changed here: services, startup apps, variables, removed apps
    for (auto& kv : DesiredList()) {
        if (FindDef(kv.first) || !SnapshotWorthy(kv.first)) continue;
        if (!StartsWith(kv.first, L"svc:") && !StartsWith(kv.first, L"env:u:") && !StartsWith(kv.first, L"startup:") && !StartsWith(kv.first, L"appx:")) continue;
        std::wstring v;
        if (ReadValue(kv.first, v)) settings.obj.emplace_back(kv.first, json::Value::Str(v));
    }
    wchar_t pc[256] = {};
    DWORD len = (DWORD)std::size(pc);
    GetComputerNameW(pc, &len);
    json::Value root = json::Value::MakeObject();
    root.Set(L"breakerSnapshot", json::Value::Num(1));
    root.Set(L"breaker", json::Value::Str(BREAKER_VERSION));
    root.Set(L"created", json::Value::Num(NowUnix()));
    root.Set(L"pc", json::Value::Str(pc));
    root.Set(L"windowsBuild", json::Value::Str(WindowsBuildText()));
    root.Set(L"settings", settings);
    std::wstring cfgText;
    json::Value cfg;
    if (ReadUtf8File(ConfigPath(), cfgText)) {
        json::Parser p(cfgText);
        if (p.Parse(cfg)) root.Set(L"breakerConfig", cfg);
    }
    std::wstring text;
    json::Write(root, text);
    if (!WriteUtf8FileAtomic(path, text)) { if (err) *err = L"Couldn't write the file: " + ErrorText(GetLastError()); return false; }
    Log(L"Snapshot saved: %d settings -> %ls", n, path.c_str());
    return true;
}

static void SnapshotImport(const std::wstring& path) {
    std::wstring text;
    auto root = std::make_shared<json::Value>();
    if (!ReadUtf8File(path, text) || !json::Parser(text).Parse(*root) || root->GetNum(L"breakerSnapshot", 0) < 1 || !root->Get(L"settings")) {
        UiToast(L"That isn't a Breaker snapshot file");
        return;
    }
    auto changes = std::make_shared<std::vector<Change>>();
    UiBusy(L"Comparing the snapshot with this PC\x2026", [root, changes] {
        for (auto& kv : root->Get(L"settings")->obj) {
            if (kv.second.type != json::Value::String) continue;
            if (!FindDef(kv.first) && !FindHandler(kv.first)) continue;      // from a newer Breaker
            std::wstring cur;
            if (!ReadValue(kv.first, cur) || cur == kv.second.s) continue;    // not on this PC, or already the same
            changes->push_back({kv.first, kv.second.s, L""});
        }
    }, [root, changes, path] {
        std::wstring from = root->GetStr(L"pc");
        if (changes->empty()) {
            UiToast(L"This PC already matches the snapshot");
        } else {
            int admin = 0;
            for (auto& c : *changes) if (FlagsOf(c.id) & F_ADMIN) ++admin;
            std::wstring msg = L"Apply this snapshot" + (from.empty() ? L"" : L" from " + from) + L"?\n\n" + std::to_wstring(changes->size()) + L" settings on this PC are different and will be changed";
            if (admin) msg += L" (" + std::to_wstring(admin) + L" need administrator rights, asked once)";
            msg += L".\nEverything is recorded in the undo history.";
            if (UiConfirm(msg)) {
                std::vector<Change> batch = *changes;
                PrependRestorePoint(batch, L"Before applying a Breaker snapshot");
                UiApply(batch, L"snapshot");
            }
        }
        if (const json::Value* cfg = root->Get(L"breakerConfig"); cfg && cfg->type == json::Value::Object &&
            UiConfirm(L"Also use this snapshot's Breaker setup?\n\nThat's its theme, pinned settings, sidebar layout and profiles.")) {
            json::Value copy = *cfg;
            for (const wchar_t* k : {L"winX", L"winY", L"winW", L"winH", L"maximized", L"lastBuild", L"lastCheck", L"startWithWindows", L"winI", L"keepInTray", L"watchdogAtSignIn"})
                copy.obj.erase(std::remove_if(copy.obj.begin(), copy.obj.end(), [&](auto& kv) { return kv.first == k; }), copy.obj.end());
            json::Value current;
            std::wstring curText, outText;
            if (ReadUtf8File(ConfigPath(), curText) && json::Parser(curText).Parse(current) && current.type == json::Value::Object) {
                for (auto& kv : copy.obj) current.Set(kv.first, kv.second);
                json::Write(current, outText);
                WriteUtf8FileAtomic(ConfigPath(), outText);
                ConfigLoad();
                UiThemeChanged();
                UiReloadPage();
            }
        }
    });
}

void BuildSnapshots(std::vector<Row>& rows) {
    Row save = Info(L"Snapshots", L"Save a snapshot", L"Writes every setting Breaker manages, plus your theme, pins and profiles, to one JSON file. Keep it somewhere safe, like a USB stick or cloud folder.");
    save.actions.push_back({L"Save snapshot", [] {
        std::wstring path;
        if (!UiPickFile(L"Breaker snapshot (*.json)\0*.json\0", path, true, L"Breaker snapshot.json", L"json")) return;
        auto ok = std::make_shared<bool>(false);
        auto err = std::make_shared<std::wstring>();
        UiBusy(L"Reading every setting\x2026", [path, ok, err] { *ok = SnapshotWrite(path, err.get()); }, [ok, err] { UiToast(*ok ? L"Snapshot saved" : *err); });
    }});
    rows.push_back(std::move(save));
    Row apply = Info(L"Snapshots", L"Apply a snapshot", L"On a fresh Windows install or another PC: install Breaker, then pick the file. Breaker shows how many settings differ and asks before changing anything. Settings that only made sense on the old PC (resolution, sound devices, wallpaper path) are left out.");
    apply.actions.push_back({L"Apply snapshot", [] {
        std::wstring path;
        if (UiPickFile(L"Breaker snapshot (*.json)\0*.json\0All files\0*.*\0", path)) SnapshotImport(path);
    }});
    rows.push_back(std::move(apply));
}

// ============================================================ restore points
void PrependRestorePoint(std::vector<Change>& changes, const std::wstring& why) {
    if (!g_config.restorePointFirst || changes.size() < 3) return;
    changes.insert(changes.begin(), Change{L"sys.restorePoint", why, L"Restore point"});
}

// ============================================================ PC Checkup
static std::vector<std::wstring> g_checkup;      // window thread
static int g_checkupCount = -1;
static std::atomic<bool> g_checkupBusy{false};

static bool CheckupCandidate(const Def& d) {
    return d.rec && *d.rec && (d.ctl == CtlToggle || d.ctl == CtlChoice) && wcscmp(d.page, L"~") != 0;
}

static std::vector<std::wstring> CheckupCompute() {   // blocking
    std::vector<std::wstring> out;
    for (int i = 0; i < kDefCount; ++i) {
        const Def& d = kDefs[i];
        if (!CheckupCandidate(d)) continue;
        std::wstring v;
        if (!ReadValue(d.id, v) || v.empty()) continue;
        CacheStore(d.id, v);
        if (v != d.rec) out.push_back(d.id);
    }
    return out;
}

int CheckupCount() { return g_checkupCount; }

void CheckupRefresh() {
    if (g_checkupBusy.exchange(true)) return;
    RunAsync([] {
        auto ids = std::make_shared<std::vector<std::wstring>>(CheckupCompute());
        Ui([ids] {
            bool changed = (int)ids->size() != g_checkupCount;
            g_checkup = *ids;
            g_checkupCount = (int)ids->size();
            g_checkupBusy = false;
            if (changed && UiRoute() == L"home") UiReloadPage();
        });
    });
}

static std::wstring CategoryName(const std::wstring& page) {
    const Category* c = FindCategory(CategoryOf(page));
    return c ? c->name : L"Other";
}

void BuildCheckup(std::vector<Row>& rows) {
    std::vector<std::wstring> ids = CheckupCompute();
    Ui([ids] { g_checkup = ids; g_checkupCount = (int)ids.size(); });
    std::vector<Change> all;
    int admin = 0;
    for (auto& id : ids) {
        all.push_back({id, FindDef(id)->rec, L""});
        if (FlagsOf(id) & F_ADMIN) ++admin;
    }
    Row top = Info(L"Checkup", ids.empty() ? L"Everything Breaker recommends is already set" : ids.size() == 1 ? L"1 recommendation isn't applied yet" : std::to_wstring(ids.size()) + L" recommendations aren't applied yet",
                   ids.empty() ? L"Nice. Breaker checks again every time it starts, so you'll see it on Home if Windows changes something."
                               : L"Every setting across Breaker where its recommended value differs from yours, in one list. Change them one at a time, or apply them all at once. Everything can be undone.");
    top.alert = false;
    if (!all.empty())
        top.actions.push_back({L"Apply all", [all, admin] {
            std::wstring msg = L"Apply all " + std::to_wstring(all.size()) + L" recommendations?";
            if (admin) msg += L"\n\n" + std::to_wstring(admin) + L" need administrator rights; Windows asks once.";
            if (g_config.restorePointFirst) msg += L"\nA System Restore point is made first.";
            msg += L"\nEach change is recorded in the undo history.";
            if (!UiConfirm(msg)) return;
            std::vector<Change> batch = all;
            PrependRestorePoint(batch, L"Before Breaker Checkup");
            UiApply(batch, L"checkup");
        }});
    rows.push_back(std::move(top));
    // grouped by category, in sidebar order
    std::vector<std::pair<std::wstring, Row>> list;
    for (auto& id : ids) {
        const Def* d = FindDef(id);
        Row r = RowFromDef(*d);
        r.group = CategoryName(d->page);
        std::wstring v;
        if (CachedValue(id, v)) { r.value = v; r.known = true; }
        const PageInfo* p = FindPage(d->page);
        r.where = std::wstring(r.group) + (p && r.group != p->title ? L"  \x203A  " + std::wstring(p->title) : L"");
        list.push_back({d->page, std::move(r)});
    }
    auto order = [](const std::wstring& page) {
        std::wstring cat = CategoryOf(page);
        for (int i = 0; i < kCategoryCount; ++i) if (cat == kCategories[i].id) return i;
        return kCategoryCount;
    };
    std::stable_sort(list.begin(), list.end(), [&](auto& a, auto& b) { return order(a.first) < order(b.first); });
    for (auto& kv : list) rows.push_back(std::move(kv.second));
}
