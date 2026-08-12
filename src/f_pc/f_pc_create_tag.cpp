/**
 * f_pc_create_tag.cpp
 * Framework - Process Create Tag
 */

#include "f_pc/f_pc_create_tag.h"

DUSK_GAME_DATA node_list_class g_fpcCtTg_Queue = {NULL, NULL, 0};
#if TARGET_PC
u64 g_fpcCtTg_ActivityEpoch = 0;
#endif

void fpcCtTg_ToCreateQ(create_tag* i_createTag) {
#if TARGET_PC
    ++g_fpcCtTg_ActivityEpoch;
#endif
    cTg_Addition(&g_fpcCtTg_Queue, &i_createTag->base);
}

void fpcCtTg_CreateQTo(create_tag* i_createTag) {
    cTg_SingleCut(&i_createTag->base);
}

int fpcCtTg_Init(create_tag* i_createTag, void* i_data) {
    cTg_Create(&i_createTag->base, i_data);
    return 1;
}
