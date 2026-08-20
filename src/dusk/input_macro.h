#ifndef DUSK_INPUT_MACRO_H
#define DUSK_INPUT_MACRO_H

#include "SSystem/SComponent/c_API_controller_pad.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace dusk::input_macro {
    enum class State {
        Idle,
        Recording,
        Playing,
    };

    void startRecording();
    void stopRecording();
    void startPlayback();
    void stopPlayback();
    void clear();
    void setLooping(bool looping);

    State state();
    bool looping();
    bool hasRecording();
    size_t recordedFrames();
    size_t playbackFrame();

    void recordResetRequest();
    std::string serializeRecording();
    bool validateSerializedRecording(std::string_view data);
    bool loadSerializedRecording(std::string_view data);
    bool tick(interface_of_controller_pad* pads, bool ctrlRResetRequested);
}

#endif
