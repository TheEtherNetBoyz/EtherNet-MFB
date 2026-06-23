#ifndef D_A_OBJ_SWORD_H
#define D_A_OBJ_SWORD_H

#include "d/actor/d_a_itembase.h"

/**
 * @ingroup actors-objects
 * @class daObjSword_c
 * @brief Ordon Sword
 *
 * @details
 *
 */
class daObjSword_c : public daItemBase_c {
public:
    typedef int (daObjSword_c::*actionFunc)();

    void initBaseMtx();
    void setBaseMtx();
    int Create();
    cPhs_Step create();
    int actionWait();
    int initActionOrderGetDemo();
    int actionOrderGetDemo();
    int actionGetDemo();
    int execute();
    int draw();
    int _delete();

    u8 getItemBit() { return fopAcM_GetParamBit(this, 0, 8); }
    u8 getSwbit() { return fopAcM_GetParamBit(this, 8, 8); }
    void setStatus(u8 status) { mStatus = status; }

private:
    /* 0x92c */ u32 field_0x92c;
    /* 0x930 */ fpc_ProcID mProcID;
    /* 0x934 */ u8 mStatus;
};

STATIC_ASSERT(sizeof(daObjSword_c) == 0x938);

// Same shape as duskRepairObjLifeVisual (d_a_obj_life_container.h): finds a
// currently-spawned Ordon Sword pickup by its global Item-bit ID
// (daObjSword_c::getItemBit() space, same as fopAcM_onItem's bitNo, not the
// dSv_memBit_c-local offset index) and deletes it, mirroring what
// daObjSword_c::actionGetDemo() does on a real pickup minus the grant.
// Returns true if a live actor was found and deleted.
bool duskRepairObjSwordVisual(int i_globalSaveBitNo);

#endif /* D_A_OBJ_SWORD_H */
