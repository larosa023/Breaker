// Breaker - settings engine: registry specs, handlers, elevation, undo history, value cache
#include "engine.h"
#include "json.h"
#include <shellapi.h>
#include <shlobj.h>
#include <mutex>
#include <map>
#include <unordered_map>
#include <algorithm>

// ============================================================ registry specs
// "ROOT\path!value=T:on/off?absent"  joined with ";;" when a setting writes several values.
//   T        d = REG_DWORD, s = REG_SZ, x = REG_EXPAND_SZ
//   on/off   the data for a toggle's two states. "~" = the value doesn't exist, "~~" = the key doesn't exist
//   ?absent  how to read a missing value (Windows' built-in default)
// Choices, sliders and text use one value with no on/off part: the data is the setting's value.
namespace {
struct RegOp {
    HKEY root = nullptr;
    std::wstring rootName, path, value;
    wchar_t type = L'd';
    bool hasOnOff = false, hasAbsent = false;
    std::wstring on, off, absent;
};
}  // namespace

static bool ParseSpec(const wchar_t* spec, std::vector<RegOp>& ops) {
    for (auto& part : Split(spec, L";;")) {
        std::wstring p = Trim(part);
        size_t bang = p.find(L'!');
        size_t slash = p.find(L'\\');
        if (bang == std::wstring::npos || slash == std::wstring::npos || slash > bang) return false;
        RegOp op;
        op.rootName = p.substr(0, slash);
        op.root = op.rootName == L"HKLM" ? HKEY_LOCAL_MACHINE : op.rootName == L"HKCR" ? HKEY_CLASSES_ROOT : HKEY_CURRENT_USER;
        op.path = p.substr(slash + 1, bang - slash - 1);
        std::wstring rest = p.substr(bang + 1);
        size_t eq = rest.find(L'=');
        if (eq == std::wstring::npos || eq + 1 >= rest.size()) return false;
        op.value = rest.substr(0, eq);
        op.type = rest[eq + 1];
        rest = rest.substr(eq + 2);
        size_t q = rest.rfind(L'?');
        if (q != std::wstring::npos) { op.hasAbsent = true; op.absent = rest.substr(q + 1); rest = rest.substr(0, q); }
        if (!rest.empty() && rest[0] == L':') {
            size_t sl = rest.find(L'/');
            if (sl == std::wstring::npos) return false;
            op.hasOnOff = true;
            op.on = rest.substr(1, sl - 1);
            op.off = rest.substr(sl + 1);
        }
        ops.push_back(std::move(op));
    }
    return !ops.empty();
}

static std::wstring RawRead(const RegOp& op) {
    const wchar_t* val = op.value.empty() ? nullptr : op.value.c_str();
    if (op.type == L'd') {
        DWORD v;
        if (RegReadDword(op.root, op.path.c_str(), val, v)) return std::to_wstring(v);
    } else {
        std::wstring s;
        if (RegReadStr(op.root, op.path.c_str(), val, s)) return s;
    }
    HKEY h;
    if (RegOpenKeyExW(op.root, op.path.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return L"~~";
    RegCloseKey(h);
    return L"~";
}

static LONG RawWrite(const RegOp& op, const std::wstring& raw) {
    const wchar_t* val = op.value.empty() ? nullptr : op.value.c_str();
    if (raw == L"~~") {
        LONG r = RegDeleteTreeW(op.root, op.path.c_str());
        return (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND) ? ERROR_SUCCESS : r;
    }
    if (raw == L"~") return RegRemoveValue(op.root, op.path.c_str(), val);
    if (op.type == L'd') return RegWriteDword(op.root, op.path.c_str(), val, (DWORD)wcstoul(raw.c_str(), nullptr, 0));
    return RegWriteStr(op.root, op.path.c_str(), val, raw, op.type == L'x' ? REG_EXPAND_SZ : REG_SZ);
}

// A missing value and a missing key mean the same thing when comparing against on/off
static bool RawEq(const std::wstring& a, const std::wstring& b) {
    auto gone = [](const std::wstring& s) { return s == L"~" || s == L"~~"; };
    return a == b || (gone(a) && gone(b));
}

// ============================================================ lookup tables
static std::unordered_map<std::wstring, int> g_defIndex;
static std::vector<std::vector<RegOp>> g_defOps;
static std::vector<Handler> g_handlers;
static std::mutex g_lock;               // cache, history, desired
static std::map<std::wstring, std::wstring> g_cache, g_desired;
static std::vector<HistEntry> g_history;
static uint64_t g_nextSeq = 1;
static bool g_cacheDirty = false;

static std::wstring CachePath() { return DataDir() + L"\\cache.json"; }
static std::wstring HistoryPath() { return DataDir() + L"\\history.json"; }
static std::wstring DesiredPath() { return DataDir() + L"\\desired.json"; }

static void LoadMap(const std::wstring& path, std::map<std::wstring, std::wstring>& m) {
    std::wstring text;
    json::Value root;
    if (!ReadUtf8File(path, text)) return;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) return;
    for (auto& kv : root.obj) if (kv.second.type == json::Value::String) m[kv.first] = kv.second.s;
}

static void SaveMap(const std::wstring& path, const std::map<std::wstring, std::wstring>& m) {
    json::Value root = json::Value::MakeObject();
    for (auto& kv : m) root.obj.emplace_back(kv.first, json::Value::Str(kv.second));
    std::wstring text;
    json::Write(root, text);
    WriteUtf8FileAtomic(path, text);
}

static void HistorySaveLocked() {
    json::Value arr = json::Value::MakeArray();
    for (auto& e : g_history) {
        json::Value v = json::Value::MakeObject();
        v.Set(L"seq", json::Value::Num((double)e.seq));
        v.Set(L"time", json::Value::Num(e.time));
        v.Set(L"id", json::Value::Str(e.id));
        v.Set(L"name", json::Value::Str(e.name));
        v.Set(L"before", json::Value::Str(e.before));
        v.Set(L"after", json::Value::Str(e.after));
        v.Set(L"was", json::Value::Str(e.was));
        v.Set(L"source", json::Value::Str(e.source));
        v.Set(L"reverted", json::Value::Boolean(e.reverted));
        arr.arr.push_back(std::move(v));
    }
    std::wstring text;
    json::Write(arr, text);
    WriteUtf8FileAtomic(HistoryPath(), text);
}

void EngineInit() {
    g_defOps.resize(kDefCount);
    for (int i = 0; i < kDefCount; ++i) {
        g_defIndex[kDefs[i].id] = i;
        const Def& d = kDefs[i];
        if (d.ctl == CtlLink || d.ctl == CtlInfo || !d.spec || d.spec[0] == L'@') continue;
        if (!ParseSpec(d.spec, g_defOps[i])) Log(L"Bad registry spec for %ls", d.id);
    }
    std::lock_guard<std::mutex> lock(g_lock);
    LoadMap(CachePath(), g_cache);
    LoadMap(DesiredPath(), g_desired);
    std::wstring text;
    json::Value root;
    if (ReadUtf8File(HistoryPath(), text)) {
        json::Parser p(text);
        if (p.Parse(root) && root.type == json::Value::Array) {
            for (auto& v : root.arr) {
                HistEntry e;
                e.seq = (uint64_t)v.GetNum(L"seq", 0);
                e.time = v.GetNum(L"time", 0);
                e.id = v.GetStr(L"id");
                e.name = v.GetStr(L"name");
                e.before = v.GetStr(L"before");
                e.after = v.GetStr(L"after");
                e.was = v.GetStr(L"was");
                e.source = v.GetStr(L"source");
                e.reverted = v.GetBool(L"reverted", false);
                if (!e.id.empty()) { g_history.push_back(e); g_nextSeq = std::max(g_nextSeq, e.seq + 1); }
            }
        }
    }
}

void RegisterHandlers(const Handler* list, int count) {
    for (int i = 0; i < count; ++i) g_handlers.push_back(list[i]);
}

const Def* FindDef(const std::wstring& id) {
    auto it = g_defIndex.find(id);
    return it == g_defIndex.end() ? nullptr : &kDefs[it->second];
}

const Handler* FindHandler(const std::wstring& id) {
    const Handler* best = nullptr;
    size_t bestLen = 0;
    for (auto& h : g_handlers) {
        size_t n = wcslen(h.prefix);
        if (!n || n < bestLen) continue;
        wchar_t last = h.prefix[n - 1];
        bool match = (last == L':' || last == L'.') ? id.compare(0, n, h.prefix) == 0 : id == h.prefix;
        if (match) { best = &h; bestLen = n; }
    }
    return best;
}

static const std::vector<RegOp>* OpsOf(const std::wstring& id) {
    auto it = g_defIndex.find(id);
    if (it == g_defIndex.end() || g_defOps[it->second].empty()) return nullptr;
    return &g_defOps[it->second];
}

unsigned FlagsOf(const std::wstring& id) {
    unsigned f = 0;
    if (const Def* d = FindDef(id)) f |= d->flags;
    if (const Handler* h = FindHandler(id)) f |= h->flags;
    if (auto* ops = OpsOf(id)) for (auto& op : *ops) if (op.root == HKEY_LOCAL_MACHINE) f |= F_ADMIN;
    return f;
}

std::wstring NameOf(const std::wstring& id) {
    if (const Def* d = FindDef(id)) return d->name;
    size_t c = id.rfind(L':');
    return c == std::wstring::npos ? id : id.substr(c + 1);
}

void GetChoices(const std::wstring& id, Choices& out) {
    const Def* d = FindDef(id);
    if (d && d->choices && d->ctl == CtlChoice) {
        for (auto& part : Split(d->choices, L"|")) {
            size_t eq = part.rfind(L'=');
            if (eq != std::wstring::npos) out.push_back({part.substr(0, eq), part.substr(eq + 1)});
        }
        return;
    }
    if (const Handler* h = FindHandler(id)) if (h->choices) h->choices(id, out);
}

std::wstring ChoiceLabel(const std::wstring& id, const std::wstring& value) {
    Choices c;
    GetChoices(id, c);
    for (auto& kv : c) if (kv.second == value) return kv.first;
    return value;
}

std::wstring DescribeValue(const std::wstring& id, const std::wstring& value) {
    if (StartsWith(value, L"raw:")) return L"its earlier state";
    const Def* d = FindDef(id);
    Ctl ctl = d ? d->ctl : CtlText;
    if (!d) {
        Choices c;
        GetChoices(id, c);
        if (!c.empty()) ctl = CtlChoice;
        else if (value == L"0" || value == L"1") ctl = CtlToggle;
    }
    if (ctl == CtlToggle) return value == L"1" ? L"On" : L"Off";
    if (ctl == CtlChoice) return ChoiceLabel(id, value);
    if (value == L"~") return L"(removed)";
    return value.size() > 60 ? value.substr(0, 57) + L"\x2026" : value;
}

std::wstring UnderTheHood(const Def& d) {
    std::wstring out;
    const std::vector<RegOp>* ops = OpsOf(d.id);
    if (ops) {
        auto show = [](const std::wstring& v) { return v == L"~" ? std::wstring(L"(value removed)") : v == L"~~" ? std::wstring(L"(key removed)") : v.empty() ? std::wstring(L"(empty)") : v; };
        for (auto& op : *ops) {
            if (!out.empty()) out += L"\n";
            out += op.rootName + L"\\" + op.path + L"  \x2192  " + (op.value.empty() ? L"(Default)" : op.value);
            out += op.type == L'd' ? L"  (REG_DWORD)" : L"  (REG_SZ)";
            if (op.hasOnOff) out += L"\n    On = " + show(op.on) + L",  Off = " + show(op.off);
        }
    } else if (d.ctl == CtlLink && d.spec) {
        out = std::wstring(L"Opens: ") + d.spec;
    }
    if (d.detail && *d.detail && !ops) out = d.detail;
    return out;
}

// ============================================================ read / write one setting
bool ReadValue(const std::wstring& id, std::wstring& out) {
    if (auto* ops = OpsOf(id)) {
        const RegOp& op = (*ops)[0];
        std::wstring raw = RawRead(op);
        bool gone = raw == L"~" || raw == L"~~";
        if (gone && op.hasAbsent) { raw = op.absent; gone = false; }
        if (op.hasOnOff) out = RawEq(raw, op.on) ? L"1" : L"0";
        else out = gone ? L"~" : raw;
        return true;
    }
    if (const Handler* h = FindHandler(id)) return h->read && h->read(id, out);
    return false;
}

// The exact state, so undo puts back precisely what was there (including "nothing was set").
static std::wstring Capture(const std::wstring& id) {
    if (auto* ops = OpsOf(id)) {
        std::wstring s = L"raw:";
        for (size_t i = 0; i < ops->size(); ++i) { if (i) s += L'\x1f'; s += RawRead((*ops)[i]); }
        return s;
    }
    std::wstring v;
    return ReadValue(id, v) ? v : L"";
}

static WriteResult WriteOne(const std::wstring& id, const std::wstring& value, std::wstring& err) {
    if (auto* ops = OpsOf(id)) {
        std::vector<std::wstring> raws;
        if (StartsWith(value, L"raw:")) {
            raws = Split(value.substr(4), std::wstring(1, L'\x1f'), true);
            if (raws.size() != ops->size()) { err = L"The saved state doesn't match this setting any more."; return W_FAIL; }
        } else {
            for (auto& op : *ops) {
                if (op.hasOnOff) raws.push_back(value == L"1" ? op.on : op.off);
                else raws.push_back(value);
            }
            if (!(*ops)[0].hasOnOff) raws.resize(1);
        }
        for (size_t i = 0; i < raws.size(); ++i) {
            LONG r = RawWrite((*ops)[i], raws[i]);
            if (r == ERROR_ACCESS_DENIED) return W_DENIED;
            if (r != ERROR_SUCCESS) { err = ErrorText(r); return W_FAIL; }
        }
        return W_OK;
    }
    const Handler* h = FindHandler(id);
    if (!h || !h->write) { err = L"Windows doesn't let other apps change this setting."; return W_FAIL; }
    return h->write(id, value, err);
}

// ============================================================ elevation
// Only the writes Windows refused are handed to an elevated copy of Breaker, in one prompt.
static bool RunElevated(const std::vector<Change>& changes, std::vector<std::pair<bool, std::wstring>>& results, bool& cancelled) {
    std::wstring file = DataDir() + L"\\elevate-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount()) + L".json";
    json::Value arr = json::Value::MakeArray();
    for (auto& c : changes) {
        json::Value v = json::Value::MakeObject();
        v.Set(L"id", json::Value::Str(c.id));
        v.Set(L"value", json::Value::Str(c.value));
        arr.arr.push_back(std::move(v));
    }
    std::wstring text;
    json::Write(arr, text);
    if (!WriteUtf8FileAtomic(file, text)) return false;
    std::wstring args = L"--elevated \"" + file + L"\"";
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        cancelled = GetLastError() == ERROR_CANCELLED;
        DeleteFileW(file.c_str());
        return false;
    }
    WaitForSingleObject(sei.hProcess, 5 * 60 * 1000);
    CloseHandle(sei.hProcess);
    std::wstring outText;
    json::Value out;
    bool ok = ReadUtf8File(file + L".out", outText);
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".out").c_str());
    if (!ok) return false;
    json::Parser p(outText);
    if (!p.Parse(out) || out.type != json::Value::Array) return false;
    for (auto& v : out.arr) results.push_back({v.GetBool(L"ok", false), v.GetStr(L"err")});
    return results.size() == changes.size();
}

int ElevatedMain(const std::wstring& file) {
    std::wstring text;
    json::Value root;
    if (!ReadUtf8File(file, text)) return 2;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Array) return 2;
    json::Value out = json::Value::MakeArray();
    for (auto& v : root.arr) {
        std::wstring err;
        WriteResult r = WriteOne(v.GetStr(L"id"), v.GetStr(L"value"), err);
        if (r == W_DENIED) err = L"Windows refused this change even with administrator rights.";
        Log(L"Elevated write %ls -> %ls", v.GetStr(L"id").c_str(), r == W_OK ? L"ok" : err.c_str());
        json::Value res = json::Value::MakeObject();
        res.Set(L"ok", json::Value::Boolean(r == W_OK));
        res.Set(L"err", json::Value::Str(err));
        out.arr.push_back(std::move(res));
    }
    std::wstring outText;
    json::Write(out, outText);
    return WriteUtf8FileAtomic(file + L".out", outText) ? 0 : 3;
}

// ============================================================ apply
static void Broadcast(unsigned flags, bool anyRegistry) {
    if (flags & F_THEME) SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet");
    if (flags & F_TRAY) SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings");
    if (anyRegistry) {
        SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Policy");
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);   // Explorer windows re-read their options
    }
}

ApplyReport ApplyChanges(const std::vector<Change>& changes, const std::wstring& source) {
    ApplyReport rep;
    struct Item { std::wstring before, was, err; int state = 0; };   // 0 pending, 1 ok, 2 failed, 3 needs admin
    std::vector<Item> items(changes.size());
    std::vector<Change> admin;
    std::vector<size_t> adminIdx;
    bool anyRegistry = false;
    for (size_t i = 0; i < changes.size(); ++i) {
        const Change& c = changes[i];
        items[i].before = Capture(c.id);
        if (StartsWith(items[i].before, L"raw:")) ReadValue(c.id, items[i].was);
        else items[i].was = items[i].before;
        WriteResult r = WriteOne(c.id, c.value, items[i].err);
        if (r == W_OK) items[i].state = 1;
        else if (r == W_DENIED && !IsElevated()) { items[i].state = 3; admin.push_back(c); adminIdx.push_back(i); }
        else { items[i].state = 2; if (r == W_DENIED) items[i].err = L"Windows refused this change."; }
    }
    if (!admin.empty()) {
        std::vector<std::pair<bool, std::wstring>> results;
        bool cancelled = false;
        if (RunElevated(admin, results, cancelled)) {
            for (size_t k = 0; k < adminIdx.size(); ++k) {
                items[adminIdx[k]].state = results[k].first ? 1 : 2;
                items[adminIdx[k]].err = results[k].second;
            }
        } else {
            rep.cancelled = cancelled;
            for (size_t k : adminIdx) {
                items[k].state = 2;
                items[k].err = cancelled ? L"Administrator permission wasn't given." : L"Couldn't start the administrator helper.";
            }
        }
    }
    // Read the new values back before taking the lock: a handler may be slow, and the window thread
    // must never wait on it.
    std::vector<std::wstring> nowVals(changes.size());
    std::vector<char> readOk(changes.size(), 0);
    for (size_t i = 0; i < changes.size(); ++i)
        if (items[i].state == 1) readOk[i] = ReadValue(changes[i].id, nowVals[i]) ? 1 : 0;
    // A registry setting that reads back differently wasn't really changed (for example, an administrator
    // prompt answered with another account writes to that account's settings, not yours).
    for (size_t i = 0; i < changes.size(); ++i) {
        if (items[i].state != 1 || !readOk[i] || !OpsOf(changes[i].id) || StartsWith(changes[i].value, L"raw:")) continue;
        if (nowVals[i] != changes[i].value) { items[i].state = 2; items[i].err = L"Windows didn't keep this change."; }
    }
    std::lock_guard<std::mutex> lock(g_lock);
    for (size_t i = 0; i < changes.size(); ++i) {
        const Change& c = changes[i];
        if (items[i].state != 1) {
            ++rep.failed;
            if (rep.error.empty()) rep.error = items[i].err.empty() ? L"The change didn't apply." : items[i].err;
            Log(L"Change failed: %ls = %ls (%ls)", c.id.c_str(), c.value.c_str(), items[i].err.c_str());
            continue;
        }
        ++rep.ok;
        unsigned f = FlagsOf(c.id);
        rep.flags |= f;
        if (OpsOf(c.id)) anyRegistry = true;
        const std::wstring& now = nowVals[i];
        if (readOk[i]) { g_cache[c.id] = now; g_cacheDirty = true; }
        Log(L"Changed %ls -> %ls (%ls)", c.id.c_str(), c.value.size() > 80 ? L"(long value)" : c.value.c_str(), source.c_str());
        if (f & F_NOHIST) continue;
        HistEntry e;
        e.seq = g_nextSeq++;
        e.time = NowUnix();
        e.id = c.id;
        e.name = c.label.empty() ? NameOf(c.id) : c.label;
        e.before = items[i].before;
        e.was = items[i].was;
        e.after = readOk[i] ? now : c.value;
        e.source = source;
        g_history.push_back(e);
        rep.seqs.push_back(e.seq);
        if (!(f & F_NOWATCH) && readOk[i]) g_desired[c.id] = now;
    }
    if (rep.ok) {
        if (g_history.size() > 1500) g_history.erase(g_history.begin(), g_history.begin() + (g_history.size() - 1500));
        HistorySaveLocked();
        SaveMap(DesiredPath(), g_desired);
        Broadcast(rep.flags, anyRegistry);
    }
    return rep;
}

// ============================================================ cache
bool CachedValue(const std::wstring& id, std::wstring& out) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_cache.find(id);
    if (it == g_cache.end()) return false;
    out = it->second;
    return true;
}

void CacheStore(const std::wstring& id, const std::wstring& value) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_cache.find(id);
    if (it != g_cache.end() && it->second == value) return;
    g_cache[id] = value;
    g_cacheDirty = true;
}

void CacheSave() {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_cacheDirty) return;
    // Lists of services, apps and the like are rebuilt on demand; only keep what's cheap to keep
    std::map<std::wstring, std::wstring> keep;
    for (auto& kv : g_cache) if (kv.second.size() < 400) keep.insert(kv);
    SaveMap(CachePath(), keep);
    g_cacheDirty = false;
}

// ============================================================ history
std::vector<HistEntry> HistoryList() {
    std::lock_guard<std::mutex> lock(g_lock);
    return std::vector<HistEntry>(g_history.rbegin(), g_history.rend());
}

static void MarkReverted(const std::vector<uint64_t>& seqs) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& e : g_history) if (std::find(seqs.begin(), seqs.end(), e.seq) != seqs.end()) e.reverted = true;
    HistorySaveLocked();
}

ApplyReport HistoryRevert(uint64_t seq) {
    Change c;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (auto& e : g_history) if (e.seq == seq) { c.id = e.id; c.value = e.before; c.label = e.name; }
    }
    ApplyReport rep;
    if (c.id.empty()) { rep.failed = 1; rep.error = L"That change isn't in the history any more."; return rep; }
    rep = ApplyChanges({c}, L"undo");
    if (rep.ok) MarkReverted({seq});
    return rep;
}

ApplyReport HistoryUndoLast() {
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (auto it = g_history.rbegin(); it != g_history.rend(); ++it)
            if (!it->reverted && it->source != L"undo" && it->source != L"restore") { seq = it->seq; break; }
    }
    if (!seq) { ApplyReport rep; rep.error = L"Nothing to undo."; return rep; }
    return HistoryRevert(seq);
}

ApplyReport HistoryRestoreTo(double unixTime) {
    std::vector<Change> changes;
    std::vector<uint64_t> seqs;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        std::map<std::wstring, bool> seen;
        for (auto& e : g_history) {            // oldest first: the first "before" after that moment is how it was
            if (e.time < unixTime || e.reverted) continue;
            seqs.push_back(e.seq);
            if (seen[e.id]) continue;
            seen[e.id] = true;
            changes.push_back({e.id, e.before, e.name});
        }
    }
    ApplyReport rep;
    if (changes.empty()) { rep.error = L"Nothing has changed since then."; return rep; }
    rep = ApplyChanges(changes, L"restore");
    if (rep.ok) MarkReverted(seqs);
    return rep;
}

void HistoryClear() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_history.clear();
    HistorySaveLocked();
}

// ============================================================ desired state (watchdog)
std::vector<std::pair<std::wstring, std::wstring>> DesiredList() {
    std::lock_guard<std::mutex> lock(g_lock);
    return std::vector<std::pair<std::wstring, std::wstring>>(g_desired.begin(), g_desired.end());
}

void DesiredSet(const std::wstring& id, const std::wstring& value) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_desired[id] = value;
    SaveMap(DesiredPath(), g_desired);
}

void DesiredForget(const std::wstring& id) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_desired.erase(id);
    SaveMap(DesiredPath(), g_desired);
}
