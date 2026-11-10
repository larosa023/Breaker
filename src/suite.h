// Breaker - the rest of the suite
#pragma once
#include "pages.h"

void RegisterSuiteHandlers();
std::wstring SuiteExe(const std::wstring& app);    // "" when the app isn't installed
// Writes Breaker's theme into the other apps' settings. Running apps are restarted when asked,
// otherwise skipped and listed. Returns how many apps were changed. Blocking.
int SuiteApplyTheme(bool restartRunning, std::vector<std::wstring>* skipped);
