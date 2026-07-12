#include "tpcm/ManifestBuilder.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace tpcm {
namespace {

const BgmEntry* findEntry(const BgmEntry* table, std::size_t tableSize, std::uint32_t bgmId) noexcept {
    for (std::size_t i = 0; i < tableSize; ++i) {
        if (table[i].bgmId == bgmId) {
            return &table[i];
        }
    }
    return nullptr;
}

std::string routeForEntry(const BgmEntry* entry) {
    if (entry == nullptr) {
        return "scene";
    }

    switch (RuntimeTriggerForBgm(*entry)) {
        case BgmTrigger::Main:
            return "main";
        case BgmTrigger::Sub:
            return "sub";
        case BgmTrigger::Fanfare:
            return "fanfare";
        case BgmTrigger::Scene:
            return "scene";
    }
    return "scene";
}

std::string kindName(MusicAssignmentKind kind) {
    switch (kind) {
        case MusicAssignmentKind::Vanilla:
            return "vanilla";
        case MusicAssignmentKind::VanillaSwap:
            return "vanilla_swap";
        case MusicAssignmentKind::CustomTprs:
            return "custom";
    }
    return "unknown";
}

void appendWave(std::vector<std::uint32_t>& waves, std::uint32_t wave) {
    if (wave != 0
        && std::find(waves.begin(), waves.end(), wave) == waves.end()) {
        waves.push_back(wave);
    }
}

void appendPreserveWave(ManifestEntry& entry, std::uint32_t wave) {
    if (wave == 0) {
        return;
    }

    if (std::find(entry.bgmWaves.begin(), entry.bgmWaves.end(), wave) != entry.bgmWaves.end()) {
        return;
    }

    appendWave(entry.preserveBgmWaves, wave);
}

void appendTargetBgmWave(ManifestEntry& entry, std::uint32_t wave) {
    if (wave == 0) {
        return;
    }

    if (std::find(entry.bgmWaves.begin(), entry.bgmWaves.end(), wave) != entry.bgmWaves.end()) {
        return;
    }

    appendWave(entry.targetBgmWaves, wave);
    appendPreserveWave(entry, wave);
}

void appendTargetSeWave(ManifestEntry& entry, std::uint32_t wave) {
    appendWave(entry.targetSeWaves, wave);
}

std::string escapeJson(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << c;
                break;
        }
    }
    return out.str();
}

void writeBgmId(std::ostringstream& out, std::uint32_t bgmId) {
    out << "\"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << bgmId
        << std::dec << std::nouppercase << "\"";
}

}  // namespace

Manifest buildManifest(const std::vector<MusicSlotAssignment>& assignments,
                       const std::vector<GeneratedBank>& generatedBanks,
                       const BgmEntry* bgmTable,
                       std::size_t bgmTableSize) {
    Manifest manifest;
    manifest.generatedBanks = generatedBanks;
    manifest.entries.reserve(assignments.size());

    for (const MusicSlotAssignment& assignment : assignments) {
        if (assignment.kind == MusicAssignmentKind::Vanilla) {
            continue;
        }

        const BgmEntry* original = findEntry(bgmTable, bgmTableSize, assignment.originalBgmId);
        ManifestEntry entry;
        entry.route = routeForEntry(original);
        entry.originalBgmId = assignment.originalBgmId;
        entry.kind = kindName(assignment.kind);
        entry.noEnemyMusic = assignment.noEnemyMusic;

        if (assignment.kind == MusicAssignmentKind::CustomTprs) {
            entry.replacementBgmId = assignment.replacementBgmId;
            appendWave(entry.bgmWaves, assignment.replacementWave1);
        } else {
            entry.replacementBgmId = assignment.replacementBgmId;
            // Match TP Randomizer's vanilla BGM contract: scene replacements
            // swap the BGM id and primary BGM wave only. Extra replacement
            // banks stay out of the sceneChange wave slots; target/local
            // support waves are tracked separately so they can still be
            // diagnosed without making every vanilla swap fight ARAM.
            appendWave(entry.bgmWaves, assignment.replacementWave1);
        }

        if (original != nullptr) {
            const BgmRuntimeInfo runtime = RuntimeInfoForBgm(*original);
            for (std::uint8_t i = 0; i < runtime.bgmWaveCount; ++i) {
                appendTargetBgmWave(entry, runtime.bgmWaves[i]);
            }
            for (std::uint8_t i = 0; i < runtime.seWaveCount; ++i) {
                appendTargetSeWave(entry, runtime.seWaves[i]);
            }
        }

        manifest.entries.push_back(std::move(entry));
    }

    return manifest;
}

std::string toJson(const Manifest& manifest) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": " << manifest.schema << ",\n";
    out << "  \"generated_banks\": [";
    if (!manifest.generatedBanks.empty()) {
        out << "\n";
    }
    for (std::size_t i = 0; i < manifest.generatedBanks.size(); ++i) {
        const GeneratedBank& bank = manifest.generatedBanks[i];
        out << "    {\n";
        out << "      \"bank_id\": " << bank.bankId << ",\n";
        out << "      \"owner\": \"" << escapeJson(bank.ownerBms) << "\",\n";
        out << "      \"wave_count\": " << bank.waveCount << ",\n";
        out << "      \"active\": " << (bank.active ? "true" : "false") << "\n";
        out << "    }" << (i + 1 == manifest.generatedBanks.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"entries\": [";
    if (!manifest.entries.empty()) {
        out << "\n";
    }
    for (std::size_t i = 0; i < manifest.entries.size(); ++i) {
        const ManifestEntry& entry = manifest.entries[i];
        out << "    {\n";
        out << "      \"route\": \"" << escapeJson(entry.route) << "\",\n";
        out << "      \"original_bgm_id\": ";
        writeBgmId(out, entry.originalBgmId);
        out << ",\n";
        out << "      \"replacement_bgm_id\": ";
        writeBgmId(out, entry.replacementBgmId);
        out << ",\n";
        out << "      \"bgm_waves\": [";
        for (std::size_t waveIndex = 0; waveIndex < entry.bgmWaves.size(); ++waveIndex) {
            out << entry.bgmWaves[waveIndex];
            if (waveIndex + 1 != entry.bgmWaves.size()) {
                out << ", ";
            }
        }
        out << "]";
        if (!entry.targetBgmWaves.empty()) {
            out << ",\n";
            out << "      \"target_bgm_waves\": [";
            for (std::size_t waveIndex = 0; waveIndex < entry.targetBgmWaves.size(); ++waveIndex) {
                out << entry.targetBgmWaves[waveIndex];
                if (waveIndex + 1 != entry.targetBgmWaves.size()) {
                    out << ", ";
                }
            }
            out << "]";
        }
        if (!entry.preserveBgmWaves.empty()) {
            out << ",\n";
            out << "      \"preserve_bgm_waves\": [";
            for (std::size_t waveIndex = 0; waveIndex < entry.preserveBgmWaves.size(); ++waveIndex) {
                out << entry.preserveBgmWaves[waveIndex];
                if (waveIndex + 1 != entry.preserveBgmWaves.size()) {
                    out << ", ";
                }
            }
            out << "]";
        }
        if (!entry.targetSeWaves.empty()) {
            out << ",\n";
            out << "      \"target_se_waves\": [";
            for (std::size_t waveIndex = 0; waveIndex < entry.targetSeWaves.size(); ++waveIndex) {
                out << entry.targetSeWaves[waveIndex];
                if (waveIndex + 1 != entry.targetSeWaves.size()) {
                    out << ", ";
                }
            }
            out << "]";
        }
        out << ",\n";
        out << "      \"kind\": \"" << escapeJson(entry.kind) << "\"";
        if (entry.noEnemyMusic) out << ",\n      \"no_enemy_music\": true";
        out << "\n";
        out << "    }" << (i + 1 == manifest.entries.size() ? "\n" : ",\n");
    }
    out << "  ]";
    if (manifest.disableAllEnemyMusic) out << ",\n  \"disable_all_enemy_music\": true";
    out << "\n}\n";
    return out.str();
}

}  // namespace tpcm
