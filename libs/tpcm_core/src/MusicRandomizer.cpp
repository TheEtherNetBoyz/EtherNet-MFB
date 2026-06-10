#include "tpcm/MusicRandomizer.h"

#include <algorithm>
#include <array>
#include <random>
#include <set>
#include <stdexcept>

namespace tpcm {
namespace {

constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> kIncompatibleReplacements = {{
    {MakeBgmId(62), MakeBgmId(148)},  // Armogohma Phase 1 over Palace Theme
    {MakeBgmId(62), MakeBgmId(98)},   // Zant Boss Theme over Palace Theme
    {MakeBgmId(44), MakeBgmId(8)},    // Ook Battle over House Interiors
    {MakeBgmId(55), MakeBgmId(8)},    // Ook Battle over Snowpeak Ruins
}};

bool isPoolEntry(const BgmEntry& entry, MusicBgmPoolMode mode) noexcept {
    switch (mode) {
        case MusicBgmPoolMode::Scene:
            return entry.sceneBgm;
        case MusicBgmPoolMode::Dungeon:
            return entry.dungeonBgm;
        case MusicBgmPoolMode::All:
            return entry.sceneBgm || entry.bossBgm || entry.minibossBgm || entry.minigameBgm
                   || entry.eventBgm;
        case MusicBgmPoolMode::Boss:
            return entry.bossBgm || entry.trigger == BgmTrigger::Main;
    }
    return false;
}

bool isValidPermutation(const std::vector<const BgmEntry*>& originals,
                        const std::vector<const BgmEntry*>& replacements) noexcept {
    for (std::size_t i = 0; i < originals.size(); ++i) {
        if (originals[i]->bgmId == replacements[i]->bgmId
            || isIncompatibleReplacement(originals[i]->bgmId, replacements[i]->bgmId)) {
            return false;
        }
    }
    return true;
}

void rotateUntilValid(const std::vector<const BgmEntry*>& originals,
                      std::vector<const BgmEntry*>& replacements) {
    if (replacements.size() < 2) {
        return;
    }

    for (std::size_t offset = 1; offset < replacements.size(); ++offset) {
        std::vector<const BgmEntry*> candidate(originals.size());
        for (std::size_t i = 0; i < originals.size(); ++i) {
            candidate[i] = originals[(i + offset) % originals.size()];
        }
        if (isValidPermutation(originals, candidate)) {
            replacements = candidate;
            return;
        }
    }
}

}  // namespace

std::vector<const BgmEntry*> selectMusicBgmPool(MusicBgmPoolMode mode) {
    std::vector<const BgmEntry*> pool;
    for (std::size_t i = 0; i < BGM_TABLE_SIZE; ++i) {
        if (isPoolEntry(BGM_TABLE[i], mode)) {
            pool.push_back(&BGM_TABLE[i]);
        }
    }
    return pool;
}

std::vector<const BgmEntry*> selectCustomMusicSlots(MusicBgmPoolMode mode) {
    std::vector<const BgmEntry*> slots;
    for (const BgmEntry* entry : selectMusicBgmPool(mode)) {
        if (entry->bmsName != nullptr) {
            slots.push_back(entry);
        }
    }
    return slots;
}

std::vector<const BgmEntry*> selectReliableMusicTargets(MusicBgmPoolMode mode) {
    std::vector<const BgmEntry*> slots;
    for (const BgmEntry* entry : selectMusicBgmPool(mode)) {
        // Entries without an archive BMS name are mostly sub-BGMs that the source
        // database currently labels as scene music. Do not promise target-slot
        // replacement until their runtime routes have been audited.
        if (entry->bmsName != nullptr || entry->trigger == BgmTrigger::Main) {
            slots.push_back(entry);
        }
    }
    return slots;
}

std::vector<MusicSlotAssignment> buildMusicAssignments(std::uint32_t seed, MusicBgmPoolMode mode) {
    return buildMusicAssignments(seed, mode, {}, {});
}

std::vector<MusicSlotAssignment> buildMusicAssignments(
    std::uint32_t seed, MusicBgmPoolMode mode,
    const std::vector<CustomMusicCandidate>& customCandidates) {
    return buildMusicAssignments(seed, mode, customCandidates, {});
}

std::vector<MusicSlotAssignment> buildMusicAssignments(
    std::uint32_t seed, MusicBgmPoolMode mode,
    const std::vector<CustomMusicCandidate>& customCandidates,
    const std::vector<ForcedAssignment>& forcedAssignments) {
    const std::vector<const BgmEntry*> pool = selectMusicBgmPool(mode);
    if (pool.size() < 2) {
        return {};
    }

    struct Candidate {
        std::uint32_t bgmId;
        std::uint8_t wave1;
        std::uint8_t wave2;
        MusicAssignmentKind kind;
        bool noEnemyMusic = false;
    };

    // Build candidates parallel to pool, applying custom overrides.
    std::vector<Candidate> candidates;
    candidates.reserve(pool.size());
    for (const BgmEntry* entry : pool) {
        const auto custom = std::find_if(customCandidates.begin(), customCandidates.end(),
            [entry](const CustomMusicCandidate& c) { return c.bgmId == entry->bgmId; });
        if (custom != customCandidates.end()) {
            candidates.push_back({custom->bgmId, custom->wave1, custom->wave2,
                                  MusicAssignmentKind::CustomTprs, custom->noEnemyMusic});
        } else {
            candidates.push_back({entry->bgmId, entry->bgmWave1, entry->bgmWave2,
                                  MusicAssignmentKind::VanillaSwap});
        }
    }

    // Pre-assign forced pairs.
    // slotForced[i]      — pool[i]'s replacement is pinned
    // forcedCandidate[i] — the pinned candidate for pool[i]
    // candidateUsed[k]   — candidates[k] is consumed by a forced assignment
    std::vector<bool>      slotForced(pool.size(), false);
    std::vector<Candidate> forcedCandidate(pool.size());
    std::vector<bool>      candidateUsed(candidates.size(), false);

    std::set<std::uint32_t> forcedReplacementIds;
    std::set<std::uint32_t> forcedTargetIds;
    for (const ForcedAssignment& fa : forcedAssignments) {
        if (fa.replacementBgmId == fa.targetBgmId) {
            throw std::invalid_argument("Forced music assignment cannot replace a slot with itself");
        }
        if (!forcedReplacementIds.insert(fa.replacementBgmId).second) {
            throw std::invalid_argument("A forced replacement song was selected more than once");
        }
        if (!forcedTargetIds.insert(fa.targetBgmId).second) {
            throw std::invalid_argument("A forced target slot was selected more than once");
        }
        if (isIncompatibleReplacement(fa.targetBgmId, fa.replacementBgmId)) {
            throw std::invalid_argument("Forced music assignment is an incompatible replacement");
        }

        // Find the target slot in pool.
        std::size_t targetIdx = pool.size();
        for (std::size_t i = 0; i < pool.size(); ++i) {
            const bool reliableTarget = pool[i]->bmsName != nullptr || pool[i]->trigger == BgmTrigger::Main;
            if (pool[i]->bgmId == fa.targetBgmId && reliableTarget && !slotForced[i]) {
                targetIdx = i;
                break;
            }
        }
        // Find the replacement candidate (must not already be used).
        std::size_t replIdx = candidates.size();
        for (std::size_t k = 0; k < candidates.size(); ++k) {
            if (candidates[k].bgmId == fa.replacementBgmId && !candidateUsed[k]) {
                replIdx = k;
                break;
            }
        }
        if (targetIdx == pool.size() || replIdx == candidates.size()) {
            throw std::invalid_argument("Forced music assignment is not in the selected pool");
        }

        slotForced[targetIdx]   = true;
        forcedCandidate[targetIdx] = candidates[replIdx];
        candidateUsed[replIdx]  = true;
    }

    // Collect free slots and free candidates.
    std::vector<std::size_t> freeSlotIndices;
    std::vector<Candidate>   freeCandidates;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (!slotForced[i]) freeSlotIndices.push_back(i);
    }
    for (std::size_t k = 0; k < candidates.size(); ++k) {
        if (!candidateUsed[k]) freeCandidates.push_back(candidates[k]);
    }

    // Shuffle free candidates into free slots, respecting incompatibilities.
    std::mt19937 rng(seed);
    const auto freeValid = [&]() {
        for (std::size_t fi = 0; fi < freeSlotIndices.size(); ++fi) {
            const std::size_t i = freeSlotIndices[fi];
            if (pool[i]->bgmId == freeCandidates[fi].bgmId
                || isIncompatibleReplacement(pool[i]->bgmId, freeCandidates[fi].bgmId))
                return false;
        }
        return true;
    };

    constexpr int kMaxShuffleAttempts = 256;
    for (int attempt = 0; attempt < kMaxShuffleAttempts; ++attempt) {
        std::shuffle(freeCandidates.begin(), freeCandidates.end(), rng);
        if (freeValid()) break;
    }
    if (!freeValid()) {
        for (std::size_t offset = 1; offset < freeCandidates.size(); ++offset) {
            std::rotate(freeCandidates.begin(), freeCandidates.begin() + 1, freeCandidates.end());
            if (freeValid()) break;
        }
    }
    if (!freeValid()) {
        throw std::runtime_error("Forced music assignments leave no valid shuffle for the remaining songs");
    }

    // Assemble final assignment list.
    std::vector<MusicSlotAssignment> assignments;
    assignments.reserve(pool.size());
    std::size_t freeIdx = 0;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        const BgmEntry& original = *pool[i];
        const Candidate& repl = slotForced[i] ? forcedCandidate[i] : freeCandidates[freeIdx++];
        assignments.push_back({original.bgmId, repl.bgmId, repl.wave1, repl.wave2, repl.kind, repl.noEnemyMusic});
    }
    return assignments;
}

std::vector<MusicSlotAssignment> buildMusicAssignments(std::uint32_t seed) {
    return buildMusicAssignments(seed, MusicBgmPoolMode::All);
}

bool isIncompatibleReplacement(std::uint32_t originalBgmId, std::uint32_t replacementBgmId) noexcept {
    for (const auto& pair : kIncompatibleReplacements) {
        if (pair.first == originalBgmId && pair.second == replacementBgmId) {
            return true;
        }
    }
    return false;
}

}  // namespace tpcm
