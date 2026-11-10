// Breaker - main window
#pragma once
#include "common.h"

// link: a page to open ("system/display", "breaker://privacy/telemetry#privacy.adId"), or empty for Home.
HWND MainCreate(HINSTANCE inst, const std::wstring& link, bool startHidden, const wchar_t* cmdLine);
void MainNotifyDrift(int count);     // tray balloon from the sign-in watchdog
