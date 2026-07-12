#include "tpcm/WaveBankBuilder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace tpcm {
namespace {

// ============================================================
// Big-endian binary helpers
// ============================================================

uint32_t readU32(const std::vector<uint8_t>& v, size_t off) {
    if (off + 4 > v.size()) throw std::runtime_error("readU32 out of bounds");
    return (uint32_t(v[off]) << 24) | (uint32_t(v[off + 1]) << 16) |
           (uint32_t(v[off + 2]) << 8) | uint32_t(v[off + 3]);
}

int16_t readS16(const std::vector<uint8_t>& v, size_t off) {
    if (off + 2 > v.size()) throw std::runtime_error("readS16 out of bounds");
    return static_cast<int16_t>((uint16_t(v[off]) << 8) | uint16_t(v[off + 1]));
}

float readF32(const std::vector<uint8_t>& v, size_t off) {
    uint32_t u = readU32(v, off);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void writeU32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    if (off + 4 > v.size()) throw std::runtime_error("writeU32 out of bounds");
    v[off]     = uint8_t((x >> 24) & 0xFF);
    v[off + 1] = uint8_t((x >> 16) & 0xFF);
    v[off + 2] = uint8_t((x >> 8) & 0xFF);
    v[off + 3] = uint8_t(x & 0xFF);
}

void appendU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t((x >> 24) & 0xFF));
    v.push_back(uint8_t((x >> 16) & 0xFF));
    v.push_back(uint8_t((x >> 8) & 0xFF));
    v.push_back(uint8_t(x & 0xFF));
}

void appendF32(std::vector<uint8_t>& v, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    appendU32(v, u);
}

void appendS16(std::vector<uint8_t>& v, int16_t x) {
    v.push_back(uint8_t((uint16_t(x) >> 8) & 0xFF));
    v.push_back(uint8_t(uint16_t(x) & 0xFF));
}

void align4(std::vector<uint8_t>& v) {
    while (v.size() % 4 != 0) v.push_back(0);
}

uint32_t fourCc(const char* s) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

void relocateBaaContentOffsets(std::vector<uint8_t>& baa, size_t shiftedFrom,
                               int64_t delta) {
    if (delta == 0) return;

    size_t pos = 0;
    while (pos + 4 <= baa.size()) {
        const uint32_t command = readU32(baa, pos);
        pos += 4;

        size_t argCount = 0;
        std::vector<size_t> contentArgs;
        if (command == fourCc("AA_<")) {
            argCount = 0;
        } else if (command == fourCc("bst ") || command == fourCc("bstn") ||
                   command == fourCc("bsc ")) {
            argCount = 2;
            contentArgs = {0, 1};
        } else if (command == fourCc("ws  ")) {
            argCount = 3;
            contentArgs = {1};
        } else if (command == fourCc("bnk ")) {
            argCount = 2;
            contentArgs = {1};
        } else if (command == fourCc("bl_<") || command == fourCc("vbnk")) {
            argCount = 2;
        } else if (command == fourCc("bmsa") || command == fourCc("dsqb") ||
                   command == fourCc("sect")) {
            argCount = 1;
        } else if (command == fourCc(">_bl")) {
            argCount = 0;
        } else if (command == fourCc("bms ")) {
            argCount = 3;
            contentArgs = {1, 2};
        } else if (command == fourCc("bsft") || command == fourCc("bfca")) {
            argCount = 1;
            contentArgs = {0};
        } else if (command == fourCc(">_AA")) {
            return;
        } else {
            throw std::runtime_error("Unsupported BAA command while relocating offsets: " +
                                     std::to_string(command));
        }

        if (pos + argCount * 4 > baa.size())
            throw std::runtime_error("Truncated BAA command table");

        for (size_t arg : contentArgs) {
            const size_t argPos = pos + arg * 4;
            const uint32_t oldOffset = readU32(baa, argPos);
            if (oldOffset < shiftedFrom) continue;

            const int64_t relocated = int64_t(oldOffset) + delta;
            if (relocated < 0 || relocated > UINT32_MAX)
                throw std::runtime_error("Relocated BAA offset is out of range");
            writeU32(baa, argPos, static_cast<uint32_t>(relocated));
        }
        pos += argCount * 4;
    }
    throw std::runtime_error("BAA command table is missing >_AA terminator");
}

// ============================================================
// File I/O
// ============================================================

std::vector<uint8_t> readAllBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open: " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

void writeAllBytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write: " + path);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

// ============================================================
// TWave (0x24 bytes, big-endian)
// offsetEnd stores the LENGTH of the sample data, not an end position.
// ============================================================

struct TWave {
    uint8_t  _00 = 0;
    uint8_t  format = 0;
    uint8_t  baseKey = 0;
    float    sampleRate = 0.0f;
    uint32_t offsetStart = 0;
    uint32_t offsetEnd = 0;   // length in bytes
    uint32_t loopFlags = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    uint32_t sampleCount = 0;
    int16_t  mpLast = 0;
    int16_t  mpPenult = 0;
};

TWave parseTWave(const std::vector<uint8_t>& v, size_t off) {
    TWave w;
    w._00        = v[off];
    w.format     = v[off + 1];
    w.baseKey    = v[off + 2];
    // off+3 is pad byte
    w.sampleRate  = readF32(v, off + 4);
    w.offsetStart = readU32(v, off + 8);
    w.offsetEnd   = readU32(v, off + 12);
    w.loopFlags   = readU32(v, off + 16);
    w.loopStart   = readU32(v, off + 20);
    w.loopEnd     = readU32(v, off + 24);
    w.sampleCount = readU32(v, off + 28);
    w.mpLast      = readS16(v, off + 32);
    w.mpPenult    = readS16(v, off + 34);
    return w;
}

void appendTWave(std::vector<uint8_t>& v, const TWave& w) {
    v.push_back(w._00);
    v.push_back(w.format);
    v.push_back(w.baseKey);
    v.push_back(0); // pad
    appendF32(v, w.sampleRate);
    appendU32(v, w.offsetStart);
    appendU32(v, w.offsetEnd);
    appendU32(v, w.loopFlags);
    appendU32(v, w.loopStart);
    appendU32(v, w.loopEnd);
    appendU32(v, w.sampleCount);
    appendS16(v, w.mpLast);
    appendS16(v, w.mpPenult);
}

// ============================================================
// Find the BGM WSYS block inside Z2Sound.baa.
// Returns the offset of the "WSYS" magic, or npos on failure.
// The BGM WSYS is identified by its first bank name starting with "Z2BgmWave_".
// ============================================================

static constexpr size_t kNpos = static_cast<size_t>(-1);

size_t findBgmWsy(const std::vector<uint8_t>& baa) {
    for (size_t p = 0; p + 0x18 <= baa.size(); ++p) {
        if (baa[p] != 'W' || baa[p + 1] != 'S' || baa[p + 2] != 'Y' || baa[p + 3] != 'S')
            continue;
        const uint32_t winfRel = readU32(baa, p + 0x10);
        if (p + winfRel + 8 > baa.size()) continue;
        const uint32_t n = readU32(baa, p + winfRel + 4);
        if (n < 1 || n > 200) continue;
        const uint32_t firstArcRel = readU32(baa, p + winfRel + 8);
        const size_t nameStart = p + firstArcRel;
        if (nameStart + 10 > baa.size()) continue;
        if (std::memcmp(baa.data() + nameStart, "Z2BgmWave_", 10) == 0)
            return p;
    }
    return kNpos;
}

// ============================================================
// Build wave_id -> (bankNum, TWave) from the original WSYS block.
// First occurrence wins (matches Python behaviour).
// ============================================================

struct WaveInfo {
    uint32_t bankNum = 0;
    TWave    twave;
};

std::unordered_map<uint32_t, WaveInfo> buildWaveMap(const std::vector<uint8_t>& wsy) {
    const uint32_t winfOff = readU32(wsy, 0x10);
    const uint32_t wbctOff = readU32(wsy, 0x14);
    const uint32_t nBanks  = readU32(wsy, winfOff + 4);
    const uint32_t nCs     = readU32(wsy, wbctOff + 8);

    std::vector<uint32_t> arcOffs(nBanks), csOffs(nBanks);
    for (uint32_t i = 0; i < nBanks; ++i)
        arcOffs[i] = readU32(wsy, winfOff + 8 + i * 4);
    for (uint32_t i = 0; i < nBanks && i < nCs; ++i)
        csOffs[i] = readU32(wsy, wbctOff + 12 + i * 4);

    std::unordered_map<uint32_t, WaveInfo> waveMap;
    for (uint32_t bank = 0; bank < nBanks; ++bank) {
        const uint32_t arcOff = arcOffs[bank];
        if (arcOff + 0x74 > wsy.size()) continue;
        const uint32_t waveCount = readU32(wsy, arcOff + 0x70);
        if (waveCount == 0) continue;

        std::vector<uint32_t> twOffs(waveCount);
        for (uint32_t j = 0; j < waveCount; ++j)
            twOffs[j] = readU32(wsy, arcOff + 0x74 + j * 4);

        const uint32_t csOff   = csOffs[bank];
        if (csOff + 0x10 > wsy.size()) continue;
        const uint32_t ctrlOff = readU32(wsy, csOff + 0x0C);
        if (ctrlOff + 8 > wsy.size()) continue;
        const uint32_t ctrlCnt = readU32(wsy, ctrlOff + 4);
        if (ctrlCnt != waveCount) continue;

        std::vector<uint32_t> cwOffs(ctrlCnt);
        for (uint32_t j = 0; j < ctrlCnt; ++j)
            cwOffs[j] = readU32(wsy, ctrlOff + 8 + j * 4);

        for (uint32_t j = 0; j < waveCount; ++j) {
            const uint32_t waveId = readU32(wsy, cwOffs[j]) & 0xFFFF;
            if (waveMap.count(waveId) == 0) {
                WaveInfo info;
                info.bankNum = bank;
                info.twave   = parseTWave(wsy, twOffs[j]);
                waveMap[waveId] = info;
            }
        }
    }
    return waveMap;
}

static std::string toHex(uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

// ============================================================
// Core algorithm shared by file-based and in-memory APIs.
// origWsy   : the WSYS block (from the backup BAA) used as both the wave map
//             source and the base WSY to patch.
// awLoader  : returns raw bytes of Z2BgmWave_<bankNum>.aw; empty = unavailable.
// outAwBytes      : filled with the new .aw content.
// outPatchedWsy   : filled with the patched WSYS bytes (ready for replaceBgmWsy).
// ============================================================
static bool patchWsyCore(
    const std::vector<uint8_t>& origWsy,
    const std::function<std::vector<uint8_t>(uint32_t)>& awLoader,
    const std::vector<uint32_t>& waveIds,
    const std::unordered_set<uint32_t>& staticBanks,
    uint32_t bankIndex,
    const std::string& outputAwName,
    std::vector<uint8_t>& outAwBytes,
    std::vector<uint8_t>& outPatchedWsy,
    const std::function<void(const std::string&)>& emit)
{
    const auto waveMap = buildWaveMap(origWsy);
    emit("  [bank_gen] Mapped " + std::to_string(waveMap.size()) + " wave IDs from WSYS");

    std::unordered_set<uint32_t> staticIds;
    for (const auto& kv : waveMap)
        if (staticBanks.count(kv.second.bankNum)) staticIds.insert(kv.first);
    emit("  [bank_gen] Skipping " + std::to_string(staticIds.size()) + " IDs in static banks");

    // ---- Pack new .aw ----
    std::vector<uint8_t> awBuf;
    std::unordered_set<uint32_t> seenIds;
    struct ValidWave { uint32_t waveId; TWave twave; };
    std::vector<ValidWave> valid;
    uint32_t skipped = 0;

    std::unordered_map<uint32_t, std::vector<uint8_t>> awCache;
    auto loadAw = [&](uint32_t bankNum) -> const std::vector<uint8_t>& {
        auto it = awCache.find(bankNum);
        if (it == awCache.end()) awCache[bankNum] = awLoader(bankNum);
        return awCache[bankNum];
    };

    for (uint32_t waveId : waveIds) {
        if (seenIds.count(waveId) || waveMap.count(waveId) == 0 || staticIds.count(waveId)) {
            ++skipped; continue;
        }
        const WaveInfo& info = waveMap.at(waveId);
        const auto& awData   = loadAw(info.bankNum);
        const uint32_t s = info.twave.offsetStart, lng = info.twave.offsetEnd;
        if (lng == 0 || static_cast<size_t>(s) + lng > awData.size()) { ++skipped; continue; }
        const uint32_t newStart = static_cast<uint32_t>(awBuf.size());
        awBuf.insert(awBuf.end(),
                     awData.begin() + static_cast<ptrdiff_t>(s),
                     awData.begin() + static_cast<ptrdiff_t>(s + lng));
        TWave updated = info.twave;
        updated.offsetStart = newStart; updated.offsetEnd = lng;
        seenIds.insert(waveId);
        valid.push_back({waveId, updated});
    }

    const uint32_t N = static_cast<uint32_t>(valid.size());
    emit("  [bank_gen] Packed " + std::to_string(N) + " waves (" +
         std::to_string(awBuf.size()) + " bytes) — skipped " + std::to_string(skipped));
    if (N == 0) { emit("  [bank_gen] ERROR: no valid waves found"); return false; }

    // ---- Patch WSYS ----
    const uint32_t winfOff = readU32(origWsy, 0x10);
    const uint32_t wbctOff = readU32(origWsy, 0x14);
    const uint32_t nCur    = readU32(origWsy, winfOff + 4);
    const uint32_t nUse    = std::min(nCur, bankIndex);

    std::vector<uint32_t> arcOffs(nUse), csOffs(nUse);
    for (uint32_t i = 0; i < nUse; ++i) arcOffs[i] = readU32(origWsy, winfOff + 8 + i * 4);
    for (uint32_t i = 0; i < nUse; ++i) csOffs[i]  = readU32(origWsy, wbctOff + 12 + i * 4);

    std::vector<uint8_t> wsy(origWsy.begin(), origWsy.begin() + static_cast<ptrdiff_t>(wbctOff));

    std::vector<uint32_t> twaveOffs(N), ctrlwaveOffs(N);
    for (uint32_t i = 0; i < N; ++i) {
        align4(wsy); twaveOffs[i] = uint32_t(wsy.size()); appendTWave(wsy, valid[i].twave);
    }
    for (uint32_t i = 0; i < N; ++i) {
        align4(wsy); ctrlwaveOffs[i] = uint32_t(wsy.size());
        appendU32(wsy, (bankIndex << 16) | (valid[i].waveId & 0xFFFF));
    }
    align4(wsy);
    const uint32_t ctrl90Off = uint32_t(wsy.size());
    wsy.push_back('C'); wsy.push_back('-'); wsy.push_back('D'); wsy.push_back('F');
    appendU32(wsy, N);
    for (uint32_t off : ctrlwaveOffs) appendU32(wsy, off);

    align4(wsy);
    const uint32_t cs90Off = uint32_t(wsy.size());
    wsy.push_back('S'); wsy.push_back('C'); wsy.push_back('N'); wsy.push_back('E');
    for (int i = 0; i < 8; ++i) wsy.push_back(0);
    appendU32(wsy, ctrl90Off);

    align4(wsy);
    const uint32_t arc90Off = uint32_t(wsy.size());
    { std::vector<uint8_t> fn(0x70, 0); const size_t cl = std::min(outputAwName.size(), size_t(0x6F));
      std::memcpy(fn.data(), outputAwName.data(), cl); wsy.insert(wsy.end(), fn.begin(), fn.end()); }
    appendU32(wsy, N);
    for (uint32_t off : twaveOffs) appendU32(wsy, off);

    align4(wsy);
    const uint32_t newWinfOff = uint32_t(wsy.size());
    wsy.push_back('W'); wsy.push_back('I'); wsy.push_back('N'); wsy.push_back('F');
    appendU32(wsy, bankIndex + 1);
    for (uint32_t off : arcOffs) appendU32(wsy, off);
    appendU32(wsy, arc90Off);

    align4(wsy);
    const uint32_t newWbctOff = uint32_t(wsy.size());
    wsy.push_back('W'); wsy.push_back('B'); wsy.push_back('C'); wsy.push_back('T');
    appendU32(wsy, 0xFFFFFFFF); appendU32(wsy, bankIndex + 1);
    for (uint32_t off : csOffs) appendU32(wsy, off);
    appendU32(wsy, cs90Off);

    writeU32(wsy, 0x04, uint32_t(wsy.size()) - 8);
    writeU32(wsy, 0x10, newWinfOff);
    writeU32(wsy, 0x14, newWbctOff);

    // Fix mWaveTableSize
    { uint32_t realMax = 0;
      const uint32_t sw = readU32(wsy, 0x14), sn = readU32(wsy, sw + 8);
      for (uint32_t i = 0; i < sn; ++i) {
          const uint32_t cs = readU32(wsy, sw + 12 + i * 4), ct = readU32(wsy, cs + 0x0C),
                         cn = readU32(wsy, ct + 4);
          for (uint32_t j = 0; j < cn; ++j) {
              const uint32_t wid = readU32(wsy, readU32(wsy, ct + 8 + j * 4)) & 0xFFFF;
              if (wid > realMax) realMax = wid;
          }
      }
      const uint32_t needed = realMax + 1, cur = readU32(wsy, 0x0C);
      if (needed > cur) { writeU32(wsy, 0x0C, needed);
          emit("  [bank_gen] mWaveTableSize bumped " + std::to_string(cur) + " -> " + std::to_string(needed)); }
    }

    emit("  [bank_gen] WSYS patched: " + std::to_string(wsy.size()) + " bytes, " +
         std::to_string(bankIndex + 1) + " banks");

    outAwBytes    = std::move(awBuf);
    outPatchedWsy = std::move(wsy);
    return true;
}

}  // namespace

// ============================================================
// Public API
// ============================================================

bool generateWaveBank(const WaveBankConfig& config,
                      const std::function<void(const std::string&)>& log)
{
    auto emit = [&](const std::string& msg) { if (log) log(msg); };

    std::vector<uint8_t> baaData;
    try { baaData = readAllBytes(config.backupBaaPath); }
    catch (const std::exception& e) {
        emit(std::string("ERROR: cannot read backup BAA: ") + e.what()); return false;
    }
    const size_t wsyPos = findBgmWsy(baaData);
    if (wsyPos == kNpos) { emit("ERROR: BGM WSYS not found in backup BAA"); return false; }
    const std::vector<uint8_t> origWsy(baaData.begin() + static_cast<ptrdiff_t>(wsyPos),
                                       baaData.end());

    auto awLoader = [&](uint32_t bankNum) -> std::vector<uint8_t> {
        const std::string path = config.backupWavesDir + "/Z2BgmWave_" +
                                 std::to_string(bankNum) + ".aw";
        try { return readAllBytes(path); } catch (...) { return {}; }
    };

    std::vector<uint8_t> awBytes, wsyBytes;
    if (!patchWsyCore(origWsy, awLoader, config.waveIds, config.staticBanks,
                      config.bankIndex, config.outputAwName, awBytes, wsyBytes, emit))
        return false;

    try { writeAllBytes(config.outputAwPath, awBytes); }
    catch (const std::exception& e) {
        emit(std::string("ERROR: cannot write .aw: ") + e.what()); return false;
    }
    try { writeAllBytes(config.wsyPath, wsyBytes); }
    catch (const std::exception& e) {
        emit(std::string("ERROR: cannot write 3.wsy: ") + e.what()); return false;
    }
    emit("  [bank_gen] Wrote " + config.outputAwName);
    return true;
}

WaveBankOutput generateWaveBankInMemory(
    const std::vector<uint8_t>& baaData,
    std::function<std::vector<uint8_t>(uint32_t)> awLoader,
    const std::vector<uint32_t>& waveIds,
    const std::string& outputAwName,
    uint32_t bankIndex,
    const std::unordered_set<uint32_t>& staticBanks,
    const std::function<void(const std::string&)>& log)
{
    auto emit = [&](const std::string& msg) { if (log) log(msg); };

    const size_t wsyPos = findBgmWsy(baaData);
    if (wsyPos == kNpos) throw std::runtime_error("BGM WSYS not found in BAA");
    const std::vector<uint8_t> origWsy(baaData.begin() + static_cast<ptrdiff_t>(wsyPos),
                                       baaData.end());
    emit("  [bank_gen] WSYS: " + std::to_string(origWsy.size()) + " bytes (from ISO)");

    std::vector<uint8_t> awBytes, wsyBytes;
    if (!patchWsyCore(origWsy, awLoader, waveIds, staticBanks,
                      bankIndex, outputAwName, awBytes, wsyBytes, emit))
        throw std::runtime_error("Wave bank generation failed — no valid waves");

    return {std::move(awBytes), replaceBgmWsy(baaData, wsyBytes)};
}

std::vector<uint8_t> replaceBgmWsy(const std::vector<uint8_t>& baa,
                                   const std::vector<uint8_t>& newWsy) {
    const size_t wsyPos = findBgmWsy(baa);
    if (wsyPos == kNpos)
        throw std::runtime_error("BGM WSYS not found in BAA");

    // WSYS fileSize field (at offset 4) excludes the first 8 bytes (magic + size).
    // Total old block size = fileSize + 8.
    const size_t oldBlockSize = static_cast<size_t>(readU32(baa, wsyPos + 4)) + 8;

    std::vector<uint8_t> result;
    result.reserve(wsyPos + newWsy.size());
    result.insert(result.end(), baa.begin(),
                  baa.begin() + static_cast<ptrdiff_t>(wsyPos));
    result.insert(result.end(), newWsy.begin(), newWsy.end());
    // Preserve any trailing data after the old WSYS (usually none in TP's BAA)
    const size_t tail = wsyPos + oldBlockSize;
    if (tail < baa.size())
        result.insert(result.end(),
                      baa.begin() + static_cast<ptrdiff_t>(tail), baa.end());

    const int64_t delta = int64_t(newWsy.size()) - int64_t(oldBlockSize);
    relocateBaaContentOffsets(result, tail, delta);
    return result;
}

}  // namespace tpcm
