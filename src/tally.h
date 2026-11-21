// Breaker - Screen time (working name Tally): the shared data store.
//
// The tracker ("Breaker.exe --tracker", tracker.cpp) writes, the Screen time pages (tallyui.cpp) read.
// Everything lives in <data folder>\screentime:
//   YYYY-MM.tsv   one line per day and app: "day<TAB>app key<TAB>seconds"
//   apps.tsv      what the tracker learned about each app: "key<TAB>name<TAB>game 0/1<TAB>exe path"
//   state.tsv     which limit and weekly notifications were already shown
// An app key is the lower-case exe name ("chrome.exe"); RetroArch content gets "retroarch|<game>".
// Categories the user picks, limits and the never-track list live in Breaker's config.json.
#pragma once
#include "common.h"
#include <map>

namespace tally {

constexpr UINT WM_TALLY_FLUSH = WM_APP + 20;    // SendMessage to the tracker: write everything to disk (returns 1)
constexpr UINT WM_TALLY_RELOAD = WM_APP + 21;   // PostMessage to the tracker: Breaker's settings changed
constexpr ULONG_PTR kCopyClear = 0x54434C52;    // WM_COPYDATA to the tracker: L"<from> <to> [key]" clears history
constexpr const wchar_t* kTrackerClass = L"BreakerTracker";

extern const wchar_t* const kCategoryNames[];   // Games, Work, Browsing, Media, Chat, Other
constexpr int kCategoryCount = 6;

struct AppInfo {
    std::wstring key, name, path;
    bool game = false;
};

using DayMap = std::map<int, std::map<std::wstring, long long>>;   // yyyymmdd -> app key -> seconds

std::wstring Dir();
// Dates are yyyymmdd ints in local time.
int Today();
int AddDays(int ymd, int days);
int DaysBetween(int from, int to);           // to - from, in days
int Weekday(int ymd);                        // 0 = Sunday
int WeekStart(int ymd);                      // first day of the week, following the Windows region setting
int MonthStart(int ymd);
int MonthEnd(int ymd);
std::wstring DayLabel(int ymd, bool longForm);   // "Mon" / "Mon, Nov 17"

bool LoadMonth(int year, int month, DayMap& out);           // adds into out
bool SaveMonth(int year, int month, const DayMap& all);    // writes the days of that month from `all`
void LoadRange(int from, int to, DayMap& out);
std::map<std::wstring, AppInfo> LoadApps();
bool SaveApps(const std::map<std::wstring, AppInfo>& apps);
bool ClearRange(int from, int to, const std::wstring& key);   // edits the files directly; key "" = every app

std::wstring AutoCategory(const std::wstring& key, bool game);
std::wstring CategoryOf(const std::wstring& key, const std::map<std::wstring, AppInfo>& apps);   // the user's choice wins
std::wstring NameOf(const std::wstring& key, const std::map<std::wstring, AppInfo>& apps);
int LimitFor(const std::wstring& limitKey);   // minutes, 0 = none ("cat:Games" or an app key)

std::wstring Dur(long long seconds);       // "2h 05m", "45m", "<1m"
std::wstring Csv(const std::wstring& s);
bool ExportCsv(const std::wstring& path);  // every day and app ever recorded; blocking

// ---- talking to the tracker (from Breaker's window)
HWND FindTracker();
bool FlushTracker();                       // blocks up to 2 s
void NotifyTrackerReload();
bool ClearHistory(int from, int to, const std::wstring& key);   // through the tracker when it runs
void EnsureTracker(bool enabled);          // Run key + start, restart after an update, or stop. Blocking (call off the UI thread)
void StopTracker();                        // uninstall

}  // namespace tally

int TrackerMain();                         // tracker.cpp: "Breaker.exe --tracker"
