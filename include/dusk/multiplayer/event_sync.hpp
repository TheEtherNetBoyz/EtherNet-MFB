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

// Memory-tier "Item" bits only (dSv_memBit_c, per-stage, persistent). `flag`
// is the already-offset local index (global ID minus MEMORY_ITEM), matching
// dSv_memBit_c::onItem's own indexing.
void notify_local_memory_item_set(int flag);

}  // namespace dusk::multiplayer
