#define NOMINMAX
#include <Windows.h>
#include <wingdi.h>

#include <FFGLSDK.h>

#include "WindowsAudio.h"
#include "PluginPaths.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;

enum Parameter : FFUInt32 {
    Mode,
    Trigger,
    Speed,
    Amount,
    FontSize,
    Hue,
    Reload,
    ParameterCount
};

constexpr std::array modeNames{"Typewriter", "Wave", "Bounce", "Scatter"};
constexpr char vertexShader[] = R"(#version 410 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texCoord;
out vec2 uv;
void main() {
    gl_Position = vec4(position, 1.0);
    uv = texCoord;
}
)";

constexpr char fragmentShader[] = R"(#version 410 core
uniform sampler2D typography;
uniform int mode;
uniform float progress;
uniform float amount;
uniform vec3 tint;
uniform vec2 textRange;
uniform vec2 canvasScale;
in vec2 uv;
out vec4 color;

vec2 remap(vec2 point) {
    vec2 centered = point - 0.5;
    if (mode == 1) {
        centered.y += sin(point.x * 18.0 + progress * 6.2831853) * 0.05 * amount;
    } else if (mode == 2) {
        float bounce = abs(sin(progress * 3.1415926)) * 0.12 * amount;
        centered.y -= bounce;
        centered *= 1.0 - bounce * 0.35;
    } else if (mode == 3) {
        float wave = sin(point.x * 25.0 + progress * 9.0) * 0.018 * amount;
        centered += vec2(wave, cos(point.x * 17.0 + progress * 7.0) * 0.03 * amount);
    }
    return centered + 0.5;
}

void main() {
    vec2 point = (uv - 0.5) * canvasScale + 0.5;
    vec2 source = remap(point);
    if (any(lessThan(source, vec2(0.0))) || any(greaterThan(source, vec2(1.0)))) {
        color = vec4(0.0);
        return;
    }
    vec4 glyph = texture(typography, vec2(source.x, 1.0 - source.y));
    if (mode == 0 && point.x > mix(textRange.x, textRange.y, clamp(progress, 0.0, 1.0))) glyph = vec4(0.0);
    color = vec4(glyph.rgb * tint, glyph.a);
}
)";

struct Bitmap {
    static constexpr int width = 2048;
    static constexpr int height = 512;
    HBITMAP handle = nullptr;
    HDC dc = nullptr;
    HGDIOBJ previous = nullptr;
    void* pixels = nullptr;

    bool Create()
    {
        Release();
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        handle = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!handle) return false;
        previous = SelectObject(dc, handle);
        return true;
    }

    void Release()
    {
        if (dc && previous) SelectObject(dc, previous);
        if (handle) DeleteObject(handle);
        if (dc) DeleteDC(dc);
        handle = nullptr;
        dc = nullptr;
        previous = nullptr;
        pixels = nullptr;
    }
    ~Bitmap() { Release(); }
};

std::string ReadFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream result;
    result << file.rdbuf();
    auto text = result.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::wstring ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return L"?";
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::array<float, 3> HsvToRgb(float hue)
{
    const float h = std::fmod(std::max(hue, 0.0f), 1.0f) * 6.0f;
    const int sector = static_cast<int>(h);
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    switch (sector) {
    case 0: return {1.0f, x, 0.0f};
    case 1: return {x, 1.0f, 0.0f};
    case 2: return {0.0f, 1.0f, x};
    case 3: return {0.0f, x, 1.0f};
    case 4: return {x, 0.0f, 1.0f};
    default: return {1.0f, 0.0f, x};
    }
}

class KineticTypography final : public CFFGLPlugin {
public:
    KineticTypography()
    {
        SetMinInputs(0);
        SetMaxInputs(0);
        SetOptionParamInfo(Mode, "Mode", static_cast<FFUInt32>(modeNames.size()), 0.0f);
        for (FFUInt32 index = 0; index < modeNames.size(); ++index) {
            SetParamElementInfo(Mode, index, modeNames[index], static_cast<float>(index));
        }
        SetParamInfof(Trigger, "Trigger", FF_TYPE_EVENT);
        SetParamInfof(Reload, "Reload", FF_TYPE_EVENT);
        SetParamInfo(Speed, "Speed", FF_TYPE_STANDARD, 0.35f);
        SetParamRange(Speed, 0.05f, 2.0f);
        SetParamInfo(Amount, "Amount", FF_TYPE_STANDARD, 0.7f);
        SetParamRange(Amount, 0.0f, 1.0f);
        SetParamInfo(FontSize, "Font Size", FF_TYPE_INTEGER, 110.0f);
        SetParamRange(FontSize, 24.0f, 220.0f);
        SetParamInfo(Hue, "Hue", FF_TYPE_STANDARD, 0.55f);
        SetParamRange(Hue, 0.0f, 1.0f);
        LoadText();
    }

    FFResult SetFloatParameter(FFUInt32 index, float value) override
    {
        if (index >= ParameterCount || !std::isfinite(value)) return FF_FAIL;
        switch (index) {
        case Mode:
            if (value < 0.0f || value >= static_cast<float>(modeNames.size()) ||
                value != std::floor(value)) return FF_FAIL;
            mode_.store(static_cast<int>(value));
            break;
        case Trigger:
            if (value > 0.0f) TriggerAnimation();
            break;
        case Reload:
            if (value > 0.0f) LoadText();
            break;
        case Speed: speed_.store(std::clamp(value, 0.05f, 2.0f)); break;
        case Amount: amount_.store(std::clamp(value, 0.0f, 1.0f)); break;
        case FontSize:
            fontSize_.store(static_cast<int>(std::lround(std::clamp(value, 24.0f, 220.0f))));
            textureDirty_.store(true);
            break;
        case Hue: hue_.store(std::clamp(value, 0.0f, 1.0f)); break;
        default: return FF_FAIL;
        }
        return FF_SUCCESS;
    }

    float GetFloatParameter(FFUInt32 index) override
    {
        switch (index) {
        case Mode: return static_cast<float>(mode_.load());
        case Speed: return speed_.load();
        case Amount: return amount_.load();
        case FontSize: return static_cast<float>(fontSize_.load());
        case Hue: return hue_.load();
        default: return 0.0f;
        }
    }

    FFResult InitGL(const FFGLViewportStruct* viewport) override
    {
        if (!shader_.Compile(vertexShader, fragmentShader) || !quad_.Initialise() ||
            !bitmap_.Create()) {
            DeInitGL();
            return FF_FAIL;
        }
        glGenTextures(1, &texture_);
        if (!texture_) {
            DeInitGL();
            return FF_FAIL;
        }
        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding binding(texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Bitmap::width, Bitmap::height, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, bitmap_.pixels);
        modeLocation_ = shader_.FindUniform("mode");
        progressLocation_ = shader_.FindUniform("progress");
        amountLocation_ = shader_.FindUniform("amount");
        tintLocation_ = shader_.FindUniform("tint");
        rangeLocation_ = shader_.FindUniform("textRange");
        scaleLocation_ = shader_.FindUniform("canvasScale");
        textureDirty_.store(true);
        return CFFGLPlugin::InitGL(viewport);
    }

    FFResult ProcessOpenGL(ProcessOpenGLStruct*) override
    {
        const auto peak = audio_.TakePeak();
        const auto now = Clock::now();
        const float elapsedSeconds = std::chrono::duration<float>(now - animationStart_).count();
        // Let a typewriter reveal finish and hold before an audio accent restarts it.
        const float cooldown = mode_.load() == 0 ? 1.0f / speed_.load() + 1.0f : 0.2f;
        if (peak >= 0.18f && peak > lastPeak_ * 1.18f && elapsedSeconds >= cooldown) TriggerAnimation();
        lastPeak_ = peak >= 0.0f ? peak : lastPeak_ * 0.96f;
        if (textureDirty_.exchange(false)) UpdateTexture();
        if (triggerRequested_.exchange(false)) animationStart_ = now;

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - animationStart_).count();
        const float phase = static_cast<float>(elapsed) / 1000.0f * speed_.load();
        const float progress = mode_.load() == 0 ? std::clamp(phase, 0.0f, 1.0f) : std::fmod(phase, 1.0f);
        const auto tint = HsvToRgb(hue_.load());
        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding binding(texture_);
        ffglex::ScopedShaderBinding shaderBinding(shader_.GetGLID());
        glUniform1i(modeLocation_, mode_.load());
        glUniform1f(progressLocation_, progress);
        glUniform1f(amountLocation_, amount_.load());
        glUniform3fv(tintLocation_, 1, tint.data());
        glUniform2f(rangeLocation_, textLeft_, textRight_);
        const float aspect = currentViewport.height ? static_cast<float>(currentViewport.width) / currentViewport.height : 1.0f;
        constexpr float canvasAspect = static_cast<float>(Bitmap::width) / Bitmap::height;
        glUniform2f(scaleLocation_, std::max(1.0f, aspect / canvasAspect), std::max(1.0f, canvasAspect / aspect));
        quad_.Draw();
        return FF_SUCCESS;
    }

    FFResult DeInitGL() override
    {
        if (texture_) glDeleteTextures(1, &texture_);
        texture_ = 0;
        shader_.FreeGLResources();
        quad_.Release();
        bitmap_.Release();
        return FF_SUCCESS;
    }

private:
    void TriggerAnimation() { triggerRequested_.store(true); }

    void LoadText()
    {
        auto text = ReadFile(pluginfiles::ConfigFile(L"KineticTypography", L"text.txt").wstring());
        if (text.empty()) text = "Kinetic Typography";
        {
            const std::lock_guard lock(textMutex_);
            text_ = std::move(text);
        }
        textureDirty_.store(true);
        TriggerAnimation();
    }

    void UpdateTexture()
    {
        if (!bitmap_.dc || !bitmap_.pixels) return;
        RECT area{0, 0, Bitmap::width, Bitmap::height};
        HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(bitmap_.dc, &area, clear);
        DeleteObject(clear);
        GdiFlush();
        auto* pixels = static_cast<unsigned char*>(bitmap_.pixels);
        for (int index = 3; index < Bitmap::width * Bitmap::height * 4; index += 4) {
            pixels[index] = 0;
        }
        const auto font = CreateFontW(fontSize_.load(), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      ANTIALIASED_QUALITY, FF_DONTCARE, L"Arial");
        const auto previous = SelectObject(bitmap_.dc, font);
        SetBkMode(bitmap_.dc, TRANSPARENT);
        SetTextColor(bitmap_.dc, RGB(255, 255, 255));
        std::string text;
        {
            const std::lock_guard lock(textMutex_);
            text = text_;
        }
        const auto wide = ToWide(text);
        RECT bounds{64, 64, Bitmap::width - 64, Bitmap::height - 64};
        RECT measured{64, 0, Bitmap::width - 64, 0};
        DrawTextW(bitmap_.dc, wide.c_str(), static_cast<int>(wide.size()), &measured,
                  DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        bounds.top = std::max(0L, (Bitmap::height - measured.bottom) / 2);
        bounds.bottom = Bitmap::height;
        DrawTextW(bitmap_.dc, wide.c_str(), static_cast<int>(wide.size()), &bounds,
                  DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        GdiFlush();
        int left = Bitmap::width, right = 0;
        for (int index = 0; index < Bitmap::width * Bitmap::height; ++index) {
            const auto offset = index * 4;
            const auto coverage = std::max({pixels[offset], pixels[offset + 1], pixels[offset + 2]});
            pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = pixels[offset + 3] = coverage;
            if (coverage) {
                left = std::min(left, index % Bitmap::width);
                right = std::max(right, index % Bitmap::width + 1);
            }
        }
        textLeft_ = static_cast<float>(left) / Bitmap::width;
        textRight_ = static_cast<float>(right) / Bitmap::width;
        SelectObject(bitmap_.dc, previous);
        DeleteObject(font);
        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding binding(texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Bitmap::width, Bitmap::height,
                        GL_BGRA, GL_UNSIGNED_BYTE, bitmap_.pixels);
    }

    std::mutex textMutex_;
    std::string text_;
    std::atomic<int> mode_{0};
    std::atomic<float> speed_{0.35f};
    std::atomic<float> amount_{0.7f};
    std::atomic<int> fontSize_{110};
    std::atomic<float> hue_{0.55f};
    std::atomic<bool> textureDirty_{true};
    std::atomic<bool> triggerRequested_{true};
    Clock::time_point animationStart_ = Clock::now();
    float lastPeak_ = 0.0f;
    WindowsAudio audio_;
    Bitmap bitmap_;
    GLuint texture_ = 0;
    ffglex::FFGLShader shader_;
    ffglex::FFGLScreenQuad quad_;
    GLint modeLocation_ = -1;
    GLint progressLocation_ = -1;
    GLint amountLocation_ = -1;
    GLint tintLocation_ = -1;
    GLint rangeLocation_ = -1;
    GLint scaleLocation_ = -1;
    float textLeft_ = 0.0f, textRight_ = 1.0f;
};

const CFFGLPluginInfo pluginInfo(
    PluginFactory<KineticTypography>, "LDkt", "KineticTypography", 2, 1, 1, 0, FF_SOURCE,
    "Audio-reactive kinetic typography generator.", "KineticTypography for Resolume");
}
