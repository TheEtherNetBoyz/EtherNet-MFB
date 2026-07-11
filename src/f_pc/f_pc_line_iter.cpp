/**
 * f_pc_line_iter.cpp
 * Framework - Process Line Iterator
 */

#include "f_pc/f_pc_line_iter.h"
#include "SSystem/SComponent/c_tag.h"
#include "SSystem/SComponent/c_tag_iter.h"
#include "SSystem/SComponent/c_tree_iter.h"
#include "f_pc/f_pc_base.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_line.h"
#include "dusk/multiplayer/multiplayer.hpp"
#include <cstdint>


static int fpcLnIt_MethodCall(create_tag_class* i_createTag, method_filter* i_filter) {
    base_process_class* process = static_cast<base_process_class*>(i_createTag->mpTagData);
#if TARGET_PC
    // Online state changes can remove a queued process during this traversal. Freed process
    // memory is poisoned with all bits set on PC, leaving a stale mpTagData value of -1.
    // Keep this additional lifecycle protection scoped to active multiplayer sessions.
    constexpr uintptr_t kFreedProcessSentinel = static_cast<uintptr_t>(-1);
    const bool invalidOnlineTag = dusk::multiplayer::is_enabled() &&
                                  (!cTg_IsUse(i_createTag) ||
                                   reinterpret_cast<uintptr_t>(process) == kFreedProcessSentinel);
    if (process == NULL || invalidOnlineTag || process->state.init_state == 3 ||
        process->layer_tag.layer == NULL)
    {
        return 0;
    }
#endif

    layer_class* layer = process->layer_tag.layer;
    layer_class* save_layer = fpcLy_CurrentLayer();
    int ret;

    fpcLy_SetCurrentLayer(layer);
    ret = cTgIt_MethodCall(i_createTag, i_filter);
    fpcLy_SetCurrentLayer(save_layer);

    return ret;
}

void fpcLnIt_Queue(fpcLnIt_QueueFunc i_queueFunc) {
    method_filter filter;
    filter.mpMethodFunc = (cNdIt_MethodFunc)i_queueFunc;
    filter.mpUserData = NULL;
    cTrIt_Method(&g_fpcLn_Queue, (cNdIt_MethodFunc)fpcLnIt_MethodCall, &filter);
}
