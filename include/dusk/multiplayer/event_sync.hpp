#pragma once

#include <cstdint>

// Deliberately minimal: this is included from core save-state headers
// (d/d_com_inf_game.h), so it must not pull in sockets/json/etc.
namespace dusk::multiplayer {

void notify_local_event_bit_set(uint16_t flag);

// Chest-open ("Tbox") bits are per-stage (dSv_memBit_c), so the receiver
// needs to know which stage's save table the bit belongs to, not just the
// bit number. The implementation determines the local stage itself.
void notify_local_tbox_set(int flag);

// Dungeon item bits, also per-stage (dSv_memBit_c). `kind` matches
// dSv_memBit_c's anonymous enum in include/d/d_save.h: 0=MAP, 1=COMPASS,
// 2=BOSS_KEY, 3=STAGE_BOSS_ENEMY, 4=STAGE_LIFE, 5=STAGE_BOSS_DEMO,
// 6=OOCCOO_NOTE (warp), 7=STAGE_BOSS_ENEMY_2 (mid-boss).
void notify_local_dungeon_item_set(int kind);

// Item acquisition lane for durable inventory/progression effects that are
// not represented by actor/chest flags alone. Keep this narrowly filtered in
// the implementation; TP's item table also includes consumables.
void notify_local_item_get(int itemId);

// Memory-tier switch bits only (dSv_memBit_c, per-stage, persistent). The
// caller (d_com_inf_game.h) already filtered out dungeon-tier and zone-tier
// switch IDs, which are deliberately ephemeral/room-resetting by design and
// not synced.
void notify_local_memory_switch_set(int flag);
void begin_local_switch_actor_context(int actorName, int room, int flag);
void end_local_switch_actor_context();
void notify_local_room_scene_initialized(int room);

// Memory-tier "Item" bits only (dSv_memBit_c, per-stage, persistent). `flag`
// is the already-offset local index (global ID minus MEMORY_ITEM), matching
// dSv_memBit_c::onItem's own indexing.
void notify_local_memory_item_set(int flag);

// Fused Shadow (mCrystal) / Mirror of Twilight (mMirror) shard collection.
// Both are single-byte bitmasks (item index 0-7), OR-merge like onCollect*.
void notify_local_collect_crystal_set(int item);
void notify_local_collect_mirror_set(int item);

// Twilight-clear / wolf-transform region permission levels
// (dSv_player_status_b_c::mDarkClearLevelFlag/mTransformLevelFlag) and the
// region-reveal bit (dSv_player_field_last_stay_info_c::mRegion). All three
// are single-byte bitmasks (index 0-7), OR-merge.
void notify_local_dark_clear_lv_set(int no);
void notify_local_transform_lv_set(int no);
void notify_local_region_bit_set(int region);

// Generic equipment-tier Collect bits (dSv_player_collect_c::mItem[8]).
// `type` matches the CollectItem enum (COLLECT_CLOTHING/SWORD/SHIELD/SMELL),
// `item` is the bit index (0-7) within that type's byte.
void notify_local_collect_set(int type, int item);

// Per-stage visited-room bits (dSv_memory2_c). `stage` is the save-table
// index (dStage_FileList2_dt_c::field_0x13), matching
// dComIfGs_onSaveVisitedRoom's own indexing.
void notify_local_visited_room_set(int stage, int roomNo);

// Letter quest "get" flags (dSv_letter_info_c::mLetterGetFlags, 64 bits).
// Does not cover "read" flags or mGetNumber, which are personal UI/record
// state, not progression.
void notify_local_letter_get_set(int no);

// Per-dungeon small key count (dSv_memBit_c::mKeyNum), current stage only.
// Mirrors OoT Anchor's UPDATE_DUNGEON_ITEMS packet: broadcasts the absolute
// current count (not a delta/bit) on every real local change -- both
// pickup (drained from dComIfGp_setItemKeyNumCount via dMeter2_c::moveKey)
// and key use/door-open (same drain path, negative delta) funnel through
// the same setter this hooks, so one hook covers both triggers. The
// receiver applies it as an absolute overwrite (last-write-wins), not an
// OR-merge -- there is no per-key identity in this lane, by design, same
// as OoT Anchor.
void notify_local_key_num_set(uint8_t keyNum);

// Light Drop tear count, per dark-twilight area (dSv_light_drop_c). Same
// absolute-overwrite model as small keys above, for the same reason: the
// count is read directly from save state, not derived from a spawned
// actor, so it works regardless of where the receiving player currently
// is. The receiver also re-checks the 15-tear threshold event bit after
// applying the value (idempotent if already set).
void notify_local_light_drop_num_set(uint8_t area, uint8_t num);

// Max life (dSv_player_status_a_c::mMaxLife). Heart pieces/containers are
// repeatable pickups sharing one item ID each (dItemNo_KAKERA_HEART_e/
// dItemNo_UTAWA_HEART_e), so the item_get lane's !isItemFirstBit() replay
// guard only lets the first one of each type apply remotely -- every
// later one is silently swallowed. Fixed the same way as small keys/Light
// Drop: broadcast the absolute current value on every real change. Unlike
// keys/tears (last-write-wins), the receiver applies this with a
// monotonic max-merge (only raise, never lower) -- max life should never
// decrease from a remote update, so there is no race-condition tradeoff
// to accept here, matching the rule save_snapshot's own max_life field
// already used for late-join catch-up.
void notify_local_max_life_set(uint8_t maxLife);

// Total occupied bottle slots (0-4), NOT bottle contents. Empty-bottle
// grants are repeatable pickups sharing one item ID
// (dItemNo_EMPTY_BOTTLE_e) across up to 4 different NPCs/quests, the same
// shape as the max-life bug above. Broadcasts the absolute slot count
// (how many of the 4 slots are occupied by any bottle item, regardless of
// what liquid each currently holds) and applies it on receive as a
// monotonic max-merge, filling additional empty slots locally to match.
// Deliberately does not sync bottle contents (what liquid is in which
// slot) -- that stays local/volatile per the existing consumables rule.
void notify_local_bottle_slot_count_set(uint8_t count);

}  // namespace dusk::multiplayer
