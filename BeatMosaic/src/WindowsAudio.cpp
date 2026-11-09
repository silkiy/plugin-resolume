#include "WindowsAudio.h"

#define NOMINMAX
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

constexpr float noMeasurement = -1.0f;
constexpr unsigned maxChannels = 64;
constexpr auto pollInterval = 8ms;
constexpr auto defaultDeviceInterval = 750ms;
constexpr auto retryInterval = 500ms;

struct CoTaskMemory {
    void operator()(void* address) const noexcept { CoTaskMemFree(address); }
};

using EndpointId = std::unique_ptr<wchar_t, CoTaskMemory>;

EndpointId ReadId(IMMDevice& device)
{
    wchar_t* id = nullptr;
    return SUCCEEDED(device.GetId(&id)) ? EndpointId(id) : nullptr;
}

EDataFlow DataFlow(AudioEndpoint endpoint)
{
    return endpoint == AudioEndpoint::Output ? eRender : eCapture;
}

enum class SampleKind { Pcm8, Pcm16, Pcm24, Pcm32, Float32, Float64 };

std::optional<SampleKind> SampleType(const WAVEFORMATEX& format)
{
    auto tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return std::nullopt;
        }
        const auto& subtype = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat;
        if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        } else if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        }
    }

    if (tag == WAVE_FORMAT_PCM) {
        switch (format.wBitsPerSample) {
        case 8: return SampleKind::Pcm8;
        case 16: return SampleKind::Pcm16;
        case 24: return SampleKind::Pcm24;
        case 32: return SampleKind::Pcm32;
        }
    } else if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        switch (format.wBitsPerSample) {
        case 32: return SampleKind::Float32;
        case 64: return SampleKind::Float64;
        }
    }
    return std::nullopt;
}

template<typename T>
T ReadUnaligned(const BYTE* bytes)
{
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

float ReadSample(const BYTE* bytes, SampleKind kind)
{
    switch (kind) {
    case SampleKind::Pcm8:
        return (static_cast<float>(bytes[0]) - 128.0f) / 128.0f;
    case SampleKind::Pcm16:
        return static_cast<float>(ReadUnaligned<std::int16_t>(bytes)) / 32768.0f;
    case SampleKind::Pcm24: {
        int sample = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
        if (sample & 0x800000) {
            sample -= 0x1000000;
        }
        return static_cast<float>(sample) / 8388608.0f;
    }
    case SampleKind::Pcm32:
        return static_cast<float>(ReadUnaligned<std::int32_t>(bytes)) / 2147483648.0f;
    case SampleKind::Float32: {
        const auto sample = ReadUnaligned<float>(bytes);
        return std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
    }
    case SampleKind::Float64: {
        const auto sample = ReadUnaligned<double>(bytes);
        return std::isfinite(sample) ? static_cast<float>(std::clamp(sample, -1.0, 1.0)) : 0.0f;
    }
    }
    return 0.0f;
}

class BassMeter {
public:
    BassMeter(const WAVEFORMATEX& format, SampleKind kind)
        : kind_(kind), channels_(format.nChannels), sampleBytes_(format.wBitsPerSample / 8),
          frameBytes_(format.nBlockAlign), windowFrames_(std::max(1u, static_cast<unsigned>(format.nSamplesPerSec / 100))),
          lowPass_(Coefficient(180.0f, format.nSamplesPerSec)), highPass_(Coefficient(35.0f, format.nSamplesPerSec)) {}

    void Clear()
    {
        filters_.fill({});
        frames_ = 0;
    }

    float Feed(const BYTE* data, UINT32 count)
    {
        float peak = noMeasurement;
        for (UINT32 frame = 0; frame < count; ++frame, data += frameBytes_) {
            for (unsigned channel = 0; channel < channels_; ++channel) {
                const float sample = ReadSample(data + channel * sampleBytes_, kind_);
                auto& filter = filters_[channel];
                filter.low += lowPass_ * (sample - filter.low);
                filter.dc += highPass_ * (filter.low - filter.dc);
                const float bass = filter.low - filter.dc;
                filter.squareSum += bass * bass;
            }
            if (++frames_ == windowFrames_) {
                peak = std::max(peak, FinishWindow());
            }
        }
        return peak;
    }

private:
    struct Channel {
        float low = 0.0f;
        float dc = 0.0f;
        float squareSum = 0.0f;
    };

    static float Coefficient(float frequency, DWORD sampleRate)
    {
        constexpr float twoPi = 6.283185307f;
        return 1.0f - std::exp(-twoPi * frequency / static_cast<float>(sampleRate));
    }

    float FinishWindow()
    {
        float loudest = 0.0f;
        // Meter channels separately so stereo signals cannot cancel each other.
        for (unsigned channel = 0; channel < channels_; ++channel) {
            loudest = std::max(loudest, filters_[channel].squareSum);
            filters_[channel].squareSum = 0.0f;
        }
        frames_ = 0;
        return std::min(1.0f, std::sqrt(loudest / static_cast<float>(windowFrames_)));
    }

    SampleKind kind_;
    unsigned channels_;
    unsigned sampleBytes_;
    unsigned frameBytes_;
    unsigned windowFrames_;
    float lowPass_;
    float highPass_;
    std::array<Channel, maxChannels> filters_{};
    unsigned frames_ = 0;
};

class CaptureStream {
public:
    ~CaptureStream()
    {
        if (started_) {
            client_->Stop();
        }
    }

    bool Start(IMMDeviceEnumerator& enumerator, AudioEndpoint endpoint)
    {
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator.GetDefaultAudioEndpoint(DataFlow(endpoint), eMultimedia, device.GetAddressOf())) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client_.GetAddressOf())))) {
            return false;
        }
        id_ = ReadId(*device.Get());
        if (!id_) {
            return false;
        }

        WAVEFORMATEX* rawFormat = nullptr;
        if (FAILED(client_->GetMixFormat(&rawFormat))) {
            return false;
        }
        const std::unique_ptr<WAVEFORMATEX, CoTaskMemory> format(rawFormat);
        const auto kind = SampleType(*format);
        if (!kind || format->nChannels == 0 || format->nChannels > maxChannels ||
            format->nSamplesPerSec == 0 || format->nBlockAlign < format->nChannels * (format->wBitsPerSample / 8)) {
            return false;
        }

        constexpr REFERENCE_TIME bufferDuration = 1000000; // 100 ms in 100 ns units.
        const DWORD flags = endpoint == AudioEndpoint::Output ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, format.get(), nullptr)) ||
            FAILED(client_->GetService(IID_PPV_ARGS(capture_.GetAddressOf())))) {
            return false;
        }

        meter_.emplace(*format, *kind);
        started_ = SUCCEEDED(client_->Start());
        return started_;
    }

    bool StillDefault(IMMDeviceEnumerator& enumerator, AudioEndpoint endpoint) const
    {
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator.GetDefaultAudioEndpoint(DataFlow(endpoint), eMultimedia, device.GetAddressOf()))) {
            return false;
        }
        const auto id = ReadId(*device.Get());
        return id && std::wcscmp(id.get(), id_.get()) == 0;
    }

    bool Read(float& peak)
    {
        // Bound each drain so endpoint changes and shutdown can interrupt a busy stream.
        for (int packet = 0; packet < 32; ++packet) {
            UINT32 frames = 0;
            if (FAILED(capture_->GetNextPacketSize(&frames))) {
                return false;
            }
            if (frames == 0) {
                return true;
            }

            BYTE* data = nullptr;
            DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                return false;
            }
            if (frames == 0) {
                return true;
            }
            if ((flags & (AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) != 0) {
                meter_->Clear();
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                peak = std::max(peak, 0.0f);
            } else if (data) {
                peak = std::max(peak, meter_->Feed(data, frames));
            }
            if (FAILED(capture_->ReleaseBuffer(frames))) {
                return false;
            }
        }
        return true;
    }

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    EndpointId id_;
    std::optional<BassMeter> meter_;
    bool started_ = false;
};
}

struct WindowsAudio::State {
    ~State()
    {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void SetEndpoint(AudioEndpoint endpoint)
    {
        {
            const std::lock_guard lock(mutex);
            if (request.endpoint == endpoint) {
                return;
            }
            if (!worker.joinable() && endpoint != AudioEndpoint::None) {
                try {
                    worker = std::thread([this] { Run(); });
                } catch (const std::system_error&) {
                    return;
                }
            }
            request.endpoint = endpoint;
            ++request.generation;
            peak.store(noMeasurement);
        }
        changed.notify_one();
    }

    float TakePeak() { return peak.exchange(noMeasurement); }

private:
    struct Request {
        AudioEndpoint endpoint = AudioEndpoint::None;
        std::uint64_t generation = 0;
    };

    std::optional<Request> AwaitRequest()
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return stopping || request.endpoint != AudioEndpoint::None; });
        return stopping ? std::nullopt : std::optional(request);
    }

    bool WaitWhileCurrent(const Request& active, std::chrono::milliseconds delay)
    {
        std::unique_lock lock(mutex);
        return !changed.wait_for(lock, delay, [this, &active] {
            return stopping || request.generation != active.generation;
        });
    }

    void Publish(const Request& active, float measured)
    {
        const std::lock_guard lock(mutex);
        if (request.generation != active.generation) {
            return;
        }
        if (measured == noMeasurement) {
            peak.store(noMeasurement);
        } else {
            float previous = peak.load();
            while (measured > previous && !peak.compare_exchange_weak(previous, measured)) {}
        }
    }

    void Capture(const Request& active)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(enumerator.GetAddressOf())))) {
            return;
        }

        CaptureStream stream;
        if (!stream.Start(*enumerator.Get(), active.endpoint)) {
            return;
        }
        auto nextDeviceCheck = std::chrono::steady_clock::now() + defaultDeviceInterval;
        while (WaitWhileCurrent(active, pollInterval)) {
            float measured = noMeasurement;
            if (!stream.Read(measured)) {
                return;
            }
            if (measured >= 0.0f) {
                Publish(active, measured);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextDeviceCheck) {
                if (!stream.StillDefault(*enumerator.Get(), active.endpoint)) {
                    return;
                }
                nextDeviceCheck = now + defaultDeviceInterval;
            }
        }
    }

    void Run()
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            return;
        }
        while (const auto active = AwaitRequest()) {
            Capture(*active);
            Publish(*active, noMeasurement);
            WaitWhileCurrent(*active, retryInterval);
        }
        CoUninitialize();
    }

    std::mutex mutex;
    std::condition_variable changed;
    Request request;
    bool stopping = false;
    std::atomic<float> peak{noMeasurement};
    std::thread worker;
};

WindowsAudio::WindowsAudio() : state_(std::make_unique<State>()) {}
WindowsAudio::~WindowsAudio() = default;

void WindowsAudio::SetEndpoint(AudioEndpoint endpoint) { state_->SetEndpoint(endpoint); }
float WindowsAudio::TakePeak() { return state_->TakePeak(); }
