// Breaker - handlers: settings that need a Windows API call instead of a plain registry value.
// Each handler reads a setting's value and writes a new one. Returning W_DENIED tells the engine to
// retry that one write from the elevated helper (one admin prompt for everything that needs it).
#include "pages.h"
#include <shlobj.h>
#include <shellapi.h>
#include <powrprof.h>
#include <winspool.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propidl.h>
#include <algorithm>
#include <ctime>
#include <set>
#include <map>
#include <mutex>
#include <cmath>

static WriteResult FromLong(LONG r, std::wstring& err) {
    if (r == ERROR_SUCCESS) return W_OK;
    if (r == ERROR_ACCESS_DENIED) return W_DENIED;
    err = ErrorText(r);
    return W_FAIL;
}
static WriteResult NeedAdmin(std::wstring& err) {
    if (!IsElevated()) return W_DENIED;
    (void)err;
    return W_OK;
}
static bool SafeArg(const std::wstring& s) {   // goes inside "quotes" on a command line
    return !s.empty() && s.find_first_of(L"\"\r\n") == std::wstring::npos;
}

// ============================================================ display
static bool CurrentMode(DEVMODEW& dm) {
    dm = {};
    dm.dmSize = sizeof(dm);
    return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) != FALSE;
}

static bool DisplayRead(const std::wstring& id, std::wstring& out) {
    DEVMODEW dm;
    if (!CurrentMode(dm)) return false;
    if (id == L"display.resolution") out = std::to_wstring(dm.dmPelsWidth) + L"x" + std::to_wstring(dm.dmPelsHeight);
    else out = std::to_wstring(dm.dmDisplayFrequency);
    return true;
}

static void DisplayChoices(const std::wstring& id, Choices& out) {
    DEVMODEW cur;
    if (!CurrentMode(cur)) return;
    std::set<std::pair<DWORD, DWORD>> sizes;
    std::set<DWORD> rates;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmBitsPerPel < 32) continue;
        if (dm.dmPelsWidth >= 800 && dm.dmPelsHeight >= 600) sizes.insert({dm.dmPelsWidth, dm.dmPelsHeight});
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight) rates.insert(dm.dmDisplayFrequency);
    }
    if (id == L"display.resolution") {
        for (auto it = sizes.rbegin(); it != sizes.rend(); ++it)
            out.push_back({std::to_wstring(it->first) + L" \x00D7 " + std::to_wstring(it->second), std::to_wstring(it->first) + L"x" + std::to_wstring(it->second)});
    } else {
        for (auto it = rates.rbegin(); it != rates.rend(); ++it) out.push_back({std::to_wstring(*it) + L" Hz", std::to_wstring(*it)});
    }
}

static WriteResult DisplayWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    DEVMODEW cur;
    if (!CurrentMode(cur)) { err = L"Couldn't read the current display mode."; return W_FAIL; }
    DWORD w = cur.dmPelsWidth, h = cur.dmPelsHeight, hz = cur.dmDisplayFrequency;
    if (id == L"display.resolution") {
        if (swscanf(v.c_str(), L"%lux%lu", &w, &h) != 2) { err = L"That isn't a resolution."; return W_FAIL; }
    } else {
        hz = wcstoul(v.c_str(), nullptr, 10);
    }
    // Pick a mode the display actually offers: exact match, else the highest refresh rate at that size
    DEVMODEW dm{}, best{};
    dm.dmSize = sizeof(dm);
    bool found = false;
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth != w || dm.dmPelsHeight != h) continue;
        if (dm.dmDisplayFrequency == hz) { best = dm; found = true; break; }
        if (!found || dm.dmDisplayFrequency > best.dmDisplayFrequency) { best = dm; found = true; }
    }
    if (!found) { err = L"The display doesn't offer that mode."; return W_FAIL; }
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    LONG r = ChangeDisplaySettingsExW(nullptr, &best, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (r == DISP_CHANGE_SUCCESSFUL) return W_OK;
    err = L"The display refused that mode.";
    return W_FAIL;
}

static bool BrightnessRead(const std::wstring&, std::wstring& out) {
    std::wstring o;
    DWORD code = 1;
    if (!RunPowerShell(L"(Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightness -ErrorAction Stop | Select-Object -First 1).CurrentBrightness", &o, 15000, &code) || code != 0) return false;
    o = Trim(o);
    if (o.empty() || !iswdigit(o[0])) return false;
    out = o;
    return true;
}

static WriteResult BrightnessWrite(const std::wstring&, const std::wstring& v, std::wstring& err) {
    int n = std::clamp(_wtoi(v.c_str()), 0, 100);
    DWORD code = 1;
    std::wstring o;
    RunPowerShell(L"$m=Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightnessMethods -ErrorAction Stop | Select-Object -First 1;"
                  L"Invoke-CimMethod -InputObject $m -MethodName WmiSetBrightness -Arguments @{Timeout=[uint32]1;Brightness=[byte]" + std::to_wstring(n) + L"} | Out-Null", &o, 15000, &code);
    if (code == 0) return W_OK;
    err = L"This screen doesn't let Windows set its brightness.";
    return W_FAIL;
}

// ============================================================ sound
static const GUID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIidEndpointVolume = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
static const GUID kClsidPolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const GUID kIidPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
static const PROPERTYKEY kKeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// The interface Windows' own sound panel uses to change the default device. Only SetDefaultEndpoint is called;
// the methods before it are declared so it lands in the right slot.
struct IPolicyConfigLite : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, INT64*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

static IMMDeviceEnumerator* AudioEnumerator() {
    IMMDeviceEnumerator* e = nullptr;
    CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, (void**)&e);
    return e;
}

static std::wstring DeviceId(IMMDevice* d) {
    LPWSTR id = nullptr;
    std::wstring s;
    if (d && SUCCEEDED(d->GetId(&id)) && id) { s = id; CoTaskMemFree(id); }
    return s;
}

static std::wstring DeviceName(IMMDevice* d) {
    IPropertyStore* ps = nullptr;
    std::wstring s;
    if (d && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kKeyFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) s = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return s;
}

static EDataFlow FlowOf(const std::wstring& id) { return id == L"sound.input" ? eCapture : eRender; }

static IAudioEndpointVolume* EndpointVolume() {
    IAudioEndpointVolume* vol = nullptr;
    IMMDeviceEnumerator* e = AudioEnumerator();
    IMMDevice* d = nullptr;
    if (e && SUCCEEDED(e->GetDefaultAudioEndpoint(eRender, eMultimedia, &d)) && d) d->Activate(kIidEndpointVolume, CLSCTX_ALL, nullptr, (void**)&vol);
    if (d) d->Release();
    if (e) e->Release();
    return vol;
}

static bool SoundRead(const std::wstring& id, std::wstring& out) {
    if (id == L"sound.volume" || id == L"sound.mute") {
        IAudioEndpointVolume* vol = EndpointVolume();
        if (!vol) return false;
        float level = 0;
        BOOL mute = FALSE;
        vol->GetMasterVolumeLevelScalar(&level);
        vol->GetMute(&mute);
        vol->Release();
        out = id == L"sound.mute" ? (mute ? L"1" : L"0") : std::to_wstring((int)std::lround(level * 100));
        return true;
    }
    IMMDeviceEnumerator* e = AudioEnumerator();
    IMMDevice* d = nullptr;
    bool ok = e && SUCCEEDED(e->GetDefaultAudioEndpoint(FlowOf(id), eMultimedia, &d)) && d;
    if (ok) out = DeviceId(d);
    if (d) d->Release();
    if (e) e->Release();
    return ok;
}

static void SoundChoices(const std::wstring& id, Choices& out) {
    if (id != L"sound.output" && id != L"sound.input") return;
    IMMDeviceEnumerator* e = AudioEnumerator();
    IMMDeviceCollection* col = nullptr;
    if (e && SUCCEEDED(e->EnumAudioEndpoints(FlowOf(id), DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(col->Item(i, &d)) && d) { out.push_back({DeviceName(d), DeviceId(d)}); d->Release(); }
        }
        col->Release();
    }
    if (e) e->Release();
}

static WriteResult SoundWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    if (id == L"sound.volume" || id == L"sound.mute") {
        IAudioEndpointVolume* vol = EndpointVolume();
        if (!vol) { err = L"No sound output device was found."; return W_FAIL; }
        HRESULT hr = id == L"sound.mute" ? vol->SetMute(v == L"1", nullptr)
                                         : vol->SetMasterVolumeLevelScalar(std::clamp(_wtoi(v.c_str()), 0, 100) / 100.0f, nullptr);
        vol->Release();
        if (SUCCEEDED(hr)) return W_OK;
        err = L"The sound device refused the change.";
        return W_FAIL;
    }
    IPolicyConfigLite* pc = nullptr;
    HRESULT hr = CoCreateInstance(kClsidPolicyConfig, nullptr, CLSCTX_ALL, kIidPolicyConfig, (void**)&pc);
    if (FAILED(hr) || !pc) { err = L"This version of Windows doesn't let other apps switch the default sound device."; return W_FAIL; }
    for (ERole role : {eConsole, eMultimedia, eCommunications}) hr = pc->SetDefaultEndpoint(v.c_str(), role);
    pc->Release();
    if (SUCCEEDED(hr)) return W_OK;
    err = L"Windows couldn't switch to that device.";
    return W_FAIL;
}

// ============================================================ power
static GUID G(const wchar_t* s) {
    GUID g{};
    std::wstring b = std::wstring(L"{") + s + L"}";
    CLSIDFromString(b.c_str(), &g);
    return g;
}
static std::wstring GuidText(const GUID& g) {
    wchar_t b[64];
    StringFromGUID2(g, b, 64);
    std::wstring s = b;
    return ToLower(s.substr(1, s.size() - 2));
}

namespace {
struct PowerIdx { const wchar_t* id; const wchar_t* sub; const wchar_t* setting; bool dc; bool toggle; };
}
static const wchar_t* kSubVideo = L"7516b95f-f776-4464-8c53-06167f40cc99";
static const wchar_t* kSubSleep = L"238c9fa8-0aad-41ed-83f4-97be242c8f20";
static const wchar_t* kSubButtons = L"4f971e89-eebd-4455-a8de-9e59040e7347";
static const wchar_t* kSubUsb = L"2a737441-1930-4402-8d77-b2bebba308a3";
static const PowerIdx kPowerIdx[] = {
    {L"power.screen.ac", kSubVideo, L"3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e", false, false},
    {L"power.screen.dc", kSubVideo, L"3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e", true, false},
    {L"power.sleep.ac", kSubSleep, L"29f6c1db-86da-48c5-9fdb-f2b67b1f44da", false, false},
    {L"power.sleep.dc", kSubSleep, L"29f6c1db-86da-48c5-9fdb-f2b67b1f44da", true, false},
    {L"power.button.ac", kSubButtons, L"7648efa3-dd9c-4e3e-b566-50f929386280", false, false},
    {L"power.button.dc", kSubButtons, L"7648efa3-dd9c-4e3e-b566-50f929386280", true, false},
    {L"power.lid.ac", kSubButtons, L"5ca83367-6e45-459f-a27b-476b1d01c936", false, false},
    {L"power.lid.dc", kSubButtons, L"5ca83367-6e45-459f-a27b-476b1d01c936", true, false},
    {L"power.usbSuspend.ac", kSubUsb, L"48e6b7a6-50f5-4782-a5d4-53bb8f07e226", false, true},
    {L"power.usbSuspend.dc", kSubUsb, L"48e6b7a6-50f5-4782-a5d4-53bb8f07e226", true, true},
};

static bool PowerRead(const std::wstring& id, std::wstring& out) {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || !active) return false;
    bool ok = false;
    if (id == L"power.plan") {
        out = GuidText(*active);
        ok = true;
    } else if (id == L"power.hibernate") {
        DWORD v = 0;
        ok = RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", v);
        out = v ? L"1" : L"0";
    } else {
        for (auto& p : kPowerIdx) {
            if (id != p.id) continue;
            GUID sub = G(p.sub), set = G(p.setting);
            DWORD v = 0;
            DWORD r = p.dc ? PowerReadDCValueIndex(nullptr, active, &sub, &set, &v) : PowerReadACValueIndex(nullptr, active, &sub, &set, &v);
            if (r == ERROR_SUCCESS) { out = p.toggle ? (v ? L"1" : L"0") : std::to_wstring(v); ok = true; }
        }
    }
    LocalFree(active);
    return ok;
}

static void PowerChoices(const std::wstring& id, Choices& out) {
    if (id != L"power.plan") return;
    for (ULONG i = 0;; ++i) {
        GUID g{};
        DWORD size = sizeof(g);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, (UCHAR*)&g, &size) != ERROR_SUCCESS) break;
        wchar_t name[256] = {};
        DWORD n = sizeof(name);
        PowerReadFriendlyName(nullptr, &g, nullptr, nullptr, (UCHAR*)name, &n);
        out.push_back({name[0] ? name : GuidText(g), GuidText(g)});
    }
}

static WriteResult PowerWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    if (id == L"power.hibernate") {
        if (NeedAdmin(err) == W_DENIED) return W_DENIED;
        DWORD code = 1;
        RunCapture(std::wstring(L"powercfg.exe /hibernate ") + (v == L"1" ? L"on" : L"off"), nullptr, 20000, &code);
        if (code == 0) return W_OK;
        err = L"This PC doesn't support hibernate.";
        return W_FAIL;
    }
    if (id == L"power.plan") {
        GUID g = G(v.c_str());
        DWORD r = PowerSetActiveScheme(nullptr, &g);
        if (r == ERROR_SUCCESS) return W_OK;
        if (r == ERROR_ACCESS_DENIED) return W_DENIED;
        err = L"That power plan isn't on this PC.";
        return W_FAIL;
    }
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || !active) { err = L"Couldn't read the active power plan."; return W_FAIL; }
    WriteResult res = W_FAIL;
    for (auto& p : kPowerIdx) {
        if (id != p.id) continue;
        GUID sub = G(p.sub), set = G(p.setting);
        DWORD val = wcstoul(v.c_str(), nullptr, 10);
        DWORD r = p.dc ? PowerWriteDCValueIndex(nullptr, active, &sub, &set, val) : PowerWriteACValueIndex(nullptr, active, &sub, &set, val);
        if (r == ERROR_SUCCESS) r = PowerSetActiveScheme(nullptr, active);   // re-apply so it takes effect now
        res = FromLong((LONG)r, err);
    }
    LocalFree(active);
    return res;
}

// ============================================================ SystemParametersInfo settings
namespace {
struct SpiDef { const wchar_t* id; UINT get, set; int kind; };   // kind 0: value in uiParam, 1: value in pvParam
}
static const SpiDef kSpi[] = {
    {L"mouse.speed", SPI_GETMOUSESPEED, SPI_SETMOUSESPEED, 1},
    {L"mouse.wheel", SPI_GETWHEELSCROLLLINES, SPI_SETWHEELSCROLLLINES, 0},
    {L"kbd.delay", SPI_GETKEYBOARDDELAY, SPI_SETKEYBOARDDELAY, 0},
    {L"kbd.rate", SPI_GETKEYBOARDSPEED, SPI_SETKEYBOARDSPEED, 0},
    {L"visual.animations", SPI_GETCLIENTAREAANIMATION, SPI_SETCLIENTAREAANIMATION, 1},
    {L"visual.dragFull", SPI_GETDRAGFULLWINDOWS, SPI_SETDRAGFULLWINDOWS, 0},
    {L"fonts.smoothing", SPI_GETFONTSMOOTHING, SPI_SETFONTSMOOTHING, 0},
};

static bool SpiRead(const std::wstring& id, std::wstring& out) {
    if (id == L"mouse.accel") {
        int m[3] = {};
        if (!SystemParametersInfoW(SPI_GETMOUSE, 0, m, 0)) return false;
        out = m[2] ? L"1" : L"0";
        return true;
    }
    if (id == L"mouse.swap") { out = GetSystemMetrics(SM_SWAPBUTTON) ? L"1" : L"0"; return true; }
    if (id == L"mouse.dblclick") { out = std::to_wstring(GetDoubleClickTime()); return true; }
    for (auto& s : kSpi) {
        if (id != s.id) continue;
        UINT v = 0;
        if (!SystemParametersInfoW(s.get, 0, &v, 0)) return false;
        out = std::to_wstring(v);
        return true;
    }
    return false;
}

static WriteResult SpiWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    const UINT flags = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    UINT n = (UINT)wcstoul(v.c_str(), nullptr, 10);
    BOOL ok = FALSE;
    if (id == L"mouse.accel") {
        int m[3] = {n ? 6 : 0, n ? 10 : 0, n ? 1 : 0};
        ok = SystemParametersInfoW(SPI_SETMOUSE, 0, m, flags);
    } else if (id == L"mouse.swap") {
        ok = SystemParametersInfoW(SPI_SETMOUSEBUTTONSWAP, n ? 1 : 0, nullptr, flags);
    } else if (id == L"mouse.dblclick") {
        ok = SystemParametersInfoW(SPI_SETDOUBLECLICKTIME, std::clamp<UINT>(n, 100, 1500), nullptr, flags);
    } else {
        for (auto& s : kSpi) {
            if (id != s.id) continue;
            ok = s.kind == 1 ? SystemParametersInfoW(s.set, 0, (PVOID)(UINT_PTR)n, flags) : SystemParametersInfoW(s.set, n, nullptr, flags);
        }
    }
    if (ok) return W_OK;
    err = ErrorText(GetLastError());
    return W_FAIL;
}

// ============================================================ wallpaper
static const wchar_t* kDesktopKey = L"Control Panel\\Desktop";

static bool WallpaperRead(const std::wstring& id, std::wstring& out) {
    if (id == L"wallpaper.path") {
        wchar_t buf[MAX_PATH * 2] = {};
        if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, (UINT)std::size(buf), buf, 0)) return false;
        out = buf;
        return true;
    }
    std::wstring style = L"10", tile = L"0";
    RegReadStr(HKEY_CURRENT_USER, kDesktopKey, L"WallpaperStyle", style);
    RegReadStr(HKEY_CURRENT_USER, kDesktopKey, L"TileWallpaper", tile);
    out = tile == L"1" ? L"t" : style;
    return true;
}

static WriteResult WallpaperWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    std::wstring path;
    if (id == L"wallpaper.path") {
        path = v;
        if (!path.empty() && !FileExists(path)) { err = L"That picture file doesn't exist any more."; return W_FAIL; }
    } else {
        WallpaperRead(L"wallpaper.path", path);
        RegWriteStr(HKEY_CURRENT_USER, kDesktopKey, L"WallpaperStyle", v == L"t" ? L"0" : v);
        RegWriteStr(HKEY_CURRENT_USER, kDesktopKey, L"TileWallpaper", v == L"t" ? L"1" : L"0");
    }
    if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) return W_OK;
    err = L"Windows couldn't use that picture.";
    return W_FAIL;
}

// ============================================================ DirectX global settings and per-app GPU choice
static const wchar_t* kGpuKey = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";

static std::wstring KvGet(const std::wstring& blob, const std::wstring& key, const std::wstring& def) {
    for (auto& part : Split(blob, L";")) {
        size_t eq = part.find(L'=');
        if (eq != std::wstring::npos && part.substr(0, eq) == key) return part.substr(eq + 1);
    }
    return def;
}
static std::wstring KvSet(const std::wstring& blob, const std::wstring& key, const std::wstring& value) {
    std::wstring out;
    bool done = false;
    for (auto& part : Split(blob, L";")) {
        size_t eq = part.find(L'=');
        if (eq != std::wstring::npos && part.substr(0, eq) == key) { out += key + L"=" + value + L";"; done = true; }
        else out += part + L";";
    }
    if (!done) out += key + L"=" + value + L";";
    return out;
}
static const wchar_t* DxKey(const std::wstring& id) {
    return id == L"gaming.windowedOpt" ? L"SwapEffectUpgradeEnable" : id == L"gaming.vrr" ? L"VRROptimizeEnable" : L"AutoHDREnable";
}

static bool DxRead(const std::wstring& id, std::wstring& out) {
    std::wstring blob;
    RegReadStr(HKEY_CURRENT_USER, kGpuKey, L"DirectXUserGlobalSettings", blob);
    out = KvGet(blob, DxKey(id), id == L"gaming.vrr" ? L"0" : L"1") == L"1" ? L"1" : L"0";
    return true;
}
static WriteResult DxWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    std::wstring blob;
    RegReadStr(HKEY_CURRENT_USER, kGpuKey, L"DirectXUserGlobalSettings", blob);
    return FromLong(RegWriteStr(HKEY_CURRENT_USER, kGpuKey, L"DirectXUserGlobalSettings", KvSet(blob, DxKey(id), v == L"1" ? L"1" : L"0")), err);
}

static bool GpuPrefRead(const std::wstring& id, std::wstring& out) {
    std::wstring blob;
    if (!RegReadStr(HKEY_CURRENT_USER, kGpuKey, id.substr(8).c_str(), blob)) { out = L"~"; return true; }
    out = KvGet(blob, L"GpuPreference", L"0");
    return true;
}
static WriteResult GpuPrefWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    std::wstring path = id.substr(8);
    if (v == L"~") return FromLong(RegRemoveValue(HKEY_CURRENT_USER, kGpuKey, path.c_str()), err);
    std::wstring blob;
    RegReadStr(HKEY_CURRENT_USER, kGpuKey, path.c_str(), blob);
    return FromLong(RegWriteStr(HKEY_CURRENT_USER, kGpuKey, path.c_str(), KvSet(blob, L"GpuPreference", v)), err);
}
static void GpuPrefChoices(const std::wstring&, Choices& out) {
    out = {{L"Let Windows decide", L"0"}, {L"Power saving", L"1"}, {L"High performance", L"2"}, {L"Remove from this list", L"~"}};
}

// ============================================================ firewall, UAC
static const wchar_t* FirewallProfile(const std::wstring& id, bool netsh) {
    if (id == L"firewall.public") return netsh ? L"publicprofile" : L"PublicProfile";
    if (id == L"firewall.domain") return netsh ? L"domainprofile" : L"DomainProfile";
    return netsh ? L"privateprofile" : L"StandardProfile";
}
static bool FirewallRead(const std::wstring& id, std::wstring& out) {
    std::wstring key = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\") + FirewallProfile(id, false);
    DWORD v = 1;
    if (!RegReadDword(HKEY_LOCAL_MACHINE, key.c_str(), L"EnableFirewall", v)) return false;
    out = v ? L"1" : L"0";
    return true;
}
static WriteResult FirewallWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    if (NeedAdmin(err) == W_DENIED) return W_DENIED;
    DWORD code = 1;
    RunCapture(std::wstring(L"netsh.exe advfirewall set ") + FirewallProfile(id, true) + L" state " + (v == L"1" ? L"on" : L"off"), nullptr, 20000, &code);
    if (code == 0) return W_OK;
    err = L"Windows Firewall refused the change. Another security product may be managing it.";
    return W_FAIL;
}

static const wchar_t* kPolSystem = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
static bool UacRead(const std::wstring&, std::wstring& out) {
    DWORD consent = 5, secure = 1;
    RegReadDword(HKEY_LOCAL_MACHINE, kPolSystem, L"ConsentPromptBehaviorAdmin", consent);
    RegReadDword(HKEY_LOCAL_MACHINE, kPolSystem, L"PromptOnSecureDesktop", secure);
    out = consent == 2 ? L"2" : consent == 0 ? L"0" : secure ? L"5" : L"4";
    return true;
}
static WriteResult UacWrite(const std::wstring&, const std::wstring& v, std::wstring& err) {
    DWORD consent = v == L"2" ? 2 : v == L"0" ? 0 : 5, secure = (v == L"4" || v == L"0") ? 0 : 1;
    LONG r = RegWriteDword(HKEY_LOCAL_MACHINE, kPolSystem, L"ConsentPromptBehaviorAdmin", consent);
    if (r == ERROR_SUCCESS) r = RegWriteDword(HKEY_LOCAL_MACHINE, kPolSystem, L"PromptOnSecureDesktop", secure);
    return FromLong(r, err);
}

// ============================================================ Windows Update pause
static const wchar_t* kWuxKey = L"SOFTWARE\\Microsoft\\WindowsUpdate\\UX\\Settings";
static const wchar_t* kPauseValues[] = {L"PauseUpdatesStartTime", L"PauseUpdatesExpiryTime", L"PauseFeatureUpdatesStartTime",
                                        L"PauseFeatureUpdatesEndTime", L"PauseQualityUpdatesStartTime", L"PauseQualityUpdatesEndTime"};

static std::wstring IsoUtc(time_t t) {
    struct tm g{};
    gmtime_s(&g, &t);
    wchar_t b[40];
    swprintf(b, 40, L"%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}
static time_t ParseIsoUtc(const std::wstring& s) {
    struct tm g{};
    if (swscanf(s.c_str(), L"%d-%d-%dT%d:%d:%d", &g.tm_year, &g.tm_mon, &g.tm_mday, &g.tm_hour, &g.tm_min, &g.tm_sec) < 3) return 0;
    g.tm_year -= 1900;
    g.tm_mon -= 1;
    return _mkgmtime(&g);
}

std::wstring WuPausedUntil() {   // "" when not paused; used by the Windows Update page
    std::wstring exp;
    if (!RegReadStr(HKEY_LOCAL_MACHINE, kWuxKey, L"PauseUpdatesExpiryTime", exp)) return L"";
    time_t t = ParseIsoUtc(exp);
    return t > time(nullptr) ? FormatWhen((double)t) : L"";
}

static bool WuPauseRead(const std::wstring&, std::wstring& out) {
    std::wstring exp;
    out = L"0";
    if (RegReadStr(HKEY_LOCAL_MACHINE, kWuxKey, L"PauseUpdatesExpiryTime", exp)) {
        double left = difftime(ParseIsoUtc(exp), time(nullptr));
        if (left > 0) out = std::to_wstring(std::clamp((int)std::ceil(left / (7 * 86400.0)), 1, 5) * 7);
    }
    return true;
}
static WriteResult WuPauseWrite(const std::wstring&, const std::wstring& v, std::wstring& err) {
    int days = _wtoi(v.c_str());
    LONG r = ERROR_SUCCESS;
    time_t now = time(nullptr);
    for (int i = 0; i < 6 && r == ERROR_SUCCESS; ++i) {
        if (days <= 0) r = RegRemoveValue(HKEY_LOCAL_MACHINE, kWuxKey, kPauseValues[i]);
        else r = RegWriteStr(HKEY_LOCAL_MACHINE, kWuxKey, kPauseValues[i], IsoUtc((i == 0 || i == 2 || i == 4) ? now : now + days * 86400));
    }
    return FromLong(r, err);
}

// ============================================================ printers
static bool PrinterRead(const std::wstring&, std::wstring& out) {
    wchar_t buf[512];
    DWORD n = (DWORD)std::size(buf);
    if (!GetDefaultPrinterW(buf, &n)) { out = L""; return true; }
    out = buf;
    return true;
}
static void PrinterChoices(const std::wstring&, Choices& out) {
    DWORD needed = 0, count = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &needed, &count);
    if (!needed) return;
    std::vector<BYTE> buf(needed);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buf.data(), needed, &needed, &count)) return;
    auto* info = (PRINTER_INFO_4W*)buf.data();
    for (DWORD i = 0; i < count; ++i) out.push_back({info[i].pPrinterName, info[i].pPrinterName});
}
static WriteResult PrinterWrite(const std::wstring&, const std::wstring& v, std::wstring& err) {
    if (SetDefaultPrinterW(v.c_str())) return W_OK;
    err = ErrorText(GetLastError());
    return W_FAIL;
}

// ============================================================ services
static bool SvcRead(const std::wstring& id, std::wstring& out) {
    std::wstring name = id.substr(id.find(L':') + 1);
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (svc) {
        if (StartsWith(id, L"svcrun:")) {
            SERVICE_STATUS st{};
            ok = QueryServiceStatus(svc, &st) != FALSE;
            out = st.dwCurrentState == SERVICE_RUNNING ? L"start" : L"stop";
        } else {
            BYTE buf[8192];
            DWORD needed = 0;
            auto* cfg = (QUERY_SERVICE_CONFIGW*)buf;
            if (QueryServiceConfigW(svc, cfg, sizeof(buf), &needed)) {
                ok = true;
                out = cfg->dwStartType == SERVICE_DISABLED ? L"disabled" : cfg->dwStartType == SERVICE_DEMAND_START ? L"manual" : L"auto";
                if (out == L"auto") {
                    SERVICE_DELAYED_AUTO_START_INFO d{};
                    if (QueryServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, (BYTE*)&d, sizeof(d), &needed) && d.fDelayedAutostart) out = L"delayed";
                }
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}
static void SvcChoices(const std::wstring& id, Choices& out) {
    if (StartsWith(id, L"svc:")) out = {{L"Automatic", L"auto"}, {L"Automatic (delayed)", L"delayed"}, {L"Manual", L"manual"}, {L"Disabled", L"disabled"}};
}
static WriteResult SvcWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    std::wstring name = id.substr(id.find(L':') + 1);
    bool run = StartsWith(id, L"svcrun:");
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { err = ErrorText(GetLastError()); return W_FAIL; }
    SC_HANDLE svc = OpenServiceW(scm, name.c_str(), run ? (SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS) : SERVICE_CHANGE_CONFIG);
    DWORD e = svc ? ERROR_SUCCESS : GetLastError();
    if (svc) {
        if (run) {
            SERVICE_STATUS st{};
            if (v == L"stop" || v == L"restart") {
                if (!ControlService(svc, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) e = GetLastError();
                for (int i = 0; i < 40 && e == ERROR_SUCCESS && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(250);
            }
            if (e == ERROR_SUCCESS && (v == L"start" || v == L"restart")) {
                if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) e = GetLastError();
                for (int i = 0; i < 40 && e == ERROR_SUCCESS && QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_START_PENDING; ++i) Sleep(250);
            }
        } else {
            DWORD type = v == L"disabled" ? SERVICE_DISABLED : v == L"manual" ? SERVICE_DEMAND_START : SERVICE_AUTO_START;
            if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) e = GetLastError();
            else if (type == SERVICE_AUTO_START) {
                SERVICE_DELAYED_AUTO_START_INFO d{v == L"delayed"};
                ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &d);
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return FromLong((LONG)e, err);
}

// ============================================================ environment variables  (env:u:NAME, env:s:NAME)
static const wchar_t* kSysEnv = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
static bool EnvRead(const std::wstring& id, std::wstring& out) {
    bool sys = id[4] == L's';
    if (!RegReadStr(sys ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, sys ? kSysEnv : L"Environment", id.substr(6).c_str(), out)) out = L"~";
    return true;
}
static WriteResult EnvWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    bool sys = id[4] == L's';
    HKEY root = sys ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const wchar_t* key = sys ? kSysEnv : L"Environment";
    std::wstring name = id.substr(6);
    if (name.empty() || name.find(L'=') != std::wstring::npos) { err = L"That isn't a valid variable name."; return W_FAIL; }
    LONG r = v == L"~" ? RegRemoveValue(root, key, name.c_str())
                       : RegWriteStr(root, key, name.c_str(), v, v.find(L'%') != std::wstring::npos ? REG_EXPAND_SZ : REG_SZ);
    if (r == ERROR_SUCCESS) SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment");
    return FromLong(r, err);
}

// ============================================================ startup apps  (startup:<where>|<name>)
// where: hkcu, hklm, hklm32, folder (your Startup folder), cfolder (everyone's Startup folder)
static bool StartupWhere(const std::wstring& id, HKEY& root, std::wstring& key, std::wstring& name) {
    size_t bar = id.find(L'|');
    if (bar == std::wstring::npos) return false;
    std::wstring where = id.substr(8, bar - 8);
    name = id.substr(bar + 1);
    root = (where == L"hkcu" || where == L"folder") ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
    key += (where == L"folder" || where == L"cfolder") ? L"StartupFolder" : where == L"hklm32" ? L"Run32" : L"Run";
    return true;
}
static bool StartupRead(const std::wstring& id, std::wstring& out) {
    HKEY root;
    std::wstring key, name;
    if (!StartupWhere(id, root, key, name)) return false;
    BYTE data[16] = {};
    DWORD size = sizeof(data);
    out = L"1";   // no entry means Windows never disabled it
    if (RegGetValueW(root, key.c_str(), name.c_str(), RRF_RT_REG_BINARY, nullptr, data, &size) == ERROR_SUCCESS && size >= 1 && (data[0] & 1)) out = L"0";
    return true;
}
static WriteResult StartupWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    HKEY root, h;
    std::wstring key, name;
    if (!StartupWhere(id, root, key, name)) { err = L"Unknown startup item."; return W_FAIL; }
    BYTE data[12] = {};
    data[0] = v == L"1" ? 2 : 3;
    if (v != L"1") {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        memcpy(data + 4, &ft, 8);
    }
    LONG r = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (r == ERROR_SUCCESS) {
        r = RegSetValueExW(h, name.c_str(), 0, REG_BINARY, data, sizeof(data));
        RegCloseKey(h);
    }
    return FromLong(r, err);
}

// ============================================================ default apps (read only)  (assoc:.pdf, assoc:http)
static bool AssocRead(const std::wstring& id, std::wstring& out) {
    std::wstring what = id.substr(6);
    std::wstring key = what[0] == L'.' ? L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + what + L"\\UserChoice"
                                       : L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\" + what + L"\\UserChoice";
    if (!RegReadStr(HKEY_CURRENT_USER, key.c_str(), L"ProgId", out)) out = L"";
    return true;
}

// ============================================================ Store apps  (appx:<package name>)
// One PowerShell call lists every Store app; the Debloat page, the watchdog and undo all share it.
static std::mutex g_appxLock;
static std::set<std::wstring> g_appx;
static DWORD g_appxAt = 0;
static bool g_appxOk = false;

bool InstalledAppx(std::set<std::wstring>& out, DWORD maxAgeMs) {
    std::lock_guard<std::mutex> lock(g_appxLock);
    if (!g_appxOk || GetTickCount() - g_appxAt > maxAgeMs) {
        std::wstring o;
        g_appxOk = RunPowerShell(L"Get-AppxPackage | ForEach-Object { $_.Name }", &o, 60000);
        g_appx.clear();
        for (auto& line : Split(o, L"\n")) { std::wstring t = ToLower(Trim(line)); if (!t.empty()) g_appx.insert(t); }
        g_appxAt = GetTickCount();
    }
    out = g_appx;
    return g_appxOk;
}

static void AppxForget() {
    std::lock_guard<std::mutex> lock(g_appxLock);
    g_appxOk = false;
}

static bool AppxRead(const std::wstring& id, std::wstring& out) {
    std::set<std::wstring> apps;
    if (!InstalledAppx(apps, 60000)) return false;
    out = apps.count(ToLower(id.substr(5))) ? L"1" : L"0";
    return true;
}
static WriteResult AppxWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    std::wstring name = id.substr(5), o;
    AppxForget();
    if (name.find_first_of(L"'\"`$;|&") != std::wstring::npos) { err = L"Unknown app."; return W_FAIL; }
    DWORD code = 1;
    if (v == L"0") {
        RunPowerShell(L"$ErrorActionPreference='Stop';Get-AppxPackage -Name '" + name + L"' | Remove-AppxPackage", &o, 120000, &code);
        if (code == 0) return W_OK;
        err = L"Windows wouldn't remove this app. Some built-in apps are protected.";
        return W_FAIL;
    }
    // Put it back from the copy Windows keeps for new users. Needs administrator rights to look that up.
    if (NeedAdmin(err) == W_DENIED) return W_DENIED;
    RunPowerShell(L"$ErrorActionPreference='Stop';$p=Get-AppxPackage -AllUsers -Name '" + name + L"' | Select-Object -First 1;"
                  L"if(-not $p){exit 3};Add-AppxPackage -DisableDevelopmentMode -Register (Join-Path $p.InstallLocation 'AppXManifest.xml')", &o, 180000, &code);
    if (code == 0) return W_OK;
    err = L"Windows no longer has a copy of this app. Reinstall it from the Microsoft Store.";
    return W_FAIL;
}

// ============================================================ DNS per adapter  (dns:<adapter name>)
static std::wstring AdapterGuid(const std::wstring& friendly) {
    ULONG size = 32 * 1024;
    std::vector<BYTE> buf(size);
    auto* a = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, a, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        a = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, a, &size) != ERROR_SUCCESS) return L"";
    }
    for (; a; a = a->Next)
        if (friendly == a->FriendlyName) return Utf8ToWide(a->AdapterName);
    return L"";
}
static bool DnsRead(const std::wstring& id, std::wstring& out) {
    std::wstring guid = AdapterGuid(id.substr(4));
    if (guid.empty()) return false;
    std::wstring key = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + guid, servers;
    RegReadStr(HKEY_LOCAL_MACHINE, key.c_str(), L"NameServer", servers);
    out = servers.empty() ? L"auto" : ReplaceAll(servers, L" ", L",");
    return true;
}
static void DnsChoices(const std::wstring& id, Choices& out) {
    out = {{L"Automatic (from your router)", L"auto"}, {L"Cloudflare  1.1.1.1", L"1.1.1.1,1.0.0.1"}, {L"Google  8.8.8.8", L"8.8.8.8,8.8.4.4"},
           {L"Quad9  9.9.9.9 (blocks malware)", L"9.9.9.9,149.112.112.112"}, {L"AdGuard (blocks ads)", L"94.140.14.14,94.140.15.15"},
           {L"OpenDNS", L"208.67.222.222,208.67.220.220"}};
    std::wstring cur;
    if (DnsRead(id, cur)) {
        bool listed = false;
        for (auto& c : out) if (c.second == cur) listed = true;
        if (!listed) out.push_back({cur, cur});
    }
}
static WriteResult DnsWrite(const std::wstring& id, const std::wstring& v, std::wstring& err) {
    std::wstring name = id.substr(4);
    if (!SafeArg(name)) { err = L"Unknown network adapter."; return W_FAIL; }
    if (v != L"auto" && v.find_first_not_of(L"0123456789abcdefABCDEF.:,") != std::wstring::npos) { err = L"That isn't a DNS server address."; return W_FAIL; }
    if (NeedAdmin(err) == W_DENIED) return W_DENIED;
    DWORD code = 1;
    std::wstring base = L"netsh.exe interface ipv4 ", o;
    if (v == L"auto") {
        RunCapture(base + L"set dnsservers name=\"" + name + L"\" source=dhcp", &o, 20000, &code);
    } else {
        auto servers = Split(v, L",");
        RunCapture(base + L"set dnsservers name=\"" + name + L"\" source=static address=" + servers[0] + L" register=primary validate=no", &o, 20000, &code);
        for (size_t i = 1; i < servers.size() && code == 0; ++i)
            RunCapture(base + L"add dnsservers name=\"" + name + L"\" address=" + servers[i] + L" index=" + std::to_wstring(i + 1) + L" validate=no", &o, 20000, &code);
    }
    if (code == 0) return W_OK;
    err = Trim(o).empty() ? L"Windows refused the DNS change." : Trim(o);
    return W_FAIL;
}

// ============================================================ System Restore point  (sys.restorePoint = description)
static bool RestorePointRead(const std::wstring&, std::wstring&) { return false; }
static WriteResult RestorePointWrite(const std::wstring&, const std::wstring& v, std::wstring& err) {
    if (!IsElevated()) return W_DENIED;
    // Windows makes at most one restore point a day unless told otherwise; lift that just for this one
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore";
    DWORD oldFreq = 0;
    bool hadFreq = RegReadDword(HKEY_LOCAL_MACHINE, key, L"SystemRestorePointCreationFrequency", oldFreq);
    RegWriteDword(HKEY_LOCAL_MACHINE, key, L"SystemRestorePointCreationFrequency", 0);
    std::wstring desc = v.empty() ? L"Breaker" : v, o;
    desc = ReplaceAll(ReplaceAll(desc, L"'", L""), L"`", L"");
    DWORD code = 1;
    RunPowerShell(L"$ErrorActionPreference='Stop';Checkpoint-Computer -Description '" + desc.substr(0, 60) + L"' -RestorePointType MODIFY_SETTINGS", &o, 5 * 60 * 1000, &code);
    if (hadFreq) RegWriteDword(HKEY_LOCAL_MACHINE, key, L"SystemRestorePointCreationFrequency", oldFreq);
    else RegRemoveValue(HKEY_LOCAL_MACHINE, key, L"SystemRestorePointCreationFrequency");
    if (code == 0) return W_OK;
    std::wstring low = ToLower(o);
    err = (low.find(L"disabled") != std::wstring::npos || low.find(L"service cannot be started") != std::wstring::npos)
              ? L"System Protection is off for the system drive"
              : L"Windows couldn't create a restore point";
    return W_FAIL;
}

// ============================================================ registration
static const Handler kHandlers[] = {
    {L"display.resolution", DisplayRead, DisplayWrite, DisplayChoices, 0},
    {L"display.refresh", DisplayRead, DisplayWrite, DisplayChoices, 0},
    {L"display.brightness", BrightnessRead, BrightnessWrite, nullptr, 0},
    {L"sound.", SoundRead, SoundWrite, SoundChoices, 0},
    {L"power.", PowerRead, PowerWrite, PowerChoices, 0},
    {L"mouse.", SpiRead, SpiWrite, nullptr, 0},
    {L"kbd.delay", SpiRead, SpiWrite, nullptr, 0},
    {L"kbd.rate", SpiRead, SpiWrite, nullptr, 0},
    {L"visual.animations", SpiRead, SpiWrite, nullptr, 0},
    {L"visual.dragFull", SpiRead, SpiWrite, nullptr, 0},
    {L"fonts.smoothing", SpiRead, SpiWrite, nullptr, 0},
    {L"wallpaper.path", WallpaperRead, WallpaperWrite, nullptr, 0},
    {L"wallpaper.fit", WallpaperRead, WallpaperWrite, nullptr, 0},
    {L"gaming.windowedOpt", DxRead, DxWrite, nullptr, 0},
    {L"gaming.vrr", DxRead, DxWrite, nullptr, 0},
    {L"gaming.autoHdr", DxRead, DxWrite, nullptr, 0},
    {L"gpupref:", GpuPrefRead, GpuPrefWrite, GpuPrefChoices, F_NOSNAP},
    {L"firewall.", FirewallRead, FirewallWrite, nullptr, 0},
    {L"sec.uac", UacRead, UacWrite, nullptr, 0},
    {L"wu.pause", WuPauseRead, WuPauseWrite, nullptr, F_NOWATCH | F_NOSNAP},
    {L"printer.default", PrinterRead, PrinterWrite, PrinterChoices, 0},
    {L"svc:", SvcRead, SvcWrite, SvcChoices, F_RISKY},
    {L"svcrun:", SvcRead, SvcWrite, nullptr, F_NOHIST | F_NOWATCH},
    {L"env:", EnvRead, EnvWrite, nullptr, 0},
    {L"startup:", StartupRead, StartupWrite, nullptr, 0},
    {L"assoc:", AssocRead, nullptr, nullptr, F_NOSNAP},
    {L"appx:", AppxRead, AppxWrite, nullptr, 0},
    {L"dns:", DnsRead, DnsWrite, DnsChoices, F_NOSNAP},
    {L"sys.restorePoint", RestorePointRead, RestorePointWrite, nullptr, F_NOHIST | F_NOSNAP | F_NOWATCH},
};

void RegisterProviderHandlers() { RegisterHandlers(kHandlers, (int)std::size(kHandlers)); }
