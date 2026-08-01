#pragma once

#include <cstdint>

// Deliberately minimal: this is included from core save-state headers
// (d/d_com_inf_game.h), so it must not pull in sockets/json/etc.
namespace dusk::multiplayer {

void notify_local_event_bit_set(uint16_t flag);

// Some event bits are toggles, not one-way "achievement" flags -- e.g.
// F_0800 (Midna unreachable on Z after the sewers escape) gets set, then
// later cleared again once the post-sewers Midna text plays. Without this
// hook, a peer who receives the "set" broadcast has no way to ever learn
// the bit was cleared again, since dComIfGs_offEventBit had no multiplayer
// hook at all -- they'd be stuck with Midna permanently unreachable until a
// full manual sync. Mirrors notify_local_event_bit_set's guard/shape.
void notify_local_event_bit_cleared(uint16_t flag);

// Chest-open ("Tbox") bits are per-stage (dSv_memBit_c), so the receiver
// needs to know which stage's save table the bit belongs to, not just the
// bit number. The implementation determines the local stage itself.
void notify_local_tbox_set(int flag);

// Dungeon item bits, also per-stage (dSv_memBit_c). `kind` matches
// dSv_memBit_c's anonymous enum in include/d/d_save.h: 0=MAP, 1=COMPASS,
// 2=BOSS_KEY, 3=STAGE_BOSS_ENEMY, 4=STAGE_LIFE, 5=STAGE_BOSS_DEMO,
// 6=OOCCOO_NOTE (warp), 7=STAGE_BOSS_ENEMY_2 (mid-boss).
void notify_local_dungeon_item_set(int kind);

void notify_local_ooccoo_acquired(int itemId);
void notify_local_ooccoo_warp_out();
void notify_local_ooccoo_cleared();
// dSv_info_c::init() replaces the active save data during boot, soft reset,
// and file selection. Ooccoo's companion runtime metadata must have the same
// lifetime as the save inventory it describes.
void notify_local_save_reset();

// Item acquisition lane for durable inventory/progression effects that are
// not represented by actor/chest flags alone. Keep this narrowly filtered in
// the implementation; TP's item table also includes consumables.
void notify_local_item_get(int itemId);
// Randomizer-only item event. Unlike the durable vanilla item lane above,
// this intentionally includes consumables. itemId is already progressively
// resolved by the originating peer and must be applied without resolving it
// again on receivers.
void notify_local_randomizer_item_get(int itemId);
void notify_local_item_first_bit_set(int itemId);
void notify_local_item_first_bit_cleared(int itemId);

// Memory-tier switch bits only (dSv_memBit_c, per-stage, persistent). The
// caller (d_com_inf_game.h) already filtered out dungeon-tier and zone-tier
// switch IDs, which are deliberately ephemeral/room-resetting by design and
// not synced.
void notify_local_memory_switch_set(int flag);
// Door/gate unlock switches can live in the room/zone switch range rather
// than the per-stage memory range. These are only emitted from a narrow actor
// context filter in the implementation.
void notify_local_room_switch_set(int flag, int room);
// Mirrors notify_local_memory_switch_set for the off/clear direction -- e.g.
// the Darkhammer mini-boss's "fight in progress" switch gets cleared on
// death (d_a_e_th.cpp), and without this a peer who wasn't present for the
// fight would have that switch stuck on forever. Subject to the same
// lifecycle-actor exclusion as the "on" side.
void notify_local_memory_switch_cleared(int flag);
void begin_local_switch_actor_context(int actorName, int room, int flag,
                                      uint32_t actorParams = 0xFFFFFFFF);
void end_local_switch_actor_context();
void notify_local_room_scene_initialized(int room);
void notify_actor_create_request(int actorName, uint32_t actorParams, int room, float x, float y,
                                 float z);
void notify_actor_delete(int actorName, uint32_t actorParams, int room, float x, float y, float z);

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
// as OoT Anchor. In randomizer, rando_item_get owns key gains while this
// lane continues to carry key consumption; previousKeyNum distinguishes the
// two without depending on a playing host or packet timing.
void notify_local_key_num_set(uint8_t previousKeyNum, uint8_t keyNum);

// Light Drop tear count, per dark-twilight area (dSv_light_drop_c). Same
// absolute-overwrite model as small keys above, for the same reason: the
// count is read directly from save state, not derived from a spawned
// actor, so it works regardless of where the receiving player currently
// is. The receiver also re-checks the 15-tear threshold event bit after
// applying the value (idempotent if already set).
void notify_local_light_drop_num_set(uint8_t area, uint8_t previousNum, uint8_t num);

// Vessel-of-light ownership bit, per dark-twilight area
// (dSv_light_drop_c::mLightDropGetFlag). This is separate from the event flag
// saying the cutscene was watched and from the tear count itself.
void notify_local_light_drop_get_flag_set(uint8_t area);

// Max life (dSv_player_status_a_c::mMaxLife). Heart pieces/containers are
// repeatable pickups sharing one item ID each (dItemNo_KAKERA_HEART_e/
// dItemNo_UTAWA_HEART_e), so the item_get lane's !isItemFirstBit() replay
// guard only lets the first one of each type apply remotely -- every
// later one is silently swallowed. Live updates include both the previous
// and current value so two different pickups made from the same shared
// starting value can be merged additively instead of collapsing under a
// plain monotonic max. Snapshots still use max-merge for late-join catch-up.
void notify_local_max_life_set(uint8_t previousMaxLife, uint8_t maxLife);

// Total occupied bottle slots (0-4), NOT bottle contents. In vanilla,
// empty-bottle grants are repeatable pickups sharing one item ID
// (dItemNo_EMPTY_BOTTLE_e) across up to 4 different NPCs/quests, the same
// shape as the max-life bug above. Live updates carry the previous and
// current slot counts so concurrent distinct grants are additive; snapshots
// retain the monotonic count merge. Additional slots are filled locally as
// generic empty bottles and existing contents are never touched.
// Randomizer live rewards use rando_item_get instead so the count and item
// lanes cannot both apply the same bottle. Snapshots still carry the count.
// Deliberately does not sync bottle contents (what liquid is in which
// slot) -- that stays local/volatile per the existing consumables rule.
void notify_local_bottle_slot_count_set(uint8_t previousCount, uint8_t count);

// Current wallet rupee total. Freestanding/hidden rupees use ordinary
// memory-item flags for "collected", so the wallet value needs an absolute
// companion lane or peers can lose the money while still losing the pickup.
void notify_local_rupee_count_set(uint16_t rupees);

// Save-backed progression counters which are not represented correctly by
// the generic item/event-bit lanes. Poe souls and Charlo's offering total are
// monotonic; Malo Mart's fundraiser may also reset when a vanilla funding
// phase completes, so its live updates carry the exact current value. Live
// Poe updates are suppressed in randomizer because rando_item_get already
// owns those rewards; snapshots still carry the total for catch-up.
void notify_local_poe_count_set(uint8_t previousCount, uint8_t count);
void notify_local_malo_fundraising_set(uint16_t value);
void notify_local_charlo_offering_set(uint16_t value);

// Fishing-journal records. Catch notifications are distinct from size-only
// updates so repeatable catches can be merged once per peer while largest
// sizes remain monotonic. Only the six species displayed by the journal are
// accepted on the wire.
void notify_local_fish_caught(uint8_t fishIndex, uint16_t count, uint8_t maxSize);
void notify_local_fish_size_set(uint8_t fishIndex, uint16_t count, uint8_t maxSize);

// The saved scent occupying the collection-screen scent slot. NONE means the
// slot does not exist; validated scent IDs merge in vanilla story order so a
// clean/stale peer cannot erase or regress an acquired scent.
void notify_local_collect_smell_set(uint8_t smell);

}  // namespace dusk::multiplayer
