#include "tpcm/BmsConverter.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    assert(tpcm::encodeVlq(0x7F) == std::vector<std::uint8_t>({0x7F}));
    assert(tpcm::encodeVlq(0x80) == std::vector<std::uint8_t>({0x81, 0x00}));
    assert(tpcm::encodeVlq(0x4000) == std::vector<std::uint8_t>({0x81, 0x80, 0x00}));

    std::size_t bytesRead = 0;
    assert(tpcm::decodeVlq(std::vector<std::uint8_t>({0x81, 0x00}), 0, bytesRead) == 0x80);
    assert(bytesRead == 2);

    const std::vector<std::uint8_t> bms = {
        0xD8, 0x62, 0x00, 0x78,
        0xE0, 0x00, 0x78,
        0xC1, 0x00, 0x00, 0x00, 0x12,
        0xCB, 0x00, 0x00,
        0xF1, 0x20,
        0xCC,
        0xE1, 0x0B, 0x20,
        0xB8, 0x00, 0x7F,
        0x3C, 0x01, 0x64,
        0xF1, 0x0A,
        0x81,
        0xFF,
    };

    const tpcm::BmsSequence sequence = tpcm::parseBms(bms);
    assert(sequence.tracks.size() == 2);
    assert(sequence.tracks[0].offset == 0);
    assert(sequence.tracks[0].events[1].type == tpcm::BmsEventType::Tempo);
    assert(sequence.tracks[0].events[1].value == 120);
    assert(sequence.tracks[0].events[2].type == tpcm::BmsEventType::OpenTrack);
    assert(sequence.tracks[0].events[2].value == 0x12);

    const tpcm::BmsTrack& child = sequence.tracks[1];
    assert(child.offset == 0x12);
    assert(child.bank == 11);
    assert(child.program == 32);
    assert(child.events[0].type == tpcm::BmsEventType::BankProgram);
    assert(child.events[2].type == tpcm::BmsEventType::NoteOn);
    assert(child.events[2].arg0 == 0x3C);
    assert(child.events[3].type == tpcm::BmsEventType::Wait);
    assert(child.events[3].value == 10);
    assert(child.events[4].type == tpcm::BmsEventType::NoteOff);
    assert(child.events[5].type == tpcm::BmsEventType::Finish);

    assert(tpcm::serializeBms(sequence) == bms);

    const tpcm::MidiFile midi = tpcm::bmsToMidi(bms);
    assert(midi.ticksPerQuarter == 120);
    assert(midi.tracks.size() == 2);
    assert(midi.tracks[0].events.size() == 1);
    assert(midi.tracks[1].events.size() == 3);
    assert(midi.tracks[1].events[0].bytes == std::vector<std::uint8_t>({0xC0, 0x20}));
    assert(midi.tracks[1].events[1].tick == 0);
    assert(midi.tracks[1].events[1].bytes == std::vector<std::uint8_t>({0x90, 0x3C, 0x64}));
    assert(midi.tracks[1].events[2].tick == 10);
    assert(midi.tracks[1].events[2].bytes == std::vector<std::uint8_t>({0x80, 0x3C, 0x00}));

    const std::vector<std::uint8_t> rawNoteIdZeroBms = {
        0xD8, 0x62, 0x00, 0x78,
        0xE0, 0x00, 0x78,
        0xC1, 0x00, 0x00, 0x00, 0x0D,
        0xFF,
        0xE1, 0x0B, 0x20,
        0x3D, 0x00, 0x50, 0x0A,
        0xFF,
    };
    const tpcm::MidiFile rawNoteMidi = tpcm::bmsToMidi(rawNoteIdZeroBms);
    assert(rawNoteMidi.tracks[1].events[1].bytes == std::vector<std::uint8_t>({0x90, 0x3D, 0x50}));
    assert(rawNoteMidi.tracks[1].events[2].tick == 10);

    const std::vector<std::uint8_t> midiBytes = tpcm::serializeMidi(midi);
    assert(midiBytes[0] == 'M');
    assert(midiBytes[1] == 'T');
    assert(midiBytes[2] == 'h');
    assert(midiBytes[3] == 'd');

    const tpcm::MidiFile reparsedMidi = tpcm::parseMidi(midiBytes);
    assert(reparsedMidi.ticksPerQuarter == 120);
    assert(reparsedMidi.tracks.size() == 2);
    assert(reparsedMidi.tracks[1].events.size() == 4);

    const std::vector<std::uint8_t> convertedBms = tpcm::midiToBms(reparsedMidi);
    const tpcm::BmsSequence convertedSequence = tpcm::parseBms(convertedBms);
    assert(convertedSequence.tracks.size() == 3);
    assert(convertedSequence.tracks[1].bank == 11);
    assert(convertedSequence.tracks[1].program == 0);
    assert(convertedSequence.tracks[2].bank == 11);
    assert(convertedSequence.tracks[2].program == 32);

    tpcm::MidiFile loopMidi;
    loopMidi.ticksPerQuarter = 120;
    tpcm::MidiTrack loopTempo;
    loopTempo.events.push_back({0, {0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90}});
    loopTempo.events.push_back({0, {0xFF, 0x01, 0x00}});
    loopMidi.tracks.push_back(loopTempo);
    tpcm::MidiTrack loopTrack;
    loopTrack.events.push_back({0, {0xC0, 0x20}});
    loopTrack.events.push_back({0, {0xB0, 7, 100}});
    loopTrack.events.push_back({0, {0xB0, 10, 64}});
    loopTrack.events.push_back({0, {0xB0, 91, 32}});
    loopTrack.events.push_back({0, {0x90, 60, 100}});
    loopTrack.events.push_back({60, {0x80, 60, 0}});
    loopTrack.events.push_back({120, {0xB0, 116, 0}});
    loopTrack.events.push_back({120, {0x90, 62, 100}});
    loopTrack.events.push_back({180, {0x80, 62, 0}});
    loopTrack.events.push_back({240, {0xB0, 117, 0}});
    loopMidi.tracks.push_back(loopTrack);

    const tpcm::BmsSequence loopSequence = tpcm::parseBms(tpcm::midiToBms(loopMidi));
    assert(loopSequence.tracks.size() == 3);
    assert(loopSequence.tracks[0].events[1].type == tpcm::BmsEventType::Tempo);
    assert(loopSequence.tracks[0].events[1].value == 240);
    bool sawLoopStart = false;
    bool sawLoopEnd = false;
    bool sawPan = false;
    bool sawReverb = false;
    for (const tpcm::BmsEvent& event : loopSequence.tracks[2].events) {
        sawLoopStart = sawLoopStart || event.type == tpcm::BmsEventType::LoopStart;
        sawLoopEnd = sawLoopEnd || event.type == tpcm::BmsEventType::LoopEnd;
        sawPan = sawPan || (event.command == 0xB8 && event.raw.size() == 3 && event.raw[1] == 0x03);
        sawReverb = sawReverb || event.command == 0xEA;
    }
    assert(sawLoopStart);
    assert(sawLoopEnd);
    assert(sawPan);
    assert(sawReverb);

    tpcm::MidiToBmsOptions drumOptions;
    drumOptions.drumChannels.insert(0);
    const tpcm::BmsSequence drumSequence = tpcm::parseBms(tpcm::midiToBms(loopMidi, drumOptions));
    bool sawDrumNote = false;
    bool sawDrumNoteOff = false;
    for (const tpcm::BmsEvent& event : drumSequence.tracks[2].events) {
        sawDrumNote = sawDrumNote || event.type == tpcm::BmsEventType::NoteOn;
        sawDrumNoteOff = sawDrumNoteOff || event.type == tpcm::BmsEventType::NoteOff;
    }
    assert(sawDrumNote);
    assert(!sawDrumNoteOff);

    tpcm::MidiToBmsOptions explicitLoopOptions;
    explicitLoopOptions.loopStart = 0;
    explicitLoopOptions.loopEnd = 180;
    const tpcm::BmsSequence explicitLoopSequence =
        tpcm::parseBms(tpcm::midiToBms(loopMidi, explicitLoopOptions));
    bool sawExplicitLoopStart = false;
    bool sawExplicitLoopEnd = false;
    for (const tpcm::BmsEvent& event : explicitLoopSequence.tracks[2].events) {
        sawExplicitLoopStart = sawExplicitLoopStart || event.type == tpcm::BmsEventType::LoopStart;
        sawExplicitLoopEnd = sawExplicitLoopEnd || event.type == tpcm::BmsEventType::LoopEnd;
    }
    assert(sawExplicitLoopStart);
    assert(sawExplicitLoopEnd);

    const auto summaries = tpcm::summarizeMidiChannels(loopMidi);
    assert(summaries.size() == 1);
    assert(summaries[0].channel == 0);
    assert(summaries[0].noteCount == 2);
    assert(summaries[0].volume == 100);

    tpcm::MidiFile highProgramMidi;
    highProgramMidi.ticksPerQuarter = 120;
    tpcm::MidiTrack highProgramTrack;
    highProgramTrack.events.push_back({0, {0xB0, 0, 111}});
    highProgramTrack.events.push_back({0, {0xC0, 29}});
    highProgramTrack.events.push_back({0, {0x90, 60, 100}});
    highProgramTrack.events.push_back({60, {0x80, 60, 0}});
    highProgramMidi.tracks.push_back(highProgramTrack);

    tpcm::MidiToBmsOptions highProgramOptions;
    highProgramOptions.channelOverrides[0].program = 157;
    const tpcm::BmsSequence highProgramSequence =
        tpcm::parseBms(tpcm::midiToBms(highProgramMidi, highProgramOptions));
    assert(highProgramSequence.tracks[1].bank == 11);
    assert(highProgramSequence.tracks[1].program == 0);
    assert(highProgramSequence.tracks[2].bank == 11);
    assert(highProgramSequence.tracks[2].program == 157);

    return 0;
}
