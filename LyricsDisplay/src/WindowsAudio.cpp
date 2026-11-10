#include "WindowsAudio.h"

#define NOMINMAX
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

using Microsoft::WRL::ComPtr;

struct WindowsAudio::State {
    std::atomic<AudioEndpoint> endpoint{AudioEndpoint::None};
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
            if (endpoint.load() != AudioEndpoint::Output) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioCaptureClient> capture;
            WAVEFORMATEX* format = nullptr;
            UINT32 channels = 0;
            WORD bitsPerSample = 0;
            bool floatSamples = false;
            bool active = false;
            bool comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
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
                bitsPerSample = format->wBitsPerSample;
                floatSamples = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
                if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
                    const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(*format);
                    floatSamples = IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                }
            }
            if (format) CoTaskMemFree(format);
            if (!active) {
                if (comInitialized) CoUninitialize();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            while (active && !stop.load() && endpoint.load() == AudioEndpoint::Output) {
                UINT32 frames = 0;
                if (FAILED(capture->GetNextPacketSize(&frames)) || frames == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                BYTE* data = nullptr;
                DWORD flags = 0;
                UINT32 count = 0;
                if (FAILED(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr))) break;
                float maximum = 0.0f;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    const auto sampleCount = count * channels;
                    if (floatSamples && bitsPerSample == 32) {
                        const auto* samples = reinterpret_cast<const float*>(data);
                        for (UINT32 i = 0; i < sampleCount; ++i) {
                            maximum = (std::max)(maximum, std::fabs(samples[i]));
                        }
                    } else if (bitsPerSample == 16) {
                        const auto* samples = reinterpret_cast<const std::int16_t*>(data);
                        for (UINT32 i = 0; i < sampleCount; ++i) {
                            maximum = (std::max)(maximum,
                                std::fabs(static_cast<float>(samples[i]) / 32768.0f));
                        }
                    } else if (!floatSamples && bitsPerSample == 32) {
                        const auto* samples = reinterpret_cast<const std::int32_t*>(data);
                        for (UINT32 i = 0; i < sampleCount; ++i) {
                            maximum = (std::max)(maximum,
                                std::fabs(static_cast<float>(samples[i]) / 2147483648.0f));
                        }
                    } else if (!floatSamples && bitsPerSample == 24) {
                        for (UINT32 i = 0; i < sampleCount; ++i) {
                            const auto* sample = data + (i * 3);
                            std::int32_t value = sample[0] | (sample[1] << 8) | (sample[2] << 16);
                            if (value & 0x00800000) value -= 0x01000000;
                            maximum = (std::max)(maximum,
                                std::fabs(static_cast<float>(value) / 8388608.0f));
                        }
                    }
                }
                peak.store(maximum);
                capture->ReleaseBuffer(count);
            }
            if (active) client->Stop();
            if (comInitialized) CoUninitialize();
        }
    }
};

WindowsAudio::WindowsAudio() : state_(std::make_unique<State>()) {}
WindowsAudio::~WindowsAudio() = default;
void WindowsAudio::SetEndpoint(AudioEndpoint endpoint) { state_->endpoint.store(endpoint); }
float WindowsAudio::TakePeak() { return state_->peak.exchange(-1.0f); }
