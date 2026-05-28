#include "dusk/input_macro.h"

#include <dolphin/pad.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dusk::input_macro {
    namespace {
        constexpr char kFileMagic[8] = {'D', 'S', 'K', 'I', 'M', 'A', 'C', '1'};
        constexpr uint32_t kFileVersion = 1;

        struct FileHeader {
            char magic[8];
            uint32_t version = kFileVersion;
            uint32_t frameCount = 0;
            uint8_t looping = 0;
            uint8_t reserved[3] = {};
        };

        struct FileFrame {
            interface_of_controller_pad pads[4];
            uint8_t ctrlRResetRequested = 0;
            uint8_t reserved[3] = {};
        };

        struct Frame {
            std::array<interface_of_controller_pad, 4> pads{};
            bool ctrlRResetRequested = false;
        };

        State sState = State::Idle;
        std::vector<Frame> sFrames;
        size_t sPlaybackFrame = 0;
        bool sLooping = false;
        bool sPendingResetRequest = false;
        bool sPlaybackResetComboHeld = false;

        Frame makeFrame(const interface_of_controller_pad* pads, bool ctrlRResetRequested) {
            Frame frame{};
            for (size_t i = 0; i < frame.pads.size(); ++i) {
                frame.pads[i] = pads[i];
            }
            frame.ctrlRResetRequested = ctrlRResetRequested || sPendingResetRequest;
            sPendingResetRequest = false;
            return frame;
        }

        bool decodeSerializedRecording(
            std::string_view data, std::vector<Frame>& outFrames, bool& outLooping) {
            if (data.size() < sizeof(FileHeader)) {
                return false;
            }

            FileHeader header{};
            std::memcpy(&header, data.data(), sizeof(header));
            if (std::memcmp(header.magic, kFileMagic, sizeof(header.magic)) != 0 ||
                header.version != kFileVersion)
            {
                return false;
            }

            const size_t frameCount = header.frameCount;
            const size_t expectedSize = sizeof(FileHeader) + frameCount * sizeof(FileFrame);
            if (data.size() != expectedSize) {
                return false;
            }

            std::vector<Frame> frames;
            frames.resize(frameCount);
            const char* cursor = data.data() + sizeof(FileHeader);
            for (Frame& frame : frames) {
                FileFrame fileFrame{};
                std::memcpy(&fileFrame, cursor, sizeof(fileFrame));
                cursor += sizeof(fileFrame);
                for (size_t i = 0; i < frame.pads.size(); ++i) {
                    frame.pads[i] = fileFrame.pads[i];
                }
                frame.ctrlRResetRequested = fileFrame.ctrlRResetRequested != 0;
            }

            outFrames = std::move(frames);
            outLooping = header.looping != 0;
            return true;
        }
    }

    void startRecording() {
        sFrames.clear();
        sPlaybackFrame = 0;
        sPendingResetRequest = false;
        sPlaybackResetComboHeld = false;
        sState = State::Recording;
    }

    void stopRecording() {
        if (sState == State::Recording) {
            sState = State::Idle;
        }
    }

    void startPlayback() {
        if (sFrames.empty()) {
            sState = State::Idle;
            sPlaybackFrame = 0;
            return;
        }

        sPlaybackFrame = 0;
        sPendingResetRequest = false;
        sPlaybackResetComboHeld = false;
        sState = State::Playing;
    }

    void stopPlayback() {
        if (sState == State::Playing) {
            sState = State::Idle;
        }
        sPlaybackFrame = 0;
        sPendingResetRequest = false;
        sPlaybackResetComboHeld = false;
    }

    void clear() {
        sFrames.clear();
        sPlaybackFrame = 0;
        sPendingResetRequest = false;
        sPlaybackResetComboHeld = false;
        sState = State::Idle;
    }

    void setLooping(bool looping) {
        sLooping = looping;
    }

    State state() {
        return sState;
    }

    bool looping() {
        return sLooping;
    }

    bool hasRecording() {
        return !sFrames.empty();
    }

    size_t recordedFrames() {
        return sFrames.size();
    }

    size_t playbackFrame() {
        return sPlaybackFrame;
    }

    void recordResetRequest() {
        if (sState != State::Recording) {
            return;
        }

        if (sFrames.empty()) {
            sPendingResetRequest = true;
            return;
        }

        sFrames.back().ctrlRResetRequested = true;
    }

    std::string serializeRecording() {
        if (sFrames.empty()) {
            return {};
        }

        FileHeader header{};
        std::memcpy(header.magic, kFileMagic, sizeof(header.magic));
        header.frameCount = static_cast<uint32_t>(sFrames.size());
        header.looping = sLooping ? 1 : 0;

        std::string data(sizeof(FileHeader) + sFrames.size() * sizeof(FileFrame), '\0');
        char* cursor = data.data();
        std::memcpy(cursor, &header, sizeof(header));
        cursor += sizeof(header);
        for (const Frame& frame : sFrames) {
            FileFrame fileFrame{};
            for (size_t i = 0; i < frame.pads.size(); ++i) {
                fileFrame.pads[i] = frame.pads[i];
            }
            fileFrame.ctrlRResetRequested = frame.ctrlRResetRequested ? 1 : 0;
            std::memcpy(cursor, &fileFrame, sizeof(fileFrame));
            cursor += sizeof(fileFrame);
        }

        return data;
    }

    bool validateSerializedRecording(std::string_view data) {
        std::vector<Frame> frames;
        bool loadedLooping = false;
        return decodeSerializedRecording(data, frames, loadedLooping);
    }

    bool loadSerializedRecording(std::string_view data) {
        std::vector<Frame> frames;
        bool loadedLooping = false;
        if (!decodeSerializedRecording(data, frames, loadedLooping)) {
            return false;
        }

        sFrames = std::move(frames);
        sLooping = loadedLooping;
        sPlaybackFrame = 0;
        sPendingResetRequest = false;
        sPlaybackResetComboHeld = false;
        sState = State::Idle;
        return true;
    }

    bool tick(interface_of_controller_pad* pads, bool ctrlRResetRequested) {
        if (sState == State::Recording) {
            sFrames.push_back(makeFrame(pads, ctrlRResetRequested));
            return false;
        }

        if (sState != State::Playing || sFrames.empty()) {
            return false;
        }

        const Frame& frame = sFrames[sPlaybackFrame];
        for (size_t i = 0; i < frame.pads.size(); ++i) {
            pads[i] = frame.pads[i];
        }

        constexpr u32 resetCombo = PAD_BUTTON_START | PAD_BUTTON_X | PAD_BUTTON_B;
        const bool resetComboHeld = (frame.pads[0].mButtonFlags & resetCombo) == resetCombo;
        const bool replayResetRequested =
            frame.ctrlRResetRequested || (resetComboHeld && !sPlaybackResetComboHeld);
        sPlaybackResetComboHeld = resetComboHeld;
        ++sPlaybackFrame;
        if (sPlaybackFrame >= sFrames.size()) {
            if (sLooping) {
                sPlaybackFrame = 0;
                sPlaybackResetComboHeld = false;
            } else {
                sState = State::Idle;
                sPlaybackFrame = 0;
                sPlaybackResetComboHeld = false;
            }
        }

        return replayResetRequested;
    }
}
