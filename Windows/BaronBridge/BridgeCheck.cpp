// Copyright (C) 2026 Baron Potato
// SPDX-License-Identifier: GPL-3.0-only

#include "AudioEndpoints.h"
#include <iasiodrv.h>
#include <ksmedia.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
using namespace vesper::bridge;
constexpr std::array frequencies{997., 1237.};
constexpr double pi = std::numbers::pi;

void expect(bool ok, const char* detail) {
    if (!ok) throw std::runtime_error(detail);
}

void expectHr(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return;
    char code[14]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    throw std::runtime_error(std::string(operation) + " (" + code + ")");
}

uint64_t join(unsigned long hi, unsigned long lo) { return (uint64_t(hi) << 32) | lo; }

class Loopback {
public:
    Loopback() {
        ComPtr<IMMDevice> endpoint;
        expectHr(openEndpoint(endpoint), "Locate WASAPI playback endpoint");
        expectHr(activateAudio(endpoint, client), "Activate loopback client");
        WAVEFORMATEX* spec = nullptr;
        expectHr(client->GetMixFormat(&spec), "Read endpoint mix format");
        format.reset(spec);
        const auto* extension = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(spec);
        isFloat = spec->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
            || (spec->wFormatTag == WAVE_FORMAT_EXTENSIBLE && extension->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        expect(spec->nChannels >= 2 && (spec->wBitsPerSample == 16 || spec->wBitsPerSample == 24 || spec->wBitsPerSample == 32),
            "Loopback endpoint has an unsupported test format");
        expectHr(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, spec, nullptr),
            "Open shared WASAPI loopback");
        expectHr(client->GetService(IID_PPV_ARGS(&capture)), "Get loopback capture service");
        expectHr(client->Start(), "Start loopback capture");
    }
    ~Loopback() { client->Stop(); }

    void read() {
        for (;;) {
            UINT32 frames = 0;
            expectHr(capture->GetNextPacketSize(&frames), "Read loopback packet size");
            if (frames == 0) break;
            BYTE* data = nullptr;
            DWORD flags = 0;
            expectHr(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Read loopback samples");
            for (UINT32 i = 0; i < frames; ++i) {
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const auto* frame = silent ? nullptr : data + i * format->nBlockAlign;
                const auto sampleBytes = format->wBitsPerSample / 8;
                const double a = silent ? 0. : sample(frame), b = silent ? 0. : sample(frame + sampleBytes);
                peak[0] = std::max(peak[0], std::abs(a));
                peak[1] = std::max(peak[1], std::abs(b));
                for (size_t tone = 0; tone < frequencies.size(); ++tone) {
                    const auto phase = 2. * pi * frequencies[tone] * double(totalFrames) / format->nSamplesPerSec;
                    const auto sine = std::sin(phase), cosine = std::cos(phase);
                    sums[0][tone][0] += sine * a;
                    sums[0][tone][1] += cosine * a;
                    sums[1][tone][0] += sine * b;
                    sums[1][tone][1] += cosine * b;
                }
                ++totalFrames;
            }
            expectHr(capture->ReleaseBuffer(frames), "Release loopback samples");
        }
    }

    void verify() const {
        expect(totalFrames > format->nSamplesPerSec / 10, "WASAPI loopback did not deliver enough frames");
        const auto amplitude = [this](size_t channel, size_t tone) {
            const auto& phases = sums[channel][tone];
            return 2. * std::hypot(phases[0], phases[1]) / double(totalFrames);
        };
        const auto left = amplitude(0, 0), right = amplitude(1, 1);
        std::cout << "  WASAPI loopback " << format->nSamplesPerSec << " Hz: L 997 Hz " << left
                  << ", R 1237 Hz " << right << '\n';
        expect(left > .0015 && right > .0015 && left > amplitude(1, 0) * 3. && right > amplitude(0, 1) * 3.,
            "Expected stereo signal was not found on the WASAPI playback endpoint");
    }
    void report() const {
        std::cout << "WASAPI loopback: " << totalFrames << " frames at " << format->nSamplesPerSec
            << " Hz, peaks L " << peak[0] << ", R " << peak[1] << '\n';
    }

private:
    double sample(const BYTE* data) const {
        if (isFloat) {
            float value = 0;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }
        const auto bytes = format->wBitsPerSample / 8;
        uint32_t raw = 0;
        for (int i = 0; i < bytes; ++i) raw |= uint32_t(data[i]) << (8 * i);
        return double(static_cast<int32_t>(raw << (32 - 8 * bytes))) / 2147483648.;
    }

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format{nullptr, CoTaskMemFree};
    std::array<std::array<std::array<double, 2>, 2>, 2> sums{};
    std::array<double, 2> peak{};
    bool isFloat = false;
    uint64_t totalFrames = 0;
};

struct Host {
    Host(IASIO& driver, double sr, long frames, bool timed)
        : asio(driver), rate(sr), block(frames), expectsTime(timed) {
        channels[0].channelNum = 0;
        channels[1].channelNum = 1;
    }
    ~Host() {
        asio.stop();
        asio.disposeBuffers();
        active = nullptr;
    }
    void start() {
        active = this;
        expect(asio.setSampleRate(rate) == ASE_OK, "Driver rejected the sample rate");
        ASIOCallbacks callbacks{buffer, sampleRateChanged, message, timestampedBuffer};
        expect(asio.createBuffers(channels.data(), 2, block, &callbacks) == ASE_OK, "Driver could not create buffers");
        if (asio.start() != ASE_OK) {
            char error[124]{};
            asio.getErrorMessage(error);
            throw std::runtime_error(std::string("Driver could not start: ") + error);
        }
    }
    static void buffer(long index, ASIOBool) { active->fill(index, nullptr); }
    static ASIOTime* timestampedBuffer(ASIOTime* time, long index, ASIOBool) {
        active->fill(index, time);
        return nullptr;
    }
    static void sampleRateChanged(ASIOSampleRate) {}
    static long message(long selector, long, void*, double*) {
        return selector == kAsioSupportsTimeInfo && active->expectsTime ? 1 : 0;
    }
    void fill(long index, const ASIOTime* timing) {
        valid = valid && (index == 0 || index == 1) && index != lastIndex;
        if (index != 0 && index != 1) return;
        ASIOSamples sample{};
        ASIOTimeStamp time{};
        valid = valid && asio.getSamplePosition(&sample, &time) == ASE_OK;
        const auto timestamp = join(time.hi, time.lo);
        valid = valid && join(sample.hi, sample.lo) == position && (position == 0 || timestamp > lastTimestamp);
        valid = valid && ((timing != nullptr) == expectsTime);
        if (timing) {
            const auto info = timing->timeInfo;
            valid = valid && info.sampleRate == rate && (info.flags & kSamplePositionValid)
                && join(info.samplePosition.hi, info.samplePosition.lo) == position;
        }
        for (size_t channel = 0; channel < channels.size(); ++channel) {
            auto* output = static_cast<float*>(channels[channel].buffers[index]);
            for (long i = 0; i < block; ++i)
                output[i] = float(.006 * std::sin(2. * pi * frequencies[channel] * double(position + uint64_t(i)) / rate));
        }
        lastIndex = index;
        lastTimestamp = timestamp;
        position += uint64_t(block);
        ++switchCount;
    }

    inline static Host* active = nullptr;
    IASIO& asio;
    const double rate;
    const long block;
    const bool expectsTime;
    bool valid = true;
    long lastIndex = -1;
    uint64_t position = 0, lastTimestamp = 0;
    size_t switchCount = 0;
    std::array<ASIOBufferInfo, 2> channels{};
};

void testAudio(IASIO& asio, double rate, long block, bool timeInfo) {
    using namespace std::chrono_literals;
    Loopback loopback;
    Host host(asio, rate, block, timeInfo);
    host.start();
    const auto end = std::chrono::steady_clock::now() + 420ms;
    while (std::chrono::steady_clock::now() < end) {
        Sleep(5);
        loopback.read();
    }
    expect(asio.stop() == ASE_OK, "Driver could not stop");
    expect(host.switchCount > 8 && host.valid, "Driver callback, sample position or timing check failed");
    long input = -1, output = -1;
    expect(asio.getLatencies(&input, &output) == ASE_OK && input == 0 && output > 0, "Driver latency report failed");
    std::cout << "PASS " << rate << " Hz, " << block << " frames, " << host.switchCount
              << " callbacks, reported output latency " << output << " frames\n";
    loopback.verify();
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        ComScope com;
        expectHr(com.result(), "Initialize COM");
        if (argc == 2 && std::wstring_view(argv[1]) == L"--meter") {
            Loopback loopback;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < end) {
                Sleep(5);
                loopback.read();
            }
            loopback.report();
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--endpoint") {
            ComPtr<IMMDevice> endpoint;
            expectHr(openEndpoint(endpoint), "Open chosen endpoint");
            const auto name = endpointName(endpoint);
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                std::string label(size_t(bytes), '\0');
                WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, label.data(), bytes, nullptr, nullptr);
                std::cout << label.c_str() << '\n';
            }
            return 0;
        }
        expect(argc == 1 || argc == 2, "Usage: baron_bridge_check [DLL path | --endpoint | --meter]");
        const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> module(
            argc == 2 ? LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
                      : nullptr, FreeLibrary);
        ComPtr<IASIO> asio;
        if (argc == 2) {
            expect(bool(module), "Could not load the Baron Bridge DLL");
            using GetFactory = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
            const auto function = GetProcAddress(module.get(), "DllGetClassObject");
            GetFactory getFactory = nullptr;
            static_assert(sizeof(getFactory) == sizeof(function));
            std::memcpy(&getFactory, &function, sizeof(getFactory));
            expect(getFactory != nullptr, "DLL does not export DllGetClassObject");
            ComPtr<IClassFactory> factory;
            expectHr(getFactory(classId, IID_PPV_ARGS(&factory)), "Get driver class factory");
            expectHr(factory->CreateInstance(nullptr, classId, reinterpret_cast<void**>(asio.GetAddressOf())), "Create driver");
        } else {
            expectHr(CoCreateInstance(classId, nullptr, CLSCTX_INPROC_SERVER, classId,
                reinterpret_cast<void**>(asio.GetAddressOf())), "Load registered driver");
        }
        expect(asio->init(nullptr) == ASIOTrue, "Driver initialization failed");
        char registeredName[32]{};
        asio->getDriverName(registeredName);
        expect(std::string_view(registeredName) == driverName, "Incorrect driver name");
        std::cout << "PASS driver name: " << registeredName << '\n';
        long input = -1, output = -1;
        expect(asio->getChannels(&input, &output) == ASE_OK && input == 0 && output == 2, "Wrong driver topology");
        ASIOChannelInfo info{};
        expect(asio->getChannelInfo(&info) == ASE_OK && info.type == ASIOSTFloat32LSB, "Wrong driver sample type");
        testAudio(*asio.Get(), 48000., 256, false);
        testAudio(*asio.Get(), 44100., 128, true);
        testAudio(*asio.Get(), 96000., 512, true);
        std::cout << "PASS stereo playback captured through Windows WASAPI loopback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
