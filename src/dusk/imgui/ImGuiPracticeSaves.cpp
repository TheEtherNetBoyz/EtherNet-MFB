#include "ImGuiPracticeSaves.hpp"
#include "ImGuiMenuTools.hpp"

#include "imgui.h"
#include "fmt/format.h"

#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JUtility/JUTResFont.h"
#include "JSystem/JUtility/TColor.h"
#include "SSystem/SComponent/c_math.h"
#include "dolphin/gx.h"
#include "m_Do/m_Do_ext.h"

#include "c/c_damagereaction.h"
#include "SSystem/SComponent/c_counter.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "d/d_kankyo.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_b_ds.h"
#include "d/actor/d_a_e_zs.h"
#include "d/actor/d_a_kago.h"
#include "d/actor/d_a_player.h"
#include "dusk/config.hpp"
#include "dusk/frame_interpolation.h"
#include "dusk/io.hpp"
#include "dusk/main.h"
#include "dusk/map_loader_definitions.h"
#include "dusk/settings.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_graphic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <vector>

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
constexpr size_t kPlacementOffset = 128;

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

int category_index(ImGuiPracticeSaves::SaveCategory category) {
    return static_cast<int>(category);
}

int main_category_index(ImGuiPracticeSaves::MainCategory category) {
    return static_cast<int>(category);
}

// Native menu: which categories present a sub-category list (gz-style) before
// their rows/saves, rather than going straight to rows.
bool category_has_sublist(ImGuiPracticeSaves::MainCategory category) {
    return category == ImGuiPracticeSaves::MainCategory::Practice ||
           category == ImGuiPracticeSaves::MainCategory::Tools ||
           category == ImGuiPracticeSaves::MainCategory::Scene;
}

uint32_t read_be32(const u8* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

s16 read_be16(const u8* data) {
    return static_cast<s16>((static_cast<u16>(data[0]) << 8) |
                            static_cast<u16>(data[1]));
}

float read_be_float(const u8* data) {
    uint32_t raw = read_be32(data);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

enum class PracticeSaveCallback : int {
    None,
    OrdonGateClip,
    SetupHugo,
    StallordInit,
    StallordSkipCAD,
    StallordSkipJoseph,
    StallordPhase2,
    SetNextStageLayer1,
    SetNextStageLayer3,
    SetNextStageLayer4SkipDemo,
    SetNextStageLayer5,
    SetEscortNextStage,
    GiveEscortKeys,
    SetSnowpeakBossKeyNextStage,
    GorgeVoidInit,
    GorgeVoidPostLoad,
    PlummMinigameInit,
};

struct PracticeSaveCallbacks {
    PracticeSaveCallback stageInit = PracticeSaveCallback::None;
    PracticeSaveCallback playerInit = PracticeSaveCallback::None;
};

PracticeSaveCallbacks practice_save_callbacks(ImGuiPracticeSaves::SaveCategory category, int index) {
    switch (category) {
    case ImGuiPracticeSaves::SaveCategory::Any:
        if (index == 0) {
            return {PracticeSaveCallback::None, PracticeSaveCallback::OrdonGateClip};
        }
        if (index == 4) {
            return {PracticeSaveCallback::None, PracticeSaveCallback::SetupHugo};
        }
        if (index == 9) {
            return {PracticeSaveCallback::GorgeVoidInit, PracticeSaveCallback::None};
        }
        if (index == 20) {
            return {PracticeSaveCallback::PlummMinigameInit, PracticeSaveCallback::PlummMinigameInit};
        }
        if (index == 21) {
            return {PracticeSaveCallback::SetNextStageLayer4SkipDemo, PracticeSaveCallback::None};
        }
        if (index == 40) {
            return {PracticeSaveCallback::StallordInit, PracticeSaveCallback::StallordInit};
        }
        if (index == 41) {
            return {PracticeSaveCallback::StallordInit, PracticeSaveCallback::StallordSkipCAD};
        }
        if (index == 42 || index == 43) {
            return {PracticeSaveCallback::StallordInit, PracticeSaveCallback::StallordSkipJoseph};
        }
        if (index == 44) {
            return {PracticeSaveCallback::StallordInit, PracticeSaveCallback::StallordPhase2};
        }
        if (index == 64) {
            return {PracticeSaveCallback::SetNextStageLayer1, PracticeSaveCallback::None};
        }
        break;
    case ImGuiPracticeSaves::SaveCategory::NoSq:
        if (index == 1) {
            return {PracticeSaveCallback::None, PracticeSaveCallback::SetupHugo};
        }
        if (index == 11) {
            return {PracticeSaveCallback::SetNextStageLayer3, PracticeSaveCallback::None};
        }
        if (index == 12) {
            return {PracticeSaveCallback::SetEscortNextStage, PracticeSaveCallback::GiveEscortKeys};
        }
        if (index == 23) {
            return {PracticeSaveCallback::StallordInit, PracticeSaveCallback::StallordInit};
        }
        break;
    case ImGuiPracticeSaves::SaveCategory::Hundred:
        if (index == 0) {
            return {PracticeSaveCallback::SetNextStageLayer5, PracticeSaveCallback::None};
        }
        if (index == 24) {
            return {PracticeSaveCallback::SetNextStageLayer3, PracticeSaveCallback::None};
        }
        if (index == 25) {
            return {PracticeSaveCallback::SetEscortNextStage, PracticeSaveCallback::GiveEscortKeys};
        }
        if (index == 34) {
            return {PracticeSaveCallback::SetNextStageLayer4SkipDemo, PracticeSaveCallback::None};
        }
        if (index == 40) {
            return {PracticeSaveCallback::StallordInit, PracticeSaveCallback::StallordInit};
        }
        if (index == 46) {
            return {PracticeSaveCallback::SetSnowpeakBossKeyNextStage, PracticeSaveCallback::None};
        }
        break;
    case ImGuiPracticeSaves::SaveCategory::AllDungeons:
        if (index == 3) {
            return {PracticeSaveCallback::None, PracticeSaveCallback::SetupHugo};
        }
        if (index == 30) {
            return {PracticeSaveCallback::StallordInit, PracticeSaveCallback::StallordInit};
        }
        break;
    default:
        break;
    }
    return {};
}

void apply_ordon_gate_clip_callback() {
    auto* rock = fopAcM_searchFromName("stoneB", 0xFFFFFFFF, 0x00FF6511);
    if (rock != nullptr) {
        rock->current.pos = cXyz(400.0f, 307.8f, -11365.0f);
        rock->old.pos = rock->current.pos;
        rock->shape_angle.y = 0x16F8;
    }
}

void apply_hugo_callback() {
    auto* hugo = fopAcM_SearchByName(fpcNm_E_RD_e);
    if (hugo != nullptr) {
        const cXyz pos(-289.9785f, 401.54f, -18533.078f);
        hugo->current.pos = pos;
        hugo->old.pos = pos;
        hugo->home.pos = pos;
        hugo->speed.set(0.0f, 0.0f, 0.0f);
        hugo->speedF = 0.0f;
        hugo->current.angle.y = 0x16F8;
        hugo->shape_angle.y = 0x16F8;
        hugo->home.angle.y = 0x16F8;
    }
}

static void* find_staltroop_actor(void* actor, void*) {
    if (fopAcM_IsActor(actor) && fopAcM_GetName(actor) == fpcNm_E_ZS_e) {
        return actor;
    }

    return nullptr;
}

void apply_stallord_init_callback() {
    if (auto* stallord = static_cast<daB_DS_c*>(fopAcM_SearchByName(fpcNm_B_DS_e))) {
        dComIfGs_onZoneSwitch(5, fopAcM_GetRoomNo(stallord));
    }

    cDmr_SkipInfo = 1;
}

void apply_stallord_skip_joseph_callback(bool moveToSkipPosition) {
    apply_stallord_init_callback();

    if (auto* joseph = static_cast<daE_ZS_c*>(fopAcM_Search(find_staltroop_actor, nullptr))) {
        joseph->duskSetupStallordSkip(moveToSkipPosition);
    }
}

void apply_stallord_phase2_callback() {
    apply_stallord_init_callback();

    if (auto* stallord = static_cast<daB_DS_c*>(fopAcM_SearchByName(fpcNm_B_DS_e))) {
        stallord->duskSetupStallordPhase2();
    }
}

void set_next_stage_layer(s8 layer) {
    if (auto* nextStage = dComIfGp_getNextStartStage()) {
        nextStage->setLayer(layer);
    }
}

void set_next_stage_route(s8 roomNo, s16 point, s8 layer) {
    auto& play = g_dComIfG_gameInfo.play;
    char stageName[8] = {};
    std::strncpy(stageName, play.getNextStageName(), sizeof(stageName) - 1);
    play.setNextStage(stageName, roomNo, point, layer, play.getNextStageWipe(), play.getNextStageWipeSpeed());
}

void apply_empty_lake_hylia_callback() {
    set_next_stage_layer(4);
    cDmr_SkipInfo = 1;
}

void apply_plumm_minigame_init_callback() {
    set_next_stage_layer(4);
    g_dComIfG_gameInfo.info.getRestart().setLastSceneInfo(0.0f, 10, 0);

    auto* kago = static_cast<daKago_c*>(fopAcM_SearchByName(fpcNm_KAGO_e));
    auto* player = static_cast<daAlink_c*>(daPy_getPlayerActorClass());
    if (kago != nullptr && player != nullptr) {
        kago->setMidnaRideOn();
        kago->setPlayerRideOn();
        player->procWolfCargoCarryInit();
    }
}

void apply_give_escort_keys_callback() {
    dComIfGs_setKeyNum(2);
}

void apply_gorge_void_save_state_callback() {
    g_dComIfG_gameInfo.info.getMemory().getBit().onSwitch(0);
    dComIfGs_offSwitch(21, 3);
    dComIfGs_putSave(g_dComIfG_gameInfo.info.getDan().mStageNo);
    dComIfGs_setTransformStatus(TF_STATUS_WOLF);

    g_dComIfG_gameInfo.info.getRestart().setLastSceneInfo(0.0f, 0x28000000, 0);
    g_dComIfG_gameInfo.info.getRestart().setStartPoint(2);

    cXyz pos(-11856.857f, -5700.0f, 56661.5f);
    g_dComIfG_gameInfo.info.getRestart().setRoom(pos, 24169, 3);

    dComIfGs_setLife(12);
    cDmr_SkipInfo = 1;
}

void apply_gorge_void_init_callback() {
    apply_gorge_void_save_state_callback();
    g_dComIfG_gameInfo.play.setNextStage("F_SP121", 3, 2, 0xE, 13, 0);
}

void apply_gorge_void_post_load_callback() {
    dComIfGs_onSwitch(21, 3);
}

void apply_post_save_inject_callback(PracticeSaveCallback callback) {
    switch (callback) {
    case PracticeSaveCallback::GorgeVoidInit:
        apply_gorge_void_save_state_callback();
        break;
    default:
        break;
    }
}

void apply_player_init_callback(PracticeSaveCallback callback) {
    switch (callback) {
    case PracticeSaveCallback::OrdonGateClip:
        apply_ordon_gate_clip_callback();
        break;
    case PracticeSaveCallback::SetupHugo:
        apply_hugo_callback();
        break;
    case PracticeSaveCallback::StallordInit:
        apply_stallord_init_callback();
        break;
    case PracticeSaveCallback::StallordSkipCAD:
        apply_stallord_skip_joseph_callback(false);
        break;
    case PracticeSaveCallback::StallordSkipJoseph:
        apply_stallord_skip_joseph_callback(true);
        break;
    case PracticeSaveCallback::StallordPhase2:
        apply_stallord_phase2_callback();
        break;
    case PracticeSaveCallback::SetNextStageLayer1:
        set_next_stage_layer(1);
        break;
    case PracticeSaveCallback::SetNextStageLayer3:
        set_next_stage_layer(3);
        break;
    case PracticeSaveCallback::SetNextStageLayer4SkipDemo:
        apply_empty_lake_hylia_callback();
        break;
    case PracticeSaveCallback::SetNextStageLayer5:
        set_next_stage_layer(5);
        break;
    case PracticeSaveCallback::SetEscortNextStage:
        set_next_stage_route(13, 98, 2);
        break;
    case PracticeSaveCallback::GiveEscortKeys:
        apply_give_escort_keys_callback();
        break;
    case PracticeSaveCallback::SetSnowpeakBossKeyNextStage: {
        const s8 layer = dComIfGp_getNextStartStage() != nullptr ? dComIfGp_getNextStartStage()->getLayer() : -1;
        set_next_stage_route(11, 0, layer);
        break;
    }
    case PracticeSaveCallback::GorgeVoidInit:
        apply_gorge_void_init_callback();
        break;
    case PracticeSaveCallback::GorgeVoidPostLoad:
        apply_gorge_void_post_load_callback();
        break;
    case PracticeSaveCallback::PlummMinigameInit:
        apply_plumm_minigame_init_callback();
        break;
    default:
        break;
    }
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

u32 game_pad_hold() {
    return mDoCPd_c::getHold(PAD_1);
}

u32 game_pad_trig() {
    return mDoCPd_c::getTrig(PAD_1);
}

constexpr u32 kPracticeMenuControllerMask = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT |
                                            PAD_BUTTON_RIGHT | PAD_BUTTON_A | PAD_BUTTON_B |
                                            PAD_TRIGGER_L | PAD_TRIGGER_R;

struct GzWarpState {
    int region = 0;
    int map = 0;
    int room = 0;
    int spawn = 0;
    int layer = -1;
};

int s_gzToolsTab = 1;
int s_gzSceneTab = 0;
GzWarpState s_gzWarpState;
int s_gzDrawRow = 0;
int s_gzSelectedRow = -1;
bool s_gzPanelFocused = false;
bool s_gzScrollSelectedRow = false;

void clamp_gz_warp_state(GzWarpState& state) {
    if (gameRegions.empty()) {
        state.region = state.map = state.room = state.spawn = 0;
        state.layer = std::clamp(state.layer, -1, 14);
        return;
    }
    state.region = std::clamp(state.region, 0, static_cast<int>(gameRegions.size()) - 1);
    const auto& region = gameRegions[state.region];
    state.map = region.maps.empty() ? 0 : std::clamp(state.map, 0, static_cast<int>(region.maps.size()) - 1);
    if (region.maps.empty()) {
        state.room = state.spawn = 0;
        return;
    }
    const auto& map = region.maps[state.map];
    state.room = map.mapRooms.empty() ? 0 : std::clamp(state.room, 0, static_cast<int>(map.mapRooms.size()) - 1);
    if (map.mapRooms.empty()) {
        state.spawn = 0;
        return;
    }
    const auto& room = map.mapRooms[state.room];
    state.spawn = room.roomPoints.empty() ? 0 : std::clamp(state.spawn, 0, static_cast<int>(room.roomPoints.size()) - 1);
    state.layer = std::clamp(state.layer, -1, 14);
}

int gz_generic_row_count(ImGuiPracticeSaves::MainCategory category) {
    switch (category) {
    case ImGuiPracticeSaves::MainCategory::Cheats: return 18;
    case ImGuiPracticeSaves::MainCategory::Tools:
        if (s_gzToolsTab == 0) return 7;
        if (s_gzToolsTab == 1) return 7;
        return 3;
    case ImGuiPracticeSaves::MainCategory::Scene:
        if (s_gzSceneTab == 0) return 3;
        if (s_gzSceneTab == 1) return 1;
        return 2;
    case ImGuiPracticeSaves::MainCategory::Settings: return 16;
    case ImGuiPracticeSaves::MainCategory::Warping: return 6;
    case ImGuiPracticeSaves::MainCategory::Flags: return static_cast<int>(kFlagRows.size());
    case ImGuiPracticeSaves::MainCategory::Inventory: return static_cast<int>(kInventoryRows.size());
    case ImGuiPracticeSaves::MainCategory::Memory: return static_cast<int>(kMemoryRows.size());
    default: return 0;
    }
}

int sorted_adjacent_index(int count, int current, int delta, const std::function<bool(int, int)>& less) {
    if (count <= 0) {
        return 0;
    }

    current = std::clamp(current, 0, count - 1);
    std::vector<int> indices;
    indices.reserve(count);
    for (int i = 0; i < count; i++) {
        indices.push_back(i);
    }

    std::sort(indices.begin(), indices.end(), less);
    const auto it = std::find(indices.begin(), indices.end(), current);
    int pos = it == indices.end() ? 0 : static_cast<int>(std::distance(indices.begin(), it));
    pos = (pos + (delta < 0 ? -1 : 1) + count) % count;
    return indices[pos];
}

bool alphabetical_less(const char* a, const char* b) {
    return std::strcmp(a != nullptr ? a : "", b != nullptr ? b : "") < 0;
}

void gz_set_bool(ConfigVar<bool>& value, bool enabled = true) {
    if (!enabled) return;
    value.setValue(!value.getValue());
    config::save();
}

bool gz_activate_generic_row(ImGuiPracticeSaves::MainCategory category, int row) {
    auto& s = getSettings();
    const bool cheatsEnabled = !s.game.speedrunMode;
    switch (category) {
    case ImGuiPracticeSaves::MainCategory::Cheats:
        switch (row) {
        case 0: gz_set_bool(s.game.enableIndefiniteItemDrops, cheatsEnabled); break;
        case 4: gz_set_bool(s.game.infiniteOxygen, cheatsEnabled); break;
        case 5: gz_set_bool(s.game.infiniteArrows, cheatsEnabled); break;
        case 6: gz_set_bool(s.game.infiniteBombs, cheatsEnabled); break;
        case 7: gz_set_bool(s.game.infiniteHearts, cheatsEnabled); break;
        case 8: gz_set_bool(s.game.infiniteOil, cheatsEnabled); break;
        case 9: gz_set_bool(s.game.infiniteRupees, cheatsEnabled); break;
        case 10: gz_set_bool(s.game.infiniteSeeds, cheatsEnabled); break;
        case 12: gz_set_bool(s.game.invincibleEnemies, cheatsEnabled); break;
        case 13: gz_set_bool(s.game.moonJump, cheatsEnabled); break;
        case 15: gz_set_bool(s.game.superClawshot, cheatsEnabled); break;
        case 16: gz_set_bool(s.game.canTransformAnywhere, cheatsEnabled); break;
        default: break;
        }
        break;
    case ImGuiPracticeSaves::MainCategory::Tools:
        if (s_gzToolsTab == 0) {
            if (row == 3) gz_set_bool(s.game.gorgeVoidChecker);
        } else if (s_gzToolsTab == 1) {
            if (row == 1) gz_set_bool(s.game.showSpeedrunRTATimer, s.game.speedrunMode);
            if (row == 2) gz_set_bool(s.game.showInputViewer);
        } else if (s_gzToolsTab == 2) {
            if (row == 0) gz_set_bool(s.game.freeCamera);
            if (row == 1) gz_set_bool(s.game.moveLink, !s.game.speedrunMode);
        }
        break;
    case ImGuiPracticeSaves::MainCategory::Warping:
        if (row == 5 && dusk::IsGameLaunched) {
            clamp_gz_warp_state(s_gzWarpState);
            const auto& region = gameRegions[s_gzWarpState.region];
            const auto& map = region.maps[s_gzWarpState.map];
            const auto& room = map.mapRooms[s_gzWarpState.room];
            dComIfGp_setNextStage(map.mapFile, room.roomPoints[s_gzWarpState.spawn], room.roomNo, s_gzWarpState.layer);
            return true;
        }
        break;
    default: break;
    }
    return false;
}

void gz_adjust_generic_row(ImGuiPracticeSaves::MainCategory category, int row, int delta) {
    if (category == ImGuiPracticeSaves::MainCategory::Tools) {
        s_gzToolsTab = (s_gzToolsTab + delta + 3) % 3;
        return;
    }
    if (category == ImGuiPracticeSaves::MainCategory::Scene) {
        s_gzSceneTab = (s_gzSceneTab + delta + 3) % 3;
        return;
    }
    if (category != ImGuiPracticeSaves::MainCategory::Warping) return;
    clamp_gz_warp_state(s_gzWarpState);
    switch (row) {
    case 0:
        s_gzWarpState.region = sorted_adjacent_index(static_cast<int>(gameRegions.size()), s_gzWarpState.region, delta, [](int a, int b) {
            return alphabetical_less(gameRegions[a].regionName, gameRegions[b].regionName);
        });
        s_gzWarpState.map = s_gzWarpState.room = s_gzWarpState.spawn = 0;
        break;
    case 1: {
        const auto& region = gameRegions[s_gzWarpState.region];
        s_gzWarpState.map = sorted_adjacent_index(static_cast<int>(region.maps.size()), s_gzWarpState.map, delta, [&](int a, int b) {
            return alphabetical_less(region.maps[a].mapName, region.maps[b].mapName);
        });
        s_gzWarpState.room = s_gzWarpState.spawn = 0;
        break;
    }
    case 2: {
        const auto& region = gameRegions[s_gzWarpState.region];
        const auto& map = region.maps[s_gzWarpState.map];
        s_gzWarpState.room = sorted_adjacent_index(static_cast<int>(map.mapRooms.size()), s_gzWarpState.room, delta, [&](int a, int b) {
            return map.mapRooms[a].roomNo < map.mapRooms[b].roomNo;
        });
        s_gzWarpState.spawn = 0;
        break;
    }
    case 3: {
        const auto& region = gameRegions[s_gzWarpState.region];
        const auto& map = region.maps[s_gzWarpState.map];
        const auto& room = map.mapRooms[s_gzWarpState.room];
        s_gzWarpState.spawn = sorted_adjacent_index(static_cast<int>(room.roomPoints.size()), s_gzWarpState.spawn, delta, [&](int a, int b) {
            return room.roomPoints[a] < room.roomPoints[b];
        });
        break;
    }
    case 4:
        s_gzWarpState.layer += delta;
        if (s_gzWarpState.layer > 14) s_gzWarpState.layer = -1;
        if (s_gzWarpState.layer < -1) s_gzWarpState.layer = 14;
        break;
    default: break;
    }
    clamp_gz_warp_state(s_gzWarpState);
}

bool gz_begin_row() {
    const int row = s_gzDrawRow++;
    const bool selected = s_gzPanelFocused && row == s_gzSelectedRow;
    if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.9f, 0.1f, 1.0f));
    return selected;
}

void gz_end_row(bool selected) {
    if (selected && s_gzScrollSelectedRow) ImGui::SetScrollHereY(0.5f);
    if (selected) ImGui::PopStyleColor();
}

bool gz_config_checkbox(const char* label, ConfigVar<bool>& value, bool enabled = true) {
    bool copy = value.getValue();
    const bool selected = gz_begin_row();
    if (!enabled) ImGui::BeginDisabled();
    const bool changed = ImGui::Checkbox(label, &copy);
    if (!enabled) ImGui::EndDisabled();
    gz_end_row(selected);
    if (changed) {
        value.setValue(copy);
        config::save();
        return true;
    }
    return false;
}

void gz_disabled_checkbox(const char* label) {
    bool off = false;
    const bool selected = gz_begin_row();
    ImGui::BeginDisabled();
    ImGui::Checkbox(label, &off);
    ImGui::EndDisabled();
    gz_end_row(selected);
}

void gz_disabled_button(const char* label) {
    const bool selected = gz_begin_row();
    ImGui::BeginDisabled();
    ImGui::Button(label, ImVec2(160.0f, 0.0f));
    ImGui::EndDisabled();
    gz_end_row(selected);
}

void draw_gz_cheats_panel() {
    auto& s = getSettings();
    const bool enabled = !s.game.speedrunMode;
    ImGui::BeginChild("##gz_cheats_panel", ImVec2(560.0f, 0.0f), true);
    if (!enabled) {
        ImGui::TextDisabled("Disabled while Speedrun Mode is active.");
    }
    gz_config_checkbox("disable item timer", s.game.enableIndefiniteItemDrops, enabled);
    gz_disabled_checkbox("disable walls");
    gz_disabled_checkbox("fast bonk recovery");
    gz_disabled_checkbox("fast movement");
    gz_config_checkbox("infinite air", s.game.infiniteOxygen, enabled);
    gz_config_checkbox("infinite arrows", s.game.infiniteArrows, enabled);
    gz_config_checkbox("infinite bombs", s.game.infiniteBombs, enabled);
    gz_config_checkbox("infinite hearts", s.game.infiniteHearts, enabled);
    gz_config_checkbox("infinite lantern oil", s.game.infiniteOil, enabled);
    gz_config_checkbox("infinite rupees", s.game.infiniteRupees, enabled);
    gz_config_checkbox("infinite slingshot seeds", s.game.infiniteSeeds, enabled);
    gz_disabled_checkbox("invincible link");
    gz_config_checkbox("invincible enemies", s.game.invincibleEnemies, enabled);
    gz_config_checkbox("moon jump", s.game.moonJump, enabled);
    gz_disabled_checkbox("no sinking in sand");
    gz_config_checkbox("super clawshot", s.game.superClawshot, enabled);
    gz_config_checkbox("transform anywhere", s.game.canTransformAnywhere, enabled);
    gz_disabled_checkbox("unrestricted items");
    ImGui::EndChild();
}

void draw_gz_tools_panel() {
    auto& s = getSettings();
    int& tab = s_gzToolsTab;
    const char* tabs[] = {"checkers", "displays", "link"};
    ImGui::BeginChild("##gz_tools_panel", ImVec2(560.0f, 0.0f), true);
    for (int i = 0; i < IM_ARRAYSIZE(tabs); i++) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::Selectable(tabs[i], tab == i, 0, ImVec2(ImGui::CalcTextSize(tabs[i]).x + 12.0f, 0.0f))) {
            tab = i;
        }
    }
    ImGui::Separator();

    if (tab == 0) {
        gz_disabled_checkbox("coro td");
        gz_disabled_checkbox("ebmb");
        gz_disabled_checkbox("elevator escape");
        gz_config_checkbox("gorge void", s.game.gorgeVoidChecker);
        gz_disabled_checkbox("ladder freezard cancel");
        gz_disabled_checkbox("rolls");
        gz_disabled_checkbox("universal map delay");
    } else if (tab == 1) {
        gz_disabled_checkbox("a/b mash rate");
        gz_config_checkbox("in-game timer", s.game.showSpeedrunRTATimer, s.game.speedrunMode);
        gz_config_checkbox("input viewer", s.game.showInputViewer);
        gz_disabled_checkbox("link debug info");
        gz_disabled_checkbox("load timer");
        gz_disabled_checkbox("stage info");
        gz_disabled_checkbox("timer");
    } else {
        gz_config_checkbox("free cam", s.game.freeCamera);
        gz_config_checkbox("move link", s.game.moveLink, !s.game.speedrunMode);
        gz_disabled_checkbox("teleport");
    }
    ImGui::EndChild();
}

void draw_gz_scene_panel() {
    int& tab = s_gzSceneTab;
    const char* tabs[] = {"environment", "viewers", "audio"};
    ImGui::BeginChild("##gz_scene_panel", ImVec2(560.0f, 0.0f), true);
    for (int i = 0; i < IM_ARRAYSIZE(tabs); i++) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::Selectable(tabs[i], tab == i, 0, ImVec2(ImGui::CalcTextSize(tabs[i]).x + 12.0f, 0.0f))) {
            tab = i;
        }
    }
    ImGui::Separator();

    if (tab == 0) {
        gz_disabled_checkbox("freeze time");
        gz_disabled_checkbox("freeze actors");
        gz_disabled_checkbox("freeze camera");
    } else if (tab == 1) {
        gz_disabled_checkbox("viewers");
    } else {
        gz_disabled_checkbox("mute bgm");
        gz_disabled_checkbox("mute sfx");
    }
    ImGui::EndChild();
}

void draw_gz_settings_panel() {
    ImGui::BeginChild("##gz_settings_panel", ImVec2(560.0f, 0.0f), true);
    gz_disabled_checkbox("boot to menu");
    gz_disabled_checkbox("cursor type");
    gz_disabled_checkbox("display mode");
    gz_disabled_checkbox("drop shadows");
    gz_disabled_checkbox("menu pauses game");
    gz_disabled_checkbox("menu sfx");
    gz_disabled_checkbox("reload type");
    gz_disabled_checkbox("state streaming");
    gz_disabled_checkbox("swap equips");
    gz_disabled_checkbox("theme");
    ImGui::Separator();
    gz_disabled_button("command combos");
    gz_disabled_button("menu positions");
    gz_disabled_button("start gdb server");
    gz_disabled_button("save settings");
    gz_disabled_button("load settings");
    gz_disabled_button("delete settings");
    ImGui::EndChild();
}

void draw_gz_warping_panel(bool& open) {
    auto& state = s_gzWarpState;
    clamp_gz_warp_state(state);
    ImGui::BeginChild("##gz_warp_panel", ImVec2(560.0f, 0.0f), true);

    auto combo_label = [](const char* label, const char* value) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(300.0f);
        return value;
    };

    const bool typeSelected = gz_begin_row();
    if (ImGui::BeginCombo("##gz_warp_type", combo_label("type", gameRegions[state.region].regionName))) {
        for (int i = 0; i < static_cast<int>(gameRegions.size()); i++) {
            if (ImGui::Selectable(gameRegions[i].regionName, state.region == i)) {
                state.region = i;
                state.map = state.room = state.spawn = 0;
                clamp_gz_warp_state(state);
            }
        }
        ImGui::EndCombo();
    }
    gz_end_row(typeSelected);

    const auto& region = gameRegions[state.region];
    const bool stageSelected = gz_begin_row();
    if (ImGui::BeginCombo("##gz_warp_stage", combo_label("stage", region.maps[state.map].mapName))) {
        for (int i = 0; i < static_cast<int>(region.maps.size()); i++) {
            if (ImGui::Selectable(region.maps[i].mapName, state.map == i)) {
                state.map = i;
                state.room = state.spawn = 0;
                clamp_gz_warp_state(state);
            }
        }
        ImGui::EndCombo();
    }
    gz_end_row(stageSelected);

    const auto& map = region.maps[state.map];
    const auto& room = map.mapRooms[state.room];
    const std::string roomLabel = fmt::format("{}", room.roomNo);
    const bool roomSelected = gz_begin_row();
    if (ImGui::BeginCombo("##gz_warp_room", combo_label("room", roomLabel.c_str()))) {
        for (int i = 0; i < static_cast<int>(map.mapRooms.size()); i++) {
            const std::string label = fmt::format("{}", map.mapRooms[i].roomNo);
            if (ImGui::Selectable(label.c_str(), state.room == i)) {
                state.room = i;
                state.spawn = 0;
                clamp_gz_warp_state(state);
            }
        }
        ImGui::EndCombo();
    }
    gz_end_row(roomSelected);

    const s16 spawnPoint = room.roomPoints[state.spawn];
    const std::string spawnLabel = fmt::format("{}", spawnPoint);
    const bool spawnSelected = gz_begin_row();
    if (ImGui::BeginCombo("##gz_warp_spawn", combo_label("spawn", spawnLabel.c_str()))) {
        for (int i = 0; i < static_cast<int>(room.roomPoints.size()); i++) {
            const std::string label = fmt::format("{}", room.roomPoints[i]);
            if (ImGui::Selectable(label.c_str(), state.spawn == i)) {
                state.spawn = i;
            }
        }
        ImGui::EndCombo();
    }
    gz_end_row(spawnSelected);

    const std::string layerLabel = state.layer < 0 ? std::string("default") : fmt::format("{}", state.layer);
    const bool layerSelected = gz_begin_row();
    if (ImGui::BeginCombo("##gz_warp_layer", combo_label("layer", layerLabel.c_str()))) {
        for (int layer = -1; layer <= 14; layer++) {
            const std::string label = layer < 0 ? std::string("default") : fmt::format("{}", layer);
            if (ImGui::Selectable(label.c_str(), state.layer == layer)) {
                state.layer = layer;
            }
        }
        ImGui::EndCombo();
    }
    gz_end_row(layerSelected);

    const bool warpSelected = gz_begin_row();
    if (!dusk::IsGameLaunched) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("warp", ImVec2(160.0f, 0.0f))) {
        dComIfGp_setNextStage(map.mapFile, spawnPoint, room.roomNo, state.layer);
        open = false;
    }
    if (!dusk::IsGameLaunched) {
        ImGui::EndDisabled();
    }
    gz_end_row(warpSelected);
    ImGui::EndChild();
}
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
            const u8* entryData = data.data() + kMetadataHeaderSize + (static_cast<size_t>(i) * kMetadataEntrySize);
            PracticeSaveEntry save;
            save.name = read_fixed_string(entryData + kNameOffset, kNameSize);
            save.description = read_fixed_string(entryData + kDescriptionOffset, kDescriptionSize);
            save.filename = read_fixed_string(entryData + kFilenameOffset, kFilenameSize);
            save.index = static_cast<int>(i);
            const u8 setFlags = entryData[kPlacementOffset];
            if ((setFlags & 1) != 0) {
                const u8* placement = entryData + kPlacementOffset;
                const s16 angle = read_be16(placement + 2);
                save.placement = PracticeSavePlacement{
                    cXyz(read_be_float(placement + 4),
                         read_be_float(placement + 8),
                         read_be_float(placement + 12)),
                    angle,
                };
            }
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
    return loadPracticeSave(m_saveCategory, entry);
}

bool ImGuiPracticeSaves::loadPracticeSave(SaveCategory category, const PracticeSaveEntry& entry) {
    try {
        const auto data = io::FileStream::ReadAllBytes(save_path(category, entry.filename));
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

        const u8 vibration = dComIfGs_getOptVibration();
        save.getPlayer().getConfig().setVibration(vibration);

        // Install the save before the stage request so room actors spawn against the practice state.
        g_dComIfG_gameInfo.info.mSavedata = save;
        dComIfGs_getSave(g_dComIfG_gameInfo.info.getDan().mStageNo);
        m_pendingVibration = vibration;
        m_pendingPlacement = entry.placement;
        const PracticeSaveCallbacks callbacks = practice_save_callbacks(category, entry.index);
        m_pendingStageInitCallback = static_cast<int>(callbacks.stageInit);
        m_pendingPlayerInitCallback = static_cast<int>(callbacks.playerInit);
        m_pendingPlacementFrames = 0;
        m_pendingPlayerInitFrames = 0;
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

        if (callbacks.stageInit != PracticeSaveCallback::None) {
            apply_player_init_callback(callbacks.stageInit);
            dComIfGs_putSave(g_dComIfG_gameInfo.info.getDan().mStageNo);
        }

        m_pendingSavedata = g_dComIfG_gameInfo.info.mSavedata;

        // Force mDan.mStageNo = -1 so dComIfGs_initDan() during scene load clears
        // dungeon switch bits when reloading the same stage. Stallord saves must
        // retain zone switch 5 while the arena actors are being created.
        if (callbacks.stageInit != PracticeSaveCallback::StallordInit) {
            dComIfGs_resetDan();
        }

        m_statusMsg = fmt::format("Loading {}.", entry.name);
        return true;
    } catch (const std::exception& e) {
        m_statusMsg = fmt::format("Failed to load {}: {}", entry.name, e.what());
        return false;
    }
}

bool ImGuiPracticeSaves::loadPracticeSaveByIndex(SaveCategory category, int index, bool checkerSetup) {
    if (!m_loaded) {
        loadMetadata();
    }

    const auto& saves = m_saves[category_index(category)];
    const auto it = std::find_if(saves.begin(), saves.end(), [index](const PracticeSaveEntry& save) {
        return save.index == index;
    });
    if (it == saves.end()) {
        m_statusMsg = "Gorge Void practice save was not found.";
        return false;
    }

    if (!loadPracticeSave(category, *it)) {
        return false;
    }

    if (checkerSetup && category == SaveCategory::Any && index == 9) {
        m_pendingPlayerInitCallback = static_cast<int>(PracticeSaveCallback::GorgeVoidPostLoad);
    }

    return true;
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

    if (m_focusSaveList && m_mainCategory != MainCategory::Practice) {
        const int count = gz_generic_row_count(m_mainCategory);
        if (count > 0) {
            if (accept(PAD_BUTTON_UP, 0.18)) {
                m_selectedGenericRow = std::max(0, m_selectedGenericRow - 1);
                m_scrollSelectedGenericRow = true;
                return;
            }
            if (accept(PAD_BUTTON_DOWN, 0.18)) {
                m_selectedGenericRow = std::min(count - 1, m_selectedGenericRow + 1);
                m_scrollSelectedGenericRow = true;
                return;
            }
            if (acceptPress(PAD_TRIGGER_L)) {
                gz_adjust_generic_row(m_mainCategory, m_selectedGenericRow, -1);
                m_selectedGenericRow = std::min(m_selectedGenericRow, std::max(0, gz_generic_row_count(m_mainCategory) - 1));
                m_scrollSelectedGenericRow = true;
                return;
            }
            if (acceptPress(PAD_TRIGGER_R)) {
                gz_adjust_generic_row(m_mainCategory, m_selectedGenericRow, 1);
                m_selectedGenericRow = std::min(m_selectedGenericRow, std::max(0, gz_generic_row_count(m_mainCategory) - 1));
                m_scrollSelectedGenericRow = true;
                return;
            }
            if (accept(PAD_BUTTON_LEFT)) {
                gz_adjust_generic_row(m_mainCategory, m_selectedGenericRow, -1);
                return;
            }
            if (accept(PAD_BUTTON_RIGHT)) {
                gz_adjust_generic_row(m_mainCategory, m_selectedGenericRow, 1);
                return;
            }
            if (accept(PAD_BUTTON_A, 0.20)) {
                if (gz_activate_generic_row(m_mainCategory, m_selectedGenericRow)) {
                    open = false;
                }
                return;
            }
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
            const int next = (category_index(m_saveCategory) + 1) % static_cast<int>(SaveCategory::Count);
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
                if (loadPracticeSave(currentSaves()[m_selectedSave])) {
                    open = false;
                }
                return;
            }
        }
        return;
    }

    if (accept(PAD_BUTTON_A, 0.16)) {
        m_focusSaveList = true;
        m_selectedGenericRow = std::min(m_selectedGenericRow, std::max(0, gz_generic_row_count(m_mainCategory) - 1));
        m_scrollSelectedGenericRow = true;
        return;
    }

    int category = main_category_index(m_mainCategory);
    if (accept(PAD_BUTTON_UP, 0.18)) {
        category = std::max(0, category - 1);
        m_mainCategory = static_cast<MainCategory>(category);
        m_selectedGenericRow = 0;
        m_scrollSelectedGenericRow = true;
        return;
    }
    if (accept(PAD_BUTTON_DOWN, 0.18)) {
        category = std::min(static_cast<int>(MainCategory::Count) - 1, category + 1);
        m_mainCategory = static_cast<MainCategory>(category);
        m_selectedGenericRow = 0;
        m_scrollSelectedGenericRow = true;
        return;
    }
    if (m_mainCategory == MainCategory::Practice && accept(PAD_BUTTON_A, 0.16)) {
        m_focusSaveList = true;
    }
}

// gz-style nested navigation for the native menu: main category list ->
// (optional) sub-category list -> rows/saves, with B walking back up one level
// at a time. No tabs / no L/R switching.
void ImGuiPracticeSaves::handleControllerNative(bool& open) {
    const u32 hold = raw_pad_hold() | raw_pad_trig();
    const double now = ImGui::GetTime();
    if (now < m_nextControllerInputTime) {
        return;
    }
    auto accept = [&](u32 button, double cooldown = 0.16) {
        if ((hold & button) == 0) {
            return false;
        }
        m_nextControllerInputTime = now + cooldown;
        return true;
    };

    const bool hasSub = category_has_sublist(m_mainCategory);

    // Sub-category list accessors (Practice -> save category, Tools/Scene -> tab).
    auto subCount = [&]() -> int {
        if (m_mainCategory == MainCategory::Practice) {
            return static_cast<int>(SaveCategory::Count);
        }
        return 3;  // tools / scene
    };
    auto subIndex = [&]() -> int {
        if (m_mainCategory == MainCategory::Practice) {
            return category_index(m_saveCategory);
        }
        return m_mainCategory == MainCategory::Tools ? s_gzToolsTab : s_gzSceneTab;
    };
    auto setSubIndex = [&](int v) {
        if (m_mainCategory == MainCategory::Practice) {
            m_saveCategory = static_cast<SaveCategory>(v);
            m_selectedSave = 0;
            m_scrollSelectedSave = true;
        } else if (m_mainCategory == MainCategory::Tools) {
            s_gzToolsTab = v;
        } else {
            s_gzSceneTab = v;
        }
    };

    // B: back out one level.
    if (accept(PAD_BUTTON_B, 0.18)) {
        if (!m_focusSaveList) {
            open = false;
        } else if (hasSub && m_inSubList) {
            m_inSubList = false;
        } else {
            m_focusSaveList = false;
        }
        return;
    }

    // Level 0: main category list.
    if (!m_focusSaveList) {
        const int cat = main_category_index(m_mainCategory);
        if (accept(PAD_BUTTON_UP, 0.18)) {
            m_mainCategory = static_cast<MainCategory>(std::max(0, cat - 1));
            return;
        }
        if (accept(PAD_BUTTON_DOWN, 0.18)) {
            m_mainCategory = static_cast<MainCategory>(
                std::min(static_cast<int>(MainCategory::Count) - 1, cat + 1));
            return;
        }
        if (accept(PAD_BUTTON_A)) {
            m_focusSaveList = true;
            m_inSubList = !category_has_sublist(m_mainCategory);
            m_selectedSave = 0;
            m_selectedGenericRow = 0;
            m_scrollSelectedSave = true;
            m_scrollSelectedGenericRow = true;
        }
        return;
    }

    // Level 1: sub-category list (Practice/Tools/Scene).
    if (hasSub && !m_inSubList) {
        const int count = subCount();
        const int idx = subIndex();
        if (accept(PAD_BUTTON_UP, 0.18)) {
            setSubIndex(std::max(0, idx - 1));
            return;
        }
        if (accept(PAD_BUTTON_DOWN, 0.18)) {
            setSubIndex(std::min(count - 1, idx + 1));
            return;
        }
        if (accept(PAD_BUTTON_A)) {
            m_inSubList = true;
            m_selectedSave = 0;
            m_selectedGenericRow = 0;
            m_scrollSelectedSave = true;
            m_scrollSelectedGenericRow = true;
        }
        return;
    }

    // Level 2: saves (Practice) or rows (everything else).
    if (m_mainCategory == MainCategory::Practice) {
        const int count = static_cast<int>(currentSaves().size());
        if (count > 0) {
            if (accept(PAD_BUTTON_UP)) {
                m_selectedSave = std::max(0, m_selectedSave - 1);
                m_scrollSelectedSave = true;
                return;
            }
            if (accept(PAD_BUTTON_DOWN)) {
                m_selectedSave = std::min(count - 1, m_selectedSave + 1);
                m_scrollSelectedSave = true;
                return;
            }
            if (accept(PAD_BUTTON_LEFT)) {
                m_selectedSave = std::max(0, m_selectedSave - 10);
                m_scrollSelectedSave = true;
                return;
            }
            if (accept(PAD_BUTTON_RIGHT)) {
                m_selectedSave = std::min(count - 1, m_selectedSave + 10);
                m_scrollSelectedSave = true;
                return;
            }
            if (accept(PAD_BUTTON_A, 0.22) && !m_loadInProgress &&
                !getTransientSettings().stateShareLoadActive) {
                if (loadPracticeSave(currentSaves()[m_selectedSave])) {
                    open = false;
                }
                return;
            }
        }
        return;
    }

    const int count = gz_generic_row_count(m_mainCategory);
    if (count > 0) {
        m_selectedGenericRow = std::clamp(m_selectedGenericRow, 0, count - 1);
        if (accept(PAD_BUTTON_UP, 0.18)) {
            m_selectedGenericRow = std::max(0, m_selectedGenericRow - 1);
            return;
        }
        if (accept(PAD_BUTTON_DOWN, 0.18)) {
            m_selectedGenericRow = std::min(count - 1, m_selectedGenericRow + 1);
            return;
        }
        // Combo categories (Warping) adjust the value with left/right; other
        // categories have no per-row adjust (their sub-lists are level 1).
        if (m_mainCategory == MainCategory::Warping) {
            if (accept(PAD_BUTTON_LEFT)) {
                gz_adjust_generic_row(m_mainCategory, m_selectedGenericRow, -1);
                return;
            }
            if (accept(PAD_BUTTON_RIGHT)) {
                gz_adjust_generic_row(m_mainCategory, m_selectedGenericRow, 1);
                return;
            }
        }
        if (accept(PAD_BUTTON_A, 0.20)) {
            if (m_mainCategory == MainCategory::Tools && s_gzToolsTab == 1) {
                if (m_selectedGenericRow == 2) {
                    gz_set_bool(getSettings().game.nativeInputViewer, !getSettings().game.speedrunMode);
                    return;
                }
                if (m_selectedGenericRow == 3) {
                    gz_set_bool(getSettings().game.nativeLinkDebugInfo, !getSettings().game.speedrunMode);
                    return;
                }
            }
            if (gz_activate_generic_row(m_mainCategory, m_selectedGenericRow)) {
                open = false;
            }
            return;
        }
    }
}

void ImGuiPracticeSaves::drawCategoryList() {
    ImGui::BeginChild("##gz_main_categories", ImVec2(170.0f, 0.0f), false);
    for (int i = 0; i < static_cast<int>(MainCategory::Count); i++) {
        const bool selected = i == main_category_index(m_mainCategory);
        const bool entered = selected && m_focusSaveList;
        const ImVec4 selectedText = ImVec4(0.1f, 0.9f, 0.1f, 1.0f);
        const ImVec4 normalText = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, entered ? selectedText : normalText);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        if (ImGui::Selectable(kMainCategoryNames[i], selected, 0, ImVec2(150.0f, 0.0f))) {
            m_mainCategory = static_cast<MainCategory>(i);
            m_focusSaveList = true;
            m_selectedGenericRow = 0;
            m_scrollSelectedGenericRow = true;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::EndChild();
}

void ImGuiPracticeSaves::drawPracticePanel(bool& open) {
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
        if (!canLoad) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Selectable(save.name.c_str(), selected, 0, ImVec2(0.0f, 0.0f))) {
            m_selectedSave = i;
            m_focusSaveList = true;
            if (canLoad && loadPracticeSave(save)) {
                open = false;
            }
        }
        if (!canLoad) {
            ImGui::EndDisabled();
        }
        if (!save.description.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", save.description.c_str());
        }
        if (selected && m_scrollSelectedSave) {
            ImGui::SetScrollHereY(0.5f);
            m_scrollSelectedSave = false;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void ImGuiPracticeSaves::drawGenericPanel(bool& open) {
    s_gzDrawRow = 0;
    s_gzSelectedRow = m_selectedGenericRow;
    s_gzPanelFocused = m_focusSaveList;
    s_gzScrollSelectedRow = m_scrollSelectedGenericRow;
    switch (m_mainCategory) {
    case MainCategory::Cheats:
        draw_gz_cheats_panel();
        break;
    case MainCategory::Scene:
        draw_gz_scene_panel();
        break;
    case MainCategory::Settings:
        draw_gz_settings_panel();
        break;
    case MainCategory::Tools:
        draw_gz_tools_panel();
        break;
    case MainCategory::Warping:
        draw_gz_warping_panel(open);
        break;
    default: {
        const char* const* rows = nullptr;
        int rowCount = 0;
        switch (m_mainCategory) {
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
        default:
            break;
        }
        ImGui::BeginChild("##gz_placeholder_panel", ImVec2(560.0f, 0.0f), true);
        for (int i = 0; i < rowCount; i++) {
            gz_disabled_checkbox(rows[i]);
        }
        ImGui::EndChild();
        break;
    }
    }
    m_scrollSelectedGenericRow = false;
}

void ImGuiPracticeSaves::executeGorgeVoidChecker() {
    constexpr u32 kGorgeVoidReloadCombo = PAD_TRIGGER_L | PAD_TRIGGER_Z;
    constexpr int kWarpCutsceneFrames = 160;
    constexpr int kEarliestRelevantFrame = 123;
    constexpr int kLatestRelevantLateFrame = 10;
    constexpr int kResultDisplayFrames = 90;

    auto& state = m_gorgeVoidChecker;
    if (state.resultTimer > 0) {
        state.resultTimer--;
    }

    const bool enabled = getSettings().game.gorgeVoidChecker.getValue() &&
                         !getSettings().game.speedrunMode.getValue();
    if (!enabled) {
        state.timerStarted = false;
        state.comboHeld = false;
        state.gotIt = false;
        state.counterDifference = 0;
        state.afterCsVal = 0;
        state.resultTimer = 0;
        state.resultText[0] = '\0';
        return;
    }

    const u32 hold = game_pad_hold();
    const bool comboHeld = (hold & kGorgeVoidReloadCombo) == kGorgeVoidReloadCombo;
    if (comboHeld && !state.comboHeld &&
        !m_loadInProgress && !getTransientSettings().stateShareLoadActive)
    {
        loadPracticeSaveByIndex(SaveCategory::Any, 9, true);
        state.timerStarted = false;
        state.gotIt = false;
        state.counterDifference = 0;
        state.afterCsVal = 0;
        state.resultTimer = 0;
    }
    state.comboHeld = comboHeld;

    const char* stageName = dComIfGp_getStartStageName();
    if (stageName == nullptr || std::strcmp(stageName, "F_SP121") != 0 ||
        m_loadInProgress || getTransientSettings().stateShareLoadActive)
    {
        state.timerStarted = false;
        state.gotIt = false;
        state.counterDifference = 0;
        state.afterCsVal = 0;
        return;
    }

    dEvt_control_c* event = dComIfGp_getEvent();
    if (event == nullptr) {
        return;
    }

    const int frame = static_cast<int>(g_Counter.mCounter0);
    const bool halt = event->mEventStatus == 1;
    if (!state.timerStarted && halt && event->mEventId == 262) {
        state.timerStarted = true;
        state.previousFrame = frame;
        state.counterDifference = 0;
        state.afterCsVal = 0;
        state.gotIt = false;
    }

    if (!state.timerStarted) {
        return;
    }

    state.counterDifference += frame - state.previousFrame;
    state.previousFrame = frame;
    if (state.counterDifference > kWarpCutsceneFrames) {
        state.afterCsVal = state.counterDifference - kWarpCutsceneFrames;
    }

    const int perfectFrame = kWarpCutsceneFrames + (frame_interp::is_enabled() ? 0 : 1);
    if (state.counterDifference <= kEarliestRelevantFrame ||
        state.counterDifference - perfectFrame >= kLatestRelevantLateFrame)
    {
        return;
    }

    const bool inputDetected = (game_pad_hold() & PAD_TRIGGER_L) != 0 &&
                               (game_pad_trig() & PAD_BUTTON_A) != 0;
    if (state.gotIt || !inputDetected) {
        return;
    }

    if (state.counterDifference < perfectFrame) {
        std::snprintf(state.resultText, sizeof(state.resultText), "-%df",
                      perfectFrame - state.counterDifference);
        state.resultColor = 1;
    } else if (state.counterDifference == perfectFrame) {
        std::snprintf(state.resultText, sizeof(state.resultText), "<3");
        state.resultColor = 2;
        state.gotIt = true;
    } else {
        std::snprintf(state.resultText, sizeof(state.resultText), "+%df",
                      state.counterDifference - perfectFrame);
        state.resultColor = 3;
    }
    state.resultTimer = kResultDisplayFrames;
}

void ImGuiPracticeSaves::drawGorgeVoidCheckerResult() {
    const auto& state = m_gorgeVoidChecker;
    if (state.resultTimer <= 0 || state.resultText[0] == '\0') {
        return;
    }

    ImU32 color = IM_COL32(255, 255, 255, 255);
    if (state.resultColor == 1) {
        color = IM_COL32(70, 145, 255, 255);
    } else if (state.resultColor == 2) {
        color = IM_COL32(60, 230, 90, 255);
    } else if (state.resultColor == 3) {
        color = IM_COL32(255, 80, 80, 255);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float fontSize = ImGui::GetFontSize() * 2.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(state.resultText);
    const ImVec2 pos(viewport->Pos.x + (viewport->Size.x - textSize.x * 2.0f) * 0.5f,
                     viewport->Pos.y + 96.0f);
    ImGui::GetForegroundDrawList()->AddText(nullptr, fontSize, pos, color, state.resultText);
}

void ImGuiPracticeSaves::tickPendingApply() {
    if (m_pendingSavedata.has_value() && !dComIfGp_isEnableNextStage()) {
        g_dComIfG_gameInfo.info.mSavedata = *m_pendingSavedata;
        if (m_pendingVibration.has_value()) {
            dComIfGs_setOptVibration(*m_pendingVibration);
            dComIfGp_setNowVibration(*m_pendingVibration);
            m_pendingVibration.reset();
        }
        m_pendingSavedata.reset();

        dComIfGs_getSave(g_dComIfG_gameInfo.info.getDan().mStageNo);
        if (auto* alink = daAlink_getAlinkActorClass()) {
            alink->duskCancelHeldItemForPracticeSave();
        }
        if (m_pendingStageInitCallback != static_cast<int>(PracticeSaveCallback::None)) {
            apply_post_save_inject_callback(static_cast<PracticeSaveCallback>(m_pendingStageInitCallback));
            m_pendingStageInitCallback = static_cast<int>(PracticeSaveCallback::None);
        }
        dKy_set_nexttime(dComIfGs_getTime());
        dComIfGp_offOxygenShowFlag();
        dComIfGp_setMaxOxygen(600);
        dComIfGp_setOxygen(600);
    }

    auto applyPendingPlacement = [this]() {
        if (dComIfGp_isEnableNextStage()) {
            return;
        }

        if (m_pendingPlacementFrames > 0) {
            if (m_pendingPlacement.has_value()) {
                if (auto* player = daPy_getPlayerActorClass()) {
                    player->setPlayerPosAndAngle(&m_pendingPlacement->pos, m_pendingPlacement->angle, 0);
                }
            }

            m_pendingPlacementFrames--;
            if (m_pendingPlacementFrames == 0) {
                m_pendingPlacement.reset();
            }
        }

        if (m_pendingPlayerInitFrames > 0) {
            apply_player_init_callback(static_cast<PracticeSaveCallback>(m_pendingPlayerInitCallback));
            m_pendingPlayerInitFrames--;
            if (m_pendingPlayerInitFrames == 0) {
                m_pendingPlayerInitCallback = static_cast<int>(PracticeSaveCallback::None);
            }
        }
    };
    if (!m_loadInProgress) {
        applyPendingPlacement();
        return;
    }

    if (fopOvlpM_IsPeek()) {
        m_loadPeekSeen = true;
    } else if (m_loadPeekSeen ||
               (getSettings().game.enableInstaLoads.getValue() &&
                !dComIfGp_isEnableNextStage() &&
                daPy_getPlayerActorClass() != nullptr))
    {
        m_loadInProgress = false;
        m_loadPeekSeen = false;
        m_pendingPlacementFrames = m_pendingPlacement.has_value() ? 20 : 0;
        m_pendingPlayerInitFrames = (m_pendingPlayerInitCallback != static_cast<int>(PracticeSaveCallback::None)) ? 60 : 0;
        getTransientSettings().stateShareLoadActive = false;
    }

    applyPendingPlacement();
}

void ImGuiPracticeSaves::draw(bool& open) {
    if (dusk::IsGameLaunched) {
        tickPendingApply();
    }

    if (!m_loaded) {
        loadMetadata();
    }

    if (dusk::IsGameLaunched) {
        executeGorgeVoidChecker();
    }
    drawGorgeVoidCheckerResult();

    if (!open) {
        return;
    }

    const bool nativeMode = getSettings().game.nativePracticeMenu;
    if (nativeMode) {
        handleControllerNative(open);
    } else {
        handleController(open);
    }
    m_lastControllerHold = raw_pad_hold() | raw_pad_trig();
    consumeControllerInput();
    if (!open) {
        return;
    }

    // In native mode the menu is drawn (resolution-independently) from the
    // game's 2D pass by drawNative(); the input/state handling above still runs
    // here every frame. In imgui mode we fall through and render the imgui
    // window, which is mouse-capable. Both share the same state and load logic.
    if (nativeMode) {
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
        drawPracticePanel(open);
    } else {
        drawGenericPanel(open);
    }

    if (!m_statusMsg.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_statusMsg.c_str());
    }

    ImGui::End();
}

namespace {

// Draws one line of menu text with a black drop shadow, matching the tpgz look.
// All positions are in the game's virtual 2D space (608x448), so output scales
// with resolution. setString stores the string raw (it does NOT printf-format;
// J2DTextBox::draw re-prints it with "%s", which safely handles any '%' in the
// text), so pass the literal string here.
void draw_native_text(JUTFont* font, float x, float y, float size,
                      const JUtility::TColor& color, const char* str) {
    if (font == nullptr) {
        return;
    }

    J2DTextBox tb;
    tb.setFont(font);
    tb.setFontSize(size, size);
    tb.setString(str);

    const JUtility::TColor shadow(0, 0, 0, color.a);
    tb.setFontColor(shadow, shadow);
    tb.draw(x + 2.0f, y + 2.0f);

    tb.setFontColor(color, color);
    tb.draw(x, y);
}

void draw_native_text_centered(JUTFont* font, float cx, float y, float size,
                               const JUtility::TColor& color, const char* str) {
    const float width = static_cast<float>(std::strlen(str)) * size * 0.55f;
    draw_native_text(font, cx - width * 0.5f, y, size, color, str);
}

void draw_native_text_plain(JUTFont* font, float x, float y, float size,
                            const JUtility::TColor& color, const char* str) {
    if (font == nullptr) {
        return;
    }

    J2DTextBox tb;
    tb.setFont(font);
    tb.setFontSize(size, size);
    tb.setString(str);
    tb.setFontColor(color, color);
    tb.draw(x, y);
}

void draw_native_text_centered_plain(JUTFont* font, float cx, float y, float size,
                                     const JUtility::TColor& color, const char* str) {
    const float width = static_cast<float>(std::strlen(str)) * size * 0.55f;
    draw_native_text_plain(font, cx - width * 0.5f, y, size, color, str);
}

GXColor gx_color(const JUtility::TColor& color) {
    return {color.r, color.g, color.b, color.a};
}

void setup_native_shape_gx() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE,
                  GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetZCompLoc(GX_ENABLE);
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_DISABLE);
    GXLoadPosMtxImm(cMtx_getIdentity(), 0);
    GXSetCurrentMtx(0);
}

void set_shape_color(const JUtility::TColor& color) {
    GXSetTevColor(GX_TEVREG0, gx_color(color));
}

void fill_rect(float x, float y, float w, float h, const JUtility::TColor& color) {
    set_shape_color(color);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition2f32(x, y);
    GXPosition2f32(x + w, y);
    GXPosition2f32(x + w, y + h);
    GXPosition2f32(x, y + h);
    GXEnd();
}

void outline_rect_line(float x, float y, float w, float h, const JUtility::TColor& color) {
    set_shape_color(color);
    GXSetLineWidth(0x10, GX_TO_ZERO);
    GXBegin(GX_LINESTRIP, GX_VTXFMT0, 5);
    GXPosition2f32(x, y);
    GXPosition2f32(x + w, y);
    GXPosition2f32(x + w, y + h);
    GXPosition2f32(x, y + h);
    GXPosition2f32(x, y);
    GXEnd();
}

void outline_rect(float x, float y, float w, float h, float t, const JUtility::TColor& color) {
    fill_rect(x, y, w, t, color);
    fill_rect(x, y + h - t, w, t, color);
    fill_rect(x, y, t, h, color);
    fill_rect(x + w - t, y, t, h, color);
}

void fill_segment(float x0, float y0, float x1, float y1, float t, const JUtility::TColor& color) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.01f) {
        return;
    }

    const float nx = -dy / len * t * 0.5f;
    const float ny = dx / len * t * 0.5f;
    set_shape_color(color);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition2f32(x0 + nx, y0 + ny);
    GXPosition2f32(x1 + nx, y1 + ny);
    GXPosition2f32(x1 - nx, y1 - ny);
    GXPosition2f32(x0 - nx, y0 - ny);
    GXEnd();
}

void fill_poly(const std::array<std::pair<float, float>, 16>& points, int count,
               const JUtility::TColor& color) {
    set_shape_color(color);
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, count);
    for (int i = 0; i < count; i++) {
        GXPosition2f32(points[i].first, points[i].second);
    }
    GXEnd();
}

void make_regular_poly(std::array<std::pair<float, float>, 16>& points, int count,
                       float cx, float cy, float radius, float rotation) {
    constexpr float kPi = 3.14159265358979323846f;
    for (int i = 0; i < count; i++) {
        const float a = rotation + kPi * 2.0f * static_cast<float>(i) / static_cast<float>(count);
        points[i] = {cx + std::cos(a) * radius, cy + std::sin(a) * radius};
    }
}

void outline_poly(const std::array<std::pair<float, float>, 16>& points, int count, float t,
                  const JUtility::TColor& color) {
    for (int i = 0; i < count; i++) {
        const auto& a = points[i];
        const auto& b = points[(i + 1) % count];
        fill_segment(a.first, a.second, b.first, b.second, t, color);
    }
}

void outline_poly_line(const std::array<std::pair<float, float>, 16>& points, int count,
                       const JUtility::TColor& color) {
    set_shape_color(color);
    GXSetLineWidth(0x10, GX_TO_ZERO);
    GXBegin(GX_LINESTRIP, GX_VTXFMT0, count + 1);
    for (int i = 0; i < count; i++) {
        GXPosition2f32(points[i].first, points[i].second);
    }
    GXPosition2f32(points[0].first, points[0].second);
    GXEnd();
}

void fill_regular_poly(float cx, float cy, float radius, int count,
                       const JUtility::TColor& color) {
    std::array<std::pair<float, float>, 16> points{};
    make_regular_poly(points, count, cx, cy, radius, 0.0f);
    fill_poly(points, count, color);
}

void outline_regular_poly(float cx, float cy, float radius, int count, float t,
                          const JUtility::TColor& color, float rotation = 0.0f) {
    std::array<std::pair<float, float>, 16> points{};
    make_regular_poly(points, count, cx, cy, radius, rotation);
    outline_poly(points, count, t, color);
}

void outline_regular_poly_line(float cx, float cy, float radius, int count,
                               const JUtility::TColor& color, float rotation = 0.0f) {
    std::array<std::pair<float, float>, 16> points{};
    make_regular_poly(points, count, cx, cy, radius, rotation);
    outline_poly_line(points, count, color);
}

void draw_ellipse(float cx, float cy, float w, float h, const JUtility::TColor& color) {
    constexpr int kPoints = 16;
    constexpr float kPi = 3.14159265358979323846f;
    set_shape_color(color);
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, kPoints);
    for (int i = 0; i < kPoints; i++) {
        const float a = kPi * 2.0f * static_cast<float>(i) / static_cast<float>(kPoints);
        GXPosition2f32(cx + std::cos(a) * w * 0.5f, cy + std::sin(a) * h * 0.5f);
    }
    GXEnd();
}

void draw_trigger(float x, float y, float w, float h, float value, bool digital, bool fillFromRight,
                  const JUtility::TColor& outline, const JUtility::TColor& active) {
    outline_rect_line(x, y, w, h, outline);
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    if (clamped > 0.01f) {
        const float fillW = w * clamped;
        fill_rect(fillFromRight ? x + w - fillW : x, y, fillW, h, digital ? active : outline);
    }
}

void draw_button_box(JUTFont* font, float x, float y, float w, float h, const char* label,
                     const JUtility::TColor& color, bool filled, float charSize) {
    setup_native_shape_gx();
    outline_rect_line(x, y, w, h, color);
    if (filled) {
        fill_rect(x, y, w, h, color);
    }
    const JUtility::TColor pressedText(0, 0, 0, 0x60);
    draw_native_text_centered_plain(font, x + w * 0.5f, y + (h + charSize * 0.5f) * 0.5f,
                                    charSize, filled ? pressedText : color, label);
}

void draw_dpad_button(JUTFont* font, float x, float y, float w, float h, const char* label,
                      bool held, const JUtility::TColor& color, float charSize) {
    draw_button_box(font, x, y, w, h, label, color, held, charSize);
}

void draw_dpad(JUTFont* font, float x, float y, float size, const JUtility::TColor& white,
               u32 hold) {
    const JUtility::TColor kMarker(80, 80, 80, 0xB0);
    const JUtility::TColor kHeldMarker(0, 0, 0, 0x60);
    const float branchW = 3.0f * size / 11.0f;
    const float branchL = 4.0f * size / 11.0f;
    const float charSize = 8.0f * size / 25.0f;
    const float vCharSize = 4.0f * size / 25.0f;
    const bool left = (hold & PAD_BUTTON_LEFT) != 0;
    const bool up = (hold & PAD_BUTTON_UP) != 0;
    const bool right = (hold & PAD_BUTTON_RIGHT) != 0;
    const bool down = (hold & PAD_BUTTON_DOWN) != 0;

    draw_dpad_button(font, x, y + branchL, branchL, branchW, "", left, white, charSize);
    draw_dpad_button(font, x + branchL, y, branchW, branchL, "", up, white, vCharSize);
    draw_dpad_button(font, x + branchL + branchW, y + branchL, branchL, branchW, "", right, white,
                     charSize);
    draw_dpad_button(font, x + branchL, y + branchL + branchW, branchW, branchL, "", down, white,
                     vCharSize);

    const float hMarkerW = branchL * 0.42f;
    const float hMarkerH = 1.6f;
    const float vMarkerW = 1.6f;
    const float vMarkerH = branchL * 0.42f;
    setup_native_shape_gx();
    fill_rect(x + (branchL - hMarkerW) * 0.5f, y + branchL + (branchW - hMarkerH) * 0.5f,
              hMarkerW, hMarkerH, left ? kHeldMarker : kMarker);
    fill_rect(x + branchL + (branchW - vMarkerW) * 0.5f, y + (branchL - vMarkerH) * 0.5f,
              vMarkerW, vMarkerH, up ? kHeldMarker : kMarker);
    fill_rect(x + branchL + branchW + (branchL - hMarkerW) * 0.5f,
              y + branchL + (branchW - hMarkerH) * 0.5f, hMarkerW, hMarkerH,
              right ? kHeldMarker : kMarker);
    fill_rect(x + branchL + (branchW - vMarkerW) * 0.5f,
              y + branchL + branchW + (branchL - vMarkerH) * 0.5f, vMarkerW, vMarkerH,
              down ? kHeldMarker : kMarker);
}

struct NativePadDisplayState {
    u32 hold = 0;
    float stickX = 0.0f;
    float stickY = 0.0f;
    float cStickX = 0.0f;
    float cStickY = 0.0f;
    float analogL = 0.0f;
    float analogR = 0.0f;
    int mainRawX = 0;
    int mainRawY = 0;
    int subRawX = 0;
    int subRawY = 0;
};

NativePadDisplayState read_native_pad_display_state() {
    NativePadDisplayState state;
    if (JUTGamePad* pad = mDoCPd_c::getGamePad(PAD_1)) {
        state.hold = pad->getButton();
        state.stickX = pad->getMainStickX();
        state.stickY = pad->getMainStickY();
        state.cStickX = pad->getSubStickX();
        state.cStickY = pad->getSubStickY();
        state.analogL = std::min(1.0f, static_cast<float>(pad->getAnalogL()) * (1.0f / 140.0f));
        state.analogR = std::min(1.0f, static_cast<float>(pad->getAnalogR()) * (1.0f / 140.0f));
        state.mainRawX = pad->mMainStick.mRawX;
        state.mainRawY = pad->mMainStick.mRawY;
        state.subRawX = pad->mSubStick.mRawX;
        state.subRawY = pad->mSubStick.mRawY;
        return state;
    }

    state.hold = mDoCPd_c::getHold(PAD_1);
    state.stickX = mDoCPd_c::getStickX(PAD_1);
    state.stickY = mDoCPd_c::getStickY(PAD_1);
    state.cStickX = mDoCPd_c::getSubStickX(PAD_1);
    state.cStickY = mDoCPd_c::getSubStickY(PAD_1);
    state.analogL = mDoCPd_c::getAnalogL(PAD_1);
    state.analogR = mDoCPd_c::getAnalogR(PAD_1);
    return state;
}

void draw_native_input_display(JUTFont* font) {
    const JUtility::TColor kWhite(0xFF, 0xFF, 0xFF, 0xFF);
    const JUtility::TColor kYellow(0xFF, 0xD1, 0x38, 0xFF);
    const JUtility::TColor kGreen(0, 230, 120, 0xFF);
    const JUtility::TColor kRed(0xFF, 0, 0, 0xFF);
    const JUtility::TColor kPurple(0x8A, 0x2B, 0xE2, 0xFF);
    const NativePadDisplayState pad = read_native_pad_display_state();

    constexpr float x = 220.0f;
    constexpr float y = 376.0f;
    constexpr float scale = 1.0f;
    constexpr float stickGateRadius = 17.5f * scale;
    constexpr float stickDotSize = 20.0f * scale;
    constexpr float stickMove = 10.0f * scale;

    setup_native_shape_gx();
    draw_trigger(x, y, 35.0f * scale, 7.0f * scale, pad.analogL,
                 (pad.hold & PAD_TRIGGER_L) != 0, false, kWhite, kGreen);
    draw_trigger(x + 45.0f * scale, y, 35.0f * scale, 7.0f * scale, pad.analogR,
                 (pad.hold & PAD_TRIGGER_R) != 0, true, kWhite, kGreen);

    const float mainCx = x + 17.5f * scale;
    const float mainCy = y + 30.0f * scale;
    const float cCx = x + 62.5f * scale;
    const float cCy = y + 30.0f * scale;
    outline_regular_poly_line(mainCx, mainCy, stickGateRadius, 8, kWhite, -1.57079632679f);
    outline_regular_poly_line(cCx, cCy, stickGateRadius, 8, kYellow, -1.57079632679f);
    draw_ellipse(mainCx + pad.stickX * stickMove, mainCy - pad.stickY * stickMove,
                 stickDotSize, stickDotSize, kWhite);
    draw_ellipse(cCx + pad.cStickX * stickMove, cCy - pad.cStickY * stickMove,
                 stickDotSize, stickDotSize, kYellow);

    draw_dpad(font, x + 95.0f * scale, y + 10.0f * scale, 25.0f * scale, kWhite, pad.hold);
    draw_button_box(font, x + 87.5f * scale, y + 40.0f * scale, 10.0f * scale, 10.0f * scale, "",
                    kWhite, (pad.hold & PAD_BUTTON_START) != 0, 8.0f * scale);
    draw_button_box(font, x + 130.0f * scale, y + 7.5f * scale, 30.0f * scale,
                    15.0f * scale, "Y", kWhite, (pad.hold & PAD_BUTTON_Y) != 0,
                    8.0f * scale);
    draw_button_box(font, x + 167.5f * scale, y + 7.5f * scale, 15.0f * scale,
                    15.0f * scale, "Z",
                    kPurple, (pad.hold & PAD_TRIGGER_Z) != 0, 8.0f * scale);
    draw_button_box(font, x + 130.0f * scale, y + 30.0f * scale, 30.0f * scale,
                    30.0f * scale, "A", kGreen, (pad.hold & PAD_BUTTON_A) != 0,
                    8.0f * scale);
    draw_button_box(font, x + 108.5f * scale, y + 45.0f * scale, 13.0f * scale,
                    13.0f * scale, "B", kRed, (pad.hold & PAD_BUTTON_B) != 0,
                    8.0f * scale);
    draw_button_box(font, x + 167.5f * scale, y + 30.0f * scale, 15.0f * scale,
                    30.0f * scale, "X", kWhite, (pad.hold & PAD_BUTTON_X) != 0,
                    8.0f * scale);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%3d", pad.mainRawX);
    draw_native_text_plain(font, x, y + 65.0f * scale, 13.0f * scale, kWhite, buf);
    std::snprintf(buf, sizeof(buf), "%3d", pad.mainRawY);
    draw_native_text_plain(font, x + 23.0f * scale, y + 65.0f * scale, 13.0f * scale, kWhite,
                           buf);
    std::snprintf(buf, sizeof(buf), "%3d", pad.subRawX);
    draw_native_text_plain(font, x + 45.0f * scale, y + 65.0f * scale, 13.0f * scale, kYellow,
                           buf);
    std::snprintf(buf, sizeof(buf), "%3d", pad.subRawY);
    draw_native_text_plain(font, x + 70.0f * scale, y + 65.0f * scale, 13.0f * scale, kYellow,
                           buf);
}

void draw_native_link_debug(JUTFont* font) {
    const JUtility::TColor kWhite(0xFF, 0xFF, 0xFF, 0xFF);
    const float left = mDoGph_gInf_c::ScaleHUDXRight(452.0f);
    daAlink_c* player = daAlink_getAlinkActorClass();
    if (player == nullptr) {
        draw_native_text(font, left, 178.0f, 14.0f, kWhite, "link: ?");
        return;
    }

    const float speed3d = std::sqrt(player->speed.x * player->speed.x +
                                    player->speed.y * player->speed.y +
                                    player->speed.z * player->speed.z);
    float hours = dComIfGs_getTime() * (1.0f / 15.0f);
    while (hours >= 24.0f) hours -= 24.0f;
    while (hours < 0.0f) hours += 24.0f;
    const int hour = static_cast<int>(hours);
    const int minute = static_cast<int>((hours - static_cast<float>(hour)) * 60.0f);

    char lines[7][40];
    std::snprintf(lines[0], sizeof(lines[0]), "time: %02d:%02d", hour, minute);
    std::snprintf(lines[1], sizeof(lines[1]), "angle: %u", static_cast<u16>(player->shape_angle.y));
    std::snprintf(lines[2], sizeof(lines[2]), "y-angle: %d", player->mBodyAngle.x);
    std::snprintf(lines[3], sizeof(lines[3]), "speed: %.4f", speed3d);
    std::snprintf(lines[4], sizeof(lines[4]), "x-pos: %.4f", player->current.pos.x);
    std::snprintf(lines[5], sizeof(lines[5]), "y-pos: %.4f", player->current.pos.y);
    std::snprintf(lines[6], sizeof(lines[6]), "z-pos: %.4f", player->current.pos.z);

    float y = 182.0f;
    for (const auto& line : lines) {
        draw_native_text(font, left, y, 14.0f, kWhite, line);
        y += 20.0f;
    }
}

}  // namespace

void ImGuiPracticeSaves::drawNative(bool menuOpen) {
    // Only the native renderer; in imgui mode draw() renders the window instead.
    auto& settings = getSettings();
    if (!settings.game.nativePracticeMenu || settings.game.speedrunMode) {
        return;
    }
    const bool nativeLinkDebugInfo = settings.game.nativeLinkDebugInfo.getValue();
    const bool nativeInputViewer = settings.game.nativeInputViewer.getValue();
    if (!menuOpen && !nativeLinkDebugInfo && !nativeInputViewer) {
        return;
    }
    // Use overlay-owned render state, as decompGZ does, instead of depending on
    // the active scene's graphics port. The scene port may not exist while a
    // load-zone transition is replacing the play scene.
    static J2DOrthoGraph sNativeOrtho(0.0f, 0.0f, 608.0f, 448.0f, -1.0f, 1.0f);
    // Match the game's 2D coordinate space. A fixed 608-wide projection stretches
    // the overlay on wider viewports and makes the aspect-aware edge offsets use
    // coordinates outside that projection.
    sNativeOrtho.setOrtho(mDoGph_gInf_c::getMinXF(), mDoGph_gInf_c::getMinYF(),
                          mDoGph_gInf_c::getWidthF(), mDoGph_gInf_c::getHeightF(),
                          -1.0f, 1.0f);
    sNativeOrtho.setPort();

    // Retain one reference to the game font once its archive becomes available.
    // The archive pointer temporarily disappears during scene transitions, but
    // the retained font remains valid for this process-lifetime overlay.
    static JUTFont* sNativeFont = nullptr;
    if (sNativeFont == nullptr && dComIfGp_getFontArchive() != nullptr) {
        sNativeFont = mDoExt_getMesgFont();
    }
    JUTFont* font = sNativeFont;
    if (font == nullptr && menuOpen) {
        return;
    }

    const JUtility::TColor kWhite(0xFF, 0xFF, 0xFF, 0xFF);
    const JUtility::TColor kGreen(26, 230, 26, 0xFF);  // imgui (0.1, 0.9, 0.1)

    const float left = mDoGph_gInf_c::ScaleHUDXLeft(24.0f);
    const float headerSize = 18.0f;
    const float itemSize = 15.0f;
    const float lineH = 19.0f;
    float y = 24.0f;

    if (menuOpen) {
        draw_native_text(font, left, y, headerSize, kGreen, "practice tools");
        y += 32.0f;
    }

    if (!menuOpen) {
        // Overlay-only frame; keep drawing selected native displays below.
    } else if (!m_focusSaveList) {
        // Top-level category list. The cursor (green) follows m_mainCategory.
        for (int i = 0; i < static_cast<int>(MainCategory::Count); i++) {
            const bool cursor = (i == main_category_index(m_mainCategory));
            draw_native_text(font, left, y, itemSize, cursor ? kGreen : kWhite,
                             kMainCategoryNames[i]);
            y += lineH;
        }
    } else if (category_has_sublist(m_mainCategory) && !m_inSubList) {
        // Level 1: the sub-category list (gz-style). The cursor follows the
        // active sub index; A enters its rows/saves, B returns to the category
        // list. No tabs.
        if (m_mainCategory == MainCategory::Practice) {
            for (int i = 0; i < static_cast<int>(SaveCategory::Count); i++) {
                const bool cursor = (i == category_index(m_saveCategory));
                draw_native_text(font, left, y, itemSize, cursor ? kGreen : kWhite,
                                 kSaveCategories[i].label);
                y += lineH;
            }
        } else {
            static const char* const kToolsTabs[] = {"checkers", "displays", "link"};
            static const char* const kSceneTabs[] = {"environment", "viewers", "audio"};
            const char* const* labels =
                (m_mainCategory == MainCategory::Tools) ? kToolsTabs : kSceneTabs;
            const int cur = (m_mainCategory == MainCategory::Tools) ? s_gzToolsTab : s_gzSceneTab;
            for (int i = 0; i < 3; i++) {
                draw_native_text(font, left, y, itemSize, i == cur ? kGreen : kWhite, labels[i]);
                y += lineH;
            }
        }
    } else if (m_mainCategory == MainCategory::Practice) {
        // Level 2: the save list for the chosen category.
        const auto& saves = currentSaves();
        const int count = static_cast<int>(saves.size());
        if (count == 0) {
            draw_native_text(font, left, y, itemSize, kWhite, "No saves found.");
        } else {
            // Window the list so the cursor row stays visible when it overflows.
            const int visible = 15;
            int start = 0;
            if (count > visible) {
                start = std::clamp(m_selectedSave - visible / 2, 0, count - visible);
            }
            const int end = std::min(count, start + visible);
            for (int i = start; i < end; i++) {
                const bool cursor = (i == m_selectedSave);
                draw_native_text(font, left, y, itemSize, cursor ? kGreen : kWhite,
                                 saves[i].name.c_str());
                y += lineH;
            }
            // Bottom hint line: description of the hovered save (imgui tooltip ->
            // tpgz's bottom line, e.g. "Hangin' with Hugo").
            if (m_selectedSave >= 0 && m_selectedSave < count &&
                !saves[m_selectedSave].description.empty()) {
                draw_native_text(font, left, 410.0f, itemSize, kWhite,
                                 saves[m_selectedSave].description.c_str());
            }
        }
    } else {
        // Generic category panels. Row order/count must match
        // gz_generic_row_count + gz_activate/adjust_generic_row so the cursor
        // (m_selectedGenericRow, driven by handleController) and the actions
        // line up. The actions are already applied by handleController; here we
        // only render. Disabled rows stay greyed, exactly as in the imgui panels.
        auto& s = settings;
        const JUtility::TColor kGrey(150, 150, 150, 0xFF);
        const float checkX = left + 248.0f;  // checkbox column for bool rows
        const float valueX = left + 96.0f;   // value column for combo rows

        int rowIdx = 0;
        auto rowCol = [&](bool disabled) -> JUtility::TColor {
            if (rowIdx == m_selectedGenericRow) return kGreen;
            return disabled ? kGrey : kWhite;
        };
        // A bool option: label on the left, [X]/[ ] in a fixed right column.
        auto boolRow = [&](const char* label, bool on, bool disabled) {
            const JUtility::TColor col = rowCol(disabled);
            draw_native_text(font, left, y, itemSize, col, label);
            draw_native_text(font, checkX, y, itemSize, col, on ? "[X]" : "[ ]");
            y += lineH;
            ++rowIdx;
        };
        auto disabledBool = [&](const char* label) { boolRow(label, false, true); };
        // A plain row (buttons / the warp action): label only.
        auto plainRow = [&](const char* label, bool disabled) {
            draw_native_text(font, left, y, itemSize, rowCol(disabled), label);
            y += lineH;
            ++rowIdx;
        };
        // A combo row: label + current value (switched with L/R).
        auto comboRow = [&](const char* label, const std::string& value) {
            const JUtility::TColor col = rowCol(false);
            draw_native_text(font, left, y, itemSize, col, label);
            draw_native_text(font, valueX, y, itemSize, col, value.c_str());
            y += lineH;
            ++rowIdx;
        };
        switch (m_mainCategory) {
        case MainCategory::Cheats: {
            const bool en = !s.game.speedrunMode;
            boolRow("disable item timer", s.game.enableIndefiniteItemDrops.getValue(), !en);
            disabledBool("disable walls");
            disabledBool("fast bonk recovery");
            disabledBool("fast movement");
            boolRow("infinite air", s.game.infiniteOxygen.getValue(), !en);
            boolRow("infinite arrows", s.game.infiniteArrows.getValue(), !en);
            boolRow("infinite bombs", s.game.infiniteBombs.getValue(), !en);
            boolRow("infinite hearts", s.game.infiniteHearts.getValue(), !en);
            boolRow("infinite lantern oil", s.game.infiniteOil.getValue(), !en);
            boolRow("infinite rupees", s.game.infiniteRupees.getValue(), !en);
            boolRow("infinite slingshot seeds", s.game.infiniteSeeds.getValue(), !en);
            disabledBool("invincible link");
            boolRow("invincible enemies", s.game.invincibleEnemies.getValue(), !en);
            boolRow("moon jump", s.game.moonJump.getValue(), !en);
            disabledBool("no sinking in sand");
            boolRow("super clawshot", s.game.superClawshot.getValue(), !en);
            boolRow("transform anywhere", s.game.canTransformAnywhere.getValue(), !en);
            disabledBool("unrestricted items");
            break;
        }
        case MainCategory::Tools: {
            if (s_gzToolsTab == 0) {
                disabledBool("coro td");
                disabledBool("ebmb");
                disabledBool("elevator escape");
                boolRow("gorge void", s.game.gorgeVoidChecker.getValue(), false);
                disabledBool("ladder freezard cancel");
                disabledBool("rolls");
                disabledBool("universal map delay");
            } else if (s_gzToolsTab == 1) {
                disabledBool("a/b mash rate");
                boolRow("in-game timer", s.game.showSpeedrunRTATimer.getValue(),
                        !s.game.speedrunMode);
                boolRow("input viewer", s.game.nativeInputViewer.getValue(), s.game.speedrunMode);
                boolRow("link debug info", s.game.nativeLinkDebugInfo.getValue(),
                        s.game.speedrunMode);
                disabledBool("load timer");
                disabledBool("stage info");
                disabledBool("timer");
            } else {
                boolRow("free cam", s.game.freeCamera.getValue(), false);
                boolRow("move link", s.game.moveLink.getValue(), s.game.speedrunMode);
                disabledBool("teleport");
            }
            break;
        }
        case MainCategory::Scene: {
            if (s_gzSceneTab == 0) {
                disabledBool("freeze time");
                disabledBool("freeze actors");
                disabledBool("freeze camera");
            } else if (s_gzSceneTab == 1) {
                disabledBool("viewers");
            } else {
                disabledBool("mute bgm");
                disabledBool("mute sfx");
            }
            break;
        }
        case MainCategory::Settings: {
            disabledBool("boot to menu");
            disabledBool("cursor type");
            disabledBool("display mode");
            disabledBool("drop shadows");
            disabledBool("menu pauses game");
            disabledBool("menu sfx");
            disabledBool("reload type");
            disabledBool("state streaming");
            disabledBool("swap equips");
            disabledBool("theme");
            plainRow("command combos", true);
            plainRow("menu positions", true);
            plainRow("start gdb server", true);
            plainRow("save settings", true);
            plainRow("load settings", true);
            plainRow("delete settings", true);
            break;
        }
        case MainCategory::Warping: {
            clamp_gz_warp_state(s_gzWarpState);
            const auto& region = gameRegions[s_gzWarpState.region];
            const auto& map = region.maps[s_gzWarpState.map];
            const auto& room = map.mapRooms[s_gzWarpState.room];
            comboRow("type", region.regionName);
            comboRow("stage", map.mapName);
            comboRow("room", fmt::format("{}", room.roomNo));
            comboRow("spawn", fmt::format("{}", room.roomPoints[s_gzWarpState.spawn]));
            comboRow("layer", s_gzWarpState.layer < 0 ? std::string("default")
                                                      : fmt::format("{}", s_gzWarpState.layer));
            plainRow("warp", !dusk::IsGameLaunched);
            break;
        }
        case MainCategory::Flags:
            for (const char* r : kFlagRows) disabledBool(r);
            break;
        case MainCategory::Inventory:
            for (const char* r : kInventoryRows) disabledBool(r);
            break;
        case MainCategory::Memory:
            for (const char* r : kMemoryRows) disabledBool(r);
            break;
        default:
            break;
        }
    }

    if (nativeLinkDebugInfo && font != nullptr) {
        draw_native_link_debug(font);
    }
    if (nativeInputViewer) {
        draw_native_input_display(font);
    }
}

void ImGuiMenuTools::ShowPracticeSaves() {
    if (getSettings().game.speedrunMode) {
        m_showPracticeSaves = false;
        getTransientSettings().practiceMenuInputCapture = false;
        return;
    }

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
