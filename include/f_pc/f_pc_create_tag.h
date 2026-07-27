
#ifndef F_PC_CREATE_TAG_H_
#define F_PC_CREATE_TAG_H_

#include "SSystem/SComponent/c_list.h"
#include "SSystem/SComponent/c_tag.h"
#include <types.h>

typedef struct create_tag {
    create_tag_class base;
} create_tag;

void fpcCtTg_ToCreateQ(create_tag* i_createTag);
void fpcCtTg_CreateQTo(create_tag* i_createTag);
int fpcCtTg_Init(create_tag* i_createTag, void* i_data);

extern node_list_class g_fpcCtTg_Queue;
#if TARGET_PC
extern u64 g_fpcCtTg_ActivityEpoch;
#endif

#endif
