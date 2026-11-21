// Breaker - main window: sidebar with search, page header with tabs, setting rows, toasts, prompts.
// Everything is drawn with Direct2D and only the rows on screen are painted. No system call ever runs
// on this thread: values come from the cache and are refreshed by background tasks.
#include "window.h"
#include "config.h"
#include "gfx.h"
#include "pages.h"
#include "features.h"
#include "textinput.h"
#include "autoupdate.h"
#include "system.h"
#include "suite.h"
#include "a11y.h"
#include <uxtheme.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <oleacc.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

using namespace gfx;

void ReleaseInstanceMutex();   // main.cpp

// ============================================================ state
namespace {
enum ZoneKind {
    ZNone, ZSide, ZTab, ZBack, ZFwd, ZSearch, ZFilter, ZCrumb, ZWinSettings, ZUpdatePill, ZCapMin, ZCapMax, ZCapClose,
    ZRow, ZTile, ZToggle, ZChoice, ZSlider, ZText, ZInfo, ZAction, ZChip, ZToastAction, ZScroll, ZPromptField, ZPromptOk, ZPromptCancel, ZBar,
};
enum Chip { ChipPin, ChipProfile, ChipWindows, ChipLink, ChipRecommended };
enum Area { AreaSidebar, AreaContent, AreaSearch, AreaFilter };
struct Zone { ZoneKind kind = ZNone; int idx = -1, sub = 0; D2D1_RECT_F r{}; };
struct LRow { int row; float x, y, w, h, textW, descH, panelH; bool head; };
struct Toast {
    std::wstring text, action;
    std::function<void()> run;
    DWORD until = 0;
    bool busy = false;
};
struct Prompt {
    bool open = false;
    std::wstring title;
    std::vector<std::wstring> labels;
    std::vector<TextInput> fields;
    int active = 0;
    std::function<void(const std::vector<std::wstring>&)> done;
};
}  // namespace

constexpr float kTitleH = 44, kCapW = 46, kResize = 6, kPad = 32, kRowGap = 3;
constexpr UINT_PTR TIMER_ANIM = 1, TIMER_CARET = 2, TIMER_TOAST = 3, TIMER_PROFILES = 4, TIMER_STARTUP = 5, TIMER_CACHE = 6;

static HWND g_hwnd = nullptr;
static DWORD g_uiThread = 0;
static ID2D1HwndRenderTarget* g_rt = nullptr;
static ID2D1SolidColorBrush* g_br = nullptr;
static Canvas g_cv;
static Theme g_theme;
static UINT g_dpi = 96;
static float g_scale = 1.0f, g_W = 0, g_H = 0;
static IDWriteTextFormat *g_fUi, *g_fUiSemi, *g_fDesc, *g_fSmall, *g_fRight, *g_fGroup, *g_fTitle, *g_fIcon, *g_fIconSm, *g_fCap,
    *g_fInput, *g_fCenter, *g_fMono, *g_fLogo;

static std::wstring g_route = L"home", g_searchReturn = L"home";
static std::vector<std::wstring> g_back, g_fwd;
static int g_gen = 0;
static std::vector<Row> g_rowsUi, g_rowsStatic, g_rowsDyn1, g_rowsDyn2, g_rows;
static bool g_loading1 = false, g_loading2 = false;
// Rows found on the PC, remembered per page: revisiting shows them instantly while a fresh copy is built
struct PageCache { std::vector<Row> dyn1, dyn2; DWORD at1 = 0, at2 = 0; DWORD used = 0; };
static std::unordered_map<std::wstring, PageCache> g_pageCache;
constexpr DWORD kPageFreshMs = 20000;     // a revisit within this time doesn't rebuild at all
// Text heights, so long lists (services, installed apps) don't re-measure every row on every layout
static std::unordered_map<std::wstring, float> g_measure;
static std::vector<int> g_vis;
static std::vector<LRow> g_lay;
static bool g_layDirty = true;
static float g_contentH = 0, g_headerBottom = 0, g_scroll = 0, g_scrollTo = 0;
static std::wstring g_expanded, g_pendingFocus, g_flashKey;
static float g_flash = 0, g_pageAnim = 1;
static int g_focus = -1, g_sideFocus = 0, g_area = AreaContent;
static bool g_kb = false;
static std::vector<Zone> g_zones;
static Zone g_hot;
static TextInput g_search, g_filter;
static Toast g_toast;
static Prompt g_prompt;
static std::unordered_map<std::wstring, float> g_tog;
static bool g_togMoving = false;      // a switch on screen is mid-animation (set while painting)
static int g_busy = 0;
static std::wstring g_dragKey;
static float g_dragValue = 0, g_scrollGrab = -1;
static std::vector<uint64_t> g_keepSeqs;
static DWORD g_keepUntil = 0;
static std::vector<AccItem> g_acc;
static std::vector<Zone> g_accZones;
static bool g_accActive = false;
static bool g_startHidden = false;

static D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
static bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static float Px2Dip(int v) { return v / g_scale; }
static void Redraw() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }
static bool Compact() { return g_config.density == L"compact"; }
static float SideW() { return g_config.sidebarWidth; }
static std::wstring RowKey(const Row& r) { return r.id.empty() ? L"#" + r.title : r.id; }
static bool IsSetting(const Row& r) { return !r.id.empty() && r.ctl != CtlLink && r.ctl != CtlInfo; }

static float AnimK() {
    const std::wstring& a = g_config.animation;
    return a == L"none" ? 1.0f : a == L"fast" ? 0.45f : a == L"slow" ? 0.12f : 0.26f;
}
static void StartAnim() { if (g_hwnd) SetTimer(g_hwnd, TIMER_ANIM, 15, nullptr); }

HWND UiWindow() { return g_hwnd; }
std::wstring UiRoute() { return g_route; }

// ============================================================ device resources
static void ReleaseFormats() {
    IDWriteTextFormat** all[] = {&g_fUi, &g_fUiSemi, &g_fDesc, &g_fSmall, &g_fRight, &g_fGroup, &g_fTitle, &g_fIcon, &g_fIconSm,
                                 &g_fCap, &g_fInput, &g_fCenter, &g_fMono, &g_fLogo};
    for (auto p : all) SafeRelease(*p);
}

// Windows "Text size" (Settings > Accessibility) scales Breaker's text too
static float TextScale() {
    DWORD v = 100;
    RegReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", v);
    return std::clamp(v / 100.0f, 1.0f, 2.25f);
}

static void CreateFormats() {
    ReleaseFormats();
    static std::wstring family;
    family = (g_config.fontFamily.empty() || !FontAvailable(g_config.fontFamily.c_str())) ? UiFont() : g_config.fontFamily;
    const wchar_t* ui = family.c_str();
    const wchar_t* display = g_config.fontFamily.empty() ? DisplayFont() : ui;
    float k = g_config.fontSize / 13.0f * TextScale();
    g_fUi = Format(ui, 13 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fUiSemi = Format(ui, 13 * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fDesc = Format(ui, 12 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
    g_fSmall = Format(ui, 11.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fRight = Format(ui, 12.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    g_fGroup = Format(ui, 12.5f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fTitle = Format(display, 24 * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fIcon = Format(IconFont(), 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconSm = Format(IconFont(), 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fCap = Format(IconFont(), 10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fInput = Format(ui, 13 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fCenter = Format(ui, 12.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_fMono = Format(MonoFont(), 11.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
    g_fLogo = Format(display, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_measure.clear();
    g_layDirty = true;
}

static void DiscardDevice() {
    SafeRelease(g_br);
    SafeRelease(g_rt);
    g_cv.rt = nullptr;
    g_cv.br = nullptr;
}

static bool EnsureDevice() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT hr = d2d->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)),
                                                D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        &g_rt);
    if (FAILED(hr)) { Log(L"CreateHwndRenderTarget failed 0x%08lx", hr); return false; }
    g_rt->SetDpi((float)g_dpi, (float)g_dpi);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &g_br);
    g_cv.rt = g_rt;
    g_cv.br = g_br;
    return true;
}

static void ApplyTheme() {
    g_theme = BuildTheme(g_config);
    ApplyWindowTheme(g_hwnd, g_theme, false);
    if (!g_theme.backdrop) {
        MARGINS m{0, 0, 1, 0};   // keeps the DWM shadow with our custom frame
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
    }
    ApplyMenuTheme(g_theme.dark);
    WTA_OPTIONS wta{WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON | WTNCA_NOSYSMENU, WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON | WTNCA_NOSYSMENU};
    SetWindowThemeAttribute(g_hwnd, WTA_NONCLIENT, &wta, sizeof(wta));
    Redraw();
}

void UiThemeChanged() {
    ApplyTheme();
    CreateFormats();
    Redraw();
}

void UiBackgroundChanged() {
    if (g_config.keepInTray) TrayAdd(g_hwnd); else TrayRemove();
    WinIHookSet(g_hwnd, g_config.winI);
    if (g_config.autoProfiles) SetTimer(g_hwnd, TIMER_PROFILES, 5000, nullptr); else KillTimer(g_hwnd, TIMER_PROFILES);
}

// ============================================================ toasts, prompts, dialogs
static void ShowToast(const std::wstring& text, const std::wstring& action = L"", std::function<void()> run = nullptr, DWORD ms = 0) {
    g_toast = {};
    g_toast.text = text;
    g_toast.action = action;
    g_toast.run = std::move(run);
    g_toast.until = GetTickCount() + (ms ? ms : (g_toast.action.empty() ? 4500 : 9000));
    SetTimer(g_hwnd, TIMER_TOAST, 250, nullptr);
    Redraw();
}

void UiToast(const std::wstring& text) { if (!text.empty()) ShowToast(text); }

bool UiConfirm(const std::wstring& text, const wchar_t* title) {
    return MessageBoxW(g_hwnd, text.c_str(), title, MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
}

bool UiPickFile(const wchar_t* filter, std::wstring& path, bool save, const wchar_t* defName, const wchar_t* defExt) {
    wchar_t buf[MAX_PATH * 4] = {};
    if (defName) wcsncpy_s(buf, defName, _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)std::size(buf);
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) {
        if (DWORD e = CommDlgExtendedError()) Log(L"File dialog failed (error 0x%lx)", e);
        return false;
    }
    path = buf;
    return true;
}

void UiPrompt(const std::wstring& title, const std::vector<std::wstring>& labels, const std::vector<std::wstring>& initial,
              std::function<void(const std::vector<std::wstring>&)> done) {
    g_prompt = {};
    g_prompt.open = true;
    g_prompt.title = title;
    g_prompt.labels = labels;
    g_prompt.fields.resize(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        g_prompt.fields[i].SetText(i < initial.size() ? initial[i] : L"");
        g_prompt.fields[i].SelectAll();
    }
    g_prompt.done = std::move(done);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    Redraw();
}

static void ClosePrompt(bool ok) {
    if (!g_prompt.open) return;
    std::vector<std::wstring> values;
    for (auto& f : g_prompt.fields) values.push_back(f.text);
    auto done = g_prompt.done;
    g_prompt = {};
    Redraw();
    if (ok && done) done(values);
}

void UiSync(std::function<void()> fn) {
    if (!g_hwnd || GetCurrentThreadId() == g_uiThread) fn();
    else SendMessageW(g_hwnd, WM_BREAKER_CALL, 1, (LPARAM)&fn);
}

void UiBusy(const std::wstring& text, std::function<void()> work, std::function<void()> done) {
    ++g_busy;
    bool show = !text.empty();
    if (show) {
        g_toast = {};
        g_toast.text = text;
        g_toast.busy = true;
        g_toast.until = GetTickCount() + 3600 * 1000;
        StartAnim();
        Redraw();
    }
    RunAsync([work, done, show] {
        work();
        Ui([done, show] {
            --g_busy;
            if (show && g_toast.busy) g_toast = {};
            if (done) done();
            Redraw();
        });
    });
}

static int PopupMenuAt(const std::vector<std::pair<std::wstring, bool>>& items, const D2D1_RECT_F* anchor) {
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].first == L"-") AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        else AppendMenuW(menu, MF_STRING | (items[i].second ? MF_CHECKED : 0), i + 1, items[i].first.c_str());
    }
    POINT pt;
    if (anchor) {
        pt = {(LONG)(anchor->left * g_scale), (LONG)(anchor->bottom * g_scale) + 2};
        ClientToScreen(g_hwnd, &pt);
    } else {
        GetCursorPos(&pt);
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    return cmd - 1;
}

// ============================================================ sidebar order
std::vector<std::wstring> SidebarOrder() {
    std::vector<std::wstring> out;
    for (auto& id : g_config.sidebarOrder)
        if (FindCategory(id) && id != L"settings" && id != L"home" && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    for (int i = 0; i < kCategoryCount; ++i) {
        std::wstring id = kCategories[i].id;
        if (id == L"settings" || id == L"home") continue;
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    out.insert(out.begin(), L"home");
    out.push_back(L"settings");
    return out;
}

static std::vector<const Category*> SideCats() {
    std::vector<const Category*> out;
    auto& hidden = g_config.sidebarHidden;
    for (auto& id : SidebarOrder())
        if (std::find(hidden.begin(), hidden.end(), id) == hidden.end()) out.push_back(FindCategory(id));
    return out;
}

static std::vector<const PageInfo*> TabsFor(const std::wstring& category) {
    std::vector<const PageInfo*> out;
    for (int i = 0; i < kPageCount; ++i) if (CategoryOf(kPages[i].route) == category) out.push_back(&kPages[i]);
    return out;
}

// ============================================================ rows and values
static void ApplyFilter() {
    g_vis.clear();
    std::wstring f = ToLower(Trim(g_filter.text));
    for (int i = 0; i < (int)g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (!f.empty() && ToLower(r.title).find(f) == std::wstring::npos && ToLower(r.desc).find(f) == std::wstring::npos &&
            ToLower(r.group).find(f) == std::wstring::npos) continue;
        g_vis.push_back(i);
    }
    g_layDirty = true;
    g_zones.clear();          // zones point at rows by position; they're rebuilt on the next paint
    g_accZones.clear();
    g_acc.clear();
    g_hot = {};
}

static Row LoadingRow(const wchar_t* text) {
    Row r;
    r.group = L"";
    r.title = text;
    r.valueText = L"\x2026";
    return r;
}

static void Merge() {
    std::wstring focusKey = (g_focus >= 0 && g_focus < (int)g_vis.size()) ? RowKey(g_rows[g_vis[g_focus]]) : L"";
    g_rows.clear();
    const PageInfo* page = FindPage(g_route);
    bool uiFirst = page && page->uiBuilder;
    auto add = [](std::vector<Row>& from) { g_rows.insert(g_rows.end(), from.begin(), from.end()); };
    if (uiFirst) add(g_rowsUi);
    add(g_rowsDyn1);
    if (g_loading1 && g_rowsDyn1.empty()) g_rows.push_back(LoadingRow(L"Reading from Windows"));
    add(g_rowsDyn2);
    if (g_loading2 && g_rowsDyn2.empty()) g_rows.push_back(LoadingRow(g_route == L"update" ? L"Asking Windows Update what's waiting" : L"Reading from Windows"));
    if (!uiFirst) add(g_rowsUi);
    add(g_rowsStatic);
    for (auto& r : g_rows) {
        if (!IsSetting(r) || r.known) continue;
        if (CachedValue(r.id, r.value)) r.known = true;
    }
    ApplyFilter();
    g_focus = -1;
    for (int i = 0; i < (int)g_vis.size() && !focusKey.empty(); ++i) if (RowKey(g_rows[g_vis[i]]) == focusKey) g_focus = i;
    if (g_accActive) AccNotifyReorder(g_hwnd);
    Redraw();
}

static void StoreValue(int gen, const std::wstring& id, bool ok, const std::wstring& value, const Choices* choices) {
    if (ok) CacheStore(id, value);
    if (gen != g_gen) return;
    for (auto& r : g_rows) {
        if (r.id != id) continue;
        if (ok) { r.value = value; r.known = true; r.unavailable = false; }
        else { r.unavailable = true; r.known = true; }
        if (choices && !choices->empty()) r.choices = *choices;
    }
    for (auto* list : {&g_rowsUi, &g_rowsStatic, &g_rowsDyn1, &g_rowsDyn2})
        for (auto& r : *list) if (r.id == id) { if (ok) { r.value = value; r.unavailable = false; } else r.unavailable = true; r.known = true; if (choices && !choices->empty()) r.choices = *choices; }
    g_layDirty = true;
    Redraw();
}

// Re-reads every setting on the page in the background. Registry-backed settings go in one quick batch;
// each handler-backed setting gets its own task so a slow one never holds up the rest.
static void RefreshValues() {
    std::vector<std::wstring> fast;
    int gen = g_gen;
    for (auto& r : g_rows) {
        if (!IsSetting(r)) continue;
        const Def* d = FindDef(r.id);
        if (!d && r.known) continue;                 // built with its value by a page builder
        bool handler = !d || (d->spec && d->spec[0] == L'@');
        if (!handler) { fast.push_back(r.id); continue; }
        std::wstring id = r.id;
        bool wantChoices = r.ctl == CtlChoice && (!d || !d->choices);
        RunAsync([gen, id, wantChoices] {
            auto choices = std::make_shared<Choices>();
            std::wstring v;
            bool ok = ReadValue(id, v);
            if (wantChoices) GetChoices(id, *choices);
            Ui([gen, id, ok, v, choices] { StoreValue(gen, id, ok, v, choices.get()); });
        });
    }
    if (fast.empty()) return;
    RunAsync([gen, fast] {
        auto vals = std::make_shared<std::vector<std::pair<bool, std::wstring>>>();
        for (auto& id : fast) {
            std::wstring v;
            bool ok = ReadValue(id, v);
            vals->push_back({ok, v});
        }
        Ui([gen, fast, vals] { for (size_t i = 0; i < fast.size(); ++i) StoreValue(gen, fast[i], (*vals)[i].first, (*vals)[i].second, nullptr); });
    });
}

static void TrimPageCache() {
    while (g_pageCache.size() > 10) {
        auto oldest = g_pageCache.begin();
        for (auto it = g_pageCache.begin(); it != g_pageCache.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
        g_pageCache.erase(oldest);
    }
}

static thread_local std::wstring t_builderRoute;
std::wstring BuilderRoute() { return t_builderRoute; }

static void RunBuilder(PageBuilder builder, int slot) {
    int gen = g_gen;
    std::wstring route = g_route;
    RunAsync([builder, slot, gen, route] {
        auto rows = std::make_shared<std::vector<Row>>();
        t_builderRoute = route;
        builder(*rows);
        t_builderRoute.clear();
        Ui([rows, slot, gen, route] {
            PageCache& pc = g_pageCache[route];
            (slot == 1 ? pc.dyn1 : pc.dyn2) = *rows;
            (slot == 1 ? pc.at1 : pc.at2) = GetTickCount();
            pc.used = GetTickCount();
            TrimPageCache();
            if (gen != g_gen) return;
            (slot == 1 ? g_rowsDyn1 : g_rowsDyn2) = std::move(*rows);
            (slot == 1 ? g_loading1 : g_loading2) = false;
            Merge();
            // settings that came with these rows still need their current values read
            for (auto& r : g_rows)
                if (IsSetting(r) && (!r.known || (r.ctl == CtlChoice && r.choices.empty()))) { RefreshValues(); break; }
        });
    });
}

// Builds the rows for g_route. keep = true refreshes in place (same scroll position, no flash of emptiness).
static void BuildPage(bool keep) {
    ++g_gen;
    const PageInfo* page = FindPage(g_route);
    g_rowsUi.clear();
    g_rowsStatic.clear();
    if (!keep) { g_rowsDyn1.clear(); g_rowsDyn2.clear(); g_scroll = g_scrollTo = 0; g_expanded.clear(); g_focus = -1; g_filter.SetText(L""); g_tog.clear(); }
    g_loading1 = g_loading2 = false;
    if (g_route == L"search") {
        SearchRows(g_search.text, g_rowsStatic);
    } else if (page) {
        if (page->uiBuilder) page->uiBuilder(g_rowsUi);
        for (int i = 0; i < kDefCount; ++i) if (g_route == kDefs[i].page) g_rowsStatic.push_back(RowFromDef(kDefs[i]));
        auto cached = g_pageCache.find(g_route);
        PageCache* pc = cached == g_pageCache.end() ? nullptr : &cached->second;
        if (pc) pc->used = GetTickCount();
        auto start = [&](PageBuilder b, int slot) {
            if (!b) return;
            std::vector<Row>& dst = slot == 1 ? g_rowsDyn1 : g_rowsDyn2;
            bool& loading = slot == 1 ? g_loading1 : g_loading2;
            bool have = pc && (slot == 1 ? pc->at1 : pc->at2);
            if (have && !keep) dst = slot == 1 ? pc->dyn1 : pc->dyn2;    // show the last result straight away
            bool fresh = have && GetTickCount() - (slot == 1 ? pc->at1 : pc->at2) < kPageFreshMs;
            if (fresh && !keep) return;                                  // just built: nothing to redo
            loading = !have && dst.empty();
            RunBuilder(b, slot);
        };
        start(page->builder, 1);
        start(page->builder2, 2);
    }
    Merge();
    RefreshValues();
    if (!g_pendingFocus.empty()) {
        for (int i = 0; i < (int)g_vis.size(); ++i)
            if (g_rows[g_vis[i]].id == g_pendingFocus) { g_focus = i; g_expanded = g_pendingFocus; g_flashKey = g_pendingFocus; g_flash = 1; g_area = AreaContent; }
        g_pendingFocus.clear();
        StartAnim();
    }
}

void UiReloadPage() { BuildPage(true); }

static void GoTo(const std::wstring& route, bool record) {
    std::wstring target = route;
    if (!FindPage(target)) {
        auto tabs = TabsFor(target);          // a category id opens its first page
        if (tabs.empty()) return;
        target = tabs[0]->route;
    }
    if (target == g_route) { BuildPage(true); return; }
    if (record) {
        g_back.push_back(g_route == L"search" ? g_searchReturn : g_route);
        if (g_back.size() > 60) g_back.erase(g_back.begin());
        g_fwd.clear();
    }
    if (g_route == L"search" || !g_search.text.empty()) g_search.SetText(L"");
    g_route = target;
    g_config.lastPage = target;
    g_pageAnim = g_config.animation == L"none" ? 1.0f : 0.0f;
    if (g_area == AreaSearch || g_area == AreaFilter) g_area = AreaContent;
    BuildPage(false);
    StartAnim();
    if (target == L"watchdog" && g_config.watchdogEnabled) WatchdogRefresh(false);   // always show a fresh comparison
}

void UiNavigate(const std::wstring& route, const std::wstring& focusId) {
    g_pendingFocus = focusId;
    if (!focusId.empty()) {
        const Def* d = FindDef(focusId);
        if (d && wcscmp(d->page, L"~") != 0 && route.empty()) { GoTo(d->page, true); return; }
    }
    GoTo(route, true);
}

static void GoBack() {
    if (g_back.empty()) return;
    std::wstring to = g_back.back();
    g_back.pop_back();
    g_fwd.push_back(g_route == L"search" ? g_searchReturn : g_route);
    GoTo(to, false);
}
static void GoForward() {
    if (g_fwd.empty()) return;
    std::wstring to = g_fwd.back();
    g_fwd.pop_back();
    g_back.push_back(g_route);
    GoTo(to, false);
}

static void OnSearchChanged() {
    if (Trim(g_search.text).empty()) {
        if (g_route == L"search") { g_route = g_searchReturn; BuildPage(false); }
        return;
    }
    if (g_route != L"search") { g_searchReturn = g_route; g_route = L"search"; }
    BuildPage(false);
}

// "breaker://system/display#display.refresh", "breaker:profiles", or a bare route
static void OpenLinkSpec(const std::wstring& spec) {
    std::wstring s = spec;
    if (StartsWith(s, L"breaker://")) s = s.substr(10);
    else if (StartsWith(s, L"breaker:")) s = s.substr(8);
    while (!s.empty() && s.back() == L'/') s.pop_back();
    size_t hash = s.find(L'#');
    std::wstring id = hash == std::wstring::npos ? L"" : s.substr(hash + 1);
    s = hash == std::wstring::npos ? s : s.substr(0, hash);
    if (s.empty() && id.empty()) return;
    UiNavigate(s, id);
}

static void RestartExplorer();

static void RunAction(const std::wstring& name) {
    if (name == L"restartexplorer") {
        RestartExplorer();
    } else if (name == L"emptybin") {
        if (!UiConfirm(L"Permanently delete everything in the Recycle Bin?\n\nThis can't be undone.")) return;
        SHEmptyRecycleBinW(g_hwnd, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI);
        UiToast(L"Recycle Bin emptied");
        if (g_route == L"system/storage") BuildPage(true);
    } else if (name == L"batteryreport") {
        auto ok = std::make_shared<bool>(false);
        std::wstring file = ExpandEnv(L"%TEMP%\\battery-report.html");
        UiBusy(L"Building the battery report\x2026", [file, ok] {
            DWORD code = 1;
            RunCapture(L"powercfg.exe /batteryreport /output \"" + file + L"\"", nullptr, 60000, &code);
            *ok = code == 0 && FileExists(file);
        }, [file, ok] {
            if (*ok) ShellOpen(file);
            else UiToast(L"Windows couldn't make a battery report. This PC may not have a battery.");
        });
    } else if (name == L"restorepoint") {
        UiApply({{L"sys.restorePoint", L"Made with Breaker", L"Restore point"}}, L"user");
    } else if (name == L"clipclear") {
        if (OpenClipboard(g_hwnd)) { EmptyClipboard(); CloseClipboard(); }
        UiToast(L"Clipboard cleared");
    } else if (name == L"flushdns") {
        UiBusy(L"Flushing the DNS cache\x2026", [] { RunCapture(L"ipconfig.exe /flushdns", nullptr, 20000); }, [] { UiToast(L"DNS cache flushed"); });
    } else if (name == L"wucheck") {
        ShellOpen(L"usoclient.exe", L"StartInteractiveScan");
        OpenTarget(L"ms-settings:windowsupdate");
    }
}

static void OpenRowLink(const std::wstring& target) {
    if (StartsWith(target, L"breaker:")) OpenLinkSpec(target);
    else if (StartsWith(target, L"action:")) RunAction(target.substr(7));
    else if (!OpenTarget(target)) UiToast(L"Windows couldn't open that on this PC");
}

// ============================================================ applying changes
static void RestartExplorer() {
    UiBusy(L"Restarting Explorer\x2026", [] {
        RunCapture(L"taskkill.exe /f /im explorer.exe", nullptr, 15000);
        Sleep(500);
        ShellOpen(L"explorer.exe");
    });
}

static void UndoSeqs(std::vector<uint64_t> seqs) {
    auto rep = std::make_shared<ApplyReport>();
    UiBusy(L"", [seqs, rep] { for (auto it = seqs.rbegin(); it != seqs.rend(); ++it) { ApplyReport r = HistoryRevert(*it); rep->ok += r.ok; rep->failed += r.failed; if (rep->error.empty()) rep->error = r.error; } },
           [rep] { UiToast(rep->ok ? (rep->ok == 1 ? L"Change undone" : std::to_wstring(rep->ok) + L" changes undone") : rep->error); BuildPage(true); WatchdogRefresh(false); });
}

static void OnApplied(const ApplyReport& rep, const std::vector<Change>& changes, const std::wstring& source) {
    bool reload = changes.size() > 1 || source != L"user" || rep.failed > 0 || g_route == L"home" || g_route == L"debloat";
    if (rep.ok) CheckupRefresh();
    for (auto& c : changes) if (!FindDef(c.id)) reload = true;
    std::wstring text, action;
    std::function<void()> run;
    if (rep.ok == 0) {
        text = rep.cancelled ? L"Nothing was changed: administrator permission wasn't given" : (rep.error.empty() ? L"That change didn't apply" : rep.error);
        if (changes.size() == 1) {
            std::wstring uri;
            if (const Def* d = FindDef(changes[0].id)) uri = d->uri ? d->uri : L"";
            if (!uri.empty() && !rep.cancelled) { action = L"Open in Windows"; run = [uri] { OpenTarget(uri); }; }
        }
    } else {
        if (changes.size() == 1) {
            const Change& c = changes[0];
            std::wstring now;
            CachedValue(c.id, now);
            std::wstring name = c.label.empty() ? NameOf(c.id) : c.label;
            if (StartsWith(c.id, L"svcrun:")) text = name + (c.value == L"stop" ? L" stopped" : L" started");
            else if (c.id == L"sys.restorePoint") text = L"Restore point created. Find it in System Restore (rstrui).";
            else text = name + L": " + DescribeValue(c.id, now.empty() ? c.value : now);
        } else {
            text = std::to_wstring(rep.ok) + L" settings changed";
            if (rep.failed) text += L", " + std::to_wstring(rep.failed) + L" couldn't be (" + rep.error + L")";
        }
        if (!rep.note.empty()) text += L"  \x00B7  " + rep.note;
        if (rep.flags & F_REBOOT) text += L"  \x00B7  takes effect after a restart";
        else if (rep.flags & F_SIGNOUT) text += L"  \x00B7  takes effect after you sign out";
        std::vector<uint64_t> seqs = rep.seqs;
        if (rep.flags & F_EXPLORER) { text += L"  \x00B7  needs Explorer to restart"; action = L"Restart Explorer"; run = [] { RestartExplorer(); }; }
        else if (!seqs.empty() && source != L"undo") { action = L"Undo"; run = [seqs] { UndoSeqs(seqs); }; }
        if ((rep.flags & F_KEEP) && !seqs.empty()) {
            g_keepSeqs = seqs;
            g_keepUntil = GetTickCount() + 15000;
            text = L"Keep this display mode?";
            action = L"Keep";
            run = [] { g_keepSeqs.clear(); UiToast(L"Display mode kept"); };
        }
    }
    ShowToast(text, action, run, (rep.flags & F_KEEP) && rep.ok ? 15500 : 0);
    if (reload) BuildPage(true); else RefreshValues();
    if (!WatchdogDrift().empty() || source == L"watchdog") WatchdogRefresh(false);
    if (g_accActive && g_focus >= 0) AccNotifyChanged(g_hwnd, AccFocusedItem());
}

void UiApply(std::vector<Change> changes, const std::wstring& source) {
    if (changes.empty()) return;
    // A choice whose value starts with "?" asks for text first ("?exe:" + what the user types)
    if (changes.size() == 1 && !changes[0].value.empty() && changes[0].value[0] == L'?') {
        Change c = changes[0];
        std::wstring prefix = c.value.substr(1);
        bool exe = prefix == L"exe:", time = prefix == L"time:";
        UiPrompt(exe ? L"Which apps?" : time ? L"Which hours?" : NameOf(c.id),
                 {exe ? L"Program names, separated by semicolons (game.exe;other.exe)" : time ? L"From-to in 24-hour time, like 22:00-07:00" : L"Value"},
                 {time ? L"22:00-07:00" : L""},
                 [c, prefix, source](const std::vector<std::wstring>& v) {
                     std::wstring t = Trim(v[0]);
                     if (!t.empty()) UiApply({{c.id, prefix + t, c.label}}, source);
                 });
        return;
    }
    if (g_config.confirmRisky && source == L"user") {
        std::wstring names;
        for (auto& c : changes) if (FlagsOf(c.id) & F_RISKY) names += (names.empty() ? L"" : L", ") + (c.label.empty() ? NameOf(c.id) : c.label);
        if (!names.empty() &&
            MessageBoxW(g_hwnd, (L"Change \"" + names + L"\"?\n\nThis one can make your PC less safe or stop something working. Breaker records the change so you can undo it from the undo history.").c_str(),
                        L"Breaker", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
    }
    for (auto& c : changes) {    // show the new state straight away; it's corrected if the change fails
        for (auto& r : g_rows) {
            if (r.id != c.id) continue;
            if (c.label.empty() && !FindDef(c.id)) c.label = r.label.empty() ? r.title : r.label;
            if (!StartsWith(c.value, L"raw:") && !StartsWith(c.id, L"svcrun:")) r.value = c.value;
        }
    }
    StartAnim();
    Redraw();
    ++g_busy;
    RunAsync([changes, source] {
        ApplyReport rep = ApplyChanges(changes, source);
        Ui([rep, changes, source] { --g_busy; OnApplied(rep, changes, source); });
    });
}

static std::wstring ChoiceText(const Row& r) {
    for (auto& c : r.choices) if (c.second == r.value) return c.first;
    if (r.value == L"~" || r.value.empty()) return L"Default";
    return r.value;
}

static void OpenChoiceMenu(int vi, const D2D1_RECT_F* anchor) {
    Row& r = g_rows[g_vis[vi]];
    if (r.choices.empty()) { UiToast(L"Still reading the choices from Windows"); return; }
    std::vector<std::pair<std::wstring, bool>> items;
    for (auto& c : r.choices) items.push_back({c.first, c.second == r.value});
    std::wstring id = r.id, label = r.label, current = r.value;
    Choices choices = r.choices;
    int pick = PopupMenuAt(items, anchor);      // modal: the rows may be rebuilt while the menu is open
    if (pick >= 0 && pick < (int)choices.size() && choices[pick].second != current) UiApply({{id, choices[pick].second, label}});
}

static void EditText(int vi) {
    Row& r = g_rows[g_vis[vi]];
    std::wstring id = r.id, label = r.label, title = r.title, value = r.value == L"~" ? L"" : r.value;
    if (r.flags & F_FILE) {
        std::wstring path;
        if (UiPickFile(L"Pictures\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp;*.tif;*.tiff\0All files\0*.*\0", path)) UiApply({{id, path, label}});
    } else if (r.flags & F_COLOR) {
        static COLORREF custom[16];
        uint32_t rgb = ParseHexColor(value, 0x4CC2FF);
        CHOOSECOLORW cc{sizeof(cc)};
        cc.hwndOwner = g_hwnd;
        cc.lpCustColors = custom;
        cc.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc))
            UiApply({{id, HexColor((GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult)), label}});
    } else {
        UiPrompt(title, {L"Value"}, {value}, [id, label](const std::vector<std::wstring>& v) { UiApply({{id, v[0], label}}); });
    }
}

static void ActivateRow(int vi) {
    if (vi < 0 || vi >= (int)g_vis.size()) return;
    Row& r = g_rows[g_vis[vi]];
    if (IsSetting(r) && r.unavailable) { if (!r.uri.empty()) OpenTarget(r.uri); return; }
    if (IsSetting(r) && !r.known) return;
    switch (r.ctl) {
        case CtlToggle: UiApply({{r.id, r.value == L"1" ? L"0" : L"1", r.label}}); break;
        case CtlChoice: OpenChoiceMenu(vi, nullptr); break;
        case CtlText: EditText(vi); break;
        case CtlLink: { std::wstring t = r.target; OpenRowLink(t); break; }
        case CtlInfo: if (!r.actions.empty()) { auto fn = r.actions[0].run; fn(); } break;
        default: break;
    }
}

static void SliderStep(int vi, int dir) {
    Row& r = g_rows[g_vis[vi]];
    if (!r.known || r.unavailable) return;
    float v = std::clamp((float)_wtof(r.value.c_str()) + dir * r.sstep, r.smin, r.smax);
    UiApply({{r.id, std::to_wstring((int)std::lround(v)), r.label}});
}

static void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) { memcpy(GlobalLock(g), s.c_str(), bytes); GlobalUnlock(g); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); }
    CloseClipboard();
}

static bool IsPinned(const std::wstring& id) { return std::find(g_config.pinned.begin(), g_config.pinned.end(), id) != g_config.pinned.end(); }

static void DoChip(const Row& r, int chip, const D2D1_RECT_F* anchor) {
    if (chip == ChipPin) {
        auto& p = g_config.pinned;
        if (IsPinned(r.id)) { p.erase(std::remove(p.begin(), p.end(), r.id), p.end()); UiToast(r.title + L" removed from Home"); }
        else { p.push_back(r.id); UiToast(r.title + L" pinned to Home"); }
        ConfigSave();
        if (g_route == L"home") BuildPage(true);
        Redraw();
    } else if (chip == ChipProfile) {
        std::vector<std::pair<std::wstring, bool>> items;
        for (auto& p : g_config.profiles) items.push_back({L"Add to " + p.name, false});
        if (items.empty()) { UiToast(L"Create a profile first, on the Profiles page"); return; }
        int pick = PopupMenuAt(items, anchor);
        if (pick >= 0 && pick < (int)g_config.profiles.size()) ProfileAddSetting(g_config.profiles[pick].name, r.id, r.value);
    } else if (chip == ChipWindows) {
        if (!OpenTarget(r.uri)) UiToast(L"Windows couldn't open that page");
    } else if (chip == ChipLink) {
        CopyText(L"breaker://" + (r.route.empty() || r.route == L"~" ? g_route : r.route) + L"#" + r.id);
        UiToast(L"Link copied. Paste it in Run (Win+R) or a shortcut to jump straight here.");
    } else if (chip == ChipRecommended) {
        UiApply({{r.id, r.rec, r.label}});
    }
}

static std::vector<std::pair<int, std::wstring>> ChipsFor(const Row& r) {
    std::vector<std::pair<int, std::wstring>> chips;
    const Def* d = FindDef(r.id);
    if (IsSetting(r) && r.known && !r.unavailable && !r.rec.empty() && r.value != r.rec) chips.push_back({ChipRecommended, L"Use recommended"});
    if (d && IsSetting(r)) chips.push_back({ChipPin, IsPinned(r.id) ? L"Unpin from Home" : L"Pin to Home"});
    if (IsSetting(r) && !(r.flags & F_NOHIST) && r.known && !r.unavailable) chips.push_back({ChipProfile, L"Add to profile"});
    if (!r.uri.empty()) chips.push_back({ChipWindows, L"Open in Windows Settings"});
    if (d) chips.push_back({ChipLink, L"Copy link"});
    return chips;
}

static bool HasInfo(const Row& r) { return !r.id.empty() && r.ctl != CtlInfo && (!r.detail.empty() || !ChipsFor(r).empty()); }

static std::wstring PanelText(const Row& r) {
    std::wstring t;
    if (!r.detail.empty()) t = (r.ctl == CtlLink ? L"" : L"What it changes\n") + r.detail;
    auto note = [&](const wchar_t* s) { t += (t.empty() ? L"" : L"\n") + std::wstring(s); };
    if (r.flags & F_ADMIN) note(L"Needs administrator rights. Windows asks once, only for this change.");
    if (r.flags & F_REBOOT) note(L"Takes effect after you restart the PC.");
    if (r.flags & F_SIGNOUT) note(L"Takes effect after you sign out and back in.");
    if (r.flags & F_EXPLORER) note(L"Takes full effect after Explorer restarts. Breaker offers to do that for you.");
    if (r.flags & F_KEEP) note(L"If the screen goes wrong, wait 15 seconds and Breaker switches back.");
    if (IsSetting(r) && !(r.flags & F_NOHIST)) note(L"Every change is recorded and can be undone (Ctrl+Z).");
    return t;
}

// ============================================================ layout
static float ContentLeft() { return SideW() + kPad; }
static float ContentWidth() { return std::min(1000.0f, g_W - SideW() - 2 * kPad); }

static float ActionW(const RowAction& a) { return std::max(34.0f, MeasureWidth(a.label, g_fCenter) + 26); }

static float ControlW(const Row& r) {
    if (IsSetting(r) && r.unavailable) return 190;
    switch (r.ctl) {
        case CtlToggle: return 92;
        case CtlChoice: return std::min(270.0f, MeasureWidth(ChoiceText(r), g_fUi) + 46);
        case CtlSlider: return 232;
        case CtlText: return (r.wide ? 300.0f : 210.0f);
        case CtlLink: return 30 + (r.valueText.empty() ? 0 : std::min(200.0f, MeasureWidth(r.valueText, g_fRight) + 14));
        default: return r.valueText.empty() ? 0 : std::min(320.0f, MeasureWidth(r.valueText, g_fRight) + 8);
    }
}

static float CachedHeight(const std::wstring& text, IDWriteTextFormat* f, float width, wchar_t kind) {
    std::wstring key = text;
    key += L'\x1f';
    key += kind;
    key += std::to_wstring((int)width);
    auto it = g_measure.find(key);
    if (it != g_measure.end()) return it->second;
    if (g_measure.size() > 6000) g_measure.clear();
    float h = MeasureHeight(text, f, width);
    g_measure.emplace(std::move(key), h);
    return h;
}

static void Layout() {
    g_lay.clear();
    float left = ContentLeft(), cw = ContentWidth(), y = 6;
    const std::wstring* prev = nullptr;
    int cols = std::clamp((int)((cw + 8) / 250), 1, 4), col = 0;
    float tileW = (cw - (cols - 1) * 8) / cols, minH = Compact() ? 44.0f : 56.0f, padV = Compact() ? 9.0f : 13.0f;
    bool lastTile = false;
    float tileH = 78;
    for (int vi = 0; vi < (int)g_vis.size(); ++vi) {
        const Row& r = g_rows[g_vis[vi]];
        LRow L{};
        L.row = g_vis[vi];
        L.head = !prev || *prev != r.group;
        if (L.head) {
            if (lastTile && col != 0) y += tileH + 8;
            col = 0;
            if (!r.group.empty()) y += (prev ? 16.0f : 2.0f) + 26;
            else if (prev) y += 10;
        }
        prev = &r.group;
        if (r.tile) {
            int tcols = r.stat ? std::clamp((int)((cw + 8) / 200), 1, 3) : cols;
            float tw = r.stat ? (cw - (tcols - 1) * 8) / tcols : tileW;
            tileH = r.stat ? 100.0f : 78.0f;
            L.x = left + col * (tw + 8);
            L.y = y;
            L.w = tw;
            L.h = tileH;
            g_lay.push_back(L);
            lastTile = true;
            if (++col >= tcols) { col = 0; y += tileH + 8; }
            continue;
        }
        if (lastTile && col != 0) { y += tileH + 8; col = 0; }
        if (!r.chart.empty()) {
            L.x = left;
            L.y = y;
            L.w = cw;
            L.h = 230;
            lastTile = false;
            g_lay.push_back(L);
            y += L.h + kRowGap;
            continue;
        }
        lastTile = false;
        float actionsW = 0;
        for (auto& a : r.actions) actionsW += ActionW(a) + 8;
        float infoW = HasInfo(r) ? 34.0f : 0.0f;
        L.textW = std::max(150.0f, cw - 32 - ControlW(r) - actionsW - infoW - 14);
        L.descH = r.desc.empty() ? 0 : CachedHeight(r.desc, g_fDesc, L.textW, L'd');
        float h = padV * 2 + 19 + (L.descH > 0 ? L.descH + 3 : 0) + (r.where.empty() ? 0 : 17) + (r.bar >= 0 ? 12 : 0);
        L.h = std::max(minH, h);
        if (g_expanded == RowKey(r) && HasInfo(r)) {
            std::wstring pt = PanelText(r);
            L.panelH = 12 + (pt.empty() ? 0 : CachedHeight(pt, g_fMono, cw - 32, L'm') + 10) + (ChipsFor(r).empty() ? 0 : 30) + 12;
            L.h += L.panelH;
        }
        L.x = left;
        L.y = y;
        L.w = cw;
        g_lay.push_back(L);
        y += L.h + kRowGap;
    }
    if (lastTile && col != 0) y += tileH + 8;
    g_contentH = y + 36;
    g_layDirty = false;
}

static float ViewH() { return g_H - g_headerBottom; }
static void ClampScroll() {
    float maxS = std::max(0.0f, g_contentH - ViewH());
    g_scrollTo = std::clamp(g_scrollTo, 0.0f, maxS);
    g_scroll = std::clamp(g_scroll, 0.0f, maxS);
}
static void EnsureVisible(int vi) {
    if (g_layDirty) Layout();
    if (vi < 0 || vi >= (int)g_lay.size()) return;
    const LRow& L = g_lay[vi];
    float top = L.y - (L.head ? 44 : 6), bottom = L.y + L.h + 10;
    if (top < g_scrollTo) g_scrollTo = top;
    else if (bottom > g_scrollTo + ViewH()) g_scrollTo = bottom - ViewH();
    ClampScroll();
    StartAnim();
}

// ============================================================ painting
static void AddZone(ZoneKind k, int idx, const D2D1_RECT_F& r, int sub = 0) { g_zones.push_back({k, idx, sub, r}); }
static bool Hot(ZoneKind k, int idx, int sub = 0) { return g_hot.kind == k && g_hot.idx == idx && g_hot.sub == sub; }
static D2D1_COLOR_F Surface() { return Alpha(g_theme.surface, g_theme.backdrop ? 0.78f : 1.0f); }

static void AccAdd(const Zone& z, const std::wstring& name, long role, long state, const std::wstring& value = L"", const std::wstring& desc = L"", const wchar_t* action = L"") {
    if (!g_accActive) return;
    AccItem it;
    it.name = name;
    it.value = value;
    it.desc = desc;
    it.role = role;
    it.state = state;
    it.action = action;
    POINT a{(LONG)(z.r.left * g_scale), (LONG)(z.r.top * g_scale)}, b{(LONG)(z.r.right * g_scale), (LONG)(z.r.bottom * g_scale)};
    ClientToScreen(g_hwnd, &a);
    ClientToScreen(g_hwnd, &b);
    it.rc = {a.x, a.y, b.x, b.y};
    g_acc.push_back(std::move(it));
    g_accZones.push_back(z);
}

static void DrawPill(const D2D1_RECT_F& r, const std::wstring& text, bool hot, bool primary) {
    float rad = std::min(g_theme.radius, 6.0f);
    D2D1_COLOR_F bg = primary ? g_theme.accent : g_theme.surfaceStrong;
    if (hot) bg = Mix(bg, primary ? g_theme.onAccent : g_theme.text, 0.12f);
    g_cv.FillRound(r, rad, bg);
    if (!primary) g_cv.StrokeRound(r, rad, g_theme.divider);
    g_cv.Text(text, g_fCenter, r, primary ? g_theme.onAccent : g_theme.text);
}

static void DrawToggle(float x, float cy, const std::wstring& key, bool on, float opacity) {
    float& t = g_tog.try_emplace(key, on ? 1.0f : 0.0f).first->second;
    float target = on ? 1.0f : 0.0f;
    if (std::fabs(t - target) > 0.01f) { t += (target - t) * std::min(1.0f, AnimK() * 1.3f); g_togMoving = true; StartAnim(); } else t = target;
    D2D1_RECT_F track = R(x, cy - 10, x + 40, cy + 10);
    D2D1_COLOR_F offC = Alpha(g_theme.subtext, 0.0f), fill = Mix(offC, g_theme.accent, t);
    g_cv.FillRound(track, 10, Alpha(fill, opacity));
    if (t < 0.99f) g_cv.StrokeRound(track, 10, Alpha(g_theme.subtext, (1 - t) * 0.9f * opacity), 1.2f);
    g_cv.Circle(x + 10 + t * 20, cy, 6.0f + t * 0.5f, Alpha(Mix(g_theme.subtext, g_theme.onAccent, t), opacity));
}

static void DrawLogo(float x, float cy) {
    D2D1_RECT_F box = R(x, cy - 10, x + 20, cy + 10);
    g_cv.FillRound(box, 5, g_theme.accent);
    g_cv.FillRound(R(x + 8.5f, cy - 6, x + 11.5f, cy + 1), 1.5f, g_theme.onAccent);       // the breaker's lever
    g_cv.FillRound(R(x + 5, cy + 3, x + 15, cy + 5.5f), 1.2f, g_theme.onAccent);
}

static void PaintSidebar() {
    float sw = SideW();
    g_cv.Fill(R(0, 0, sw, g_H), Alpha(g_theme.bottomBg, g_theme.backdrop ? g_theme.opacity * 0.9f : 1.0f));
    g_cv.Fill(R(sw - 1, 0, sw, g_H), g_theme.divider);
    // back / forward + name
    D2D1_RECT_F back = R(8, 7, 38, 37), fwd = R(40, 7, 70, 37);
    if (Hot(ZBack, 0) && !g_back.empty()) g_cv.FillRound(back, 6, g_theme.hover);
    if (Hot(ZFwd, 0) && !g_fwd.empty()) g_cv.FillRound(fwd, 6, g_theme.hover);
    g_cv.Glyph(0xE72B, g_fIconSm, back, Alpha(g_theme.text, g_back.empty() ? 0.3f : 0.9f));
    g_cv.Glyph(0xE72A, g_fIconSm, fwd, Alpha(g_theme.text, g_fwd.empty() ? 0.3f : 0.9f));
    AddZone(ZBack, 0, back);
    AddZone(ZFwd, 0, fwd);
    AccAdd(g_zones[g_zones.size() - 2], L"Back", ROLE_SYSTEM_PUSHBUTTON, g_back.empty() ? STATE_SYSTEM_UNAVAILABLE : 0, L"", L"", L"Press");
    DrawLogo(80, 22);
    g_cv.Text(L"Breaker", g_fLogo, R(108, 8, sw - 8, 36), g_theme.text);

    // search
    D2D1_RECT_F box = R(12, 50, sw - 12, 84);
    bool focused = g_area == AreaSearch && !g_prompt.open;
    g_cv.FillRound(box, std::min(g_theme.radius, 8.0f), Alpha(g_theme.surface, g_theme.backdrop ? 0.7f : 1.0f));
    g_cv.StrokeRound(box, std::min(g_theme.radius, 8.0f), focused ? g_theme.accent : g_theme.divider, focused ? 1.6f : 1.0f);
    g_cv.Glyph(0xE721, g_fIconSm, R(box.left + 6, box.top, box.left + 32, box.bottom), g_theme.subtext);
    D2D1_RECT_F in = R(box.left + 34, box.top + 2, box.right - 8, box.bottom - 2);
    if (g_search.text.empty() && !focused) g_cv.Text(L"Search settings", g_fUi, in, g_theme.subtext);
    if (g_search.text.empty()) g_cv.Text(L"Ctrl+K", g_fRight, R(in.left, in.top, in.right - 2, in.bottom), Alpha(g_theme.subtext, 0.7f));
    g_search.Draw(g_cv, g_fInput, in, g_theme.text, Alpha(g_theme.accent, 0.35f), g_theme.text, focused);
    AddZone(ZSearch, 0, box);
    AccAdd(g_zones.back(), L"Search settings", ROLE_SYSTEM_TEXT, STATE_SYSTEM_FOCUSABLE | (focused ? STATE_SYSTEM_FOCUSED : 0), g_search.text);

    // categories; Breaker settings is pinned to the bottom
    auto cats = SideCats();
    std::wstring current = g_route == L"search" ? L"" : CategoryOf(g_route);
    float itemH = Compact() ? 32.0f : 36.0f, y = 94;
    float bottomY = g_H - itemH - 10;
    // Squeeze the rows a little rather than drop categories when the window is short
    if (cats.size() > 1) itemH = std::clamp((bottomY - 6 - y - 12) / (float)(cats.size() - 1), 24.0f, itemH);
    int prevGroup = 0;
    for (int i = 0; i < (int)cats.size(); ++i) {
        const Category* c = cats[i];
        bool last = c->group == 2;
        if (!last && c->group != prevGroup) { g_cv.Fill(R(16, y + 4, sw - 16, y + 5), g_theme.divider); y += 10; prevGroup = c->group; }
        float iy = last ? bottomY : y;
        if (!last && iy + itemH > bottomY - 6) continue;      // window too short: the rest stay reachable by search and keyboard
        D2D1_RECT_F r = R(8, iy, sw - 9, iy + itemH - 2);
        bool sel = current == c->id, hot = Hot(ZSide, i);
        if (sel) g_cv.FillRound(r, 6, Alpha(g_theme.text, g_theme.dark ? 0.09f : 0.07f));
        else if (hot) g_cv.FillRound(r, 6, g_theme.hover);
        if (sel) g_cv.FillRound(R(r.left, r.top + 9, r.left + 3, r.bottom - 9), 1.5f, g_theme.accent);
        if (g_kb && g_area == AreaSidebar && g_sideFocus == i) g_cv.StrokeRound(r, 6, g_theme.accent, 1.6f);
        g_cv.Glyph(c->glyph, g_fIcon, R(r.left + 8, r.top, r.left + 38, r.bottom), sel ? g_theme.accent : g_theme.text);
        g_cv.Text(c->name, sel ? g_fUiSemi : g_fUi, R(r.left + 44, r.top, r.right - 8, r.bottom), g_theme.text);
        if (wcscmp(c->id, L"watchdog") == 0 && !WatchdogDrift().empty()) g_cv.Circle(r.right - 14, (r.top + r.bottom) / 2, 4, g_theme.accent);
        if (wcscmp(c->id, L"settings") == 0 && AutoUpdateAvailable()) g_cv.Circle(r.right - 14, (r.top + r.bottom) / 2, 4, g_theme.accent);
        AddZone(ZSide, i, r);
        AccAdd(g_zones.back(), c->name, ROLE_SYSTEM_LISTITEM, STATE_SYSTEM_SELECTABLE | STATE_SYSTEM_FOCUSABLE | (sel ? STATE_SYSTEM_SELECTED : 0), L"", L"", L"Open");
        if (!last) y += itemH;
    }
}

static void PaintCaption() {
    bool active = GetForegroundWindow() == g_hwnd;
    struct { ZoneKind k; wchar_t g; const wchar_t* name; } btn[] = {{ZCapMin, 0xE921, L"Minimize"}, {ZCapMax, IsZoomed(g_hwnd) ? (wchar_t)0xE923 : (wchar_t)0xE922, L"Maximize"}, {ZCapClose, 0xE8BB, L"Close"}};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F r = R(g_W - (3 - i) * kCapW, 0, g_W - (2 - i) * kCapW, kTitleH - 12);
        bool hot = g_hot.kind == btn[i].k;
        if (hot) g_cv.Fill(r, btn[i].k == ZCapClose ? Rgb(0xC42B1C) : g_theme.hover);
        g_cv.Glyph(btn[i].g, g_fCap, r, hot && btn[i].k == ZCapClose ? Rgb(0xFFFFFF) : Alpha(g_theme.text, active ? 0.95f : 0.5f));
        AddZone(btn[i].k, 0, r);
        AccAdd(g_zones.back(), btn[i].name, ROLE_SYSTEM_PUSHBUTTON, 0, L"", L"", L"Press");
    }
}

static std::wstring PageTitle() {
    if (g_route == L"search") return L"Results for \x201C" + Trim(g_search.text) + L"\x201D";
    if (StartsWith(g_route, L"myapps/")) return g_route.substr(7);
    if (StartsWith(g_route, L"screentime/app/")) return TallyAppTitle(g_route.substr(15));
    const PageInfo* p = FindPage(g_route);
    return p ? p->title : L"";
}

// Title strip (breadcrumbs, Windows button, caption buttons), page title, blurb, tabs, filter box.
static void PaintHeader() {
    float left = ContentLeft(), cw = ContentWidth(), y = 0;
    const PageInfo* page = FindPage(g_route);
    std::wstring cat = CategoryOf(g_route);
    const Category* c = g_route == L"search" ? nullptr : FindCategory(cat);
    // breadcrumbs
    float x = left;
    if (c) {
        float w = MeasureWidth(c->name, g_fSmall) + 4;
        D2D1_RECT_F r = R(x, 12, x + w, 34);
        g_cv.Text(c->name, g_fSmall, r, Hot(ZCrumb, 0) ? g_theme.text : g_theme.subtext);
        AddZone(ZCrumb, 0, r);
        x += w;
        std::wstring title = PageTitle();
        if (title != c->name) {
            g_cv.Text(L"\x203A", g_fSmall, R(x + 2, 12, x + 14, 34), g_theme.subtext);
            g_cv.Text(title, g_fSmall, R(x + 16, 12, g_W - 3 * kCapW - 170, 34), g_theme.text);
        }
    }
    // "open the same page in Windows Settings"
    std::wstring wl = L"Windows Settings \x2197";
    float ww = MeasureWidth(wl, g_fSmall) + 18;
    D2D1_RECT_F wr = R(g_W - 3 * kCapW - ww - 10, 9, g_W - 3 * kCapW - 10, 33);
    if (Hot(ZWinSettings, 0)) g_cv.FillRound(wr, 6, g_theme.hover);
    g_cv.Text(wl, g_fSmall, R(wr.left + 9, wr.top, wr.right, wr.bottom), g_theme.subtext);
    AddZone(ZWinSettings, 0, wr);
    AccAdd(g_zones.back(), L"Open this page in Windows Settings", ROLE_SYSTEM_PUSHBUTTON, 0, L"", L"", L"Press");
    PaintCaption();

    y = kTitleH + 4;
    g_cv.Text(PageTitle(), g_fTitle, R(left, y, left + cw, y + 38), g_theme.text);
    y += 42;
    if (g_route == L"search") {
        std::wstring n = g_rows.empty() ? L"Nothing matches. Try another word, like \"dark\", \"startup\" or \"mouse\"." : std::to_wstring(g_rows.size()) + (g_rows.size() == 1 ? L" result" : L" results") + L". Change settings right here, or press Enter on one to see where it lives.";
        g_cv.Text(n, g_fDesc, R(left, y, left + cw, y + 34), g_theme.subtext);
        y += 26;
    } else if (page && page->blurb) {
        float bh = MeasureHeight(page->blurb, g_fDesc, cw);
        g_cv.Text(page->blurb, g_fDesc, R(left, y, left + cw, y + bh + 2), g_theme.subtext);
        y += bh + 10;
    }
    // tabs for the pages of this category
    auto tabs = (g_route == L"search" || StartsWith(g_route, L"myapps/") || StartsWith(g_route, L"screentime/app/")) ? std::vector<const PageInfo*>() : TabsFor(cat);
    if (tabs.size() > 1) {
        float tx = left;
        for (int i = 0; i < (int)tabs.size(); ++i) {
            float w = MeasureWidth(tabs[i]->title, g_fUi) + 24;
            if (tx + w > left + cw) { tx = left; y += 34; }
            D2D1_RECT_F r = R(tx, y, tx + w, y + 30);
            bool sel = g_route == tabs[i]->route;
            if (sel) g_cv.FillRound(r, 15, g_theme.accent);
            else if (Hot(ZTab, i)) g_cv.FillRound(r, 15, g_theme.hover);
            else g_cv.StrokeRound(r, 15, g_theme.divider);
            g_cv.Text(tabs[i]->title, g_fCenter, r, sel ? g_theme.onAccent : g_theme.text);
            AddZone(ZTab, i, r);
            AccAdd(g_zones.back(), tabs[i]->title, ROLE_SYSTEM_PAGETAB, STATE_SYSTEM_SELECTABLE | (sel ? STATE_SYSTEM_SELECTED : 0), L"", L"", L"Switch");
            tx += w + 6;
        }
        y += 40;
    }
    if (page && page->filter && g_route != L"search") {
        D2D1_RECT_F box = R(left, y, left + std::min(cw, 320.0f), y + 32);
        bool focused = g_area == AreaFilter && !g_prompt.open;
        g_cv.FillRound(box, 6, Surface());
        g_cv.StrokeRound(box, 6, focused ? g_theme.accent : g_theme.divider, focused ? 1.6f : 1.0f);
        g_cv.Glyph(0xE71C, g_fIconSm, R(box.left + 4, box.top, box.left + 30, box.bottom), g_theme.subtext);
        D2D1_RECT_F in = R(box.left + 32, box.top + 2, box.right - 8, box.bottom - 2);
        if (g_filter.text.empty() && !focused) g_cv.Text(L"Filter this list  (Ctrl+F)", g_fUi, in, g_theme.subtext);
        g_filter.Draw(g_cv, g_fInput, in, g_theme.text, Alpha(g_theme.accent, 0.35f), g_theme.text, focused);
        if (!g_filter.text.empty()) g_cv.Text(std::to_wstring(g_vis.size()) + L" shown", g_fSmall, R(box.right + 12, box.top, box.right + 160, box.bottom), g_theme.subtext);
        AddZone(ZFilter, 0, box);
        y += 42;
    }
    g_headerBottom = y + 2;
}

static void PaintStat(const LRow& L, int vi, float top) {
    const Row& r = g_rows[L.row];
    D2D1_RECT_F card = R(L.x, top, L.x + L.w, top + L.h);
    g_cv.FillRound(card, g_theme.radius, Surface());
    g_cv.StrokeRound(card, g_theme.radius, g_theme.divider);
    if (g_kb && g_area == AreaContent && g_focus == vi) g_cv.StrokeRound(card, g_theme.radius, g_theme.accent, 2);
    g_cv.Text(r.title, g_fSmall, R(card.left + 16, card.top + 12, card.right - 14, card.top + 30), g_theme.subtext);
    g_cv.Text(r.valueText, g_fTitle, R(card.left + 16, card.top + 32, card.right - 14, card.top + 68), g_theme.text);
    bool up = StartsWith(r.desc, L"\x25B2"), down = StartsWith(r.desc, L"\x25BC");
    g_cv.Text(r.desc, g_fSmall, R(card.left + 16, card.top + 70, card.right - 14, card.bottom - 8), up || down ? Alpha(g_theme.accent, 0.95f) : g_theme.subtext);
    AddZone(ZTile, vi, card);
    AccAdd(g_zones.back(), r.title, ROLE_SYSTEM_STATICTEXT, STATE_SYSTEM_FOCUSABLE | (g_focus == vi && g_area == AreaContent ? STATE_SYSTEM_FOCUSED : 0), r.valueText, r.desc);
}

// A row holding a bar chart: title top left, valueText top right, a scale, one bar per value, labels under,
// and the bar's own text when the pointer is over it.
static void PaintChart(const LRow& L, int vi, float top) {
    const Row& r = g_rows[L.row];
    D2D1_RECT_F card = R(L.x, top, L.x + L.w, top + L.h);
    g_cv.FillRound(card, g_theme.radius, Surface());
    if (!g_theme.dark) g_cv.StrokeRound(card, g_theme.radius, g_theme.divider);
    if (g_kb && g_area == AreaContent && g_focus == vi) g_cv.StrokeRound(card, g_theme.radius, g_theme.accent, 2);
    AddZone(ZRow, vi, card);
    g_cv.Text(r.title, g_fUiSemi, R(card.left + 16, card.top + 12, card.left + L.w / 2, card.top + 32), g_theme.text);
    if (!r.valueText.empty()) g_cv.Text(r.valueText, g_fRight, R(card.left + L.w / 2, card.top + 12, card.right - 16, card.top + 32), g_theme.subtext);
    float maxV = 0;
    for (float v : r.chart) maxV = std::max(maxV, v);
    // a round number for the top of the scale
    float scale = maxV <= 0.25f ? 0.25f : maxV <= 0.5f ? 0.5f : maxV <= 1 ? 1.0f : std::ceil(maxV);
    std::wstring scaleText = scale < 1 ? std::to_wstring((int)std::lround(scale * 60)) + L"m" : std::to_wstring((int)scale) + r.chartUnit;
    float left = card.left + 44, right = card.right - 16, bTop = card.top + 52, bBottom = card.bottom - 34;
    g_cv.Text(scaleText, g_fSmall, R(card.left + 12, bTop - 9, left - 6, bTop + 9), g_theme.subtext);
    g_cv.Text(L"0", g_fSmall, R(card.left + 12, bBottom - 9, left - 6, bBottom + 9), g_theme.subtext);
    g_cv.Fill(R(left, bTop, right, bTop + 1), Alpha(g_theme.divider, 0.8f));
    g_cv.Fill(R(left, (bTop + bBottom) / 2, right, (bTop + bBottom) / 2 + 1), Alpha(g_theme.divider, 0.5f));
    g_cv.Fill(R(left, bBottom, right, bBottom + 1), g_theme.divider);
    int n = (int)r.chart.size();
    float slot = (right - left) / std::max(1, n), bw = std::min(slot * 0.62f, 44.0f);
    int hot = -1;
    for (int i = 0; i < n; ++i) {
        float cx = left + slot * (i + 0.5f), h = scale > 0 ? (bBottom - bTop) * std::min(1.0f, r.chart[i] / scale) : 0;
        D2D1_RECT_F bar = R(cx - bw / 2, bBottom - std::max(h, 2.0f), cx + bw / 2, bBottom);
        bool isHot = Hot(ZBar, vi, i);
        if (isHot) hot = i;
        D2D1_COLOR_F c = h < 1 ? g_theme.divider : i == r.chartHi ? g_theme.accent : Alpha(g_theme.accent, isHot ? 0.75f : 0.45f);
        g_cv.FillRound(bar, std::min(4.0f, bw / 3), c);
        if (i < (int)r.chartLabels.size() && !r.chartLabels[i].empty())
            g_cv.Text(r.chartLabels[i], g_fCenter, R(cx - slot / 2 - 8, bBottom + 6, cx + slot / 2 + 8, bBottom + 26), i == r.chartHi ? g_theme.text : g_theme.subtext);
        AddZone(ZBar, vi, R(cx - slot / 2, bTop, cx + slot / 2, bBottom + 26), i);
    }
    if (hot >= 0 && hot < (int)r.chartTips.size()) {
        const std::wstring& tip = r.chartTips[hot];
        float tw = MeasureWidth(tip, g_fSmall) + 18, cx = left + slot * (hot + 0.5f);
        float tx = std::clamp(cx - tw / 2, card.left + 8, card.right - 8 - tw);
        float h = scale > 0 ? (bBottom - bTop) * std::min(1.0f, r.chart[hot] / scale) : 0;
        float ty = std::max(card.top + 6, bBottom - h - 30);
        D2D1_RECT_F tr = R(tx, ty, tx + tw, ty + 22);
        g_cv.FillRound(tr, 6, g_theme.dark ? Rgb(0x3A3A42) : Rgb(0xFFFFFF));
        g_cv.StrokeRound(tr, 6, g_theme.divider);
        g_cv.Text(tip, g_fSmall, R(tr.left + 9, tr.top, tr.right, tr.bottom), g_theme.dark ? Rgb(0xF5F5F7) : Rgb(0x1A1A1F));
    }
    std::wstring summary = r.title + L". " + r.valueText;
    for (auto& t : r.chartTips) summary += L". " + t;
    AccAdd(g_zones[g_zones.size() - n - 1], r.title, ROLE_SYSTEM_CHART, STATE_SYSTEM_FOCUSABLE | (g_focus == vi && g_area == AreaContent ? STATE_SYSTEM_FOCUSED : 0), r.valueText, summary);
}

static void PaintTile(const LRow& L, int vi, float top) {
    const Row& r = g_rows[L.row];
    if (r.stat) { PaintStat(L, vi, top); return; }
    D2D1_RECT_F card = R(L.x, top, L.x + L.w, top + L.h);
    bool hot = Hot(ZTile, vi), on = r.ctl == CtlToggle && r.value == L"1" && r.known;
    D2D1_COLOR_F bg = on ? Mix(Surface(), g_theme.accent, g_theme.dark ? 0.16f : 0.12f) : Surface();
    g_cv.FillRound(card, g_theme.radius, hot ? Mix(bg, g_theme.text, 0.06f) : bg);
    g_cv.StrokeRound(card, g_theme.radius, on ? Alpha(g_theme.accent, 0.55f) : g_theme.divider);
    if (g_kb && g_area == AreaContent && g_focus == vi) g_cv.StrokeRound(card, g_theme.radius, g_theme.accent, 2);
    g_cv.Text(r.title, g_fUiSemi, R(card.left + 14, card.top + 12, card.right - (r.ctl == CtlToggle ? 62 : 14), card.top + 34), g_theme.text);
    std::wstring state = !r.known ? L"\x2026" : r.unavailable ? L"Not available" : r.ctl == CtlToggle ? (on ? L"On" : L"Off") : r.ctl == CtlChoice ? ChoiceText(r) : r.value + r.unit;
    g_cv.Text(state, g_fSmall, R(card.left + 14, card.top + 40, card.right - 14, card.top + 62), on ? g_theme.accent : g_theme.subtext);
    if (r.ctl == CtlToggle) DrawToggle(card.right - 54, card.top + 24, r.id, on, r.known && !r.unavailable ? 1.0f : 0.4f);
    AddZone(ZTile, vi, card);
    AccAdd(g_zones.back(), r.title, r.ctl == CtlToggle ? ROLE_SYSTEM_CHECKBUTTON : ROLE_SYSTEM_PUSHBUTTON,
           STATE_SYSTEM_FOCUSABLE | (on ? STATE_SYSTEM_CHECKED : 0) | (g_focus == vi && g_area == AreaContent ? STATE_SYSTEM_FOCUSED : 0), state, r.desc, L"Toggle");
}

static void PaintRow(const LRow& L, int vi, float top) {
    const Row& r = g_rows[L.row];
    std::wstring key = RowKey(r);
    bool expanded = L.panelH > 0, setting = IsSetting(r);
    float bodyH = L.h - L.panelH, cy = top + bodyH / 2, rad = g_theme.radius;
    D2D1_RECT_F card = R(L.x, top, L.x + L.w, top + L.h), body = R(L.x, top, L.x + L.w, top + bodyH);
    bool rowHot = g_hot.idx == vi && (g_hot.kind == ZRow || g_hot.kind == ZToggle || g_hot.kind == ZChoice || g_hot.kind == ZSlider || g_hot.kind == ZText ||
                                      g_hot.kind == ZInfo || g_hot.kind == ZAction || g_hot.kind == ZChip);
    D2D1_COLOR_F bg = Surface();
    if (r.alert) bg = Mix(bg, g_theme.accent, 0.10f);
    if (rowHot && (setting || r.ctl == CtlLink)) bg = Mix(bg, g_theme.text, 0.045f);
    g_cv.FillRound(card, rad, bg);
    if (!g_theme.dark || r.alert) g_cv.StrokeRound(card, rad, r.alert ? Alpha(g_theme.accent, 0.5f) : g_theme.divider);
    if (g_flash > 0.01f && g_flashKey == key) g_cv.StrokeRound(card, rad, Alpha(g_theme.accent, g_flash), 2);
    if (g_kb && g_area == AreaContent && g_focus == vi) g_cv.StrokeRound(card, rad, g_theme.accent, 2);
    AddZone(ZRow, vi, body);
    size_t rowZone = g_zones.size() - 1;

    // ---- right side, laid out from the right edge inwards
    float x = card.right - 14;
    float opacity = setting && (!r.known || r.unavailable) ? 0.4f : 1.0f;
    if (HasInfo(r)) {
        D2D1_RECT_F ir = R(x - 28, cy - 14, x, cy + 14);
        if (Hot(ZInfo, vi) || expanded) g_cv.FillRound(ir, 6, g_theme.hover);
        g_cv.Glyph(0xE946, g_fIcon, ir, expanded ? g_theme.accent : g_theme.subtext);
        AddZone(ZInfo, vi, ir);
        AccAdd(g_zones.back(), L"Details for " + r.title, ROLE_SYSTEM_PUSHBUTTON, expanded ? STATE_SYSTEM_EXPANDED : STATE_SYSTEM_COLLAPSED, L"", L"", L"Expand");
        x -= 34;
    }
    std::wstring accValue;
    long accRole = ROLE_SYSTEM_STATICTEXT, accState = STATE_SYSTEM_FOCUSABLE;
    const wchar_t* accAction = L"";
    if (setting && r.unavailable) {
        D2D1_RECT_F tr = R(x - 190, top, x, top + bodyH);
        g_cv.Text(r.uri.empty() ? L"Not available on this PC" : L"Open in Windows \x2197", g_fRight, tr, r.uri.empty() ? g_theme.subtext : g_theme.accent);
        x -= 190;
        accValue = L"Not available on this PC";
    } else if (r.ctl == CtlToggle) {
        bool on = r.value == L"1";
        D2D1_RECT_F tr = R(x - 40, cy - 14, x, cy + 14);
        DrawToggle(x - 40, cy, key, on && r.known, opacity);
        g_cv.Text(!r.known ? L"" : on ? L"On" : L"Off", g_fRight, R(x - 92, top, x - 50, top + bodyH), g_theme.subtext);
        AddZone(ZToggle, vi, R(x - 92, tr.top, x, tr.bottom));
        x -= 92;
        accRole = ROLE_SYSTEM_CHECKBUTTON;
        accState |= on ? STATE_SYSTEM_CHECKED : 0;
        accValue = on ? L"On" : L"Off";
        accAction = L"Toggle";
    } else if (r.ctl == CtlChoice) {
        std::wstring label = r.known ? ChoiceText(r) : L"\x2026";
        float w = ControlW(r);
        D2D1_RECT_F cr = R(x - w, cy - 15, x, cy + 15);
        g_cv.FillRound(cr, std::min(rad, 6.0f), Hot(ZChoice, vi) ? Mix(g_theme.surfaceStrong, g_theme.text, 0.08f) : g_theme.surfaceStrong);
        g_cv.StrokeRound(cr, std::min(rad, 6.0f), g_theme.divider);
        g_cv.Text(label, g_fUi, R(cr.left + 12, cr.top, cr.right - 28, cr.bottom), Alpha(g_theme.text, opacity));
        g_cv.Glyph(0xE70D, g_fCap, R(cr.right - 26, cr.top, cr.right - 6, cr.bottom), g_theme.subtext);
        AddZone(ZChoice, vi, cr);
        x -= w;
        accRole = ROLE_SYSTEM_COMBOBOX;
        accValue = label;
        accAction = L"Choose";
    } else if (r.ctl == CtlSlider) {
        float v = g_dragKey == key ? g_dragValue : (float)_wtof(r.value.c_str());
        float frac = r.smax > r.smin ? std::clamp((v - r.smin) / (r.smax - r.smin), 0.0f, 1.0f) : 0;
        D2D1_RECT_F sr = R(x - 232, cy - 14, x - 62, cy + 14);
        float tx0 = sr.left + 8, tx1 = sr.right - 8, kx = tx0 + (tx1 - tx0) * frac;
        g_cv.FillRound(R(tx0, cy - 2, tx1, cy + 2), 2, Alpha(g_theme.subtext, 0.35f * opacity));
        g_cv.FillRound(R(tx0, cy - 2, kx, cy + 2), 2, Alpha(g_theme.accent, opacity));
        g_cv.Circle(kx, cy, (Hot(ZSlider, vi) || g_dragKey == key) ? 9.0f : 8.0f, Alpha(g_theme.accent, opacity));
        g_cv.Circle(kx, cy, 3.5f, Alpha(g_theme.onAccent, opacity));
        std::wstring vt = r.known ? std::to_wstring((int)std::lround(v)) + r.unit : L"\x2026";
        g_cv.Text(vt, g_fRight, R(x - 60, top, x, top + bodyH), g_theme.subtext);
        AddZone(ZSlider, vi, sr);
        x -= 232;
        accRole = ROLE_SYSTEM_SLIDER;
        accValue = vt;
    } else if (r.ctl == CtlText) {
        float w = ControlW(r);
        D2D1_RECT_F tr = R(x - w, cy - 15, x, cy + 15);
        g_cv.FillRound(tr, std::min(rad, 6.0f), Hot(ZText, vi) ? Mix(g_theme.surfaceStrong, g_theme.text, 0.08f) : g_theme.surfaceStrong);
        g_cv.StrokeRound(tr, std::min(rad, 6.0f), g_theme.divider);
        std::wstring shown = !r.known ? L"\x2026" : (r.value.empty() || r.value == L"~") ? L"(not set)" : r.value;
        float tl = tr.left + 12;
        if (r.flags & F_FILE) { size_t s = shown.find_last_of(L'\\'); if (s != std::wstring::npos) shown = shown.substr(s + 1); }
        if ((r.flags & F_COLOR) && r.known) { g_cv.Circle(tr.left + 17, cy, 7, Rgb(ParseHexColor(r.value, 0x808080))); tl += 20; }
        g_cv.Text(shown, g_fUi, R(tl, tr.top, tr.right - 30, tr.bottom), Alpha(g_theme.text, opacity));
        g_cv.Glyph((r.flags & F_FILE) ? 0xE838 : 0xE70F, g_fIconSm, R(tr.right - 28, tr.top, tr.right - 4, tr.bottom), g_theme.subtext);
        AddZone(ZText, vi, tr);
        x -= w;
        accRole = ROLE_SYSTEM_TEXT;
        accValue = shown;
        accAction = L"Edit";
    } else if (r.ctl == CtlLink) {
        bool inside = StartsWith(r.target, L"breaker:") || StartsWith(r.target, L"action:");
        g_cv.Glyph(inside ? 0xE76C : 0xE8A7, g_fIconSm, R(x - 26, top, x, top + bodyH), g_theme.subtext);
        x -= 30;
        if (!r.valueText.empty()) {
            float w = std::min(200.0f, MeasureWidth(r.valueText, g_fRight) + 14);
            g_cv.Text(r.valueText, g_fRight, R(x - w, top, x - 4, top + bodyH), g_theme.text);
            x -= w;
            accValue = r.valueText;
        }
        accRole = ROLE_SYSTEM_LINK;
        accAction = L"Open";
    } else if (!r.valueText.empty()) {
        float w = ControlW(r);
        g_cv.Text(r.valueText, g_fRight, R(x - w, top, x, top + bodyH), r.alert ? g_theme.accent : g_theme.subtext);
        x -= w;
        accValue = r.valueText;
    }
    x -= 6;
    for (int a = (int)r.actions.size() - 1; a >= 0; --a) {
        float w = ActionW(r.actions[a]);
        D2D1_RECT_F ar = R(x - w, cy - 14, x, cy + 14);
        DrawPill(ar, r.actions[a].label, Hot(ZAction, vi, a), r.alert && a == 0);
        AddZone(ZAction, vi, ar, a);
        AccAdd(g_zones.back(), r.actions[a].label + L", " + r.title, ROLE_SYSTEM_PUSHBUTTON, 0, L"", L"", L"Press");
        x -= w + 8;
    }

    // ---- left side: title (with shield and recommendation), description, where, usage bar
    float tx = card.left + 16, ty = top + (bodyH - (19 + (L.descH > 0 ? L.descH + 3 : 0) + (r.where.empty() ? 0 : 17) + (r.bar >= 0 ? 12 : 0))) / 2;
    float titleW = std::min(MeasureWidth(r.title, g_fUiSemi) + 2, std::max(60.0f, x - tx - 8));
    g_cv.Text(r.title, g_fUiSemi, R(tx, ty, tx + titleW, ty + 19), g_theme.text);
    float mx = tx + titleW + 8;
    if ((r.flags & F_ADMIN) && setting && mx + 18 < x) { g_cv.Glyph(0xEA18, g_fIconSm, R(mx - 2, ty, mx + 16, ty + 19), g_theme.subtext); mx += 22; }
    if (setting && g_config.showRecommended && !r.rec.empty() && r.known && !r.unavailable) {
        bool ok = r.value == r.rec;
        std::wstring t = ok ? L"\x2713 Recommended" : L"Recommended: " + (r.ctl == CtlToggle ? std::wstring(r.rec == L"1" ? L"On" : L"Off") : [&] { for (auto& c : r.choices) if (c.second == r.rec) return c.first; return r.rec; }());
        float w = MeasureWidth(t, g_fSmall) + 14;
        if (mx + w < x) {
            D2D1_RECT_F pr = R(mx, ty + 1, mx + w, ty + 18);
            g_cv.FillRound(pr, 8.5f, Alpha(ok ? g_theme.subtext : g_theme.accent, ok ? 0.12f : 0.18f));
            g_cv.Text(t, g_fSmall, R(pr.left + 7, pr.top, pr.right, pr.bottom), ok ? g_theme.subtext : g_theme.accent);
        }
    }
    float dy = ty + 21;
    if (L.descH > 0) { g_cv.Text(r.desc, g_fDesc, R(tx, dy, tx + L.textW, dy + L.descH + 2), g_theme.subtext); dy += L.descH + 3; }
    if (!r.where.empty()) { g_cv.Text(r.where, g_fSmall, R(tx, dy, tx + L.textW, dy + 16), Alpha(g_theme.accent, 0.9f)); dy += 17; }
    if (r.bar >= 0) {
        D2D1_RECT_F br = R(tx, dy + 3, tx + std::min(L.textW, 360.0f), dy + 9);
        g_cv.FillRound(br, 3, Alpha(g_theme.subtext, 0.25f));
        g_cv.FillRound(R(br.left, br.top, br.left + (br.right - br.left) * std::clamp(r.bar, 0.0f, 1.0f), br.bottom), 3, r.alert ? Rgb(0xE5484D) : g_theme.accent);
    }
    AccAdd(g_zones[rowZone], r.title, accRole, accState | (g_focus == vi && g_area == AreaContent ? STATE_SYSTEM_FOCUSED : 0), accValue,
           r.desc + (r.where.empty() ? L"" : L" In " + r.where), accAction);

    // ---- details panel
    if (expanded) {
        float py = top + bodyH;
        g_cv.Fill(R(card.left + 16, py, card.right - 16, py + 1), g_theme.divider);
        py += 12;
        std::wstring pt = PanelText(r);
        if (!pt.empty()) {
            float h = MeasureHeight(pt, g_fMono, L.w - 32);
            g_cv.Text(pt, g_fMono, R(card.left + 16, py, card.right - 16, py + h + 2), g_theme.subtext);
            py += h + 10;
        }
        float cx = card.left + 16;
        for (auto& chip : ChipsFor(r)) {
            float w = MeasureWidth(chip.second, g_fCenter) + 24;
            D2D1_RECT_F cr = R(cx, py, cx + w, py + 26);
            DrawPill(cr, chip.second, Hot(ZChip, vi, chip.first), chip.first == ChipRecommended);
            AddZone(ZChip, vi, cr, chip.first);
            AccAdd(g_zones.back(), chip.second + L", " + r.title, ROLE_SYSTEM_PUSHBUTTON, 0, L"", L"", L"Press");
            cx += w + 8;
        }
    }
}

static void PaintRows() {
    if (g_layDirty) Layout();
    ClampScroll();
    float viewTop = g_headerBottom, viewH = ViewH();
    float slide = (1 - g_pageAnim) * 10;
    g_rt->PushAxisAlignedClip(R(SideW(), viewTop, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    ID2D1Layer* layer = nullptr;
    if (g_pageAnim < 0.99f && SUCCEEDED(g_rt->CreateLayer(&layer)) && layer)
        g_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), g_pageAnim), layer);
    const std::wstring* prev = nullptr;
    for (int vi = 0; vi < (int)g_lay.size(); ++vi) {
        const LRow& L = g_lay[vi];
        const Row& r = g_rows[L.row];
        float top = viewTop + L.y - g_scroll + slide;
        bool head = L.head && !r.group.empty();
        prev = &r.group;
        if (top > g_H) break;
        if (top + L.h < viewTop - 40) continue;
        if (head) g_cv.Text(r.group, g_fGroup, R(r.tile ? ContentLeft() : L.x, top - 26, ContentLeft() + ContentWidth(), top - 4), g_theme.text);
        if (r.tile) PaintTile(L, vi, top);
        else if (!r.chart.empty()) PaintChart(L, vi, top);
        else PaintRow(L, vi, top);
    }
    (void)prev;
    if (g_vis.empty() && !g_filter.text.empty())
        g_cv.Text(L"Nothing on this page matches \x201C" + g_filter.text + L"\x201D", g_fUi, R(ContentLeft(), viewTop + 20, ContentLeft() + ContentWidth(), viewTop + 50), g_theme.subtext);
    if (layer) { g_rt->PopLayer(); layer->Release(); }
    // scrollbar
    if (g_contentH > viewH + 1) {
        float trackH = viewH - 12, th = std::max(36.0f, trackH * viewH / g_contentH);
        float ty = viewTop + 6 + (trackH - th) * (g_scroll / (g_contentH - viewH));
        bool hot = g_hot.kind == ZScroll || g_scrollGrab >= 0;
        D2D1_RECT_F thumb = R(g_W - (hot ? 10.0f : 7.0f), ty, g_W - 3, ty + th);
        g_cv.FillRound(thumb, 3, Alpha(g_theme.subtext, hot ? 0.7f : 0.4f));
        AddZone(ZScroll, 0, R(g_W - 14, viewTop, g_W, g_H));
    }
    g_rt->PopAxisAlignedClip();
}

static void PaintToast() {
    if (g_toast.text.empty()) return;
    std::wstring text = g_toast.text;
    if (!g_keepSeqs.empty() && g_keepUntil > GetTickCount()) text += L"  Going back in " + std::to_wstring((g_keepUntil - GetTickCount()) / 1000 + 1) + L" s";
    if (g_toast.busy) {
        static const wchar_t* dots[] = {L"", L" .", L" . .", L" . . ."};
        text += dots[(GetTickCount() / 350) % 4];
    }
    float aw = g_toast.action.empty() ? 0 : MeasureWidth(g_toast.action, g_fCenter) + 28;
    float maxW = g_W - SideW() - 60;
    float tw = std::min(MeasureWidth(text, g_fUi) + 4, maxW - aw - 40);
    float w = tw + aw + 36 + (aw > 0 ? 6 : 0), cx = SideW() + (g_W - SideW()) / 2;
    D2D1_RECT_F r = R(cx - w / 2, g_H - 62, cx + w / 2, g_H - 20);
    g_cv.FillRound(R(r.left, r.top + 2, r.right, r.bottom + 3), 10, Rgb(0x000000, 0.22f));
    g_cv.FillRound(r, 10, g_theme.dark ? Rgb(0x3A3A42) : Rgb(0xFFFFFF));
    g_cv.StrokeRound(r, 10, g_theme.divider);
    g_cv.Text(text, g_fUi, R(r.left + 18, r.top, r.left + 18 + tw, r.bottom), g_theme.dark ? Rgb(0xF5F5F7) : Rgb(0x1A1A1F));
    if (aw > 0) {
        D2D1_RECT_F ar = R(r.right - aw - 8, r.top + 7, r.right - 8, r.bottom - 7);
        DrawPill(ar, g_toast.action, Hot(ZToastAction, 0), true);
        AddZone(ZToastAction, 0, ar);
        AccAdd(g_zones.back(), g_toast.action, ROLE_SYSTEM_PUSHBUTTON, 0, L"", text, L"Press");
    }
}

static void PaintPrompt() {
    if (!g_prompt.open) return;
    g_cv.Fill(R(0, 0, g_W, g_H), g_theme.scrim);
    float w = std::min(480.0f, g_W - 40), h = 74 + g_prompt.fields.size() * 66 + 56;
    D2D1_RECT_F card = R((g_W - w) / 2, (g_H - h) / 2, (g_W + w) / 2, (g_H + h) / 2);
    g_cv.FillRound(card, std::max(g_theme.radius, 4.0f), g_theme.dark ? Mix(g_theme.surface, Rgb(0x000000), 0.1f) : Rgb(0xFFFFFF));
    g_cv.StrokeRound(card, std::max(g_theme.radius, 4.0f), g_theme.divider);
    g_cv.Text(g_prompt.title, g_fLogo, R(card.left + 22, card.top + 16, card.right - 22, card.top + 46), g_theme.text);
    float y = card.top + 56;
    for (int i = 0; i < (int)g_prompt.fields.size(); ++i) {
        g_cv.Text(g_prompt.labels[i], g_fSmall, R(card.left + 22, y, card.right - 22, y + 18), g_theme.subtext);
        D2D1_RECT_F box = R(card.left + 22, y + 22, card.right - 22, y + 56);
        bool focused = g_prompt.active == i;
        g_cv.FillRound(box, 6, g_theme.surfaceStrong);
        g_cv.StrokeRound(box, 6, focused ? g_theme.accent : g_theme.divider, focused ? 1.6f : 1.0f);
        g_prompt.fields[i].Draw(g_cv, g_fInput, R(box.left + 10, box.top + 2, box.right - 10, box.bottom - 2), g_theme.text, Alpha(g_theme.accent, 0.35f), g_theme.text, focused);
        AddZone(ZPromptField, i, box);
        y += 66;
    }
    D2D1_RECT_F ok = R(card.right - 22 - 96, card.bottom - 48, card.right - 22, card.bottom - 18), cancel = R(ok.left - 104, ok.top, ok.left - 8, ok.bottom);
    DrawPill(cancel, L"Cancel", Hot(ZPromptCancel, 0), false);
    DrawPill(ok, L"OK", Hot(ZPromptOk, 0), true);
    AddZone(ZPromptCancel, 0, cancel);
    AddZone(ZPromptOk, 0, ok);
}

static void Paint() {
    if (!EnsureDevice()) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_W = Px2Dip(rc.right);
    g_H = Px2Dip(rc.bottom);
    g_zones.clear();
    g_acc.clear();
    g_accZones.clear();
    g_rt->BeginDraw();
    g_rt->Clear(g_theme.backdrop ? D2D1::ColorF(0, 0, 0, 0) : g_theme.bg);
    if (g_theme.backdrop) g_cv.Fill(R(SideW(), 0, g_W, g_H), Alpha(g_theme.bg, g_theme.opacity * 0.72f));
    PaintHeader();
    PaintRows();
    PaintSidebar();
    PaintToast();
    if (g_prompt.open) { g_zones.clear(); PaintPrompt(); }
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { DiscardDevice(); Redraw(); }
}

// ============================================================ accessibility hooks
const std::vector<AccItem>& AccItems() { return g_acc; }

int AccFocusedItem() {
    for (int i = 0; i < (int)g_accZones.size(); ++i) {
        const Zone& z = g_accZones[i];
        if (g_area == AreaContent && (z.kind == ZRow || z.kind == ZTile) && z.idx == g_focus) return i;
        if (g_area == AreaSidebar && z.kind == ZSide && z.idx == g_sideFocus) return i;
        if (g_area == AreaSearch && z.kind == ZSearch) return i;
    }
    return -1;
}

static void ClickZone(const Zone& z, float x, float y, bool keyboard);
void AccInvoke(int index) {
    if (index < 0 || index >= (int)g_accZones.size()) return;
    Zone z = g_accZones[index];
    ClickZone(z, (z.r.left + z.r.right) / 2, (z.r.top + z.r.bottom) / 2, true);
}

// ============================================================ input
static Zone HitZone(float x, float y) {
    for (auto it = g_zones.rbegin(); it != g_zones.rend(); ++it) if (In(it->r, x, y)) return *it;
    return {};
}

static void FocusArea(int area) {
    g_area = area;
    if (area == AreaSearch || area == AreaFilter) SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    Redraw();
}

static void OpenWindowsPage() {
    std::wstring uri = L"ms-settings:";
    if (g_focus >= 0 && g_focus < (int)g_vis.size() && !g_rows[g_vis[g_focus]].uri.empty()) uri = g_rows[g_vis[g_focus]].uri;
    else for (auto& r : g_rows) if (StartsWith(r.uri, L"ms-settings:")) { uri = r.uri; break; }
    OpenTarget(uri);
}

static void RowMenu(int vi) {
    if (vi < 0 || vi >= (int)g_vis.size()) return;
    Row r = g_rows[g_vis[vi]];
    auto chips = ChipsFor(r);
    std::vector<std::pair<std::wstring, bool>> items;
    if (HasInfo(r)) items.push_back({g_expanded == RowKey(r) ? L"Hide details" : L"Show details", false});
    for (auto& c : chips) items.push_back({c.second, false});
    if (items.empty()) return;
    int pick = PopupMenuAt(items, nullptr);
    if (pick < 0) return;
    if (HasInfo(r)) {
        if (pick == 0) { g_expanded = g_expanded == RowKey(r) ? L"" : RowKey(r); g_layDirty = true; Redraw(); return; }
        --pick;
    }
    if (pick < (int)chips.size()) DoChip(r, chips[pick].first, nullptr);
}

static void ClickZone(const Zone& z, float x, float y, bool keyboard) {
    (void)y;
    if (z.kind >= ZRow && z.kind <= ZChip && (z.idx < 0 || z.idx >= (int)g_vis.size())) return;
    switch (z.kind) {
        case ZBack: GoBack(); break;
        case ZFwd: GoForward(); break;
        case ZSearch: FocusArea(AreaSearch); if (!keyboard) { g_search.caret = g_search.anchor = g_search.HitTest(g_fInput, R(z.r.left + 34, z.r.top, z.r.right - 8, z.r.bottom), x); } break;
        case ZFilter: FocusArea(AreaFilter); break;
        case ZSide: {
            auto cats = SideCats();
            if (z.idx < (int)cats.size()) { g_sideFocus = z.idx; GoTo(cats[z.idx]->id, true); }
            break;
        }
        case ZTab: {
            auto tabs = TabsFor(CategoryOf(g_route));
            if (z.idx < (int)tabs.size()) GoTo(tabs[z.idx]->route, true);
            break;
        }
        case ZCrumb: GoTo(CategoryOf(g_route), true); break;
        case ZWinSettings: OpenWindowsPage(); break;
        case ZCapMin: ShowWindow(g_hwnd, SW_MINIMIZE); break;
        case ZCapMax: ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case ZCapClose: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
        case ZTile: g_focus = z.idx; g_area = AreaContent; ActivateRow(z.idx); break;
        case ZRow: {
            g_focus = z.idx;
            g_area = AreaContent;
            const Row& r = g_rows[g_vis[z.idx]];
            if (g_route == L"search" && IsSetting(r) && !keyboard) { std::wstring id = r.id; UiNavigate(L"", id); }   // jump to where it lives
            else if (r.ctl == CtlToggle || r.ctl == CtlLink || keyboard) ActivateRow(z.idx);
            Redraw();
            break;
        }
        case ZToggle: g_focus = z.idx; ActivateRow(z.idx); break;
        case ZChoice: g_focus = z.idx; OpenChoiceMenu(z.idx, &z.r); break;
        case ZText: g_focus = z.idx; EditText(z.idx); break;
        case ZInfo: {
            std::wstring key = RowKey(g_rows[g_vis[z.idx]]);
            g_expanded = g_expanded == key ? L"" : key;
            g_focus = z.idx;
            g_layDirty = true;
            EnsureVisible(z.idx);
            Redraw();
            break;
        }
        case ZAction: {
            Row& r = g_rows[g_vis[z.idx]];
            if (z.sub < (int)r.actions.size()) { auto fn = r.actions[z.sub].run; fn(); }
            break;
        }
        case ZChip: { Row copy = g_rows[g_vis[z.idx]]; DoChip(copy, z.sub, &z.r); break; }
        case ZToastAction: {
            auto fn = g_toast.run;
            g_toast = {};
            if (fn) fn();
            Redraw();
            break;
        }
        case ZPromptOk: ClosePrompt(true); break;
        case ZPromptCancel: ClosePrompt(false); break;
        case ZPromptField: g_prompt.active = z.idx; Redraw(); break;
        default: break;
    }
}

static float SliderValueAt(const Row& r, const D2D1_RECT_F& zr, float x) {
    float frac = std::clamp((x - (zr.left + 8)) / std::max(1.0f, zr.right - zr.left - 16), 0.0f, 1.0f);
    float v = r.smin + frac * (r.smax - r.smin);
    return r.sstep > 0 ? std::clamp(r.smin + std::round((v - r.smin) / r.sstep) * r.sstep, r.smin, r.smax) : v;
}

static void OnLButtonDown(float x, float y) {
    g_kb = false;
    Zone z = HitZone(x, y);
    if (g_prompt.open && z.kind == ZNone) return;
    if (z.kind != ZSearch && z.kind != ZFilter && (g_area == AreaSearch || g_area == AreaFilter)) g_area = AreaContent;
    if (z.kind == ZSlider) {
        Row& r = g_rows[g_vis[z.idx]];
        if (!r.known || r.unavailable) return;
        g_focus = z.idx;
        g_dragKey = RowKey(r);
        g_dragValue = SliderValueAt(r, z.r, x);
        SetCapture(g_hwnd);
        Redraw();
        return;
    }
    if (z.kind == ZScroll) {
        g_scrollGrab = y;
        SetCapture(g_hwnd);
        return;
    }
    ClickZone(z, x, y, false);
}

static void OnMouseMove(float x, float y) {
    if (!g_dragKey.empty()) {
        for (auto& z : g_zones) if (z.kind == ZSlider && RowKey(g_rows[g_vis[z.idx]]) == g_dragKey) g_dragValue = SliderValueAt(g_rows[g_vis[z.idx]], z.r, x);
        Redraw();
        return;
    }
    if (g_scrollGrab >= 0) {
        float viewH = ViewH(), trackH = viewH - 12, th = std::max(36.0f, trackH * viewH / g_contentH);
        g_scrollTo = g_scroll = g_scroll + (y - g_scrollGrab) * (g_contentH - viewH) / std::max(1.0f, trackH - th);
        g_scrollGrab = y;
        ClampScroll();
        Redraw();
        return;
    }
    Zone z = HitZone(x, y);
    if (z.kind != g_hot.kind || z.idx != g_hot.idx || z.sub != g_hot.sub) { g_hot = z; Redraw(); }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g_hwnd, 0};
    TrackMouseEvent(&tme);
}

static void OnLButtonUp(float, float) {
    if (!g_dragKey.empty()) {
        std::wstring key = g_dragKey;
        g_dragKey.clear();
        ReleaseCapture();
        for (auto& r : g_rows)
            if (RowKey(r) == key && (int)std::lround(g_dragValue) != _wtoi(r.value.c_str())) { UiApply({{r.id, std::to_wstring((int)std::lround(g_dragValue)), r.label}}); break; }
        Redraw();
    }
    if (g_scrollGrab >= 0) { g_scrollGrab = -1; ReleaseCapture(); Redraw(); }
}

static void OnRightClick(float x, float y) {
    Zone z = HitZone(x, y);
    if (z.kind == ZRow || z.kind == ZTile || z.kind == ZToggle || z.kind == ZChoice || z.kind == ZText || z.kind == ZSlider) { g_focus = z.idx; RowMenu(z.idx); }
}

static void OnWheel(int delta) {
    if (g_prompt.open) return;
    g_scrollTo -= delta / 120.0f * 84;
    ClampScroll();
    if (g_config.animation == L"none") g_scroll = g_scrollTo;
    StartAnim();
    Redraw();
}

static void MoveFocus(int delta) {
    if (g_vis.empty()) return;
    g_focus = std::clamp(g_focus < 0 ? (delta > 0 ? 0 : (int)g_vis.size() - 1) : g_focus + delta, 0, (int)g_vis.size() - 1);
    EnsureVisible(g_focus);
    if (g_accActive) { Paint(); AccNotifyFocus(g_hwnd, AccFocusedItem()); }
    Redraw();
}

static void SwitchTab(int delta) {
    auto tabs = TabsFor(CategoryOf(g_route));
    if (tabs.size() < 2) return;
    int cur = 0;
    for (int i = 0; i < (int)tabs.size(); ++i) if (g_route == tabs[i]->route) cur = i;
    GoTo(tabs[(cur + delta + tabs.size()) % tabs.size()]->route, true);
}

static void UndoLast() {
    auto rep = std::make_shared<ApplyReport>();
    UiBusy(L"", [rep] { *rep = HistoryUndoLast(); }, [rep] {
        UiToast(rep->ok ? L"Last change undone" : (rep->error.empty() ? L"Nothing to undo" : rep->error));
        BuildPage(true);
        WatchdogRefresh(false);
    });
}

static bool OnInputKey(TextInput& in, UINT vk) {
    bool handled = false;
    bool changed = in.OnKey(g_hwnd, vk, handled);
    if (changed) { if (&in == &g_search) OnSearchChanged(); else if (&in == &g_filter) { ApplyFilter(); g_scroll = g_scrollTo = 0; } }
    if (handled) Redraw();
    return handled;
}

static void OnKeyDown(UINT vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    g_kb = true;
    if (g_prompt.open) {
        if (vk == VK_ESCAPE) ClosePrompt(false);
        else if (vk == VK_RETURN) ClosePrompt(true);
        else if (vk == VK_TAB) { g_prompt.active = (g_prompt.active + (shift ? (int)g_prompt.fields.size() - 1 : 1)) % (int)g_prompt.fields.size(); Redraw(); }
        else { bool handled = false; g_prompt.fields[g_prompt.active].OnKey(g_hwnd, vk, handled); Redraw(); }
        return;
    }
    // global shortcuts
    if (ctrl && vk == 'K') { FocusArea(AreaSearch); g_search.SelectAll(); return; }
    if (ctrl && vk == 'F') { const PageInfo* p = FindPage(g_route); if (p && p->filter) { FocusArea(AreaFilter); g_filter.SelectAll(); } else { FocusArea(AreaSearch); g_search.SelectAll(); } return; }
    if (ctrl && vk == 'Z' && g_area != AreaSearch && g_area != AreaFilter) { UndoLast(); return; }
    if (ctrl && vk == VK_OEM_COMMA) { GoTo(L"settings", true); return; }
    if (ctrl && vk == VK_TAB) { SwitchTab(shift ? -1 : 1); return; }
    if (ctrl && (vk == VK_NEXT || vk == VK_PRIOR)) { SwitchTab(vk == VK_NEXT ? 1 : -1); return; }
    if (alt && vk == VK_LEFT) { GoBack(); return; }
    if (alt && vk == VK_RIGHT) { GoForward(); return; }
    if (alt && vk == VK_HOME) { GoTo(L"home", true); return; }
    if (vk == VK_F5) { BuildPage(true); return; }
    if (vk == VK_BROWSER_BACK) { GoBack(); return; }
    if (vk == VK_BROWSER_FORWARD) { GoForward(); return; }

    if (g_area == AreaSearch || g_area == AreaFilter) {
        TextInput& in = g_area == AreaSearch ? g_search : g_filter;
        if (vk == VK_ESCAPE) {
            if (!in.text.empty()) { in.SetText(L""); if (g_area == AreaSearch) OnSearchChanged(); else ApplyFilter(); }
            g_area = AreaContent;
            Redraw();
        } else if (vk == VK_DOWN || vk == VK_RETURN || vk == VK_TAB) {
            g_area = AreaContent;
            if (!g_vis.empty()) { g_focus = 0; EnsureVisible(0); }
            Redraw();
        } else OnInputKey(in, vk);
        return;
    }
    if (vk == VK_ESCAPE) {
        if (!g_toast.text.empty() && !g_toast.busy && g_keepSeqs.empty()) { g_toast = {}; Redraw(); }
        else if (!g_expanded.empty()) { g_expanded.clear(); g_layDirty = true; Redraw(); }
        else GoBack();
        return;
    }
    if (vk == VK_BACK) { GoBack(); return; }
    if (vk == VK_TAB) {
        g_area = g_area == AreaSidebar ? AreaContent : AreaSidebar;
        if (g_area == AreaContent && g_focus < 0 && !g_vis.empty()) g_focus = 0;
        if (g_area == AreaSidebar) {
            auto cats = SideCats();
            for (int i = 0; i < (int)cats.size(); ++i) if (CategoryOf(g_route) == cats[i]->id) g_sideFocus = i;
        }
        Redraw();
        return;
    }
    if (g_area == AreaSidebar) {
        auto cats = SideCats();
        if (vk == VK_UP) g_sideFocus = std::max(0, g_sideFocus - 1);
        else if (vk == VK_DOWN) g_sideFocus = std::min((int)cats.size() - 1, g_sideFocus + 1);
        else if (vk == VK_HOME) g_sideFocus = 0;
        else if (vk == VK_END) g_sideFocus = (int)cats.size() - 1;
        else if (vk == VK_RETURN || vk == VK_SPACE || vk == VK_RIGHT) {
            if (g_sideFocus < (int)cats.size()) GoTo(cats[g_sideFocus]->id, true);
            g_area = AreaContent;
            if (!g_vis.empty()) g_focus = 0;
        }
        if (g_accActive) { Paint(); AccNotifyFocus(g_hwnd, AccFocusedItem()); }
        Redraw();
        return;
    }
    // content
    bool haveRow = g_focus >= 0 && g_focus < (int)g_vis.size();
    switch (vk) {
        case VK_DOWN: MoveFocus(1); break;
        case VK_UP: if (g_focus <= 0) { FocusArea(AreaSearch); } else MoveFocus(-1); break;
        case VK_NEXT: g_scrollTo += ViewH() - 60; ClampScroll(); StartAnim(); break;
        case VK_PRIOR: g_scrollTo -= ViewH() - 60; ClampScroll(); StartAnim(); break;
        case VK_HOME: g_focus = -1; MoveFocus(1); break;
        case VK_END: g_focus = (int)g_vis.size(); MoveFocus(-1); break;
        case VK_LEFT:
            if (haveRow && g_rows[g_vis[g_focus]].ctl == CtlSlider) SliderStep(g_focus, -1);
            else { g_area = AreaSidebar; Redraw(); }
            break;
        case VK_RIGHT: if (haveRow && g_rows[g_vis[g_focus]].ctl == CtlSlider) SliderStep(g_focus, 1); break;
        case VK_RETURN:
        case VK_SPACE:
            if (haveRow) {
                if (alt) { g_expanded = g_expanded == RowKey(g_rows[g_vis[g_focus]]) ? L"" : RowKey(g_rows[g_vis[g_focus]]); g_layDirty = true; EnsureVisible(g_focus); Redraw(); }
                else if (g_route == L"search" && vk == VK_RETURN && shift) { std::wstring id = g_rows[g_vis[g_focus]].id; UiNavigate(L"", id); }
                else ActivateRow(g_focus);
            }
            break;
        case VK_F1:
            if (haveRow && HasInfo(g_rows[g_vis[g_focus]])) { g_expanded = g_expanded == RowKey(g_rows[g_vis[g_focus]]) ? L"" : RowKey(g_rows[g_vis[g_focus]]); g_layDirty = true; EnsureVisible(g_focus); Redraw(); }
            break;
        case VK_APPS: if (haveRow) RowMenu(g_focus); break;
        case VK_F10: if (shift && haveRow) RowMenu(g_focus); break;
        default: break;
    }
}

static void OnChar(wchar_t ch) {
    if (g_prompt.open) {
        if (ch >= 32) { g_prompt.fields[g_prompt.active].OnChar(ch); Redraw(); }
        return;
    }
    if (ch < 32 || ch == 127) return;
    if (g_area == AreaFilter) { if (g_filter.OnChar(ch)) { ApplyFilter(); g_scroll = g_scrollTo = 0; Redraw(); } return; }
    if (g_area != AreaSearch) {
        if (ch == L' ') return;                 // Space activates the focused row
        g_area = AreaSearch;                    // start typing anywhere to search
        g_search.SetText(L"");
        SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    }
    if (g_search.OnChar(ch)) OnSearchChanged();
    Redraw();
}

// ============================================================ timers
static void OnAnimTimer() {
    bool more = false;
    float k = AnimK();
    if (std::fabs(g_scroll - g_scrollTo) > 0.5f) { g_scroll += (g_scrollTo - g_scroll) * std::min(1.0f, k * 1.25f); more = true; } else g_scroll = g_scrollTo;
    if (g_pageAnim < 1) { g_pageAnim = std::min(1.0f, g_pageAnim + std::max(0.07f, k * 0.5f)); more = true; }
    if (g_flash > 0.01f) { g_flash -= 0.012f; more = true; } else g_flash = 0;
    if (g_togMoving) { more = true; g_togMoving = false; }
    if (g_toast.busy) more = true;
    if (!more) KillTimer(g_hwnd, TIMER_ANIM);
    Redraw();
}

static void OnToastTimer() {
    DWORD now = GetTickCount();
    if (!g_keepSeqs.empty()) {
        if (now >= g_keepUntil) {
            auto seqs = g_keepSeqs;
            g_keepSeqs.clear();
            g_toast = {};
            UndoSeqs(seqs);
        }
        Redraw();
        return;
    }
    if (g_toast.text.empty()) { KillTimer(g_hwnd, TIMER_TOAST); return; }
    if (!g_toast.busy && now >= g_toast.until) { g_toast = {}; KillTimer(g_hwnd, TIMER_TOAST); Redraw(); }
}

// ============================================================ window plumbing
static int FrameThickness() { return GetSystemMetricsForDpi(SM_CXFRAME, g_dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g_dpi); }

static LRESULT HitTestNC(HWND hwnd, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd, &pt);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int b = (int)(kResize * g_scale);
    if (!IsZoomed(hwnd)) {
        bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, bo = pt.y >= rc.bottom - b;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (bo && l) return HTBOTTOMLEFT;
        if (bo && r) return HTBOTTOMRIGHT;
        if (t) return HTTOP;
        if (bo) return HTBOTTOM;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
    }
    float x = Px2Dip(pt.x), y = Px2Dip(pt.y);
    if (g_prompt.open) return HTCLIENT;
    if (y < kTitleH) {
        if (x >= g_W - 2 * kCapW && x < g_W - kCapW && y < kTitleH - 12) return HTMAXBUTTON;   // Windows 11 snap layouts
        Zone z = HitZone(x, y);
        return z.kind == ZNone ? HTCAPTION : HTCLIENT;
    }
    return HTCLIENT;
}

static void SaveWindowPlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    g_config.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    g_config.winX = wp.rcNormalPosition.left;
    g_config.winY = wp.rcNormalPosition.top;
    g_config.winW = (int)((wp.rcNormalPosition.right - wp.rcNormalPosition.left) / g_scale);
    g_config.winH = (int)((wp.rcNormalPosition.bottom - wp.rcNormalPosition.top) / g_scale);
}

static void ReleaseForUpdate() {
    SaveWindowPlacement();
    ConfigSave();
    CacheSave();
    TrayRemove();
    WinIHookSet(g_hwnd, false);
    ReleaseInstanceMutex();
}

static void ShowMain() {
    ForceForeground(g_hwnd);
    BuildPage(true);       // the PC may have changed while Breaker sat in the tray
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                if (IsZoomed(hwnd)) {
                    int f = FrameThickness();
                    p->rgrc[0].left += f;
                    p->rgrc[0].top += f;
                    p->rgrc[0].right -= f;
                    p->rgrc[0].bottom -= f;
                    APPBARDATA abd{sizeof(abd)};
                    if (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) p->rgrc[0].bottom -= 1;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            LRESULT hit = HitTestNC(hwnd, lp);
            if (hit == HTMAXBUTTON && g_hot.kind != ZCapMax) { g_hot = {}; g_hot.kind = ZCapMax; Redraw(); }
            else if (hit != HTMAXBUTTON && hit != HTCLIENT && g_hot.kind == ZCapMax) { g_hot = {}; Redraw(); }
            return hit;
        }
        case WM_NCMOUSEMOVE:
            if (wp == HTMAXBUTTON) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            break;
        case WM_NCMOUSELEAVE:
            if (g_hot.kind == ZCapMax) { g_hot = {}; Redraw(); }
            break;
        case WM_NCLBUTTONDOWN:
            if (wp == HTMAXBUTTON) return 0;
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON) { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
            break;
        case WM_NCACTIVATE:
            Redraw();
            return DefWindowProcW(hwnd, msg, wp, -1);
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && g_hwnd && IsWindowVisible(hwnd)) {     // pick up changes made elsewhere
                static DWORD last = 0;
                if (GetTickCount() - last > 3000) { last = GetTickCount(); RefreshValues(); }
            }
            Redraw();
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {(LONG)(860 * g_scale), (LONG)(520 * g_scale)};
            return 0;
        }
        case WM_SIZE:
            if (g_rt) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_rt->Resize(D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
            }
            g_layDirty = true;
            Redraw();
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            g_scale = g_dpi / 96.0f;
            if (g_rt) g_rt->SetDpi((float)g_dpi, (float)g_dpi);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            g_layDirty = true;
            Redraw();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_GETOBJECT:
            if ((LONG)lp == OBJID_CLIENT) {
                if (!g_accActive) { g_accActive = true; Paint(); }
                return AccOnGetObject(hwnd, wp, lp);
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                bool text = g_hot.kind == ZSearch || g_hot.kind == ZFilter || g_hot.kind == ZPromptField;
                SetCursor(LoadCursorW(nullptr, text ? IDC_IBEAM : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: OnMouseMove(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_MOUSELEAVE:
            if (g_hot.kind != ZNone && g_hot.kind != ZCapMax) { g_hot = {}; Redraw(); }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: OnLButtonDown(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_LBUTTONUP: OnLButtonUp(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_RBUTTONUP: OnRightClick(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) GoBack(); else GoForward();
            return TRUE;
        case WM_MOUSEWHEEL: OnWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
        case WM_CAPTURECHANGED:
            g_dragKey.clear();
            g_scrollGrab = -1;
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && (wp == VK_F4 || wp == VK_SPACE)) break;
            if (msg == WM_SYSKEYDOWN && wp != VK_LEFT && wp != VK_RIGHT && wp != VK_HOME && wp != VK_RETURN && wp != VK_F10) break;
            OnKeyDown((UINT)wp);
            return 0;
        case WM_SYSCHAR:
            return 0;
        case WM_CHAR: OnChar((wchar_t)wp); return 0;
        case WM_TIMER:
            switch (wp) {
                case TIMER_ANIM: OnAnimTimer(); break;
                case TIMER_CARET: {
                    bool input = g_prompt.open || g_area == AreaSearch || g_area == AreaFilter;
                    if (!input) { KillTimer(hwnd, TIMER_CARET); break; }
                    TextInput& in = g_prompt.open ? g_prompt.fields[g_prompt.active] : (g_area == AreaSearch ? g_search : g_filter);
                    in.caretOn = !in.caretOn;
                    Redraw();
                    break;
                }
                case TIMER_TOAST: OnToastTimer(); break;
                case TIMER_PROFILES: ProfilesTick(); break;
                case TIMER_CACHE: CacheSave(); break;
                case TIMER_STARTUP:
                    KillTimer(hwnd, TIMER_STARTUP);
                    if (g_config.watchdogEnabled) WatchdogRefresh(false);
                    CheckupRefresh();
                    break;
                case TIMER_HEALTHY: KillTimer(hwnd, TIMER_HEALTHY); AutoUpdateOnStarted(); break;
                case TIMER_UPDATE_CHECK: AutoUpdateOnTimer(); break;
            }
            return 0;
        case WM_BREAKER_CALL: {
            auto* fn = reinterpret_cast<std::function<void()>*>(lp);
            if (wp == 1) { (*fn)(); return 0; }      // UiSync: the caller owns it and is waiting
            (*fn)();
            delete fn;
            return 0;
        }
        case WM_BREAKER_UPDATE: {
            AutoUpdateOnStatus();
            updater::Status st = updater::Get();
            if (st.state == updater::State::Available && !g_config.autoInstall && g_config.notifiedVersion != st.release.version) {
                g_config.notifiedVersion = st.release.version;
                ConfigSave();
                ShowToast(L"Breaker " + st.release.version + L" is available", L"What's new", [] { UiNavigate(L"settings/updates"); });
            }
            if (g_route == L"settings/updates" || g_route == L"home") BuildPage(true);
            Redraw();
            return 0;
        }
        case WM_BREAKER_TRAY: TrayOnMessage(hwnd, lp); return 0;
        case WM_BREAKER_SHOW:
            if (wp == 2) OpenTarget(L"ms-settings:");
            else if (IsWindowVisible(hwnd) && GetForegroundWindow() == hwnd) FocusArea(AreaSearch);
            else ShowMain();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_THEMECHANGED:
            UiThemeChanged();
            return 0;
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == 0x4252) {
                std::wstring link(cds->lpData ? (const wchar_t*)cds->lpData : L"", cds->cbData / sizeof(wchar_t));
                while (!link.empty() && link.back() == 0) link.pop_back();
                ShowMain();
                if (!link.empty()) OpenLinkSpec(link);
            }
            return TRUE;
        }
        case WM_CLOSE:
            if (g_config.keepInTray && wp != 1) {
                SaveWindowPlacement();
                ConfigSave();
                CacheSave();
                ShowWindow(hwnd, SW_HIDE);
                DiscardDevice();          // give the graphics memory back while hidden
                g_rowsDyn1.clear();
                g_rowsDyn2.clear();
                g_rows.clear();
                g_vis.clear();
                g_lay.clear();
                g_pageCache.clear();
                g_measure.clear();
                g_zones.clear();
                g_acc.clear();
                g_accZones.clear();
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);   // a tray icon shouldn't hold pages in RAM
                return 0;
            }
            SaveWindowPlacement();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ConfigSave();
            CacheSave();
            TrayRemove();
            WinIHookSet(hwnd, false);
            DiscardDevice();
            ReleaseFormats();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND MainCreate(HINSTANCE inst, const std::wstring& link, bool startHidden, const wchar_t* cmdLine) {
    if (!gfx::Init()) { Log(L"Direct2D / DirectWrite aren't available"); return nullptr; }
    g_uiThread = GetCurrentThreadId();
    g_startHidden = startHidden;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = BREAKER_WNDCLASS;
    RegisterClassExW(&wc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    g_scale = g_dpi / 96.0f;
    int w = (int)(g_config.winW * g_scale), h = (int)(g_config.winH * g_scale);
    int x = g_config.winX, y = g_config.winY;
    RECT want{x, y, x + w, y + h};
    if (x == -1 || !MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
    g_hwnd = CreateWindowExW(0, BREAKER_WNDCLASS, L"Breaker", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { Log(L"CreateWindow failed (error %lu)", GetLastError()); return nullptr; }
    g_dpi = GetDpiForWindow(g_hwnd);
    g_scale = g_dpi / 96.0f;
    TasksInit(g_hwnd);
    ApplyTheme();
    CreateFormats();
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    g_releaseAppForUpdate = ReleaseForUpdate;
    g_updateBlocked = [] { return g_busy > 0; };
    AutoUpdateInit(g_hwnd, cmdLine);
    UiBackgroundChanged();
    if (startHidden && !g_config.keepInTray) TrayAdd(g_hwnd);     // started for the sign-in watchdog: stay reachable

    // Reopen where the user was after an update; otherwise start on Home (or the page a link asked for)
    g_route = L"home";
    if (AutoUpdateStartedAfterUpdate() && FindPage(g_config.lastPage)) g_route = g_config.lastPage;
    BuildPage(false);
    if (!link.empty()) OpenLinkSpec(link);
    if (!AutoUpdateFailedVersion().empty()) UiToast(L"The update to " + AutoUpdateFailedVersion() + L" didn't start, so Breaker went back to this version");

    if (!startHidden) {
        ShowWindow(g_hwnd, g_config.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        UpdateWindow(g_hwnd);
    }
    SetTimer(g_hwnd, TIMER_STARTUP, 1200, nullptr);
    SetTimer(g_hwnd, TIMER_CACHE, 60 * 1000, nullptr);
    return g_hwnd;
}

void MainNotifyDrift(int count) {
    TrayNotify(L"Windows changed your settings", count == 1 ? L"One setting you chose was changed. Click to review it in Breaker."
                                                           : std::to_wstring(count) + L" settings you chose were changed. Click to review them in Breaker.");
}
