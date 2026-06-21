#pragma once

#include "tpcm/BgmDatabase.h"

#include <cstdint>
#include <vector>

namespace tpcm {

enum class MusicBgmPoolMode : std::uint8_t {
    Scene,
    Dungeon,
    All,
    Boss,
};

enum class MusicAssignmentKind : std::uint8_t {
    Vanilla,
    VanillaSwap,
    CustomTprs,
};

enum class MusicSlotCompatibility : std::uint8_t {
    Automatic,
    TargetOnly,
    Excluded,
};

struct MusicSlotAssignment {
    std::uint32_t originalBgmId;
    std::uint32_t replacementBgmId;
    std::uint8_t replacementWave1;
    std::uint8_t replacementWave2;
    MusicAssignmentKind kind;
    bool noEnemyMusic = false;
};

struct CustomMusicCandidate {
    std::uint32_t bgmId;
    std::uint8_t wave1;
    std::uint8_t wave2;
    bool noEnemyMusic = false;
};

// A forced assignment pins a specific replacement song to a specific target slot,
// regardless of the seed. The randomizer shuffles all other pool entries around it.
struct ForcedAssignment {
    std::uint32_t replacementBgmId;  // the song that will play (left dropdown)
    std::uint32_t targetBgmId;       // the slot it plays in (right dropdown)
};

struct MusicCompatibilityEntry {
    const BgmEntry* entry = nullptr;
    BgmRuntimeInfo runtime;
    MusicSlotCompatibility compatibility = MusicSlotCompatibility::Excluded;
    const char* reason = "";
};

std::vector<const BgmEntry*> selectMusicBgmPool(MusicBgmPoolMode mode);
std::vector<const BgmEntry*> selectCustomMusicSlots(MusicBgmPoolMode mode);
std::vector<const BgmEntry*> selectReliableMusicTargets(MusicBgmPoolMode mode);
std::vector<MusicCompatibilityEntry> buildMusicCompatibilityReport(MusicBgmPoolMode mode);

std::vector<MusicSlotAssignment> buildMusicAssignments(std::uint32_t seed, MusicBgmPoolMode mode);
std::vector<MusicSlotAssignment> buildMusicAssignments(
    std::uint32_t seed, MusicBgmPoolMode mode,
    const std::vector<CustomMusicCandidate>& customCandidates);
std::vector<MusicSlotAssignment> buildMusicAssignments(
    std::uint32_t seed, MusicBgmPoolMode mode,
    const std::vector<CustomMusicCandidate>& customCandidates,
    const std::vector<ForcedAssignment>& forcedAssignments);

std::vector<MusicSlotAssignment> buildMusicAssignments(std::uint32_t seed);

bool isIncompatibleReplacement(std::uint32_t originalBgmId, std::uint32_t replacementBgmId) noexcept;

}  // namespace tpcm
