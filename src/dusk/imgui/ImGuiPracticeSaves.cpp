#include "ImGuiPracticeSaves.hpp"
#include "ImGuiMenuTools.hpp"

#include "imgui.h"
#include "fmt/format.h"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dusk/io.hpp"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_controller_pad.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>

namespace dusk {

namespace {

constexpr size_t kMetadataHeaderSize = 32;
constexpr size_t kMetadataEntrySize = 192;
constexpr size_t kNameOffset = 0;
constexpr size_t kNameSize = 32;
constexpr size_t kDescriptionOffset = 32;
constexpr size_t kDescriptionSize = 64;
constexpr size_t kFilenameOffset = 96;
constexpr size_t kFilenameSize = 32;

struct SaveCategoryInfo {
    const char* label;
    const char* folder;
    const char* metadata;
};

constexpr std::array kSaveCategories = {
    SaveCategoryInfo{"Any%", "any_saves", "any"},
    SaveCategoryInfo{"No SQ", "nosq_saves", "nosq"},
    SaveCategoryInfo{"100%", "hundo_saves", "hundo"},
    SaveCategoryInfo{"All Dungeons", "ad_saves", "ad"},
    SaveCategoryInfo{"Glitchless", "glitchless_saves", "glitchless"},
};

constexpr std::array kMainCategoryNames = {
    "cheats",
    "flags",
    "inventory",
    "memory",
    "practice",
    "scene",
    "settings",
    "tools",
    "warping",
};

constexpr std::array kCheatRows = {
    "disable item timer",
    "disable walls",
    "fast bonk recovery",
    "fast movement",
    "infinite air",
    "infinite arrows",
    "infinite bombs",
    "infinite hearts",
    "infinite oil",
    "infinite rupees",
    "infinite slingshot",
    "invincible link",
    "invincible enemies",
    "moon jump",
    "no sinking in sand",
    "super clawshot",
    "transform anywhere",
    "unrestricted items",
};

constexpr std::array kFlagRows = {
    "general",
    "dungeon",
    "portal",
    "rupee",
    "boss flag",
    "map warping",
    "midna charge",
    "midna on back",
    "transform warp",
    "wolf sense",
};

constexpr std::array kInventoryRows = {
    "items",
    "equipment",
    "wallet",
    "current life",
    "heart pieces",
    "hidden skills",
    "bugs",
    "letters",
    "fish journal",
};

constexpr std::array kMemoryRows = {
    "watch address",
    "watch value",
    "poke value",
    "heap info",
    "actor list",
};

constexpr std::array kSceneRows = {
    "time",
    "weather",
    "reload room",
    "event bits",
    "audio",
    "hide actors",
    "show collision",
};

constexpr std::array kSettingsRows = {
    "boot to menu",
    "cursor type",
    "display mode",
    "drop shadow",
    "menu pauses game",
    "menu sfx",
    "reload type",
    "state streaming",
    "swap equips",
    "theme",
};

constexpr std::array kToolRows = {
    "checkers",
    "displays",
    "link debug info",
    "position",
    "angle",
    "speed",
    "load timer",
};

constexpr std::array kWarpRows = {
    "type",
    "stage",
    "room",
    "spawn",
    "layer",
    "execute",
};

int category_index(ImGuiPracticeSaves::SaveCategory category) {
    return static_cast<int>(category);
}

int main_category_index(ImGuiPracticeSaves::MainCategory category) {
    return static_cast<int>(category);
}

uint32_t read_be32(const u8* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

std::string read_fixed_string(const u8* data, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && data[len] != 0) {
        len++;
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

std::filesystem::path save_root_path() {
    return std::filesystem::path("res/gz");
}

std::filesystem::path save_path(ImGuiPracticeSaves::SaveCategory category, const std::string& filename) {
    return save_root_path() / kSaveCategories[category_index(category)].folder / (filename + ".bin");
}

std::filesystem::path metadata_path(ImGuiPracticeSaves::SaveCategory category) {
    const auto& info = kSaveCategories[category_index(category)];
    return save_root_path() / info.folder / (std::string(info.metadata) + ".bin");
}

u32 raw_pad_hold() {
    if (JUTGamePad* pad = mDoCPd_c::getGamePad(PAD_1)) {
        return pad->getButton();
    }
    return mDoCPd_c::getHold(PAD_1);
}

u32 raw_pad_trig() {
    if (JUTGamePad* pad = mDoCPd_c::getGamePad(PAD_1)) {
        return pad->getTrigger();
    }
    return mDoCPd_c::getTrig(PAD_1);
}

constexpr u32 kPracticeMenuControllerMask = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT |
                                            PAD_BUTTON_RIGHT | PAD_BUTTON_A | PAD_BUTTON_B |
                                            PAD_TRIGGER_L | PAD_TRIGGER_R;

}  // namespace

void ImGuiPracticeSaves::loadMetadata() {
    m_loaded = true;
    for (auto& saves : m_saves) {
        saves.clear();
    }
    for (int i = 0; i < static_cast<int>(SaveCategory::Count); i++) {
        loadCategoryMetadata(static_cast<SaveCategory>(i));
    }

    m_statusMsg.clear();
}

void ImGuiPracticeSaves::loadCategoryMetadata(SaveCategory category) {
    auto& saves = m_saves[category_index(category)];
    saves.clear();
    try {
        const auto data = io::FileStream::ReadAllBytes(metadata_path(category));
        if (data.size() < kMetadataHeaderSize) {
            return;
        }

        const uint32_t count = read_be32(data.data());
        const size_t requiredSize = kMetadataHeaderSize + (static_cast<size_t>(count) * kMetadataEntrySize);
        if (data.size() < requiredSize) {
            return;
        }

        saves.reserve(count);
        for (uint32_t i = 0; i < count; i++) {
            const u8* entry = data.data() + kMetadataHeaderSize + (static_cast<size_t>(i) * kMetadataEntrySize);
            PracticeSaveEntry save;
            save.name = read_fixed_string(entry + kNameOffset, kNameSize);
            save.description = read_fixed_string(entry + kDescriptionOffset, kDescriptionSize);
            save.filename = read_fixed_string(entry + kFilenameOffset, kFilenameSize);
            if (!save.name.empty() && !save.filename.empty()) {
                saves.push_back(std::move(save));
            }
        }
    } catch (const std::exception& e) {
        m_statusMsg = fmt::format("Failed to load {} practice metadata: {}",
                                  kSaveCategories[category_index(category)].label, e.what());
    }
}

bool ImGuiPracticeSaves::loadPracticeSave(const PracticeSaveEntry& entry) {
    try {
        const auto data = io::FileStream::ReadAllBytes(save_path(m_saveCategory, entry.filename));
        if (data.size() < sizeof(dSv_save_c)) {
            m_statusMsg = fmt::format("{} is too small to contain a raw save.", entry.filename);
            return false;
        }

        dSv_save_c save = {};
        std::memcpy(&save, data.data(), sizeof(dSv_save_c));

        auto& returnPlace = save.getPlayer().getPlayerReturnPlace();
        if (returnPlace.getName()[0] == '\0') {
            m_statusMsg = fmt::format("{} has no return stage.", entry.name);
            return false;
        }

        m_pendingSavedata = save;
        m_loadInProgress = true;
        m_loadPeekSeen = false;
        getTransientSettings().stateShareLoadActive = true;

        dComIfGp_setNextStage(returnPlace.getName(),
                              returnPlace.getPlayerStatus(),
                              returnPlace.getRoomNo(),
                              -1,
                              0.0f,
                              0,
                              1,
                              0,
                              0,
                              1,
                              3);

        m_statusMsg = fmt::format("Loading {}.", entry.name);
        return true;
    } catch (const std::exception& e) {
        m_statusMsg = fmt::format("Failed to load {}: {}", entry.name, e.what());
        return false;
    }
}

std::vector<ImGuiPracticeSaves::PracticeSaveEntry>& ImGuiPracticeSaves::currentSaves() {
    return m_saves[category_index(m_saveCategory)];
}

const std::vector<ImGuiPracticeSaves::PracticeSaveEntry>& ImGuiPracticeSaves::currentSaves() const {
    return m_saves[category_index(m_saveCategory)];
}

void ImGuiPracticeSaves::suppressControllerInput() {
    m_nextControllerInputTime = ImGui::GetTime() + 0.18;
    consumeControllerInput();
}

void ImGuiPracticeSaves::consumeControllerInput() {
    auto& pad = mDoCPd_c::getCpadInfo(PAD_1);
    pad.mButtonFlags &= ~kPracticeMenuControllerMask;
    pad.mPressedButtonFlags &= ~kPracticeMenuControllerMask;
    pad.mTriggerLeft = 0.0f;
    pad.mTriggerRight = 0.0f;
    pad.mHoldLockL = 0;
    pad.mTrigLockL = 0;
    pad.mHoldLockR = 0;
    pad.mTrigLockR = 0;
}

void ImGuiPracticeSaves::handleController(bool& open) {
    const u32 hold = raw_pad_hold() | raw_pad_trig();
    const double now = ImGui::GetTime();
    if (now < m_nextControllerInputTime) {
        return;
    }

    auto accept = [&](u32 button, double cooldown = 0.12) {
        if ((hold & button) == 0) {
            return false;
        }
        m_nextControllerInputTime = now + cooldown;
        return true;
    };
    auto acceptPress = [&](u32 button, double cooldown = 0.16) {
        if ((hold & button) == 0 || (m_lastControllerHold & button) != 0) {
            return false;
        }
        m_nextControllerInputTime = now + cooldown;
        return true;
    };

    if (accept(PAD_BUTTON_B, 0.18)) {
        if (m_focusSaveList) {
            m_focusSaveList = false;
        } else {
            open = false;
        }
        return;
    }

    if (m_focusSaveList && m_mainCategory == MainCategory::Practice) {
        if (acceptPress(PAD_TRIGGER_L)) {
            int next = category_index(m_saveCategory) - 1;
            if (next < 0) {
                next = static_cast<int>(SaveCategory::Count) - 1;
            }
            m_saveCategory = static_cast<SaveCategory>(next);
            m_selectedSave = 0;
            m_scrollSelectedSave = true;
            return;
        }
        if (acceptPress(PAD_TRIGGER_R)) {
            int next = (category_index(m_saveCategory) + 1) % static_cast<int>(SaveCategory::Count);
            m_saveCategory = static_cast<SaveCategory>(next);
            m_selectedSave = 0;
            m_scrollSelectedSave = true;
            return;
        }

        const int count = static_cast<int>(currentSaves().size());
        if (count > 0) {
            if (accept(PAD_BUTTON_LEFT, 0.18)) {
                m_selectedSave = std::max(0, m_selectedSave - 10);
                m_scrollSelectedSave = true;
                return;
            }
            if (accept(PAD_BUTTON_RIGHT, 0.18)) {
                m_selectedSave = std::min(count - 1, m_selectedSave + 10);
                m_scrollSelectedSave = true;
                return;
            }
            if (accept(PAD_BUTTON_UP, 0.18)) {
                m_selectedSave = std::max(0, m_selectedSave - 1);
                m_scrollSelectedSave = true;
                return;
            }
            if (accept(PAD_BUTTON_DOWN, 0.18)) {
                m_selectedSave = std::min(count - 1, m_selectedSave + 1);
                m_scrollSelectedSave = true;
                return;
            }
            if (accept(PAD_BUTTON_A, 0.22) && !m_loadInProgress && !getTransientSettings().stateShareLoadActive) {
                loadPracticeSave(currentSaves()[m_selectedSave]);
                return;
            }
        }
    } else {
        if (accept(PAD_BUTTON_LEFT)) {
            m_focusSaveList = false;
            return;
        }
        if (m_mainCategory == MainCategory::Practice && accept(PAD_BUTTON_RIGHT)) {
            m_focusSaveList = true;
            return;
        }

        int category = main_category_index(m_mainCategory);
        if (accept(PAD_BUTTON_UP, 0.18)) {
            category = std::max(0, category - 1);
            m_mainCategory = static_cast<MainCategory>(category);
            return;
        }
        if (accept(PAD_BUTTON_DOWN, 0.18)) {
            category = std::min(static_cast<int>(MainCategory::Count) - 1, category + 1);
            m_mainCategory = static_cast<MainCategory>(category);
            return;
        }
        if (m_mainCategory == MainCategory::Practice && accept(PAD_BUTTON_A, 0.16)) {
            m_focusSaveList = true;
        }
    }
}

void ImGuiPracticeSaves::drawCategoryList() {
    ImGui::BeginChild("##gz_main_categories", ImVec2(170.0f, 0.0f), false);
    for (int i = 0; i < static_cast<int>(MainCategory::Count); i++) {
        const bool selected = i == main_category_index(m_mainCategory);
        const ImVec4 color = selected ? ImVec4(0.1f, 0.9f, 0.1f, 1.0f) :
                                        ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        if (ImGui::Selectable(kMainCategoryNames[i], selected, 0, ImVec2(150.0f, 0.0f))) {
            m_mainCategory = static_cast<MainCategory>(i);
            m_focusSaveList = false;
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

void ImGuiPracticeSaves::drawPracticePanel() {
    const bool canLoad = dusk::IsGameLaunched && !m_loadInProgress && !getTransientSettings().stateShareLoadActive;

    ImGui::BeginChild("##gz_practice_panel", ImVec2(560.0f, 0.0f), true);
    for (int i = 0; i < static_cast<int>(SaveCategory::Count); i++) {
        if (i > 0) {
            ImGui::SameLine();
        }
        const bool selected = i == category_index(m_saveCategory);
        const float tabWidth = ImGui::CalcTextSize(kSaveCategories[i].label).x + (ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::Selectable(kSaveCategories[i].label, selected, 0, ImVec2(tabWidth, 0.0f))) {
            if (!selected) {
                m_saveCategory = static_cast<SaveCategory>(i);
                m_selectedSave = 0;
                m_focusSaveList = true;
                m_scrollSelectedSave = true;
            }
        }
    }

    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##practice_saves", ImVec2(0.0f, rowH * 13), true);
    const auto& saves = currentSaves();
    if (saves.empty()) {
        ImGui::TextDisabled("No saves found.");
    }

    for (int i = 0; i < static_cast<int>(saves.size()); i++) {
        const auto& save = saves[i];
        ImGui::PushID(i);
        const bool selected = m_focusSaveList && i == m_selectedSave;
        if (ImGui::Selectable(save.name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(310.0f, 0.0f))) {
            m_selectedSave = i;
            m_focusSaveList = true;
            if (ImGui::IsMouseDoubleClicked(0) && canLoad) {
                loadPracticeSave(save);
            }
        }
        if (!save.description.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", save.description.c_str());
        }
        if (selected && m_scrollSelectedSave) {
            ImGui::SetScrollHereY(0.5f);
            m_scrollSelectedSave = false;
        }

        ImGui::SameLine();
        if (!canLoad) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Load##practice_save_load")) {
            m_selectedSave = i;
            loadPracticeSave(save);
        }
        if (!canLoad) {
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void ImGuiPracticeSaves::drawGenericPanel() {
    const char* const* rows = nullptr;
    int rowCount = 0;

    switch (m_mainCategory) {
    case MainCategory::Cheats:
        rows = kCheatRows.data();
        rowCount = static_cast<int>(kCheatRows.size());
        break;
    case MainCategory::Flags:
        rows = kFlagRows.data();
        rowCount = static_cast<int>(kFlagRows.size());
        break;
    case MainCategory::Inventory:
        rows = kInventoryRows.data();
        rowCount = static_cast<int>(kInventoryRows.size());
        break;
    case MainCategory::Memory:
        rows = kMemoryRows.data();
        rowCount = static_cast<int>(kMemoryRows.size());
        break;
    case MainCategory::Scene:
        rows = kSceneRows.data();
        rowCount = static_cast<int>(kSceneRows.size());
        break;
    case MainCategory::Settings:
        rows = kSettingsRows.data();
        rowCount = static_cast<int>(kSettingsRows.size());
        break;
    case MainCategory::Tools:
        rows = kToolRows.data();
        rowCount = static_cast<int>(kToolRows.size());
        break;
    case MainCategory::Warping:
        rows = kWarpRows.data();
        rowCount = static_cast<int>(kWarpRows.size());
        break;
    default:
        break;
    }

    ImGui::BeginChild("##gz_generic_panel", ImVec2(560.0f, 0.0f), true);
    static std::array<std::array<bool, 64>, static_cast<int>(MainCategory::Count)> sOptionStates = {};
    auto& optionStates = sOptionStates[main_category_index(m_mainCategory)];
    for (int i = 0; i < rowCount; i++) {
        ImGui::PushID(i);
        if (m_mainCategory == MainCategory::Warping && i == rowCount - 1) {
            ImGui::Button(rows[i], ImVec2(160.0f, 0.0f));
        } else {
            ImGui::Checkbox(rows[i], &optionStates[i]);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void ImGuiPracticeSaves::tickPendingApply() {
    if (m_pendingSavedata.has_value() && !dComIfGp_isEnableNextStage()) {
        g_dComIfG_gameInfo.info.mSavedata = *m_pendingSavedata;
        m_pendingSavedata.reset();

        dComIfGs_getSave(g_dComIfG_gameInfo.info.getDan().mStageNo);
        dKy_set_nexttime(dComIfGs_getTime());
        dComIfGp_offOxygenShowFlag();
        dComIfGp_setMaxOxygen(600);
        dComIfGp_setOxygen(600);
    }

    if (!m_loadInProgress) {
        return;
    }

    if (fopOvlpM_IsPeek()) {
        m_loadPeekSeen = true;
    } else if (m_loadPeekSeen) {
        m_loadInProgress = false;
        m_loadPeekSeen = false;
        getTransientSettings().stateShareLoadActive = false;
    }
}

void ImGuiPracticeSaves::draw(bool& open) {
    if (dusk::IsGameLaunched) {
        tickPendingApply();
    }

    if (!m_loaded) {
        loadMetadata();
    }

    if (!open) {
        return;
    }

    handleController(open);
    m_lastControllerHold = raw_pad_hold() | raw_pad_trig();
    consumeControllerInput();
    if (!open) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(18.0f, 36.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(760.0f, 500.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("Practice Tools", &open,
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    drawCategoryList();
    ImGui::SameLine();

    if (m_mainCategory == MainCategory::Practice) {
        drawPracticePanel();
    } else {
        drawGenericPanel();
    }

    if (!m_statusMsg.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_statusMsg.c_str());
    }

    ImGui::End();
}

void ImGuiMenuTools::ShowPracticeSaves() {
    static bool sComboHeld = false;
    const bool comboDown = dusk::IsGameLaunched &&
                           (raw_pad_hold() & (PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_DOWN)) ==
                               (PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_BUTTON_DOWN);
    if (comboDown && !sComboHeld) {
        togglePracticeSaves();
        m_practiceSaves.suppressControllerInput();
    }
    sComboHeld = comboDown;

    getTransientSettings().practiceMenuInputCapture = m_showPracticeSaves;
    m_practiceSaves.draw(m_showPracticeSaves);
    getTransientSettings().practiceMenuInputCapture = m_showPracticeSaves;
}

}
