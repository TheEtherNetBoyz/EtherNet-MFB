#include "tpcm/TprsReader.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct ZipFixtureEntry {
    std::string name;
    std::string text;
};

void appendU16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void appendU32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    appendU16(data, static_cast<std::uint16_t>(value & 0xFFFF));
    appendU16(data, static_cast<std::uint16_t>((value >> 16) & 0xFFFF));
}

void appendText(std::vector<std::uint8_t>& data, const std::string& text) {
    data.insert(data.end(), text.begin(), text.end());
}

void writeStoredZip(const std::filesystem::path& path, const std::vector<ZipFixtureEntry>& entries) {
    std::vector<std::uint8_t> zip;
    std::vector<std::uint32_t> localOffsets;

    for (const ZipFixtureEntry& entry : entries) {
        localOffsets.push_back(static_cast<std::uint32_t>(zip.size()));
        appendU32(zip, 0x04034B50u);
        appendU16(zip, 20);
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU32(zip, 0);
        appendU32(zip, static_cast<std::uint32_t>(entry.text.size()));
        appendU32(zip, static_cast<std::uint32_t>(entry.text.size()));
        appendU16(zip, static_cast<std::uint16_t>(entry.name.size()));
        appendU16(zip, 0);
        appendText(zip, entry.name);
        appendText(zip, entry.text);
    }

    const std::uint32_t centralOffset = static_cast<std::uint32_t>(zip.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipFixtureEntry& entry = entries[i];
        appendU32(zip, 0x02014B50u);
        appendU16(zip, 20);
        appendU16(zip, 20);
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU32(zip, 0);
        appendU32(zip, static_cast<std::uint32_t>(entry.text.size()));
        appendU32(zip, static_cast<std::uint32_t>(entry.text.size()));
        appendU16(zip, static_cast<std::uint16_t>(entry.name.size()));
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU16(zip, 0);
        appendU32(zip, 0);
        appendU32(zip, localOffsets[i]);
        appendText(zip, entry.name);
    }
    const std::uint32_t centralSize = static_cast<std::uint32_t>(zip.size()) - centralOffset;

    appendU32(zip, 0x06054B50u);
    appendU16(zip, 0);
    appendU16(zip, 0);
    appendU16(zip, static_cast<std::uint16_t>(entries.size()));
    appendU16(zip, static_cast<std::uint16_t>(entries.size()));
    appendU32(zip, centralSize);
    appendU32(zip, centralOffset);
    appendU16(zip, 0);

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
}

std::filesystem::path fixturePath(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

int main() {
    const std::filesystem::path valid = fixturePath("tpcm_valid.tprs");
    writeStoredZip(valid,
                   {
                       {"song.mid", "MThd fake midi"},
                       {"categories.txt", "# metadata only\n1\n2\nbad\n12\n"},
                       {"meta.json",
                        "{\"title\":\"My Song\",\"author\":\"Someone\",\"drum_channels\":[9, 10],"
                        "\"master_vol\":100,\"loop_enabled\":false,\"loop_start\":12,\"loop_end\":345,"
                        "\"channel_overrides\":{\"0\":{\"program\":32}}}"},
                   });

    const tpcm::TprsPackage package = tpcm::TprsReader(valid).read();
    assert(package.midiBytes.size() == 14);
    assert(package.categories.size() == 3);
    assert(package.categories[0] == 1);
    assert(package.categories[1] == 2);
    assert(package.categories[2] == 12);
    assert(package.meta.title == "My Song");
    assert(package.meta.author == "Someone");
    assert(package.meta.drumChannels.size() == 2);
    assert(package.meta.drumChannels[0] == 9);
    assert(package.meta.drumChannels[1] == 10);
    assert(package.meta.masterVol == 100);
    assert(!package.meta.loopEnabled);
    assert(package.meta.loopStart == 12);
    assert(package.meta.loopEnd == 345);
    assert(!package.meta.channelOverridesJson.empty());
    assert(tpcm::isCompatibleWithCategory(package.categories, 2));
    assert(!tpcm::isCompatibleWithCategory(package.categories, 6));
    assert(tpcm::TprsReader(valid).validate().empty());
    tpcm::updateTprsMeta(valid,
        "{\"title\":\"Updated\",\"drum_channels\":[1],\"master_vol\":77,"
        "\"channel_overrides\":{\"2\":{\"program\":12,\"mute\":true}}}");
    const tpcm::TprsPackage updated = tpcm::TprsReader(valid).read();
    assert(updated.meta.title == "Updated");
    assert(updated.meta.masterVol == 77);
    assert(updated.meta.drumChannels == std::vector<int>({1}));
    assert(updated.meta.channelOverridesJson.find("\"mute\":true") != std::string::npos);
    assert(updated.midiBytes.size() == 14);
    assert(updated.categories.size() == 3);

    const std::filesystem::path noMeta = fixturePath("tpcm_no_meta.tprs");
    writeStoredZip(noMeta, {{"song.midi", "MThd"}, {"categories.txt", "0\n"}});
    const tpcm::TprsPackage noMetaPackage = tpcm::TprsReader(noMeta).read();
    assert(noMetaPackage.meta.title == "tpcm_no_meta");
    assert(noMetaPackage.meta.masterVol == 127);
    assert(tpcm::isCompatibleWithCategory(noMetaPackage.categories, 6));

    const std::filesystem::path invalid = fixturePath("tpcm_invalid.tprs");
    writeStoredZip(invalid, {{"song.mid", "MThd"}, {"categories.txt", "99\n"}});
    const std::vector<std::string> problems = tpcm::TprsReader(invalid).validate();
    assert(!problems.empty());

    std::filesystem::remove(valid);
    std::filesystem::remove(noMeta);
    std::filesystem::remove(invalid);
    return 0;
}
