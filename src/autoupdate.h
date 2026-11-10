// Breaker - wiring between the shared updater module (same as Foyer's and Strata's) and the app
#pragma once
#include "common.h"
#include "updater.h"

void AutoUpdateInit(HWND owner, const wchar_t* cmdLine);
void AutoUpdateOnStarted();          // the app is up: report healthy, schedule the first check
void AutoUpdateOnTimer();
void AutoUpdateOnStatus();           // WM_BREAKER_UPDATE
void AutoUpdateApplySettings();
void AutoUpdateCheckNow();
void AutoUpdateDownloadNow();
void AutoUpdateInstallNow();
bool AutoUpdateAvailable();
bool AutoUpdateStartedAfterUpdate(); // this run is the first after an update (reopen the same page)
std::wstring AutoUpdateFailedVersion();   // set when the last update failed to start and was rolled back
extern void (*g_releaseAppForUpdate)();   // set by the window: saves state, removes the tray icon and hook, releases the mutex
extern bool (*g_updateBlocked)();         // set by the window: true while a setting change is being applied
constexpr UINT_PTR TIMER_UPDATE_CHECK = 100;
constexpr UINT_PTR TIMER_HEALTHY = 101;
