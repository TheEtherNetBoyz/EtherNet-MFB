#include "tpcm/BgmDatabase.h"

#include <cassert>
#include <cstdint>
#include <string_view>

namespace {

const tpcm::BgmEntry& RequireBgm(std::uint32_t bgmId) {
    const tpcm::BgmEntry* entry = tpcm::FindBgmById(bgmId);
    assert(entry != nullptr);
    return *entry;
}

}  // namespace

int main() {
    assert(tpcm::BGM_TABLE_SIZE == 172);

    const auto& fieldDay = RequireBgm(0x01000000);
    assert(std::string_view(fieldDay.bmsName) == "field_link_day.bms");
    assert(fieldDay.bgmWave1 == 25);
    assert(fieldDay.bgmWave2 == 0);
    assert(fieldDay.pinnedBank == 25);
    assert(fieldDay.sceneBgm);
    const auto fieldDayRuntime = tpcm::RuntimeInfoForBgm(fieldDay);
    assert(fieldDayRuntime.targetAllowed);
    assert(fieldDayRuntime.replacementAllowed);
    assert(fieldDayRuntime.autoShuffle);

    const auto& holyForest = RequireBgm(0x01000041);
    assert(std::string_view(holyForest.bmsName) == "holy_forest.bms");
    assert(holyForest.bgmWave1 == 42);
    assert(holyForest.bgmWave2 == 27);
    assert(holyForest.category == tpcm::BmsCategory::Overworld);

    const auto& hiddenVillage = RequireBgm(0x01000057);
    assert(std::string_view(hiddenVillage.bmsName) == "hiding_village.bms");
    assert(hiddenVillage.bgmWave1 == 49);
    assert(hiddenVillage.pinnedBank == 49);

    const auto& minigameRoom = RequireBgm(0x0100007A);
    assert(std::string_view(minigameRoom.bmsName) == "minigame_before.bms");
    assert(minigameRoom.bgmWave1 == 70);
    assert(minigameRoom.bgmWave2 == 70);

    const auto& ganonOne = RequireBgm(0x01000086);
    assert(std::string_view(ganonOne.bmsName) == "e_ganon01.bms");
    assert(ganonOne.trigger == tpcm::BgmTrigger::Main);
    assert(tpcm::RuntimeTriggerForBgm(ganonOne) == tpcm::BgmTrigger::Main);
    assert(ganonOne.bgmWave1 == 73);

    for (const auto bossId : {0x0100000Du, 0x0100000Eu, 0x01000025u, 0x01000030u,
                              0x01000031u, 0x0100004Cu, 0x0100004Du, 0x01000062u,
                              0x0100008Bu, 0x0100008Cu, 0x0100008Fu, 0x01000090u,
                              0x01000094u, 0x01000095u}) {
        assert(tpcm::RuntimeTriggerForBgm(RequireBgm(bossId)) == tpcm::BgmTrigger::Main);
    }

    const auto& postman = RequireBgm(0x0100009E);
    assert(postman.trigger == tpcm::BgmTrigger::Scene);
    assert(tpcm::RuntimeTriggerForBgm(postman) == tpcm::BgmTrigger::Sub);

    const auto& snowboard = RequireBgm(0x0100004B);
    const auto snowboardRuntime = tpcm::RuntimeInfoForBgm(snowboard);
    assert(snowboardRuntime.trigger == tpcm::BgmTrigger::Sub);
    assert(snowboardRuntime.bgmWaveCount == 1);
    assert(snowboardRuntime.bgmWaves[0] == 58);
    assert(snowboardRuntime.seWaveCount == 2);
    assert(snowboardRuntime.seWaves[0] == 0x46);
    assert(snowboardRuntime.seWaves[1] == 0x47);
    assert(snowboardRuntime.replacementAllowed);
    assert(snowboardRuntime.autoShuffle);

    const auto& fyrus = RequireBgm(0x01000025);
    assert(fyrus.bgmWave1 == 22);
    const auto fyrusRuntime = tpcm::RuntimeInfoForBgm(fyrus);
    assert(fyrusRuntime.trigger == tpcm::BgmTrigger::Main);
    assert(fyrusRuntime.bgmWaveCount == 1);
    assert(fyrusRuntime.bgmWaves[0] == 22);
    assert(fyrusRuntime.targetAllowed);
    assert(fyrusRuntime.replacementAllowed);
    assert(fyrusRuntime.autoShuffle);

    const auto& lake = RequireBgm(0x0100005A);
    const auto lakeRuntime = tpcm::RuntimeInfoForBgm(lake);
    assert(lakeRuntime.replacementAllowed);
    assert(lakeRuntime.autoShuffle);

    for (const auto automaticId : {0x01000000u, 0x01000001u, 0x0100003Bu, 0x0100003Fu,
                                   0x0100004Au, 0x0100004Bu, 0x0100005Au,
                                   0x01000065u, 0x01000078u, 0x0100007Au,
                                   0x0100009Eu}) {
        const auto automaticRuntime = tpcm::RuntimeInfoForBgm(RequireBgm(automaticId));
        assert(automaticRuntime.portability == tpcm::BgmPortability::Standalone);
        assert(!automaticRuntime.dynamicTracks);
        assert(automaticRuntime.targetAllowed);
        assert(automaticRuntime.replacementAllowed);
        assert(automaticRuntime.autoShuffle);
    }

    const auto& itemGet = RequireBgm(0x0100000A);
    assert(tpcm::RuntimeTriggerForBgm(itemGet) == tpcm::BgmTrigger::Fanfare);
    const auto itemGetRuntime = tpcm::RuntimeInfoForBgm(itemGet);
    assert(!itemGetRuntime.targetAllowed);
    assert(!itemGetRuntime.replacementAllowed);
    assert(tpcm::RuntimeTriggerForBgmId(0x02000000) == tpcm::BgmTrigger::Scene);

    assert(tpcm::FindBgmById(0x02000000) == nullptr);
    return 0;
}
