// Copyright (C) 2026 Baron Potato
// SPDX-License-Identifier: GPL-3.0-only

#include "AudioEndpoints.h"
#include <propsys.h>
#include <algorithm>
#include <memory>

namespace vesper::bridge {
namespace {
constexpr PROPERTYKEY friendlyName{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
};

HRESULT enumerate(ComPtr<IMMDeviceEnumerator>& result) noexcept {
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&result));
}
}

std::wstring savedEndpoint() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"Endpoint", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS
            || bytes < sizeof(wchar_t) || bytes > 16384)
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"Endpoint", RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    value.resize(std::char_traits<wchar_t>::length(value.c_str()));
    return value;
}

HRESULT saveEndpoint(const std::wstring& id) noexcept {
    if (id.empty()) {
        const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, settingsKey, L"Endpoint");
        return result == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(result);
    }
    return HRESULT_FROM_WIN32(RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, L"Endpoint", REG_SZ,
        id.c_str(), static_cast<DWORD>((id.size() + 1) * sizeof(wchar_t))));
}

HRESULT openEndpoint(ComPtr<IMMDevice>& result) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (const HRESULT hr = enumerate(enumerator); FAILED(hr)) return hr;
    const auto id = savedEndpoint();
    if (id.empty()) return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &result);
    if (const HRESULT hr = enumerator->GetDevice(id.c_str(), &result); FAILED(hr)) return hr;
    DWORD state = 0;
    const HRESULT hr = result->GetState(&state);
    if (FAILED(hr)) return hr;
    return state == DEVICE_STATE_ACTIVE ? S_OK : AUDCLNT_E_DEVICE_INVALIDATED;
}

HRESULT activateAudio(const ComPtr<IMMDevice>& endpoint, ComPtr<IAudioClient>& result) noexcept {
    return endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(result.ReleaseAndGetAddressOf()));
}

std::wstring endpointName(const ComPtr<IMMDevice>& endpoint) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(endpoint->OpenPropertyStore(STGM_READ, &properties))) return {};
    PROPVARIANT value{};
    const HRESULT hr = properties->GetValue(friendlyName, &value);
    std::wstring name;
    if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) name = value.pwszVal;
    PropVariantClear(&value);
    return name;
}

std::vector<Endpoint> renderEndpoints() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerate(enumerator))
            || FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return {};
    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) return {};
    std::vector<Endpoint> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;
        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId))) continue;
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> id(rawId, CoTaskMemFree);
        auto name = endpointName(device);
        if (!name.empty()) result.push_back({id.get(), std::move(name)});
    }
    std::sort(result.begin(), result.end(), [](const Endpoint& a, const Endpoint& b) { return a.name < b.name; });
    return result;
}
}
