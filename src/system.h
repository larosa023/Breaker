// Breaker - Windows integration: install / uninstall, portable mode, start with Windows, tray icon,
// the Win+I shortcut, the breaker: link protocol and the sign-in watchdog entry
#pragma once
#include "common.h"

bool StartWithWindowsEnabled();
void SetStartWithWindows(bool on);          // "Breaker.exe --tray" in the user's Run key
void SetWatchdogAtSignIn(bool on);          // "Breaker.exe --watchdog" in the user's Run key

bool IsInstalledCopy();                     // running from %LOCALAPPDATA%\Programs\Breaker
std::wstring InstallDir();
bool InstallBreaker(std::wstring* error);   // copies the exe, adds Start menu shortcut, uninstall entry and breaker: links
void UninstallBreaker(bool keepSettings);
bool IsPortable();
bool SetPortable(bool on, std::wstring* error);
void RegisterProtocol();                    // breaker://page links open this exe

void TrayAdd(HWND owner);
void TrayRemove();
void TrayOnMessage(HWND owner, LPARAM lParam);
void TrayNotify(const std::wstring& title, const std::wstring& text);

void WinIHookSet(HWND owner, bool on);      // Win+I shows Breaker; Shift+Win+I opens Windows Settings
void ForceForeground(HWND hwnd);
