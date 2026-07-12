#include "tpcm/WaveBankBuilder.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================
// Binary helpers (local to test)
// ============================================================
namespace {

void append8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }

void appendU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t((x >> 24) & 0xFF));
    v.push_back(uint8_t((x >> 16) & 0xFF));
    v.push_back(uint8_t((x >> 8) & 0xFF));
    v.push_back(uint8_t(x & 0xFF));
}

void appendF32(std::vector<uint8_t>& v, float f) {
    uint32_t u; std::memcpy(&u, &f, 4); appendU32(v, u);
}

void appendS16(std::vector<uint8_t>& v, int16_t x) {
    v.push_back(uint8_t((uint16_t(x) >> 8) & 0xFF));
    v.push_back(uint8_t(uint16_t(x) & 0xFF));
}

void writeU32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off]     = uint8_t((x >> 24) & 0xFF);
    v[off + 1] = uint8_t((x >> 16) & 0xFF);
    v[off + 2] = uint8_t((x >> 8) & 0xFF);
    v[off + 3] = uint8_t(x & 0xFF);
}

uint32_t readU32(const std::vector<uint8_t>& v, size_t off) {
    return (uint32_t(v[off]) << 24) | (uint32_t(v[off + 1]) << 16) |
           (uint32_t(v[off + 2]) << 8) | uint32_t(v[off + 3]);
}

void align4(std::vector<uint8_t>& v) {
    while (v.size() % 4 != 0) v.push_back(0);
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

// ============================================================
// Build a minimal WSYS with 1 bank (bank 0), 2 waves.
//   wave0: id=w0Id, source bytes at aw[aw0Start..aw0Start+aw0Len)
//   wave1: id=w1Id, source bytes at aw[aw1Start..aw1Start+aw1Len)
//
// Layout (all offsets from WSYS start):
//   0x000  WSYS header  (0x18)
//   0x018  TWave[0]     (0x24)
//   0x03C  TWave[1]     (0x24)
//   0x060  TCtrlWave[0] (0x04)
//   0x064  TCtrlWave[1] (0x04)
//   0x068  TCtrl        (0x10)
//   0x078  TCtrlScene   (0x10)
//   0x088  TWaveArchive (0x70+4+8 = 0x80)
//   0x108  WINF         (0x0C)
//   0x114  WBCT         (0x10)
//   Total: 0x124
// ============================================================
std::vector<uint8_t> buildWsy1Bank(uint32_t w0Id, uint32_t w1Id,
                                   uint32_t aw0Start, uint32_t aw0Len,
                                   uint32_t aw1Start, uint32_t aw1Len)
{
    std::vector<uint8_t> wsy(0x18, 0); // header placeholder

    // TWave[0] @ 0x018
    assert(wsy.size() == 0x18);
    const uint32_t tw0Off = uint32_t(wsy.size());
    append8(wsy, 0); append8(wsy, 0); append8(wsy, 60); append8(wsy, 0); // _00,format,baseKey,pad
    appendF32(wsy, 32000.0f);      // sampleRate
    appendU32(wsy, aw0Start);      // offsetStart
    appendU32(wsy, aw0Len);        // offsetEnd = length
    appendU32(wsy, 0);             // loopFlags
    appendU32(wsy, 0);             // loopStart
    appendU32(wsy, 0);             // loopEnd
    appendU32(wsy, 100);           // sampleCount
    appendS16(wsy, 0);             // mpLast
    appendS16(wsy, 0);             // mpPenult
    assert(wsy.size() == 0x3C);

    // TWave[1] @ 0x03C
    const uint32_t tw1Off = uint32_t(wsy.size());
    append8(wsy, 0); append8(wsy, 0); append8(wsy, 60); append8(wsy, 0);
    appendF32(wsy, 32000.0f);
    appendU32(wsy, aw1Start);
    appendU32(wsy, aw1Len);
    appendU32(wsy, 0); appendU32(wsy, 0); appendU32(wsy, 0);
    appendU32(wsy, 100);
    appendS16(wsy, 0); appendS16(wsy, 0);
    assert(wsy.size() == 0x60);

    // TCtrlWave[0] @ 0x060 — upper16=bank(0), lower16=wave_id
    const uint32_t cw0Off = uint32_t(wsy.size());
    appendU32(wsy, (0u << 16) | (w0Id & 0xFFFF));

    // TCtrlWave[1] @ 0x064
    const uint32_t cw1Off = uint32_t(wsy.size());
    appendU32(wsy, (0u << 16) | (w1Id & 0xFFFF));

    // TCtrl @ 0x068 — "C-DF", waveCount=2, [cw0Off, cw1Off]
    const uint32_t ctrlOff = uint32_t(wsy.size());
    wsy.push_back('C'); wsy.push_back('-'); wsy.push_back('D'); wsy.push_back('F');
    appendU32(wsy, 2);
    appendU32(wsy, cw0Off);
    appendU32(wsy, cw1Off);
    assert(wsy.size() == 0x78);

    // TCtrlScene @ 0x078 — "SCNE" + 8 zeros + ctrl_offset
    const uint32_t csOff = uint32_t(wsy.size());
    wsy.push_back('S'); wsy.push_back('C'); wsy.push_back('N'); wsy.push_back('E');
    for (int i = 0; i < 8; ++i) wsy.push_back(0);
    appendU32(wsy, ctrlOff);
    assert(wsy.size() == 0x88);

    // TWaveArchive @ 0x088 — char[0x70] name, waveCount=2, [tw0Off, tw1Off]
    const uint32_t arcOff = uint32_t(wsy.size());
    {
        const char* fname = "Z2BgmWave_0.aw";
        std::vector<uint8_t> nameBuf(0x70, 0);
        for (size_t i = 0; fname[i]; ++i) nameBuf[i] = uint8_t(fname[i]);
        wsy.insert(wsy.end(), nameBuf.begin(), nameBuf.end());
    }
    appendU32(wsy, 2);
    appendU32(wsy, tw0Off);
    appendU32(wsy, tw1Off);
    assert(wsy.size() == 0x104); // 0x088 + 0x70 + 4 + 4*2 = 0x104

    // WINF @ 0x104 — "WINF", bankCount=1, arcOff
    const uint32_t winfOff = uint32_t(wsy.size());
    wsy.push_back('W'); wsy.push_back('I'); wsy.push_back('N'); wsy.push_back('F');
    appendU32(wsy, 1);
    appendU32(wsy, arcOff);
    assert(wsy.size() == 0x110);

    // WBCT @ 0x110 — "WBCT", 0xFFFFFFFF, bankCount=1, csOff
    const uint32_t wbctOff = uint32_t(wsy.size());
    wsy.push_back('W'); wsy.push_back('B'); wsy.push_back('C'); wsy.push_back('T');
    appendU32(wsy, 0xFFFFFFFF);
    appendU32(wsy, 1);
    appendU32(wsy, csOff);
    assert(wsy.size() == 0x120);

    // Fix up header
    wsy[0] = 'W'; wsy[1] = 'S'; wsy[2] = 'Y'; wsy[3] = 'S';
    writeU32(wsy, 0x04, uint32_t(wsy.size()) - 8); // fileSize
    writeU32(wsy, 0x08, 0);                        // id
    writeU32(wsy, 0x0C, std::max(w0Id, w1Id) + 1); // mWaveTableSize
    writeU32(wsy, 0x10, winfOff);
    writeU32(wsy, 0x14, wbctOff);

    return wsy;
}

} // namespace

// ============================================================
// Test helpers
// ============================================================

// Verify that the patched WSY has the expected 2-bank structure after
// generateWaveBank adds bank 1 with the given wave IDs.
static void verifyPatchedWsy(const std::vector<uint8_t>& wsy,
                              uint32_t expectedW0Id, uint32_t expectedW1Id,
                              uint32_t bankIndex, const std::string& awName)
{
    // WSYS magic
    assert(wsy[0] == 'W' && wsy[1] == 'S' && wsy[2] == 'Y' && wsy[3] == 'S');

    // WINF: 2 banks
    const uint32_t winfOff = readU32(wsy, 0x10);
    assert(wsy[winfOff]     == 'W' && wsy[winfOff + 1] == 'I' &&
           wsy[winfOff + 2] == 'N' && wsy[winfOff + 3] == 'F');
    assert(readU32(wsy, winfOff + 4) == bankIndex + 1);

    // WBCT: 2 banks
    const uint32_t wbctOff = readU32(wsy, 0x14);
    assert(wsy[wbctOff]     == 'W' && wsy[wbctOff + 1] == 'B' &&
           wsy[wbctOff + 2] == 'C' && wsy[wbctOff + 3] == 'T');
    assert(readU32(wsy, wbctOff + 8) == bankIndex + 1);

    // New bank's TWaveArchive
    const uint32_t arc1Off  = readU32(wsy, winfOff + 8 + bankIndex * 4);
    assert(std::memcmp(wsy.data() + arc1Off, awName.data(), awName.size()) == 0);
    assert(readU32(wsy, arc1Off + 0x70) == 2); // waveCount

    // New bank's TCtrlScene -> TCtrl -> TCtrlWave entries
    const uint32_t cs1Off   = readU32(wsy, wbctOff + 12 + bankIndex * 4);
    assert(std::memcmp(wsy.data() + cs1Off, "SCNE", 4) == 0);
    const uint32_t ctrl1Off = readU32(wsy, cs1Off + 0x0C);
    assert(std::memcmp(wsy.data() + ctrl1Off, "C-DF", 4) == 0);
    assert(readU32(wsy, ctrl1Off + 4) == 2); // waveCount

    const uint32_t cw0Off = readU32(wsy, ctrl1Off + 8);
    const uint32_t cw1Off = readU32(wsy, ctrl1Off + 12);
    const uint32_t cw0    = readU32(wsy, cw0Off);
    const uint32_t cw1    = readU32(wsy, cw1Off);
    assert((cw0 >> 16) == bankIndex);
    assert((cw0 & 0xFFFF) == expectedW0Id);
    assert((cw1 >> 16) == bankIndex);
    assert((cw1 & 0xFFFF) == expectedW1Id);
}

int main() {
    const fs::path tmpDir = fs::temp_directory_path() / "tpcm_wave_bank_builder_test";
    fs::create_directories(tmpDir / "backup" / "Waves");
    fs::create_directories(tmpDir / "working");
    fs::create_directories(tmpDir / "output");

    // Source audio: wave_id=10 at bytes [0..3], wave_id=20 at bytes [4..7]
    const std::vector<uint8_t> sourceAw = {0xAA, 0xBB, 0xCC, 0xDD,
                                           0x11, 0x22, 0x33, 0x44};

    // Synthetic WSYS: 1 bank (bank 0), 2 waves
    //   wave_id=10: aw[0..4), length=4
    //   wave_id=20: aw[4..8), length=4
    const std::vector<uint8_t> origWsy = buildWsy1Bank(10, 20, 0, 4, 4, 4);

    writeFile(tmpDir / "backup" / "Z2Sound.baa", origWsy);  // BAA = just the WSYS block
    writeFile(tmpDir / "backup" / "Waves" / "Z2BgmWave_0.aw", sourceAw);
    writeFile(tmpDir / "working" / "3.wsy", origWsy);       // starting WSY to patch

    // ============================================================
    // Test 1: normal generation, no static bank exclusion
    // ============================================================
    {
        tpcm::WaveBankConfig cfg;
        cfg.backupBaaPath  = (tmpDir / "backup" / "Z2Sound.baa").string();
        cfg.backupWavesDir = (tmpDir / "backup" / "Waves").string();
        cfg.wsyPath        = (tmpDir / "working" / "3.wsy").string();
        cfg.outputAwPath   = (tmpDir / "output" / "Z2BgmWave_1.aw").string();
        cfg.outputAwName   = "Z2BgmWave_1.aw";
        cfg.bankIndex      = 1;
        cfg.waveIds        = {10, 20};
        cfg.staticBanks    = {}; // no exclusions

        const bool ok = tpcm::generateWaveBank(cfg);
        assert(ok);

        // Output .aw must contain both samples in order
        const std::vector<uint8_t> outAw = readFile(tmpDir / "output" / "Z2BgmWave_1.aw");
        assert(outAw.size() == 8);
        assert(std::equal(outAw.begin(), outAw.begin() + 4, sourceAw.begin()));
        assert(std::equal(outAw.begin() + 4, outAw.end(), sourceAw.begin() + 4));

        // Patched WSY must have 2 banks and correct bank 1 structure
        const std::vector<uint8_t> patchedWsy = readFile(tmpDir / "working" / "3.wsy");
        verifyPatchedWsy(patchedWsy, 10, 20, 1, "Z2BgmWave_1.aw");
    }

    // ============================================================
    // Test 2: both waves are in a static bank — generation must fail
    // ============================================================
    {
        // Reset the working WSY
        writeFile(tmpDir / "working" / "3.wsy", origWsy);

        tpcm::WaveBankConfig cfg;
        cfg.backupBaaPath  = (tmpDir / "backup" / "Z2Sound.baa").string();
        cfg.backupWavesDir = (tmpDir / "backup" / "Waves").string();
        cfg.wsyPath        = (tmpDir / "working" / "3.wsy").string();
        cfg.outputAwPath   = (tmpDir / "output" / "Z2BgmWave_1_skip.aw").string();
        cfg.outputAwName   = "Z2BgmWave_1_skip.aw";
        cfg.bankIndex      = 1;
        cfg.waveIds        = {10, 20};
        cfg.staticBanks    = {0}; // bank 0 is static — both waves skipped

        const bool ok = tpcm::generateWaveBank(cfg);
        assert(!ok); // must fail: no valid waves after exclusion
    }

    // ============================================================
    // Test 3: deduplicate repeated wave IDs in the request list
    // ============================================================
    {
        writeFile(tmpDir / "working" / "3.wsy", origWsy);

        tpcm::WaveBankConfig cfg;
        cfg.backupBaaPath  = (tmpDir / "backup" / "Z2Sound.baa").string();
        cfg.backupWavesDir = (tmpDir / "backup" / "Waves").string();
        cfg.wsyPath        = (tmpDir / "working" / "3.wsy").string();
        cfg.outputAwPath   = (tmpDir / "output" / "Z2BgmWave_1_dedup.aw").string();
        cfg.outputAwName   = "Z2BgmWave_1_dedup.aw";
        cfg.bankIndex      = 1;
        cfg.waveIds        = {10, 10, 20, 20}; // duplicates
        cfg.staticBanks    = {};

        const bool ok = tpcm::generateWaveBank(cfg);
        assert(ok);

        // Output must still be exactly 8 bytes (each wave_id packed once)
        const std::vector<uint8_t> outAw = readFile(tmpDir / "output" / "Z2BgmWave_1_dedup.aw");
        assert(outAw.size() == 8);
    }

    // ============================================================
    // Test 4: growing the WSYS relocates trailing BAA content offsets
    // ============================================================
    {
        constexpr uint32_t wsyOffset = 0x40;
        const uint32_t oldBankOffset = wsyOffset + static_cast<uint32_t>(origWsy.size());

        std::vector<uint8_t> baa;
        baa.insert(baa.end(), {'A', 'A', '_', '<'});
        baa.insert(baa.end(), {'w', 's', ' ', ' '});
        appendU32(baa, 3);
        appendU32(baa, wsyOffset);
        appendU32(baa, 0);
        baa.insert(baa.end(), {'b', 'n', 'k', ' '});
        appendU32(baa, 1);
        appendU32(baa, oldBankOffset);
        baa.insert(baa.end(), {'>', '_', 'A', 'A'});
        baa.resize(wsyOffset, 0);
        baa.insert(baa.end(), origWsy.begin(), origWsy.end());
        baa.insert(baa.end(), {'I', 'B', 'N', 'K'});

        std::vector<uint8_t> largerWsy = origWsy;
        largerWsy.insert(largerWsy.end(), 12, 0);
        writeU32(largerWsy, 4, static_cast<uint32_t>(largerWsy.size()) - 8);

        const auto replaced = tpcm::replaceBgmWsy(baa, largerWsy);
        const uint32_t relocatedBankOffset = readU32(replaced, 28);
        assert(relocatedBankOffset == oldBankOffset + 12);
        assert(std::memcmp(replaced.data() + relocatedBankOffset, "IBNK", 4) == 0);
    }

    fs::remove_all(tmpDir);
    return 0;
}
