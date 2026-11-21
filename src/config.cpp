// Breaker - settings persisted as config.json
#include "config.h"
#include "json.h"
#include <algorithm>
#include <cmath>

Config g_config;

std::wstring ConfigPath() { return DataDir() + L"\\config.json"; }

static std::wstring OneOf(const std::wstring& v, std::initializer_list<const wchar_t*> allowed, const wchar_t* def) {
    for (auto a : allowed) if (v == a) return v;
    return def;
}

namespace {
struct BoolField { const wchar_t* key; bool Config::*m; };
struct StrField { const wchar_t* key; std::wstring Config::*m; std::initializer_list<const wchar_t*> allowed; };
struct FloatField { const wchar_t* key; float Config::*m; float lo, hi; };
struct IntField { const wchar_t* key; int Config::*m; int lo, hi; };
struct ListField { const wchar_t* key; std::vector<std::wstring> Config::*m; };
}  // namespace

static const BoolField kBools[] = {
    {L"useWindowsAccent", &Config::useWindowsAccent}, {L"accentGlow", &Config::accentGlow},
    {L"respectHighContrast", &Config::respectHighContrast}, {L"syncSuiteTheme", &Config::syncSuiteTheme},
    {L"showRecommended", &Config::showRecommended}, {L"keepInTray", &Config::keepInTray},
    {L"startWithWindows", &Config::startWithWindows}, {L"winI", &Config::winI},
    {L"watchdogAtSignIn", &Config::watchdogAtSignIn}, {L"watchdogEnabled", &Config::watchdogEnabled},
    {L"confirmRisky", &Config::confirmRisky}, {L"restorePointFirst", &Config::restorePointFirst}, {L"autoProfiles", &Config::autoProfiles},
    {L"autoCheck", &Config::autoCheck}, {L"autoInstall", &Config::autoInstall}, {L"maximized", &Config::maximized},
};
static const StrField kStrs[] = {
    {L"theme", &Config::theme, {L"dark", L"light", L"midnight", L"glass", L"ember", L"ocean", L"system", L"custom"}},
    {L"backdrop", &Config::backdrop, {L"solid", L"mica", L"acrylic"}},
    {L"density", &Config::density, {L"compact", L"comfortable"}},
    {L"iconSize", &Config::iconSize, {L"small", L"medium", L"large"}},
    {L"animation", &Config::animation, {L"none", L"fast", L"smooth", L"slow"}},
    {L"channel", &Config::channel, {L"stable", L"beta"}},
    {L"customTheme", &Config::customTheme, {}}, {L"fontFamily", &Config::fontFamily, {}},
    {L"activeProfile", &Config::activeProfile, {}}, {L"lastPage", &Config::lastPage, {}},
    {L"lastBuild", &Config::lastBuild, {}}, {L"notifiedVersion", &Config::notifiedVersion, {}},
};
static const FloatField kFloats[] = {
    {L"opacity", &Config::opacity, 0.4f, 1.0f}, {L"cornerRadius", &Config::cornerRadius, 0, 16},
    {L"fontSize", &Config::fontSize, 11, 17}, {L"sidebarWidth", &Config::sidebarWidth, 180, 360},
};
static const IntField kInts[] = {
    {L"checkHours", &Config::checkHours, 1, 168}, {L"winX", &Config::winX, -32000, 32000}, {L"winY", &Config::winY, -32000, 32000},
    {L"winW", &Config::winW, 640, 10000}, {L"winH", &Config::winH, 420, 10000},
};
static const ListField kLists[] = {
    {L"sidebarOrder", &Config::sidebarOrder}, {L"sidebarHidden", &Config::sidebarHidden},
    {L"pinned", &Config::pinned}, {L"suiteApps", &Config::suiteApps},
};

static void DefaultProfiles(Config& c) {
    c.profiles = {
        {L"Gaming", L"game", {{L"gaming.gameMode", L"1"}, {L"notify.toasts", L"0"}, {L"power.sleep.ac", L"0"}}},
        {L"Work", L"", {{L"notify.toasts", L"1"}, {L"clipboard.history", L"1"}, {L"power.sleep.ac", L"1800"}}},
        {L"Battery Saver", L"battery", {{L"colors.transparency", L"0"}, {L"power.screen.dc", L"120"}, {L"power.sleep.dc", L"300"}}},
        {L"Presentation", L"", {{L"notify.toasts", L"0"}, {L"power.screen.ac", L"0"}, {L"power.sleep.ac", L"0"}}},
    };
}

static void DefaultPins(Config& c) {
    c.pinned = {L"colors.darkApps", L"colors.transparency", L"gaming.gameMode", L"clipboard.history", L"explorer.extensions", L"notify.toasts"};
}

void ConfigLoad() {
    Config c;
    std::wstring text;
    json::Value root;
    bool fresh = true;
    if (ReadUtf8File(ConfigPath(), text)) {
        json::Parser p(text);
        if (p.Parse(root) && root.type == json::Value::Object) fresh = false;
        else {
            Log(L"config.json is invalid; backing it up and using defaults");
            MoveFileExW(ConfigPath().c_str(), (ConfigPath() + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
            root = json::Value::MakeObject();
        }
    }
    for (auto& f : kBools) c.*f.m = root.GetBool(f.key, c.*f.m);
    for (auto& f : kStrs) {
        std::wstring v = root.GetStr(f.key, c.*f.m);
        c.*f.m = f.allowed.size() ? OneOf(v, f.allowed, (c.*f.m).c_str()) : v;
    }
    for (auto& f : kFloats) c.*f.m = std::clamp((float)root.GetNum(f.key, c.*f.m), f.lo, f.hi);
    for (auto& f : kInts) c.*f.m = std::clamp((int)root.GetNum(f.key, c.*f.m), f.lo, f.hi);
    for (auto& f : kLists)
        if (const json::Value* a = root.Get(f.key); a && a->type == json::Value::Array)
            for (auto& v : a->arr) if (v.type == json::Value::String) (c.*f.m).push_back(v.s);
    c.accentColor = ParseHexColor(root.GetStr(L"accentColor"), c.accentColor);
    c.lastCheck = root.GetNum(L"lastCheck", 0);
    if (const json::Value* cc = root.Get(L"customColors"); cc && cc->type == json::Value::Object)
        for (auto& kv : cc->obj) if (kv.second.type == json::Value::String) c.customColors.push_back({kv.first, ParseHexColor(kv.second.s, 0)});
    if (const json::Value* ps = root.Get(L"profiles"); ps && ps->type == json::Value::Array) {
        for (auto& pv : ps->arr) {
            Profile p;
            p.name = pv.GetStr(L"name");
            p.trigger = pv.GetStr(L"trigger");
            if (const json::Value* vals = pv.Get(L"values"); vals && vals->type == json::Value::Object)
                for (auto& kv : vals->obj) if (kv.second.type == json::Value::String) p.values.push_back({kv.first, kv.second.s});
            if (!p.name.empty()) c.profiles.push_back(std::move(p));
        }
    } else {
        DefaultProfiles(c);
    }
    if (fresh || !root.Get(L"pinned")) DefaultPins(c);
    g_config = std::move(c);
    Log(L"Config loaded: theme %ls, %u profiles, %u pinned", g_config.theme.c_str(), (unsigned)g_config.profiles.size(),
        (unsigned)g_config.pinned.size());
}

void ConfigSave() {
    using json::Value;
    const Config& c = g_config;
    Value root = Value::MakeObject();
    for (auto& f : kStrs) root.Set(f.key, Value::Str(c.*f.m));
    for (auto& f : kBools) root.Set(f.key, Value::Boolean(c.*f.m));
    for (auto& f : kFloats) root.Set(f.key, Value::Num(std::round(c.*f.m * 100) / 100));
    for (auto& f : kInts) root.Set(f.key, Value::Num(c.*f.m));
    for (auto& f : kLists) {
        Value a = Value::MakeArray();
        for (auto& s : c.*f.m) a.arr.push_back(Value::Str(s));
        root.Set(f.key, a);
    }
    root.Set(L"accentColor", Value::Str(HexColor(c.accentColor)));
    root.Set(L"lastCheck", Value::Num(c.lastCheck));
    Value cc = Value::MakeObject();
    for (auto& kv : c.customColors) cc.Set(kv.first, Value::Str(HexColor(kv.second)));
    root.Set(L"customColors", cc);
    Value ps = Value::MakeArray();
    for (auto& p : c.profiles) {
        Value pv = Value::MakeObject();
        pv.Set(L"name", Value::Str(p.name));
        pv.Set(L"trigger", Value::Str(p.trigger));
        Value vals = Value::MakeObject();
        for (auto& kv : p.values) vals.Set(kv.first, Value::Str(kv.second));
        pv.Set(L"values", vals);
        ps.arr.push_back(pv);
    }
    root.Set(L"profiles", ps);
    std::wstring text;
    json::Write(root, text);
    if (!WriteUtf8FileAtomic(ConfigPath(), text)) Log(L"Failed to save config.json (error %lu)", GetLastError());
}

Profile* FindProfile(const std::wstring& name) {
    for (auto& p : g_config.profiles) if (p.name == name) return &p;
    return nullptr;
}

// ---------------------------------------------------------------- theme files
// The suite's theme file: a small JSON object using the same keys as the settings:
// {"suiteTheme":1,"name":"...","base":"dark","accentColor":"#4CC2FF","useWindowsAccent":false,
//  "backdrop":"mica","opacity":0.86,"cornerRadius":6,"colors":{"bg":"#202024",...}}
bool ThemeExport(const std::wstring& path, const std::wstring& name) {
    const Config& c = g_config;
    json::Value root = json::Value::MakeObject();
    root.Set(L"suiteTheme", json::Value::Num(1));
    root.Set(L"name", json::Value::Str(name));
    std::wstring base = c.theme;
    if (base == L"custom") {
        base = L"dark";
        for (auto& kv : c.customColors) if (StartsWith(kv.first, L"base:")) base = kv.first.substr(5);
    }
    root.Set(L"base", json::Value::Str(base));
    root.Set(L"useWindowsAccent", json::Value::Boolean(c.useWindowsAccent));
    root.Set(L"accentColor", json::Value::Str(HexColor(c.accentColor)));
    root.Set(L"backdrop", json::Value::Str(c.backdrop));
    root.Set(L"opacity", json::Value::Num(std::round(c.opacity * 100) / 100));
    root.Set(L"cornerRadius", json::Value::Num(std::round(c.cornerRadius)));
    json::Value colors = json::Value::MakeObject();
    for (auto& kv : c.customColors) if (!StartsWith(kv.first, L"base:")) colors.Set(kv.first, json::Value::Str(HexColor(kv.second)));
    root.Set(L"colors", colors);
    std::wstring text;
    json::Write(root, text);
    return WriteUtf8FileAtomic(path, text);
}

bool ThemeImport(const std::wstring& path, std::wstring* error) {
    std::wstring text;
    if (!ReadUtf8File(path, text)) { if (error) *error = L"Couldn't read that file."; return false; }
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) { if (error) *error = L"That isn't a theme file."; return false; }
    Config& c = g_config;
    std::wstring base = root.GetStr(L"base", root.GetStr(L"theme", L"dark"));
    c.useWindowsAccent = root.GetBool(L"useWindowsAccent", false);
    c.accentColor = ParseHexColor(root.GetStr(L"accentColor"), c.accentColor);
    c.backdrop = OneOf(root.GetStr(L"backdrop", c.backdrop), {L"solid", L"mica", L"acrylic"}, L"mica");
    c.opacity = std::clamp((float)root.GetNum(L"opacity", c.opacity), 0.4f, 1.0f);
    c.cornerRadius = std::clamp((float)root.GetNum(L"cornerRadius", c.cornerRadius), 0.0f, 16.0f);
    c.customColors.clear();
    // Theme files use "colors"; a Foyer or Strata config.json uses "customColors"
    const json::Value* cc = root.Get(L"colors");
    if (!cc) cc = root.Get(L"customColors");
    if (cc && cc->type == json::Value::Object)
        for (auto& kv : cc->obj)
            if (kv.second.type == json::Value::String && !StartsWith(kv.first, L"base:")) c.customColors.push_back({kv.first, ParseHexColor(kv.second.s, 0)});
    std::wstring baseOk = OneOf(base, {L"dark", L"light", L"midnight", L"glass", L"ember", L"ocean", L"system"}, L"dark");
    if (c.customColors.empty()) {
        c.theme = baseOk;
    } else {
        c.theme = L"custom";
        c.customTheme = root.GetStr(L"name", L"Imported theme");
        c.customColors.insert(c.customColors.begin(), {L"base:" + baseOk, 0});
    }
    ConfigSave();
    return true;
}
