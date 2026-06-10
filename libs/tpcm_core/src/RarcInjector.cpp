#include "tpcm/RarcInjector.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace tpcm {
namespace {

constexpr std::size_t kRarcInfoOffset = 0x20;
constexpr std::size_t kRarcEntrySize = 0x14;

std::uint32_t readU32Be(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("Truncated u32");
    }
    return static_cast<std::uint32_t>((data[offset] << 24) | (data[offset + 1] << 16)
                                      | (data[offset + 2] << 8) | data[offset + 3]);
}

std::uint16_t readU16Be(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        throw std::runtime_error("Truncated u16");
    }
    return static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
}

void writeU32Be(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("Cannot write u32 outside buffer");
    }
    data[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

bool startsWithYaz0(const std::vector<std::uint8_t>& data) {
    return data.size() >= 4 && data[0] == 'Y' && data[1] == 'a' && data[2] == 'z' && data[3] == '0';
}

bool startsWithRarc(const std::vector<std::uint8_t>& data) {
    return data.size() >= 4 && data[0] == 'R' && data[1] == 'A' && data[2] == 'R' && data[3] == 'C';
}

std::size_t align32(std::size_t value) {
    return (value + 31u) & ~static_cast<std::size_t>(31u);
}

std::string readCString(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("RARC string offset outside archive");
    }
    std::string value;
    while (offset < data.size() && data[offset] != 0) {
        value.push_back(static_cast<char>(data[offset++]));
    }
    return value;
}

bool endsWithBms(const std::string& name) {
    return name.size() >= 4 && name.substr(name.size() - 4) == ".bms";
}

}  // namespace

std::vector<std::uint8_t> yaz0Decompress(const std::vector<std::uint8_t>& data) {
    if (!startsWithYaz0(data) || data.size() < 16) {
        throw std::runtime_error("Invalid Yaz0 stream");
    }

    const std::uint32_t outputSize = readU32Be(data, 4);
    std::vector<std::uint8_t> out;
    out.reserve(outputSize);

    std::size_t pos = 16;
    std::uint8_t code = 0;
    int bitsLeft = 0;
    while (out.size() < outputSize) {
        if (bitsLeft == 0) {
            if (pos >= data.size()) {
                throw std::runtime_error("Truncated Yaz0 code byte");
            }
            code = data[pos++];
            bitsLeft = 8;
        }

        if ((code & 0x80) != 0) {
            if (pos >= data.size()) {
                throw std::runtime_error("Truncated Yaz0 literal");
            }
            out.push_back(data[pos++]);
        } else {
            if (pos + 2 > data.size()) {
                throw std::runtime_error("Truncated Yaz0 back-reference");
            }
            const std::uint8_t byte1 = data[pos++];
            const std::uint8_t byte2 = data[pos++];
            std::uint32_t count = byte1 >> 4;
            const std::uint32_t distance = static_cast<std::uint32_t>(((byte1 & 0x0F) << 8) | byte2) + 1;
            if (distance > out.size()) {
                throw std::runtime_error("Invalid Yaz0 back-reference distance");
            }
            if (count == 0) {
                if (pos >= data.size()) {
                    throw std::runtime_error("Truncated Yaz0 extended length");
                }
                count = static_cast<std::uint32_t>(data[pos++]) + 0x12;
            } else {
                count += 2;
            }
            for (std::uint32_t i = 0; i < count && out.size() < outputSize; ++i) {
                out.push_back(out[out.size() - distance]);
            }
        }

        code <<= 1;
        --bitsLeft;
    }

    return out;
}

std::vector<std::uint8_t> yaz0Compress(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'Y', 'a', 'z', '0'});
    out.push_back(static_cast<std::uint8_t>((data.size() >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((data.size() >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((data.size() >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(data.size() & 0xFF));
    out.resize(16, 0);

    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t codeOffset = out.size();
        out.push_back(0);
        std::uint8_t code = 0;

        for (int bit = 0; bit < 8 && pos < data.size(); ++bit) {
            std::size_t bestLength = 0;
            std::size_t bestDistance = 0;
            const std::size_t windowStart = pos > 0x1000 ? pos - 0x1000 : 0;
            for (std::size_t candidate = windowStart; candidate < pos; ++candidate) {
                std::size_t length = 0;
                while (length < 273 && pos + length < data.size() && data[candidate + length] == data[pos + length]) {
                    ++length;
                    if (candidate + length >= pos) {
                        break;
                    }
                }
                if (length > bestLength && length >= 3) {
                    bestLength = length;
                    bestDistance = pos - candidate;
                }
            }

            if (bestLength >= 3) {
                const std::size_t disp = bestDistance - 1;
                if (bestLength >= 0x12) {
                    out.push_back(static_cast<std::uint8_t>((disp >> 8) & 0x0F));
                    out.push_back(static_cast<std::uint8_t>(disp & 0xFF));
                    out.push_back(static_cast<std::uint8_t>(bestLength - 0x12));
                } else {
                    out.push_back(static_cast<std::uint8_t>(((bestLength - 2) << 4) | ((disp >> 8) & 0x0F)));
                    out.push_back(static_cast<std::uint8_t>(disp & 0xFF));
                }
                pos += bestLength;
            } else {
                code |= static_cast<std::uint8_t>(0x80 >> bit);
                out.push_back(data[pos++]);
            }
        }

        out[codeOffset] = code;
    }

    return out;
}

RarcArchive::RarcArchive(std::vector<std::uint8_t> data) : m_data(std::move(data)) {
    if (!startsWithRarc(m_data) || m_data.size() < 0x40) {
        throw std::runtime_error("Invalid RARC archive");
    }
}

std::size_t RarcArchive::fileDataStart() const {
    return kRarcInfoOffset + readU32Be(m_data, 0x0C);
}

std::size_t RarcArchive::fileEntryStart() const {
    return kRarcInfoOffset + readU32Be(m_data, 0x2C);
}

std::uint32_t RarcArchive::fileEntryCount() const {
    return readU32Be(m_data, 0x28);
}

std::size_t RarcArchive::stringTableStart() const {
    return kRarcInfoOffset + readU32Be(m_data, 0x34);
}

void RarcArchive::writeU32(std::size_t offset, std::uint32_t value) {
    writeU32Be(m_data, offset, value);
}

std::vector<RarcArchive::Entry> RarcArchive::entries() const {
    const std::size_t entryStart = fileEntryStart();
    const std::size_t stringStart = stringTableStart();
    const std::uint32_t count = fileEntryCount();
    if (entryStart + static_cast<std::size_t>(count) * kRarcEntrySize > m_data.size()) {
        throw std::runtime_error("RARC file entry table is outside archive");
    }

    std::vector<Entry> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t offset = entryStart + static_cast<std::size_t>(i) * kRarcEntrySize;
        Entry entry;
        entry.offset = offset;
        entry.flags = readU16Be(m_data, offset + 4);
        entry.nameOffset = readU16Be(m_data, offset + 6);
        entry.dataOffset = readU32Be(m_data, offset + 8);
        entry.size = readU32Be(m_data, offset + 12);
        entry.name = readCString(m_data, stringStart + entry.nameOffset);
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<std::string> RarcArchive::listFiles() const {
    std::vector<std::string> names;
    for (const Entry& entry : entries()) {
        if (entry.flags == 0x11 && endsWithBms(entry.name)) {
            names.push_back(entry.name);
        }
    }
    return names;
}

std::vector<std::uint8_t> RarcArchive::getFile(const std::string& target) const {
    const std::size_t dataStart = fileDataStart();
    for (const Entry& entry : entries()) {
        if (entry.name == target) {
            const std::size_t begin = dataStart + entry.dataOffset;
            const std::size_t end = begin + entry.size;
            if (end > m_data.size()) {
                throw std::runtime_error("RARC file payload is outside archive");
            }
            return {m_data.begin() + static_cast<std::ptrdiff_t>(begin),
                    m_data.begin() + static_cast<std::ptrdiff_t>(end)};
        }
    }
    throw std::runtime_error("RARC file not found");
}

void RarcArchive::replaceFile(const std::string& target, const std::vector<std::uint8_t>& newData) {
    const std::size_t dataStart = fileDataStart();
    const std::vector<Entry> snapshot = entries();
    const auto found = std::find_if(snapshot.begin(), snapshot.end(), [&](const Entry& entry) {
        return entry.name == target;
    });
    if (found == snapshot.end()) {
        throw std::runtime_error("RARC file not found");
    }

    const std::size_t oldBegin = dataStart + found->dataOffset;
    const std::size_t oldEnd = oldBegin + found->size;
    if (oldEnd > m_data.size()) {
        throw std::runtime_error("RARC file payload is outside archive");
    }

    if (newData.size() <= found->size) {
        std::copy(newData.begin(), newData.end(), m_data.begin() + static_cast<std::ptrdiff_t>(oldBegin));
        std::fill(m_data.begin() + static_cast<std::ptrdiff_t>(oldBegin + newData.size()),
                  m_data.begin() + static_cast<std::ptrdiff_t>(oldEnd), 0);
        writeU32(found->offset + 12, static_cast<std::uint32_t>(newData.size()));
        return;
    }

    const std::size_t oldAlignedEnd = dataStart + align32(found->dataOffset + found->size);
    const std::size_t insertEnd = std::max(oldEnd, oldAlignedEnd);
    const std::size_t newAlignedSize = align32(newData.size());
    std::vector<std::uint8_t> replacement(newAlignedSize, 0);
    std::copy(newData.begin(), newData.end(), replacement.begin());

    m_data.erase(m_data.begin() + static_cast<std::ptrdiff_t>(oldBegin),
                 m_data.begin() + static_cast<std::ptrdiff_t>(insertEnd));
    m_data.insert(m_data.begin() + static_cast<std::ptrdiff_t>(oldBegin), replacement.begin(), replacement.end());

    const std::int64_t delta = static_cast<std::int64_t>(newAlignedSize) - static_cast<std::int64_t>(insertEnd - oldBegin);
    for (const Entry& entry : snapshot) {
        if (entry.offset == found->offset) {
            writeU32(entry.offset + 12, static_cast<std::uint32_t>(newData.size()));
        } else if (entry.dataOffset > found->dataOffset) {
            writeU32(entry.offset + 8, static_cast<std::uint32_t>(static_cast<std::int64_t>(entry.dataOffset) + delta));
        }
    }

    writeU32(0x04, static_cast<std::uint32_t>(m_data.size()));
    writeU32(0x10, static_cast<std::uint32_t>(m_data.size() - dataStart));
}

const std::vector<std::uint8_t>& RarcArchive::toBytes() const {
    return m_data;
}

}  // namespace tpcm
