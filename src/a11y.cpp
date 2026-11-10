// Breaker - screen reader support. The window is custom-drawn, so it describes itself through MSAA:
// every control on screen is a "simple element" child with a name, role, state, value and position.
// Narrator, NVDA and JAWS read these through the UI Automation bridge.
#include "a11y.h"
#include <oleacc.h>
#include <oleauto.h>

namespace {
class Acc final : public IAccessible {
    LONG ref_ = 1;
    HWND hwnd_;

    const AccItem* Item(const VARIANT& v) const {
        if (v.vt != VT_I4 || v.lVal <= 0 || v.lVal > (LONG)AccItems().size()) return nullptr;
        return &AccItems()[v.lVal - 1];
    }
    static bool Self(const VARIANT& v) { return v.vt == VT_I4 && v.lVal == CHILDID_SELF; }
    static HRESULT Str(const std::wstring& s, BSTR* out) {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (s.empty()) return S_FALSE;
        *out = SysAllocString(s.c_str());
        return *out ? S_OK : E_OUTOFMEMORY;
    }

public:
    explicit Acc(HWND h) : hwnd_(h) {}
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible) { *out = static_cast<IAccessible*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }
    // IDispatch (not used by assistive technology)
    STDMETHODIMP GetTypeInfoCount(UINT* n) override { if (n) *n = 0; return S_OK; }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    // IAccessible
    STDMETHODIMP get_accParent(IDispatch** out) override { return AccessibleObjectFromWindow(hwnd_, (DWORD)OBJID_WINDOW, IID_IDispatch, (void**)out); }
    STDMETHODIMP get_accChildCount(long* n) override { if (!n) return E_INVALIDARG; *n = (long)AccItems().size(); return S_OK; }
    STDMETHODIMP get_accChild(VARIANT, IDispatch** out) override { if (out) *out = nullptr; return S_FALSE; }
    STDMETHODIMP get_accName(VARIANT v, BSTR* out) override {
        if (Self(v)) return Str(L"Breaker", out);
        const AccItem* it = Item(v);
        return it ? Str(it->name, out) : E_INVALIDARG;
    }
    STDMETHODIMP get_accValue(VARIANT v, BSTR* out) override {
        const AccItem* it = Item(v);
        return it ? Str(it->value, out) : (out ? (*out = nullptr, S_FALSE) : E_INVALIDARG);
    }
    STDMETHODIMP get_accDescription(VARIANT v, BSTR* out) override {
        const AccItem* it = Item(v);
        return it ? Str(it->desc, out) : (out ? (*out = nullptr, S_FALSE) : E_INVALIDARG);
    }
    STDMETHODIMP get_accRole(VARIANT v, VARIANT* out) override {
        if (!out) return E_INVALIDARG;
        out->vt = VT_I4;
        const AccItem* it = Item(v);
        out->lVal = Self(v) ? ROLE_SYSTEM_CLIENT : it ? it->role : ROLE_SYSTEM_STATICTEXT;
        return (Self(v) || it) ? S_OK : E_INVALIDARG;
    }
    STDMETHODIMP get_accState(VARIANT v, VARIANT* out) override {
        if (!out) return E_INVALIDARG;
        out->vt = VT_I4;
        const AccItem* it = Item(v);
        out->lVal = Self(v) ? STATE_SYSTEM_FOCUSABLE : it ? it->state : 0;
        return (Self(v) || it) ? S_OK : E_INVALIDARG;
    }
    STDMETHODIMP get_accHelp(VARIANT, BSTR* out) override { if (out) *out = nullptr; return S_FALSE; }
    STDMETHODIMP get_accHelpTopic(BSTR* file, VARIANT, long*) override { if (file) *file = nullptr; return S_FALSE; }
    STDMETHODIMP get_accKeyboardShortcut(VARIANT, BSTR* out) override { if (out) *out = nullptr; return S_FALSE; }
    STDMETHODIMP get_accFocus(VARIANT* out) override {
        if (!out) return E_INVALIDARG;
        int f = AccFocusedItem();
        if (GetFocus() != hwnd_) { out->vt = VT_EMPTY; return S_FALSE; }
        out->vt = VT_I4;
        out->lVal = f >= 0 ? f + 1 : CHILDID_SELF;
        return S_OK;
    }
    STDMETHODIMP get_accSelection(VARIANT* out) override { if (out) out->vt = VT_EMPTY; return S_FALSE; }
    STDMETHODIMP get_accDefaultAction(VARIANT v, BSTR* out) override {
        const AccItem* it = Item(v);
        return it ? Str(it->action, out) : (out ? (*out = nullptr, S_FALSE) : E_INVALIDARG);
    }
    STDMETHODIMP accSelect(long, VARIANT) override { return S_FALSE; }
    STDMETHODIMP accLocation(long* x, long* y, long* w, long* h, VARIANT v) override {
        if (!x || !y || !w || !h) return E_INVALIDARG;
        RECT rc;
        if (Self(v)) { GetClientRect(hwnd_, &rc); MapWindowPoints(hwnd_, nullptr, (POINT*)&rc, 2); }
        else if (const AccItem* it = Item(v)) rc = it->rc;
        else return E_INVALIDARG;
        *x = rc.left; *y = rc.top; *w = rc.right - rc.left; *h = rc.bottom - rc.top;
        return S_OK;
    }
    STDMETHODIMP accNavigate(long dir, VARIANT start, VARIANT* out) override {
        if (!out) return E_INVALIDARG;
        out->vt = VT_EMPTY;
        long n = (long)AccItems().size(), id = 0;
        if (Self(start)) id = dir == NAVDIR_FIRSTCHILD ? 1 : dir == NAVDIR_LASTCHILD ? n : 0;
        else if (start.vt == VT_I4) id = (dir == NAVDIR_NEXT || dir == NAVDIR_DOWN || dir == NAVDIR_RIGHT) ? start.lVal + 1
                                       : (dir == NAVDIR_PREVIOUS || dir == NAVDIR_UP || dir == NAVDIR_LEFT) ? start.lVal - 1 : 0;
        if (id < 1 || id > n) return S_FALSE;
        out->vt = VT_I4;
        out->lVal = id;
        return S_OK;
    }
    STDMETHODIMP accHitTest(long x, long y, VARIANT* out) override {
        if (!out) return E_INVALIDARG;
        POINT pt{x, y};
        auto& items = AccItems();
        for (long i = (long)items.size() - 1; i >= 0; --i)
            if (PtInRect(&items[i].rc, pt)) { out->vt = VT_I4; out->lVal = i + 1; return S_OK; }
        RECT rc;
        GetClientRect(hwnd_, &rc);
        MapWindowPoints(hwnd_, nullptr, (POINT*)&rc, 2);
        if (PtInRect(&rc, pt)) { out->vt = VT_I4; out->lVal = CHILDID_SELF; return S_OK; }
        out->vt = VT_EMPTY;
        return S_FALSE;
    }
    STDMETHODIMP accDoDefaultAction(VARIANT v) override {
        if (!Item(v)) return E_INVALIDARG;
        AccInvoke(v.lVal - 1);
        return S_OK;
    }
    STDMETHODIMP put_accName(VARIANT, BSTR) override { return DISP_E_MEMBERNOTFOUND; }
    STDMETHODIMP put_accValue(VARIANT, BSTR) override { return DISP_E_MEMBERNOTFOUND; }
};
}  // namespace

static Acc* g_acc = nullptr;

LRESULT AccOnGetObject(HWND hwnd, WPARAM wp, LPARAM) {
    if (!g_acc) g_acc = new Acc(hwnd);
    return LresultFromObject(IID_IAccessible, wp, static_cast<IAccessible*>(g_acc));
}

void AccNotifyFocus(HWND hwnd, int index) { NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, index >= 0 ? index + 1 : CHILDID_SELF); }
void AccNotifyChanged(HWND hwnd, int index) {
    if (index < 0) return;
    NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd, OBJID_CLIENT, index + 1);
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd, OBJID_CLIENT, index + 1);
}
void AccNotifyReorder(HWND hwnd) { NotifyWinEvent(EVENT_OBJECT_REORDER, hwnd, OBJID_CLIENT, CHILDID_SELF); }
