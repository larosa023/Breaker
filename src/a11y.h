// Breaker - screen reader support (MSAA / IAccessible over the custom-drawn UI)
#pragma once
#include "common.h"

struct AccItem {
    std::wstring name, value, desc;
    RECT rc{};            // screen coordinates
    long role = 0;        // ROLE_SYSTEM_*
    long state = 0;       // STATE_SYSTEM_*
    std::wstring action;  // default action name ("Toggle", "Open"...)
};

// Provided by the window
const std::vector<AccItem>& AccItems();
int AccFocusedItem();              // index into AccItems, or -1
void AccInvoke(int index);

LRESULT AccOnGetObject(HWND hwnd, WPARAM wp, LPARAM lp);
void AccNotifyFocus(HWND hwnd, int index);
void AccNotifyChanged(HWND hwnd, int index);
void AccNotifyReorder(HWND hwnd);
