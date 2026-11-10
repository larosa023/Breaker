// Breaker - shared declarations
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <functional>

#define BREAKER_VERSION L"1.0.0"
#define BREAKER_WNDCLASS L"BreakerMain"

// Messages posted to the main window
constexpr UINT WM_BREAKER_CALL   = WM_APP + 1;  // lParam: std::function<void()>* (work finished on a background thread)
constexpr UINT WM_BREAKER_UPDATE = WM_APP + 2;  // updater status changed
constexpr UINT WM_BREAKER_TRAY   = WM_APP + 3;  // tray icon callback
constexpr UINT WM_BREAKER_SHOW   = WM_APP + 4;  // Win+I hook / second instance: show the window

// ---- log.cpp ----
void LogInit();
void Log(const wchar_t* fmt, ...);
std::wstring DataDir();          // %APPDATA%\Breaker, or exe folder\data in portable mode
std::wstring ExeDir();
std::wstring ExePath();

// ---- util.cpp ----
inline std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
bool ReadUtf8File(const std::wstring& path, std::wstring& out);
bool WriteUtf8FileAtomic(const std::wstring& path, const std::wstring& text);
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::vector<std::wstring> Split(const std::wstring& s, const std::wstring& sep, bool keepEmpty = false);
std::wstring Trim(const std::wstring& s);
bool StartsWith(const std::wstring& s, const wchar_t* prefix);
std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to);
uint32_t ParseHexColor(const std::wstring& s, uint32_t def);
std::wstring HexColor(uint32_t c);
double NowUnix();
std::wstring FormatWhen(double unixTime);          // "Today 14:32", "Nov 9, 14:32"
std::wstring ErrorText(DWORD err);
std::wstring FileVersion(const std::wstring& path);
std::wstring ExpandEnv(const std::wstring& s);
bool FileExists(const std::wstring& path);
bool IsElevated();

// Registry helpers. `value` == nullptr means the key's default value.
bool RegReadStr(HKEY root, const wchar_t* path, const wchar_t* value, std::wstring& out);
bool RegReadDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD& out);
LONG RegWriteStr(HKEY root, const wchar_t* path, const wchar_t* value, const std::wstring& data, DWORD type = REG_SZ);
LONG RegWriteDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD data);
LONG RegRemoveValue(HKEY root, const wchar_t* path, const wchar_t* value);

// Processes. Everything here blocks, so call it from a background task.
bool RunCapture(const std::wstring& cmdLine, std::wstring* out, DWORD timeoutMs = 30000, DWORD* exitCode = nullptr);
bool RunPowerShell(const std::wstring& script, std::wstring* out, DWORD timeoutMs = 60000, DWORD* exitCode = nullptr);
bool ShellOpen(const std::wstring& target, const std::wstring& args = L"", bool elevated = false);
bool OpenTarget(const std::wstring& spec);         // "ms-settings:x", "control /name ..", "x.cpl", "x.msc", exe + args

// Background work: `work` runs on a pool thread; Ui(fn) runs fn on the window thread.
void TasksInit(HWND owner);
void RunAsync(std::function<void()> work);
void Ui(std::function<void()> fn);
