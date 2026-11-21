// Breaker - the page list, and the builders that find things on this PC (drives, adapters, apps, services...)
// Builders run on a background thread and may be slow; the window shows the page straight away and
// drops these rows in when they arrive.
#include "pages.h"
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <ras.h>
#include <raserror.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <winspool.h>
#include <algorithm>
#include <set>
#include <map>

std::wstring WuPausedUntil();   // handlers.cpp

// ============================================================ helpers
static Row Info(const wchar_t* group, const std::wstring& title, const std::wstring& desc, const std::wstring& valueText = L"") {
    Row r;
    r.group = group;
    r.title = title;
    r.desc = desc;
    r.valueText = valueText;
    return r;
}

static std::wstring Bytes(uint64_t b) {
    const wchar_t* units[] = {L"bytes", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1024 && u < 4) { v /= 1024; ++u; }
    wchar_t buf[48];
    swprintf(buf, 48, (u == 0 || v >= 100) ? L"%.0f %ls" : L"%.1f %ls", v, units[u]);
    return buf;
}

static void SortByTitle(std::vector<Row>& rows, size_t from = 0) {
    std::sort(rows.begin() + from, rows.end(), [](const Row& a, const Row& b) { return _wcsicmp(a.title.c_str(), b.title.c_str()) < 0; });
}

// Runs a command line the way the shell would (so installers that need admin rights can ask for them).
static bool LaunchCommandLine(const std::wstring& cmd) {
    std::wstring exe, args;
    if (!cmd.empty() && cmd[0] == L'"') {
        size_t q = cmd.find(L'"', 1);
        if (q == std::wstring::npos) return false;
        exe = cmd.substr(1, q - 1);
        args = Trim(cmd.substr(q + 1));
    } else {
        std::wstring low = ToLower(cmd);
        size_t e = low.find(L".exe");
        if (e == std::wstring::npos) { size_t sp = cmd.find(L' '); exe = cmd.substr(0, sp); args = sp == std::wstring::npos ? L"" : cmd.substr(sp + 1); }
        else { exe = cmd.substr(0, e + 4); args = Trim(cmd.substr(e + 4)); }
    }
    return ShellOpen(exe, args);
}

static std::vector<IP_ADAPTER_ADDRESSES*> Adapters(std::vector<BYTE>& buf) {
    std::vector<IP_ADAPTER_ADDRESSES*> out;
    ULONG size = 48 * 1024;
    buf.resize(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS;
    DWORD r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (r != ERROR_SUCCESS) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next)
        if (a->IfType != IF_TYPE_SOFTWARE_LOOPBACK && a->IfType != IF_TYPE_TUNNEL) out.push_back(a);
    return out;
}

static std::wstring AddrText(SOCKET_ADDRESS& sa) {
    wchar_t buf[80] = {};
    if (!sa.lpSockaddr) return L"";
    if (sa.lpSockaddr->sa_family == AF_INET) {
        const BYTE* b = (const BYTE*)&((sockaddr_in*)sa.lpSockaddr)->sin_addr;
        swprintf(buf, 80, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    } else if (sa.lpSockaddr->sa_family == AF_INET6) {
        const BYTE* b = (const BYTE*)&((sockaddr_in6*)sa.lpSockaddr)->sin6_addr;
        swprintf(buf, 80, L"%x:%x:%x:%x:%x:%x:%x:%x", (b[0] << 8) | b[1], (b[2] << 8) | b[3], (b[4] << 8) | b[5], (b[6] << 8) | b[7],
                 (b[8] << 8) | b[9], (b[10] << 8) | b[11], (b[12] << 8) | b[13], (b[14] << 8) | b[15]);
    }
    return buf;
}

// ============================================================ System
static void BuildStorage(std::vector<Row>& rows) {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        ULARGE_INTEGER freeBytes{}, total{};
        if (!GetDiskFreeSpaceExW(root, &freeBytes, &total, nullptr) || !total.QuadPart) continue;
        wchar_t label[128] = {};
        GetVolumeInformationW(root, label, 128, nullptr, nullptr, nullptr, nullptr, 0);
        std::wstring letter(root, 2);
        Row r = Info(L"Drives", (label[0] ? std::wstring(label) : std::wstring(type == DRIVE_REMOVABLE ? L"Removable drive" : L"Local disk")) + L" (" + letter + L")",
                     Bytes(freeBytes.QuadPart) + L" free of " + Bytes(total.QuadPart));
        r.bar = 1.0f - (float)((double)freeBytes.QuadPart / (double)total.QuadPart);
        r.alert = r.bar > 0.92f;
        std::wstring path = root;
        r.actions.push_back({L"Open", [path] { ShellOpen(path); }});
        rows.push_back(std::move(r));
    }
    SHQUERYRBINFO rb{sizeof(rb)};
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb))) {
        Row r = Info(L"Drives", L"Recycle Bin", std::to_wstring(rb.i64NumItems) + L" items, " + Bytes((uint64_t)rb.i64Size));
        if (rb.i64NumItems > 0)
            r.actions.push_back({L"Empty", [] {
                if (!UiConfirm(L"Permanently delete everything in the Recycle Bin?\n\nThis can't be undone.")) return;
                SHEmptyRecycleBinW(UiWindow(), nullptr, SHERB_NOCONFIRMATION);
                UiReloadPage();
            }});
        rows.push_back(std::move(r));
    }
}

static void BuildAbout(std::vector<Row>& rows) {
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring product, display, build, cpu;
    DWORD ubr = 0;
    RegReadStr(HKEY_LOCAL_MACHINE, nt, L"ProductName", product);
    RegReadStr(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion", display);
    RegReadStr(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild", build);
    RegReadDword(HKEY_LOCAL_MACHINE, nt, L"UBR", ubr);
    if (_wtoi(build.c_str()) >= 22000) product = ReplaceAll(product, L"Windows 10", L"Windows 11");
    RegReadStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", cpu);
    wchar_t name[256] = {};
    DWORD n = (DWORD)std::size(name);
    GetComputerNameExW(ComputerNameDnsHostname, name, &n);
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    DISPLAY_DEVICEW dd{sizeof(dd)};
    std::wstring gpu;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i)
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) gpu = dd.DeviceString;
    ULONGLONG up = GetTickCount64() / 1000;
    wchar_t upText[64];
    swprintf(upText, 64, L"%llu d %llu h %llu min", up / 86400, (up % 86400) / 3600, (up % 3600) / 60);

    Row pc = Info(L"This PC", L"Device name", L"The name other devices see on the network.", name);
    pc.actions.push_back({L"Rename", [] { OpenTarget(L"ms-settings:about"); }});
    rows.push_back(pc);
    rows.push_back(Info(L"This PC", L"Processor", L"", Trim(cpu)));
    rows.push_back(Info(L"This PC", L"Memory", L"", Bytes(ms.ullTotalPhys) + L" (" + std::to_wstring(ms.dwMemoryLoad) + L"% in use)"));
    if (!gpu.empty()) rows.push_back(Info(L"This PC", L"Graphics", L"", gpu));
    rows.push_back(Info(L"This PC", L"Running since restart", L"", upText));
    rows.push_back(Info(L"Windows", L"Edition", L"", product));
    rows.push_back(Info(L"Windows", L"Version", L"", display + L"  (build " + build + L"." + std::to_wstring(ubr) + L")"));
    Row act = Info(L"Windows", L"Activation and product key", L"Windows only shows activation in its own Settings.");
    act.ctl = CtlLink;
    act.target = L"ms-settings:activation";
    rows.push_back(act);
}

// ============================================================ Devices
static void BuildPrinters(std::vector<Row>& rows) {
    DWORD needed = 0, count = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &needed, &count);
    if (!needed) { rows.push_back(Info(L"Installed printers", L"No printers installed", L"Add one in Windows Settings.")); return; }
    std::vector<BYTE> buf(needed);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buf.data(), needed, &needed, &count)) return;
    auto* info = (PRINTER_INFO_4W*)buf.data();
    wchar_t def[512] = {};
    DWORD dn = (DWORD)std::size(def);
    GetDefaultPrinterW(def, &dn);
    for (DWORD i = 0; i < count; ++i) {
        std::wstring name = info[i].pPrinterName;
        Row r = Info(L"Installed printers", name, (info[i].Attributes & PRINTER_ATTRIBUTE_NETWORK) ? L"Network printer" : L"Local printer", name == def ? L"Default" : L"");
        r.actions.push_back({L"Open queue", [name] { ShellOpen(L"rundll32.exe", L"printui.dll,PrintUIEntry /o /n \"" + name + L"\""); }});
        if (name != def) r.actions.push_back({L"Make default", [name] { UiApply({{L"printer.default", name, L""}}); }});
        rows.push_back(std::move(r));
    }
}

static void BuildControllers(std::vector<Row>& rows) {
    struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    struct XBattery { BYTE type, level; };
    HMODULE x = LoadLibraryW(L"xinput1_4.dll");
    using GetStateFn = DWORD(WINAPI*)(DWORD, XState*);
    using GetBatteryFn = DWORD(WINAPI*)(DWORD, BYTE, XBattery*);
    auto getState = x ? (GetStateFn)GetProcAddress(x, "XInputGetState") : nullptr;
    auto getBattery = x ? (GetBatteryFn)GetProcAddress(x, "XInputGetBatteryInformation") : nullptr;
    int found = 0;
    for (DWORD i = 0; getState && i < 4; ++i) {
        XState st{};
        if (getState(i, &st) != ERROR_SUCCESS) continue;
        ++found;
        std::wstring battery = L"Connected";
        XBattery b{};
        if (getBattery && getBattery(i, 0, &b) == ERROR_SUCCESS) {
            if (b.type == 1) battery = L"Connected, wired";
            else if (b.type == 2 || b.type == 3) battery = std::wstring(L"Battery ") + (b.level == 0 ? L"empty" : b.level == 1 ? L"low" : b.level == 2 ? L"medium" : L"full");
        }
        rows.push_back(Info(L"Connected controllers", L"Controller " + std::to_wstring(i + 1), L"Xbox-compatible (XInput)", battery));
    }
    if (!found) rows.push_back(Info(L"Connected controllers", L"No controller connected", L"Xbox-compatible controllers show up here when plugged in or paired."));
    if (x) FreeLibrary(x);
}

// ============================================================ Network
static void BuildNetwork(std::vector<Row>& rows) {
    std::vector<BYTE> buf;
    for (auto* a : Adapters(buf)) {
        bool up = a->OperStatus == IfOperStatusUp;
        std::wstring ip, dns;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            if (u->Address.lpSockaddr->sa_family == AF_INET) ip = AddrText(u->Address);
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
            std::wstring s = AddrText(d->Address);
            if (s.find(L"fec0:") == 0) continue;
            dns += (dns.empty() ? L"" : L", ") + s;
        }
        wchar_t mac[32] = {};
        if (a->PhysicalAddressLength == 6)
            swprintf(mac, 32, L"%02X-%02X-%02X-%02X-%02X-%02X", a->PhysicalAddress[0], a->PhysicalAddress[1], a->PhysicalAddress[2],
                     a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
        std::wstring desc = a->Description;
        if (up && !ip.empty()) desc += L"\nIP " + ip + (dns.empty() ? L"" : L"   DNS " + dns);
        if (mac[0]) desc += std::wstring(up && !ip.empty() ? L"   " : L"\n") + L"MAC " + mac;
        std::wstring speed;
        if (up && a->TransmitLinkSpeed && a->TransmitLinkSpeed < 100000000000ULL) {
            double mbps = a->TransmitLinkSpeed / 1e6;
            wchar_t sb[40];
            swprintf(sb, 40, mbps >= 1000 ? L"%.1f Gbps" : L"%.0f Mbps", mbps >= 1000 ? mbps / 1000 : mbps);
            speed = sb;
        }
        Row r = Info(L"Adapters", a->FriendlyName, desc, up ? (speed.empty() ? L"Connected" : L"Connected, " + speed) : L"Not connected");
        rows.push_back(std::move(r));
    }
    // Saved Wi-Fi networks
    HANDLE wlan = nullptr;
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &wlan) == ERROR_SUCCESS) {
        WLAN_INTERFACE_INFO_LIST* ifs = nullptr;
        if (WlanEnumInterfaces(wlan, nullptr, &ifs) == ERROR_SUCCESS && ifs) {
            for (DWORD i = 0; i < ifs->dwNumberOfItems; ++i) {
                GUID guid = ifs->InterfaceInfo[i].InterfaceGuid;
                std::wstring connected;
                WLAN_CONNECTION_ATTRIBUTES* ca = nullptr;
                DWORD size = 0;
                if (WlanQueryInterface(wlan, &guid, wlan_intf_opcode_current_connection, nullptr, &size, (PVOID*)&ca, nullptr) == ERROR_SUCCESS && ca) {
                    if (ca->isState == wlan_interface_state_connected) connected = ca->strProfileName;
                    WlanFreeMemory(ca);
                }
                WLAN_PROFILE_INFO_LIST* profiles = nullptr;
                if (WlanGetProfileList(wlan, &guid, nullptr, &profiles) == ERROR_SUCCESS && profiles) {
                    for (DWORD k = 0; k < profiles->dwNumberOfItems; ++k) {
                        std::wstring name = profiles->ProfileInfo[k].strProfileName;
                        Row r = Info(L"Saved Wi-Fi networks", name, L"Windows connects to this network automatically when it's in range.", name == connected ? L"Connected" : L"");
                        r.actions.push_back({L"Forget", [name, guid] {
                            if (!UiConfirm(L"Forget the Wi-Fi network \"" + name + L"\"?\n\nYou'll need its password to connect again.")) return;
                            HANDLE h = nullptr;
                            DWORD v = 0;
                            if (WlanOpenHandle(2, nullptr, &v, &h) == ERROR_SUCCESS) {
                                DWORD e = WlanDeleteProfile(h, &guid, name.c_str(), nullptr);
                                WlanCloseHandle(h, nullptr);
                                UiToast(e == ERROR_SUCCESS ? L"Forgot " + name : L"Windows wouldn't forget that network: " + ErrorText(e));
                            }
                            UiReloadPage();
                        }});
                        rows.push_back(std::move(r));
                    }
                    WlanFreeMemory(profiles);
                }
            }
            WlanFreeMemory(ifs);
        }
        WlanCloseHandle(wlan, nullptr);
    }
}

static void BuildDns(std::vector<Row>& rows) {
    std::vector<BYTE> buf;
    for (auto* a : Adapters(buf)) {
        if (a->OperStatus != IfOperStatusUp) continue;
        Row r;
        r.id = std::wstring(L"dns:") + a->FriendlyName;
        r.group = L"DNS servers";
        r.title = a->FriendlyName;
        r.label = std::wstring(L"DNS for ") + a->FriendlyName;
        r.desc = L"DNS turns website names into addresses. A faster or filtering DNS can speed up browsing or block ads and malware.";
        r.detail = L"Runs \"netsh interface ipv4 set dnsservers\" for this adapter. Automatic goes back to the servers your router hands out.";
        r.ctl = CtlChoice;
        r.flags = F_ADMIN;
        GetChoices(r.id, r.choices);
        ReadValue(r.id, r.value);
        rows.push_back(std::move(r));
    }
    if (rows.empty()) rows.push_back(Info(L"DNS servers", L"No network connection", L"Connect to a network to choose its DNS servers."));
}

static void BuildVpn(std::vector<Row>& rows) {
    DWORD size = 0, count = 0;
    RASENTRYNAMEW probe{};
    probe.dwSize = sizeof(probe);
    size = sizeof(probe);
    DWORD r = RasEnumEntriesW(nullptr, nullptr, &probe, &size, &count);
    std::vector<BYTE> buf;
    RASENTRYNAMEW* list = &probe;
    if (r == ERROR_BUFFER_TOO_SMALL) {
        buf.resize(size);
        list = (RASENTRYNAMEW*)buf.data();
        list[0].dwSize = sizeof(RASENTRYNAMEW);
        r = RasEnumEntriesW(nullptr, nullptr, list, &size, &count);
    }
    if (r != ERROR_SUCCESS || count == 0) {
        rows.push_back(Info(L"VPN connections", L"No VPN connections", L"VPNs you add in Windows Settings appear here."));
        return;
    }
    for (DWORD i = 0; i < count; ++i) {
        std::wstring name = list[i].szEntryName;
        Row row = Info(L"VPN connections", name, L"A VPN connection saved in Windows.");
        row.actions.push_back({L"Connect", [name] { ShellOpen(L"rasphone.exe", L"-d \"" + name + L"\""); }});
        row.actions.push_back({L"Disconnect", [name] { ShellOpen(L"rasphone.exe", L"-h \"" + name + L"\""); }});
        rows.push_back(std::move(row));
    }
}

static void BuildData(std::vector<Row>& rows) {
    std::vector<BYTE> buf;
    for (auto* a : Adapters(buf)) {
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = a->Luid;
        if (GetIfEntry2(&row) != NO_ERROR || (row.InOctets == 0 && row.OutOctets == 0)) continue;
        rows.push_back(Info(L"Since the last restart", a->FriendlyName, a->Description,
                            L"\x2193 " + Bytes(row.InOctets) + L"    \x2191 " + Bytes(row.OutOctets)));
    }
    if (rows.empty()) rows.push_back(Info(L"Since the last restart", L"No traffic yet", L""));
}

// ============================================================ Apps
static void EnumUninstall(HKEY root, REGSAM view, std::vector<Row>& rows, std::set<std::wstring>& seen) {
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY h;
    if (RegOpenKeyExW(root, base, 0, KEY_READ | view, &h) != ERROR_SUCCESS) return;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = (DWORD)std::size(sub);
        if (RegEnumKeyExW(h, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(h, sub, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
        auto str = [&](const wchar_t* v) {
            wchar_t b[1024];
            DWORD size = sizeof(b);
            return RegGetValueW(k, nullptr, v, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, b, &size) == ERROR_SUCCESS ? std::wstring(b) : std::wstring();
        };
        auto num = [&](const wchar_t* v) {
            DWORD d = 0, size = sizeof(d);
            return RegGetValueW(k, nullptr, v, RRF_RT_REG_DWORD, nullptr, &d, &size) == ERROR_SUCCESS ? d : 0;
        };
        std::wstring name = str(L"DisplayName"), uninstall = str(L"UninstallString");
        bool skip = name.empty() || num(L"SystemComponent") || !str(L"ParentKeyName").empty() || uninstall.empty();
        if (!skip && seen.insert(ToLower(name)).second) {
            if (num(L"WindowsInstaller")) uninstall = std::wstring(L"msiexec.exe /x") + sub;
            std::wstring desc = str(L"Publisher"), ver = str(L"DisplayVersion");
            DWORD kb = num(L"EstimatedSize");
            if (!ver.empty()) desc += (desc.empty() ? L"" : L"  \x00B7  ") + ver;
            if (kb) desc += (desc.empty() ? L"" : L"  \x00B7  ") + Bytes((uint64_t)kb * 1024);
            Row r = Info(L"Installed apps", name, desc);
            r.actions.push_back({L"Uninstall", [name, uninstall] {
                if (!UiConfirm(L"Uninstall " + name + L"?\n\nIts own uninstaller will open.")) return;
                if (!LaunchCommandLine(uninstall)) UiToast(L"Couldn't start the uninstaller for " + name);
            }});
            rows.push_back(std::move(r));
        }
        RegCloseKey(k);
    }
    RegCloseKey(h);
}

static void BuildInstalled(std::vector<Row>& rows) {
    std::set<std::wstring> seen;
    EnumUninstall(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, rows, seen);
    EnumUninstall(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, rows, seen);
    EnumUninstall(HKEY_CURRENT_USER, 0, rows, seen);
    SortByTitle(rows);
}

static void BuildDefaults(std::vector<Row>& rows) {
    static const struct { const wchar_t* what; const wchar_t* label; } kinds[] = {
        {L"http", L"Web browser"}, {L"https", L"Secure web links"}, {L"mailto", L"Email"}, {L".pdf", L"PDF files"},
        {L".txt", L"Text files"}, {L".jpg", L"Photos (.jpg)"}, {L".png", L"Images (.png)"}, {L".mp3", L"Music (.mp3)"},
        {L".mp4", L"Video (.mp4)"}, {L".html", L"Web pages (.html)"}, {L".zip", L"Zip files"},
    };
    std::vector<std::pair<std::wstring, std::wstring>> current;
    for (auto& k : kinds) {
        wchar_t app[512] = {};
        DWORD n = (DWORD)std::size(app);
        AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME, k.what, nullptr, app, &n);
        std::wstring id = std::wstring(L"assoc:") + k.what, progId;
        ReadValue(id, progId);
        current.push_back({id, progId});
        Row r = Info(L"Current defaults", k.label, std::wstring(k.what[0] == L'.' ? L"Opens " : L"Handles ") + k.what + (k.what[0] == L'.' ? L" files" : L" links"),
                     app[0] ? app : L"Not set");
        r.actions.push_back({L"Change", [] { OpenTarget(L"ms-settings:defaultapps"); }});
        rows.push_back(std::move(r));
    }
    Row watch = Info(L"Watch for changes", L"Tell me if Windows changes these",
                     L"Breaker remembers today's defaults. If an update switches your browser or PDF app, you'll see it on Home.");
    watch.actions.push_back({L"Remember these", [current] {
        for (auto& kv : current) if (!kv.second.empty()) DesiredSet(kv.first, kv.second);
        UiToast(L"Breaker will tell you if these defaults change");
    }});
    rows.push_back(std::move(watch));
}

static void StartupFromRun(HKEY root, REGSAM view, const wchar_t* where, const wchar_t* source, std::vector<Row>& rows) {
    HKEY h;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ | view, &h) != ERROR_SUCCESS) return;
    wchar_t name[512];
    wchar_t data[2048];
    for (DWORD i = 0;; ++i) {
        DWORD n = (DWORD)std::size(name), size = sizeof(data), type = 0;
        LONG r = RegEnumValueW(h, i, name, &n, nullptr, &type, (BYTE*)data, &size);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || !name[0]) continue;
        data[std::min<size_t>(size / sizeof(wchar_t), std::size(data) - 1)] = 0;
        Row row;
        row.id = std::wstring(L"startup:") + where + L"|" + name;
        row.group = L"Starts with Windows";
        row.title = name;
        row.label = std::wstring(L"Startup: ") + name;
        row.desc = std::wstring(data) + L"\n" + source;
        row.detail = L"Writes the same enabled/disabled marker Task Manager uses, under Explorer\\StartupApproved. The program's own entry is left in place, so turning it back on always works.";
        row.ctl = CtlToggle;
        if (root == HKEY_LOCAL_MACHINE) row.flags = F_ADMIN;
        ReadValue(row.id, row.value);
        rows.push_back(std::move(row));
    }
    RegCloseKey(h);
}

static void StartupFromFolder(REFKNOWNFOLDERID folder, const wchar_t* where, const wchar_t* source, std::vector<Row>& rows) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &p))) return;
    std::wstring dir = p;
    CoTaskMemFree(p);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || _wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;
        std::wstring file = fd.cFileName, title = file;
        size_t dot = title.rfind(L'.');
        if (dot != std::wstring::npos) title = title.substr(0, dot);
        Row row;
        row.id = std::wstring(L"startup:") + where + L"|" + file;
        row.group = L"Starts with Windows";
        row.title = title;
        row.label = L"Startup: " + title;
        row.desc = dir + L"\\" + file + L"\n" + source;
        row.ctl = CtlToggle;
        if (wcscmp(where, L"cfolder") == 0) row.flags = F_ADMIN;
        ReadValue(row.id, row.value);
        rows.push_back(std::move(row));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static void BuildStartup(std::vector<Row>& rows) {
    StartupFromRun(HKEY_CURRENT_USER, 0, L"hkcu", L"For your account", rows);
    StartupFromRun(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"hklm", L"For everyone on this PC", rows);
    StartupFromRun(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"hklm32", L"For everyone on this PC (32-bit)", rows);
    StartupFromFolder(FOLDERID_Startup, L"folder", L"Your Startup folder", rows);
    StartupFromFolder(FOLDERID_CommonStartup, L"cfolder", L"Everyone's Startup folder", rows);
    SortByTitle(rows);
    if (rows.empty()) rows.push_back(Info(L"Starts with Windows", L"Nothing starts with Windows", L"Store apps manage their own startup in Windows Settings."));
}

// ============================================================ Gaming
static void BuildGraphics(std::vector<Row>& rows) {
    Row add = Info(L"Per-app graphics", L"Choose a GPU for an app", L"Pick which graphics card a game or app uses: the power-saving one or the fast one.");
    add.actions.push_back({L"Add an app", [] {
        std::wstring path;
        if (!UiPickFile(L"Programs (*.exe)\0*.exe\0All files\0*.*\0", path)) return;
        std::wstring file = path.substr(path.find_last_of(L'\\') + 1);
        UiApply({{L"gpupref:" + path, L"2", L"Graphics for " + file}});
    }});
    rows.push_back(std::move(add));
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    wchar_t name[1024];
    size_t first = rows.size();
    for (DWORD i = 0;; ++i) {
        DWORD n = (DWORD)std::size(name);
        if (RegEnumValueW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring path = name;
        if (path == L"DirectXUserGlobalSettings" || path.empty()) continue;
        Row r;
        r.id = L"gpupref:" + path;
        r.group = L"Per-app graphics";
        size_t slash = path.find_last_of(L'\\');
        r.title = slash == std::wstring::npos ? path : path.substr(slash + 1);
        r.label = L"Graphics for " + r.title;
        r.desc = path;
        r.ctl = CtlChoice;
        GetChoices(r.id, r.choices);
        ReadValue(r.id, r.value);
        rows.push_back(std::move(r));
    }
    RegCloseKey(h);
    SortByTitle(rows, first);
}

// ============================================================ Accounts
static bool InAdminsGroup() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    BYTE sidBuf[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(sidBuf), size = 0;
    bool admin = false;
    CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sidBuf, &sidSize);
    GetTokenInformation(tok, TokenGroups, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    if (size && GetTokenInformation(tok, TokenGroups, buf.data(), size, &size)) {
        auto* groups = (TOKEN_GROUPS*)buf.data();
        for (DWORD i = 0; i < groups->GroupCount; ++i) if (EqualSid(groups->Groups[i].Sid, sidBuf)) admin = true;
    }
    CloseHandle(tok);
    return admin;
}

static void BuildAccounts(std::vector<Row>& rows) {
    wchar_t user[256] = {}, pc[256] = {};
    DWORD n = (DWORD)std::size(user);
    GetUserNameW(user, &n);
    n = (DWORD)std::size(pc);
    GetComputerNameW(pc, &n);
    rows.push_back(Info(L"Signed in as", user, std::wstring(L"On ") + pc, InAdminsGroup() ? L"Administrator" : L"Standard user"));
}

// ============================================================ Privacy & Security
static void BuildSecurity(std::vector<Row>& rows) {
    std::wstring out;
    DWORD code = 1;
    RunPowerShell(L"$s=Get-MpComputerStatus -ErrorAction Stop;'{0}|{1}|{2}|{3:yyyy-MM-dd HH:mm}|{4:yyyy-MM-dd HH:mm}' -f $s.AntivirusEnabled,$s.RealTimeProtectionEnabled,$s.IsTamperProtected,$s.AntivirusSignatureLastUpdated,$s.QuickScanEndTime", &out, 30000, &code);
    auto f = Split(Trim(out), L"|", true);
    if (code != 0 || f.size() < 5) {
        Row r = Info(L"Virus protection", L"Microsoft Defender isn't reporting", L"Another antivirus may be protecting this PC. Open Windows Security to see which.");
        r.actions.push_back({L"Open Windows Security", [] { OpenTarget(L"windowsdefender:"); }});
        rows.push_back(std::move(r));
        return;
    }
    auto onOff = [](const std::wstring& s) { return s == L"True" ? std::wstring(L"On") : std::wstring(L"Off"); };
    Row rt = Info(L"Virus protection", L"Real-time protection", L"Windows protects this switch so other apps (and malware) can't turn it off. Change it in Windows Security.", onOff(f[1]));
    rt.alert = f[1] != L"True";
    rt.actions.push_back({L"Open", [] { OpenTarget(L"windowsdefender://threatsettings"); }});
    rows.push_back(std::move(rt));
    rows.push_back(Info(L"Virus protection", L"Tamper protection", L"Stops apps changing Defender's settings behind your back.", onOff(f[2])));
    Row defs = Info(L"Virus protection", L"Virus definitions", L"Last updated " + (f[3].empty() ? L"never" : f[3]));
    defs.actions.push_back({L"Update now", [] {
        UiBusy(L"Updating virus definitions\x2026", [] { RunPowerShell(L"Update-MpSignature", nullptr, 180000); }, [] { UiToast(L"Virus definitions updated"); UiReloadPage(); });
    }});
    rows.push_back(std::move(defs));
    Row scan = Info(L"Virus protection", L"Quick scan", L"Last finished " + (f[4].empty() ? L"never" : f[4]));
    scan.actions.push_back({L"Scan now", [] {
        UiBusy(L"Scanning for threats\x2026 You can keep using Breaker.", [] { RunPowerShell(L"Start-MpScan -ScanType QuickScan", nullptr, 30 * 60 * 1000); },
               [] { UiToast(L"Quick scan finished. Details are in Windows Security."); UiReloadPage(); });
    }});
    rows.push_back(std::move(scan));
}

// ============================================================ Windows Update
static void BuildUpdateStatus(std::vector<Row>& rows) {
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring display, build;
    DWORD ubr = 0;
    RegReadStr(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion", display);
    RegReadStr(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild", build);
    RegReadDword(HKEY_LOCAL_MACHINE, nt, L"UBR", ubr);
    rows.push_back(Info(L"Status", L"Windows version", L"", display + L"  (build " + build + L"." + std::to_wstring(ubr) + L")"));
    std::wstring paused = WuPausedUntil();
    if (!paused.empty()) {
        Row p = Info(L"Status", L"Updates are paused", L"Windows won't download or install updates until " + paused + L".");
        p.alert = true;
        p.actions.push_back({L"Resume", [] { UiApply({{L"wu.pause", L"0", L""}}); }});
        rows.push_back(std::move(p));
    }
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        RegCloseKey(h);
        Row r = Info(L"Status", L"A restart is needed to finish installing updates", L"Save your work first.");
        r.alert = true;
        r.actions.push_back({L"Restart now", [] {
            if (UiConfirm(L"Restart the PC now to finish installing updates?\n\nUnsaved work in other apps will be lost.")) ShellOpen(L"shutdown.exe", L"/r /t 5");
        }});
        rows.push_back(std::move(r));
    }
    std::wstring out;
    RunPowerShell(L"$q=(New-Object -ComObject Microsoft.Update.Session).CreateUpdateSearcher();$n=$q.GetTotalHistoryCount();"
                  L"if($n -gt 0){$q.QueryHistory(0,[Math]::Min($n,20))|ForEach-Object{'{0:yyyy-MM-dd}|{1}|{2}' -f $_.Date,[int]$_.ResultCode,$_.Title}}", &out, 45000);
    for (auto& line : Split(out, L"\n")) {
        auto f = Split(Trim(line), L"|", true);
        if (f.size() < 3 || f[2].empty()) continue;
        int code = _wtoi(f[1].c_str());
        Row r = Info(L"Recently installed", f[2], L"", f[0] + (code == 2 ? L"" : code == 3 ? L"  \x00B7  with warnings" : code == 4 ? L"  \x00B7  failed" : code == 5 ? L"  \x00B7  cancelled" : L"  \x00B7  in progress"));
        r.alert = code == 4;
        rows.push_back(std::move(r));
    }
}

static void BuildUpdatePending(std::vector<Row>& rows) {
    std::wstring out;
    DWORD code = 1;
    bool finished = RunPowerShell(L"$r=(New-Object -ComObject Microsoft.Update.Session).CreateUpdateSearcher().Search('IsInstalled=0 and IsHidden=0');"
                                  L"$r.Updates|ForEach-Object{'{0}|{1}' -f $_.IsDownloaded,$_.Title}", &out, 120000, &code);
    if (!finished || code != 0) {
        rows.push_back(Info(L"Waiting to install", L"Couldn't ask Windows Update", L"The update service didn't answer. Open Windows Update to check there."));
        return;
    }
    int count = 0;
    for (auto& line : Split(out, L"\n")) {
        auto f = Split(Trim(line), L"|", true);
        if (f.size() < 2 || f[1].empty()) continue;
        ++count;
        Row r = Info(L"Waiting to install", f[1], f[0] == L"True" ? L"Downloaded, ready to install" : L"Available");
        r.actions.push_back({L"Install in Windows Update", [] { OpenTarget(L"ms-settings:windowsupdate-action"); }});
        rows.push_back(std::move(r));
    }
    if (!count) rows.push_back(Info(L"Waiting to install", L"You're up to date", L"Windows Update has nothing waiting.", L"\x2713"));
}

// ============================================================ Advanced
static void BuildServices(std::vector<Row>& rows) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
    std::vector<BYTE> buf(needed + 4096);
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(), (DWORD)buf.size(), &needed, &count, &resume, nullptr)) {
        auto* list = (ENUM_SERVICE_STATUS_PROCESSW*)buf.data();
        Choices choices;
        GetChoices(L"svc:x", choices);
        for (DWORD i = 0; i < count; ++i) {
            std::wstring name = list[i].lpServiceName;
            bool running = list[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING;
            Row r;
            r.id = L"svc:" + name;
            r.group = L"Services";
            r.title = list[i].lpDisplayName;
            r.label = L"Service: " + r.title;
            r.desc = name + (running ? L"  \x00B7  Running" : L"  \x00B7  Stopped");
            r.detail = L"Calls ChangeServiceConfig to set how this service starts. Disabling a service Windows depends on can stop features working; undo puts it back.";
            r.ctl = CtlChoice;
            r.choices = choices;
            r.flags = F_ADMIN | F_RISKY;
            if (!ReadValue(r.id, r.value)) continue;
            std::wstring title = r.title;
            r.actions.push_back({running ? L"Stop" : L"Start", [name, running, title] {
                UiApply({{L"svcrun:" + name, running ? L"stop" : L"start", title}});
            }});
            rows.push_back(std::move(r));
        }
    }
    CloseServiceHandle(scm);
    SortByTitle(rows);
}

static void EnvRows(HKEY root, const wchar_t* key, const wchar_t* group, const wchar_t* scope, std::vector<Row>& rows) {
    HKEY h;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    wchar_t name[512];
    size_t first = rows.size();
    for (DWORD i = 0;; ++i) {
        DWORD n = (DWORD)std::size(name), type = 0;
        if (RegEnumValueW(h, i, name, &n, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS) break;
        if ((type != REG_SZ && type != REG_EXPAND_SZ) || !name[0]) continue;
        Row r;
        r.id = std::wstring(L"env:") + scope + L":" + name;
        r.group = group;
        r.title = name;
        r.label = std::wstring(L"Variable ") + name;
        r.ctl = CtlText;
        r.wide = true;
        if (root == HKEY_LOCAL_MACHINE) r.flags = F_ADMIN;
        ReadValue(r.id, r.value);
        std::wstring id = r.id, nm = name;
        r.actions.push_back({L"Delete", [id, nm] {
            if (UiConfirm(L"Delete the variable " + nm + L"?\n\nYou can bring it back from the undo history.")) UiApply({{id, L"~", L"Variable " + nm}});
        }});
        rows.push_back(std::move(r));
    }
    RegCloseKey(h);
    SortByTitle(rows, first);
}

static void BuildEnv(std::vector<Row>& rows) {
    auto adder = [](const wchar_t* scope, const wchar_t* what) {
        return [scope, what] {
            std::wstring s = scope;
            UiPrompt(std::wstring(L"New ") + what + L" variable", {L"Name", L"Value"}, {L"", L""}, [s](const std::vector<std::wstring>& v) {
                if (!Trim(v[0]).empty()) UiApply({{L"env:" + s + L":" + Trim(v[0]), v[1], L"Variable " + Trim(v[0])}});
            });
        };
    };
    Row add = Info(L"Add", L"New variable", L"Variables for your account apply to you only. System variables apply to everyone and need administrator rights.");
    add.actions.push_back({L"For your account", adder(L"u", L"user")});
    add.actions.push_back({L"For everyone", adder(L"s", L"system")});
    rows.push_back(std::move(add));
    EnvRows(HKEY_CURRENT_USER, L"Environment", L"Your account", L"u", rows);
    EnvRows(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", L"Everyone (system)", L"s", rows);
}

// ============================================================ tables
const Category kCategories[] = {
    {L"home", L"Home", 0xE80F, 0},
    {L"system", L"System", 0xE7F8, 0},
    {L"devices", L"Devices", 0xE772, 0},
    {L"network", L"Network", 0xE774, 0},
    {L"personalization", L"Personalization", 0xE771, 0},
    {L"apps", L"Apps", 0xE71D, 0},
    {L"gaming", L"Gaming", 0xE7FC, 0},
    {L"accounts", L"Accounts", 0xE77B, 0},
    {L"privacy", L"Privacy & Security", 0xE72E, 0},
    {L"update", L"Windows Update", 0xE895, 0},
    {L"debloat", L"Debloat", 0xE74D, 0},
    {L"advanced", L"Advanced", 0xE90F, 0},
    {L"screentime", L"Screen time", 0xE916, 1},
    {L"checkup", L"PC Checkup", 0xE95E, 1},
    {L"myapps", L"My Apps", 0xE74C, 1},
    {L"profiles", L"Profiles", 0xE8AB, 1},
    {L"history", L"Undo history", 0xE81C, 1},
    {L"watchdog", L"Update watchdog", 0xEA18, 1},
    {L"snapshots", L"Snapshots", 0xE74E, 1},
    {L"settings", L"Breaker settings", 0xE713, 2},
};
const int kCategoryCount = (int)std::size(kCategories);

// route, title, blurb, background builder, slow background builder, window-thread builder, filter box
const PageInfo kPages[] = {
    {L"home", L"Home", nullptr, nullptr, nullptr, BuildHome, false},

    {L"system/display", L"Display", nullptr, nullptr, nullptr, nullptr, false},
    {L"system/sound", L"Sound", nullptr, nullptr, nullptr, nullptr, false},
    {L"system/notifications", L"Notifications", nullptr, nullptr, nullptr, nullptr, false},
    {L"system/power", L"Power", nullptr, nullptr, nullptr, nullptr, false},
    {L"system/storage", L"Storage", nullptr, BuildStorage, nullptr, nullptr, false},
    {L"system/clipboard", L"Clipboard", nullptr, nullptr, nullptr, nullptr, false},
    {L"system/multitasking", L"Multitasking", nullptr, nullptr, nullptr, nullptr, false},
    {L"system/about", L"About", nullptr, BuildAbout, nullptr, nullptr, false},

    {L"devices/bluetooth", L"Bluetooth", nullptr, nullptr, nullptr, nullptr, false},
    {L"devices/printers", L"Printers", nullptr, BuildPrinters, nullptr, nullptr, false},
    {L"devices/mouse", L"Mouse", nullptr, nullptr, nullptr, nullptr, false},
    {L"devices/keyboard", L"Keyboard", nullptr, nullptr, nullptr, nullptr, false},
    {L"devices/controllers", L"Controllers", nullptr, BuildControllers, nullptr, nullptr, false},
    {L"devices/usb", L"USB", nullptr, nullptr, nullptr, nullptr, false},

    {L"network/status", L"Connections", nullptr, BuildNetwork, nullptr, nullptr, false},
    {L"network/dns", L"DNS", nullptr, BuildDns, nullptr, nullptr, false},
    {L"network/vpn", L"VPN", nullptr, BuildVpn, nullptr, nullptr, false},
    {L"network/proxy", L"Proxy", nullptr, nullptr, nullptr, nullptr, false},
    {L"network/data", L"Data usage", nullptr, BuildData, nullptr, nullptr, false},

    {L"personalization/background", L"Background", nullptr, nullptr, nullptr, nullptr, false},
    {L"personalization/colors", L"Colors", nullptr, nullptr, nullptr, nullptr, false},
    {L"personalization/taskbar", L"Taskbar", nullptr, nullptr, nullptr, nullptr, false},
    {L"personalization/start", L"Start", nullptr, nullptr, nullptr, nullptr, false},
    {L"personalization/fonts", L"Fonts", nullptr, nullptr, nullptr, nullptr, false},
    {L"personalization/cursors", L"Cursors", nullptr, nullptr, nullptr, nullptr, false},
    {L"personalization/sounds", L"Sounds", nullptr, nullptr, nullptr, nullptr, false},
    {L"personalization/effects", L"Effects", nullptr, nullptr, nullptr, nullptr, false},

    {L"apps/installed", L"Installed apps", nullptr, BuildInstalled, nullptr, nullptr, true},
    {L"apps/defaults", L"Default apps", nullptr, BuildDefaults, nullptr, nullptr, false},
    {L"apps/startup", L"Startup", L"Apps that start by themselves when you sign in. Fewer means a faster start.", BuildStartup, nullptr, nullptr, true},
    {L"apps/features", L"Features", nullptr, nullptr, nullptr, nullptr, false},

    {L"gaming/general", L"Game Mode", nullptr, nullptr, nullptr, nullptr, false},
    {L"gaming/graphics", L"Graphics per app", nullptr, BuildGraphics, nullptr, nullptr, false},

    {L"accounts", L"Accounts", L"Windows keeps account and sign-in changes inside its own Settings app, so those open there.", BuildAccounts, nullptr, nullptr, false},

    {L"privacy/telemetry", L"Telemetry", nullptr, nullptr, nullptr, nullptr, false},
    {L"privacy/permissions", L"App permissions", L"What apps are allowed to use. These are the same switches as Windows Settings > Privacy.", nullptr, nullptr, nullptr, false},
    {L"privacy/security", L"Security", nullptr, BuildSecurity, nullptr, nullptr, false},

    {L"update", L"Windows Update", nullptr, BuildUpdateStatus, BuildUpdatePending, nullptr, false},

    {L"debloat", L"Debloat", L"Everything Windows adds that most people don't want, in one place. Each switch shows what it is, and every change can be undone.", BuildDebloatApps, nullptr, BuildDebloat, false},

    {L"advanced/explorer", L"File Explorer", nullptr, nullptr, nullptr, nullptr, false},
    {L"advanced/services", L"Services", L"Background programs that run without a window. Change how they start, or start and stop them.", BuildServices, nullptr, nullptr, true},
    {L"advanced/env", L"Environment variables", nullptr, BuildEnv, nullptr, nullptr, true},
    {L"advanced/system", L"System tweaks", nullptr, nullptr, nullptr, nullptr, false},
    {L"advanced/legacy", L"Control Panel", L"Every classic Control Panel item and admin tool, searchable.", nullptr, nullptr, nullptr, true},

    {L"screentime/today", L"Today", nullptr, BuildTallyToday, nullptr, nullptr, false},
    {L"screentime/week", L"This week", nullptr, BuildTallyWeek, nullptr, nullptr, false},
    {L"screentime/month", L"This month", nullptr, BuildTallyMonth, nullptr, nullptr, false},
    {L"checkup", L"PC Checkup", nullptr, BuildCheckup, nullptr, nullptr, false},
    {L"myapps", L"My Apps", L"The rest of the suite: open an app, change its settings, keep it up to date.", BuildMyAppsRemote, nullptr, BuildMyApps, false},
    {L"profiles", L"Profiles", L"Saved groups of settings. Apply one in a click, or let Breaker switch for you.", nullptr, nullptr, BuildProfiles, false},
    {L"history", L"Undo history", L"Everything Breaker has changed, newest first. Ctrl+Z undoes the latest.", nullptr, nullptr, BuildHistory, true},
    {L"watchdog", L"Update watchdog", L"Breaker remembers the settings you chose and checks whether Windows changed them back.", nullptr, nullptr, BuildWatchdog, false},
    {L"snapshots", L"Snapshots", L"Save every setting to one file, then apply it after a fresh install or on another PC.", nullptr, nullptr, BuildSnapshots, false},

    {L"settings/general", L"General", nullptr, nullptr, nullptr, BuildSettingsGeneral, false},
    {L"settings/appearance", L"Appearance", nullptr, nullptr, nullptr, BuildSettingsAppearance, false},
    {L"settings/layout", L"Layout", nullptr, nullptr, nullptr, BuildSettingsLayout, false},
    {L"settings/screentime", L"Screen time", L"Breaker's screen time tracking: limits, notifications, privacy and your data.", nullptr, nullptr, BuildTallySettings, false},
    {L"settings/updates", L"Updates", nullptr, nullptr, nullptr, BuildSettingsUpdates, false},
    {L"settings/about", L"About", nullptr, nullptr, nullptr, BuildSettingsAbout, false},
};
const int kPageCount = (int)std::size(kPages);

const PageInfo* FindPage(const std::wstring& route) {
    static const PageInfo appPage = {L"myapps/*", L"App settings", L"Suite apps save their settings when they close, so a running app is restarted when you change something here.", nullptr, nullptr, BuildMyAppSettings, false};
    for (auto& p : kPages) if (route == p.route) return &p;
    if (StartsWith(route, L"myapps/") && route.size() > 7) return &appPage;
    static const PageInfo tallyApp = {L"screentime/app/*", L"App", nullptr, BuildTallyApp, nullptr, nullptr, false};
    if (StartsWith(route, L"screentime/app/") && route.size() > 15) return &tallyApp;
    return nullptr;
}

const Category* FindCategory(const std::wstring& id) {
    for (auto& c : kCategories) if (id == c.id) return &c;
    return nullptr;
}

std::wstring CategoryOf(const std::wstring& route) {
    size_t s = route.find(L'/');
    return s == std::wstring::npos ? route : route.substr(0, s);
}

Row RowFromDef(const Def& d) {
    Row r;
    r.id = d.id;
    r.group = d.group;
    r.title = d.name;
    r.desc = d.desc ? d.desc : L"";
    r.ctl = d.ctl;
    r.flags = FlagsOf(d.id);
    r.rec = d.rec ? d.rec : L"";
    r.uri = d.uri ? d.uri : L"";
    r.route = d.page;
    if (d.ctl == CtlLink) r.target = d.spec;
    if (d.ctl == CtlChoice && d.choices) GetChoices(d.id, r.choices);
    if (d.ctl == CtlSlider && d.choices) {
        auto p = Split(d.choices, L"|", true);
        if (p.size() >= 3) { r.smin = (float)_wtof(p[0].c_str()); r.smax = (float)_wtof(p[1].c_str()); r.sstep = (float)_wtof(p[2].c_str()); }
        if (p.size() >= 4) r.unit = p[3];
    }
    r.detail = UnderTheHood(d);
    r.known = d.ctl == CtlLink || d.ctl == CtlInfo;
    return r;
}
