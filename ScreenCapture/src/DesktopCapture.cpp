#include "DesktopCapture.h"

#include <windows.h>

#include <algorithm>
#include <new>
#include <utility>

namespace
{
class ScopedDpiAwareness
{
public:
    ScopedDpiAwareness()
        : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
    {
    }

    ~ScopedDpiAwareness()
    {
        if (previous_)
            SetThreadDpiAwarenessContext(previous_);
    }

private:
    DPI_AWARENESS_CONTEXT previous_;
};

struct ScreenDc
{
    HDC handle = GetDC(nullptr);

    ~ScreenDc()
    {
        if (handle)
            ReleaseDC(nullptr, handle);
    }
};

bool ReadMonitorInfo(HMONITOR monitor, MONITORINFOEXW& info)
{
    info.cbSize = sizeof(info);
    return monitor && GetMonitorInfoW(monitor, &info);
}

bool IsPrimary(const MONITORINFOEXW& info)
{
    return (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

bool Matches(const MONITORINFOEXW& info, const std::wstring& deviceName)
{
    return deviceName.empty() ? IsPrimary(info) : deviceName == info.szDevice;
}

BOOL CALLBACK CollectDisplay(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto& displays = *reinterpret_cast<std::vector<Display>*>(data);
    MONITORINFOEXW info{};
    if (ReadMonitorInfo(monitor, info))
    {
        try
        {
            displays.push_back({info.szDevice, IsPrimary(info)});
        }
        catch (const std::bad_alloc&)
        {
            return FALSE;
        }
    }
    return TRUE;
}

struct MonitorSearch
{
    const std::wstring& deviceName;
    HMONITOR monitor = nullptr;
    MONITORINFOEXW info{};
};

BOOL CALLBACK FindDisplay(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto& search = *reinterpret_cast<MonitorSearch*>(data);
    if (!ReadMonitorInfo(monitor, search.info) || !Matches(search.info, search.deviceName))
        return TRUE;

    search.monitor = monitor;
    return FALSE;
}
}

struct DesktopCapture::Frame
{
    Frame(HMONITOR target, int frameWidth, int frameHeight)
        : monitor(target), width(frameWidth), height(frameHeight)
    {
    }

    ~Frame()
    {
        if (previousBitmap)
            SelectObject(memoryDc, previousBitmap);
        if (memoryDc)
            DeleteDC(memoryDc);
        if (bitmap)
            DeleteObject(bitmap);
    }

    bool Initialise(HDC screenDc)
    {
        memoryDc = CreateCompatibleDC(screenDc);
        if (!memoryDc)
            return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels)
            return false;

        const HGDIOBJ previous = SelectObject(memoryDc, bitmap);
        if (!previous || previous == HGDI_ERROR)
            return false;

        previousBitmap = previous;
        return true;
    }

    const HMONITOR monitor;
    const int width;
    const int height;
    HDC memoryDc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    void* pixels = nullptr;
};

std::vector<Display> ListDisplays()
{
    const ScopedDpiAwareness dpiAwareness;
    std::vector<Display> displays;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectDisplay, reinterpret_cast<LPARAM>(&displays)))
        return {};

    std::sort(displays.begin(), displays.end(), [](const Display& left, const Display& right) {
        if (left.primary != right.primary)
            return left.primary;
        return left.deviceName < right.deviceName;
    });
    return displays;
}

DesktopCapture::DesktopCapture() = default;
DesktopCapture::~DesktopCapture() = default;

bool DesktopCapture::Capture(const std::wstring& deviceName)
{
    const ScopedDpiAwareness dpiAwareness;
    MonitorSearch search{deviceName};
    if (frame_ && ReadMonitorInfo(frame_->monitor, search.info) && Matches(search.info, deviceName))
        search.monitor = frame_->monitor;
    else
        EnumDisplayMonitors(nullptr, nullptr, FindDisplay, reinterpret_cast<LPARAM>(&search));

    if (!search.monitor)
    {
        Reset();
        return false;
    }

    const RECT& bounds = search.info.rcMonitor;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const ScreenDc screen;
    if (!screen.handle || width <= 0 || height <= 0)
    {
        Reset();
        return false;
    }

    if (!frame_ || frame_->monitor != search.monitor || frame_->width != width || frame_->height != height)
    {
        auto frame = std::unique_ptr<Frame>(new (std::nothrow) Frame(search.monitor, width, height));
        if (!frame || !frame->Initialise(screen.handle))
        {
            Reset();
            return false;
        }
        frame_ = std::move(frame);
    }

    if (!BitBlt(frame_->memoryDc, 0, 0, width, height, screen.handle, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT)
        || !GdiFlush())
    {
        Reset();
        return false;
    }

    return true;
}

const void* DesktopCapture::Pixels() const
{
    return frame_ ? frame_->pixels : nullptr;
}

int DesktopCapture::Width() const
{
    return frame_ ? frame_->width : 0;
}

int DesktopCapture::Height() const
{
    return frame_ ? frame_->height : 0;
}

void DesktopCapture::Reset()
{
    frame_.reset();
}
