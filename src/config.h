// Breaker - its own settings (config.json), profiles and theme files
#pragma once
#include "common.h"

struct Profile {
    std::wstring name;
    std::wstring trigger;      // "" manual, "game" (a full-screen game is running), "battery", "exe:a.exe;b.exe"
    std::vector<std::pair<std::wstring, std::wstring>> values;   // setting id -> value
};

struct Config {
    // ---- Appearance (keys shared with Foyer and Strata so theme files work in all three)
    std::wstring theme = L"dark";          // dark, light, midnight, glass, ember, ocean, system, custom
    bool useWindowsAccent = true;
    uint32_t accentColor = 0x4CC2FF;       // 0xRRGGBB when not using the Windows accent
    std::wstring backdrop = L"mica";       // solid, mica, acrylic
    float opacity = 0.86f;                 // tint strength over the backdrop (0.4 - 1.0)
    float cornerRadius = 8.0f;             // 0 - 16
    bool accentGlow = false;               // Foyer key; unused here
    std::wstring customTheme;              // name of the imported theme when theme == custom
    std::vector<std::pair<std::wstring, uint32_t>> customColors;   // color overrides from a theme file
    std::wstring fontFamily;               // "" = Segoe UI Variable / Segoe UI
    float fontSize = 13.0f;                // 11 - 17
    std::wstring density = L"comfortable"; // compact, comfortable
    std::wstring iconSize = L"medium";     // suite key; unused here
    std::wstring animation = L"smooth";    // none, fast, smooth, slow
    bool respectHighContrast = true;
    bool syncSuiteTheme = false;           // write theme changes to the other installed suite apps too

    // ---- Layout
    std::vector<std::wstring> sidebarOrder;    // category ids in display order (others follow)
    std::vector<std::wstring> sidebarHidden;   // category ids not shown in the sidebar
    std::vector<std::wstring> pinned;          // setting ids shown as quick toggles on Home
    float sidebarWidth = 236;
    bool showRecommended = true;

    // ---- Behavior
    bool keepInTray = false;               // closing the window keeps Breaker in the tray
    bool startWithWindows = false;         // mirrors the registry state
    bool winI = false;                     // Win+I opens Breaker (needs the tray process)
    bool watchdogAtSignIn = false;         // quick check at sign-in, then exit
    bool watchdogEnabled = true;           // compare settings whenever Breaker starts
    bool confirmRisky = true;
    bool restorePointFirst = true;        // make a System Restore point before big batches of changes
    bool autoProfiles = false;             // switch profiles on their triggers (needs the tray process)
    std::wstring activeProfile;
    std::wstring lastPage = L"home";
    std::wstring lastBuild;                // Windows build seen on the last run (watchdog)
    std::vector<Profile> profiles;
    std::vector<std::wstring> suiteApps;   // extra app names for My Apps

    // ---- Screen time (read by the background tracker too)
    bool tallyEnabled = true;              // track which app is in front
    int tallyIdleMin = 2;                  // no input this long (and no video or game) = away
    bool tallyWeekly = true;               // a summary notification at the start of each week
    bool tallyLimitNotify = true;          // gentle notifications at 80% and 100% of a limit
    std::vector<std::wstring> tallyNever;  // app keys never recorded
    std::vector<std::pair<std::wstring, std::wstring>> tallyCats;   // app key -> category the user chose
    std::vector<std::pair<std::wstring, int>> tallyLimits;          // app key or "cat:Games" -> minutes a day

    // ---- Updates
    bool autoCheck = true;
    bool autoInstall = false;
    std::wstring channel = L"stable";      // stable, beta
    int checkHours = 6;
    double lastCheck = 0;
    std::wstring notifiedVersion;

    // ---- Window
    int winX = -1, winY = -1, winW = 1140, winH = 780;
    bool maximized = false;
};

extern Config g_config;
void ConfigLoad();
void ConfigSave();
std::wstring ConfigPath();
Profile* FindProfile(const std::wstring& name);

// Theme files: the JSON format Foyer and Strata import and export.
bool ThemeExport(const std::wstring& path, const std::wstring& name);
bool ThemeImport(const std::wstring& path, std::wstring* error);
