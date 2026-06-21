#include "tpcm/BmsConverter.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace tpcm {
namespace {

constexpr std::uint8_t CMD_NOTE = 0xB3;
constexpr std::uint8_t CMD_NOTE_ON = 0xB1;
constexpr std::uint8_t CMD_NOTE_OFF = 0xB2;
constexpr std::uint8_t CMD_OPEN = 0xC1;
constexpr std::uint8_t CMD_LOOP_S = 0xCB;
constexpr std::uint8_t CMD_LOOP_E = 0xCC;
constexpr std::uint8_t CMD_TEMPO = 0xE0;
constexpr std::uint8_t CMD_BANK_PRG = 0xE1;
constexpr std::uint8_t CMD_BANK = 0xE2;
constexpr std::uint8_t CMD_PRG = 0xE3;
constexpr std::uint8_t CMD_WAIT = 0xF0;
constexpr std::uint8_t CMD_WAIT_B = 0xF1;
constexpr std::uint8_t CMD_FINISH = 0xFF;

constexpr int paramBytes(int type) {
    switch (type) {
        case 0:
            return 1;
        case 1:
            return 2;
        case 2:
            return 3;
        case 3:
            return 1;
    }
    return 0;
}

constexpr int skipBytes(int count, int types) {
    int total = 0;
    for (int i = 0; i < count; ++i) {
        total += paramBytes((types >> (i * 2)) & 3);
    }
    return total;
}

constexpr std::array<int, 0x100> makeSkipTable() {
    std::array<int, 0x100> table{};
    table[0xB1] = skipBytes(3, 0x0000);
    table[0xB2] = skipBytes(1, 0x0000);
    table[0xB3] = skipBytes(4, 0x0040);
    table[0xB4] = skipBytes(1, 0x0000);
    table[0xB8] = skipBytes(2, 0x0000);
    table[0xB9] = skipBytes(2, 0x0004);
    table[0xBA] = skipBytes(3, 0x0010);
    table[0xBB] = skipBytes(3, 0x0014);
    table[0xC1] = skipBytes(2, 0x0008);
    table[0xC2] = skipBytes(1, 0x0000);
    table[0xC3] = skipBytes(1, 0x0002);
    table[0xC4] = skipBytes(2, 0x0008);
    table[0xC5] = 0;
    table[0xC6] = skipBytes(1, 0x0000);
    table[0xC7] = skipBytes(1, 0x0002);
    table[0xC8] = skipBytes(2, 0x0008);
    table[0xC9] = skipBytes(2, 0x000B);
    table[0xCA] = skipBytes(2, 0x000B);
    table[0xCB] = skipBytes(1, 0x0001);
    table[0xCC] = 0;
    table[0xD0] = skipBytes(2, 0x0000);
    table[0xD1] = skipBytes(2, 0x000C);
    table[0xD2] = skipBytes(1, 0x0000);
    table[0xD3] = skipBytes(1, 0x0000);
    table[0xD4] = skipBytes(2, 0x000C);
    table[0xD5] = skipBytes(2, 0x000C);
    table[0xD6] = skipBytes(2, 0x0000);
    table[0xD7] = skipBytes(2, 0x0000);
    table[0xD8] = skipBytes(2, 0x0004);
    table[0xD9] = skipBytes(3, 0x0030);
    table[0xDA] = skipBytes(3, 0x0010);
    table[0xDB] = skipBytes(2, 0x0000);
    table[0xDC] = skipBytes(4, 0x00E0);
    table[0xE0] = skipBytes(1, 0x0001);
    table[0xE1] = skipBytes(1, 0x0001);
    table[0xE2] = skipBytes(1, 0x0000);
    table[0xE3] = skipBytes(1, 0x0000);
    table[0xE7] = skipBytes(2, 0x0004);
    table[0xE8] = skipBytes(2, 0x0008);
    table[0xE9] = skipBytes(5, 0x0155);
    table[0xEA] = skipBytes(2, 0x0004);
    table[0xEB] = skipBytes(1, 0x0000);
    table[0xEC] = skipBytes(4, 0x0055);
    table[0xED] = skipBytes(1, 0x0002);
    table[0xF1] = skipBytes(1, 0x0000);
    table[0xF3] = skipBytes(1, 0x0002);
    table[0xF4] = skipBytes(1, 0x0001);
    table[0xF5] = skipBytes(1, 0x0001);
    table[0xF6] = 0;
    table[0xF7] = 0;
    table[0xF8] = skipBytes(2, 0x0004);
    table[0xF9] = skipBytes(1, 0x0001);
    table[0xFE] = 0;
    table[0xFF] = 0;
    return table;
}

constexpr std::array<int, 0x100> kCmdSkip = makeSkipTable();

std::uint16_t readU16Be(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        throw std::runtime_error("Truncated BMS u16");
    }
    return static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
}

std::uint32_t readU32Be(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("Truncated u32");
    }
    return static_cast<std::uint32_t>((data[offset] << 24) | (data[offset + 1] << 16)
                                      | (data[offset + 2] << 8) | data[offset + 3]);
}

std::uint32_t readU24Be(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 3 > data.size()) {
        throw std::runtime_error("Truncated BMS u24");
    }
    return static_cast<std::uint32_t>((data[offset] << 16) | (data[offset + 1] << 8) | data[offset + 2]);
}

void appendU16Be(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendU24Be(std::vector<std::uint8_t>& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendU32Be(std::vector<std::uint8_t>& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void requireBytes(const std::vector<std::uint8_t>& data, std::size_t offset, std::size_t size) {
    if (offset + size > data.size()) {
        throw std::runtime_error("Truncated BMS command");
    }
}

BmsTrack parseTrack(const std::vector<std::uint8_t>& data,
                    std::uint32_t offset,
                    std::uint8_t id,
                    std::map<std::uint32_t, std::uint8_t>& childIds) {
    BmsTrack track;
    track.offset = offset;
    track.id = id;

    std::size_t pos = offset;
    std::uint32_t tick = 0;
    while (pos < data.size()) {
        const std::uint8_t command = data[pos];
        BmsEvent event;
        event.command = command;
        event.tick = tick;

        if (command < 0x80) {
            requireBytes(data, pos, 3);
            event.arg0 = command;
            event.arg1 = data[pos + 1];
            event.arg2 = data[pos + 2];
            pos += 3;
            if ((event.arg1 & 0x07) == 0) {
                std::size_t bytesRead = 0;
                event.type = BmsEventType::Note;
                event.value = decodeVlq(data, pos, bytesRead);
                event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos - 3),
                             data.begin() + static_cast<std::ptrdiff_t>(pos + bytesRead)};
                tick += event.value;
                pos += bytesRead;
            } else {
                event.type = BmsEventType::NoteOn;
                event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos - 3),
                             data.begin() + static_cast<std::ptrdiff_t>(pos)};
            }
        } else if (command >= 0x80 && command < 0x88) {
            event.type = BmsEventType::NoteOff;
            event.arg0 = static_cast<std::uint8_t>(command & 0x07);
            event.raw = {command};
            ++pos;
        } else if (command == CMD_WAIT) {
            std::size_t bytesRead = 0;
            const std::uint32_t wait = decodeVlq(data, pos + 1, bytesRead);
            event.type = BmsEventType::Wait;
            event.value = wait;
            event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 1 + bytesRead)};
            tick += wait;
            pos += 1 + bytesRead;
        } else if (command == CMD_WAIT_B) {
            requireBytes(data, pos, 2);
            event.type = BmsEventType::Wait;
            event.value = data[pos + 1];
            event.raw = {data[pos], data[pos + 1]};
            tick += event.value;
            pos += 2;
        } else if (command == CMD_TEMPO) {
            requireBytes(data, pos, 3);
            event.type = BmsEventType::Tempo;
            event.value = readU16Be(data, pos + 1);
            event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 3)};
            pos += 3;
        } else if (command == CMD_BANK_PRG) {
            requireBytes(data, pos, 3);
            event.type = BmsEventType::BankProgram;
            event.value = readU16Be(data, pos + 1);
            track.bank = static_cast<std::uint8_t>((event.value >> 8) & 0xFF);
            track.program = static_cast<std::uint8_t>(event.value & 0xFF);
            event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 3)};
            pos += 3;
        } else if (command == CMD_BANK) {
            requireBytes(data, pos, 2);
            event.type = BmsEventType::Bank;
            event.value = data[pos + 1];
            track.bank = data[pos + 1];
            event.raw = {data[pos], data[pos + 1]};
            pos += 2;
        } else if (command == CMD_PRG) {
            requireBytes(data, pos, 2);
            event.type = BmsEventType::Program;
            event.value = data[pos + 1];
            track.program = data[pos + 1];
            event.raw = {data[pos], data[pos + 1]};
            pos += 2;
        } else if (command == CMD_NOTE) {
            requireBytes(data, pos, 6);
            event.type = BmsEventType::Note;
            event.arg0 = data[pos + 1];
            event.arg1 = data[pos + 2];
            event.arg2 = data[pos + 3];
            event.value = readU16Be(data, pos + 4);
            event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 6)};
            tick += event.value;
            pos += 6;
        } else if (command == CMD_OPEN) {
            requireBytes(data, pos, 5);
            event.type = BmsEventType::OpenTrack;
            event.arg0 = data[pos + 1];
            event.value = readU24Be(data, pos + 2);
            childIds.emplace(event.value, event.arg0);
            event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 5)};
            pos += 5;
        } else if (command == CMD_LOOP_S) {
            requireBytes(data, pos, 3);
            event.type = BmsEventType::LoopStart;
            event.value = readU16Be(data, pos + 1);
            event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 3)};
            pos += 3;
        } else if (command == CMD_LOOP_E) {
            event.type = BmsEventType::LoopEnd;
            event.raw = {command};
            ++pos;
            track.events.push_back(std::move(event));
            break;
        } else if (command == CMD_FINISH) {
            event.type = BmsEventType::Finish;
            event.raw = {command};
            ++pos;
            track.events.push_back(std::move(event));
            break;
        } else {
            const int skip = kCmdSkip[command];
            requireBytes(data, pos, 1 + static_cast<std::size_t>(skip));
            event.type = BmsEventType::Unknown;
            event.raw = {data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + 1 + skip)};
            pos += 1 + static_cast<std::size_t>(skip);
        }

        track.events.push_back(std::move(event));
    }

    return track;
}

void appendEventBytes(std::vector<std::uint8_t>& out, const BmsEvent& event) {
    if (!event.raw.empty()) {
        out.insert(out.end(), event.raw.begin(), event.raw.end());
        return;
    }

    switch (event.type) {
        case BmsEventType::Wait:
            if (event.value <= 255) {
                out.push_back(CMD_WAIT_B);
                out.push_back(static_cast<std::uint8_t>(event.value));
            } else {
                out.push_back(CMD_WAIT);
                const std::vector<std::uint8_t> vlq = encodeVlq(event.value);
                out.insert(out.end(), vlq.begin(), vlq.end());
            }
            break;
        case BmsEventType::Tempo:
            out.push_back(CMD_TEMPO);
            appendU16Be(out, static_cast<std::uint16_t>(event.value));
            break;
        case BmsEventType::BankProgram:
            out.push_back(CMD_BANK_PRG);
            appendU16Be(out, static_cast<std::uint16_t>(event.value));
            break;
        case BmsEventType::Bank:
            out.push_back(CMD_BANK);
            out.push_back(static_cast<std::uint8_t>(event.value));
            break;
        case BmsEventType::Program:
            out.push_back(CMD_PRG);
            out.push_back(static_cast<std::uint8_t>(event.value));
            break;
        case BmsEventType::Note:
            out.push_back(CMD_NOTE);
            out.push_back(event.arg0);
            out.push_back(event.arg1);
            out.push_back(event.arg2);
            appendU16Be(out, static_cast<std::uint16_t>(event.value));
            break;
        case BmsEventType::NoteOn:
            out.push_back(event.arg0 & 0x7F);
            out.push_back(event.arg1 & 0x07);
            out.push_back(event.arg2 & 0x7F);
            break;
        case BmsEventType::NoteOff:
            out.push_back(static_cast<std::uint8_t>(0x80 | (event.arg0 & 0x07)));
            break;
        case BmsEventType::OpenTrack:
            out.push_back(CMD_OPEN);
            out.push_back(event.arg0);
            appendU24Be(out, event.value);
            break;
        case BmsEventType::LoopStart:
            out.push_back(CMD_LOOP_S);
            appendU16Be(out, static_cast<std::uint16_t>(event.value));
            break;
        case BmsEventType::LoopEnd:
            out.push_back(CMD_LOOP_E);
            break;
        case BmsEventType::Finish:
            out.push_back(CMD_FINISH);
            break;
        case BmsEventType::Unknown:
            throw std::runtime_error("Cannot serialize unknown BMS event without raw bytes");
    }
}

void appendMidiVarLen(std::vector<std::uint8_t>& data, std::uint32_t value) {
    const std::vector<std::uint8_t> encoded = encodeVlq(value);
    data.insert(data.end(), encoded.begin(), encoded.end());
}

std::vector<std::uint8_t> serializeMidiTrack(const MidiTrack& track) {
    std::vector<MidiEvent> events = track.events;
    std::sort(events.begin(), events.end(), [](const MidiEvent& a, const MidiEvent& b) {
        return a.tick < b.tick;
    });

    std::vector<std::uint8_t> data;
    std::uint32_t previousTick = 0;
    for (const MidiEvent& event : events) {
        appendMidiVarLen(data, event.tick - previousTick);
        data.insert(data.end(), event.bytes.begin(), event.bytes.end());
        previousTick = event.tick;
    }

    appendMidiVarLen(data, 0);
    data.insert(data.end(), {0xFF, 0x2F, 0x00});
    return data;
}

std::uint16_t detectTimebase(const std::vector<std::uint8_t>& data) {
    for (std::size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] == 0xD8 && data[i + 1] == 0x62) {
            return readU16Be(data, i + 2);
        }
    }
    return kGameTicksPerQuarter;
}

std::uint16_t firstTempoBpm(const BmsSequence& sequence) {
    for (const BmsTrack& track : sequence.tracks) {
        for (const BmsEvent& event : track.events) {
            if (event.type == BmsEventType::Tempo && event.value > 0) {
                return static_cast<std::uint16_t>(event.value);
            }
        }
    }
    return 120;
}

void addTempoTrack(MidiFile& midi, std::uint16_t bpm) {
    MidiTrack tempoTrack;
    const std::uint32_t tempo = 60000000u / bpm;
    tempoTrack.events.push_back({0, {0xFF, 0x51, 0x03,
                                     static_cast<std::uint8_t>((tempo >> 16) & 0xFF),
                                     static_cast<std::uint8_t>((tempo >> 8) & 0xFF),
                                     static_cast<std::uint8_t>(tempo & 0xFF)}});
    midi.tracks.push_back(std::move(tempoTrack));
}

std::uint8_t midiBankForGameBank(std::uint8_t bank, std::uint8_t program) {
    if (bank == 11 && program >= 128) return 111;
    if (bank == 11) return 11;
    if (bank == 12 && program >= 128) return 112;
    if (bank == 12) return 12;
    if (bank == 13 && program >= 128) return 113;
    if (bank == 13) return 13;
    if (bank == 50 && program >= 128) return 150;
    if (bank == 50) return 50;
    if (bank == 51 && program >= 128) return 151;
    if (bank == 51) return 51;
    if (bank == 52 && program >= 128) return 152;
    if (bank == 52) return 52;
    if (bank == 53 && program >= 128) return 153;
    if (bank == 53) return 53;
    return 0;
}

std::uint8_t midiProgramForGameProgram(std::uint8_t bank, std::uint8_t program) {
    if ((bank == 11 || bank == 12 || bank == 13 || bank == 50 || bank == 51 || bank == 52
         || bank == 53)
        && program >= 128) {
        return static_cast<std::uint8_t>(program - 128);
    }
    return static_cast<std::uint8_t>(std::min<int>(program, 127));
}

bool hasNotes(const BmsTrack& track) {
    for (const BmsEvent& event : track.events) {
        if (event.type == BmsEventType::Note || event.type == BmsEventType::NoteOn) {
            return true;
        }
    }
    return false;
}

struct MidiNote {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
};

struct MidiChannelData {
    std::uint8_t bank = 0;
    std::uint8_t program = 0;
    std::uint8_t volume = 127;
    bool hasPan = false;
    std::uint8_t pan = 64;
    bool hasReverb = false;
    std::uint8_t reverb = 0;
    bool hasLoopStart = false;
    bool hasLoopEnd = false;
    std::uint32_t loopStart = 0;
    std::uint32_t loopEnd = 0;
    std::vector<MidiNote> notes;
};

std::uint16_t bpmFromTempoMeta(const std::vector<std::uint8_t>& bytes) {
    const std::uint32_t micros = (bytes[3] << 16) | (bytes[4] << 8) | bytes[5];
    if (micros != 0) {
        return static_cast<std::uint16_t>(std::max<std::uint32_t>(1, 60000000u / micros));
    }
    return 120;
}

bool isTempoMeta(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() == 6 && bytes[0] == 0xFF && bytes[1] == 0x51 && bytes[2] == 0x03;
}

std::uint8_t gameBankForMidiBank(std::uint8_t midiBank) {
    if (midiBank == 111) return 11;
    if (midiBank == 12)  return 12;
    if (midiBank == 112) return 12;
    if (midiBank == 13)  return 13;
    if (midiBank == 113) return 13;
    if (midiBank == 50)  return 50;
    if (midiBank == 150) return 50;
    if (midiBank == 51)  return 51;
    if (midiBank == 151) return 51;
    if (midiBank == 52)  return 52;
    if (midiBank == 53)  return 53;
    if (midiBank == 152) return 52;
    if (midiBank == 153) return 53;
    return 11;
}

std::uint8_t gameProgramForMidi(std::uint8_t midiBank, std::uint8_t midiProgram) {
    if (midiBank == 111 || midiBank == 112 || midiBank == 113 || midiBank == 150
        || midiBank == 151 || midiBank == 152 || midiBank == 153) {
        return static_cast<std::uint8_t>(midiProgram + 128);
    }
    return midiProgram;
}

void appendWait(std::vector<std::uint8_t>& out, std::uint32_t ticks) {
    if (ticks == 0) {
        return;
    }
    if (ticks <= 255) {
        out.push_back(CMD_WAIT_B);
        out.push_back(static_cast<std::uint8_t>(ticks));
    } else {
        out.push_back(CMD_WAIT);
        const std::vector<std::uint8_t> wait = encodeVlq(ticks);
        out.insert(out.end(), wait.begin(), wait.end());
    }
}

void appendNoteEdges(std::vector<std::uint8_t>& out, const std::vector<MidiNote>& notes,
                     std::uint32_t& cursor, bool drum) {
    if (drum) {
        std::uint8_t noteId = 1;
        std::vector<MidiNote> sortedNotes = notes;
        std::sort(sortedNotes.begin(), sortedNotes.end(), [](const MidiNote& a, const MidiNote& b) {
            return std::tie(a.start, a.key) < std::tie(b.start, b.key);
        });
        for (const MidiNote& note : sortedNotes) {
            if (note.start > cursor) appendWait(out, note.start - cursor);
            cursor = note.start;
            out.push_back(note.key & 0x7F);
            out.push_back(noteId);
            out.push_back(note.velocity & 0x7F);
            noteId = static_cast<std::uint8_t>((noteId % 7) + 1);
        }
        return;
    }

    struct NoteEdge {
        std::uint32_t tick = 0;
        bool on = false;
        std::uint8_t noteId = 0;
        std::uint8_t key = 0;
        std::uint8_t velocity = 0;
    };

    std::array<std::uint32_t, 8> noteIdFreeAt{};
    std::vector<NoteEdge> edges;
    std::vector<MidiNote> sortedNotes = notes;
    std::sort(sortedNotes.begin(), sortedNotes.end(), [](const MidiNote& a, const MidiNote& b) {
        return std::tie(a.start, a.end, a.key) < std::tie(b.start, b.end, b.key);
    });

    for (const MidiNote& note : sortedNotes) {
        std::uint8_t chosen = 1;
        for (std::uint8_t i = 2; i <= 7; ++i) {
            if (noteIdFreeAt[i] < noteIdFreeAt[chosen]) {
                chosen = i;
            }
        }
        noteIdFreeAt[chosen] = note.end;
        edges.push_back({note.start, true, chosen, note.key, note.velocity});
        edges.push_back({note.end, false, chosen, 0, 0});
    }

    std::sort(edges.begin(), edges.end(), [](const NoteEdge& a, const NoteEdge& b) {
        if (a.tick != b.tick) {
            return a.tick < b.tick;
        }
        return a.on && !b.on;
    });

    for (const NoteEdge& edge : edges) {
        if (edge.tick > cursor) {
            appendWait(out, edge.tick - cursor);
        }
        cursor = edge.tick;
        if (edge.on) {
            out.push_back(edge.key & 0x7F);
            out.push_back(edge.noteId & 0x07);
            out.push_back(edge.velocity & 0x7F);
        } else {
            out.push_back(static_cast<std::uint8_t>(0x80 | (edge.noteId & 0x07)));
        }
    }
}

std::vector<std::uint8_t> buildMidiChildTrack(const MidiChannelData& channel, bool enableLoops,
                                               bool drum) {
    std::vector<std::uint8_t> out;
    const std::uint8_t bank = gameBankForMidiBank(channel.bank);
    const std::uint8_t program = gameProgramForMidi(channel.bank, channel.program);
    out.push_back(CMD_BANK_PRG);
    appendU16Be(out, static_cast<std::uint16_t>((bank << 8) | program));
    out.insert(out.end(), {0xB8, 0x00, static_cast<std::uint8_t>(channel.volume & 0x7F)});
    if (channel.hasPan) {
        out.insert(out.end(), {0xB8, 0x03, static_cast<std::uint8_t>(channel.pan & 0x7F)});
    }
    if (channel.hasReverb && channel.reverb > 0) {
        const std::uint16_t level = static_cast<std::uint16_t>((channel.reverb * 0x4000) / 127);
        out.insert(out.end(), {0xEA, 0x00});
        appendU16Be(out, level);
    }
    std::uint32_t cursor = 0;

    const bool hasLoop = enableLoops && channel.hasLoopStart;
    if (hasLoop) {
        std::vector<MidiNote> preLoop;
        std::vector<MidiNote> loopBody;
        for (const MidiNote& note : channel.notes) {
            if (note.start < channel.loopStart) {
                preLoop.push_back(note);
            } else if (!channel.hasLoopEnd || note.start < channel.loopEnd) {
                loopBody.push_back(note);
            }
        }
        appendNoteEdges(out, preLoop, cursor, drum);
        if (channel.loopStart > cursor) {
            appendWait(out, channel.loopStart - cursor);
        }
        cursor = channel.loopStart;
        out.push_back(CMD_LOOP_S);
        appendU16Be(out, 0);
        appendNoteEdges(out, loopBody, cursor, drum);
        if (channel.hasLoopEnd && channel.loopEnd > cursor) {
            appendWait(out, channel.loopEnd - cursor);
            cursor = channel.loopEnd;
        }
        out.push_back(CMD_LOOP_E);
    } else {
        appendNoteEdges(out, channel.notes, cursor, drum);
    }

    out.push_back(CMD_FINISH);
    return out;
}

std::vector<std::uint8_t> buildSilentTrack() {
    std::vector<std::uint8_t> out;
    out.push_back(CMD_BANK_PRG);
    appendU16Be(out, 0x0B00);
    out.insert(out.end(), {0xB8, 0x00, 0x7F, CMD_WAIT, 0xFF, 0xFF, 0x7F, CMD_FINISH});
    return out;
}

}  // namespace

std::vector<std::uint8_t> encodeVlq(std::uint32_t value) {
    if (value < 0x80) {
        return {static_cast<std::uint8_t>(value)};
    }
    if (value < 0x4000) {
        return {static_cast<std::uint8_t>((value >> 7) | 0x80), static_cast<std::uint8_t>(value & 0x7F)};
    }
    return {static_cast<std::uint8_t>((value >> 14) | 0x80),
            static_cast<std::uint8_t>(((value >> 7) & 0x7F) | 0x80),
            static_cast<std::uint8_t>(value & 0x7F)};
}

std::uint32_t decodeVlq(const std::vector<std::uint8_t>& data, std::size_t offset, std::size_t& bytesRead) {
    requireBytes(data, offset, 1);
    std::uint8_t byte = data[offset];
    if (byte < 0x80) {
        bytesRead = 1;
        return byte;
    }

    std::uint32_t value = byte & 0x7F;
    requireBytes(data, offset + 1, 1);
    byte = data[offset + 1];
    value = (value << 7) | (byte & 0x7F);
    if ((byte & 0x80) == 0) {
        bytesRead = 2;
        return value;
    }

    requireBytes(data, offset + 2, 1);
    byte = data[offset + 2];
    bytesRead = 3;
    return (value << 7) | (byte & 0x7F);
}

BmsSequence parseBms(const std::vector<std::uint8_t>& data) {
    if (data.empty()) {
        throw std::runtime_error("Empty BMS data");
    }

    BmsSequence sequence;
    std::map<std::uint32_t, std::uint8_t> childIds;
    sequence.tracks.push_back(parseTrack(data, 0, 0, childIds));

    for (const auto& child : childIds) {
        if (child.first < data.size()) {
            sequence.tracks.push_back(parseTrack(data, child.first, child.second, childIds));
        }
    }

    std::sort(sequence.tracks.begin(), sequence.tracks.end(), [](const BmsTrack& a, const BmsTrack& b) {
        return a.offset < b.offset;
    });
    return sequence;
}

std::vector<std::uint8_t> serializeBms(const BmsSequence& sequence) {
    std::vector<std::uint8_t> out;
    for (const BmsTrack& track : sequence.tracks) {
        if (out.size() < track.offset) {
            out.resize(track.offset, 0);
        }
        if (out.size() != track.offset) {
            throw std::runtime_error("Cannot serialize overlapping BMS tracks");
        }
        for (const BmsEvent& event : track.events) {
            appendEventBytes(out, event);
        }
    }
    return out;
}

MidiFile bmsToMidi(const std::vector<std::uint8_t>& bmsData) {
    const BmsSequence sequence = parseBms(bmsData);

    MidiFile midi;
    midi.ticksPerQuarter = detectTimebase(bmsData);
    addTempoTrack(midi, firstTempoBpm(sequence));

    std::uint8_t channel = 0;
    for (const BmsTrack& bmsTrack : sequence.tracks) {
        if (!hasNotes(bmsTrack)) {
            continue;
        }

        MidiTrack midiTrack;
        const std::uint8_t midiBank = midiBankForGameBank(bmsTrack.bank, bmsTrack.program);
        const std::uint8_t midiProgram = midiProgramForGameProgram(bmsTrack.bank, bmsTrack.program);
        if (midiBank != 0) {
            midiTrack.events.push_back({0, {static_cast<std::uint8_t>(0xB0 | channel), 0x00, midiBank}});
        }
        midiTrack.events.push_back({0, {static_cast<std::uint8_t>(0xC0 | channel), midiProgram}});

        std::map<std::uint8_t, BmsEvent> activeNotes;
        std::set<std::uint32_t> loopStartTicks;
        std::set<std::uint32_t> loopEndTicks;
        for (const BmsEvent& event : bmsTrack.events) {
            if (event.type == BmsEventType::LoopStart) {
                loopStartTicks.insert(event.tick);
            } else if (event.type == BmsEventType::LoopEnd) {
                loopEndTicks.insert(event.tick);
            }
        }
        for (const std::uint32_t tick : loopStartTicks) {
            midiTrack.events.push_back({tick, {static_cast<std::uint8_t>(0xB0 | channel), 116, 0}});
        }
        for (const std::uint32_t tick : loopEndTicks) {
            midiTrack.events.push_back({tick, {static_cast<std::uint8_t>(0xB0 | channel), 117, 0}});
        }

        for (const BmsEvent& event : bmsTrack.events) {
            if (event.type == BmsEventType::Note) {
                const std::uint8_t note = event.command == CMD_NOTE ? (event.arg1 & 0x7F) : (event.arg0 & 0x7F);
                const std::uint8_t velocity = event.arg2 & 0x7F;
                midiTrack.events.push_back({event.tick, {static_cast<std::uint8_t>(0x90 | channel), note, velocity}});
                midiTrack.events.push_back(
                    {event.tick + event.value, {static_cast<std::uint8_t>(0x80 | channel), note, 0}});
            } else if (event.type == BmsEventType::NoteOn) {
                const std::uint8_t noteId = event.arg1 & 0x07;
                activeNotes[noteId] = event;
            } else if (event.type == BmsEventType::NoteOff) {
                const auto found = activeNotes.find(event.arg0 & 0x07);
                if (found != activeNotes.end()) {
                    const BmsEvent& start = found->second;
                    const std::uint8_t note = start.arg0 & 0x7F;
                    const std::uint8_t velocity = start.arg2 & 0x7F;
                    midiTrack.events.push_back({start.tick, {static_cast<std::uint8_t>(0x90 | channel), note, velocity}});
                    midiTrack.events.push_back({event.tick, {static_cast<std::uint8_t>(0x80 | channel), note, 0}});
                    activeNotes.erase(found);
                }
            }
        }

        midi.tracks.push_back(std::move(midiTrack));
        channel = static_cast<std::uint8_t>((channel + 1) % 16);
    }

    return midi;
}

std::vector<std::uint8_t> serializeMidi(const MidiFile& midi) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    appendU32Be(out, 6);
    appendU16Be(out, 1);
    appendU16Be(out, static_cast<std::uint16_t>(midi.tracks.size()));
    appendU16Be(out, midi.ticksPerQuarter);

    for (const MidiTrack& track : midi.tracks) {
        const std::vector<std::uint8_t> trackBytes = serializeMidiTrack(track);
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        appendU32Be(out, static_cast<std::uint32_t>(trackBytes.size()));
        out.insert(out.end(), trackBytes.begin(), trackBytes.end());
    }
    return out;
}

MidiFile parseMidi(const std::vector<std::uint8_t>& midiData) {
    if (midiData.size() < 14 || midiData[0] != 'M' || midiData[1] != 'T' || midiData[2] != 'h'
        || midiData[3] != 'd') {
        throw std::runtime_error("Invalid MIDI header");
    }

    const std::uint32_t headerSize = readU32Be(midiData, 4);
    if (headerSize < 6 || 8 + headerSize > midiData.size()) {
        throw std::runtime_error("Invalid MIDI header size");
    }
    const std::uint16_t trackCount = readU16Be(midiData, 10);

    MidiFile midi;
    midi.ticksPerQuarter = readU16Be(midiData, 12);

    std::size_t pos = 8 + headerSize;
    for (std::uint16_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        if (pos + 8 > midiData.size() || midiData[pos] != 'M' || midiData[pos + 1] != 'T'
            || midiData[pos + 2] != 'r' || midiData[pos + 3] != 'k') {
            throw std::runtime_error("Invalid MIDI track header");
        }

        const std::uint32_t trackSize = readU32Be(midiData, pos + 4);
        pos += 8;
        const std::size_t trackEnd = pos + trackSize;
        if (trackEnd > midiData.size()) {
            throw std::runtime_error("Truncated MIDI track");
        }

        MidiTrack track;
        std::uint32_t tick = 0;
        std::uint8_t runningStatus = 0;
        while (pos < trackEnd) {
            std::size_t deltaBytes = 0;
            tick += decodeVlq(midiData, pos, deltaBytes);
            pos += deltaBytes;
            if (pos >= trackEnd) {
                break;
            }

            std::uint8_t status = midiData[pos++];
            if (status < 0x80) {
                if (runningStatus == 0) {
                    throw std::runtime_error("MIDI running status without status byte");
                }
                --pos;
                status = runningStatus;
            } else if (status < 0xF0) {
                runningStatus = status;
            }

            MidiEvent event;
            event.tick = tick;
            if (status == 0xFF) {
                requireBytes(midiData, pos, 1);
                const std::uint8_t type = midiData[pos++];
                std::size_t lenBytes = 0;
                const std::uint32_t len = decodeVlq(midiData, pos, lenBytes);
                pos += lenBytes;
                requireBytes(midiData, pos, len);
                event.bytes = {0xFF, type};
                const std::vector<std::uint8_t> lenEncoded = encodeVlq(len);
                event.bytes.insert(event.bytes.end(), lenEncoded.begin(), lenEncoded.end());
                event.bytes.insert(event.bytes.end(), midiData.begin() + static_cast<std::ptrdiff_t>(pos),
                                   midiData.begin() + static_cast<std::ptrdiff_t>(pos + len));
                pos += len;
                track.events.push_back(std::move(event));
                if (type == 0x2F) {
                    break;
                }
            } else if (status == 0xF0 || status == 0xF7) {
                std::size_t lenBytes = 0;
                const std::uint32_t len = decodeVlq(midiData, pos, lenBytes);
                pos += lenBytes;
                requireBytes(midiData, pos, len);
                event.bytes = {status};
                const std::vector<std::uint8_t> lenEncoded = encodeVlq(len);
                event.bytes.insert(event.bytes.end(), lenEncoded.begin(), lenEncoded.end());
                event.bytes.insert(event.bytes.end(), midiData.begin() + static_cast<std::ptrdiff_t>(pos),
                                   midiData.begin() + static_cast<std::ptrdiff_t>(pos + len));
                pos += len;
                track.events.push_back(std::move(event));
            } else {
                const std::uint8_t high = status & 0xF0;
                const std::size_t dataBytes = (high == 0xC0 || high == 0xD0) ? 1 : 2;
                requireBytes(midiData, pos, dataBytes);
                event.bytes.push_back(status);
                event.bytes.insert(event.bytes.end(), midiData.begin() + static_cast<std::ptrdiff_t>(pos),
                                   midiData.begin() + static_cast<std::ptrdiff_t>(pos + dataBytes));
                pos += dataBytes;
                track.events.push_back(std::move(event));
            }
        }

        pos = trackEnd;
        midi.tracks.push_back(std::move(track));
    }

    return midi;
}

std::vector<std::uint8_t> midiToBms(const MidiFile& midi, MidiToBmsOptions options) {
    std::array<MidiChannelData, 16> channels{};
    std::map<std::pair<std::uint8_t, std::uint8_t>, MidiNote> activeNotes;
    std::uint16_t bpm = 120;

    for (const MidiTrack& track : midi.tracks) {
        for (const MidiEvent& event : track.events) {
            if (event.bytes.empty()) {
                continue;
            }
            if (isTempoMeta(event.bytes)) {
                bpm = bpmFromTempoMeta(event.bytes);
                continue;
            }
            if (event.bytes[0] == 0xFF) {
                continue;
            }

            const std::uint8_t status = event.bytes[0] & 0xF0;
            const std::uint8_t channel = event.bytes[0] & 0x0F;
            if (status == 0xB0 && event.bytes.size() >= 3) {
                if (event.bytes[1] == 0) {
                    channels[channel].bank = event.bytes[2];
                } else if (event.bytes[1] == 7) {
                    channels[channel].volume = event.bytes[2] & 0x7F;
                } else if (event.bytes[1] == 10) {
                    channels[channel].pan = event.bytes[2] & 0x7F;
                    channels[channel].hasPan = true;
                } else if (event.bytes[1] == 91) {
                    channels[channel].reverb = event.bytes[2] & 0x7F;
                    channels[channel].hasReverb = true;
                } else if (event.bytes[1] == 116) {
                    channels[channel].loopStart = event.tick;
                    channels[channel].hasLoopStart = true;
                } else if (event.bytes[1] == 117) {
                    channels[channel].loopEnd = event.tick;
                    channels[channel].hasLoopEnd = true;
                }
            } else if (status == 0xC0 && event.bytes.size() >= 2) {
                channels[channel].program = event.bytes[1];
            } else if (status == 0x90 && event.bytes.size() >= 3 && event.bytes[2] > 0) {
                activeNotes[{channel, event.bytes[1]}] = {event.tick, event.tick + 1, event.bytes[1], event.bytes[2]};
            } else if ((status == 0x80 || (status == 0x90 && event.bytes.size() >= 3 && event.bytes[2] == 0))
                       && event.bytes.size() >= 3) {
                const auto key = std::make_pair(channel, event.bytes[1]);
                const auto found = activeNotes.find(key);
                if (found != activeNotes.end()) {
                    MidiNote note = found->second;
                    note.end = std::max<std::uint32_t>(event.tick, note.start + 1);
                    channels[channel].notes.push_back(note);
                    activeNotes.erase(found);
                }
            }
        }
    }

    if (options.enableLoops && options.loopStart >= 0 && options.loopEnd > options.loopStart) {
        for (MidiChannelData& channel : channels) {
            if (channel.notes.empty()) continue;
            channel.loopStart = static_cast<std::uint32_t>(options.loopStart);
            channel.loopEnd = static_cast<std::uint32_t>(options.loopEnd);
            channel.hasLoopStart = true;
            channel.hasLoopEnd = true;
        }
    }

    std::vector<std::vector<std::uint8_t>> childTracks;
    childTracks.push_back(buildSilentTrack());
    for (std::size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
        MidiChannelData& channel = channels[channelIndex];
        bool drum = options.drumChannels.count(static_cast<int>(channelIndex)) != 0;
        const auto overrideIt = options.channelOverrides.find(static_cast<int>(channelIndex));
        if (overrideIt != options.channelOverrides.end()) {
            const MidiToBmsOptions::ChannelOverride& edit = overrideIt->second;
            if (edit.mute) continue;
            if (edit.bank >= 0 || edit.program >= 0) {
                const std::uint8_t gameBank = edit.bank >= 0
                    ? static_cast<std::uint8_t>(edit.bank)
                    : gameBankForMidiBank(channel.bank);
                const std::uint8_t gameProgram = edit.program >= 0
                    ? static_cast<std::uint8_t>(edit.program)
                    : gameProgramForMidi(channel.bank, channel.program);
                channel.bank = midiBankForGameBank(gameBank, gameProgram);
                channel.program = midiProgramForGameProgram(gameBank, gameProgram);
            }
            if (edit.volume >= 0) channel.volume = static_cast<std::uint8_t>(edit.volume & 0x7F);
            if (edit.pan >= 0) {
                channel.pan = static_cast<std::uint8_t>(edit.pan & 0x7F);
                channel.hasPan = true;
            }
            if (edit.reverb >= 0) {
                channel.reverb = static_cast<std::uint8_t>(edit.reverb & 0x7F);
                channel.hasReverb = true;
            }
            if (edit.drumSet) drum = edit.drum;
        }
        if (!channel.notes.empty()) {
            childTracks.push_back(buildMidiChildTrack(channel, options.enableLoops, drum));
        }
    }

    const std::uint32_t rootSize = 4 + 3 + 3 + static_cast<std::uint32_t>(childTracks.size()) * 5 + 3 + 2 + 1;
    std::vector<std::uint32_t> offsets;
    std::uint32_t offset = rootSize;
    for (const std::vector<std::uint8_t>& child : childTracks) {
        offsets.push_back(offset);
        offset += static_cast<std::uint32_t>(child.size());
    }

    std::vector<std::uint8_t> out;
    out.insert(out.end(), {0xD8, 0x62});
    appendU16Be(out, midi.ticksPerQuarter);
    out.push_back(CMD_TEMPO);
    appendU16Be(out, bpm);
    out.insert(out.end(), {0xB8, 0x00, static_cast<std::uint8_t>(std::max(0, std::min(127, options.masterVol)))});
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        out.push_back(CMD_OPEN);
        out.push_back(static_cast<std::uint8_t>(i & 0xFF));
        appendU24Be(out, offsets[i]);
    }
    out.insert(out.end(), {CMD_LOOP_S, 0x00, 0x00, CMD_WAIT_B, 0xFF, CMD_LOOP_E});

    for (const std::vector<std::uint8_t>& child : childTracks) {
        out.insert(out.end(), child.begin(), child.end());
    }
    return out;
}

std::vector<MidiChannelSummary> summarizeMidiChannels(const MidiFile& midi) {
    std::array<MidiChannelSummary, 16> rows{};
    std::array<bool, 16> used{};
    for (int i = 0; i < 16; ++i) rows[i].channel = i;

    for (const MidiTrack& track : midi.tracks) {
        for (const MidiEvent& event : track.events) {
            if (event.bytes.empty() || event.bytes[0] >= 0xF0) continue;
            const int channel = event.bytes[0] & 0x0F;
            const std::uint8_t type = event.bytes[0] & 0xF0;
            MidiChannelSummary& row = rows[channel];
            if (type == 0xB0 && event.bytes.size() >= 3) {
                if (event.bytes[1] == 0) row.midiBank = event.bytes[2];
                else if (event.bytes[1] == 7) row.volume = event.bytes[2] & 0x7F;
                else if (event.bytes[1] == 10) row.pan = event.bytes[2] & 0x7F;
                else if (event.bytes[1] == 91) row.reverb = event.bytes[2] & 0x7F;
            } else if (type == 0xC0 && event.bytes.size() >= 2) {
                row.midiProgram = event.bytes[1];
            } else if (type == 0x90 && event.bytes.size() >= 3 && event.bytes[2] > 0) {
                ++row.noteCount;
                used[channel] = true;
            }
        }
    }

    std::vector<MidiChannelSummary> result;
    for (int i = 0; i < 16; ++i) {
        if (!used[i]) continue;
        rows[i].gameBank = gameBankForMidiBank(static_cast<std::uint8_t>(rows[i].midiBank));
        rows[i].gameProgram = gameProgramForMidi(
            static_cast<std::uint8_t>(rows[i].midiBank),
            static_cast<std::uint8_t>(rows[i].midiProgram));
        result.push_back(rows[i]);
    }
    return result;
}

}  // namespace tpcm
