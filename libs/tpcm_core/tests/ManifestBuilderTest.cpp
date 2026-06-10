#include "tpcm/ManifestBuilder.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    const std::vector<tpcm::GeneratedBank> banks = {
        {90, "field_link_day.bms", true},
        {91, "unused.bms", false},
    };

    const std::vector<tpcm::MusicSlotAssignment> assignments = {
        {0x01000005u, 0x01000010u, 1, 2, tpcm::MusicAssignmentKind::VanillaSwap},
        {0x01000000u, 0x01000000u, 90, 0, tpcm::MusicAssignmentKind::CustomTprs},
        {0x01000086u, 0x01000087u, 74, 0, tpcm::MusicAssignmentKind::VanillaSwap},
        {0x01000041u, 0x01000041u, 0, 0, tpcm::MusicAssignmentKind::Vanilla},
    };

    const tpcm::Manifest manifest =
        tpcm::buildManifest(assignments, banks, tpcm::BGM_TABLE, tpcm::BGM_TABLE_SIZE);

    assert(manifest.schema == 1);
    assert(manifest.generatedBanks.size() == 2);
    assert(manifest.entries.size() == 3);

    const tpcm::ManifestEntry& vanilla = manifest.entries[0];
    assert(vanilla.route == "scene");
    assert(vanilla.originalBgmId == 0x01000005u);
    assert(vanilla.replacementBgmId == 0x01000010u);
    assert(vanilla.bgmWaves.size() == 2);
    assert(vanilla.bgmWaves[0] == 1);
    assert(vanilla.bgmWaves[1] == 2);
    assert(vanilla.kind == "vanilla_swap");

    const tpcm::ManifestEntry& custom = manifest.entries[1];
    assert(custom.route == "scene");
    assert(custom.originalBgmId == 0x01000000u);
    assert(custom.replacementBgmId == 0x01000000u);
    assert(custom.bgmWaves.size() == 2);
    assert(custom.bgmWaves[0] == 90);
    assert(custom.bgmWaves[1] == 25);
    assert(custom.kind == "custom");

    const tpcm::Manifest randomizedCustom = tpcm::buildManifest(
        {{0x01000001u, 0x01000000u, 90, 0, tpcm::MusicAssignmentKind::CustomTprs}},
        {}, tpcm::BGM_TABLE, tpcm::BGM_TABLE_SIZE);
    assert(randomizedCustom.entries.size() == 1);
    assert(randomizedCustom.entries[0].originalBgmId == 0x01000001u);
    assert(randomizedCustom.entries[0].replacementBgmId == 0x01000000u);

    const tpcm::ManifestEntry& boss = manifest.entries[2];
    assert(boss.route == "main");
    assert(boss.originalBgmId == 0x01000086u);
    assert(boss.replacementBgmId == 0x01000087u);
    assert(boss.bgmWaves.size() == 1);
    assert(boss.bgmWaves[0] == 74);

    const std::string json = tpcm::toJson(manifest);
    assert(json.find("\"schema\": 1") != std::string::npos);
    assert(json.find("\"generated_banks\"") != std::string::npos);
    assert(json.find("\"entries\"") != std::string::npos);
    assert(json.find("\"original_bgm_id\": \"0x01000000\"") != std::string::npos);
    assert(json.find("\"replacement_bgm_id\": \"0x01000087\"") != std::string::npos);
    assert(json.find("\"bgm_waves\": [90, 25]") != std::string::npos);
    return 0;
}
