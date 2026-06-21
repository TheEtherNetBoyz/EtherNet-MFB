#pragma once

#include <tpcm/BmsConverter.h>
#include <tpcm/MusicRandomizer.h>

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dusk::custom_music {

struct CustomSong {
    std::filesystem::path path;
    std::string title;
    std::string author;
    bool isTprs = false;
    bool available = false;
    std::string tprsMetaJson;
    int masterVolume = 127;
    bool loopEnabled = true;
    int loopStart = 0;
    int loopEnd = 0;
    std::set<int> drumChannels;
    std::vector<tpcm::MidiChannelSummary> channels;
    std::map<int, tpcm::MidiToBmsOptions::ChannelOverride> overrides;
    bool noEnemyMusic = false;
};

struct CustomMusicProject {
    std::filesystem::path cleanSourceIso;
    std::filesystem::path lastImportDirectory;
    bool mirrorImportDirectory = false;
    std::vector<CustomSong> library;
    std::vector<tpcm::ForcedAssignment> forcedAssignments;
    int selectedSong = -1;
    int replacementTarget = 0;
    int randomizerSeed = 12345;
    tpcm::MusicBgmPoolMode poolMode = tpcm::MusicBgmPoolMode::All;
    bool showReplacementSpoilers = false;
    bool disableEnemyMusicGlobal = false;

    void importPath(const std::filesystem::path& path);
    void refresh();
    void removeSong(std::size_t index);
    void saveSelectedSource();
    void packSelectedMidiToTprs();
    void load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
};

std::vector<std::uint8_t> loadSongMidi(const CustomSong& song);
tpcm::MidiToBmsOptions buildSongOptions(const CustomSong& song);
std::string buildTprsMetadata(const CustomSong& song);

}  // namespace dusk::custom_music
