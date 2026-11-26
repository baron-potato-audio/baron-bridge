// Copyright (C) 2026 Baron Potato
// SPDX-License-Identifier: GPL-3.0-only

#include "AudioEndpoints.h"
#include "Resource.h"

namespace vesper::bridge {
namespace {
struct Panel {
    std::vector<Endpoint> endpoints;
    std::wstring original;
    bool changed = false;
};

INT_PTR CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        auto* panel = reinterpret_cast<Panel*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        const auto combo = GetDlgItem(window, IDC_PLAYBACK_ENDPOINT);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Windowsの既定の再生デバイス"));
        size_t selected = 0;
        for (size_t i = 0; i < panel->endpoints.size(); ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(panel->endpoints[i].name.c_str()));
            if (panel->endpoints[i].id == panel->original) selected = i + 1;
        }
        if (selected == 0 && !panel->original.empty()) {
            panel->endpoints.push_back({panel->original, L"保存済みの再生デバイス（未接続）"});
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(panel->endpoints.back().name.c_str()));
            selected = panel->endpoints.size();
        }
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    if (LOWORD(wParam) != IDOK) return FALSE;
    auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, DWLP_USER));
    const auto selected = SendDlgItemMessageW(window, IDC_PLAYBACK_ENDPOINT, CB_GETCURSEL, 0, 0);
    if (selected < 0 || size_t(selected) > panel->endpoints.size()) return TRUE;
    const auto id = selected == 0 ? std::wstring{} : panel->endpoints[size_t(selected) - 1].id;
    if (id != panel->original && FAILED(saveEndpoint(id))) {
        MessageBoxW(window, L"再生先の設定を保存できませんでした。", L"Baron Bridge", MB_OK | MB_ICONERROR);
        return TRUE;
    }
    panel->changed = id != panel->original;
    EndDialog(window, IDOK);
    return TRUE;
}
}

bool showOutputPanel(HINSTANCE module, HWND owner, bool& changed) {
    ComScope com(COINIT_APARTMENTTHREADED);
    if (FAILED(com.result())) return false;
    Panel panel{renderEndpoints(), savedEndpoint()};
    const auto result = DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_BRIDGE_SETTINGS),
        IsWindow(owner) ? owner : nullptr, dialogProc, reinterpret_cast<LPARAM>(&panel));
    changed = panel.changed;
    return result != -1;
}
}
