#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tpcm {

// Read/write access to a GameCube ISO via its File System Table (FST).
// Files are looked up by filename only (not full path); TP audio files
// (Z2SoundSeqs.arc, Z2BgmWave_*.aw, Z2Sound.baa) have unique names in the FST.
//
// replaceFile: in-place if new size <= original; appends to end of ISO otherwise.
class IsoFileSystem {
public:
    static IsoFileSystem open(const std::string& isoPath);

    bool hasFile(const std::string& name) const;
    std::vector<uint8_t> readFile(const std::string& name) const;
    void replaceFile(const std::string& name, const std::vector<uint8_t>& newData);

    // Add a brand-new wave bank beside Z2BgmWave_89.aw in the FST, relocating
    // the expanded FST to the end of the ISO.
    void addFile(const std::string& name, const std::vector<uint8_t>& data);

    // Convenience: replace if file exists, add if it doesn't.
    void writeFile(const std::string& name, const std::vector<uint8_t>& data);

    std::string isoPath() const { return m_path; }

private:
    struct Entry {
        bool     isDir = false;
        uint32_t dataOffset = 0;  // file: absolute ISO byte offset; dir: parent index
        uint32_t size = 0;        // file: byte count; dir: past-end entry index
        uint32_t fstIndex = 0;
        std::string name;
    };

    void parseFst();
    void patchFstEntry(uint32_t fstIndex, uint32_t newOffset, uint32_t newSize);
    uint64_t appendOffset() const;

    std::string m_path;
    uint32_t    m_fstOffset = 0;
    uint32_t    m_fstSize = 0;
    std::vector<Entry> m_entries;
};

}  // namespace tpcm
