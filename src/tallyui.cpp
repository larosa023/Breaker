// Breaker - Screen time pages: Today, This week, This month, one page per app, and the settings tab.
// The numbers come from the files the tracker writes (tally.cpp); pages ask it to save first, so they're current.
#include "tally.h"
#include "pages.h"
#include "config.h"
#include <algorithm>
#include <mutex>

using namespace tally;

namespace {
struct Data {
    DayMap days;
    std::map<std::wstring, AppInfo> apps;
    bool running = false;
};
struct Totals {
    long long total = 0, gaming = 0;
    std::map<std::wstring, long long> perApp, perCat;
    std::map<int, long long> perDay;
};
}  // namespace

static std::mutex g_namesLock;
static std::map<std::wstring, std::wstring> g_names;   // app key -> name, for page titles

static Data Load(int from, int to) {
    Data d;
    d.running = FlushTracker();
    d.apps = LoadApps();
    LoadRange(from, to, d.days);
    std::lock_guard<std::mutex> lock(g_namesLock);
    for (auto& kv : d.apps) g_names[kv.first] = kv.second.name;
    return d;
}

std::wstring TallyAppTitle(const std::wstring& key) {
    std::lock_guard<std::mutex> lock(g_namesLock);
    if (g_names.empty())   // nothing loaded yet this session: read the small apps list once
        for (auto& kv : LoadApps()) g_names[kv.first] = kv.second.name.empty() ? L" " : kv.second.name;
    auto it = g_names.find(key);
    return it != g_names.end() && !it->second.empty() ? it->second : tally::NameOf(key, {});
}

static Totals Sum(const Data& d, int from, int to) {
    Totals t;
    for (auto it = d.days.lower_bound(from); it != d.days.end() && it->first <= to; ++it)
        for (auto& kv : it->second) {
            std::wstring cat = CategoryOf(kv.first, d.apps);
            t.total += kv.second;
            t.perApp[kv.first] += kv.second;
            t.perCat[cat] += kv.second;
            t.perDay[it->first] += kv.second;
            if (cat == L"Games") t.gaming += kv.second;
        }
    return t;
}

// ============================================================ row helpers
static Row Info(const wchar_t* group, const std::wstring& title, const std::wstring& desc, const std::wstring& valueText = L"") {
    Row r;
    r.group = group;
    r.title = title;
    r.desc = desc;
    r.valueText = valueText;
    return r;
}

static Row Stat(const wchar_t* group, const std::wstring& title, const std::wstring& value, const std::wstring& desc) {
    Row r = Info(group, title, desc, value);
    r.tile = r.stat = true;
    return r;
}

static std::wstring Delta(long long now, long long before, const wchar_t* than) {
    if (before <= 0) return L"";
    long long d = now - before;
    if (std::llabs(d) < 60) return std::wstring(L"Same as ") + than;
    return (d > 0 ? L"\x25B2 " : L"\x25BC ") + Dur(std::llabs(d)) + (d > 0 ? L" more than " : L" less than ") + than;
}

static std::wstring TopName(const Totals& t, const Data& d, long long* secs) {
    if (t.perApp.empty()) { if (secs) *secs = 0; return L"\x2014"; }
    auto top = std::max_element(t.perApp.begin(), t.perApp.end(), [](auto& a, auto& b) { return a.second < b.second; });
    if (secs) *secs = top->second;
    return tally::NameOf(top->first, d.apps);
}

static Row DayChart(const wchar_t* group, const wchar_t* title, const Data& d, int from, int to, int today, bool shortLabels) {
    Row r;
    r.group = group;
    r.title = title;
    r.chartUnit = L"h";
    long long sum = 0;
    int counted = 0;
    for (int day = from; day <= to; day = AddDays(day, 1)) {
        long long s = 0;
        auto it = d.days.find(day);
        if (it != d.days.end()) for (auto& kv : it->second) s += kv.second;
        r.chart.push_back(s / 3600.0f);
        int dom = day % 100;
        r.chartLabels.push_back(shortLabels ? (dom == 1 || dom % 5 == 0 ? std::to_wstring(dom) : L"") : DayLabel(day, false));
        r.chartTips.push_back(DayLabel(day, true) + L": " + Dur(s));
        if (day == today) r.chartHi = (int)r.chart.size() - 1;
        if (day <= today) { sum += s; ++counted; }
    }
    if (counted) r.valueText = L"Daily average " + Dur(sum / counted);
    return r;
}

static void CategoryRows(std::vector<Row>& rows, const Totals& t) {
    if (t.total <= 0) return;
    std::vector<std::pair<long long, std::wstring>> cats;
    for (auto& kv : t.perCat) if (kv.second > 0) cats.push_back({kv.second, kv.first});
    std::sort(cats.rbegin(), cats.rend());
    for (auto& c : cats) {
        int pct = (int)(c.first * 100 / t.total);
        Row r = Info(L"By category", c.second, (pct < 1 ? std::wstring(L"Under 1%") : std::to_wstring(pct) + L"%") + L" of screen time", Dur(c.first));
        r.bar = (float)c.first / (float)cats.front().first;
        rows.push_back(std::move(r));
    }
}

static void AppRows(std::vector<Row>& rows, const wchar_t* group, const Totals& t, const Data& d) {
    std::vector<std::pair<long long, std::wstring>> list;
    for (auto& kv : t.perApp) if (kv.second >= 30) list.push_back({kv.second, kv.first});
    std::sort(list.rbegin(), list.rend());
    if (list.empty()) return;
    long long top = list.front().first;
    int shown = 0;
    for (auto& a : list) {
        if (++shown > 120) break;
        std::wstring cat = CategoryOf(a.second, d.apps), desc = cat;
        auto it = d.apps.find(a.second);
        if (it != d.apps.end() && it->second.game && cat != L"Games") desc += L"  \x00B7  game";
        if (int lim = LimitFor(a.second)) desc += L"  \x00B7  limit " + Dur(lim * 60LL) + L" a day";
        Row r = Info(group, tally::NameOf(a.second, d.apps), desc, Dur(a.first));
        r.ctl = CtlLink;
        r.target = L"breaker:screentime/app/" + a.second;
        r.bar = (float)a.first / (float)top;
        rows.push_back(std::move(r));
    }
}

static void StatusRows(std::vector<Row>& rows, const Data& d) {
    bool enabled = false;
    UiSync([&] { enabled = g_config.tallyEnabled; });
    if (!enabled) {
        Row r = Info(L"", L"Screen time is off", L"Turn it on and Breaker counts which app is in front every few seconds, in the background, using almost no CPU or memory. Everything stays on this PC.");
        r.alert = true;
        r.actions.push_back({L"Turn on", [] { UiApply({{L"tally.enabled", L"1", L"Screen time"}}); }});
        rows.push_back(std::move(r));
    } else if (!d.running) {
        Row r = Info(L"", L"The screen time tracker isn't running", L"It normally starts with Windows. Start it now to keep counting.");
        r.alert = true;
        r.actions.push_back({L"Start it", [] { UiBusy(L"", [] { EnsureTracker(true); Sleep(1500); }, [] { UiReloadPage(); }); }});
        rows.push_back(std::move(r));
    }
}

// ============================================================ pages
void BuildTallyToday(std::vector<Row>& rows) {
    int today = Today(), week = WeekStart(today);
    int from = std::min(week, AddDays(today, -1));
    Data d = Load(from, AddDays(week, 6));
    StatusRows(rows, d);
    Totals t = Sum(d, today, today), y = Sum(d, AddDays(today, -1), AddDays(today, -1));
    long long topSecs = 0;
    std::wstring top = TopName(t, d, &topSecs);
    rows.push_back(Stat(L"Today", L"Screen time", Dur(t.total), Delta(t.total, y.total, L"yesterday")));
    rows.push_back(Stat(L"Today", L"Gaming", Dur(t.gaming), t.total ? std::to_wstring(t.gaming * 100 / std::max(1LL, t.total)) + L"% of today" : L""));
    rows.push_back(Stat(L"Today", L"Most used", top, topSecs ? Dur(topSecs) : L"Nothing yet"));
    rows.push_back(DayChart(L"This week", L"Screen time per day", d, week, AddDays(week, 6), today, false));
    if (t.total < 30) {
        rows.push_back(Info(L"Apps today", L"Nothing counted yet today", L"Breaker notes the app in front every few seconds, skipping time when you're away, the PC is locked or asleep. Check back in a few minutes."));
        return;
    }
    CategoryRows(rows, t);
    AppRows(rows, L"Apps today", t, d);
}

void BuildTallyWeek(std::vector<Row>& rows) {
    int today = Today(), week = WeekStart(today), prev = AddDays(week, -7);
    Data d = Load(prev, AddDays(week, 6));
    StatusRows(rows, d);
    Totals t = Sum(d, week, AddDays(week, 6));
    // compare with the same part of last week
    Totals last = Sum(d, prev, AddDays(prev, DaysBetween(week, today)));
    long long topSecs = 0;
    std::wstring top = TopName(t, d, &topSecs);
    rows.push_back(Stat(L"This week", L"Screen time", Dur(t.total), Delta(t.total, last.total, L"this point last week")));
    rows.push_back(Stat(L"This week", L"Gaming", Dur(t.gaming), t.total ? std::to_wstring(t.gaming * 100 / std::max(1LL, t.total)) + L"% of the week" : L""));
    rows.push_back(Stat(L"This week", L"Most used", top, topSecs ? Dur(topSecs) : L"Nothing yet"));
    rows.push_back(DayChart(L"Per day", L"Screen time per day", d, week, AddDays(week, 6), today, false));
    CategoryRows(rows, t);
    AppRows(rows, L"Apps this week", t, d);
}

void BuildTallyMonth(std::vector<Row>& rows) {
    int today = Today(), from = MonthStart(today), to = MonthEnd(today);
    Data d = Load(from, to);
    StatusRows(rows, d);
    Totals t = Sum(d, from, to);
    int days = DaysBetween(from, today) + 1;
    long long topSecs = 0;
    std::wstring top = TopName(t, d, &topSecs);
    rows.push_back(Stat(L"This month", L"Screen time", Dur(t.total), L"Daily average " + Dur(t.total / std::max(1, days))));
    rows.push_back(Stat(L"This month", L"Gaming", Dur(t.gaming), L"Daily average " + Dur(t.gaming / std::max(1, days))));
    rows.push_back(Stat(L"This month", L"Most used", top, topSecs ? Dur(topSecs) : L"Nothing yet"));
    rows.push_back(DayChart(L"Per day", L"Screen time per day", d, from, to, today, true));
    CategoryRows(rows, t);
    AppRows(rows, L"Apps this month", t, d);
    Row exp = Info(L"Your data", L"Export, limits and privacy", L"Export to CSV, set daily limits, choose apps to never track and clear history in Breaker settings.");
    exp.ctl = CtlLink;
    exp.target = L"breaker:settings/screentime";
    rows.push_back(std::move(exp));
}

static Choices LimitChoices() {
    return {{L"No limit", L"0"}, {L"15 minutes", L"15"}, {L"30 minutes", L"30"}, {L"45 minutes", L"45"}, {L"1 hour", L"60"}, {L"1.5 hours", L"90"},
            {L"2 hours", L"120"}, {L"3 hours", L"180"}, {L"4 hours", L"240"}, {L"6 hours", L"360"}, {L"8 hours", L"480"}};
}

void BuildTallyApp(std::vector<Row>& rows) {
    std::wstring route = BuilderRoute();
    if (!StartsWith(route, L"screentime/app/")) return;
    std::wstring key = route.substr(15);
    int today = Today(), from = AddDays(today, -29), week = WeekStart(today);
    Data d = Load(std::min(from, MonthStart(today)), today);
    std::wstring name = tally::NameOf(key, d.apps);
    auto app = d.apps.find(key);
    long long tToday = 0, tWeek = 0, t30 = 0;
    Row chart;
    chart.group = L"Last 30 days";
    chart.title = L"Time per day";
    chart.chartUnit = L"h";
    for (int day = from; day <= today; day = AddDays(day, 1)) {
        long long s = 0;
        auto it = d.days.find(day);
        if (it != d.days.end()) { auto a = it->second.find(key); if (a != it->second.end()) s = a->second; }
        t30 += s;
        if (day == today) tToday = s;
        if (day >= week) tWeek = s + tWeek;
        chart.chart.push_back(s / 3600.0f);
        int dom = day % 100;
        chart.chartLabels.push_back(dom == 1 || dom % 5 == 0 ? std::to_wstring(dom) : L"");
        chart.chartTips.push_back(DayLabel(day, true) + L": " + Dur(s));
    }
    chart.chartHi = (int)chart.chart.size() - 1;
    chart.valueText = L"Daily average " + Dur(t30 / 30);
    rows.push_back(Stat(L"Time spent", L"Today", Dur(tToday), L""));
    rows.push_back(Stat(L"Time spent", L"This week", Dur(tWeek), L""));
    rows.push_back(Stat(L"Time spent", L"Last 30 days", Dur(t30), L""));
    rows.push_back(std::move(chart));

    Row cat;
    cat.id = L"tally.cat:" + key;
    cat.group = L"Settings for this app";
    cat.title = L"Category";
    cat.label = name + L" category";
    cat.desc = L"Breaker picked " + AutoCategory(key, app != d.apps.end() && app->second.game) + L" by itself. Choose another and every day's totals follow.";
    cat.ctl = CtlChoice;
    cat.choices.push_back({L"Automatic (" + AutoCategory(key, app != d.apps.end() && app->second.game) + L")", L""});
    for (int i = 0; i < tally::kCategoryCount; ++i) cat.choices.push_back({kCategoryNames[i], kCategoryNames[i]});
    cat.flags = F_NOHIST;
    cat.known = false;
    rows.push_back(std::move(cat));
    Row lim;
    lim.id = L"tally.limit:" + key;
    lim.group = L"Settings for this app";
    lim.title = L"Daily limit";
    lim.label = name + L" daily limit";
    lim.desc = L"A gentle notification at 80% and at 100%. Breaker never closes or blocks anything.";
    lim.ctl = CtlChoice;
    lim.choices = LimitChoices();
    lim.flags = F_NOHIST;
    lim.known = false;
    rows.push_back(std::move(lim));
    Row track;
    track.id = L"tally.never:" + key;
    track.group = L"Settings for this app";
    track.title = L"Track this app";
    track.label = L"Tracking " + name;
    track.desc = L"Off means Breaker never records time in it from now on.";
    track.ctl = CtlToggle;
    track.flags = F_NOHIST;
    track.known = false;
    rows.push_back(std::move(track));
    Row del = Info(L"Settings for this app", L"Delete its history", L"Removes every day of recorded time for " + name + L".");
    del.actions.push_back({L"Delete", [key, name] {
        if (!UiConfirm(L"Delete all recorded screen time for " + name + L"?\n\nThis can't be undone.")) return;
        UiBusy(L"", [key] { ClearHistory(20000101, 29991231, key); }, [] { UiToast(L"History deleted"); UiReloadPage(); });
    }});
    rows.push_back(std::move(del));
    if (app != d.apps.end() && !app->second.path.empty())
        rows.push_back(Info(L"About", app->second.game ? L"Recognized as a game" : L"App", app->second.path));
}

// ============================================================ settings tab
static std::wstring TrackerState() { return FindTracker() ? L"Running" : L"Not running"; }

static Row Opt(const std::wstring& id, const wchar_t* group, const std::wstring& title, const std::wstring& desc, Ctl ctl) {
    Row r;
    r.id = id;
    r.group = group;
    r.title = title;
    r.desc = desc;
    r.ctl = ctl;
    r.flags = F_NOHIST;
    r.known = false;
    return r;
}

static void ClearRangeAsk(int from, int to, const std::wstring& what) {
    if (!UiConfirm(L"Delete screen time history for " + what + L"?\n\nThis can't be undone.")) return;
    auto ok = std::make_shared<bool>(false);
    UiBusy(L"Clearing history\x2026", [from, to, ok] { *ok = ClearHistory(from, to, L""); },
           [ok] { UiToast(*ok ? L"Screen time history cleared" : L"Couldn't clear all of it; see breaker.log"); UiReloadPage(); });
}

static int ParseDate(const std::wstring& s) {
    int y, m, d;
    if (swscanf(s.c_str(), L"%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    return y * 10000 + m * 100 + d;
}

static void ExportCsvAsk() {
    std::wstring path;
    if (!UiPickFile(L"CSV files (*.csv)\0*.csv\0", path, true, L"Screen time.csv", L"csv")) return;
    auto ok = std::make_shared<bool>(false);
    UiBusy(L"Exporting\x2026", [path, ok] {
        FlushTracker();
        *ok = ExportCsv(path);
    }, [ok] { UiToast(*ok ? L"Screen time exported" : L"Couldn't write that file"); });
}

void BuildTallySettings(std::vector<Row>& rows) {
    Row on = Opt(L"tally.enabled", L"Tracking", L"Track screen time", L"Counts which app is in front, every 5 seconds, even when Breaker's window is closed. A tiny background tracker starts with Windows; it uses almost no CPU and a few MB of memory.", CtlToggle);
    on.valueText = L"";
    rows.push_back(std::move(on));
    rows.push_back(Opt(L"tally.idle", L"Tracking", L"Count me as away after", L"No mouse or keyboard input for this long stops the clock, unless a game or video is playing. Locked and sleeping time never counts.", CtlChoice));
    rows.push_back(Info(L"Tracking", L"Tracker", L"The background process is \"Breaker.exe --tracker\". It has no window and only reads which app is in front.", TrackerState()));
    rows.push_back(Opt(L"tally.limitNotify", L"Notifications", L"Daily limit reminders", L"A gentle notification at 80% and 100% of a limit. Nothing is ever closed or blocked.", CtlToggle));
    rows.push_back(Opt(L"tally.weekly", L"Notifications", L"Weekly summary", L"At the start of each week: last week's screen time, gaming time and most used app.", CtlToggle));

    for (int i = 0; i < tally::kCategoryCount; ++i) {
        std::wstring cat = kCategoryNames[i];
        Row r = Opt(L"tally.limit:cat:" + cat, L"Daily limits by category", cat, L"", CtlChoice);
        r.choices = LimitChoices();
        r.label = cat + L" daily limit";
        rows.push_back(std::move(r));
    }
    bool anyAppLimit = false;
    for (auto& kv : g_config.tallyLimits) {
        if (StartsWith(kv.first, L"cat:")) continue;
        anyAppLimit = true;
        std::wstring key = kv.first;
        Row r = Info(L"Daily limits for apps", TallyAppTitle(key), L"", Dur(kv.second * 60LL) + L" a day");
        r.actions.push_back({L"Remove", [key] { UiApply({{L"tally.limit:" + key, L"0", TallyAppTitle(key) + L" daily limit"}}); }});
        rows.push_back(std::move(r));
    }
    if (!anyAppLimit) {
        Row r = Info(L"Daily limits for apps", L"No app limits", L"Open an app in Screen time to give it a daily limit.");
        r.ctl = CtlLink;
        r.target = L"breaker:screentime/today";
        rows.push_back(std::move(r));
    }

    for (auto& key : g_config.tallyNever) {
        std::wstring k = key;
        Row r = Info(L"Never track", TallyAppTitle(k), k);
        r.actions.push_back({L"Track again", [k] { UiApply({{L"tally.never:" + k, L"1", L"Tracking " + TallyAppTitle(k)}}); }});
        rows.push_back(std::move(r));
    }
    Row add = Info(L"Never track", g_config.tallyNever.empty() ? L"Every app is tracked" : L"Add another app",
                   L"Apps on this list are never recorded. Type the program's file name, like \"keepass.exe\".");
    add.actions.push_back({L"Add an app", [] {
        UiPrompt(L"Never track an app", {L"Program file name (for example keepass.exe)"}, {L""}, [](const std::vector<std::wstring>& v) {
            std::wstring k = ToLower(Trim(v[0]));
            if (k.empty()) return;
            if (k.find(L'.') == std::wstring::npos) k += L".exe";
            UiApply({{L"tally.never:" + k, L"0", L"Tracking " + k}});
        });
    }});
    rows.push_back(std::move(add));

    for (auto& kv : g_config.tallyCats) {
        std::wstring key = kv.first;
        Row r = Info(L"Categories you changed", TallyAppTitle(key), L"", kv.second);
        r.actions.push_back({L"Reset", [key] { UiApply({{L"tally.cat:" + key, L"", TallyAppTitle(key) + L" category"}}); }});
        rows.push_back(std::move(r));
    }

    Row exp = Info(L"Your data", L"Export to CSV", L"Every day and app with its minutes, for Excel or anything else.");
    exp.actions.push_back({L"Export", [] { ExportCsvAsk(); }});
    rows.push_back(std::move(exp));
    Row clr = Info(L"Your data", L"Clear history", L"Delete recorded screen time. Your limits and settings stay.");
    int today = Today();
    clr.actions.push_back({L"Today", [today] { ClearRangeAsk(today, today, L"today"); }});
    clr.actions.push_back({L"Last 7 days", [today] { ClearRangeAsk(AddDays(today, -6), today, L"the last 7 days"); }});
    clr.actions.push_back({L"Dates\x2026", [] {
        UiPrompt(L"Clear screen time history", {L"From (YYYY-MM-DD)", L"To (YYYY-MM-DD)"}, {L"", L""}, [](const std::vector<std::wstring>& v) {
            int from = ParseDate(Trim(v[0])), to = ParseDate(Trim(v[1]));
            if (!from || !to || to < from) { UiToast(L"Use dates like 2026-11-01, with the first one earlier"); return; }
            ClearRangeAsk(from, to, Trim(v[0]) + L" to " + Trim(v[1]));
        });
    }});
    clr.actions.push_back({L"Everything", [] { ClearRangeAsk(19700101, 29991231, L"all time"); }});
    rows.push_back(std::move(clr));
    Row folder = Info(L"Your data", L"Where it's kept", Dir() + L"\nPlain text files, one per month. Nothing leaves this PC.");
    folder.actions.push_back({L"Open folder", [] { ShellOpen(Dir()); }});
    rows.push_back(std::move(folder));
}

// ============================================================ the "tally." settings
static bool TallyReadUi(const std::wstring& id, std::wstring& out) {
    Config& c = g_config;
    if (id == L"tally.enabled") out = c.tallyEnabled ? L"1" : L"0";
    else if (id == L"tally.weekly") out = c.tallyWeekly ? L"1" : L"0";
    else if (id == L"tally.limitNotify") out = c.tallyLimitNotify ? L"1" : L"0";
    else if (id == L"tally.idle") out = std::to_wstring(c.tallyIdleMin);
    else if (StartsWith(id, L"tally.cat:")) {
        out.clear();
        for (auto& kv : c.tallyCats) if (kv.first == id.substr(10)) out = kv.second;
    } else if (StartsWith(id, L"tally.limit:")) out = std::to_wstring(LimitFor(id.substr(12)));
    else if (StartsWith(id, L"tally.never:")) {
        out = L"1";
        for (auto& k : c.tallyNever) if (k == id.substr(12)) out = L"0";
    } else return false;
    return true;
}

static WriteResult TallyWriteUi(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    Config& c = g_config;
    bool restart = false;
    if (id == L"tally.enabled") { c.tallyEnabled = v == L"1"; restart = true; }
    else if (id == L"tally.weekly") c.tallyWeekly = v == L"1";
    else if (id == L"tally.limitNotify") c.tallyLimitNotify = v == L"1";
    else if (id == L"tally.idle") c.tallyIdleMin = std::clamp(_wtoi(v.c_str()), 1, 60);
    else if (StartsWith(id, L"tally.cat:")) {
        std::wstring key = id.substr(10);
        auto& cats = c.tallyCats;
        cats.erase(std::remove_if(cats.begin(), cats.end(), [&](auto& kv) { return kv.first == key; }), cats.end());
        if (!v.empty()) cats.push_back({key, v});
    } else if (StartsWith(id, L"tally.limit:")) {
        std::wstring key = id.substr(12);
        auto& lims = c.tallyLimits;
        lims.erase(std::remove_if(lims.begin(), lims.end(), [&](auto& kv) { return kv.first == key; }), lims.end());
        if (int m = _wtoi(v.c_str()); m > 0) lims.push_back({key, m});
    } else if (StartsWith(id, L"tally.never:")) {
        std::wstring key = id.substr(12);
        auto& nv = c.tallyNever;
        nv.erase(std::remove(nv.begin(), nv.end(), key), nv.end());
        if (v != L"1") nv.push_back(key);
    } else {
        err = L"Unknown screen time setting.";
        return W_FAIL;
    }
    ConfigSave();
    NotifyTrackerReload();
    if (restart) {
        bool on = c.tallyEnabled;
        RunAsync([on] { EnsureTracker(on); });
    }
    return W_OK;
}

static bool TallyRead(const std::wstring& id, std::wstring& out) {
    bool ok = false;
    UiSync([&] { ok = TallyReadUi(id, out); });
    return ok;
}

static WriteResult TallyWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    WriteResult r = W_FAIL;
    UiSync([&] { r = TallyWriteUi(id, v, err); });
    return r;
}

static void TallyChoices(const std::wstring& id, Choices& out) {
    if (id == L"tally.idle") out = {{L"1 minute", L"1"}, {L"2 minutes", L"2"}, {L"5 minutes", L"5"}, {L"10 minutes", L"10"}, {L"15 minutes", L"15"}};
    else if (StartsWith(id, L"tally.limit:")) out = LimitChoices();
    else if (StartsWith(id, L"tally.cat:")) {
        std::wstring key = id.substr(10);
        auto apps = LoadApps();
        auto it = apps.find(key);
        out.push_back({L"Automatic (" + AutoCategory(key, it != apps.end() && it->second.game) + L")", L""});
        for (int i = 0; i < tally::kCategoryCount; ++i) out.push_back({kCategoryNames[i], kCategoryNames[i]});
    }
}

static const Handler kTallyHandlers[] = {{L"tally.", TallyRead, TallyWrite, TallyChoices, F_NOHIST | F_NOSNAP | F_NOWATCH}};
void RegisterTallyHandlers() { RegisterHandlers(kTallyHandlers, 1); }
