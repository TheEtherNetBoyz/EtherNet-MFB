#include "dusk/custom_music/CustomMusicService.h"

#include "dusk/custom_music/CustomMusicIsoTransaction.h"

#include <tpcm/BgmDatabase.h>
#include <tpcm/InstrumentMap.h>
#include <tpcm/IsoFileSystem.h>
#include <tpcm/ManifestBuilder.h>
#include <tpcm/MusicRandomizer.h>
#include <tpcm/RarcInjector.h>
#include <tpcm/WaveBankBuilder.h>

#include "dusk/config.hpp"
#include "dusk/data.hpp"
#include "dusk/settings.h"
#include "dusk/ui/prelaunch.hpp"

#include <fstream>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace dusk::custom_music {
namespace {

bool isActiveIso(const fs::path& path) {
    dusk::ui::ensure_initialized();
    const fs::path active = dusk::ui::prelaunch_state().activeDiscPath;
    if (active.empty()) return false;
    std::error_code pathError;
    const fs::path canonicalPath = fs::weakly_canonical(path, pathError);
    if (pathError) return path == active;
    std::error_code activeError;
    const fs::path canonicalActive = fs::weakly_canonical(active, activeError);
    return activeError ? path == active : canonicalPath == canonicalActive;
}

std::vector<std::uint32_t> collectWaveIds(
    const tpcm::MidiFile& midi, const tpcm::InstrumentMap& instruments,
    const tpcm::MidiToBmsOptions& options) {
    struct KeyRange {
        std::uint8_t lo = 127;
        std::uint8_t hi = 0;
    };

    const auto mergeKey = [](KeyRange& range, std::uint8_t key) {
        constexpr int kKeyMargin = 1;
        const int lo = std::max(0, static_cast<int>(key) - kKeyMargin);
        const int hi = std::min(127, static_cast<int>(key) + kKeyMargin);
        range.lo = static_cast<std::uint8_t>(std::min<int>(range.lo, lo));
        range.hi = static_cast<std::uint8_t>(std::max<int>(range.hi, hi));
    };

    // Collect the game (bank, program, key range) actually used by the song.
    // TP instruments are multi-region, so collecting a single wave per note is
    // too brittle; however, packing every zone of every touched instrument can
    // make custom banks so large that scene SE banks no longer fit in ARAM.
    // Pack all zones overlapping the played key range, plus a small margin.
    std::map<std::uint16_t, KeyRange> usedRanges;  // key = (gameBank << 8) | gameProgram
    for (const auto& track : midi.tracks) {
        std::uint8_t bankCc0[16] = {};
        std::uint8_t program[16] = {};
        for (const auto& event : track.events) {
            if (event.bytes.empty()) continue;
            const std::uint8_t status = event.bytes[0];
            const std::uint8_t channel = status & 0x0f;
            const std::uint8_t type = status & 0xf0;
            if (type == 0xb0 && event.bytes.size() >= 3 && event.bytes[1] == 0) {
                bankCc0[channel] = event.bytes[2];
            } else if (type == 0xc0 && event.bytes.size() >= 2) {
                program[channel] = event.bytes[1];
            } else if (type == 0x90 && event.bytes.size() >= 3 && event.bytes[2] > 0) {
                std::uint8_t gameBank = 11;
                std::uint8_t gameProgram = program[channel];
                const std::uint8_t cc0 = bankCc0[channel];
                if (cc0 == 12 || cc0 == 112) gameBank = 12;
                else if (cc0 == 13 || cc0 == 113) gameBank = 13;
                else if (cc0 == 50 || cc0 == 150) gameBank = 50;
                else if (cc0 == 51 || cc0 == 151) gameBank = 51;
                else if (cc0 == 52 || cc0 == 152) gameBank = 52;
                else if (cc0 == 53 || cc0 == 153) gameBank = 53;
                else gameBank = 11;
                if (cc0 == 111 || cc0 == 112 || cc0 == 113 || cc0 == 150 || cc0 == 151
                    || cc0 == 152 || cc0 == 153)
                    gameProgram = static_cast<std::uint8_t>(gameProgram + 128);
                const auto overrideIt = options.channelOverrides.find(channel);
                if (overrideIt != options.channelOverrides.end()) {
                    if (overrideIt->second.mute) continue;
                    if (overrideIt->second.bank >= 0) gameBank = static_cast<std::uint8_t>(overrideIt->second.bank);
                    if (overrideIt->second.program >= 0) gameProgram = static_cast<std::uint8_t>(overrideIt->second.program);
                }
                mergeKey(usedRanges[static_cast<std::uint16_t>((gameBank << 8) | gameProgram)],
                         event.bytes[1] & 0x7f);
            }
        }
    }

    std::set<std::uint32_t> ids;
    for (const auto& [key, range] : usedRanges) {
        const std::uint8_t bank = static_cast<std::uint8_t>(key >> 8);
        const std::uint8_t program = static_cast<std::uint8_t>(key & 0xff);
        for (const auto& zone : instruments.zonesFor(bank, program)) {
            if (zone.waveId != 0xffff && zone.loKey <= range.hi && zone.hiKey >= range.lo) {
                ids.insert(zone.waveId);
            }
        }
    }
    return {ids.begin(), ids.end()};
}

void injectWaveBank(tpcm::IsoFileSystem& iso, const std::vector<std::uint32_t>& ids,
                    std::uint8_t bankIndex, const TransactionLog& log) {
    if (ids.empty()) return;
    const auto baa = iso.readFile("Z2Sound.baa");
    auto loader = [&](std::uint32_t bank) {
        try {
            return iso.readFile("Z2BgmWave_" + std::to_string(bank) + ".aw");
        } catch (...) {
            return std::vector<std::uint8_t>{};
        }
    };
    const std::string bankName = "Z2BgmWave_" + std::to_string(bankIndex) + ".aw";
    const auto output = tpcm::generateWaveBankInMemory(
        baa, loader, ids, bankName, bankIndex, {0}, log);
    iso.replaceFile("Z2Sound.baa", output.baaData);
    iso.writeFile(bankName, output.awData);
}

struct CustomBankPlan {
    std::uint8_t bankIndex = 90;
    std::uint32_t hostBgmId = 0;
    std::string ownerBms;
    std::vector<std::uint32_t> waves;
};

std::vector<tpcm::GeneratedBank> manifestBanksFor(const std::vector<CustomBankPlan>& plans) {
    std::vector<tpcm::GeneratedBank> banks;
    banks.reserve(plans.size());
    for (const CustomBankPlan& plan : plans) {
        banks.push_back({plan.bankIndex, plan.ownerBms, true,
                         static_cast<std::uint32_t>(plan.waves.size())});
    }
    return banks;
}

std::string applyReplacement(const fs::path& isoPath, const CustomMusicProject& project,
                             const TransactionLog& log) {
    if (project.selectedSong < 0 || project.selectedSong >= static_cast<int>(project.library.size())) {
        throw std::runtime_error("No custom song is selected.");
    }
    if (project.replacementTarget < 0
        || project.replacementTarget >= static_cast<int>(tpcm::BGM_TABLE_SIZE)) {
        throw std::runtime_error("Invalid replacement target.");
    }
    const CustomSong& song = project.library[static_cast<std::size_t>(project.selectedSong)];
    const tpcm::BgmEntry& target = tpcm::BGM_TABLE[project.replacementTarget];
    if (!song.available) {
        throw std::runtime_error("Selected custom song is unavailable.");
    }
    if (!tpcm::RuntimeInfoForBgm(target).targetAllowed) {
        throw std::runtime_error("Selected target is not a supported music playback slot.");
    }
    const auto midi = tpcm::parseMidi(loadSongMidi(song));
    const auto options = buildSongOptions(song);
    const auto bms = tpcm::midiToBms(midi, options);
    const auto waves = collectWaveIds(midi, tpcm::InstrumentMap::fromBuiltin(), options);
    if (waves.empty()) {
        throw std::runtime_error(
            "Custom song has no supported playable instruments: " + song.title);
    }

    // Store the custom BMS under the TARGET's own sequence slot so it plays
    // with the target's real BGM identity. getMainBgmID() then returns the
    // target id at runtime, so the game applies that scene's default dynamic
    // state via changeBgmStatus exactly as it does for a vanilla->vanilla swap
    // in the TP randomizer. A neutral host slot was previously used to dodge
    // the target's per-BGM cutscene mutes, but it also suppressed the song's
    // default-state muting and left dynamic layers (e.g. the Sacred Grove
    // trumpet) stuck on. We match the randomizer and accept the target's
    // per-BGM logic.
    if (target.bmsName == nullptr) {
        throw std::runtime_error("Replacement target has no sequence slot.");
    }

    tpcm::IsoFileSystem iso = tpcm::IsoFileSystem::open(isoPath.string());
    tpcm::RarcArchive archive(tpcm::yaz0Decompress(iso.readFile("Z2SoundSeqs.arc")));
    log(std::string("Replacing ") + target.displayName + " with " + song.title + "...");
    archive.replaceFile(target.bmsName, bms);
    iso.replaceFile("Z2SoundSeqs.arc", tpcm::yaz0Compress(archive.toBytes()));
    constexpr std::uint8_t customBank = 90;
    injectWaveBank(iso, waves, customBank, log);

    tpcm::MusicSlotAssignment assignment{
        target.bgmId, target.bgmId,
        customBank, std::uint8_t{0},
        tpcm::MusicAssignmentKind::CustomTprs,
        song.noEnemyMusic,
    };
    const std::vector<tpcm::GeneratedBank> generatedBanks = {
        {customBank, target.bmsName ? target.bmsName : "", true,
         static_cast<std::uint32_t>(waves.size())},
    };
    auto manifest = tpcm::buildManifest({assignment}, generatedBanks, tpcm::BGM_TABLE, tpcm::BGM_TABLE_SIZE);
    manifest.disableAllEnemyMusic = project.disableEnemyMusicGlobal;
    return tpcm::toJson(manifest);
}

std::string applyRandomizer(const fs::path& isoPath, const CustomMusicProject& project,
                            const TransactionLog& log) {
    std::vector<tpcm::CustomMusicCandidate> candidates;
    std::map<std::uint32_t, std::string> customNames;
    const auto slots = tpcm::selectCustomMusicSlots(project.poolMode);
    tpcm::IsoFileSystem iso = tpcm::IsoFileSystem::open(isoPath.string());
    tpcm::RarcArchive archive(tpcm::yaz0Decompress(iso.readFile("Z2SoundSeqs.arc")));
    const auto instruments = tpcm::InstrumentMap::fromBuiltin();
    std::vector<CustomBankPlan> customBanks;
    std::size_t slotIndex = 0;
    for (const CustomSong& song : project.library) {
        if (!song.available) continue;
        if (slotIndex >= slots.size()) break;
        const auto* slot = slots[slotIndex++];
        const std::size_t bankNumber = 90 + customBanks.size();
        if (bankNumber > 0xFF) {
            throw std::runtime_error("Too many custom songs for per-song BGM wave banks.");
        }
        const auto midi = tpcm::parseMidi(loadSongMidi(song));
        const auto options = buildSongOptions(song);
        archive.replaceFile(slot->bmsName, tpcm::midiToBms(midi, options));
        const auto waves = collectWaveIds(midi, instruments, options);
        if (waves.empty()) {
            throw std::runtime_error(
                "Custom song has no supported playable instruments: " + song.title);
        }
        const auto bankIndex = static_cast<std::uint8_t>(bankNumber);
        customBanks.push_back({bankIndex, slot->bgmId, slot->bmsName ? slot->bmsName : "", waves});
        candidates.push_back({slot->bgmId, bankIndex, std::uint8_t{0},
                              song.noEnemyMusic || project.disableEnemyMusicGlobal});
        customNames[slot->bgmId] = song.title;
        log("Prepared custom song: " + song.title
            + " -> BGM wave " + std::to_string(bankIndex)
            + " (" + std::to_string(waves.size()) + " source waves)");
    }
    if (!candidates.empty()) {
        iso.replaceFile("Z2SoundSeqs.arc", tpcm::yaz0Compress(archive.toBytes()));
        for (const CustomBankPlan& bank : customBanks) {
            injectWaveBank(iso, bank.waves, bank.bankIndex, log);
        }
    }

    std::vector<tpcm::ForcedAssignment> validForced;
    for (const auto& fa : project.forcedAssignments) {
        if (fa.replacementBgmId != fa.targetBgmId) {
            validForced.push_back(fa);
        } else {
            log("Warning: skipping self-referential forced assignment (same song and target); remove it in the Randomizer tab.");
        }
    }

    const auto compatibility = tpcm::buildMusicCompatibilityReport(project.poolMode);
    std::size_t automaticCount = 0;
    std::size_t targetOnlyCount = 0;
    std::map<std::string, std::size_t> targetOnlyReasons;
    for (const auto& entry : compatibility) {
        if (entry.compatibility == tpcm::MusicSlotCompatibility::Automatic) {
            ++automaticCount;
        } else if (entry.compatibility == tpcm::MusicSlotCompatibility::TargetOnly) {
            ++targetOnlyCount;
            targetOnlyReasons[entry.reason]++;
        }
    }
    log("Music pool: " + std::to_string(automaticCount)
        + " automatic slots, " + std::to_string(targetOnlyCount)
        + " target-only contextual slots available for forced assignments.");
    if (!targetOnlyReasons.empty()) {
        std::string reasonLine = "Target-only reasons:";
        for (const auto& [reason, count] : targetOnlyReasons) {
            reasonLine += " " + reason + "=" + std::to_string(count) + ";";
        }
        log(reasonLine);
    }

    const auto assignments = tpcm::buildMusicAssignments(
        static_cast<std::uint32_t>(project.randomizerSeed), project.poolMode,
        candidates, validForced);
    if (project.showReplacementSpoilers) {
        log("Music replacements:");
        for (const auto& assignment : assignments) {
            const auto* original = tpcm::FindBgmById(assignment.originalBgmId);
            const auto* replacement = tpcm::FindBgmById(assignment.replacementBgmId);
            const std::string originalName = original ? original->displayName : std::to_string(assignment.originalBgmId);
            const auto custom = customNames.find(assignment.replacementBgmId);
            const std::string replacementName = custom != customNames.end()
                ? custom->second + " (custom)"
                : replacement ? replacement->displayName : std::to_string(assignment.replacementBgmId);
            log(originalName + " -> " + replacementName);
        }
    } else {
        log("Music replacements hidden.");
    }
    auto manifest = tpcm::buildManifest(
        assignments, manifestBanksFor(customBanks), tpcm::BGM_TABLE, tpcm::BGM_TABLE_SIZE);
    manifest.disableAllEnemyMusic = project.disableEnemyMusicGlobal;
    return tpcm::toJson(manifest);
}

}  // namespace

CustomMusicService& CustomMusicService::instance() {
    static CustomMusicService service;
    return service;
}

CustomMusicService::~CustomMusicService() {
    m_cancel.store(true);
    if (m_worker.joinable()) m_worker.join();
}

CustomMusicProject& CustomMusicService::project() { initialize(); return m_project; }
const CustomMusicProject& CustomMusicService::project() const { return m_project; }

void CustomMusicService::initialize() {
    if (m_initialized) return;
    m_initialized = true;
    try {
        m_project.load(projectPath());
        m_project.refresh();
    } catch (const std::exception& e) {
        log(std::string("Could not load custom music library: ") + e.what());
    }
    cleanSourceIso();
}

fs::path CustomMusicService::projectPath() const {
    return data::configured_data_path() / "custom-music" / "project.json";
}

fs::path CustomMusicService::cleanSourceIso() {
    const fs::path configured = getSettings().backend.isoPath.getValue();
    if (!configured.empty()) {
        const std::string suffix = "-custom-music";
        const std::string stem = configured.stem().string();
        if (stem.ends_with(suffix)) {
            const fs::path original = configured.parent_path()
                / (stem.substr(0, stem.size() - suffix.size()) + configured.extension().string());
            if (fs::is_regular_file(original)) m_project.cleanSourceIso = original;
        } else if (fs::is_regular_file(configured)) {
            m_project.cleanSourceIso = configured;
        }
    }
    if (!m_project.cleanSourceIso.empty() && !fs::is_regular_file(m_project.cleanSourceIso)) {
        m_project.cleanSourceIso.clear();
    }
    return m_project.cleanSourceIso;
}

void CustomMusicService::saveProject() {
    initialize();
    m_project.save(projectPath());
}

void CustomMusicService::importPath(const fs::path& path) {
    initialize();
    m_project.importPath(path);
    saveProject();
}

void CustomMusicService::refreshLibrary() {
    initialize();
    fs::path previouslySelected;
    if (m_project.selectedSong >= 0
        && m_project.selectedSong < static_cast<int>(m_project.library.size())) {
        previouslySelected = m_project.library[static_cast<std::size_t>(m_project.selectedSong)].path;
    }
    try {
        m_project.load(projectPath());
    } catch (const std::exception& e) {
        log(std::string("Could not reload custom music library: ") + e.what());
    }
    m_project.refresh();
    cleanSourceIso();
    if (!previouslySelected.empty()) {
        const auto match = std::find_if(m_project.library.begin(), m_project.library.end(),
            [&](const CustomSong& song) { return song.path == previouslySelected; });
        if (match != m_project.library.end()) {
            m_project.selectedSong = static_cast<int>(std::distance(m_project.library.begin(), match));
        }
    }
    saveProject();
}

void CustomMusicService::saveSelectedSource() {
    initialize();
    m_project.saveSelectedSource();
    saveProject();
    log("Saved metadata into TPRS source.");
}

void CustomMusicService::packSelectedMidiToTprs() {
    initialize();
    m_project.packSelectedMidiToTprs();
    saveProject();
    log("Packed MIDI into TPRS.");
}

void CustomMusicService::launch(CustomMusicJobKind kind, std::function<void()> operation) {
    if (m_worker.joinable()) m_worker.join();
    m_cancel.store(false);
    {
        std::lock_guard lock(m_mutex);
        m_status = {kind, true, false, 0.0f, "Starting", {}, {}};
    }
    m_worker = std::thread([this, operation = std::move(operation)]() {
        try {
            operation();
        } catch (const std::exception& e) {
            finish(false, e.what());
        }
    });
}

bool CustomMusicService::startApplyReplacement(bool replaceExisting) {
    initialize();
    if (status().running) return false;
    const CustomMusicProject snapshot = m_project;
    const fs::path source = cleanSourceIso();
    if (isActiveIso(CustomMusicIsoTransaction::stagedPathFor(source))) {
        finish(false, "The custom-music ISO is currently active. Switch to the clean ISO and restart first.");
        return false;
    }
    launch(CustomMusicJobKind::ApplyReplacement, [this, snapshot, source, replaceExisting]() {
        const auto result = CustomMusicIsoTransaction::apply(
            source, [&](const fs::path& iso, const TransactionLog& callback) {
                return applyReplacement(iso, snapshot, callback);
            }, replaceExisting, m_cancel, [this](const std::string& line) { log(line); },
            [this](const std::string& stage, float value) { setStage(stage, value); });
        finish(result.success, result.message, result.outputIso);
    });
    return true;
}

bool CustomMusicService::startRandomizeSongs(bool replaceExisting) {
    initialize();
    if (status().running) return false;
    const CustomMusicProject snapshot = m_project;
    const fs::path source = cleanSourceIso();
    if (isActiveIso(CustomMusicIsoTransaction::stagedPathFor(source))) {
        finish(false, "The custom-music ISO is currently active. Switch to the clean ISO and restart first.");
        return false;
    }
    launch(CustomMusicJobKind::RandomizeSongs, [this, snapshot, source, replaceExisting]() {
        const auto result = CustomMusicIsoTransaction::apply(
            source, [&](const fs::path& iso, const TransactionLog& callback) {
                return applyRandomizer(iso, snapshot, callback);
            }, replaceExisting, m_cancel, [this](const std::string& line) { log(line); },
            [this](const std::string& stage, float value) { setStage(stage, value); });
        finish(result.success, result.message, result.outputIso);
    });
    return true;
}

bool CustomMusicService::startCreateSnapshot() {
    initialize();
    if (status().running) return false;
    const fs::path source = getSettings().backend.isoPath.getValue();
    if (source.empty()) {
        log("Cannot create a snapshot because Dusklight has no configured ISO.");
        return false;
    }
    launch(CustomMusicJobKind::CreateSnapshot, [this, source]() {
        const auto result = CustomMusicIsoTransaction::createSnapshot(
            source, m_cancel, [this](const std::string& line) { log(line); },
            [this](const std::string& stage, float value) { setStage(stage, value); });
        finish(result.success, result.message, result.outputIso);
    });
    return true;
}

bool CustomMusicService::startRestoreIso(const fs::path& restoreSource) {
    initialize();
    if (status().running) return false;
    const fs::path target = getSettings().backend.isoPath.getValue();
    if (target.empty()) {
        log("Cannot restore because Dusklight has no configured ISO.");
        return false;
    }
    launch(CustomMusicJobKind::RestoreIso, [this, restoreSource, target]() {
        const auto result = CustomMusicIsoTransaction::prepareRestore(
            restoreSource, target, m_cancel,
            [this](const std::string& line) { log(line); },
            [this](const std::string& stage, float value) { setStage(stage, value); });
        finish(result.success, result.message, result.outputIso);
    });
    return true;
}

void CustomMusicService::resetMusicRandomizer() {
    initialize();
    const fs::path configured = getSettings().backend.isoPath.getValue();
    if (configured.empty()) {
        log("Cannot reset music randomizer because Dusklight has no configured ISO.");
        return;
    }
    std::error_code error;
    const fs::path specific = configured.parent_path()
        / (configured.stem().string() + ".dusk_music_manifest.json");
    const bool removedSpecific = fs::remove(specific, error);
    if (error) {
        log("Could not reset music randomizer: " + error.message());
        return;
    }
    bool switchedToClean = false;
    const fs::path clean = cleanSourceIso();
    if (!clean.empty() && configured != clean
        && configured == CustomMusicIsoTransaction::stagedPathFor(clean)) {
        getSettings().backend.isoPath.setValue(clean.string());
        config::save();
        switchedToClean = true;
    }
    if (removedSpecific || switchedToClean) {
        log("Music randomizer reset. Restart Dusklight to return music to the clean source ISO.");
    } else {
        log("Music randomizer is already reset for the configured ISO.");
    }
}

void CustomMusicService::cancel() { m_cancel.store(true); }

CustomMusicJobStatus CustomMusicService::status() const {
    std::lock_guard lock(m_mutex);
    return m_status;
}

std::vector<std::string> CustomMusicService::logLines() const {
    std::lock_guard lock(m_mutex);
    return {m_log.begin(), m_log.end()};
}

void CustomMusicService::clearLog() {
    std::lock_guard lock(m_mutex);
    m_log.clear();
}

void CustomMusicService::report(const std::string& line) { log(line); }

void CustomMusicService::finish(bool success, const std::string& result, const fs::path& output) {
    std::lock_guard lock(m_mutex);
    m_status.running = false;
    m_status.succeeded = success;
    m_status.progress = 1.0f;
    m_status.stage = success ? "Complete" : "Failed";
    m_status.result = result;
    m_status.outputIso = output;
    if (success && !output.empty()
        && (m_status.kind == CustomMusicJobKind::ApplyReplacement
            || m_status.kind == CustomMusicJobKind::RandomizeSongs)) {
        m_pendingIsoSwitch = output;
    }
    m_log.push_back((success ? "Done: " : "Error: ") + result);
}

void CustomMusicService::log(const std::string& line) {
    std::lock_guard lock(m_mutex);
    m_log.push_back(line);
    while (m_log.size() > 2000) m_log.pop_front();
    if (m_status.running) {
        m_status.stage = line;
        m_status.progress = std::min(0.9f, m_status.progress + 0.05f);
    }
}

void CustomMusicService::setStage(const std::string& stage, float progress) {
    std::lock_guard lock(m_mutex);
    m_status.stage = stage;
    m_status.progress = progress;
}

bool CustomMusicService::hasPendingIsoSwitch() const {
    std::lock_guard lock(m_mutex);
    return !m_pendingIsoSwitch.empty();
}

fs::path CustomMusicService::pendingIsoSwitch() const {
    std::lock_guard lock(m_mutex);
    return m_pendingIsoSwitch;
}

void CustomMusicService::acceptPendingIsoSwitch() {
    fs::path path;
    {
        std::lock_guard lock(m_mutex);
        path = m_pendingIsoSwitch;
        m_pendingIsoSwitch.clear();
    }
    if (path.empty()) return;
    getSettings().backend.isoPath.setValue(path.string());
    config::save();
    log("Staged ISO selected. Restart Dusklight when ready to use it.");
}

void CustomMusicService::dismissPendingIsoSwitch() {
    std::lock_guard lock(m_mutex);
    m_pendingIsoSwitch.clear();
}

bool CustomMusicService::isStagedIsoActive() const {
    const fs::path source = m_project.cleanSourceIso;
    if (source.empty()) return false;
    return isActiveIso(CustomMusicIsoTransaction::stagedPathFor(source));
}

void CustomMusicService::switchToCleanIso() {
    initialize();
    const fs::path clean = cleanSourceIso();
    if (clean.empty()) {
        log("Cannot switch: clean source ISO is not known.");
        return;
    }
    getSettings().backend.isoPath.setValue(clean.string());
    config::save();
    log("Switched to clean ISO. Restart Dusklight, then re-run the randomizer.");
}

}  // namespace dusk::custom_music
