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

}  // namespace dusk::multiplayer
