// Breaker - pages, rows and the services the window offers to page code
#pragma once
#include "engine.h"

struct RowAction {
    std::wstring label;
    std::function<void()> run;     // runs on the window thread
};

// One line on a page. Catalog settings become rows automatically; page builders add rows for things
// found at run time (services, startup apps, drives...).
struct Row {
    std::wstring id;               // setting id; empty for information rows
    std::wstring group;            // heading this row sits under
    std::wstring title, desc, detail;
    std::wstring value;            // current value (toggle "1"/"0", choice value, number, text)
    std::wstring valueText;        // right-aligned text for information rows
    Ctl ctl = CtlInfo;
    Choices choices;
    float smin = 0, smax = 100, sstep = 1;
    std::wstring unit;
    unsigned flags = 0;
    std::wstring rec, uri, target; // recommended value, Windows page, link target
    std::wstring label;            // name used in the undo history for dynamic ids
    std::wstring where;            // "System > Display" (search results)
    std::wstring route;            // page this row lives on (search results)
    float bar = -1;                // 0..1 draws a usage bar
    bool known = true;             // false until the value has been read
    bool unavailable = false;      // the read failed: this PC doesn't have it
    bool alert = false;            // draw as a warning
    bool wide = false;             // text control: wide field
    bool tile = false;             // Home: draw as a quick-toggle tile
    bool stat = false;             // with tile: a figure (title small, valueText large, desc below)
    std::vector<float> chart;      // a bar chart row: one value per bar (valueText is drawn top right)
    std::vector<std::wstring> chartLabels, chartTips;   // label under each bar ("" = none), text on hover
    int chartHi = -1;              // bar drawn in the accent color (today)
    std::wstring chartUnit;        // scale label suffix, e.g. "h"
    std::vector<RowAction> actions;
};

using PageBuilder = void (*)(std::vector<Row>& rows);

struct PageInfo {
    const wchar_t* route;          // "system/display"
    const wchar_t* title;          // "Display"
    const wchar_t* blurb;          // one line under the title, may be nullptr
    PageBuilder builder;           // background builder for dynamic rows (may be slow), or nullptr
    PageBuilder builder2;          // a second, slower background builder, or nullptr
    PageBuilder uiBuilder;         // builder that runs on the window thread (Breaker's own pages)
    bool filter;                   // show a filter box (long lists)
};

struct Category {
    const wchar_t* id;             // "system"
    const wchar_t* name;
    wchar_t glyph;
    int group;                     // 0 Windows settings, 1 tools, 2 Breaker
};

extern const Category kCategories[];
extern const int kCategoryCount;
extern const PageInfo kPages[];
extern const int kPageCount;
const PageInfo* FindPage(const std::wstring& route);
const Category* FindCategory(const std::wstring& id);
std::wstring CategoryOf(const std::wstring& route);   // "system/display" -> "system"
Row RowFromDef(const Def& d);
void RegisterProviderHandlers();
#include <set>
bool InstalledAppx(std::set<std::wstring>& out, DWORD maxAgeMs);   // lower-case package names; blocking
void RegisterAppHandlers();

// ---- services from window.cpp (window thread only)
HWND UiWindow();
void UiApply(std::vector<Change> changes, const std::wstring& source = L"user");
void UiToast(const std::wstring& text);
void UiReloadPage();
void UiNavigate(const std::wstring& route, const std::wstring& focusId = L"");
void UiPrompt(const std::wstring& title, const std::vector<std::wstring>& labels, const std::vector<std::wstring>& initial,
              std::function<void(const std::vector<std::wstring>&)> done);
bool UiConfirm(const std::wstring& text, const wchar_t* title = L"Breaker");
bool UiPickFile(const wchar_t* filter, std::wstring& path, bool save = false, const wchar_t* defName = nullptr, const wchar_t* defExt = nullptr);
void UiSync(std::function<void()> fn);   // from a background thread: run fn on the window thread and wait for it
std::wstring UiRoute();            // the page being shown
std::wstring BuilderRoute();       // inside a background page builder: the page it's building
void UiThemeChanged();             // Breaker's own look changed: rebuild fonts and repaint
void UiBusy(const std::wstring& text, std::function<void()> work, std::function<void()> done = nullptr);   // work off-thread

// ---- builders for Breaker's own pages (features.cpp, suite.cpp, system.cpp)
void BuildHome(std::vector<Row>& rows);
void BuildDebloat(std::vector<Row>& rows);
void BuildDebloatApps(std::vector<Row>& rows);
void BuildCheckup(std::vector<Row>& rows);
void BuildProfiles(std::vector<Row>& rows);
void BuildHistory(std::vector<Row>& rows);
void BuildWatchdog(std::vector<Row>& rows);
void BuildSnapshots(std::vector<Row>& rows);
void BuildMyApps(std::vector<Row>& rows);
void BuildMyAppsRemote(std::vector<Row>& rows);
void BuildMyAppSettings(std::vector<Row>& rows);
// Screen time (tallyui.cpp)
void BuildTallyToday(std::vector<Row>& rows);
void BuildTallyWeek(std::vector<Row>& rows);
void BuildTallyMonth(std::vector<Row>& rows);
void BuildTallyApp(std::vector<Row>& rows);
void BuildTallySettings(std::vector<Row>& rows);
void RegisterTallyHandlers();
std::wstring TallyAppTitle(const std::wstring& key);   // window thread
void BuildSettingsGeneral(std::vector<Row>& rows);
void BuildSettingsAppearance(std::vector<Row>& rows);
void BuildSettingsLayout(std::vector<Row>& rows);
void BuildSettingsUpdates(std::vector<Row>& rows);
void BuildSettingsAbout(std::vector<Row>& rows);
