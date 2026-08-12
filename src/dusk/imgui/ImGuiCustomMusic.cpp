#include "ImGuiMenuTools.hpp"

#include "ImGuiEngine.hpp"
#include "borealis/file_select.hpp"
#include "dusk/custom_music/CustomMusicService.h"
#include "dusk/custom_music/CustomMusicIsoTransaction.h"
#include "dusk/main.h"
#include "dusk/settings.h"

#include <tpcm/BgmDatabase.h>

#include <aurora/aurora.h>
#include <aurora/lib/window.hpp>
#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <string>

namespace fs = std::filesystem;

namespace dusk {
namespace {

using custom_music::CustomMusicService;
using custom_music::CustomSong;
fs::path s_pendingIsoRestore;

void applyImportResult(borealis::file_select::Result result) {
    auto& service = CustomMusicService::instance();
    if (result.status == borealis::file_select::Status::Failed) {
        service.report("Import failed: " + result.message);
        return;
    }
    if (result.status == borealis::file_select::Status::Selected && !result.locations.empty()) {
        try {
            service.importPath(result.locations.front());
        } catch (const std::exception& e) {
            service.report(std::string("Import failed: ") + e.what());
        }
    }
}

void applyRestoreResult(borealis::file_select::Result result) {
    auto& service = CustomMusicService::instance();
    if (result.status == borealis::file_select::Status::Failed) {
        service.report("Restore ISO selection failed: " + result.message);
    } else if (result.status == borealis::file_select::Status::Selected && !result.locations.empty()) {
        s_pendingIsoRestore = result.locations.front();
    }
}

const char* poolName(tpcm::MusicBgmPoolMode mode) {
    switch (mode) {
    case tpcm::MusicBgmPoolMode::Scene: return "Scene";
    case tpcm::MusicBgmPoolMode::Dungeon: return "Dungeon";
    case tpcm::MusicBgmPoolMode::Boss: return "Boss";
    default: return "All";
    }
}

bool isReplacementTargetEntry(const tpcm::BgmEntry& entry) {
    return tpcm::RuntimeInfoForBgm(entry).targetAllowed;
}

void drawLibrary(CustomMusicService& service) {
    auto& project = service.project();
    if (ImGui::BeginTable("##custom_music_library", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 120))) {
        ImGui::TableSetupColumn("Song");
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < project.library.size(); ++i) {
            const CustomSong& song = project.library[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(song.title.c_str(), project.selectedSong == static_cast<int>(i),
                    ImGuiSelectableFlags_SpanAllColumns)) {
                project.selectedSong = static_cast<int>(i);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(song.isTprs ? "TPRS" : "MIDI");
            ImGui::TableNextColumn();
            if (song.available) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.55f, 1), "Available");
            else ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Missing");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void drawReplacement(CustomMusicService& service) {
    auto& project = service.project();
    ImGui::SetNextItemWidth(310);
    const char* sourceLabel = project.selectedSong >= 0
        && project.selectedSong < static_cast<int>(project.library.size())
        ? project.library[static_cast<std::size_t>(project.selectedSong)].title.c_str() : "Select source song";
    if (ImGui::BeginCombo("Source song", sourceLabel)) {
        for (std::size_t i = 0; i < project.library.size(); ++i) {
            const CustomSong& s = project.library[i];
            if (ImGui::Selectable(s.title.c_str(), project.selectedSong == static_cast<int>(i)))
                project.selectedSong = static_cast<int>(i);
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(310);
    const char* target = project.replacementTarget >= 0
        && project.replacementTarget < static_cast<int>(tpcm::BGM_TABLE_SIZE)
        && isReplacementTargetEntry(tpcm::BGM_TABLE[project.replacementTarget])
        ? tpcm::BGM_TABLE[project.replacementTarget].displayName : "Select target";
    if (ImGui::BeginCombo("Target song", target)) {
        for (std::size_t i = 0; i < tpcm::BGM_TABLE_SIZE; ++i) {
            if (!isReplacementTargetEntry(tpcm::BGM_TABLE[i])) continue;
            if (ImGui::Selectable(tpcm::BGM_TABLE[i].displayName,
                    project.replacementTarget == static_cast<int>(i))) {
                project.replacementTarget = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    const bool selectedAvailable = project.selectedSong >= 0
        && project.selectedSong < static_cast<int>(project.library.size())
        && project.library[static_cast<std::size_t>(project.selectedSong)].available;
    const bool targetReplaceable = project.replacementTarget >= 0
        && project.replacementTarget < static_cast<int>(tpcm::BGM_TABLE_SIZE)
        && isReplacementTargetEntry(tpcm::BGM_TABLE[project.replacementTarget]);
    const bool blocked = service.status().running || !selectedAvailable
        || !targetReplaceable || project.cleanSourceIso.empty();
    ImGui::BeginDisabled(blocked);
    if (ImGui::Button("Apply Selected Replacement")) {
        const fs::path output = custom_music::CustomMusicIsoTransaction::stagedPathFor(project.cleanSourceIso);
        if (service.isStagedIsoActive()) ImGui::OpenPopup("Staged ISO active##replacement");
        else if (fs::exists(output)) ImGui::OpenPopup("Replace staged ISO?##replacement");
        else service.startApplyReplacement(false);
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Staged ISO active##replacement", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The custom-music ISO is currently loaded by Dusklight.");
        ImGui::TextUnformatted("Switch to the clean ISO, then restart before applying a replacement.");
        if (ImGui::Button("Switch to Clean ISO")) {
            service.switchToCleanIso();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Replace staged ISO?##replacement", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Replace the existing staged custom-music ISO?");
        if (ImGui::Button("Replace")) {
            service.startApplyReplacement(true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void drawRandomizer(CustomMusicService& service) {
    auto& project = service.project();
    ImGui::SetNextItemWidth(150);
    ImGui::InputInt("Seed", &project.randomizerSeed);
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("Pool", poolName(project.poolMode))) {
        for (const auto mode : {tpcm::MusicBgmPoolMode::All, tpcm::MusicBgmPoolMode::Scene,
                 tpcm::MusicBgmPoolMode::Dungeon, tpcm::MusicBgmPoolMode::Boss}) {
            if (ImGui::Selectable(poolName(mode), project.poolMode == mode)) {
                project.poolMode = mode;
                project.forcedAssignments.clear();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Show Music Replacements", &project.showReplacementSpoilers);
    ImGui::Checkbox("Disable enemy music for all songs", &project.disableEnemyMusicGlobal);
    const auto replacements = tpcm::selectMusicBgmPool(project.poolMode);
    const auto targets = tpcm::selectReliableMusicTargets(project.poolMode);
    const auto customSlots = tpcm::selectCustomMusicSlots(project.poolMode);
    const auto compatibility = tpcm::buildMusicCompatibilityReport(project.poolMode);
    const auto targetOnlyCount = static_cast<std::size_t>(std::count_if(
        compatibility.begin(), compatibility.end(), [](const tpcm::MusicCompatibilityEntry& entry) {
            return entry.compatibility == tpcm::MusicSlotCompatibility::TargetOnly;
        }));
    const auto availableCustom = static_cast<std::size_t>(std::count_if(
        project.library.begin(), project.library.end(),
        [](const CustomSong& song) { return song.available; }));
    ImGui::TextDisabled("%d automatic slots; %d contextual targets for forced assignments.",
        static_cast<int>(replacements.size()), static_cast<int>(targetOnlyCount));
    ImGui::TextDisabled("%d of %d available custom songs will be universal candidates.",
        static_cast<int>(std::min(availableCustom, customSlots.size())),
        static_cast<int>(availableCustom));
    std::map<std::uint32_t, std::string> labels;
    for (const auto* entry : replacements) labels[entry->bgmId] = entry->displayName;
    std::size_t customIndex = 0;
    for (const CustomSong& song : project.library) {
        if (!song.available || customIndex >= customSlots.size()) continue;
        labels[customSlots[customIndex++]->bgmId] = song.title + " (custom)";
    }
    ImGui::SeparatorText("Forced Assignments");
    ImGui::BeginDisabled(replacements.empty() || targets.empty());
    if (ImGui::SmallButton("Add Forced Assignment")) {
        const std::uint32_t defaultTarget = targets.front()->bgmId;
        const std::uint32_t defaultReplacement = [&]() {
            for (const auto* entry : replacements) {
                if (entry->bgmId != defaultTarget) return entry->bgmId;
            }
            return replacements.front()->bgmId;
        }();
        project.forcedAssignments.push_back({defaultReplacement, defaultTarget});
    }
    ImGui::EndDisabled();
    ImGui::BeginChild("##forced_assignments", ImVec2(0, 150), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (std::size_t i = 0; i < project.forcedAssignments.size();) {
        auto& assignment = project.forcedAssignments[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string replacementLabel = labels.contains(assignment.replacementBgmId)
            ? labels[assignment.replacementBgmId] : "Select song";
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##forced_replacement", replacementLabel.c_str())) {
            for (const auto& [id, label] : labels) {
                if (ImGui::Selectable(label.c_str(), assignment.replacementBgmId == id))
                    assignment.replacementBgmId = id;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("->");
        ImGui::SameLine();
        const auto* targetEntry = tpcm::FindBgmById(assignment.targetBgmId);
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##forced_target", targetEntry ? targetEntry->displayName : "Select target")) {
            for (const auto* entry : targets) {
                if (ImGui::Selectable(entry->displayName, assignment.targetBgmId == entry->bgmId))
                    assignment.targetBgmId = entry->bgmId;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton("Remove");
        if (assignment.replacementBgmId == assignment.targetBgmId) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "! same song and target - will be ignored");
        }
        ImGui::PopID();
        if (remove) project.forcedAssignments.erase(project.forcedAssignments.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    ImGui::EndChild();

    const bool blocked = service.status().running || project.cleanSourceIso.empty();
    ImGui::BeginDisabled(blocked);
    if (ImGui::Button("Randomize Songs")) {
        const fs::path output = custom_music::CustomMusicIsoTransaction::stagedPathFor(project.cleanSourceIso);
        if (service.isStagedIsoActive()) ImGui::OpenPopup("Staged ISO active##randomizer");
        else if (fs::exists(output)) ImGui::OpenPopup("Replace staged ISO?##randomizer");
        else service.startRandomizeSongs(false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(service.status().running || getSettings().backend.isoPath.getValue().empty());
    if (ImGui::Button("Reset Music Randomizer")) ImGui::OpenPopup("Reset music randomizer?");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Staged ISO active##randomizer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The custom-music ISO is currently loaded by Dusklight.");
        ImGui::TextUnformatted("Switch to the clean ISO, then restart before randomizing.");
        if (ImGui::Button("Switch to Clean ISO")) {
            service.switchToCleanIso();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Replace staged ISO?##randomizer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Replace the existing staged custom-music ISO?");
        if (ImGui::Button("Replace")) {
            service.startRandomizeSongs(true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Reset music randomizer?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Return randomized song routing to vanilla after the next restart?");
        ImGui::TextDisabled("This does not restore physically replaced music inside the ISO.");
        if (ImGui::Button("Reset")) {
            service.resetMusicRandomizer();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void drawIsoTools(CustomMusicService& service) {
    const auto& project = service.project();
    const std::string configured = getSettings().backend.isoPath.getValue();
    const bool busy = service.status().running;

    ImGui::TextDisabled("Configured ISO:");
    ImGui::SameLine();
    ImGui::TextUnformatted(configured.empty() ? "None" : configured.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || configured.empty());
    if (ImGui::SmallButton("Snapshot")) service.startCreateSnapshot();
    ImGui::SameLine();
    if (ImGui::SmallButton("Restore ISO")) {
        borealis::file_select::open_file(
            {
                .parentWindow = aurora::window::get_sdl_window(),
                .filters = {{"ISO image", "iso;gcm"}},
                .defaultLocation = fs::path(configured).parent_path().string(),
            },
            applyRestoreResult);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Clean source:");
    ImGui::SameLine();
    const std::string cleanPath = project.cleanSourceIso.string();
    ImGui::TextUnformatted(cleanPath.empty() ? "None" : cleanPath.c_str());
    ImGui::SameLine();
    const bool onClean = !cleanPath.empty() && fs::weakly_canonical(fs::path(configured)) == fs::weakly_canonical(fs::path(cleanPath));
    ImGui::BeginDisabled(busy || cleanPath.empty() || onClean);
    if (ImGui::SmallButton("Use Clean ISO")) service.switchToCleanIso();
    ImGui::EndDisabled();
    if (!s_pendingIsoRestore.empty()) ImGui::OpenPopup("Restore ISO?");
    if (ImGui::BeginPopupModal("Restore ISO?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Replace Dusklight's configured ISO with this ISO on the next launch?");
        ImGui::TextUnformatted("The selected source ISO will not be changed.");
        ImGui::TextWrapped("%s", s_pendingIsoRestore.string().c_str());
        if (ImGui::Button("Restore")) {
            service.startRestoreIso(s_pendingIsoRestore);
            s_pendingIsoRestore.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            s_pendingIsoRestore.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void drawJob(CustomMusicService& service) {
    const auto status = service.status();
    if (status.running) {
        ImGui::TextUnformatted(status.stage.c_str());
        char progressLabel[16];
        std::snprintf(progressLabel, sizeof(progressLabel), "%.0f%%", status.progress * 100.0f);
        ImGui::ProgressBar(status.progress, ImVec2(-80, 0), progressLabel);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) service.cancel();
    } else if (!status.result.empty()) {
        ImGui::TextColored(status.succeeded ? ImVec4(0.45f, 0.9f, 0.55f, 1)
                                            : ImVec4(1, 0.45f, 0.4f, 1),
            "%s", status.result.c_str());
    }
    if (service.hasPendingIsoSwitch()) ImGui::OpenPopup("Use staged ISO?");
    if (ImGui::BeginPopupModal("Use staged ISO?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Use the completed staged ISO after the next restart?");
        ImGui::TextWrapped("%s", service.pendingIsoSwitch().string().c_str());
        if (ImGui::Button("Use After Next Restart")) {
            service.acceptPendingIsoSwitch();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep Current ISO")) {
            service.dismissPendingIsoSwitch();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void drawLog(CustomMusicService& service) {
    ImGui::SeparatorText("Log");
    if (ImGui::SmallButton("Clear")) service.clearLog();
    ImGui::BeginChild("##custom_music_log", ImVec2(0, 115), ImGuiChildFlags_Borders);
    for (const std::string& line : service.logLines()) ImGui::TextUnformatted(line.c_str());
    ImGui::EndChild();
}

void drawEditor(CustomMusicService& service) {
    auto& project = service.project();
    if (project.selectedSong < 0 || project.selectedSong >= static_cast<int>(project.library.size())) {
        ImGui::TextDisabled("Select a song from the library to edit it.");
        return;
    }
    CustomSong& song = project.library[static_cast<std::size_t>(project.selectedSong)];
    ImGui::InputText("Title", &song.title);
    ImGui::InputText("Author", &song.author);
    ImGui::SliderInt("Master volume", &song.masterVolume, 0, 127);
    ImGui::Checkbox("Disable enemy music", &song.noEnemyMusic);
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &song.loopEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Start", &song.loopStart);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("End", &song.loopEnd);

    if (ImGui::BeginTable("##custom_music_channels", 9,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 300))) {
        for (const char* name : {"Ch", "Notes", "Bank", "Program", "Vol", "Pan", "Reverb", "Drum", "Mute"})
            ImGui::TableSetupColumn(name);
        ImGui::TableHeadersRow();
        for (const auto& row : song.channels) {
            auto& edit = song.overrides[row.channel];
            int bank = edit.bank >= 0 ? edit.bank : row.gameBank;
            int program = edit.program >= 0 ? edit.program : row.gameProgram;
            int volume = edit.volume >= 0 ? edit.volume : row.volume;
            int pan = edit.pan >= 0 ? edit.pan : (row.pan >= 0 ? row.pan : 64);
            int reverb = edit.reverb >= 0 ? edit.reverb : (row.reverb >= 0 ? row.reverb : 0);
            bool drum = edit.drumSet ? edit.drum : song.drumChannels.contains(row.channel);
            ImGui::PushID(row.channel);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", row.channel);
            ImGui::TableNextColumn(); ImGui::Text("%d", row.noteCount);
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::InputInt("##bank", &bank)) edit.bank = bank;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::InputInt("##program", &program)) edit.program = program;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::InputInt("##volume", &volume)) edit.volume = volume;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::InputInt("##pan", &pan)) edit.pan = pan;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::InputInt("##reverb", &reverb)) edit.reverb = reverb;
            ImGui::TableNextColumn(); if (ImGui::Checkbox("##drum", &drum)) { edit.drum = drum; edit.drumSet = true; }
            ImGui::TableNextColumn(); ImGui::Checkbox("##mute", &edit.mute);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Save Library Changes")) {
        try {
            service.saveProject();
            service.report("Saved custom music library changes.");
        } catch (const std::exception& e) {
            service.report(std::string("Could not save library changes: ") + e.what());
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!song.available);
    if (song.isTprs) {
        if (ImGui::Button("Save Changes to TPRS")) {
            try {
                service.saveSelectedSource();
            } catch (const std::exception& e) {
                service.report(std::string("Could not save TPRS: ") + e.what());
            }
        }
    } else {
        if (ImGui::Button("Pack to TPRS")) {
            try {
                service.packSelectedMidiToTprs();
            } catch (const std::exception& e) {
                service.report(std::string("Could not pack MIDI to TPRS: ") + e.what());
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Creates a .tprs next to the .mid with all current settings baked in.");
    }
    ImGui::EndDisabled();
}

}  // namespace

void ImGuiMenuTools::ShowCustomMusic() {
    if (!m_showCustomMusic) return;
    auto& service = CustomMusicService::instance();
    service.initialize();
    ImGui::SetNextWindowSize(ImVec2(900, 650), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Custom Music", &m_showCustomMusic)) {
        ImGui::End();
        return;
    }
    drawIsoTools(service);
    drawJob(service);
    {
        auto& project = service.project();
        ImGui::SameLine();
        if (ImGui::Button("Import File")) {
            const std::string directory = project.lastImportDirectory.string();
            borealis::file_select::open_file(
                {
                    .parentWindow = aurora::window::get_sdl_window(),
                    .filters = {{"MIDI / TPRS", "mid;midi;tprs"}},
                    .defaultLocation = directory,
                },
                applyImportResult);
        }
        ImGui::SameLine();
        if (ImGui::Button("Import Folder")) {
            const std::string directory = project.lastImportDirectory.string();
            borealis::file_select::open_folder(
                {
                    .parentWindow = aurora::window::get_sdl_window(),
                    .defaultLocation = directory,
                },
                applyImportResult);
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh Library")) service.refreshLibrary();
    }
    ImGui::Separator();
    if (ImGui::BeginTabBar("##custom_music_tabs")) {
        if (ImGui::BeginTabItem("Injection")) {
            drawReplacement(service);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Randomizer")) {
            drawRandomizer(service);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MIDI / TPRS Editor")) {
            drawLibrary(service);
            ImGui::Separator();
            drawEditor(service);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    drawLog(service);
    ImGui::End();
}

}  // namespace dusk
