#include "tpcm/IsoFileSystem.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace tpcm {
namespace {

uint32_t readU32Be(const std::vector<uint8_t>& v, size_t off) {
    return (uint32_t(v[off]) << 24) | (uint32_t(v[off + 1]) << 16) |
           (uint32_t(v[off + 2]) << 8) | uint32_t(v[off + 3]);
}
uint32_t readU24Be(const std::vector<uint8_t>& v, size_t off) {
    return (uint32_t(v[off]) << 16) | (uint32_t(v[off + 1]) << 8) | uint32_t(v[off + 2]);
}
void writeU32Be(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off]     = uint8_t((x >> 24) & 0xFF);
    v[off + 1] = uint8_t((x >> 16) & 0xFF);
    v[off + 2] = uint8_t((x >> 8) & 0xFF);
    v[off + 3] = uint8_t(x & 0xFF);
}

std::vector<uint8_t> readAt(const std::string& path, uint64_t offset, size_t size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open ISO: " + path);
    f.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    if (!f) throw std::runtime_error("ISO read failed at offset " + std::to_string(offset));
    return buf;
}

// Write data into the ISO at the given absolute offset.
// If offset is past the current end of file the gap is zero-filled automatically
// on MSVC (seekp past EOF in in|out mode extends the file).
void writeAt(const std::string& path, uint64_t offset, const std::vector<uint8_t>& data) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) throw std::runtime_error("Cannot open ISO for writing: " + path);
    f.seekp(static_cast<std::streamoff>(offset));
    if (!f) throw std::runtime_error("ISO seekp failed at offset " + std::to_string(offset));
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!f) throw std::runtime_error("ISO write failed at offset " + std::to_string(offset));
}

}  // namespace

IsoFileSystem IsoFileSystem::open(const std::string& path) {
    IsoFileSystem fs;
    fs.m_path = path;
    // Boot.bin: FST offset at 0x424, FST size at 0x428 (both u32 BE)
    const auto hdr = readAt(path, 0x424, 8);
    fs.m_fstOffset = readU32Be(hdr, 0);
    fs.m_fstSize   = readU32Be(hdr, 4);
    fs.parseFst();
    return fs;
}

void IsoFileSystem::parseFst() {
    const auto fst = readAt(m_path, m_fstOffset, m_fstSize);

    // Root entry (index 0): size field = total entry count
    const uint32_t totalEntries = readU32Be(fst, 8);
    const size_t strBase = static_cast<size_t>(totalEntries) * 12;

    m_entries.clear();
    m_entries.reserve(totalEntries);

    for (uint32_t i = 0; i < totalEntries; ++i) {
        const size_t eOff = i * 12;
        Entry e;
        e.fstIndex   = i;
        e.isDir      = (fst[eOff] != 0);
        const uint32_t nameOff = readU24Be(fst, eOff + 1);
        e.dataOffset = readU32Be(fst, eOff + 4);
        e.size       = readU32Be(fst, eOff + 8);
        // Read null-terminated name from string table
        for (size_t ns = strBase + nameOff; ns < fst.size() && fst[ns]; ++ns)
            e.name.push_back(static_cast<char>(fst[ns]));
        m_entries.push_back(std::move(e));
    }
}

std::vector<uint8_t> IsoFileSystem::readFile(const std::string& name) const {
    for (const Entry& e : m_entries) {
        if (!e.isDir && e.name == name)
            return readAt(m_path, e.dataOffset, e.size);
    }
    throw std::runtime_error("ISO file not found: " + name);
}

void IsoFileSystem::replaceFile(const std::string& name, const std::vector<uint8_t>& newData) {
    for (Entry& e : m_entries) {
        if (e.isDir || e.name != name) continue;

        if (newData.size() <= e.size) {
            // In-place: write data zero-padded to original slot size
            std::vector<uint8_t> padded(e.size, 0);
            std::copy(newData.begin(), newData.end(), padded.begin());
            writeAt(m_path, e.dataOffset, padded);
            // Update FST size field only (offset unchanged)
            patchFstEntry(e.fstIndex, e.dataOffset, static_cast<uint32_t>(newData.size()));
            e.size = static_cast<uint32_t>(newData.size());
        } else {
            // Expand: append to end, update FST offset + size
            const uint64_t rawEnd    = appendOffset();
            const uint64_t aligned   = (rawEnd + 3) & ~uint64_t(3);
            const uint32_t newOffset = static_cast<uint32_t>(aligned);

            // Write alignment padding then the new data
            if (aligned > rawEnd) {
                std::vector<uint8_t> pad(static_cast<size_t>(aligned - rawEnd), 0);
                writeAt(m_path, rawEnd, pad);
            }
            writeAt(m_path, aligned, newData);

            patchFstEntry(e.fstIndex, newOffset, static_cast<uint32_t>(newData.size()));
            e.dataOffset = newOffset;
            e.size       = static_cast<uint32_t>(newData.size());
        }
        return;
    }
    throw std::runtime_error("ISO file not found: " + name);
}

bool IsoFileSystem::hasFile(const std::string& name) const {
    for (const Entry& e : m_entries)
        if (!e.isDir && e.name == name) return true;
    return false;
}

void IsoFileSystem::writeFile(const std::string& name, const std::vector<uint8_t>& data) {
    if (hasFile(name)) replaceFile(name, data);
    else               addFile(name, data);
}

void IsoFileSystem::addFile(const std::string& name, const std::vector<uint8_t>& data) {
    const auto fst = readAt(m_path, m_fstOffset, m_fstSize);
    const uint32_t curCount      = readU32Be(fst, 8);
    const size_t   strBase       = curCount * 12;
    const uint32_t curStrTabSize = static_cast<uint32_t>(fst.size() - strBase);

    std::vector<uint8_t> nameBytes(name.begin(), name.end());
    nameBytes.push_back(0);
    const uint32_t newFstSize = static_cast<uint32_t>(fst.size()) + 12 +
                                static_cast<uint32_t>(nameBytes.size());

    // Directory membership is encoded by FST entry ordering. Insert new wave
    // banks at the end of the directory containing the existing final bank.
    auto sibling = std::find_if(m_entries.begin(), m_entries.end(), [](const Entry& e) {
        return !e.isDir && e.name == "Z2BgmWave_89.aw";
    });
    if (sibling == m_entries.end())
        throw std::runtime_error("Cannot add wave bank: Z2BgmWave_89.aw not found");

    const Entry* parent = nullptr;
    for (const Entry& e : m_entries) {
        if (!e.isDir || e.fstIndex >= sibling->fstIndex || sibling->fstIndex >= e.size)
            continue;
        if (!parent || (e.size - e.fstIndex) < (parent->size - parent->fstIndex))
            parent = &e;
    }
    if (!parent)
        throw std::runtime_error("Cannot add wave bank: parent directory not found");
    const uint32_t insertIndex = parent->size;

    const uint64_t rawEnd     = appendOffset();
    const uint32_t awOff      = static_cast<uint32_t>((rawEnd + 3) & ~uint64_t(3));
    if (awOff > rawEnd) {
        std::vector<uint8_t> pad(static_cast<size_t>(awOff - rawEnd), 0);
        writeAt(m_path, rawEnd, pad);
    }
    writeAt(m_path, awOff, data);

    // Build new FST entry (nameOff relative to string table start)
    std::vector<uint8_t> newEntry(12, 0);
    newEntry[1] = (curStrTabSize >> 16) & 0xFF;
    newEntry[2] = (curStrTabSize >> 8)  & 0xFF;
    newEntry[3] =  curStrTabSize        & 0xFF;
    newEntry[4] = (awOff >> 24) & 0xFF; newEntry[5] = (awOff >> 16) & 0xFF;
    newEntry[6] = (awOff >> 8)  & 0xFF; newEntry[7] =  awOff        & 0xFF;
    const uint32_t sz = static_cast<uint32_t>(data.size());
    newEntry[8]  = (sz >> 24) & 0xFF; newEntry[9]  = (sz >> 16) & 0xFF;
    newEntry[10] = (sz >> 8)  & 0xFF; newEntry[11] =  sz        & 0xFF;

    // Inserting an entry shifts later directory indices. Repair both directory
    // parent indices and past-the-end indices before assembling the new FST.
    std::vector<uint8_t> entries(fst.begin(), fst.begin() + static_cast<ptrdiff_t>(strBase));
    for (uint32_t i = 0; i < curCount; ++i) {
        const size_t entryOff = static_cast<size_t>(i) * 12;
        if (entries[entryOff] == 0) continue;
        const uint32_t parentIndex = readU32Be(entries, entryOff + 4);
        const uint32_t nextIndex = readU32Be(entries, entryOff + 8);
        if (parentIndex >= insertIndex)
            writeU32Be(entries, entryOff + 4, parentIndex + 1);
        if (nextIndex >= insertIndex)
            writeU32Be(entries, entryOff + 8, nextIndex + 1);
    }

    std::vector<uint8_t> newFst;
    newFst.reserve(newFstSize);
    const size_t insertionByte = static_cast<size_t>(insertIndex) * 12;
    newFst.insert(newFst.end(), entries.begin(),
                  entries.begin() + static_cast<ptrdiff_t>(insertionByte));
    newFst.insert(newFst.end(), newEntry.begin(), newEntry.end());
    newFst.insert(newFst.end(), entries.begin() + static_cast<ptrdiff_t>(insertionByte),
                  entries.end());
    newFst.insert(newFst.end(), fst.begin() + static_cast<ptrdiff_t>(strBase), fst.end());
    newFst.insert(newFst.end(), nameBytes.begin(), nameBytes.end());
    const uint32_t newCount = curCount + 1;
    newFst[8]  = (newCount >> 24) & 0xFF; newFst[9]  = (newCount >> 16) & 0xFF;
    newFst[10] = (newCount >> 8)  & 0xFF; newFst[11] =  newCount        & 0xFF;

    // Relocate the expanded FST after the new file, matching the Python
    // injector and avoiding assumptions about unused space after the old FST.
    const uint64_t afterAw = static_cast<uint64_t>(awOff) + data.size();
    const uint32_t newFstOffset = static_cast<uint32_t>((afterAw + 3) & ~uint64_t(3));
    if (newFstOffset > afterAw) {
        std::vector<uint8_t> pad(static_cast<size_t>(newFstOffset - afterAw), 0);
        writeAt(m_path, afterAw, pad);
    }

    writeAt(m_path, newFstOffset, newFst);

    // Update boot header: FST offset (0x424), FST size (0x428), max FST size (0x42C)
    std::vector<uint8_t> hdr(12);
    writeU32Be(hdr, 0, newFstOffset);
    writeU32Be(hdr, 4, newFstSize);
    writeU32Be(hdr, 8, newFstSize);
    writeAt(m_path, 0x424, hdr);

    m_fstOffset = newFstOffset;
    m_fstSize   = newFstSize;
    parseFst();
}

void IsoFileSystem::patchFstEntry(uint32_t fstIndex, uint32_t newOffset, uint32_t newSize) {
    // Re-read FST, patch offset and size for the given entry, write back
    auto fst = readAt(m_path, m_fstOffset, m_fstSize);
    writeU32Be(fst, fstIndex * 12 + 4, newOffset);
    writeU32Be(fst, fstIndex * 12 + 8, newSize);
    writeAt(m_path, m_fstOffset, fst);
}

uint64_t IsoFileSystem::appendOffset() const {
    uint64_t end = std::filesystem::file_size(m_path);
    end = std::max(end, static_cast<uint64_t>(m_fstOffset) + m_fstSize);
    for (const Entry& e : m_entries) {
        if (!e.isDir) {
            const uint64_t fileEnd = uint64_t(e.dataOffset) + e.size;
            if (fileEnd > end) end = fileEnd;
        }
    }
    return end;
}

}  // namespace tpcm
