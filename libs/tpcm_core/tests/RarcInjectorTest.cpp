#include "tpcm/RarcInjector.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void writeU16(std::vector<std::uint8_t>& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
}

void writeU32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    data[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint32_t>((data[offset] << 24) | (data[offset + 1] << 16)
                                      | (data[offset + 2] << 8) | data[offset + 3]);
}

std::vector<std::uint8_t> makeArchive() {
    std::vector<std::uint8_t> data(0xC4, 0);
    data[0] = 'R';
    data[1] = 'A';
    data[2] = 'R';
    data[3] = 'C';
    writeU32(data, 0x04, static_cast<std::uint32_t>(data.size()));
    writeU32(data, 0x08, 0x20);
    writeU32(data, 0x0C, 0x80);
    writeU32(data, 0x10, 0x24);

    writeU32(data, 0x20, 0);
    writeU32(data, 0x24, 0);
    writeU32(data, 0x28, 2);
    writeU32(data, 0x2C, 0x20);
    writeU32(data, 0x30, 0x0C);
    writeU32(data, 0x34, 0x60);

    writeU16(data, 0x44, 0x11);
    writeU16(data, 0x46, 0);
    writeU32(data, 0x48, 0);
    writeU32(data, 0x4C, 4);

    writeU16(data, 0x58, 0x11);
    writeU16(data, 0x5A, 6);
    writeU32(data, 0x5C, 0x20);
    writeU32(data, 0x60, 4);

    const std::vector<std::uint8_t> names = {'a', '.', 'b', 'm', 's', 0, 'b', '.', 'b', 'm', 's', 0};
    for (std::size_t i = 0; i < names.size(); ++i) {
        data[0x80 + i] = static_cast<std::uint8_t>(names[i]);
    }

    data[0xA0] = 'A';
    data[0xA1] = 'A';
    data[0xA2] = 'A';
    data[0xA3] = 'A';
    data[0xC0] = 'B';
    data[0xC1] = 'B';
    data[0xC2] = 'B';
    data[0xC3] = 'B';
    return data;
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> source = {'A', 'B', 'A', 'B', 'A', 'B', 'A', 'B',
                                             'A', 'B', 'A', 'B', 'A', 'B', 'A', 'B'};
    const std::vector<std::uint8_t> compressed = tpcm::yaz0Compress(source);
    assert(compressed.size() >= 16);
    assert(compressed[0] == 'Y');
    assert(compressed[1] == 'a');
    assert(compressed[2] == 'z');
    assert(compressed[3] == '0');
    assert(tpcm::yaz0Decompress(compressed) == source);

    tpcm::RarcArchive archive(makeArchive());
    const std::vector<std::string> files = archive.listFiles();
    assert(files.size() == 2);
    assert(files[0] == "a.bms");
    assert(files[1] == "b.bms");
    assert(archive.getFile("a.bms") == std::vector<std::uint8_t>({'A', 'A', 'A', 'A'}));
    assert(archive.getFile("b.bms") == std::vector<std::uint8_t>({'B', 'B', 'B', 'B'}));

    archive.replaceFile("a.bms", std::vector<std::uint8_t>(40, 0x5A));
    assert(archive.getFile("a.bms") == std::vector<std::uint8_t>(40, 0x5A));
    assert(archive.getFile("b.bms") == std::vector<std::uint8_t>({'B', 'B', 'B', 'B'}));

    const std::vector<std::uint8_t>& bytes = archive.toBytes();
    assert(readU32(bytes, 0x04) == bytes.size());
    assert(readU32(bytes, 0x5C) == 0x40);
    assert(readU32(bytes, 0x4C) == 40);

    archive.replaceFile("a.bms", std::vector<std::uint8_t>({'C', 'C', 'C'}));
    assert(archive.getFile("a.bms") == std::vector<std::uint8_t>({'C', 'C', 'C'}));
    assert(archive.getFile("b.bms") == std::vector<std::uint8_t>({'B', 'B', 'B', 'B'}));
    return 0;
}
