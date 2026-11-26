// Copyright (C) 2026 Baron Potato
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace vesper::bridge {
using Microsoft::WRL::ComPtr;

inline constexpr GUID classId{0x7ab3f5dc, 0x6138, 0x43e2, {0x83, 0x1a, 0x96, 0xc4, 0xe1, 0x1d, 0x5b, 0xe1}};
inline constexpr char driverName[] = "Baron Bridge";
inline constexpr wchar_t settingsKey[] = L"Software\\Vesper Clav\\ASIO Bridge";

class ComScope {
public:
    explicit ComScope(DWORD model = COINIT_MULTITHREADED) noexcept : status(CoInitializeEx(nullptr, model)) {}
    ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
    HRESULT result() const noexcept { return status == RPC_E_CHANGED_MODE ? S_OK : status; }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
private:
    const HRESULT status;
};

struct Endpoint {
    std::wstring id;
    std::wstring name;
};

std::wstring savedEndpoint();
HRESULT saveEndpoint(const std::wstring& id) noexcept;
HRESULT openEndpoint(ComPtr<IMMDevice>& result);
HRESULT activateAudio(const ComPtr<IMMDevice>& endpoint, ComPtr<IAudioClient>& result) noexcept;
std::vector<Endpoint> renderEndpoints();
std::wstring endpointName(const ComPtr<IMMDevice>& endpoint);
}
