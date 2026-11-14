#include "LrcParser.h"
#include "LyricsJson.h"
#include "WindowsAudio.h"

#define NOMINMAX
#include <Windows.h>
#include <wingdi.h>
#include <winnls.h>
#include <winuser.h>
#include <winhttp.h>

#include <FFGLSDK.h>
#include "PluginPaths.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

enum Parameter : FFUInt32 {
    Source,
    Start,
    Stop,
    Reload,
    Offset,
    FontSize,
    Opacity,
    ParameterCount
};

constexpr std::array sourceNames{"Local LRC", "Online LRCLIB", "Auto"};
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
uniform sampler2D lyrics;
uniform float opacity;
uniform vec2 canvasScale;
in vec2 uv;
out vec4 color;
void main() {
    vec2 source = (uv - 0.5) * canvasScale + 0.5;
    if (any(lessThan(source, vec2(0.0))) || any(greaterThan(source, vec2(1.0)))) {
        color = vec4(0.0);
        return;
    }
    color = texture(lyrics, vec2(source.x, 1.0 - source.y)) * opacity;
}
)";

struct TextureBitmap {
    HBITMAP bitmap = nullptr;
    HDC dc = nullptr;
    HGDIOBJ previous = nullptr;
    void* pixels = nullptr;
    static constexpr int width = 2048;
    static constexpr int height = 256;

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
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) return false;
        previous = SelectObject(dc, bitmap);
        return true;
    }

    void Release()
    {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        bitmap = nullptr;
        dc = nullptr;
        previous = nullptr;
        pixels = nullptr;
    }
    ~TextureBitmap() { Release(); }
};

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

std::string UrlEncode(const std::string& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            result += static_cast<char>(character);
        } else {
            result += '%';
            result += hex[character >> 4];
            result += hex[character & 15];
        }
    }
    return result;
}

std::string OnlineLyrics(const std::string& artist, const std::string& title)
{
    if (artist.empty() || title.empty()) return {};
    const std::string query = "/api/search?artist_name=" + UrlEncode(artist) +
                               "&track_name=" + UrlEncode(title);
    HINTERNET session = WinHttpOpen(L"LyricsDisplay/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return {};
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000);
    HINTERNET connection = WinHttpConnect(session, L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return {};
    }
    const auto wideQuery = ToWide(query);
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", wideQuery.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        if (request) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return {};
    }
    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        if (available > 4 * 1024 * 1024 || body.size() + available > 4 * 1024 * 1024) {
            body.clear();
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        body += chunk;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    return ExtractSyncedLyrics(body);
}

class LyricsDisplay final : public CFFGLPlugin {
public:
    LyricsDisplay()
    {
        SetMinInputs(0);
        SetMaxInputs(0);
        SetOptionParamInfo(Source, "Source", static_cast<FFUInt32>(sourceNames.size()), 2.0f);
        for (FFUInt32 index = 0; index < sourceNames.size(); ++index) {
            SetParamElementInfo(Source, index, sourceNames[index], static_cast<float>(index));
        }
        SetParamInfof(Start, "Start", FF_TYPE_EVENT);
        SetParamInfof(Stop, "Stop", FF_TYPE_EVENT);
        SetParamInfof(Reload, "Reload", FF_TYPE_EVENT);
        SetParamInfo(Offset, "Offset ms", FF_TYPE_INTEGER, 0.0f);
        SetParamRange(Offset, -10000.0f, 10000.0f);
        SetParamInfo(FontSize, "Font Size", FF_TYPE_INTEGER, 72.0f);
        SetParamRange(FontSize, 20.0f, 180.0f);
        SetParamInfo(Opacity, "Opacity", FF_TYPE_STANDARD, 1.0f);
        SetParamRange(Opacity, 0.0f, 1.0f);
        audio_.SetEndpoint(AudioEndpoint::Output);
        loaderThread_ = std::thread([this] { RunLoader(); });
        Load();
    }

    ~LyricsDisplay() override
    {
        {
            const std::lock_guard lock(loaderMutex_);
            stopLoader_ = true;
        }
        loaderWake_.notify_one();
        if (loaderThread_.joinable()) loaderThread_.join();
    }

    FFResult SetFloatParameter(FFUInt32 index, float value) override
    {
        if (index >= ParameterCount || !std::isfinite(value)) return FF_FAIL;
        if (index == Source) {
            if (value < 0.0f || value > 2.0f || value != std::floor(value)) return FF_FAIL;
            source_.store(static_cast<int>(value));
            Load();
        } else if (index == Start && value > 0.0f) {
            playback_.store(Playback::Start);
        } else if (index == Stop && value > 0.0f) {
            playback_.store(Playback::Stop);
        } else if (index == Reload && value > 0.0f) {
            Load();
        } else if (index == Offset) {
            offsetMs_.store(static_cast<int>(std::lround(std::clamp(value, -10000.0f, 10000.0f))));
        } else if (index == FontSize) {
            fontSize_.store(static_cast<int>(std::lround(std::clamp(value, 20.0f, 180.0f))));
            textureDirty_.store(true);
        } else if (index == Opacity) {
            opacity_.store(std::clamp(value, 0.0f, 1.0f));
        }
        return FF_SUCCESS;
    }

    float GetFloatParameter(FFUInt32 index) override
    {
        if (index == Source) return static_cast<float>(source_.load());
        if (index == Offset) return static_cast<float>(offsetMs_.load());
        if (index == FontSize) return static_cast<float>(fontSize_.load());
        if (index == Opacity) return opacity_.load();
        return 0.0f;
    }

    FFResult InitGL(const FFGLViewportStruct* viewport) override
    {
        if (!shader_.Compile(vertexShader, fragmentShader) || !quad_.Initialise() ||
            !bitmap_.Create()) {
            DeInitGL();
            return FF_FAIL;
        }
        glGenTextures(1, &texture_);
        if (texture_ == 0) {
            DeInitGL();
            return FF_FAIL;
        }
        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding binding(texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TextureBitmap::width, TextureBitmap::height,
                     0, GL_BGRA, GL_UNSIGNED_BYTE, bitmap_.pixels);
        opacityLocation_ = shader_.FindUniform("opacity");
        scaleLocation_ = shader_.FindUniform("canvasScale");
        textureDirty_.store(true);
        return CFFGLPlugin::InitGL(viewport);
    }

    FFResult ProcessOpenGL(ProcessOpenGLStruct*) override
    {
        const auto command = playback_.exchange(Playback::None);
        if (command == Playback::Start) {
            startedAt_ = Clock::now();
            running_ = true;
            autoStartArmed_ = false;
        } else if (command == Playback::Stop) {
            running_ = false;
            autoStartArmed_ = false;
        }
        const auto peak = audio_.TakePeak();
        if (autoStartArmed_ && peak >= 0.12f) {
            startedAt_ = Clock::now();
            running_ = true;
            autoStartArmed_ = false;
        }
        const auto elapsed = running_
            ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt_).count() +
                  offsetMs_.load()
            : -1LL;
        std::string text;
        {
            const std::lock_guard lock(dataMutex_);
            if (elapsed >= 0) {
                for (const auto& line : lines_) {
                    if (line.timestamp.count() <= elapsed) text = line.text;
                    else break;
                }
            }
        }
        const bool dirty = textureDirty_.exchange(false);
        if (text != currentText_ || dirty) {
            currentText_ = text;
            UpdateTexture(text);
        }
        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding binding(texture_);
        ffglex::ScopedShaderBinding shaderBinding(shader_.GetGLID());
        glUniform1f(opacityLocation_, opacity_.load());
        const float aspect = currentViewport.height ? static_cast<float>(currentViewport.width) / currentViewport.height : 1.0f;
        constexpr float canvasAspect = static_cast<float>(TextureBitmap::width) / TextureBitmap::height;
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
    void Load()
    {
        {
            const std::lock_guard lock(loaderMutex_);
            ++loadRevision_;
        }
        loaderWake_.notify_one();
    }

    void RunLoader()
    {
        unsigned revision = 0;
        while (true) {
            {
                std::unique_lock lock(loaderMutex_);
                loaderWake_.wait(lock, [&] { return stopLoader_ || loadRevision_ != revision; });
                if (stopLoader_) return;
                revision = loadRevision_;
            }
            const int mode = source_.load();
            const auto local = ReadUtf8File(pluginfiles::ConfigFile(L"LyricsDisplay", L"lyrics.lrc").wstring());
            auto lines = ParseLrc(local);
            if (mode == 1 || (mode == 2 && lines.empty())) {
                auto track = ReadUtf8File(pluginfiles::ConfigFile(L"LyricsDisplay", L"track.txt").wstring());
                if (track.compare(0, 3, "\xEF\xBB\xBF") == 0) track.erase(0, 3);
                std::istringstream input(track);
                std::string artist, title;
                std::getline(input, artist);
                std::getline(input, title);
                auto trim = [](std::string& value) {
                    const auto start = value.find_first_not_of(" \t\r\n");
                    const auto end = value.find_last_not_of(" \t\r\n");
                    value = start == std::string::npos ? "" : value.substr(start, end - start + 1);
                };
                trim(artist);
                trim(title);
                auto online = ParseLrc(OnlineLyrics(artist, title));
                if (!online.empty()) lines = std::move(online);
            }
            const std::lock_guard lock(loaderMutex_);
            if (stopLoader_) return;
            if (revision != loadRevision_) continue;
            SetLines(std::move(lines));
        }
    }

    void SetLines(std::vector<LyricLine> lines)
    {
        const std::lock_guard lock(dataMutex_);
        lines_ = std::move(lines);
        textureDirty_.store(true);
    }

    void UpdateTexture(const std::string& text)
    {
        if (!bitmap_.dc || !bitmap_.pixels) return;
        RECT area{0, 0, TextureBitmap::width, TextureBitmap::height};
        HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(bitmap_.dc, &area, clear);
        DeleteObject(clear);
        GdiFlush();
        auto* pixels = static_cast<unsigned char*>(bitmap_.pixels);
        for (int index = 3; index < TextureBitmap::width * TextureBitmap::height * 4; index += 4) {
            pixels[index] = 0;
        }
        const auto font = CreateFontW(fontSize_.load(), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      ANTIALIASED_QUALITY, FF_DONTCARE, L"Arial");
        const auto previousFont = SelectObject(bitmap_.dc, font);
        SetBkMode(bitmap_.dc, TRANSPARENT);
        SetTextColor(bitmap_.dc, RGB(255, 255, 255));
        const auto wide = ToWide(text);
        SIZE size{};
        GetTextExtentPoint32W(bitmap_.dc, wide.c_str(), static_cast<int>(wide.size()), &size);
        TextOutW(bitmap_.dc, (TextureBitmap::width - size.cx) / 2,
                 (TextureBitmap::height - size.cy) / 2, wide.c_str(),
                 static_cast<int>(wide.size()));
        GdiFlush();
        for (int index = 0; index < TextureBitmap::width * TextureBitmap::height; ++index) {
            const auto offset = index * 4;
            const auto coverage = std::max({pixels[offset], pixels[offset + 1], pixels[offset + 2]});
            pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = pixels[offset + 3] = coverage;
        }
        SelectObject(bitmap_.dc, previousFont);
        DeleteObject(font);
        ffglex::ScopedSamplerActivation unit(0);
        ffglex::Scoped2DTextureBinding binding(texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TextureBitmap::width, TextureBitmap::height,
                        GL_BGRA, GL_UNSIGNED_BYTE, bitmap_.pixels);
    }

    std::mutex dataMutex_;
    std::vector<LyricLine> lines_;
    std::string currentText_;
    enum class Playback { None, Start, Stop };
    std::atomic<Playback> playback_{Playback::None};
    bool running_ = false;
    bool autoStartArmed_ = true;
    std::mutex loaderMutex_;
    std::condition_variable loaderWake_;
    unsigned loadRevision_ = 0;
    bool stopLoader_ = false;
    std::thread loaderThread_;
    std::atomic<int> source_{2};
    std::atomic<int> offsetMs_{0};
    std::atomic<int> fontSize_{72};
    std::atomic<float> opacity_{1.0f};
    std::atomic<bool> textureDirty_{true};
    Clock::time_point startedAt_ = Clock::now();
    WindowsAudio audio_;
    TextureBitmap bitmap_;
    GLuint texture_ = 0;
    ffglex::FFGLShader shader_;
    ffglex::FFGLScreenQuad quad_;
    GLint opacityLocation_ = -1;
    GLint scaleLocation_ = -1;
};

const CFFGLPluginInfo pluginInfo(
    PluginFactory<LyricsDisplay>, "LDly", "LyricsDisplay", 2, 1, 1, 0, FF_SOURCE,
    "Synchronized local and online lyrics overlay.", "LyricsDisplay for Resolume");
}
