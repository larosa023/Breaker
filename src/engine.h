// Breaker - the settings engine
//
// Every setting is an id with a string value:
//   toggle  "1" / "0"        choice  one of its choice values
//   slider  a number         text    the text itself
// Settings come from two places:
//   * the catalog (catalog.cpp): one Def per setting. Most are pure data: a registry spec the engine
//     reads, writes and undoes by itself. A spec of "@" means a Handler does the work.
//   * handlers (providers.cpp, suite.cpp, window.cpp): code for settings that need an API call, and for
//     dynamic ids such as "svc:Spooler" or "env:u:PATH" that don't exist until the PC is inspected.
// ApplyChanges() is the only way anything is changed. It captures the exact previous state, writes,
// asks for admin rights only for the writes Windows refused, and records the change for undo.
#pragma once
#include "common.h"

enum Ctl { CtlToggle, CtlChoice, CtlSlider, CtlText, CtlLink, CtlInfo };

enum : unsigned {
    F_ADMIN    = 1 << 0,   // needs administrator rights (shows a shield; the engine also detects this by itself)
    F_RISKY    = 1 << 1,   // ask before changing
    F_EXPLORER = 1 << 2,   // takes full effect after Explorer restarts
    F_SIGNOUT  = 1 << 3,   // takes effect after signing out
    F_REBOOT   = 1 << 4,   // takes effect after a restart
    F_THEME    = 1 << 5,   // broadcast a theme change so apps repaint
    F_DEBLOAT  = 1 << 6,   // also listed on the Debloat page
    F_KEEP     = 1 << 7,   // revert automatically unless the user confirms within 15 seconds
    F_NOSNAP   = 1 << 8,   // specific to this PC: left out of snapshots
    F_NOHIST   = 1 << 9,   // not recorded for undo (Breaker's own options, one-off actions)
    F_NOWATCH  = 1 << 10,  // changes all the time outside Breaker: the watchdog ignores it
    F_FILE     = 1 << 11,  // text control: pick a file
    F_COLOR    = 1 << 12,  // text control: pick a color
    F_TRAY     = 1 << 13,  // tell the taskbar to reload
};

struct Def {
    const wchar_t* id;        // "colors.darkApps"
    const wchar_t* page;      // "personalization/colors"
    const wchar_t* group;     // heading on the page
    const wchar_t* name;
    const wchar_t* desc;      // one plain-English sentence
    const wchar_t* keys;      // extra search words
    Ctl ctl;
    const wchar_t* spec;      // registry spec, "@" for a handler, or the target of a link
    const wchar_t* choices;   // choice: "Label=value|Label=value"   slider: "min|max|step|unit"
    const wchar_t* rec;       // recommended value, or nullptr
    unsigned flags;
    const wchar_t* uri;       // where this lives in Windows Settings (fallback and "open in Windows")
    const wchar_t* detail;    // longer explanation for the info panel
};

extern const Def kDefs[];
extern const int kDefCount;

using Choices = std::vector<std::pair<std::wstring, std::wstring>>;   // label, value

enum WriteResult { W_OK, W_DENIED, W_FAIL };

struct Handler {
    const wchar_t* prefix;    // a whole id, or a prefix ending in ':' or '.'
    bool (*read)(const std::wstring& id, std::wstring& out);
    WriteResult (*write)(const std::wstring& id, const std::wstring& value, std::wstring& err);
    void (*choices)(const std::wstring& id, Choices& out);    // optional: choices found at run time
    unsigned flags;
};

struct Change {
    std::wstring id, value;
    std::wstring label;       // display name for dynamic ids (history)
};

struct ApplyReport {
    int ok = 0, failed = 0;
    bool cancelled = false;   // the user said no to the admin prompt
    unsigned flags = 0;       // union of the flags of everything that changed
    std::wstring error;       // first error, in plain words
    std::vector<uint64_t> seqs;   // history entries created
};

struct HistEntry {
    uint64_t seq = 0;
    double time = 0;
    std::wstring id, name, before, after, source;   // before: exact captured state; after: value written
    std::wstring was;         // the value before, in the same form as `after` (for display)
    bool reverted = false;
};

void EngineInit();
void RegisterHandlers(const Handler* list, int count);
const Def* FindDef(const std::wstring& id);
const Handler* FindHandler(const std::wstring& id);
unsigned FlagsOf(const std::wstring& id);
std::wstring NameOf(const std::wstring& id);
void GetChoices(const std::wstring& id, Choices& out);
std::wstring ChoiceLabel(const std::wstring& id, const std::wstring& value);
std::wstring UnderTheHood(const Def& d);
std::wstring DescribeValue(const std::wstring& id, const std::wstring& value);   // "On", "Dark", "50"

// Slow or blocking: background tasks only.
bool ReadValue(const std::wstring& id, std::wstring& out);
ApplyReport ApplyChanges(const std::vector<Change>& changes, const std::wstring& source);
int ElevatedMain(const std::wstring& file);     // "Breaker.exe --elevated <file>"

// Last known values, so pages paint instantly and refresh behind the scenes.
bool CachedValue(const std::wstring& id, std::wstring& out);
void CacheStore(const std::wstring& id, const std::wstring& value);
void CacheSave();

// Undo history (newest first)
std::vector<HistEntry> HistoryList();
ApplyReport HistoryRevert(uint64_t seq);
ApplyReport HistoryUndoLast();
ApplyReport HistoryRestoreTo(double unixTime);  // undo everything changed since then
void HistoryClear();

// What the user chose, for the update watchdog
std::vector<std::pair<std::wstring, std::wstring>> DesiredList();
void DesiredSet(const std::wstring& id, const std::wstring& value);
void DesiredForget(const std::wstring& id);
