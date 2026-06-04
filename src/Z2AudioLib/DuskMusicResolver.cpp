#include "Z2AudioLib/DuskMusicResolver.h"

#include "Z2AudioLib/Z2SceneMgr.h"
#include "dusk/io.hpp"
#include "dusk/settings.h"
#include "nlohmann/json.hpp"
#include "os_report.h"

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dusk::music {
namespace {

using json = nlohmann::json;

constexpr char ManifestFileName[] = "dusk_music_manifest.json";
constexpr u32 SeqIdMask = 0xFF000000;
constexpr u32 SeqIdPrefix = 0x01000000;

struct ManifestEntry {
    Route route = Route::Scene;
    u32 originalId = 0;
    u32 replacementId = 0;
    std::vector<u32> bgmWaves;
};

std::vector<ManifestEntry> s_entries;
std::filesystem::path s_loadedPath;
bool s_loaded = false;

std::filesystem::path manifestPath() {
    const std::string isoPath = dusk::getSettings().backend.isoPath.getValue();
    if (isoPath.empty()) {
        return {};
    }
    return std::filesystem::path(isoPath).parent_path() / ManifestFileName;
}

std::optional<u32> readId(const json& value) {
    try {
        if (value.is_number_unsigned() || value.is_number_integer()) {
            return value.get<u32>();
        }

        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            size_t end = 0;
            const unsigned long parsed = std::stoul(text, &end, 0);
            if (end == text.size()) {
                return static_cast<u32>(parsed);
            }
        }
    } catch (...) {
    }

    return std::nullopt;
}

std::optional<Route> readRoute(const json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }

    const std::string route = value.get<std::string>();
    if (route == "scene") {
        return Route::Scene;
    }
    if (route == "main" || route == "bgmStart") {
        return Route::Main;
    }
    if (route == "sub") {
        return Route::Sub;
    }
    if (route == "fanfare") {
        return Route::Fanfare;
    }

    return std::nullopt;
}

bool isSeqId(u32 soundId) {
    return (soundId & SeqIdMask) == SeqIdPrefix;
}

void appendWaves(ManifestEntry& entry, const json& value) {
    if (!value.is_array()) {
        return;
    }

    for (const json& waveValue : value) {
        if (entry.bgmWaves.size() >= MaxBgmWaves) {
            OS_REPORT("[DuskMusicResolver] Too many BGM waves for 0x%08x; truncating at %u\n",
                      entry.originalId, static_cast<u32>(MaxBgmWaves));
            return;
        }

        const auto wave = readId(waveValue);
        if (wave && *wave != 0) {
            entry.bgmWaves.push_back(*wave);
        }
    }
}

void ensureLoaded() {
    const std::filesystem::path path = manifestPath();
    if (s_loaded && path == s_loadedPath) {
        return;
    }

    s_entries.clear();
    s_loadedPath = path;
    s_loaded = true;

    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }

    try {
        const auto bytes = dusk::io::FileStream::ReadAllBytes(path);
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const json manifest = json::parse(text);
        if (!manifest.contains("entries") || !manifest["entries"].is_array()) {
            OS_REPORT("[DuskMusicResolver] Manifest has no entries array: %s\n", path.string().c_str());
            return;
        }

        for (const json& row : manifest["entries"]) {
            if (!row.is_object()) {
                OS_REPORT("[DuskMusicResolver] Skipping non-object manifest entry\n");
                continue;
            }

            const auto route = readRoute(row.value("route", json()));
            const auto originalId = readId(row.value("original_bgm_id", json()));
            const auto replacementId = readId(row.value("replacement_bgm_id", json()));
            if (!route || !originalId || !replacementId || !isSeqId(*originalId)
                || !isSeqId(*replacementId))
            {
                OS_REPORT("[DuskMusicResolver] Skipping malformed manifest entry\n");
                continue;
            }

            ManifestEntry entry;
            entry.route = *route;
            entry.originalId = *originalId;
            entry.replacementId = *replacementId;
            appendWaves(entry, row.value("bgm_waves", json::array()));
            appendWaves(entry, row.value("extra_bgm_waves", json::array()));
            appendWaves(entry, row.value("preserve_bgm_waves", json::array()));
            s_entries.push_back(std::move(entry));
        }

        OS_REPORT("[DuskMusicResolver] Loaded %u manifest entries from %s\n",
                  static_cast<u32>(s_entries.size()), path.string().c_str());
    } catch (const std::exception& e) {
        s_entries.clear();
        OS_REPORT("[DuskMusicResolver] Failed to load %s: %s\n", path.string().c_str(), e.what());
    } catch (...) {
        s_entries.clear();
        OS_REPORT("[DuskMusicResolver] Failed to load %s\n", path.string().c_str());
    }
}

const char* routeName(Route route) {
    switch (route) {
    case Route::Scene:
        return "scene";
    case Route::Main:
        return "main";
    case Route::Sub:
        return "sub";
    case Route::Fanfare:
        return "fanfare";
    }

    return "unknown";
}

} // namespace

ResolvedMusic Resolve(Route route, JAISoundID originalId) {
    ensureLoaded();

    const u32 original = static_cast<u32>(originalId);
    if (!isSeqId(original)) {
        return {};
    }

    for (const ManifestEntry& entry : s_entries) {
        if (entry.route != route || entry.originalId != original) {
            continue;
        }

        ResolvedMusic resolved;
        resolved.matched = true;
        resolved.originalId = JAISoundID(entry.originalId);
        resolved.replacementId = JAISoundID(entry.replacementId);
        resolved.bgmWaveCount = entry.bgmWaves.size();
        for (size_t i = 0; i < entry.bgmWaves.size() && i < resolved.bgmWaves.size(); ++i) {
            resolved.bgmWaves[i] = entry.bgmWaves[i];
        }

        return resolved;
    }

    return {};
}

void LoadResolvedBgmWaves(const ResolvedMusic& resolved, size_t startIndex) {
    if (!resolved.matched || startIndex >= resolved.bgmWaveCount) {
        return;
    }

    for (size_t i = startIndex; i < resolved.bgmWaveCount; ++i) {
        const u32 wave = resolved.bgmWaves[i];
        if (wave == 0) {
            continue;
        }

        if (!Z2GetSceneMgr()->loadBgmWave(wave)) {
            OS_REPORT("[DuskMusicResolver] Failed loading BGM wave %u for 0x%08x -> 0x%08x\n",
                      wave, static_cast<u32>(resolved.originalId),
                      static_cast<u32>(resolved.replacementId));
        }
    }
}

void ApplySceneResolution(JAISoundID& bgm, u8& bgmWave1, u8& bgmWave2) {
    const ResolvedMusic resolved = Resolve(Route::Scene, bgm);
    if (!resolved.matched) {
        return;
    }

    bgm = resolved.replacementId;
    bgmWave1 = 0;
    // bgmWave2 is preserved unless the manifest explicitly provides a second wave entry.
    // This matches ZSRTP's behaviour: BgmWave2 passes through sceneChange unchanged so
    // sub-BGM banks (e.g. Cowboy on Ranch) are loaded during normal scene setup.

    if (resolved.bgmWaveCount > 0) {
        if (resolved.bgmWaves[0] <= 0xFF) {
            bgmWave1 = static_cast<u8>(resolved.bgmWaves[0]);
        } else {
            LoadResolvedBgmWaves(resolved, 0);
        }
    }

    if (resolved.bgmWaveCount > 1) {
        if (resolved.bgmWaves[1] <= 0xFF) {
            bgmWave2 = static_cast<u8>(resolved.bgmWaves[1]);
        } else {
            LoadResolvedBgmWaves(resolved, 1);
        }
    }

    LoadResolvedBgmWaves(resolved, 2);
    OS_REPORT("[DuskMusicResolver] scene 0x%08x -> 0x%08x waves:%u,%u (+%u)\n",
              static_cast<u32>(resolved.originalId), static_cast<u32>(resolved.replacementId),
              bgmWave1, bgmWave2,
              resolved.bgmWaveCount > 2 ? static_cast<u32>(resolved.bgmWaveCount - 2) : 0);
}

JAISoundID ResolvePlaybackAndLoad(Route route, JAISoundID originalId) {
    const ResolvedMusic resolved = Resolve(route, originalId);
    if (!resolved.matched) {
        return originalId;
    }

    LoadResolvedBgmWaves(resolved);
    OS_REPORT("[DuskMusicResolver] %s 0x%08x -> 0x%08x waves:%u\n",
              routeName(route), static_cast<u32>(resolved.originalId),
              static_cast<u32>(resolved.replacementId),
              static_cast<u32>(resolved.bgmWaveCount));
    return resolved.replacementId;
}

} // namespace dusk::music
