#pragma once

#include <memory>

enum class AudioEndpoint { None, Output, Input };

class WindowsAudio {
public:
    WindowsAudio();
    ~WindowsAudio();

    void SetEndpoint(AudioEndpoint endpoint);

    // Returns the peak since the last read. -1 means no new measurement.
    float TakePeak();

private:
    struct State;
    std::unique_ptr<State> state_;
};
