// Copyright (C) 2026 Baron Potato
// SPDX-License-Identifier: GPL-3.0-only

#include "AudioEndpoints.h"
#include <iasiodrv.h>
#include <avrt.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace vesper::bridge {
bool showOutputPanel(HINSTANCE module, HWND owner, bool& changed);

namespace {
HINSTANCE moduleInstance = nullptr;
std::atomic<long> activeObjects{0};

using Event = std::unique_ptr<void, decltype(&CloseHandle)>;
Event makeEvent(bool manualReset = false) noexcept {
    return Event(CreateEventW(nullptr, manualReset, FALSE, nullptr), CloseHandle);
}

constexpr long minBlock = 64, maxBlock = 2048, preferredBlock = 256;
constexpr std::array supportedRates{44100., 48000., 88200., 96000., 176400., 192000.};

template<class Position>
Position split(uint64_t integer) noexcept {
    return {static_cast<unsigned long>(integer >> 32), static_cast<unsigned long>(integer)};
}

class RenderStream {
public:
    HRESULT open(DWORD rate, UINT32 hostBlock) {
        notify = makeEvent();
        if (!notify) return HRESULT_FROM_WIN32(GetLastError());
        ComPtr<IMMDevice> endpoint;
        if (const HRESULT hr = openEndpoint(endpoint); FAILED(hr)) return hr;
        if (const HRESULT hr = activateAudio(endpoint, audio); FAILED(hr)) return hr;

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 2 * sizeof(float);
        format.nAvgBytesPerSec = rate * format.nBlockAlign;

        if (FAILED(openLowLatency(format, hostBlock))) {
            // Use a fresh client for the format-converting shared stream.
            if (const HRESULT hr = activateAudio(endpoint, audio); FAILED(hr)) return hr;
            constexpr DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            constexpr REFERENCE_TIME duration = 200000; // 20 ms total endpoint buffer
            if (const HRESULT hr = audio->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, &format, nullptr);
                    FAILED(hr)) return hr;
        }
        if (const HRESULT hr = audio->SetEventHandle(notify.get()); FAILED(hr)) return hr;
        if (const HRESULT hr = audio->GetBufferSize(&capacity); FAILED(hr)) return hr;
        if (const HRESULT hr = audio->GetService(IID_PPV_ARGS(&render)); FAILED(hr)) return hr;

        REFERENCE_TIME streamTime = 0, period = 0;
        audio->GetStreamLatency(&streamTime);
        audio->GetDevicePeriod(&period, nullptr);
        const auto periodFrames = UINT32(std::clamp<REFERENCE_TIME>(
            (period * rate + 9999999) / 10000000, 1, capacity));
        queueFrames = std::min(capacity, periodFrames * 2);
        latencyFrames = std::max(hostBlock, UINT32((streamTime * rate + 9999999) / 10000000)) + hostBlock;

        BYTE* data = nullptr;
        if (const HRESULT hr = render->GetBuffer(queueFrames, &data); FAILED(hr)) return hr;
        if (const HRESULT hr = render->ReleaseBuffer(queueFrames, AUDCLNT_BUFFERFLAGS_SILENT); FAILED(hr)) return hr;
        return audio->Start();
    }

    HANDLE event() const noexcept { return notify.get(); }
    long latency() const noexcept { return long(latencyFrames); }

    template<class Fill>
    HRESULT write(Fill&& fill) noexcept {
        UINT32 padding = 0;
        if (const HRESULT hr = audio->GetCurrentPadding(&padding); FAILED(hr)) return hr;
        if (padding >= queueFrames) return S_OK;
        const auto count = queueFrames - padding;
        BYTE* data = nullptr;
        if (const HRESULT hr = render->GetBuffer(count, &data); FAILED(hr)) return hr;
        fill(reinterpret_cast<float*>(data), count);
        return render->ReleaseBuffer(count, 0);
    }

    void stop() noexcept { audio->Stop(); }

private:
    HRESULT openLowLatency(const WAVEFORMATEX& format, UINT32 hostBlock) {
        ComPtr<IAudioClient3> modern;
        if (const HRESULT hr = audio.As(&modern); FAILED(hr)) return hr;
        UINT32 normal = 0, step = 0, minimum = 0, maximum = 0;
        if (const HRESULT hr = modern->GetSharedModeEnginePeriod(&format, &normal, &step, &minimum, &maximum);
                FAILED(hr)) return hr;
        if (step == 0 || minimum > maximum) return E_UNEXPECTED;
        const auto period = std::clamp((hostBlock / step) * step, minimum, maximum);
        return modern->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, &format, nullptr);
    }

    Event notify{nullptr, CloseHandle};
    ComPtr<IAudioClient> audio;
    ComPtr<IAudioRenderClient> render;
    UINT32 capacity = 0, queueFrames = 0, latencyFrames = 0;
};

class Driver final : public IASIO {
public:
    Driver() { ++activeObjects; }
    ~Driver() { stop(); --activeObjects; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (result == nullptr) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != classId) return E_NOINTERFACE;
        *result = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references;
        if (left == 0) delete this;
        return left;
    }

    ASIOBool init(void* sysHandle) override {
        owner = static_cast<HWND>(sysHandle);
        try {
            ComScope com;
            if (FAILED(com.result())) { lastError = com.result(); return ASIOFalse; }
            ComPtr<IMMDevice> endpoint;
            ComPtr<IAudioClient> client;
            HRESULT hr = openEndpoint(endpoint);
            if (SUCCEEDED(hr)) hr = activateAudio(endpoint, client);
            WAVEFORMATEX* rawFormat = nullptr;
            if (SUCCEEDED(hr)) hr = client->GetMixFormat(&rawFormat);
            const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(rawFormat, CoTaskMemFree);
            if (SUCCEEDED(hr) && canSampleRate(format->nSamplesPerSec) == ASE_OK)
                rate = format->nSamplesPerSec;
            lastError = hr;
            // Allow the host to open the control panel if a saved endpoint is unplugged.
            return ASIOTrue;
        } catch (...) {
            lastError = E_OUTOFMEMORY;
            return ASIOFalse;
        }
    }
    void getDriverName(char* name) override { if (name) std::memcpy(name, driverName, sizeof(driverName)); }
    long getDriverVersion() override { return 0x00010002; }
    void getErrorMessage(char* message) override {
        if (message == nullptr) return;
        const auto hr = lastError.load();
        if (FAILED(hr))
            std::snprintf(message, 124, "WASAPI shared output: 0x%08lX. Check the playback device in the Baron Bridge control panel.",
                static_cast<unsigned long>(hr));
        else
            message[0] = '\0';
    }

    ASIOError getChannels(long* inputs, long* outputs) override {
        if (!inputs || !outputs) return ASE_InvalidParameter;
        *inputs = 0;
        *outputs = 2;
        return ASE_OK;
    }
    ASIOError getLatencies(long* inputs, long* outputs) override {
        if (!inputs || !outputs) return ASE_InvalidParameter;
        *inputs = 0;
        *outputs = outputLatency.load();
        return ASE_OK;
    }
    ASIOError getBufferSize(long* minimum, long* maximum, long* preferred, long* granularity) override {
        if (!minimum || !maximum || !preferred || !granularity) return ASE_InvalidParameter;
        *minimum = minBlock;
        *maximum = maxBlock;
        *preferred = preferredBlock;
        *granularity = -1; // powers of two
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override {
        return std::find(supportedRates.begin(), supportedRates.end(), sampleRate) != supportedRates.end()
            ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override {
        if (!sampleRate) return ASE_InvalidParameter;
        *sampleRate = rate.load();
        return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override {
        if (canSampleRate(sampleRate) != ASE_OK) return ASE_NoClock;
        const std::lock_guard lock(lifecycle);
        if (running.load() && rate.load() != sampleRate) return ASE_InvalidMode;
        rate = sampleRate;
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* clocks, long* count) override {
        if (!clocks || !count || *count < 1) return ASE_InvalidParameter;
        clocks[0] = {0, -1, -1, ASIOTrue, "Windows playback"};
        *count = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long index) override { return index == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* time) override {
        if (!position || !time) return ASE_InvalidParameter;
        if (!running.load()) return ASE_SPNotAdvancing;
        const auto sample = lastPosition.load();
        *position = split<ASIOSamples>(sample);
        *time = split<ASIOTimeStamp>(timestamp(sample));
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info || info->isInput || info->channel < 0 || info->channel > 1) return ASE_InvalidParameter;
        info->channelGroup = 0;
        info->isActive = (channelMask.load() & (1u << info->channel)) ? ASIOTrue : ASIOFalse;
        info->type = ASIOSTFloat32LSB;
        strcpy_s(info->name, info->channel == 0 ? "WASAPI Left" : "WASAPI Right");
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* host) override {
        if (!infos || !host || count < 1 || count > 2 || size < minBlock || size > maxBlock || (size & (size - 1)) != 0)
            return ASE_InvalidParameter;
        const std::lock_guard lock(lifecycle);
        if (worker.joinable() || channelMask.load() != 0) return ASE_InvalidMode;
        unsigned mask = 0;
        for (long i = 0; i < count; ++i) {
            if (infos[i].isInput || infos[i].channelNum < 0 || infos[i].channelNum > 1) return ASE_InvalidParameter;
            const auto bit = 1u << infos[i].channelNum;
            if (mask & bit) return ASE_InvalidParameter;
            mask |= bit;
        }
        timeInfo = host->bufferSwitchTimeInfo && host->asioMessage
            && host->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) != 0;
        if (!timeInfo && !host->bufferSwitch) return ASE_InvalidParameter;
        callbacks = *host;
        blockFrames = UINT32(size);
        outputLatency = size + long(rate.load() * .01);
        for (long i = 0; i < count; ++i) {
            const auto channel = size_t(infos[i].channelNum);
            for (int buffer = 0; buffer < 2; ++buffer)
                infos[i].buffers[buffer] = buffers[channel][buffer].data();
        }
        channelMask = mask;
        return ASE_OK;
    }

    ASIOError disposeBuffers() override {
        const std::lock_guard lock(lifecycle);
        if (!stopLocked()) return ASE_InvalidMode;
        channelMask = 0;
        callbacks = {};
        return ASE_OK;
    }

    ASIOError start() override {
        const std::lock_guard lock(lifecycle);
        if (running.load()) return ASE_OK;
        if (channelMask.load() == 0 || !stopLocked()) return ASE_InvalidMode;
        stopSignal = makeEvent(true);
        opened = makeEvent(true);
        if (!stopSignal || !opened) {
            lastError = HRESULT_FROM_WIN32(GetLastError());
            return ASE_HWMalfunction;
        }
        try {
            worker = std::thread([this] { run(); });
        } catch (...) {
            lastError = E_OUTOFMEMORY;
            return ASE_NoMemory;
        }
        if (WaitForSingleObject(opened.get(), 5000) != WAIT_OBJECT_0) {
            stopLocked();
            lastError = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            return ASE_HWMalfunction;
        }
        if (FAILED(startupResult)) {
            stopLocked();
            return ASE_HWMalfunction;
        }
        return ASE_OK;
    }
    ASIOError stop() override {
        const std::lock_guard lock(lifecycle);
        return stopLocked() ? ASE_OK : ASE_InvalidMode;
    }
    ASIOError controlPanel() override {
        try {
            bool changed = false;
            if (!showOutputPanel(moduleInstance, owner, changed)) return ASE_HWMalfunction;
            if (changed) requestReset();
            return ASE_OK;
        } catch (...) {
            return ASE_NoMemory;
        }
    }
    ASIOError future(long selector, void*) override {
        return selector == kAsioCanTimeInfo ? ASE_SUCCESS : ASE_NotPresent;
    }
    ASIOError outputReady() override { return ASE_NotPresent; }

private:
    bool stopLocked() noexcept {
        if (!worker.joinable()) return true;
        SetEvent(stopSignal.get());
        if (worker.get_id() == std::this_thread::get_id()) return false;
        worker.join();
        return true;
    }
    void requestReset() noexcept {
        const auto notify = callbacks.asioMessage;
        if (notify && notify(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr))
            notify(kAsioResetRequest, 0, nullptr, nullptr);
    }
    uint64_t timestamp(uint64_t frame) const noexcept {
        return epoch.load() + uint64_t(double(frame) * 1.e9 / rate.load());
    }

    void produce(float* target, UINT32 count) noexcept {
        const auto mask = channelMask.load();
        while (count != 0) {
            if (sourceOffset == blockFrames) {
                blockIndex ^= 1;
                for (auto& channel : buffers) std::fill_n(channel[blockIndex].data(), blockFrames, 0.f);
                lastPosition.store(nextPosition);
                if (timeInfo) {
                    ASIOTime timing{};
                    timing.timeInfo = {1., split<ASIOTimeStamp>(timestamp(nextPosition)), split<ASIOSamples>(nextPosition),
                        rate.load(), kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid, {}};
                    callbacks.bufferSwitchTimeInfo(&timing, blockIndex, ASIOTrue);
                } else {
                    callbacks.bufferSwitch(blockIndex, ASIOTrue);
                }
                sourceOffset = 0;
                nextPosition += blockFrames;
            }
            const auto frames = std::min(count, blockFrames - sourceOffset);
            const auto* left = buffers[0][blockIndex].data() + sourceOffset;
            const auto* right = buffers[1][blockIndex].data() + sourceOffset;
            for (UINT32 i = 0; i < frames; ++i) {
                target[2 * i] = mask & 1u ? left[i] : 0.f;
                target[2 * i + 1] = mask & 2u ? right[i] : 0.f;
            }
            target += 2 * frames;
            sourceOffset += frames;
            count -= frames;
        }
    }

    void run() noexcept {
        bool started = false;
        HRESULT hr;
        try {
            hr = stream(started);
        } catch (...) {
            hr = E_OUTOFMEMORY;
        }
        lastError = hr;
        running.store(false);
        if (!started) {
            startupResult = hr;
            SetEvent(opened.get());
        } else if (FAILED(hr) && WaitForSingleObject(stopSignal.get(), 0) != WAIT_OBJECT_0) {
            requestReset();
        }
    }
    HRESULT stream(bool& started) {
        ComScope com;
        if (FAILED(com.result())) return com.result();
        RenderStream output;
        if (const HRESULT hr = output.open(DWORD(rate.load()), blockFrames); FAILED(hr)) return hr;
        DWORD taskIndex = 0;
        auto priority = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        sourceOffset = blockFrames;
        blockIndex = 1;
        nextPosition = 0;
        lastPosition.store(0);
        epoch.store(uint64_t(timeGetTime()) * 1000000u);
        outputLatency = output.latency();
        startupResult = S_OK;
        running.store(true);
        started = true;
        SetEvent(opened.get());
        const HANDLE events[]{stopSignal.get(), output.event()};
        HRESULT status = S_OK;
        for (;;) {
            const auto ready = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (ready == WAIT_OBJECT_0) break;
            if (ready != WAIT_OBJECT_0 + 1) { status = HRESULT_FROM_WIN32(GetLastError()); break; }
            status = output.write([this](float* out, UINT32 n) { produce(out, n); });
            if (FAILED(status)) break;
        }
        output.stop();
        if (priority) AvRevertMmThreadCharacteristics(priority);
        return status;
    }

    std::atomic<ULONG> references{1};
    HWND owner = nullptr;
    std::atomic<HRESULT> lastError{S_OK};
    std::atomic<double> rate{48000.};
    std::atomic<long> outputLatency{preferredBlock + 480};
    std::atomic<unsigned> channelMask{0};
    std::atomic<uint64_t> lastPosition{0}, epoch{0};
    std::atomic<bool> running{false};
    std::mutex lifecycle;
    Event stopSignal{nullptr, CloseHandle}, opened{nullptr, CloseHandle};
    std::thread worker;
    std::atomic<HRESULT> startupResult{E_FAIL};
    ASIOCallbacks callbacks{};
    bool timeInfo = false;
    UINT32 blockFrames = preferredBlock, sourceOffset = 0;
    long blockIndex = 0;
    uint64_t nextPosition = 0;
    std::array<std::array<std::array<float, maxBlock>, 2>, 2> buffers{};
};

class Factory final : public IClassFactory {
public:
    Factory() { ++activeObjects; }
    ~Factory() { --activeObjects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *result = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = --references;
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* driver = new (std::nothrow) Driver;
        if (!driver) return E_OUTOFMEMORY;
        const auto status = driver->QueryInterface(iid, result);
        driver->Release();
        return status;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++activeObjects; else --activeObjects;
        return S_OK;
    }
private:
    std::atomic<ULONG> references{1};
};
}
}

extern "C" BOOL WINAPI DllMain(HINSTANCE handle, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) vesper::bridge::moduleInstance = handle;
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    using namespace vesper::bridge;
    if (!result) return E_POINTER;
    *result = nullptr;
    if (clsid != classId) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) Factory;
    if (!factory) return E_OUTOFMEMORY;
    const auto status = factory->QueryInterface(iid, result);
    factory->Release();
    return status;
}

STDAPI DllCanUnloadNow() {
    return vesper::bridge::activeObjects.load() == 0 ? S_OK : S_FALSE;
}
