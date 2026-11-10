#pragma once

#include <memory>

enum class AudioEndpoint { None, Output };

class WindowsAudio {
public:
    WindowsAudio();
    ~WindowsAudio();
    void SetEndpoint(AudioEndpoint endpoint);
    float TakePeak();

private:
    struct State;
    std::unique_ptr<State> state_;
};
