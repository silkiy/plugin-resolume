#pragma once

#include <memory>

class WindowsAudio {
public:
    WindowsAudio();
    ~WindowsAudio();
    float TakePeak();

private:
    struct State;
    std::unique_ptr<State> state_;
};
