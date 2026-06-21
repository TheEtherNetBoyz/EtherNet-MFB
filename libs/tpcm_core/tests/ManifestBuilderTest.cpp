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
        {0x0100009Eu, 0x0100009Eu, 90, 0, tpcm::MusicAssignmentKind::CustomTprs},
        {0x0100004Au, 0x0100004Au, 90, 0, tpcm::MusicAssignmentKind::CustomTprs},
        {0x0100004Bu, 0x0100004Au, 90, 0, tpcm::MusicAssignmentKind::CustomTprs},
        {0x01000041u, 0x01000041u, 0, 0, tpcm::MusicAssignmentKind::Vanilla},
    };

    const tpcm::Manifest manifest =
        tpcm::buildManifest(assignments, banks, tpcm::BGM_TABLE, tpcm::BGM_TABLE_SIZE);

    assert(manifest.schema == 1);
    assert(manifest.generatedBanks.size() == 2);
    assert(manifest.entries.size() == 6);

    const tpcm::ManifestEntry& vanilla = manifest.entries[0];
    assert(vanilla.route == "scene");
    assert(vanilla.originalBgmId == 0x01000005u);
    assert(vanilla.replacementBgmId == 0x01000010u);
    assert(vanilla.bgmWaves.size() == 1);
    assert(vanilla.bgmWaves[0] == 1);
    assert(vanilla.preserveBgmWaves.size() == 2);
    assert(vanilla.preserveBgmWaves[0] == 3);
    assert(vanilla.preserveBgmWaves[1] == 4);
    assert(vanilla.targetBgmWaves.size() == 2);
    assert(vanilla.targetBgmWaves[0] == 3);
    assert(vanilla.targetBgmWaves[1] == 4);
    assert(vanilla.kind == "vanilla_swap");

    const tpcm::ManifestEntry& custom = manifest.entries[1];
    assert(custom.route == "scene");
    assert(custom.originalBgmId == 0x01000000u);
    assert(custom.replacementBgmId == 0x01000000u);
    assert(custom.bgmWaves.size() == 1);
    assert(custom.bgmWaves[0] == 90);
    assert(custom.preserveBgmWaves.size() == 1);
    assert(custom.preserveBgmWaves[0] == 25);
    assert(custom.targetBgmWaves.size() == 1);
    assert(custom.targetBgmWaves[0] == 25);
    assert(custom.kind == "custom");

    const tpcm::Manifest randomizedCustom = tpcm::buildManifest(
        {{0x01000001u, 0x01000000u, 90, 0, tpcm::MusicAssignmentKind::CustomTprs}},
        {}, tpcm::BGM_TABLE, tpcm::BGM_TABLE_SIZE);
    assert(randomizedCustom.entries.size() == 1);
    assert(randomizedCustom.entries[0].originalBgmId == 0x01000001u);
    assert(randomizedCustom.entries[0].replacementBgmId == 0x01000000u);

    const tpcm::Manifest fieldToTot = tpcm::buildManifest(
        {{0x01000000u, 0x0100003Cu, 38, 64, tpcm::MusicAssignmentKind::VanillaSwap}},
        {}, tpcm::BGM_TABLE, tpcm::BGM_TABLE_SIZE);
    assert(fieldToTot.entries.size() == 1);
    assert(fieldToTot.entries[0].bgmWaves.size() == 1);
    assert(fieldToTot.entries[0].bgmWaves[0] == 38);
    assert(fieldToTot.entries[0].preserveBgmWaves.size() == 1);
    assert(fieldToTot.entries[0].preserveBgmWaves[0] == 25);
    assert(fieldToTot.entries[0].targetBgmWaves.size() == 1);
    assert(fieldToTot.entries[0].targetBgmWaves[0] == 25);

    const tpcm::ManifestEntry& boss = manifest.entries[2];
    assert(boss.route == "main");
    assert(boss.originalBgmId == 0x01000086u);
    assert(boss.replacementBgmId == 0x01000087u);
    assert(boss.bgmWaves.size() == 1);
    assert(boss.bgmWaves[0] == 74);
    assert(boss.preserveBgmWaves.size() == 1);
    assert(boss.preserveBgmWaves[0] == 73);

    const tpcm::ManifestEntry& postman = manifest.entries[3];
    assert(postman.route == "sub");
    assert(postman.originalBgmId == 0x0100009Eu);
    assert(postman.bgmWaves.size() == 1);
    assert(postman.bgmWaves[0] == 90);
    assert(postman.preserveBgmWaves.size() == 1);
    assert(postman.preserveBgmWaves[0] == 25);
    assert(postman.targetBgmWaves.size() == 1);
    assert(postman.targetBgmWaves[0] == 25);

    const tpcm::ManifestEntry& snowpeak = manifest.entries[4];
    assert(snowpeak.route == "scene");
    assert(snowpeak.originalBgmId == 0x0100004Au);
    assert(snowpeak.bgmWaves.size() == 1);
    assert(snowpeak.bgmWaves[0] == 90);
    assert(snowpeak.preserveBgmWaves.size() == 2);
    assert(snowpeak.preserveBgmWaves[0] == 45);
    assert(snowpeak.preserveBgmWaves[1] == 58);
    assert(snowpeak.targetBgmWaves.size() == 2);
    assert(snowpeak.targetBgmWaves[0] == 45);
    assert(snowpeak.targetBgmWaves[1] == 58);

    const tpcm::ManifestEntry& snowboard = manifest.entries[5];
    assert(snowboard.route == "sub");
    assert(snowboard.originalBgmId == 0x0100004Bu);
    assert(snowboard.bgmWaves.size() == 1);
    assert(snowboard.bgmWaves[0] == 90);
    assert(snowboard.targetBgmWaves.size() == 1);
    assert(snowboard.targetBgmWaves[0] == 58);
    assert(snowboard.targetSeWaves.size() == 2);
    assert(snowboard.targetSeWaves[0] == 0x46);
    assert(snowboard.targetSeWaves[1] == 0x47);

    const std::string json = tpcm::toJson(manifest);
    assert(json.find("\"schema\": 1") != std::string::npos);
    assert(json.find("\"generated_banks\"") != std::string::npos);
    assert(json.find("\"entries\"") != std::string::npos);
    assert(json.find("\"original_bgm_id\": \"0x01000000\"") != std::string::npos);
    assert(json.find("\"replacement_bgm_id\": \"0x01000087\"") != std::string::npos);
    assert(json.find("\"bgm_waves\": [90]") != std::string::npos);
    assert(json.find("\"target_bgm_waves\": [25]") != std::string::npos);
    assert(json.find("\"target_se_waves\": [70, 71]") != std::string::npos);
    assert(json.find("\"preserve_bgm_waves\": [25]") != std::string::npos);
    assert(json.find("\"route\": \"sub\"") != std::string::npos);
    return 0;
}
