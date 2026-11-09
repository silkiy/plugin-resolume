#include "BeatShaders.h"
#include "WindowsAudio.h"

#include <FFGLSDK.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>

namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using namespace std::chrono_literals;

enum Parameter : FFUInt32 { Input, Pattern, Grid, Rate, Amount, Sensitivity, Decay, AudioLevel, Hit, ParameterCount };
enum InputMode { WindowsOutput, WindowsInput, ResolumeFft, ResolumeBpm, Manual };
constexpr int strobePattern = 3;
constexpr float defaultAmount = 0.65f;
constexpr float defaultSensitivity = 0.65f;
constexpr float defaultDecay = 180.0f;
constexpr float minDecay = 80.0f;
constexpr float maxDecay = 600.0f;
constexpr auto minimumStrobeInterval = 350ms;
constexpr auto audioPacketHold = 80ms;
constexpr std::array inputNames{"Windows Output", "Windows Input", "Resolume FFT", "Resolume BPM", "Manual"};
constexpr std::array patternNames{"Slice", "Wave", "Shuffle", "Strobe"};

class OnsetDetector {
public:
    bool Update(float level, float sensitivity, Time now)
    {
        const float dt = lastSample_
            ? std::clamp(std::chrono::duration<float>(now - *lastSample_).count(), 0.0f, 0.25f)
            : 1.0f / 60.0f;
        lastSample_ = now;

        const float floor = 0.014f - 0.012f * sensitivity;
        const float threshold = floor + baseline_ * (2.10f - 0.65f * sensitivity);
        if (level <= std::max(floor * 0.75f, baseline_ * 1.10f)) {
            armed_ = true;
        }
        const bool hit = armed_ && level >= threshold && level - previous_ >= floor * 0.35f &&
            (!lastHit_ || now - *lastHit_ >= 140ms);
        if (hit) {
            lastHit_ = now;
            armed_ = false;
        }

        baseline_ += (level - baseline_) * (1.0f - std::exp(-dt / 0.27f));
        previous_ = level;
        return hit;
    }

private:
    std::optional<Time> lastSample_;
    std::optional<Time> lastHit_;
    float baseline_ = 0.0f;
    float previous_ = 0.0f;
    bool armed_ = true;
};

class BeatMosaic final : public CFFGLPlugin {
public:
    BeatMosaic()
    {
        SetMinInputs(1);
        SetMaxInputs(1);

        SetNamedOptions(Input, "Input", inputNames);
        SetNamedOptions(Pattern, "Pattern", patternNames);
        SetOptionParamInfo(Grid, "Grid", 2, 4.0f);
        SetParamElementInfo(Grid, 0, "4 x 4", 4.0f);
        SetParamElementInfo(Grid, 1, "8 x 8", 8.0f);
        SetOptionParamInfo(Rate, "Rate", 3, 1.0f);
        SetParamElementInfo(Rate, 0, "Every Beat", 1.0f);
        SetParamElementInfo(Rate, 1, "Every 2 Beats", 2.0f);
        SetParamElementInfo(Rate, 2, "Every 4 Beats", 4.0f);
        SetParamInfo(Amount, "Amount", FF_TYPE_STANDARD, defaultAmount);
        SetParamInfo(Sensitivity, "Sensitivity", FF_TYPE_STANDARD, defaultSensitivity);
        SetParamInfo(Decay, "Decay", FF_TYPE_INTEGER, defaultDecay);
        SetParamRange(Decay, minDecay, maxDecay);
        SetParamInfo(AudioLevel, "Audio Level", FF_TYPE_STANDARD, 0.0f);
        SetParamInfo(Hit, "Hit", FF_TYPE_EVENT, 0.0f);
    }

    FFResult SetFloatParameter(FFUInt32 index, float value) override
    {
        if (index >= ParameterCount || !std::isfinite(value)) {
            return FF_FAIL;
        }
        switch (index) {
        case Input:
        case Pattern: {
            const auto count = index == Input ? inputNames.size() : patternNames.size();
            if (value < 0.0f || value >= static_cast<float>(count) || value != std::floor(value)) {
                return FF_FAIL;
            }
            break;
        }
        case Grid:
            if (value != 4.0f && value != 8.0f) {
                return FF_FAIL;
            }
            break;
        case Rate:
            if (value != 1.0f && value != 2.0f && value != 4.0f) {
                return FF_FAIL;
            }
            break;
        case Decay:
            value = std::round(std::clamp(value, minDecay, maxDecay));
            break;
        case Hit:
            if (value > 0.0f) {
                hitRequested_.store(true);
            }
            return FF_SUCCESS;
        default:
            value = std::clamp(value, 0.0f, 1.0f);
            break;
        }
        values_[index].store(value);
        return FF_SUCCESS;
    }

    float GetFloatParameter(FFUInt32 index) override
    {
        return index < Hit ? values_[index].load() : 0.0f;
    }

    void SetBeatInfo(float tempo, float phase) override
    {
        const bool valid = std::isfinite(tempo) && tempo > 0.0f &&
            std::isfinite(phase) && phase >= 0.0f && phase <= 1.0f;
        hostPhase_.store(valid ? phase : -1.0f);
    }

    FFResult InitGL(const FFGLViewportStruct* viewport) override
    {
        if (!shader_.Compile(beatVertexShader, beatFragmentShader) || !quad_.Initialise()) {
            DeInitGL();
            return FF_FAIL;
        }
        textureLocation_ = shader_.FindUniform("inputTexture");
        uvLocation_ = shader_.FindUniform("inputMaxUV");
        texelLocation_ = shader_.FindUniform("inputHalfTexel");
        patternLocation_ = shader_.FindUniform("pattern");
        gridLocation_ = shader_.FindUniform("gridSize");
        stepLocation_ = shader_.FindUniform("stepIndex");
        amountLocation_ = shader_.FindUniform("amount");
        pulseLocation_ = shader_.FindUniform("pulse");
        return CFFGLPlugin::InitGL(viewport);
    }

    FFResult ProcessOpenGL(ProcessOpenGLStruct* frame) override
    {
        if (!frame || frame->numInputTextures == 0 || !frame->inputTextures || !frame->inputTextures[0]) {
            return FF_FAIL;
        }
        const auto& texture = *frame->inputTextures[0];
        if (!texture.Handle || !texture.Width || !texture.Height ||
            texture.Width > texture.HardwareWidth || texture.Height > texture.HardwareHeight) {
            return FF_FAIL;
        }

        const auto now = Clock::now();
        const auto mode = static_cast<InputMode>(static_cast<int>(values_[Input].load()));
        const int pattern = static_cast<int>(values_[Pattern].load());
        const int rate = static_cast<int>(values_[Rate].load());
        UpdateControls(mode, pattern, rate);
        const bool automaticHit = AutomaticHit(mode, rate, now);
        if (hitRequested_.exchange(false) || automaticHit) {
            Trigger(pattern, now);
        }

        const auto maxUv = GetMaxGLTexCoords(texture);
        ffglex::ScopedShaderBinding shaderBinding(shader_.GetGLID());
        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding textureBinding(texture.Handle);
        glUniform1i(textureLocation_, 0);
        glUniform2f(uvLocation_, maxUv.s, maxUv.t);
        glUniform2f(texelLocation_, 0.5f / static_cast<float>(texture.HardwareWidth),
            0.5f / static_cast<float>(texture.HardwareHeight));
        glUniform1i(patternLocation_, pattern);
        glUniform1i(gridLocation_, static_cast<int>(values_[Grid].load()));
        glUniform1i(stepLocation_, static_cast<int>(stepIndex_));
        glUniform1f(amountLocation_, values_[Amount].load());
        glUniform1f(pulseLocation_, Pulse(pattern, now));
        quad_.Draw();
        return FF_SUCCESS;
    }

    FFResult Disconnect() override
    {
        audio_.SetEndpoint(AudioEndpoint::None);
        activeMode_.reset();
        lastPulse_.reset();
        hitRequested_.store(false);
        hostPhase_.store(-1.0f);
        return FF_SUCCESS;
    }

    FFResult DeInitGL() override
    {
        shader_.FreeGLResources();
        quad_.Release();
        return Disconnect();
    }

private:
    template <std::size_t Count>
    void SetNamedOptions(Parameter parameter, const char* name, const std::array<const char*, Count>& labels)
    {
        SetOptionParamInfo(parameter, name, static_cast<FFUInt32>(Count), 0.0f);
        for (FFUInt32 index = 0; index < Count; ++index) {
            SetParamElementInfo(parameter, index, labels[index], static_cast<float>(index));
        }
    }

    void UpdateControls(InputMode mode, int pattern, int rate)
    {
        if (activeMode_ != mode) {
            const auto endpoint = mode == WindowsOutput ? AudioEndpoint::Output
                : mode == WindowsInput ? AudioEndpoint::Input : AudioEndpoint::None;
            audio_.SetEndpoint(endpoint);
            activeMode_ = mode;
            onset_ = {};
            audioPeak_ = 0.0f;
            lastAudioPacket_.reset();
            previousPhase_.reset();
            beatPosition_ = 0;
            lastPulse_.reset();
        }
        if (activePattern_ != pattern) {
            activePattern_ = pattern;
            lastPulse_.reset();
        }
        if (activeRate_ != rate) {
            activeRate_ = rate;
            previousPhase_.reset();
            beatPosition_ = 0;
        }
    }

    bool AutomaticHit(InputMode mode, int rate, Time now)
    {
        if (mode == Manual) {
            return false;
        }
        if (mode == ResolumeBpm) {
            const float phase = hostPhase_.load();
            if (phase < 0.0f) {
                previousPhase_.reset();
                return false;
            }
            const auto previous = previousPhase_;
            previousPhase_ = phase;
            if (!previous) {
                return false;
            }
            const float slots = 4.0f / static_cast<float>(rate);
            const auto slot = [slots](float p) { return std::floor(std::min(p, 0.999999f) * slots); };
            return *previous - phase > 0.5f || slot(phase) > slot(*previous);
        }

        float level = values_[AudioLevel].load();
        if (mode == WindowsOutput || mode == WindowsInput) {
            const float peak = audio_.TakePeak();
            if (peak >= 0.0f) {
                audioPeak_ = peak;
                lastAudioPacket_ = now;
            }
            level = lastAudioPacket_ && now - *lastAudioPacket_ <= audioPacketHold ? audioPeak_ : 0.0f;
        }
        if (!onset_.Update(level, values_[Sensitivity].load(), now)) {
            return false;
        }
        const bool selected = beatPosition_ == 0;
        beatPosition_ = (beatPosition_ + 1) % rate;
        return selected;
    }

    void Trigger(int pattern, Time now)
    {
        if (pattern == strobePattern) {
            if (lastStrobe_ && now - *lastStrobe_ < minimumStrobeInterval) {
                return;
            }
            lastStrobe_ = now;
        }
        lastPulse_ = now;
        stepIndex_ = (stepIndex_ + 1u) & 0x7fffu;
    }

    float Pulse(int pattern, Time now) const
    {
        if (!lastPulse_) {
            return 0.0f;
        }
        const float decay = values_[Decay].load();
        const float duration = pattern == strobePattern ? std::min(decay, 140.0f) : decay;
        const float elapsed = std::chrono::duration<float, std::milli>(now - *lastPulse_).count();
        const float remaining = std::max(0.0f, 1.0f - elapsed / duration);
        return remaining * remaining;
    }

    std::atomic<float> values_[Hit]{0.0f, 0.0f, 4.0f, 1.0f, defaultAmount, defaultSensitivity, defaultDecay, 0.0f};
    std::atomic<float> hostPhase_{-1.0f};
    std::atomic<bool> hitRequested_{false};
    WindowsAudio audio_;
    OnsetDetector onset_;
    std::optional<InputMode> activeMode_;
    std::optional<float> previousPhase_;
    std::optional<Time> lastAudioPacket_;
    std::optional<Time> lastPulse_;
    std::optional<Time> lastStrobe_;
    float audioPeak_ = 0.0f;
    int activePattern_ = 0;
    int activeRate_ = 1;
    int beatPosition_ = 0;
    unsigned stepIndex_ = 0;

    ffglex::FFGLShader shader_;
    ffglex::FFGLScreenQuad quad_;
    GLint textureLocation_ = -1;
    GLint uvLocation_ = -1;
    GLint texelLocation_ = -1;
    GLint patternLocation_ = -1;
    GLint gridLocation_ = -1;
    GLint stepLocation_ = -1;
    GLint amountLocation_ = -1;
    GLint pulseLocation_ = -1;
};

const CFFGLPluginInfo pluginInfo(
    PluginFactory<BeatMosaic>, "LDbm", "BeatMosaic", 2, 1, 1, 0, FF_EFFECT,
    "Audio-reactive tile and brightness accents for live visuals.", "BeatMosaic for Resolume");
}
