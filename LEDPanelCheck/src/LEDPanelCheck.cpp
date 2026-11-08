#include "PanelShaders.h"

#include <FFGLSDK.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace {
enum Parameter : FFUInt32 { Pattern, CabinetWidth, CabinetHeight, ParameterCount };

constexpr std::array patternNames{"Cabinet Map", "Checker 1px", "Rows 1px", "Columns 1px"};
constexpr float minimumSize = 16.0f;
constexpr float maximumSize = 4096.0f;
constexpr int defaultSize = 128;

class LEDPanelCheck final : public CFFGLPlugin {
public:
    LEDPanelCheck()
    {
        SetMinInputs(0);
        SetMaxInputs(0);

        SetOptionParamInfo(Pattern, "Pattern", static_cast<FFUInt32>(patternNames.size()), 0.0f);
        for (FFUInt32 index = 0; index < patternNames.size(); ++index) {
            SetParamElementInfo(Pattern, index, patternNames[index], static_cast<float>(index));
        }

        SetParamInfo(CabinetWidth, "Cabinet W", FF_TYPE_INTEGER, static_cast<float>(defaultSize));
        SetParamRange(CabinetWidth, minimumSize, maximumSize);
        SetParamInfo(CabinetHeight, "Cabinet H", FF_TYPE_INTEGER, static_cast<float>(defaultSize));
        SetParamRange(CabinetHeight, minimumSize, maximumSize);
    }

    FFResult SetFloatParameter(FFUInt32 index, float value) override
    {
        if (index >= ParameterCount || !std::isfinite(value)) {
            return FF_FAIL;
        }
        if (index == Pattern) {
            if (value < 0.0f || value >= static_cast<float>(patternNames.size()) || value != std::floor(value)) {
                return FF_FAIL;
            }
            values_[index].store(static_cast<int>(value));
        } else {
            const auto clamped = std::clamp(value, minimumSize, maximumSize);
            values_[index].store(static_cast<int>(std::lround(clamped)));
        }
        return FF_SUCCESS;
    }

    float GetFloatParameter(FFUInt32 index) override
    {
        return index < ParameterCount ? static_cast<float>(values_[index].load()) : 0.0f;
    }

    FFResult InitGL(const FFGLViewportStruct* viewport) override
    {
        if (!shader_.Compile(panelVertexShader, panelFragmentShader) || !quad_.Initialise()) {
            DeInitGL();
            return FF_FAIL;
        }

        originLocation_ = shader_.FindUniform("viewportOrigin");
        viewportLocation_ = shader_.FindUniform("viewportSize");
        cabinetLocation_ = shader_.FindUniform("cabinetSize");
        patternLocation_ = shader_.FindUniform("pattern");
        return CFFGLPlugin::InitGL(viewport);
    }

    FFResult ProcessOpenGL(ProcessOpenGLStruct*) override
    {
        // Measure the active render viewport so the grid and patterns use actual framebuffer pixels.
        std::array<GLint, 4> viewport{};
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        if (viewport[2] <= 0 || viewport[3] <= 0) {
            return FF_SUCCESS;
        }

        ffglex::ScopedShaderBinding shaderBinding(shader_.GetGLID());
        glUniform2i(originLocation_, viewport[0], viewport[1]);
        glUniform2i(viewportLocation_, viewport[2], viewport[3]);
        glUniform2i(cabinetLocation_, values_[CabinetWidth].load(), values_[CabinetHeight].load());
        glUniform1i(patternLocation_, values_[Pattern].load());
        quad_.Draw();
        return FF_SUCCESS;
    }

    FFResult DeInitGL() override
    {
        shader_.FreeGLResources();
        quad_.Release();
        return FF_SUCCESS;
    }

private:
    std::atomic<int> values_[ParameterCount]{0, defaultSize, defaultSize};
    ffglex::FFGLShader shader_;
    ffglex::FFGLScreenQuad quad_;
    GLint originLocation_ = -1;
    GLint viewportLocation_ = -1;
    GLint cabinetLocation_ = -1;
    GLint patternLocation_ = -1;
};

const CFFGLPluginInfo pluginInfo(
    PluginFactory<LEDPanelCheck>, "LDpc", "LEDPanelCheck", 2, 1, 1, 0, FF_SOURCE,
    "Cabinet map and one-pixel test patterns for LED walls.", "LEDPanelCheck for Resolume");
}
