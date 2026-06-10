#include "dusk/custom_music/CustomMusicProject.h"

#include <tpcm/TprsReader.h>

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dusk::custom_music {
namespace {

std::vector<std::uint8_t> readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), {}};
}


void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write " + temporary.string());
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("Failed writing " + temporary.string());
    }
    const fs::path previous = path.string() + ".previous";
    std::error_code ignored;
    fs::remove(previous, ignored);
    fs::rename(path, previous);
    try {
        fs::rename(temporary, path);
        fs::remove(previous);
    } catch (...) {
        fs::remove(path, ignored);
        fs::rename(previous, path);
        throw;
    }
}

std::pair<int, int> midiBankProgram(int gameBank, int gameProgram) {
    if (gameBank == 11 && gameProgram >= 128) return {111, gameProgram - 128};
    if (gameBank == 11) return {11, gameProgram};
    if (gameBank == 12) return {12, gameProgram};
    if (gameBank == 13) return {13, gameProgram};
    if (gameBank == 50) return {50, gameProgram};
    if (gameBank == 51) return {51, gameProgram};
    if (gameBank == 52 && gameProgram >= 128) return {152, gameProgram - 128};
    if (gameBank == 52) return {52, gameProgram};
    if (gameBank == 53 && gameProgram >= 128) return {153, gameProgram - 128};
    if (gameBank == 53) return {53, gameProgram};
    throw std::runtime_error("This game bank/program cannot be represented in a standard MIDI file.");
}

void saveMidiEdits(const CustomSong& song) {
    tpcm::MidiFile midi = tpcm::parseMidi(readBytes(song.path));
    if (midi.tracks.empty()) midi.tracks.push_back({});
    for (const auto& [channel, edit] : song.overrides) {
        for (auto& track : midi.tracks) {
            std::erase_if(track.events, [&](const tpcm::MidiEvent& event) {
                if (event.bytes.empty() || (event.bytes[0] & 0xf0) == 0xf0
                    || (event.bytes[0] & 0x0f) != channel) return false;
                const std::uint8_t type = event.bytes[0] & 0xf0;
                if ((edit.program >= 0 || edit.bank >= 0) && type == 0xc0) return true;
                if (type != 0xb0 || event.bytes.size() < 3) return false;
                return (edit.bank >= 0 && event.bytes[1] == 0)
                    || (edit.volume >= 0 && event.bytes[1] == 7)
                    || (edit.pan >= 0 && event.bytes[1] == 10)
                    || (edit.reverb >= 0 && event.bytes[1] == 91);
            });
        }
        auto& events = midi.tracks.front().events;
        std::vector<tpcm::MidiEvent> setupEvents;
        if (edit.bank >= 0 || edit.program >= 0) {
            int gameBank = edit.bank >= 0 ? edit.bank : 11;
            int gameProgram = edit.program >= 0 ? edit.program : 0;
            const auto summary = std::find_if(song.channels.begin(), song.channels.end(),
                [&](const tpcm::MidiChannelSummary& row) { return row.channel == channel; });
            if (summary != song.channels.end()) {
                if (edit.bank < 0) gameBank = summary->gameBank;
                if (edit.program < 0) gameProgram = summary->gameProgram;
            }
            const auto [bank, program] = midiBankProgram(gameBank, gameProgram);
            setupEvents.push_back({0, {static_cast<std::uint8_t>(0xb0 | channel), 0, static_cast<std::uint8_t>(bank)}});
            setupEvents.push_back({0, {static_cast<std::uint8_t>(0xc0 | channel), static_cast<std::uint8_t>(program)}});
        }
        const auto addController = [&](int controller, int value) {
            if (value >= 0) setupEvents.push_back({0, {static_cast<std::uint8_t>(0xb0 | channel),
                static_cast<std::uint8_t>(controller), static_cast<std::uint8_t>(std::clamp(value, 0, 127))}});
        };
        addController(7, edit.volume);
        addController(10, edit.pan);
        addController(91, edit.reverb);
        events.insert(events.begin(), setupEvents.begin(), setupEvents.end());
    }
    writeBytes(song.path, tpcm::serializeMidi(midi));
}
std::string lowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool isMidiExt(const fs::path& path) {
    const std::string ext = lowerExt(path);
    return ext == ".mid" || ext == ".midi";
}

bool isTprsExt(const fs::path& path) {
    return lowerExt(path) == ".tprs";
}

bool supported(const fs::path& path) {
    const std::string ext = lowerExt(path);
    return ext == ".mid" || ext == ".midi" || ext == ".tprs";
}

// When a MIDI and a TPRS share the same name in the same folder, the TPRS wins:
// drop the shadowed MIDI so it cannot linger in memory and be replaced twice.
void dropShadowedMidis(std::vector<CustomSong>& library, int& selectedSong) {
    std::set<fs::path> tprsStems;
    for (const CustomSong& song : library) {
        if (isTprsExt(song.path)) tprsStems.insert(song.path.parent_path() / song.path.stem());
    }
    if (tprsStems.empty()) return;

    fs::path selectedPath;
    if (selectedSong >= 0 && selectedSong < static_cast<int>(library.size())) {
        selectedPath = library[static_cast<std::size_t>(selectedSong)].path;
    }
    std::erase_if(library, [&](const CustomSong& song) {
        return isMidiExt(song.path)
            && tprsStems.count(song.path.parent_path() / song.path.stem()) > 0;
    });
    if (!selectedPath.empty()) {
        const auto match = std::find_if(library.begin(), library.end(),
            [&](const CustomSong& song) { return song.path == selectedPath; });
        selectedSong = match == library.end()
            ? (library.empty() ? -1 : 0)
            : static_cast<int>(std::distance(library.begin(), match));
    } else if (selectedSong >= static_cast<int>(library.size())) {
        selectedSong = library.empty() ? -1 : static_cast<int>(library.size()) - 1;
    }
}

void parseOverrides(const std::string& text,
                    std::map<int, tpcm::MidiToBmsOptions::ChannelOverride>& overrides) {
    if (text.empty()) {
        return;
    }
    const json values = json::parse(text, nullptr, false);
    if (!values.is_object()) {
        return;
    }
    for (const auto& [key, value] : values.items()) {
        int channel = -1;
        try {
            channel = std::stoi(key);
        } catch (...) {
            continue;
        }
        if (channel < 0 || channel > 15 || !value.is_object()) {
            continue;
        }
        auto& edit = overrides[channel];
        edit.bank = value.value("game_bank", value.value("bank", -1));
        edit.program = value.value("program", value.value("prog", -1));
        edit.volume = value.value("volume", -1);
        edit.pan = value.value("pan", -1);
        edit.reverb = value.value("reverb", -1);
        if (value.contains("drum")) {
            edit.drumSet = true;
            edit.drum = value.value("drum", false);
        }
        edit.mute = value.value("mute", false);
    }
}

CustomSong readSong(const fs::path& path) {
    CustomSong song;
    song.path = path;
    song.title = path.stem().string();
    song.isTprs = path.extension() == ".tprs" || path.extension() == ".TPRS";
    song.available = fs::is_regular_file(path);
    if (!song.available) {
        return song;
    }

    std::vector<std::uint8_t> midi;
    if (song.isTprs) {
        const tpcm::TprsPackage package = tpcm::TprsReader(path).read();
        midi = package.midiBytes;
        song.tprsMetaJson = package.meta.rawJson;
        if (!package.meta.title.empty()) {
            song.title = package.meta.title;
        }
        song.author = package.meta.author;
        song.masterVolume = package.meta.masterVol;
        song.loopEnabled = package.meta.loopEnabled;
        song.loopStart = package.meta.loopStart;
        song.loopEnd = package.meta.loopEnd;
        song.drumChannels.insert(package.meta.drumChannels.begin(), package.meta.drumChannels.end());
        parseOverrides(package.meta.channelOverridesJson, song.overrides);
        song.noEnemyMusic = package.meta.noEnemyMusic;
    } else {
        midi = readBytes(path);
    }
    song.channels = tpcm::summarizeMidiChannels(tpcm::parseMidi(midi));
    return song;
}

json overrideJson(const tpcm::MidiToBmsOptions::ChannelOverride& edit) {
    json out = json::object();
    if (edit.bank >= 0) out["game_bank"] = edit.bank;
    if (edit.program >= 0) out["program"] = edit.program;
    if (edit.volume >= 0) out["volume"] = edit.volume;
    if (edit.pan >= 0) out["pan"] = edit.pan;
    if (edit.reverb >= 0) out["reverb"] = edit.reverb;
    if (edit.drumSet) out["drum"] = edit.drum;
    if (edit.mute) out["mute"] = true;
    return out;
}

}  // namespace

void CustomMusicProject::importPath(const fs::path& path) {
    std::vector<fs::path> paths;
    if (fs::is_directory(path)) {
        lastImportDirectory = path;
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file() && supported(entry.path())) {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
    } else if (fs::is_regular_file(path) && supported(path)) {
        lastImportDirectory = path.parent_path();
        paths.push_back(path);
    } else {
        throw std::runtime_error("No supported MIDI or TPRS files found at " + path.string());
    }

    for (const fs::path& songPath : paths) {
        const auto existing = std::find_if(library.begin(), library.end(), [&](const CustomSong& song) {
            return song.path == songPath;
        });
        CustomSong loaded = readSong(songPath);
        if (existing == library.end()) {
            library.push_back(std::move(loaded));
        } else {
            *existing = std::move(loaded);
        }
    }
    dropShadowedMidis(library, selectedSong);
    if (selectedSong < 0 && !library.empty()) {
        selectedSong = 0;
    }
    forcedAssignments.clear();
}

void CustomMusicProject::refresh() {
    fs::path selectedPath;
    if (selectedSong >= 0 && selectedSong < static_cast<int>(library.size())) {
        selectedPath = library[static_cast<std::size_t>(selectedSong)].path;
    }

    // Re-read entries that still exist (preserving in-memory metadata edits) and
    // drop any whose backing file has been deleted.
    std::vector<CustomSong> updated;
    updated.reserve(library.size());
    for (CustomSong& song : library) {
        fs::path pathToTry = song.path;
        if (!fs::is_regular_file(pathToTry) && !lastImportDirectory.empty()) {
            const fs::path candidate = lastImportDirectory / song.path.filename();
            if (fs::is_regular_file(candidate))
                pathToTry = candidate;
        }
        if (!fs::is_regular_file(pathToTry)) {
            continue;  // File deleted on disk -> remove it from the library.
        }
        try {
            CustomSong refreshed = readSong(pathToTry);
            if (!refreshed.available) continue;
            refreshed.title = song.title;
            refreshed.author = song.author;
            refreshed.masterVolume = song.masterVolume;
            refreshed.loopEnabled = song.loopEnabled;
            refreshed.loopStart = song.loopStart;
            refreshed.loopEnd = song.loopEnd;
            refreshed.drumChannels = song.drumChannels;
            refreshed.overrides = song.overrides;
            refreshed.path = pathToTry;
            updated.push_back(std::move(refreshed));
        } catch (...) {
            // Unreadable -> treat as gone and drop it.
        }
    }
    library = std::move(updated);

    // Pick up files newly added to the import directory.
    if (!lastImportDirectory.empty() && fs::is_directory(lastImportDirectory)) {
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(lastImportDirectory)) {
            if (entry.is_regular_file() && supported(entry.path())) {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const fs::path& songPath : paths) {
            const bool known = std::any_of(library.begin(), library.end(),
                [&](const CustomSong& song) { return song.path == songPath; });
            if (known) continue;
            try {
                CustomSong loaded = readSong(songPath);
                if (loaded.available) library.push_back(std::move(loaded));
            } catch (...) {
                // Skip files that cannot be read.
            }
        }
    }

    // Restore the previous selection by path before deduping, so it survives the
    // add/remove churn above.
    selectedSong = -1;
    if (!selectedPath.empty()) {
        const auto match = std::find_if(library.begin(), library.end(),
            [&](const CustomSong& song) { return song.path == selectedPath; });
        if (match != library.end()) {
            selectedSong = static_cast<int>(std::distance(library.begin(), match));
        }
    }

    dropShadowedMidis(library, selectedSong);
    if (selectedSong < 0 && !library.empty()) {
        selectedSong = 0;
    }
}

void CustomMusicProject::removeSong(std::size_t index) {
    if (index >= library.size()) {
        return;
    }
    library.erase(library.begin() + static_cast<std::ptrdiff_t>(index));
    forcedAssignments.clear();
    if (library.empty()) {
        selectedSong = -1;
    } else if (selectedSong >= static_cast<int>(library.size())) {
        selectedSong = static_cast<int>(library.size()) - 1;
    }
}

void CustomMusicProject::saveSelectedSource() {
    if (selectedSong < 0 || selectedSong >= static_cast<int>(library.size())) {
        throw std::runtime_error("No custom song is selected.");
    }
    CustomSong& song = library[static_cast<std::size_t>(selectedSong)];
    if (!song.available) throw std::runtime_error("The selected song is unavailable.");
    if (song.isTprs) {
        tpcm::updateTprsMeta(song.path, buildTprsMetadata(song));
        song = readSong(song.path);
    } else {
        const CustomSong settings = song;
        saveMidiEdits(song);
        song = readSong(song.path);
        song.title = settings.title;
        song.author = settings.author;
        song.masterVolume = settings.masterVolume;
        song.loopEnabled = settings.loopEnabled;
        song.loopStart = settings.loopStart;
        song.loopEnd = settings.loopEnd;
        song.drumChannels = settings.drumChannels;
        song.overrides = settings.overrides;
    }
}

void CustomMusicProject::packSelectedMidiToTprs() {
    if (selectedSong < 0 || selectedSong >= static_cast<int>(library.size())) {
        throw std::runtime_error("No custom song is selected.");
    }
    CustomSong& song = library[static_cast<std::size_t>(selectedSong)];
    if (!song.available) throw std::runtime_error("The selected song is unavailable.");
    if (song.isTprs) throw std::runtime_error("Selected song is already a TPRS.");

    const std::vector<std::uint8_t> midiBytes = readBytes(song.path);
    const std::string metaJson = buildTprsMetadata(song);
    const std::string midiName = song.path.filename().string();
    const fs::path tprsPath = song.path.parent_path() / (song.path.stem().string() + ".tprs");
    tpcm::createTprs(tprsPath, midiBytes, midiName, metaJson, {0});
    song = readSong(tprsPath);
}

void CustomMusicProject::load(const fs::path& path) {
    if (!fs::exists(path)) {
        return;
    }
    std::ifstream input(path);
    const json root = json::parse(input, nullptr, false);
    if (!root.is_object()) {
        return;
    }
    cleanSourceIso = root.value("clean_source_iso", "");
    lastImportDirectory = root.value("last_import_directory", "");
    randomizerSeed = root.value("randomizer_seed", 12345);
    poolMode = static_cast<tpcm::MusicBgmPoolMode>(root.value("pool_mode", 2));
    replacementTarget = root.value("replacement_target", 0);
    showReplacementSpoilers = root.value("show_replacement_spoilers", false);
    disableEnemyMusicGlobal = root.value("disable_enemy_music_global", false);
    library.clear();
    for (const auto& value : root.value("library", json::array())) {
        if (!value.is_object()) continue;
        CustomSong song = readSong(value.value("path", ""));
        library.push_back(std::move(song));
    }
    forcedAssignments.clear();
    for (const auto& value : root.value("forced_assignments", json::array())) {
        if (!value.is_object()) continue;
        forcedAssignments.push_back({
            value.value("replacement_bgm_id", 0U),
            value.value("target_bgm_id", 0U),
        });
    }
    dropShadowedMidis(library, selectedSong);
    selectedSong = library.empty() ? -1 : 0;
}

void CustomMusicProject::save(const fs::path& path) const {
    fs::create_directories(path.parent_path());
    json root = {
        {"version", 1},
        {"clean_source_iso", cleanSourceIso.string()},
        {"last_import_directory", lastImportDirectory.string()},
        {"randomizer_seed", randomizerSeed},
        {"pool_mode", static_cast<int>(poolMode)},
        {"replacement_target", replacementTarget},
        {"show_replacement_spoilers", showReplacementSpoilers},
        {"disable_enemy_music_global", disableEnemyMusicGlobal},
        {"library", json::array()},
        {"forced_assignments", json::array()},
    };
    for (const CustomSong& song : library) {
        root["library"].push_back({
            {"path", song.path.string()},
        });
    }
    for (const auto& assignment : forcedAssignments) {
        root["forced_assignments"].push_back({
            {"replacement_bgm_id", assignment.replacementBgmId},
            {"target_bgm_id", assignment.targetBgmId},
        });
    }
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Cannot save custom music library to " + temporary.string());
        }
        output << root.dump(2);
        if (!output) {
            throw std::runtime_error("Failed writing custom music library to " + temporary.string());
        }
    }
    const fs::path previous = path.string() + ".previous";
    std::error_code ignored;
    fs::remove(previous, ignored);
    if (fs::exists(path)) fs::rename(path, previous);
    try {
        fs::rename(temporary, path);
        fs::remove(previous, ignored);
    } catch (...) {
        fs::remove(path, ignored);
        if (fs::exists(previous)) fs::rename(previous, path);
        throw;
    }
}

std::vector<std::uint8_t> loadSongMidi(const CustomSong& song) {
    if (!song.available) {
        throw std::runtime_error("Custom song is unavailable: " + song.path.string());
    }
    if (song.isTprs) {
        return tpcm::TprsReader(song.path).read().midiBytes;
    }
    return readBytes(song.path);
}

tpcm::MidiToBmsOptions buildSongOptions(const CustomSong& song) {
    tpcm::MidiToBmsOptions options;
    options.masterVol = song.masterVolume;
    options.enableLoops = song.loopEnabled;
    if (song.loopEnabled && song.loopEnd > song.loopStart) {
        options.loopStart = song.loopStart;
        options.loopEnd = song.loopEnd;
    }
    options.drumChannels = song.drumChannels;
    options.channelOverrides = song.overrides;
    for (const auto& [channel, edit] : song.overrides) {
        if (!edit.drumSet) continue;
        if (edit.drum) options.drumChannels.insert(channel);
        else options.drumChannels.erase(channel);
    }
    return options;
}

std::string buildTprsMetadata(const CustomSong& song) {
    json metadata = json::parse(song.tprsMetaJson, nullptr, false);
    if (!metadata.is_object()) metadata = json::object();
    json overrides = json::object();
    std::set<int> drums = song.drumChannels;
    for (const auto& [channel, edit] : song.overrides) {
        const json value = overrideJson(edit);
        if (!value.empty()) overrides[std::to_string(channel)] = value;
        if (edit.drumSet) {
            if (edit.drum) drums.insert(channel);
            else drums.erase(channel);
        }
    }
    metadata["title"] = song.title;
    metadata["author"] = song.author;
    metadata["drum_channels"] = drums;
    metadata["master_vol"] = song.masterVolume;
    metadata["loop_enabled"] = song.loopEnabled;
    metadata["loop_start"] = song.loopStart;
    metadata["loop_end"] = song.loopEnd;
    metadata["channel_overrides"] = overrides;
    if (song.noEnemyMusic) metadata["no_enemy_music"] = true;
    return metadata.dump(2);
}

}  // namespace dusk::custom_music
