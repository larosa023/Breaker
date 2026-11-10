// Breaker - smart features: search, profiles, the update watchdog, snapshots
#pragma once
#include "pages.h"

struct Drift {
    std::wstring id, name, wanted, now;
};

std::wstring WindowsBuildText();                   // "26100.2314"
std::vector<Drift> WatchdogCompute();              // blocking: compare what the user chose with what's set now
void WatchdogRefresh(bool toastWhenClean);         // window thread: check in the background, then update the UI
const std::vector<Drift>& WatchdogDrift();         // window thread: result of the last check
std::wstring WatchdogBuildNote();                  // "Windows updated from build A to B", or ""

void ProfileApply(const std::wstring& name);
void ProfileAddSetting(const std::wstring& profile, const std::wstring& id, const std::wstring& value);
void ProfilesTick();                               // automatic switching; called on a timer while it's enabled

void SearchRows(const std::wstring& query, std::vector<Row>& out);
bool SnapshotWrite(const std::wstring& path, std::wstring* err);   // blocking
