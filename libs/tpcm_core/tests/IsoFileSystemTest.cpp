#include "tpcm/IsoFileSystem.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

void appendU32Be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8)  & 0xFF); v.push_back(x & 0xFF);
}
void appendU24Be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}
uint32_t readU32Be(const std::vector<uint8_t>& v, size_t off) {
    return (uint32_t(v[off])<<24)|(uint32_t(v[off+1])<<16)|(uint32_t(v[off+2])<<8)|v[off+3];
}

// Build a minimal synthetic GameCube ISO with one file named "test.bms".
//
// Layout:
//   0x000..0x43F  boot area (0x440 bytes)  — FST ptr at 0x424/0x428
//   0x440..0x457  FST entries (2 * 12 = 24 bytes)
//   0x458..0x461  FST string table (10 bytes: '\0' + "test.bms\0")
//   0x462..0x463  alignment padding
//   0x464..0x467  file data for "test.bms" (4 bytes)
//
// FST_OFFSET = 0x440, FST_SIZE = 34, file data at 0x464
//
std::vector<uint8_t> buildSyntheticIso(const std::vector<uint8_t>& fileData) {
    const uint32_t FST_OFFSET  = 0x440;
    const uint32_t NUM_ENTRIES = 2;
    const uint32_t STR_SIZE    = 10; // '\0' + "test.bms\0"
    const uint32_t FST_SIZE    = NUM_ENTRIES * 12 + STR_SIZE; // 34
    const uint32_t FILE_OFFSET = 0x464; // FST_OFFSET + 34 aligned up to 4

    std::vector<uint8_t> iso(FILE_OFFSET + fileData.size(), 0);

    // Boot area: write FST offset and size
    iso[0x424] = (FST_OFFSET >> 24) & 0xFF;
    iso[0x425] = (FST_OFFSET >> 16) & 0xFF;
    iso[0x426] = (FST_OFFSET >> 8)  & 0xFF;
    iso[0x427] =  FST_OFFSET        & 0xFF;
    iso[0x428] = (FST_SIZE >> 24) & 0xFF;
    iso[0x429] = (FST_SIZE >> 16) & 0xFF;
    iso[0x42A] = (FST_SIZE >> 8)  & 0xFF;
    iso[0x42B] =  FST_SIZE        & 0xFF;

    // FST entry 0: root directory (flags=1, nameOff=0, offset=0, size=NUM_ENTRIES)
    // Entry layout: [flags(1)] [nameOff(3)] [dataOffset(4)] [size(4)]
    //               0x440       0x441-443    0x444-447        0x448-44B
    iso[0x440] = 0x01;
    // nameOff[3] = 0 (already zero)
    // dataOffset = 0 (already zero)
    iso[0x44B] = uint8_t(NUM_ENTRIES & 0xFF); // size field low byte = 2 (offset 8-11)

    // FST entry 1: file "test.bms" (flags=0, nameOff=1, offset=FILE_OFFSET, size=4)
    // flags byte at 0x44C = 0x00 (already zero)
    iso[0x44D] = 0x00; iso[0x44E] = 0x00; iso[0x44F] = 0x01; // nameOff=1
    iso[0x450] = (FILE_OFFSET >> 24) & 0xFF;
    iso[0x451] = (FILE_OFFSET >> 16) & 0xFF;
    iso[0x452] = (FILE_OFFSET >> 8)  & 0xFF;
    iso[0x453] =  FILE_OFFSET        & 0xFF;
    iso[0x454] = 0x00; iso[0x455] = 0x00; iso[0x456] = 0x00; iso[0x457] = 0x04; // size=4

    // String table at 0x458: '\0' then "test.bms\0"
    iso[0x458] = 0x00;
    const char* fname = "test.bms";
    for (size_t i = 0; fname[i]; ++i) iso[0x459 + i] = uint8_t(fname[i]);
    // iso[0x461] = 0x00 (terminator, already zero)

    // File data
    for (size_t i = 0; i < fileData.size(); ++i)
        iso[FILE_OFFSET + i] = fileData[i];

    return iso;
}

std::vector<uint8_t> buildWaveDirectoryIso() {
    constexpr uint32_t fstOffset = 0x440;
    constexpr uint32_t bankOffset = 0x600;
    constexpr uint32_t rootFileOffset = 0x604;

    std::vector<uint8_t> fst;
    auto appendEntry = [&](bool isDir, uint32_t nameOff, uint32_t offset, uint32_t size) {
        fst.push_back(isDir ? 1 : 0);
        appendU24Be(fst, nameOff);
        appendU32Be(fst, offset);
        appendU32Be(fst, size);
    };
    // /Audiores/Waves/Z2BgmWave_89.aw followed by a root-level file.
    appendEntry(true, 0, 0, 5);
    appendEntry(true, 1, 0, 4);
    appendEntry(true, 10, 1, 4);
    appendEntry(false, 16, bankOffset, 4);
    appendEntry(false, 32, rootFileOffset, 4);
    const char names[] = "\0Audiores\0Waves\0Z2BgmWave_89.aw\0root.bin\0";
    fst.insert(fst.end(), names, names + sizeof(names));

    std::vector<uint8_t> iso(rootFileOffset + 4, 0);
    iso[0x424] = (fstOffset >> 24) & 0xFF;
    iso[0x425] = (fstOffset >> 16) & 0xFF;
    iso[0x426] = (fstOffset >> 8) & 0xFF;
    iso[0x427] = fstOffset & 0xFF;
    const uint32_t fstSize = static_cast<uint32_t>(fst.size());
    iso[0x428] = (fstSize >> 24) & 0xFF;
    iso[0x429] = (fstSize >> 16) & 0xFF;
    iso[0x42A] = (fstSize >> 8) & 0xFF;
    iso[0x42B] = fstSize & 0xFF;
    std::copy(fst.begin(), fst.end(), iso.begin() + fstOffset);
    std::copy_n("BANK", 4, iso.begin() + bankOffset);
    std::copy_n("ROOT", 4, iso.begin() + rootFileOffset);
    return iso;
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

}  // namespace

int main() {
    const fs::path tmpDir = fs::temp_directory_path() / "tpcm_iso_fs_test";
    fs::create_directories(tmpDir);
    const fs::path isoPath = tmpDir / "test.iso";

    const std::vector<uint8_t> origData = {0xAA, 0xBB, 0xCC, 0xDD};
    writeFile(isoPath, buildSyntheticIso(origData));

    // ============================================================
    // Test 1: readFile finds the file and returns correct bytes
    // ============================================================
    {
        auto iso = tpcm::IsoFileSystem::open(isoPath.string());
        const auto data = iso.readFile("test.bms");
        assert(data == origData);
    }

    // ============================================================
    // Test 2: readFile throws for unknown filename
    // ============================================================
    {
        auto iso = tpcm::IsoFileSystem::open(isoPath.string());
        bool threw = false;
        try { iso.readFile("missing.bms"); } catch (const std::exception&) { threw = true; }
        assert(threw);
    }

    // ============================================================
    // Test 3: replaceFile in-place (new size <= original)
    // ============================================================
    {
        writeFile(isoPath, buildSyntheticIso(origData)); // reset
        auto iso = tpcm::IsoFileSystem::open(isoPath.string());
        const std::vector<uint8_t> smaller = {0x11, 0x22};
        iso.replaceFile("test.bms", smaller);
        // Verify via a fresh IsoFileSystem (re-parses FST from disk)
        auto iso2 = tpcm::IsoFileSystem::open(isoPath.string());
        assert(iso2.readFile("test.bms") == smaller);
    }

    // ============================================================
    // Test 4: replaceFile expands (new size > original) — appends to end
    // ============================================================
    {
        writeFile(isoPath, buildSyntheticIso(origData)); // reset
        auto iso = tpcm::IsoFileSystem::open(isoPath.string());
        const std::vector<uint8_t> larger(16, 0xEE);
        iso.replaceFile("test.bms", larger);
        auto iso2 = tpcm::IsoFileSystem::open(isoPath.string());
        const auto read = iso2.readFile("test.bms");
        assert(read == larger);
    }

    // ============================================================
    // Test 5: addFile inserts the new wave bank inside Waves
    // ============================================================
    {
        writeFile(isoPath, buildWaveDirectoryIso());
        auto iso = tpcm::IsoFileSystem::open(isoPath.string());
        const std::vector<uint8_t> bank90 = {0x90, 0x91, 0x92};
        iso.addFile("Z2BgmWave_90.aw", bank90);

        auto iso2 = tpcm::IsoFileSystem::open(isoPath.string());
        assert(iso2.readFile("Z2BgmWave_90.aw") == bank90);
        assert(iso2.readFile("root.bin") == std::vector<uint8_t>({'R', 'O', 'O', 'T'}));

        const auto bytes = readFile(isoPath);
        const uint32_t newFstOffset = readU32Be(bytes, 0x424);
        const uint32_t newFstSize = readU32Be(bytes, 0x428);
        std::vector<uint8_t> fst(bytes.begin() + newFstOffset,
                                 bytes.begin() + newFstOffset + newFstSize);
        assert(readU32Be(fst, 8) == 6);       // root next index
        assert(readU32Be(fst, 12 + 8) == 5); // Audiores next index
        assert(readU32Be(fst, 24 + 8) == 5); // Waves next index includes bank 90
        assert(fst[4 * 12] == 0);             // bank 90 inserted before root.bin
    }

    fs::remove_all(tmpDir);
    return 0;
}
