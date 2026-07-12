#include "Z2AudioLib/DuskMusicResolver.h"

#include "Z2AudioLib/Z2SceneMgr.h"
#include "dusk/io.hpp"
#include "dusk/settings.h"
#include "nlohmann/json.hpp"
#include "os_report.h"
#include "tpcm/BgmDatabase.h"

#include <exception>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "dusk/logging.h"
#include <cstdarg>
#include <cstdio>

// Always-on diagnostic logging. Bypass OSReport because OSReportDisable()
// can drop late audio logs. Keep printf-style MUSIC_LOG call sites, format
// locally, then send the final line through the normal Dusk aurora logger.
// Tagged [music] for easy grepping.
static void MusicLogPrintf(const char* fmt, ...) {
    char buffer[2048];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    DuskLog.debug("[music] {}", buffer);
}

#define MUSIC_LOG(...) MusicLogPrintf(__VA_ARGS__)

namespace dusk::music {
namespace {

using json = nlohmann::json;

constexpr u32 SeqIdMask = 0xFF000000;
constexpr u32 SeqIdPrefix = 0x01000000;
constexpr size_t MaxSceneBgmWaves = MaxBgmWaves * 2;

struct ManifestEntry {
    Route route = Route::Scene;
    u32 originalId = 0;
    u32 replacementId = 0;
    std::vector<u32> bgmWaves;
    std::vector<u32> targetBgmWaves;
    std::vector<u32> targetSeWaves;
    std::string kind;
    bool noEnemyMusic = false;
};

std::vector<ManifestEntry> s_entries;
bool s_disableAllEnemyMusic = false;
std::filesystem::path s_loadedPath;
std::filesystem::file_time_type s_loadedMtime;
bool s_loaded = false;

// bankId -> number of source waves packed into it (from manifest generated_banks).
std::map<u32, u32> s_bankWaveCounts;

JAISoundID s_sceneOriginalId;
JAISoundID s_sceneReplacementId;
u32 s_sceneRequiredBgmWaves[MaxSceneBgmWaves] {};
size_t s_sceneRequiredBgmWaveCount = 0;
u32 s_sceneReleaseBgmWaves[MaxSceneBgmWaves] {};
size_t s_sceneReleaseBgmWaveCount = 0;
u32 s_temporarilyEvictedSceneBgmWaves[MaxSceneBgmWaves] {};
size_t s_temporarilyEvictedSceneBgmWaveCount = 0;
u32 s_temporaryRouteBgmWaves[MaxSceneBgmWaves] {};
size_t s_temporaryRouteBgmWaveCount = 0;
u32 s_mainRouteBgmWaves[MaxSceneBgmWaves] {};
size_t s_mainRouteBgmWaveCount = 0;
bool s_allowSceneRequiredEviction = false;

void ensureLoaded();
const char* routeName(Route route);

const char* bgmName(u32 bgmId) {
    const tpcm::BgmEntry* entry = tpcm::FindBgmById(bgmId);
    return entry != nullptr ? entry->displayName : "?";
}

const char* waveStatusName(int status) {
    switch (status) {
    case 0:
        return "not_loaded";
    case 1:
        return "loading";
    case 2:
        return "loaded";
    }
    return "unknown";
}

bool waveIsAvailableOrLoading(int status) {
    return status == 1 || status == 2;
}

// Render a wave-bank list for logging, annotating each generated custom bank
// with its packed wave count, e.g. "90(123w),27".
std::string waveListStr(const u32* waves, size_t count) {
    std::string s;
    for (size_t i = 0; i < count; ++i) {
        if (!s.empty()) {
            s += ',';
        }
        s += std::to_string(waves[i]);
        const auto it = s_bankWaveCounts.find(waves[i]);
        if (it != s_bankWaveCounts.end()) {
            s += '(' + std::to_string(it->second) + "w)";
        }
    }
    return s.empty() ? std::string("-") : s;
}

std::string runtimeWaveListStr(const std::uint8_t* waves, size_t count) {
    u32 expanded[tpcm::kMaxRuntimeBgmWaves] {};
    const size_t clampedCount = std::min(count, tpcm::kMaxRuntimeBgmWaves);
    for (size_t i = 0; i < clampedCount; ++i) {
        expanded[i] = waves[i];
    }
    return waveListStr(expanded, clampedCount);
}

u32 generatedWaveCountForBank(u32 wave) {
    const auto it = s_bankWaveCounts.find(wave);
    return it != s_bankWaveCounts.end() ? it->second : 0;
}

const char* generatedBankKnown(u32 wave) {
    return s_bankWaveCounts.find(wave) != s_bankWaveCounts.end() ? "yes" : "no";
}

bool isGeneratedBgmBank(u32 wave) {
    return s_bankWaveCounts.find(wave) != s_bankWaveCounts.end();
}

bool appendRequiredSceneBgmWave(u32 wave) {
    if (wave == 0) {
        return false;
    }

    for (size_t i = 0; i < s_sceneRequiredBgmWaveCount; ++i) {
        if (s_sceneRequiredBgmWaves[i] == wave) {
            return false;
        }
    }

    if (s_sceneRequiredBgmWaveCount >= MaxSceneBgmWaves) {
        MUSIC_LOG("  scene residency overflow, dropping BGM bank %u\n", wave);
        return false;
    }

    s_sceneRequiredBgmWaves[s_sceneRequiredBgmWaveCount++] = wave;
    return true;
}

void clearSceneRequiredBgmWaves() {
    s_sceneOriginalId = JAISoundID(0xFFFFFFFF);
    s_sceneReplacementId = JAISoundID(0xFFFFFFFF);
    s_sceneRequiredBgmWaveCount = 0;
    for (size_t i = 0; i < MaxSceneBgmWaves; ++i) {
        s_sceneRequiredBgmWaves[i] = 0;
    }
}

void stageSceneRequiredRelease() {
    s_sceneReleaseBgmWaveCount = s_sceneRequiredBgmWaveCount;
    for (size_t i = 0; i < s_sceneRequiredBgmWaveCount; ++i) {
        s_sceneReleaseBgmWaves[i] = s_sceneRequiredBgmWaves[i];
    }
    for (size_t i = s_sceneRequiredBgmWaveCount; i < MaxSceneBgmWaves; ++i) {
        s_sceneReleaseBgmWaves[i] = 0;
    }
}

std::string activeSceneRequiredBgmWaveList() {
    return waveListStr(s_sceneRequiredBgmWaves, s_sceneRequiredBgmWaveCount);
}

bool requestBgmWaveForId(u32 bgmId, u32 wave, const char* reason) {
    if (wave == 0) {
        MUSIC_LOG("  skip BGM bank 0 for 0x%08x(%s), reason=%s\n",
                  bgmId, bgmName(bgmId), reason);
        return true;
    }

    const int beforeStatus = Z2GetSceneMgr()->getBgmLoadStatus(wave);
    const bool ok = Z2GetSceneMgr()->loadBgmWave(wave);
    const int afterStatus = Z2GetSceneMgr()->getBgmLoadStatus(wave);
    const bool available = ok || waveIsAvailableOrLoading(afterStatus);

    MUSIC_LOG("  request BGM bank %u for 0x%08x(%s), reason=%s, status=%d(%s)->%d(%s), generated=%s, waveCount=%u -> %s\n",
              wave, bgmId, bgmName(bgmId), reason,
              beforeStatus, waveStatusName(beforeStatus),
              afterStatus, waveStatusName(afterStatus),
              generatedBankKnown(wave), generatedWaveCountForBank(wave),
              available ? (ok ? "requested" : "already_or_pending") : "missing_or_failed");
    return available;
}

bool sceneRequiresBgmWave(u32 wave) {
    for (size_t i = 0; i < s_sceneRequiredBgmWaveCount; ++i) {
        if (s_sceneRequiredBgmWaves[i] == wave) {
            return true;
        }
    }
    return false;
}

bool mainRouteRequiresBgmWave(u32 wave) {
    for (size_t i = 0; i < s_mainRouteBgmWaveCount; ++i) {
        if (s_mainRouteBgmWaves[i] == wave) {
            return true;
        }
    }
    return false;
}

void rememberMainRouteBgmWave(u32 wave) {
    if (wave == 0) {
        return;
    }

    for (size_t i = 0; i < s_mainRouteBgmWaveCount; ++i) {
        if (s_mainRouteBgmWaves[i] == wave) {
            return;
        }
    }

    if (s_mainRouteBgmWaveCount >= MaxSceneBgmWaves) {
        MUSIC_LOG("  main route BGM list overflow, dropping bank %u\n", wave);
        return;
    }

    s_mainRouteBgmWaves[s_mainRouteBgmWaveCount++] = wave;
}

void clearMainRouteBgmWaves() {
    s_mainRouteBgmWaveCount = 0;
    for (size_t i = 0; i < MaxSceneBgmWaves; ++i) {
        s_mainRouteBgmWaves[i] = 0;
    }
}

void rememberTemporarilyEvictedSceneBgmWave(u32 wave) {
    if (wave == 0) {
        return;
    }

    for (size_t i = 0; i < s_temporarilyEvictedSceneBgmWaveCount; ++i) {
        if (s_temporarilyEvictedSceneBgmWaves[i] == wave) {
            return;
        }
    }

    if (s_temporarilyEvictedSceneBgmWaveCount >= MaxSceneBgmWaves) {
        MUSIC_LOG("  temporary scene BGM eviction list overflow, dropping bank %u\n", wave);
        return;
    }

    s_temporarilyEvictedSceneBgmWaves[s_temporarilyEvictedSceneBgmWaveCount++] = wave;
}

void rememberTemporaryRouteBgmWave(u32 wave) {
    if (wave == 0 || sceneRequiresBgmWave(wave) || mainRouteRequiresBgmWave(wave)) {
        return;
    }

    for (size_t i = 0; i < s_temporaryRouteBgmWaveCount; ++i) {
        if (s_temporaryRouteBgmWaves[i] == wave) {
            return;
        }
    }

    if (s_temporaryRouteBgmWaveCount >= MaxSceneBgmWaves) {
        MUSIC_LOG("  temporary route BGM list overflow, dropping bank %u\n", wave);
        return;
    }

    s_temporaryRouteBgmWaves[s_temporaryRouteBgmWaveCount++] = wave;
}

bool evictOneSceneBgmWaveForTemporary(u32 neededWave) {
    for (size_t i = s_sceneRequiredBgmWaveCount; i > 0; --i) {
        const u32 wave = s_sceneRequiredBgmWaves[i - 1];
        if (wave == 0 || wave == neededWave) {
            continue;
        }

        const int beforeStatus = Z2GetSceneMgr()->getBgmLoadStatus(wave);
        if (beforeStatus == 0) {
            continue;
        }

        s_allowSceneRequiredEviction = true;
        const bool erased = Z2GetSceneMgr()->eraseBgmWave(wave);
        s_allowSceneRequiredEviction = false;
        const int afterStatus = Z2GetSceneMgr()->getBgmLoadStatus(wave);
        MUSIC_LOG("  temporary route evict scene BGM bank %u for bank %u status=%d(%s)->%d(%s) -> %s\n",
                  wave, neededWave,
                  beforeStatus, waveStatusName(beforeStatus),
                  afterStatus, waveStatusName(afterStatus),
                  erased ? "erased" : "kept_or_pending");
        if (erased || afterStatus == 0) {
            rememberTemporarilyEvictedSceneBgmWave(wave);
            return true;
        }
    }

    return false;
}

bool requestTemporaryBgmWaveForId(Route route, u32 bgmId, u32 wave, const char* reason) {
    if (requestBgmWaveForId(bgmId, wave, reason)) {
        return true;
    }

    if (route == Route::Sub && !sceneRequiresBgmWave(wave)) {
        for (u32 retry = 0; retry < MaxBgmWaves; ++retry) {
            if (!evictOneSceneBgmWaveForTemporary(wave)) {
                break;
            }
            if (requestBgmWaveForId(bgmId, wave, "temporary route retry")) {
                return true;
            }
        }
    }

    return false;
}

bool releaseTemporaryRouteBgmWaves(const char* reason) {
    bool releasedAny = false;

    if (s_temporaryRouteBgmWaveCount != 0) {
        MUSIC_LOG("%s temporary route BGM banks: [%s]\n",
                  reason, waveListStr(s_temporaryRouteBgmWaves,
                                      s_temporaryRouteBgmWaveCount).c_str());
    }

    for (size_t i = 0; i < s_temporaryRouteBgmWaveCount; ++i) {
        const u32 wave = s_temporaryRouteBgmWaves[i];
        if (wave == 0 || sceneRequiresBgmWave(wave) || mainRouteRequiresBgmWave(wave)) {
            continue;
        }

        const int beforeStatus = Z2GetSceneMgr()->getBgmLoadStatus(wave);
        if (beforeStatus == 0) {
            continue;
        }

        const bool erased = Z2GetSceneMgr()->eraseBgmWave(wave);
        const int afterStatus = Z2GetSceneMgr()->getBgmLoadStatus(wave);
        MUSIC_LOG("  release temporary route BGM bank %u status=%d(%s)->%d(%s) -> %s\n",
                  wave, beforeStatus, waveStatusName(beforeStatus),
                  afterStatus, waveStatusName(afterStatus),
                  erased ? "erased" : "kept_or_pending");
        releasedAny = releasedAny || erased || afterStatus == 0;
    }

    s_temporaryRouteBgmWaveCount = 0;
    for (size_t i = 0; i < MaxSceneBgmWaves; ++i) {
        s_temporaryRouteBgmWaves[i] = 0;
    }

    return releasedAny;
}

void clearTemporaryEvictionState() {
    s_temporarilyEvictedSceneBgmWaveCount = 0;
    for (size_t i = 0; i < MaxSceneBgmWaves; ++i) {
        s_temporarilyEvictedSceneBgmWaves[i] = 0;
    }
}

void LoadRuntimeBgmWavesForPassthrough(Route route, u32 bgmId) {
    const tpcm::BgmEntry* entry = tpcm::FindBgmById(bgmId);
    if (entry == nullptr) {
        return;
    }

    const tpcm::BgmRuntimeInfo runtime = tpcm::RuntimeInfoForBgm(*entry);
    if (runtime.bgmWaveCount == 0) {
        MUSIC_LOG("%s passthrough 0x%08x(%s): known but no runtime BGM waves\n",
                  routeName(route), bgmId, bgmName(bgmId));
        return;
    }

    MUSIC_LOG("%s passthrough 0x%08x(%s): runtime_bgm=[%s]\n",
              routeName(route), bgmId, bgmName(bgmId),
              runtimeWaveListStr(runtime.bgmWaves, runtime.bgmWaveCount).c_str());

    for (std::uint8_t i = 0; i < runtime.bgmWaveCount; ++i) {
        const u32 wave = runtime.bgmWaves[i];
        if (requestTemporaryBgmWaveForId(route, bgmId, wave, "vanilla passthrough runtime")
            && route == Route::Main)
        {
            rememberMainRouteBgmWave(wave);
        }
        if (route == Route::Sub)
        {
            rememberTemporaryRouteBgmWave(wave);
        }
    }
}

std::filesystem::path manifestPath() {
    const std::string isoPath = dusk::getSettings().backend.isoPath.getValue();
    if (isoPath.empty()) {
        return {};
    }
    const std::filesystem::path iso(isoPath);
    return iso.parent_path()
        / (iso.stem().string() + ".dusk_music_manifest.json");
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

void appendWaves(std::vector<u32>& waves, const json& value) {
    if (!value.is_array()) {
        return;
    }

    for (const json& waveValue : value) {
        if (waves.size() >= MaxBgmWaves) {
            OS_REPORT("[DuskMusicResolver] Too many waves; truncating at %u\n",
                      static_cast<u32>(MaxBgmWaves));
            return;
        }

        const auto wave = readId(waveValue);
        if (wave && *wave != 0
            && std::find(waves.begin(), waves.end(), *wave) == waves.end()) {
            waves.push_back(*wave);
        }
    }
}

const ManifestEntry* findEntry(Route route, u32 originalId) {
    for (const ManifestEntry& entry : s_entries) {
        if (entry.route == route && entry.originalId == originalId) {
            return &entry;
        }
    }
    return nullptr;
}

const ManifestEntry* findPlaybackEntry(Route route, u32 playbackId) {
    for (const ManifestEntry& entry : s_entries) {
        if (entry.route == route && entry.replacementId == playbackId) {
            return &entry;
        }
    }
    return nullptr;
}

ResolvedMusic makeResolved(const ManifestEntry& entry) {
    ResolvedMusic resolved;
    resolved.matched = true;
    resolved.originalId = JAISoundID(entry.originalId);
    resolved.replacementId = JAISoundID(entry.replacementId);
    resolved.bgmWaveCount = entry.bgmWaves.size();
    for (size_t i = 0; i < entry.bgmWaves.size() && i < MaxBgmWaves; ++i) {
        resolved.bgmWaves[i] = entry.bgmWaves[i];
    }
    resolved.targetBgmWaveCount = entry.targetBgmWaves.size();
    for (size_t i = 0; i < entry.targetBgmWaves.size() && i < MaxBgmWaves; ++i) {
        resolved.targetBgmWaves[i] = entry.targetBgmWaves[i];
    }
    resolved.targetSeWaveCount = entry.targetSeWaves.size();
    for (size_t i = 0; i < entry.targetSeWaves.size() && i < MaxBgmWaves; ++i) {
        resolved.targetSeWaves[i] = entry.targetSeWaves[i];
    }
    return resolved;
}

struct PlaybackMatch {
    const ManifestEntry* entry = nullptr;
    Route matchedRoute = Route::Scene;
    bool alreadyPlayback = false;
    const char* kind = "none";
};

PlaybackMatch findPlaybackMatch(Route route, u32 requestedId) {
    ensureLoaded();

    if (!isSeqId(requestedId)) {
        return {};
    }

    if (const ManifestEntry* exact = findEntry(route, requestedId)) {
        return {exact, route, false, "exact"};
    }

    if (route != Route::Scene) {
        // sceneBgmStart calls bgmStart after ApplySceneResolution has already
        // replaced the scene BGM. When that happens, the runtime start sees the
        // replacement ID, not the original ID; still pin and verify its banks.
        if (const ManifestEntry* scenePlayback = findPlaybackEntry(Route::Scene, requestedId)) {
            return {scenePlayback, Route::Scene, true, "scene_playback"};
        }

        // Some no-loading-zone transitions start music through sub/main even
        // when the manifest entry was authored as a scene replacement.
        if (const ManifestEntry* sceneExact = findEntry(Route::Scene, requestedId)) {
            return {sceneExact, Route::Scene, false, "scene_fallback"};
        }
    }

    return {};
}

void ensureLoaded() {
    const std::filesystem::path path = manifestPath();
    std::error_code ec;
    const std::filesystem::file_time_type mtime =
        std::filesystem::exists(path, ec) && !ec
            ? std::filesystem::last_write_time(path, ec)
            : std::filesystem::file_time_type{};
    if (s_loaded && path == s_loadedPath && mtime == s_loadedMtime) {
        return;
    }

    s_entries.clear();
    s_bankWaveCounts.clear();
    s_disableAllEnemyMusic = false;
    s_loadedPath = path;
    s_loadedMtime = mtime;
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
            entry.kind = row.value("kind", "");
            entry.noEnemyMusic = row.value("no_enemy_music", false);
            appendWaves(entry.bgmWaves, row.value("bgm_waves", json::array()));
            appendWaves(entry.bgmWaves, row.value("extra_bgm_waves", json::array()));
            appendWaves(entry.targetBgmWaves, row.value("target_bgm_waves", json::array()));
            appendWaves(entry.targetBgmWaves, row.value("preserve_bgm_waves", json::array()));
            appendWaves(entry.targetSeWaves, row.value("target_se_waves", json::array()));
            s_entries.push_back(std::move(entry));
        }

        if (manifest.contains("generated_banks") && manifest["generated_banks"].is_array()) {
            for (const json& bank : manifest["generated_banks"]) {
                if (!bank.is_object()) {
                    continue;
                }
                const auto bankId = readId(bank.value("bank_id", json()));
                const auto waveCount = readId(bank.value("wave_count", json()));
                if (bankId) {
                    s_bankWaveCounts[*bankId] = waveCount.value_or(0);
                }
            }
        }

        s_disableAllEnemyMusic = manifest.value("disable_all_enemy_music", false);
        MUSIC_LOG("manifest loaded: %u entries, %u generated banks, disable_enemy=%d  (%s)\n",
                  static_cast<u32>(s_entries.size()), static_cast<u32>(s_bankWaveCounts.size()),
                  static_cast<int>(s_disableAllEnemyMusic), path.string().c_str());
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

    if (const ManifestEntry* entry = findEntry(route, original)) {
        return makeResolved(*entry);
    }

    return {};
}

JAISoundID LogicalOriginalForPlayback(Route route, JAISoundID playbackId) {
    if (!CustomAudioActive()) {
        return playbackId;
    }
    ensureLoaded();

    const u32 played = static_cast<u32>(playbackId);
    for (const ManifestEntry& entry : s_entries) {
        if (entry.route == route && entry.replacementId == played) {
            return JAISoundID(entry.originalId);
        }
    }

    return playbackId;
}

bool IsResolvedPlayback(Route route, JAISoundID playbackId) {
    if (!CustomAudioActive()) {
        return false;
    }
    ensureLoaded();

    const u32 played = static_cast<u32>(playbackId);
    for (const ManifestEntry& entry : s_entries) {
        if (entry.route == route && entry.replacementId == played) {
            return true;
        }
    }

    return false;
}

bool IsCurrentSceneReplacementPlayback(JAISoundID playbackId) {
    if (!CustomAudioActive()) {
        return false;
    }
    return s_sceneReplacementId == playbackId && s_sceneOriginalId != s_sceneReplacementId;
}

bool HasPlaybackMatch(Route route, JAISoundID requestedId) {
    if (!CustomAudioActive()) {
        return false;
    }
    return findPlaybackMatch(route, static_cast<u32>(requestedId)).entry != nullptr;
}

void LoadResolvedBgmWaves(const ResolvedMusic& resolved, size_t startIndex) {
    if (!CustomAudioActive()) {
        return;
    }
    if (!resolved.matched || startIndex >= resolved.bgmWaveCount) {
        return;
    }

    for (size_t i = startIndex; i < resolved.bgmWaveCount; ++i) {
        const u32 wave = resolved.bgmWaves[i];
        if (wave == 0) {
            continue;
        }

        requestBgmWaveForId(static_cast<u32>(resolved.replacementId), wave, "resolved replacement");
    }
}

void ApplySceneResolution(JAISoundID& bgm, u8& bgmWave1, u8& bgmWave2) {
    if (!CustomAudioActive()) {
        return;
    }
    const ResolvedMusic resolved = Resolve(Route::Scene, bgm);
    if (!resolved.matched) {
        stageSceneRequiredRelease();
        clearSceneRequiredBgmWaves();
        return;
    }

    const u32 origId = static_cast<u32>(resolved.originalId);
    const u32 replId = static_cast<u32>(resolved.replacementId);

    bgm = resolved.replacementId;
    bgmWave1 = 0;
    stageSceneRequiredRelease();
    clearSceneRequiredBgmWaves();
    s_sceneOriginalId = resolved.originalId;
    s_sceneReplacementId = resolved.replacementId;

    for (size_t i = 0; i < resolved.bgmWaveCount; ++i) {
        appendRequiredSceneBgmWave(resolved.bgmWaves[i]);
    }

    // Generated custom banks are self-contained, so a one-bank custom replacement must
    // not inherit the scene's stale secondary bank. For vanilla swaps, bgm_waves are the
    // replacement song's actual banks and must be allowed to occupy bgmWave1/2 normally.
    if (resolved.bgmWaveCount == 1 && isGeneratedBgmBank(resolved.bgmWaves[0])) {
        bgmWave2 = 0;
    }

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

    MUSIC_LOG("SCENE 0x%08x(%s) -> 0x%08x(%s)  bgmWave1=%u bgmWave2=%u\n",
              origId, bgmName(origId), replId, bgmName(replId), bgmWave1, bgmWave2);
    MUSIC_LOG("  banks=[%s] target_bgm=[%s] target_se=[%s] required_bgm=[%s] -> scene residency\n",
              waveListStr(resolved.bgmWaves, resolved.bgmWaveCount).c_str(),
              waveListStr(resolved.targetBgmWaves, resolved.targetBgmWaveCount).c_str(),
              waveListStr(resolved.targetSeWaves, resolved.targetSeWaveCount).c_str(),
              activeSceneRequiredBgmWaveList().c_str());
}

bool SceneResolvedWavesStillLoading() {
    if (!CustomAudioActive()) {
        return false;
    }
    for (size_t i = 0; i < s_sceneRequiredBgmWaveCount; ++i) {
        if (Z2GetSceneMgr()->getBgmLoadStatus(s_sceneRequiredBgmWaves[i]) == 1) {
            return true;
        }
    }
    return false;
}

void ReleaseUnneededSceneBgmWaves() {
    if (!CustomAudioActive()) {
        return;
    }
    if (s_sceneReleaseBgmWaveCount == 0) {
        return;
    }

    MUSIC_LOG("scene residency release previous_bgm=[%s] keep_bgm=[%s]\n",
              waveListStr(s_sceneReleaseBgmWaves, s_sceneReleaseBgmWaveCount).c_str(),
              activeSceneRequiredBgmWaveList().c_str());

    for (size_t i = 0; i < s_sceneReleaseBgmWaveCount; ++i) {
        const u32 wave = s_sceneReleaseBgmWaves[i];
        if (wave == 0 || IsBgmWaveLocked(wave)) {
            if (wave != 0) {
                MUSIC_LOG("  keep old scene BGM bank %u because it is locked\n", wave);
            }
            continue;
        }

        const int beforeStatus = Z2GetSceneMgr()->getBgmLoadStatus(wave);
        if (beforeStatus == 0) {
            MUSIC_LOG("  old scene BGM bank %u already not_loaded\n", wave);
            continue;
        }

        const bool erased = Z2GetSceneMgr()->eraseBgmWave(wave);
        const int afterStatus = Z2GetSceneMgr()->getBgmLoadStatus(wave);
        MUSIC_LOG("  release old scene BGM bank %u status=%d(%s)->%d(%s) -> %s\n",
                  wave, beforeStatus, waveStatusName(beforeStatus),
                  afterStatus, waveStatusName(afterStatus),
                  erased ? "erased" : "kept_or_pending");
    }

    s_sceneReleaseBgmWaveCount = 0;
    for (size_t i = 0; i < MaxSceneBgmWaves; ++i) {
        s_sceneReleaseBgmWaves[i] = 0;
    }
}

void LoadSceneRequiredBgmWaves(size_t startIndex) {
    if (!CustomAudioActive()) {
        return;
    }
    if (s_sceneRequiredBgmWaveCount == 0 || startIndex >= s_sceneRequiredBgmWaveCount) {
        return;
    }

    MUSIC_LOG("scene residency load original=0x%08x(%s) playback=0x%08x(%s) start=%u required_bgm=[%s]\n",
              static_cast<u32>(s_sceneOriginalId), bgmName(static_cast<u32>(s_sceneOriginalId)),
              static_cast<u32>(s_sceneReplacementId), bgmName(static_cast<u32>(s_sceneReplacementId)),
              static_cast<u32>(startIndex), activeSceneRequiredBgmWaveList().c_str());

    for (size_t i = startIndex; i < s_sceneRequiredBgmWaveCount; ++i) {
        requestBgmWaveForId(static_cast<u32>(s_sceneReplacementId), s_sceneRequiredBgmWaves[i],
                            "scene residency");
    }
}

JAISoundID ResolvePlaybackAndLoad(Route route, JAISoundID originalId) {
    if (!CustomAudioActive()) {
        return originalId;
    }
    const PlaybackMatch match = findPlaybackMatch(route, static_cast<u32>(originalId));
    if (match.entry == nullptr) {
        LoadRuntimeBgmWavesForPassthrough(route, static_cast<u32>(originalId));
        MUSIC_LOG("%s passthrough 0x%08x(%s): no manifest match\n",
                  routeName(route), static_cast<u32>(originalId),
                  bgmName(static_cast<u32>(originalId)));
        return originalId;
    }

    const ResolvedMusic resolved = makeResolved(*match.entry);
    const JAISoundID playback = match.alreadyPlayback ? originalId : resolved.replacementId;

    if (route == Route::Main && match.matchedRoute == Route::Scene && match.alreadyPlayback) {
        MUSIC_LOG("%s match=%s/%s requested=0x%08x(%s) original=0x%08x(%s) playback=0x%08x(%s) banks=[%s] target_bgm=[%s] target_se=[%s] load=scene-managed\n",
                  routeName(route), match.kind, routeName(match.matchedRoute),
                  static_cast<u32>(originalId), bgmName(static_cast<u32>(originalId)),
                  static_cast<u32>(resolved.originalId),
                  bgmName(static_cast<u32>(resolved.originalId)),
                  static_cast<u32>(playback), bgmName(static_cast<u32>(playback)),
                  waveListStr(resolved.bgmWaves, resolved.bgmWaveCount).c_str(),
                  waveListStr(resolved.targetBgmWaves, resolved.targetBgmWaveCount).c_str(),
                  waveListStr(resolved.targetSeWaves, resolved.targetSeWaveCount).c_str());
        return playback;
    }

    LoadResolvedBgmWaves(resolved);
    MUSIC_LOG("%s match=%s/%s requested=0x%08x(%s) original=0x%08x(%s) playback=0x%08x(%s) banks=[%s] target_bgm=[%s] target_se=[%s] load=resolver\n",
              routeName(route), match.kind, routeName(match.matchedRoute),
              static_cast<u32>(originalId), bgmName(static_cast<u32>(originalId)),
              static_cast<u32>(resolved.originalId),
              bgmName(static_cast<u32>(resolved.originalId)),
              static_cast<u32>(playback), bgmName(static_cast<u32>(playback)),
              waveListStr(resolved.bgmWaves, resolved.bgmWaveCount).c_str(),
              waveListStr(resolved.targetBgmWaves, resolved.targetBgmWaveCount).c_str(),
              waveListStr(resolved.targetSeWaves, resolved.targetSeWaveCount).c_str());
    return playback;
}

JAISoundID ResolvePlaybackAndLock(Route route, JAISoundID originalId, LockSlot slot) {
    (void)slot;
    if (!CustomAudioActive()) {
        return originalId;
    }
    const JAISoundID playback = ResolvePlaybackAndLoad(route, originalId);
    MUSIC_LOG("tp-style no-lock route=%s requested=0x%08x(%s) playback=0x%08x(%s)\n",
              routeName(route), static_cast<u32>(originalId),
              bgmName(static_cast<u32>(originalId)), static_cast<u32>(playback),
              bgmName(static_cast<u32>(playback)));
    return playback;
}

void ClearBgmWaveLock(LockSlot slot) {
    (void)slot;
}

bool IsBgmWaveLocked(u32 wave) {
    if (!CustomAudioActive()) {
        return false;
    }
    if (s_allowSceneRequiredEviction) {
        return false;
    }

    if (wave == 0) {
        return true;
    }

    for (size_t i = 0; i < s_sceneRequiredBgmWaveCount; ++i) {
        if (s_sceneRequiredBgmWaves[i] == wave) {
            return true;
        }
    }

    return false;
}

void ClearRuntimeSupportWavesForScene(u8 bgmWave1, u8 bgmWave2) {
    (void)bgmWave1;
    (void)bgmWave2;

    if (!CustomAudioActive()) {
        return;
    }

    if (s_mainRouteBgmWaveCount != 0) {
        MUSIC_LOG("scene change clears main route BGM banks: [%s]\n",
                  waveListStr(s_mainRouteBgmWaves, s_mainRouteBgmWaveCount).c_str());
        clearMainRouteBgmWaves();
    }
    if (s_temporaryRouteBgmWaveCount != 0) {
        releaseTemporaryRouteBgmWaves("scene change release stale");
    }
    if (s_temporarilyEvictedSceneBgmWaveCount != 0) {
        MUSIC_LOG("scene change drops pending temporary scene restore: [%s]\n",
                  waveListStr(s_temporarilyEvictedSceneBgmWaves,
                              s_temporarilyEvictedSceneBgmWaveCount).c_str());
        clearTemporaryEvictionState();
    }
}

void RestoreEvictedMainBgmWaves() {
    if (!CustomAudioActive()) {
        return;
    }
    if (s_temporarilyEvictedSceneBgmWaveCount == 0 && s_temporaryRouteBgmWaveCount == 0) {
        return;
    }

    releaseTemporaryRouteBgmWaves("release before scene restore");

    if (s_temporarilyEvictedSceneBgmWaveCount != 0) {
        MUSIC_LOG("restore scene BGM banks after temporary route: [%s]\n",
                  waveListStr(s_temporarilyEvictedSceneBgmWaves,
                              s_temporarilyEvictedSceneBgmWaveCount).c_str());
    }

    for (size_t i = 0; i < s_temporarilyEvictedSceneBgmWaveCount; ++i) {
        const u32 wave = s_temporarilyEvictedSceneBgmWaves[i];
        if (sceneRequiresBgmWave(wave)) {
            requestBgmWaveForId(static_cast<u32>(s_sceneReplacementId), wave,
                                "restore scene after temporary route");
        }
    }

    s_temporarilyEvictedSceneBgmWaveCount = 0;
    for (size_t i = 0; i < MaxSceneBgmWaves; ++i) {
        s_temporarilyEvictedSceneBgmWaves[i] = 0;
    }
}

bool ReleaseBgmWavesForSeRetry(u32 seWave) {
    if (!CustomAudioActive()) {
        return false;
    }
    bool freedAny = false;

    if (s_sceneReleaseBgmWaveCount != 0) {
        MUSIC_LOG("SE bank %u failed; forcing stale scene BGM cleanup before retry\n", seWave);
        ReleaseUnneededSceneBgmWaves();
        freedAny = true;
    }

    if (s_temporaryRouteBgmWaveCount != 0) {
        MUSIC_LOG("SE bank %u failed; freeing temporary route BGM before retry\n", seWave);
        freedAny = releaseTemporaryRouteBgmWaves("SE retry release") || freedAny;
    }

    return freedAny;
}

bool IsAllEnemyMusicDisabled() {
    if (!CustomAudioActive()) {
        return false;
    }
    ensureLoaded();
    return s_disableAllEnemyMusic;
}

bool IsEnemyMusicDisabledFor(JAISoundID bgmId) {
    if (!CustomAudioActive()) {
        return false;
    }
    ensureLoaded();
    const u32 id = static_cast<u32>(bgmId);
    for (const ManifestEntry& entry : s_entries) {
        if (entry.noEnemyMusic && (entry.originalId == id || entry.replacementId == id)) {
            return true;
        }
    }
    return false;
}

// True only when a music-randomized ROM's matching manifest with real entries
// is loaded. No matching manifest -> false, so TP audio hooks fall through to
// the original path.
bool CustomAudioActive() {
    ensureLoaded();
    return s_loaded && !s_entries.empty();
}

} // namespace dusk::music
