// Breaker - small helpers: files, strings, registry, processes, background tasks
#include "common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <ctime>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <thread>
#include <algorithm>

bool ReadUtf8File(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size > 32 * 1024 * 1024) { CloseHandle(h); return false; }
    std::string buf(size, '\0');
    DWORD read = 0;
    bool ok = size == 0 || ReadFile(h, &buf[0], size, &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    size_t skip = (read >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) ? 3 : 0;
    out = Utf8ToWide(std::string(buf.data() + skip, read - skip));
    return true;
}

bool WriteUtf8FileAtomic(const std::wstring& path, const std::wstring& text) {
    std::string buf = WideToUtf8(text);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &written, nullptr) && written == buf.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::wstring Utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string b(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &b[0], n, nullptr, nullptr);
    return b;
}

std::vector<std::wstring> Split(const std::wstring& s, const std::wstring& sep, bool keepEmpty) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::wstring::npos) j = s.size();
        if (keepEmpty || j > i) out.push_back(s.substr(i, j - i));
        i = j + sep.size();
    }
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return s;
    for (size_t i = 0; (i = s.find(from, i)) != std::wstring::npos; i += to.size()) s.replace(i, from.size(), to);
    return s;
}

uint32_t ParseHexColor(const std::wstring& s, uint32_t def) {
    std::wstring t = s;
    if (!t.empty() && t[0] == L'#') t = t.substr(1);
    if (t.size() != 6) return def;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(t.c_str(), &end, 16);
    return (end && *end == 0) ? (uint32_t)v : def;
}

std::wstring HexColor(uint32_t c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%06X", c & 0xFFFFFF);
    return b;
}

double NowUnix() { return (double)time(nullptr); }

std::wstring FormatWhen(double unixTime) {
    time_t t = (time_t)unixTime, now = time(nullptr);
    struct tm a{}, b{};
    localtime_s(&a, &t);
    localtime_s(&b, &now);
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t buf[64];
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) swprintf(buf, 64, L"Today %02d:%02d", a.tm_hour, a.tm_min);
    else if (a.tm_year == b.tm_year) swprintf(buf, 64, L"%ls %d, %02d:%02d", mon[a.tm_mon], a.tm_mday, a.tm_hour, a.tm_min);
    else swprintf(buf, 64, L"%ls %d %d, %02d:%02d", mon[a.tm_mon], a.tm_mday, a.tm_year + 1900, a.tm_hour, a.tm_min);
    return buf;
}

std::wstring ErrorText(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                   (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : (L"Error " + std::to_wstring(err));
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

std::wstring FileVersion(const std::wstring& path) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return L"";
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return L"";
    // Prefer the ProductVersion string (it keeps suffixes like -beta.1)
    void* str = nullptr; UINT len = 0;
    if (VerQueryValueW(buf.data(), L"\\StringFileInfo\\040904B0\\ProductVersion", &str, &len) && len > 1) return (const wchar_t*)str;
    VS_FIXEDFILEINFO* ffi = nullptr;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len) || !ffi) return L"";
    wchar_t b[64];
    swprintf(b, 64, L"%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS));
    return b;
}

std::wstring ExpandEnv(const std::wstring& s) {
    wchar_t buf[4096];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, (DWORD)std::size(buf));
    return (n && n <= std::size(buf)) ? std::wstring(buf) : s;
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD n = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n) && el.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

// ---------------------------------------------------------------- registry
bool RegReadStr(HKEY root, const wchar_t* path, const wchar_t* value, std::wstring& out) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    LONG r = RegGetValueW(root, path, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size);
    if (r == ERROR_MORE_DATA) {
        std::vector<wchar_t> big(size / sizeof(wchar_t) + 2);
        size = (DWORD)(big.size() * sizeof(wchar_t));
        r = RegGetValueW(root, path, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, big.data(), &size);
        if (r == ERROR_SUCCESS) out = big.data();
        return r == ERROR_SUCCESS;
    }
    if (r != ERROR_SUCCESS) return false;
    out = buf;
    return true;
}

bool RegReadDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD& out) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, path, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    out = v;
    return true;
}

LONG RegWriteStr(HKEY root, const wchar_t* path, const wchar_t* value, const std::wstring& data, DWORD type) {
    HKEY h;
    LONG r = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(h, value, 0, type, (const BYTE*)data.c_str(), (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r;
}

LONG RegWriteDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD data) {
    HKEY h;
    LONG r = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(h, value, 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
    RegCloseKey(h);
    return r;
}

LONG RegRemoveValue(HKEY root, const wchar_t* path, const wchar_t* value) {
    HKEY h;
    LONG r = RegOpenKeyExW(root, path, 0, KEY_SET_VALUE, &h);
    if (r == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (r != ERROR_SUCCESS) return r;
    r = RegDeleteValueW(h, value);
    RegCloseKey(h);
    return r == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : r;
}

// ---------------------------------------------------------------- processes
bool RunCapture(const std::wstring& cmdLine, std::wstring* out, DWORD timeoutMs, DWORD* exitCode) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }
    std::string bytes;
    DWORD start = GetTickCount();
    bool timedOut = false;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;   // pipe closed: the process ended
        if (avail) {
            char chunk[8192];
            DWORD got = 0;
            if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) || !got) break;
            bytes.append(chunk, got);
            if (bytes.size() > 16 * 1024 * 1024) break;
        } else {
            if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
                if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || !avail) break;
            }
            if (GetTickCount() - start > timeoutMs) { timedOut = true; TerminateProcess(pi.hProcess, 1); break; }
        }
    }
    WaitForSingleObject(pi.hProcess, 2000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = timedOut ? 1 : code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (out) {
        // UTF-8 when it decodes cleanly (PowerShell is told to use it), else the console code page
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
        if (n > 0 || bytes.empty()) *out = Utf8ToWide(bytes);
        else {
            n = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
            out->assign(n, L'\0');
            if (n) MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), (int)bytes.size(), &(*out)[0], n);
        }
    }
    return !timedOut;
}

static std::wstring Base64(const BYTE* data, size_t len) {
    static const wchar_t* tbl = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < len) ? tbl[(v >> 6) & 63] : L'=';
        out += (i + 2 < len) ? tbl[v & 63] : L'=';
    }
    return out;
}

bool RunPowerShell(const std::wstring& script, std::wstring* out, DWORD timeoutMs, DWORD* exitCode) {
    std::wstring full = L"$ProgressPreference='SilentlyContinue';[Console]::OutputEncoding=[Text.Encoding]::UTF8;" + script;
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " +
                       Base64((const BYTE*)full.c_str(), full.size() * sizeof(wchar_t));
    return RunCapture(cmd, out, timeoutMs, exitCode);
}

bool ShellOpen(const std::wstring& target, const std::wstring& args, bool elevated) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = elevated ? L"runas" : L"open";
    sei.lpFile = target.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

// Opens a "target spec" used by link rows: a URI, a shell: path, or "program arguments".
bool OpenTarget(const std::wstring& spec) {
    if (spec.empty()) return false;
    if (spec.find(L"://") != std::wstring::npos || StartsWith(spec, L"ms-settings:") || StartsWith(spec, L"shell:") ||
        StartsWith(spec, L"windowsdefender:") || StartsWith(spec, L"ms-")) return ShellOpen(spec);
    size_t sp = spec.find(L' ');
    std::wstring prog = sp == std::wstring::npos ? spec : spec.substr(0, sp);
    std::wstring args = sp == std::wstring::npos ? L"" : spec.substr(sp + 1);
    return ShellOpen(prog, args);
}

// ---------------------------------------------------------------- background tasks
static HWND g_taskOwner = nullptr;
static std::mutex g_taskLock;
static std::condition_variable g_taskCv;
static std::deque<std::function<void()>> g_taskQueue;
static int g_taskThreads = 0, g_taskIdle = 0;

static void TaskThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(g_taskLock);
            ++g_taskIdle;
            g_taskCv.wait(lock, [] { return !g_taskQueue.empty(); });
            --g_taskIdle;
            job = std::move(g_taskQueue.front());
            g_taskQueue.pop_front();
        }
        job();
    }
}

void TasksInit(HWND owner) { g_taskOwner = owner; }

void RunAsync(std::function<void()> work) {
    std::lock_guard<std::mutex> lock(g_taskLock);
    g_taskQueue.push_back(std::move(work));
    // Threads are created on demand (up to 4), so an idle Breaker has none spinning.
    if (g_taskIdle == 0 && g_taskThreads < 4) {
        ++g_taskThreads;
        std::thread(TaskThread).detach();
    }
    g_taskCv.notify_one();
}

void Ui(std::function<void()> fn) {
    if (!g_taskOwner) return;
    auto* p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(g_taskOwner, WM_BREAKER_CALL, 0, (LPARAM)p)) delete p;
}
