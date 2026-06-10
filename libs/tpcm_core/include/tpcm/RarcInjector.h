#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpcm {

std::vector<std::uint8_t> yaz0Decompress(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> yaz0Compress(const std::vector<std::uint8_t>& data);

class RarcArchive {
public:
    explicit RarcArchive(std::vector<std::uint8_t> data);

    std::vector<std::string> listFiles() const;
    std::vector<std::uint8_t> getFile(const std::string& target) const;
    void replaceFile(const std::string& target, const std::vector<std::uint8_t>& newData);
    const std::vector<std::uint8_t>& toBytes() const;

private:
    struct Entry {
        std::size_t offset = 0;
        std::uint16_t flags = 0;
        std::uint16_t nameOffset = 0;
        std::uint32_t dataOffset = 0;
        std::uint32_t size = 0;
        std::string name;
    };

    std::vector<Entry> entries() const;
    std::size_t fileDataStart() const;
    std::size_t stringTableStart() const;
    std::size_t fileEntryStart() const;
    std::uint32_t fileEntryCount() const;
    void writeU32(std::size_t offset, std::uint32_t value);

    std::vector<std::uint8_t> m_data;
};

}  // namespace tpcm
