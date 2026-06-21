#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace tpcm {

constexpr std::uint16_t kGameTicksPerQuarter = 120;

enum class BmsEventType : std::uint8_t {
    Wait,
    Tempo,
    BankProgram,
    Bank,
    Program,
    Note,
    NoteOn,
    NoteOff,
    OpenTrack,
    LoopStart,
    LoopEnd,
    Finish,
    Unknown,
};

struct BmsEvent {
    BmsEventType type = BmsEventType::Unknown;
    std::uint32_t tick = 0;
    std::uint8_t command = 0;
    std::uint32_t value = 0;
    std::uint8_t arg0 = 0;
    std::uint8_t arg1 = 0;
    std::uint8_t arg2 = 0;
    std::vector<std::uint8_t> raw;
};

struct BmsTrack {
    std::uint32_t offset = 0;
    std::uint8_t id = 0;
    std::uint8_t bank = 11;
    std::uint8_t program = 0;
    std::vector<BmsEvent> events;
};

struct BmsSequence {
    std::vector<BmsTrack> tracks;
};

struct MidiEvent {
    std::uint32_t tick = 0;
    std::vector<std::uint8_t> bytes;
};

struct MidiTrack {
    std::vector<MidiEvent> events;
};

struct MidiFile {
    std::uint16_t ticksPerQuarter = kGameTicksPerQuarter;
    std::vector<MidiTrack> tracks;
};

struct MidiToBmsOptions {
    int masterVol = 127;
    bool enableLoops = true;
    int loopStart = -1;
    int loopEnd = -1;
    std::set<int> drumChannels;

    struct ChannelOverride {
        int bank = -1;
        int program = -1;
        int volume = -1;
        int pan = -1;
        int reverb = -1;
        bool drum = false;
        bool drumSet = false;
        bool mute = false;
    };
    std::map<int, ChannelOverride> channelOverrides;
};

struct MidiChannelSummary {
    int channel = 0;
    int noteCount = 0;
    int midiBank = 0;
    int midiProgram = 0;
    int gameBank = 11;
    int gameProgram = 0;
    int volume = 127;
    int pan = -1;
    int reverb = -1;
};

std::vector<std::uint8_t> encodeVlq(std::uint32_t value);
std::uint32_t decodeVlq(const std::vector<std::uint8_t>& data, std::size_t offset, std::size_t& bytesRead);

BmsSequence parseBms(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> serializeBms(const BmsSequence& sequence);

MidiFile bmsToMidi(const std::vector<std::uint8_t>& bmsData);
std::vector<std::uint8_t> serializeMidi(const MidiFile& midi);
MidiFile parseMidi(const std::vector<std::uint8_t>& midiData);
std::vector<std::uint8_t> midiToBms(const MidiFile& midi, MidiToBmsOptions options = {});
std::vector<MidiChannelSummary> summarizeMidiChannels(const MidiFile& midi);

}  // namespace tpcm
