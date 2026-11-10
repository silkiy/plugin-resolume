#include "WindowsAudio.h"

#define NOMINMAX
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

using Microsoft::WRL::ComPtr;

struct WindowsAudio::State {
    std::atomic<float> peak{-1.0f};
    std::atomic<bool> stop{false};
    std::thread worker;

    State() : worker([this] { Run(); }) {}
    ~State()
    {
        stop.store(true);
        if (worker.joinable()) worker.join();
    }

    void Run()
    {
        while (!stop.load()) {
            bool comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioCaptureClient> capture;
            WAVEFORMATEX* format = nullptr;
            bool active = false;
            UINT32 channels = 0;
            WORD bits = 0;
            bool floating = false;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                           IID_PPV_ARGS(&enumerator))) &&
                SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
                SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(client.GetAddressOf()))) &&
                SUCCEEDED(client->GetMixFormat(&format)) &&
                SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, format, nullptr)) &&
                SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) &&
                SUCCEEDED(client->Start())) {
                active = true;
                channels = format->nChannels;
                bits = format->wBitsPerSample;
                floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
                if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
                    const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(*format);
                    floating = IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                }
            }
            if (format) CoTaskMemFree(format);
            if (!active) {
                if (comInitialized) CoUninitialize();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            while (!stop.load()) {
                UINT32 frames = 0;
                if (FAILED(capture->GetNextPacketSize(&frames)) || frames == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    continue;
                }
                BYTE* data = nullptr;
                DWORD flags = 0;
                UINT32 count = 0;
                if (FAILED(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr))) break;
                float maximum = 0.0f;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    const auto total = count * channels;
                    if (floating && bits == 32) {
                        const auto* samples = reinterpret_cast<const float*>(data);
                        for (UINT32 i = 0; i < total; ++i) maximum = (std::max)(maximum, std::fabs(samples[i]));
                    } else if (!floating && bits == 16) {
                        const auto* samples = reinterpret_cast<const std::int16_t*>(data);
                        for (UINT32 i = 0; i < total; ++i) {
                            maximum = (std::max)(maximum, std::fabs(static_cast<float>(samples[i]) / 32768.0f));
                        }
                    }
                }
                peak.store(std::clamp(maximum, 0.0f, 1.0f));
                capture->ReleaseBuffer(count);
            }
            client->Stop();
            if (comInitialized) CoUninitialize();
        }
    }
};

WindowsAudio::WindowsAudio() : state_(std::make_unique<State>()) {}
WindowsAudio::~WindowsAudio() = default;
float WindowsAudio::TakePeak() { return state_->peak.exchange(-1.0f); }
