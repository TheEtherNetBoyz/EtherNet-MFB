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
    assert(ganonOne.bgmWave1 == 73);

    assert(tpcm::FindBgmById(0x02000000) == nullptr);
    return 0;
}
