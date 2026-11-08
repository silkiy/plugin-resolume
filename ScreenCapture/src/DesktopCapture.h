#pragma once

#include <memory>
#include <string>
#include <vector>

struct Display
{
    std::wstring deviceName;
    bool primary;
};

std::vector<Display> ListDisplays();

class DesktopCapture
{
public:
    DesktopCapture();
    ~DesktopCapture();

    // An empty device name tracks the current primary display.
    bool Capture(const std::wstring& deviceName);

    // Valid after a successful capture until the next Capture or Reset call.
    // Rows run bottom to top. Each pixel is four bytes: blue, green, red, unused.
    const void* Pixels() const;
    int Width() const;
    int Height() const;
    void Reset();

private:
    struct Frame;
    std::unique_ptr<Frame> frame_;
};
