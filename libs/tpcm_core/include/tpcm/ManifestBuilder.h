#pragma once

#include "tpcm/BgmDatabase.h"
#include "tpcm/MusicRandomizer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tpcm {

struct GeneratedBank {
    std::uint32_t bankId;
    std::string ownerBms;
    bool active;
};

struct ManifestEntry {
    std::string route;
    std::uint32_t originalBgmId;
    std::uint32_t replacementBgmId;
    std::vector<std::uint32_t> bgmWaves;
    std::string kind;
    bool noEnemyMusic = false;
};

struct Manifest {
    int schema = 1;
    std::vector<GeneratedBank> generatedBanks;
    std::vector<ManifestEntry> entries;
    bool disableAllEnemyMusic = false;
};

Manifest buildManifest(const std::vector<MusicSlotAssignment>& assignments,
                       const std::vector<GeneratedBank>& generatedBanks,
                       const BgmEntry* bgmTable,
                       std::size_t bgmTableSize);

std::string toJson(const Manifest& manifest);

}  // namespace tpcm
