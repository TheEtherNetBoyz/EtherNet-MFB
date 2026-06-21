#include "tpcm/MusicRandomizer.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

bool containsBgm(const std::vector<const tpcm::BgmEntry*>& entries, std::uint32_t bgmId) {
    return std::find_if(entries.begin(), entries.end(), [bgmId](const tpcm::BgmEntry* entry) {
        return entry->bgmId == bgmId;
    }) != entries.end();
}

const tpcm::MusicCompatibilityEntry& requireReportEntry(
    const std::vector<tpcm::MusicCompatibilityEntry>& report, std::uint32_t bgmId) {
    const auto it = std::find_if(report.begin(), report.end(), [bgmId](const auto& entry) {
        return entry.entry != nullptr && entry.entry->bgmId == bgmId;
    });
    assert(it != report.end());
    return *it;
}

void requireValidAssignments(const std::vector<tpcm::MusicSlotAssignment>& assignments) {
    std::set<std::uint32_t> originals;
    std::set<std::uint32_t> replacements;

    for (const tpcm::MusicSlotAssignment& assignment : assignments) {
        assert(assignment.kind == tpcm::MusicAssignmentKind::VanillaSwap);
        assert(assignment.originalBgmId != assignment.replacementBgmId);
        assert(!tpcm::isIncompatibleReplacement(assignment.originalBgmId, assignment.replacementBgmId));

        const tpcm::BgmEntry* replacement = tpcm::FindBgmById(assignment.replacementBgmId);
        assert(replacement != nullptr);
        assert(tpcm::RuntimeInfoForBgm(*replacement).replacementAllowed);
        assert(assignment.replacementWave1 == replacement->bgmWave1);
        assert(assignment.replacementWave2 == replacement->bgmWave2);

        const tpcm::BgmEntry* original = tpcm::FindBgmById(assignment.originalBgmId);
        assert(original != nullptr);
        assert(tpcm::RuntimeInfoForBgm(*original).autoShuffle);

        assert(originals.insert(assignment.originalBgmId).second);
        assert(replacements.insert(assignment.replacementBgmId).second);
    }
}

}  // namespace

int main() {
    assert(tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Scene).size() == 46);
    assert(tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Dungeon).size() == 9);
    assert(tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::All).size() == 66);
    assert(!tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Boss).empty());
    for (const tpcm::BgmEntry* entry : tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::All)) {
        assert(tpcm::RuntimeInfoForBgm(*entry).autoShuffle);
    }
    for (const tpcm::BgmEntry* entry : tpcm::selectCustomMusicSlots(tpcm::MusicBgmPoolMode::All)) {
        assert(entry->bmsName != nullptr);
    }
    for (const tpcm::BgmEntry* entry : tpcm::selectReliableMusicTargets(tpcm::MusicBgmPoolMode::All)) {
        assert(entry->bmsName != nullptr || tpcm::RuntimeTriggerForBgm(*entry) != tpcm::BgmTrigger::Scene);
    }
    const auto allPool = tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::All);
    const auto allTargets = tpcm::selectReliableMusicTargets(tpcm::MusicBgmPoolMode::All);
    const auto allReport = tpcm::buildMusicCompatibilityReport(tpcm::MusicBgmPoolMode::All);
    std::size_t automaticReportCount = 0;
    std::size_t targetOnlyReportCount = 0;
    for (const auto& entry : allReport) {
        if (entry.compatibility == tpcm::MusicSlotCompatibility::Automatic) {
            ++automaticReportCount;
        } else if (entry.compatibility == tpcm::MusicSlotCompatibility::TargetOnly) {
            ++targetOnlyReportCount;
        }
    }
    assert(automaticReportCount == allPool.size());
    assert(automaticReportCount + targetOnlyReportCount == allTargets.size());
    assert(targetOnlyReportCount == 0);
    for (const auto automaticId : {0x01000000u, 0x01000001u, 0x01000006u, 0x0100000Fu,
                                   0x01000011u, 0x01000024u, 0x0100003Bu,
                                   0x0100003Fu, 0x0100004Au, 0x0100004Bu,
                                   0x0100005Au, 0x01000065u, 0x01000078u,
                                   0x0100007Au, 0x0100009Eu}) {
        assert(requireReportEntry(allReport, automaticId).compatibility
               == tpcm::MusicSlotCompatibility::Automatic);
        assert(std::string_view(requireReportEntry(allReport, automaticId).reason)
               == "automatic shuffle");
        assert(containsBgm(allPool, automaticId));
    }

    const auto bossPool = tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Boss);
    const auto bossTargets = tpcm::selectReliableMusicTargets(tpcm::MusicBgmPoolMode::Boss);
    assert(containsBgm(bossTargets, 0x01000025u));
    assert(containsBgm(bossPool, 0x01000025u));

    const auto sceneAssignments = tpcm::buildMusicAssignments(0x12345678u, tpcm::MusicBgmPoolMode::Scene);
    assert(sceneAssignments.size() == tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Scene).size());
    requireValidAssignments(sceneAssignments);

    const auto dungeonAssignments = tpcm::buildMusicAssignments(0x12345678u, tpcm::MusicBgmPoolMode::Dungeon);
    assert(dungeonAssignments.size() == tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Dungeon).size());
    requireValidAssignments(dungeonAssignments);

    const auto allAssignments = tpcm::buildMusicAssignments(0x12345678u);
    assert(allAssignments.size() == tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::All).size());
    requireValidAssignments(allAssignments);

    const auto bossAssignments = tpcm::buildMusicAssignments(0x12345678u, tpcm::MusicBgmPoolMode::Boss);
    assert(bossAssignments.size() == tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Boss).size());
    requireValidAssignments(bossAssignments);

    const auto customAssignments = tpcm::buildMusicAssignments(
        0x12345678u, tpcm::MusicBgmPoolMode::All,
        {{tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::All)[0]->bgmId, 90, 0}});
    bool sawCustom = false;
    for (const auto& assignment : customAssignments) {
        if (assignment.kind == tpcm::MusicAssignmentKind::CustomTprs) {
            sawCustom = true;
            assert(assignment.replacementWave1 == 90);
        }
    }
    assert(sawCustom);

    const auto forcedPool = tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::All);
    assert(forcedPool.size() >= 2);
    const auto forcedAssignments = tpcm::buildMusicAssignments(
        0x12345678u, tpcm::MusicBgmPoolMode::All, {},
        {{forcedPool[1]->bgmId, forcedPool[0]->bgmId}});
    bool sawForced = false;
    for (const auto& assignment : forcedAssignments) {
        if (assignment.originalBgmId == forcedPool[0]->bgmId) {
            sawForced = true;
            assert(assignment.replacementBgmId == forcedPool[1]->bgmId);
        }
    }
    assert(sawForced);
    requireValidAssignments(forcedAssignments);

    const auto customSlots = tpcm::selectCustomMusicSlots(tpcm::MusicBgmPoolMode::All);
    assert(customSlots.size() >= 2);
    const std::uint32_t customSlot = customSlots[1]->bgmId;
    const std::uint32_t customTarget = customSlots[0]->bgmId;
    const auto forcedCustomAssignments = tpcm::buildMusicAssignments(
        0x12345678u, tpcm::MusicBgmPoolMode::All,
        {{customSlot, 90, 0}},
        {{customSlot, customTarget}});
    bool sawForcedCustom = false;
    for (const auto& assignment : forcedCustomAssignments) {
        if (assignment.originalBgmId == customTarget) {
            sawForcedCustom = true;
            assert(assignment.replacementBgmId == customSlot);
            assert(assignment.replacementWave1 == 90);
            assert(assignment.kind == tpcm::MusicAssignmentKind::CustomTprs);
        }
    }
    assert(sawForcedCustom);

    bool rejectedSelf = false;
    try {
        (void)tpcm::buildMusicAssignments(
            0x12345678u, tpcm::MusicBgmPoolMode::All, {},
            {{forcedPool[0]->bgmId, forcedPool[0]->bgmId}});
    } catch (const std::invalid_argument&) {
        rejectedSelf = true;
    }
    assert(rejectedSelf);

    bool rejectedOutsidePool = false;
    try {
        (void)tpcm::buildMusicAssignments(
            0x12345678u, tpcm::MusicBgmPoolMode::Boss, {},
            {{forcedPool[0]->bgmId, 0x01000086u}});
    } catch (const std::invalid_argument&) {
        rejectedOutsidePool = true;
    }
    assert(rejectedOutsidePool);

    assert(tpcm::isIncompatibleReplacement(0x0100003Eu, 0x01000094u));
    assert(!tpcm::isIncompatibleReplacement(0x01000000u, 0x01000001u));
    return 0;
}
