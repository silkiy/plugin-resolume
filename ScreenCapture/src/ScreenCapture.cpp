#include "DesktopCapture.h"
#include "WindowCapture.h"
#include <FFGLSDK.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <random>
#include <string_view>
#include <unordered_map>

namespace {
constexpr FFUInt32 monitorParameter = 0;
constexpr FFUInt32 sourceParameter = 1;
constexpr FFUInt32 windowParameter = 2;
constexpr FFUInt32 refreshParameter = 3;
constexpr unsigned maxDisplayNumber = 65535;
constexpr unsigned maxWindowValue = (1u << 24) - 1;
constexpr std::wstring_view displayPrefix = LR"(\\.\DISPLAY)";
constexpr auto captureInterval = std::chrono::microseconds(33333);

enum class Source { Monitor, Window };

struct WindowChoice {
    CaptureWindow target{};
    unsigned value = 0;
};

unsigned NextWindowValue()
{
    // Option values fit exactly in a float and aren't reused on a list refresh.
    // Use a different initial range each run so the values don't act as saved list positions.
    static std::atomic<unsigned> counter{std::random_device{}() & maxWindowValue};
    unsigned value = counter.fetch_add(1) & maxWindowValue;
    if (value == 0) {
        value = counter.fetch_add(1) & maxWindowValue;
    }
    return value;
}

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
uniform sampler2D desktop;
uniform vec2 fit;
uniform bool hasFrame;
in vec2 uv;
out vec4 color;
void main() {
    vec2 captureUV = (uv - 0.5) / fit + 0.5;
    if (!hasFrame || any(lessThan(captureUV, vec2(0.0))) || any(greaterThan(captureUV, vec2(1.0)))) {
        color = vec4(0.0);
    } else {
        color = vec4(texture(desktop, captureUV).rgb, 1.0);
    }
}
)";

// Give Resolume a static browser thumbnail without asking it to render the desktop.
constexpr FFUInt32 thumbnailWidth = 48;
constexpr FFUInt32 thumbnailHeight = 30;
const CFFGLThumbnailInfo thumbnail(thumbnailWidth, thumbnailHeight, [] {
    std::vector<CFFGLColor> pixels(thumbnailWidth * thumbnailHeight, CFFGLColor(0x182435FF));
    for (FFUInt32 y = 5; y < 25; ++y) {
        for (FFUInt32 x = 8; x < 40; ++x) {
            if ((y < 21 && (x < 10 || x >= 38 || y < 7 || y >= 19)) ||
                (y >= 21 && x >= 22 && x < 26) || (y >= 24 && x >= 17 && x < 31)) {
                pixels[y * thumbnailWidth + x] = CFFGLColor(0x67DCF9FF);
            }
        }
    }
    return pixels;
}());

class PixelUnpack {
public:
    PixelUnpack()
    {
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer_);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        for (std::size_t i = 0; i < std::size(settings_); ++i) {
            glGetIntegerv(settings_[i], &saved_[i]);
            glPixelStorei(settings_[i], i == 0 ? 4 : 0);
        }
    }

    ~PixelUnpack()
    {
        for (std::size_t i = 0; i < std::size(settings_); ++i) {
            glPixelStorei(settings_[i], saved_[i]);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(buffer_));
    }

private:
    static constexpr GLenum settings_[] = {
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS
    };
    GLint saved_[std::size(settings_)]{};
    GLint buffer_ = 0;
};

class ScreenCapture final : public CFFGLPlugin {
public:
    ScreenCapture()
    {
        SetMinInputs(0);
        SetMaxInputs(0);

        // Keep each option value tied to its Windows display name across restarts.
        std::vector<unsigned> numbers;
        for (const auto& display : ListDisplays()) {
            if (display.deviceName.compare(0, displayPrefix.size(), displayPrefix) == 0) {
                const wchar_t* digits = display.deviceName.c_str() + displayPrefix.size();
                wchar_t* end = nullptr;
                const auto number = std::wcstoul(digits, &end, 10);
                if (*end == L'\0' && number > 0 && number <= maxDisplayNumber) {
                    numbers.push_back(number);
                }
            }
        }
        std::sort(numbers.begin(), numbers.end());
        SetOptionParamInfo(monitorParameter, "Monitor", static_cast<FFUInt32>(numbers.size()) + 1, 0.0f);
        SetParamElementInfo(monitorParameter, 0, "Primary", 0.0f);
        FFUInt32 option = 1;
        for (const auto number : numbers) {
            const auto label = "Display " + std::to_string(number);
            SetParamElementInfo(monitorParameter, option++, label.c_str(), static_cast<float>(number));
        }

        SetOptionParamInfo(sourceParameter, "Source", 2, 0.0f);
        SetParamElementInfo(sourceParameter, 0, "Monitor", 0.0f);
        SetParamElementInfo(sourceParameter, 1, "Window", 1.0f);
        SetOptionParamInfo(windowParameter, "Window", 1, 0.0f);
        SetParamElementInfo(windowParameter, 0, "None", 0.0f);
        SetParamInfof(refreshParameter, "Refresh Windows", FF_TYPE_EVENT);
        SetSourceVisibility(Source::Monitor, false);
    }

    FFResult SetFloatParameter(FFUInt32 index, float value) override
    {
        switch (index) {
        case monitorParameter:
            if (!(value >= 0.0f && value <= maxDisplayNumber) || value != std::floor(value)) {
                return FF_FAIL;
            }
            selectedDisplay_.store(static_cast<unsigned>(value));
            return FF_SUCCESS;
        case sourceParameter: {
            if (value != 0.0f && value != 1.0f) {
                return FF_FAIL;
            }
            const auto source = value == 0.0f ? Source::Monitor : Source::Window;
            if (source == selectedSource_.load()) {
                return FF_SUCCESS;
            }
            if (source == Source::Window) {
                RefreshWindowChoices();
            }
            SetSourceVisibility(source, true);
            selectedSource_.store(source);
            return FF_SUCCESS;
        }
        case windowParameter:
            if (!(value >= 0.0f && value <= maxWindowValue) || value != std::floor(value)) {
                return FF_FAIL;
            }
            SelectWindow(static_cast<unsigned>(value));
            return FF_SUCCESS;
        case refreshParameter:
            if (value > 0.0f) {
                RefreshWindowChoices();
            }
            return FF_SUCCESS;
        default:
            return FF_FAIL;
        }
    }

    float GetFloatParameter(FFUInt32 index) override
    {
        switch (index) {
        case monitorParameter: return static_cast<float>(selectedDisplay_.load());
        case sourceParameter: return selectedSource_.load() == Source::Window ? 1.0f : 0.0f;
        case windowParameter: return static_cast<float>(SelectedWindow().value);
        default: return 0.0f;
        }
    }

    FFResult InitGL(const FFGLViewportStruct* viewport) override
    {
        if (!shader_.Compile(vertexShader, fragmentShader) || !quad_.Initialise()) {
            DeInitGL();
            return FF_FAIL;
        }

        glGenTextures(1, &texture_);
        if (texture_ == 0) {
            DeInitGL();
            return FF_FAIL;
        }

        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding textureBinding(texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize_);

        ffglex::ScopedShaderBinding shaderBinding(shader_.GetGLID());
        glUniform1i(shader_.FindUniform("desktop"), 0);
        fitLocation_ = shader_.FindUniform("fit");
        hasFrameLocation_ = shader_.FindUniform("hasFrame");
        return CFFGLPlugin::InitGL(viewport);
    }

    FFResult ProcessOpenGL(ProcessOpenGLStruct*) override
    {
        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding textureBinding(texture_);

        const auto now = std::chrono::steady_clock::now();
        const auto source = selectedSource_.load();
        const auto window = source == Source::Window ? SelectedWindow() : WindowChoice{};
        const auto selection = source == Source::Window ? window.value : selectedDisplay_.load();
        if (source != activeSource_ || selection != activeSelection_) {
            capture_.Reset();
            windowCapture_.Reset();
            activeSource_ = source;
            activeSelection_ = selection;
            if (source == Source::Monitor) {
                activeDevice_ = selection == 0 ? std::wstring{} : std::wstring(displayPrefix) + std::to_wstring(selection);
            }
            hasFrame_ = false;
            nextCapture_ = now;
        }
        if (now >= nextCapture_) {
            nextCapture_ = now + captureInterval;
            CaptureFrame(source, window.target);
        }

        float fitX = 1.0f;
        float fitY = 1.0f;
        if (hasFrame_ && currentViewport.width > 0 && currentViewport.height > 0) {
            const float desktopAspect = static_cast<float>(textureWidth_) / static_cast<float>(textureHeight_);
            const float viewportAspect = static_cast<float>(currentViewport.width) / static_cast<float>(currentViewport.height);
            fitX = std::min(1.0f, desktopAspect / viewportAspect);
            fitY = std::min(1.0f, viewportAspect / desktopAspect);
        }

        ffglex::ScopedShaderBinding shaderBinding(shader_.GetGLID());
        glUniform2f(fitLocation_, fitX, fitY);
        glUniform1i(hasFrameLocation_, hasFrame_);
        quad_.Draw();
        return FF_SUCCESS;
    }

    FFResult DeInitGL() override
    {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
        textureWidth_ = 0;
        textureHeight_ = 0;
        shader_.FreeGLResources();
        quad_.Release();
        return Disconnect();
    }

    FFResult Disconnect() override
    {
        capture_.Reset();
        windowCapture_.Reset();
        hasFrame_ = false;
        nextCapture_ = {};
        return FF_SUCCESS;
    }

private:
    void SetSourceVisibility(Source source, bool notify)
    {
        const bool windowMode = source == Source::Window;
        SetParamVisibility(monitorParameter, !windowMode, notify);
        SetParamVisibility(windowParameter, windowMode, notify);
        SetParamVisibility(refreshParameter, windowMode, notify);
    }

    WindowChoice SelectedWindow()
    {
        const std::lock_guard lock(windowMutex_);
        return selectedWindow_;
    }

    void SelectWindow(unsigned value)
    {
        const std::lock_guard lock(windowMutex_);
        const auto choice = std::find_if(windowChoices_.begin(), windowChoices_.end(), [value](const auto& item) {
            return item.value == value;
        });
        if (choice == windowChoices_.end()) {
            selectedWindow_ = {};
            if (value != 0) {
                RaiseParamEvent(windowParameter, FF_EVENT_FLAG_VALUE);
            }
        } else {
            selectedWindow_ = {{choice->target.handle, choice->target.processId, {}}, value};
        }
    }

    void RefreshWindowChoices()
    {
        auto targets = ListCaptureWindows();
        const std::lock_guard lock(windowMutex_);
        std::vector<WindowChoice> refreshed;
        std::vector<std::string> names{"None"};
        std::vector<float> values{0.0f};
        std::unordered_map<std::string, unsigned> titleCounts{{"None", 1}};

        bool selectionAvailable = selectedWindow_.value == 0;
        for (auto& target : targets) {
            const auto previous = std::find_if(windowChoices_.begin(), windowChoices_.end(), [&target](const auto& item) {
                return item.target.handle == target.handle && item.target.processId == target.processId;
            });
            const auto value = previous == windowChoices_.end() ? NextWindowValue() : previous->value;
            const auto duplicate = ++titleCounts[target.title];
            auto label = target.title;
            if (duplicate > 1) {
                label += " (" + std::to_string(duplicate) + ")";
            }
            names.push_back(std::move(label));
            values.push_back(static_cast<float>(value));
            selectionAvailable = selectionAvailable || value == selectedWindow_.value;
            refreshed.push_back({std::move(target), value});
        }
        windowChoices_ = std::move(refreshed);
        SetParamElements(windowParameter, std::move(names), values, true);
        if (!selectionAvailable) {
            selectedWindow_ = {};
            RaiseParamEvent(windowParameter, FF_EVENT_FLAG_VALUE);
        }
    }

    void CaptureFrame(Source source, const CaptureWindow& window)
    {
        if (source == Source::Monitor) {
            hasFrame_ = capture_.Capture(activeDevice_) &&
                UpdateTexture(capture_.Pixels(), capture_.Width(), capture_.Height());
            return;
        }
        if (window.handle == 0) {
            return;
        }

        switch (windowCapture_.Capture(window)) {
        case WindowCapture::Result::NewFrame:
            hasFrame_ = UpdateTexture(windowCapture_.Pixels(), windowCapture_.Width(), windowCapture_.Height());
            break;
        case WindowCapture::Result::Unavailable:
            hasFrame_ = false;
            break;
        case WindowCapture::Result::Waiting:
            break;
        }
    }

    bool UpdateTexture(const void* pixels, int width, int height)
    {
        if (!pixels || width <= 0 || height <= 0 || width > maxTextureSize_ || height > maxTextureSize_) {
            return false;
        }
        PixelUnpack unpack;
        if (width != textureWidth_ || height != textureHeight_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &textureWidth_);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &textureHeight_);
            return textureWidth_ == width && textureHeight_ == height;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        return true;
    }

    std::atomic<unsigned> selectedDisplay_{0};
    std::atomic<Source> selectedSource_{Source::Monitor};
    std::mutex windowMutex_;
    std::vector<WindowChoice> windowChoices_;
    WindowChoice selectedWindow_;
    Source activeSource_ = Source::Monitor;
    unsigned activeSelection_ = 0;
    std::wstring activeDevice_;
    DesktopCapture capture_;
    WindowCapture windowCapture_;
    std::chrono::steady_clock::time_point nextCapture_{};
    ffglex::FFGLShader shader_;
    ffglex::FFGLScreenQuad quad_;
    GLuint texture_ = 0;
    GLint textureWidth_ = 0;
    GLint textureHeight_ = 0;
    GLint maxTextureSize_ = 0;
    GLint fitLocation_ = -1;
    GLint hasFrameLocation_ = -1;
    bool hasFrame_ = false;
};

const CFFGLPluginInfo pluginInfo(
    PluginFactory<ScreenCapture>, "LDsc", "Screen Capture", 2, 1, 1, 1, FF_SOURCE,
    "Shows a Windows monitor or application window in Resolume.", "Screen Capture for Resolume");
}
