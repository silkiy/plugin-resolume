#include "TimerShaders.h"

#include <FFGLSDK.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>

namespace {
using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::seconds;

enum Parameter : FFUInt32 { Minutes, ExtraSeconds, Warning, Start, Pause, Reset };

struct Setting {
    const char* name;
    int maximum;
    int initial;
};

constexpr std::array settings{
    Setting{"Minutes", 99, 5},
    Setting{"Seconds", 59, 0},
    Setting{"Warning", 59, 10},
};

constexpr std::array normalColor{0.96f, 0.98f, 1.0f};
constexpr std::array warningColor{1.0f, 0.67f, 0.18f};
constexpr std::array finishedColor{1.0f, 0.31f, 0.33f};

class CueTimer final : public CFFGLPlugin {
public:
    CueTimer()
    {
        SetMinInputs(0);
        SetMaxInputs(0);
        for (FFUInt32 index = 0; index < settings.size(); ++index) {
            const auto& setting = settings[index];
            values_[index] = setting.initial;
            SetParamInfo(index, setting.name, FF_TYPE_INTEGER, static_cast<float>(setting.initial));
            SetParamRange(index, 0.0f, static_cast<float>(setting.maximum));
        }
        SetParamInfo(Start, "Start", FF_TYPE_EVENT, 0.0f);
        SetParamInfo(Pause, "Pause", FF_TYPE_EVENT, 0.0f);
        SetParamInfo(Reset, "Reset", FF_TYPE_EVENT, 0.0f);
        ResetTimer();
    }

    FFResult SetFloatParameter(FFUInt32 index, float value) override
    {
        if (index > Reset || !std::isfinite(value)) {
            return FF_FAIL;
        }

        const std::lock_guard lock(timerMutex_);
        if (index < settings.size()) {
            const auto maximum = static_cast<float>(settings[index].maximum);
            const auto whole = static_cast<int>(std::lround(std::clamp(value, 0.0f, maximum)));
            if (values_[index] != whole) {
                values_[index] = whole;
                if (index != Warning) {
                    ResetTimer();
                }
            }
        } else if (value > 0.0f) {
            switch (index) {
            case Start: StartTimer(Clock::now()); break;
            case Pause: PauseTimer(Clock::now()); break;
            case Reset: ResetTimer(); break;
            }
        }
        return FF_SUCCESS;
    }

    float GetFloatParameter(FFUInt32 index) override
    {
        const std::lock_guard lock(timerMutex_);
        return index < settings.size() ? static_cast<float>(values_[index]) : 0.0f;
    }

    FFResult InitGL(const FFGLViewportStruct* viewport) override
    {
        if (!shader_.Compile(timerVertexShader, timerFragmentShader) || !quad_.Initialise()) {
            DeInitGL();
            return FF_FAIL;
        }
        digitsLocation_ = shader_.FindUniform("digits");
        aspectLocation_ = shader_.FindUniform("viewportAspect");
        colorLocation_ = shader_.FindUniform("digitColor");
        return CFFGLPlugin::InitGL(viewport);
    }

    FFResult ProcessOpenGL(ProcessOpenGLStruct*) override
    {
        int secondsLeft;
        int warning;
        {
            const std::lock_guard lock(timerMutex_);
            secondsLeft = static_cast<int>(std::chrono::ceil<Seconds>(Remaining(Clock::now())).count());
            warning = values_[Warning];
        }

        const int minutes = secondsLeft / 60;
        const int seconds = secondsLeft % 60;
        const float aspect = currentViewport.width > 0 && currentViewport.height > 0
            ? static_cast<float>(currentViewport.width) / static_cast<float>(currentViewport.height)
            : 1.0f;
        const auto& color = secondsLeft == 0 ? finishedColor
            : secondsLeft <= warning ? warningColor : normalColor;

        ffglex::ScopedShaderBinding shaderBinding(shader_.GetGLID());
        glUniform4i(digitsLocation_, minutes / 10, minutes % 10, seconds / 10, seconds % 10);
        glUniform1f(aspectLocation_, aspect);
        glUniform3fv(colorLocation_, 1, color.data());
        quad_.Draw();
        return FF_SUCCESS;
    }

    FFResult Disconnect() override
    {
        const std::lock_guard lock(timerMutex_);
        PauseTimer(Clock::now());
        return FF_SUCCESS;
    }

    FFResult DeInitGL() override
    {
        shader_.FreeGLResources();
        quad_.Release();
        return Disconnect();
    }

private:
    Seconds Preset() const { return Seconds(values_[Minutes] * 60 + values_[ExtraSeconds]); }

    Clock::duration Remaining(Clock::time_point now) const
    {
        return deadline_ ? std::max(Clock::duration::zero(), *deadline_ - now) : pausedTime_;
    }

    void StartTimer(Clock::time_point now)
    {
        auto remaining = Remaining(now);
        if (remaining == Clock::duration::zero()) {
            remaining = Preset();
        }
        if (remaining > Clock::duration::zero()) {
            deadline_ = now + remaining;
        }
    }

    void PauseTimer(Clock::time_point now)
    {
        pausedTime_ = Remaining(now);
        deadline_.reset();
    }

    void ResetTimer()
    {
        pausedTime_ = Preset();
        deadline_.reset();
    }

    std::mutex timerMutex_;
    std::array<int, settings.size()> values_{};
    Clock::duration pausedTime_{};
    std::optional<Clock::time_point> deadline_;
    ffglex::FFGLShader shader_;
    ffglex::FFGLScreenQuad quad_;
    GLint digitsLocation_ = -1;
    GLint aspectLocation_ = -1;
    GLint colorLocation_ = -1;
};

const CFFGLPluginInfo pluginInfo(
    PluginFactory<CueTimer>, "LDCt", "CueTimer", 2, 1, 1, 0, FF_SOURCE,
    "An adjustable countdown overlay for live events.", "CueTimer for Resolume");
}
