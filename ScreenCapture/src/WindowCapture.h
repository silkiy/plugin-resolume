#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct CaptureWindow
{
    std::uintptr_t handle = 0;
    std::uint32_t processId = 0;
    std::string title;
};

std::vector<CaptureWindow> ListCaptureWindows();

class WindowCapture
{
public:
    enum class Result { NewFrame, Waiting, Unavailable };

    WindowCapture();
    ~WindowCapture();

    Result Capture(const CaptureWindow& target);

    // A new frame contains tightly packed BGRA pixels, with the bottom row first.
    const void* Pixels() const;
    int Width() const;
    int Height() const;
    void Reset();

private:
    struct State;
    std::unique_ptr<State> state_;
};
