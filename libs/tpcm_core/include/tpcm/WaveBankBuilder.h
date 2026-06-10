#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace tpcm {

struct WaveBankConfig {
    std::string backupBaaPath;     // path to __backup/Z2Sound.baa
    std::string backupWavesDir;    // path to __backup/Waves/ directory
    std::string wsyPath;           // path to 3.wsy (patched in-place)
    std::string outputAwPath;      // full path to write the new .aw file
    std::string outputAwName;      // filename only, e.g. "Z2BgmWave_90.aw"
    uint32_t bankIndex = 90;       // zero-based bank index to create
    std::vector<uint32_t> waveIds; // requested wave IDs (parsed from toolkit JSON by caller)
    std::unordered_set<uint32_t> staticBanks = {0}; // always-resident banks; skip their IDs
};

// Ports build_bank90.py. Reads raw DSP-ADPCM from backup .aw files (no re-encoding),
// writes the new .aw, and appends the new bank entry to 3.wsy.
// Returns false and logs an error on failure.
bool generateWaveBank(const WaveBankConfig& config,
                      const std::function<void(const std::string&)>& log = nullptr);

// Result of in-memory wave bank generation.
struct WaveBankOutput {
    std::vector<uint8_t> awData;   // new .aw file content to inject into the ISO
    std::vector<uint8_t> baaData;  // updated Z2Sound.baa (new WSYS spliced in)
};

// Toolkit-free in-memory API. All source data comes from the ISO itself.
// baaData    : raw bytes of Z2Sound.baa read from the ISO.
// awLoader   : callback — given a bank number returns Z2BgmWave_<N>.aw bytes
//              (read directly from the ISO); return empty vector if unavailable.
// waveIds    : wave IDs to pack into the new bank (derived from MIDI instrument usage).
// outputAwName : filename for the new .aw, e.g. "Z2BgmWave_90.aw".
// Throws std::runtime_error on failure.
WaveBankOutput generateWaveBankInMemory(
    const std::vector<uint8_t>& baaData,
    std::function<std::vector<uint8_t>(uint32_t bankNum)> awLoader,
    const std::vector<uint32_t>& waveIds,
    const std::string& outputAwName,
    uint32_t bankIndex = 90,
    const std::unordered_set<uint32_t>& staticBanks = {0},
    const std::function<void(const std::string&)>& log = nullptr);

// Given the original Z2Sound.baa bytes and a freshly-patched WSYS block,
// returns new BAA bytes with the BGM WSYS replaced in-place.
std::vector<uint8_t> replaceBgmWsy(const std::vector<uint8_t>& baaData,
                                   const std::vector<uint8_t>& newWsyData);

}  // namespace tpcm
