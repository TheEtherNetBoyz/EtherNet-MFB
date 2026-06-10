#include "tpcm/MusicRandomizer.h"

#include <cassert>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace {

void requireValidAssignments(const std::vector<tpcm::MusicSlotAssignment>& assignments) {
    std::set<std::uint32_t> originals;
    std::set<std::uint32_t> replacements;

    for (const tpcm::MusicSlotAssignment& assignment : assignments) {
        assert(assignment.kind == tpcm::MusicAssignmentKind::VanillaSwap);
        assert(assignment.originalBgmId != assignment.replacementBgmId);
        assert(!tpcm::isIncompatibleReplacement(assignment.originalBgmId, assignment.replacementBgmId));

        const tpcm::BgmEntry* replacement = tpcm::FindBgmById(assignment.replacementBgmId);
        assert(replacement != nullptr);
        assert(assignment.replacementWave1 == replacement->bgmWave1);
        assert(assignment.replacementWave2 == replacement->bgmWave2);

        assert(originals.insert(assignment.originalBgmId).second);
        assert(replacements.insert(assignment.replacementBgmId).second);
    }
}

}  // namespace

int main() {
    assert(tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Scene).size() == 46);
    assert(tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Dungeon).size() == 9);
    assert(tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::All).size() == 66);
    assert(tpcm::selectMusicBgmPool(tpcm::MusicBgmPoolMode::Boss).size() == 4);
    for (const tpcm::BgmEntry* entry : tpcm::selectCustomMusicSlots(tpcm::MusicBgmPoolMode::All)) {
        assert(entry->bmsName != nullptr);
    }
    for (const tpcm::BgmEntry* entry : tpcm::selectReliableMusicTargets(tpcm::MusicBgmPoolMode::All)) {
        assert(entry->bmsName != nullptr || entry->trigger == tpcm::BgmTrigger::Main);
    }

    const auto sceneAssignments = tpcm::buildMusicAssignments(0x12345678u, tpcm::MusicBgmPoolMode::Scene);
    assert(sceneAssignments.size() == 46);
    requireValidAssignments(sceneAssignments);

    const auto dungeonAssignments = tpcm::buildMusicAssignments(0x12345678u, tpcm::MusicBgmPoolMode::Dungeon);
    assert(dungeonAssignments.size() == 9);
    requireValidAssignments(dungeonAssignments);

    const auto allAssignments = tpcm::buildMusicAssignments(0x12345678u);
    assert(allAssignments.size() == 66);
    requireValidAssignments(allAssignments);

    const auto bossAssignments = tpcm::buildMusicAssignments(0x12345678u, tpcm::MusicBgmPoolMode::Boss);
    assert(bossAssignments.size() == 4);
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

    const auto forcedAssignments = tpcm::buildMusicAssignments(
        0x12345678u, tpcm::MusicBgmPoolMode::All, {},
        {{0x01000000u, 0x01000005u}});
    bool sawForced = false;
    for (const auto& assignment : forcedAssignments) {
        if (assignment.originalBgmId == 0x01000005u) {
            sawForced = true;
            assert(assignment.replacementBgmId == 0x01000000u);
        }
    }
    assert(sawForced);
    requireValidAssignments(forcedAssignments);

    const std::uint32_t customSlot =
        tpcm::selectCustomMusicSlots(tpcm::MusicBgmPoolMode::All)[0]->bgmId;
    const auto forcedCustomAssignments = tpcm::buildMusicAssignments(
        0x12345678u, tpcm::MusicBgmPoolMode::All,
        {{customSlot, 90, 0}},
        {{customSlot, 0x01000005u}});
    bool sawForcedCustom = false;
    for (const auto& assignment : forcedCustomAssignments) {
        if (assignment.originalBgmId == 0x01000005u) {
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
            {{0x01000000u, 0x01000000u}});
    } catch (const std::invalid_argument&) {
        rejectedSelf = true;
    }
    assert(rejectedSelf);

    bool rejectedOutsidePool = false;
    try {
        (void)tpcm::buildMusicAssignments(
            0x12345678u, tpcm::MusicBgmPoolMode::Boss, {},
            {{0x01000000u, 0x01000086u}});
    } catch (const std::invalid_argument&) {
        rejectedOutsidePool = true;
    }
    assert(rejectedOutsidePool);

    bool rejectedUnreliableTarget = false;
    try {
        (void)tpcm::buildMusicAssignments(
            0x12345678u, tpcm::MusicBgmPoolMode::All, {},
            {{0x01000000u, 0x01000006u}});
    } catch (const std::invalid_argument&) {
        rejectedUnreliableTarget = true;
    }
    assert(rejectedUnreliableTarget);

    assert(tpcm::isIncompatibleReplacement(0x0100003Eu, 0x01000094u));
    assert(!tpcm::isIncompatibleReplacement(0x01000000u, 0x01000001u));
    return 0;
}
