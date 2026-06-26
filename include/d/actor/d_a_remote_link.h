#ifndef D_A_REMOTE_LINK_H
#define D_A_REMOTE_LINK_H

#include "SSystem/SComponent/c_phase.h"
#include "d/d_resorce.h"
#include "d/d_kankyo.h"
#include "dusk/multiplayer/multiplayer.hpp"
#include "f_op/f_op_actor_mng.h"

class mDoExt_bckAnm;
class JKRExpHeap;
class JKRMemArchive;
class J3DAnmTransform;
class J3DAnmTevRegKey;
class J3DAnmTextureSRTKey;
class J3DShape;

/**
 * Remote Link visual owner.
 *
 * This actor is intentionally not daAlink_c and does not participate in player
 * state. It owns the human Link visual model set and submits it through the
 * normal actor model lifecycle.
 */
class daRemoteLink_c : public fopAc_ac_c {
public:
    daRemoteLink_c();

    cPhs_Step create();
    int CreateHeap();
    int Execute();
    int Draw();
    int Delete();

    void setRemotePose(const cXyz& i_pos, s16 i_angleY, s8 i_roomNo);
    void setRemoteActionState(int i_procId, int i_procVar0, int i_procVar1, int i_procVar2,
                              int i_procVar3, int i_procVar5, f32 i_underFrame,
                              u16 i_underBck0, f32 i_underFrame0, f32 i_underRate0,
                              u16 i_upperBck2, f32 i_upperFrame2, f32 i_upperRate2,
                              u16 i_equipItem, int i_swordVariant, int i_shieldVariant,
                              bool i_swordDraw, bool i_shieldDraw, bool i_swordOut,
                              bool i_itemDraw, bool i_kanteraDraw, int i_itemActorKind,
                              int i_rideActorKind);
    void setRemoteMatrices(const dusk::multiplayer::RemoteLinkMatrixSnapshot& i_matrices);

    static int createHeapCallBack(fopAc_ac_c* i_this);

private:
    enum VisualForm {
        FORM_HUMAN_KOKIRI,
        FORM_WOLF,
    };

    struct VisualState {
        VisualForm form;
    };

    struct BckCacheEntry {
        u16 resId;
        J3DAnmTransform* bck;
    };

    struct OwnedResourceCacheEntry {
        s32 index;
        void* resource;
    };

    struct AramResourceCacheEntry {
        u16 resId;
        void* resource;
    };

    struct HeldItemVisualDesc {
        u16 itemNo;
        u16 bmdResId;
        u32 bmdBufferSize;
        u32 modelFlags;
        u32 diffFlags;
        u16 brkResId;
    };

    struct OwnedArchiveSlot {
        const char* arcName;
        JKRMemArchive* archive;
        s32 entry;
        bool mounted;
        OwnedResourceCacheEntry cache[16];
    };

    void setOriginalHeap(JKRExpHeap** i_ppheap, u32 i_size);
    J3DModel* initModel(J3DModelData* i_modelData, u32 i_mdlFlags, u32 i_diffFlags);
    J3DModel* initModel(J3DModelData* i_modelData, u32 i_diffFlags);
    void setupHumanKokiriModel();
    void setupWolfModel();
    bool setupMagicArmorBrk();
    void setupEquipmentModels();
    void destroyEquipmentModels();
    const HeldItemVisualDesc* getHeldItemVisualDesc(u16 i_itemNo) const;
    void* loadAramResource(u16 i_resId, u32 i_bufSize, bool i_isModel);
    J3DModelData* loadAramBmd(u16 i_resId, u32 i_bufSize);
    J3DAnmTevRegKey* loadAramItemBrk(u16 i_resId, J3DModel* i_model);
    void setupHeldItemModel();
    void clearHeldItemExtras();
    void setupLinkedItemModels();
    void setupSwordMaterialAnm(J3DModel* i_model, int i_swordVariant);
    void applySwordShapeVisibility();
    void setupMotionAnimation();
    J3DAnmTransform* loadMotionBck(u16 i_resId);
    J3DAnmTransform* getMotionBck(u16 i_resId);
    u16 selectActionBck(f32* o_speed);
    void updateMotionAnimation();
    bool setMotionBck(u16 i_resId, f32 i_speed);
    bool reserveSlot();
    void releaseSlot();
    bool mountOwnedArchive();
    bool mountArchiveSlot(OwnedArchiveSlot& i_slot);
    void initArchiveSlot(OwnedArchiveSlot& i_slot, const char* i_arcName);
    void deleteArchiveSlot(OwnedArchiveSlot& i_slot);
    const char* getCurrentArcName() const;
    const char* getBodyResName() const;
    const char* getHeadResName() const;
    const char* getFaceResName() const;
    const char* getHandResName() const;
    u32 getOwnedArchiveNodeType(u32 i_fileIndex) const;
    u32 getArchiveNodeType(JKRMemArchive* i_archive, u32 i_fileIndex) const;
    void* convertOwnedObjectRes(s32 i_index);
    void* convertArchiveObjectRes(OwnedArchiveSlot& i_slot, s32 i_index);
    void* getOwnedObjectRes(const char* i_resName);
    void* getArchiveObjectRes(OwnedArchiveSlot& i_slot, const char* i_resName);
    void* getArchiveObjectRes(OwnedArchiveSlot& i_slot, s32 i_index);
    bool copyRemoteModelMatrices(J3DModel* i_model,
                                 const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source);
    void setBaseMtx();
    void calcModels();
    void drawModel(J3DModel* i_model);
    void drawLinkedItemActorModel();
    void hideAllHandShapes();
    void setupDrawHands();

    /* 0x568 */ request_of_phase_process_class mPhase;
    /* 0x570 */ JKRExpHeap* mpArcHeap;
    /* 0x574 */ JKRMemArchive* mpOwnedArchive;
    /* 0x578 */ OwnedResourceCacheEntry mOwnedResourceCache[16];
    /* 0x5F8 */ s32 mOwnedArchiveEntry;
    /* 0x5FC */ bool mOwnedArchiveMounted;
    /* 0x600 */ OwnedArchiveSlot mEquipmentArchives[5];
    /* 0x920 */ VisualState mVisualState;
    /* 0x924 */ void* mpWarpTexData;
    /* 0x928 */ J3DModel* mpBodyModel;
    /* 0x92C */ J3DModel* mpHeadModel;
    /* 0x930 */ J3DModel* mpHandModel;
    /* 0x934 */ J3DModel* mpFaceModel;
    /* 0x938 */ J3DModel* mpSwordModel;
    /* 0x93C */ J3DModel* mpSheathModel;
    /* 0x940 */ J3DModel* mpShieldModel;
    /* 0x944 */ J3DModel* mpHeldItemModel;
    /* 0x948 */ J3DModel* mpHookTipModel;
    /* 0x94C */ J3DModel* mpHookSubItemModel;
    /* 0x950 */ J3DModel* mpHookSubTipModel;
    /* 0x954 */ J3DModel* mpArrowModel;
    /* 0x958 */ J3DModel* mpKanteraModel;
    /* 0x95C */ J3DModel* mpKanteraGlowModel;
    /* 0x960 */ J3DModel* mpItemActorModel;
    /* 0x964 */ J3DModel* mpRideActorModel;
    /* 0x968 */ J3DModel* mpMidnaModel;
    /* 0x96C */ J3DModel* mpMidnaMaskModel;
    /* 0x970 */ J3DModel* mpMidnaHandModel;
    /* 0x974 */ J3DModel* mpMidnaHairModel;
    /* 0x978 */ J3DAnmTevRegKey* mpHeldItemBrk;
    /* 0x97C */ AramResourceCacheEntry mAramResourceCache[16];
    /* 0x9FC */ J3DAnmTevRegKey* mpMagicArmorBodyBrk;
    /* 0xA00 */ J3DAnmTevRegKey* mpMagicArmorHeadBrk;
    /* 0xA04 */ mDoExt_bckAnm* mpMotionBck;
    /* 0xA08 */ BckCacheEntry mBckCache[48];
    /* 0xB88 */ u16 mMotionBckResId;
    /* 0xB8C */ f32 mRemoteMoveSpeed;
    /* 0xB90 */ cXyz mLastRemotePos;
    /* 0xB9C */ int mRemoteProcId;
    /* 0xBA0 */ int mRemoteProcVar0;
    /* 0xBA4 */ int mRemoteProcVar1;
    /* 0xBA8 */ int mRemoteProcVar2;
    /* 0xBAC */ int mRemoteProcVar3;
    /* 0xBB0 */ int mRemoteProcVar5;
    /* 0xBB4 */ f32 mRemoteUnderFrame;
    /* 0xBB8 */ u16 mRemoteUnderBck0;
    /* 0xBBC */ f32 mRemoteUnderFrame0;
    /* 0xBC0 */ f32 mRemoteUnderRate0;
    /* 0xBC4 */ u16 mRemoteUpperBck2;
    /* 0xBC8 */ f32 mRemoteUpperFrame2;
    /* 0xBCC */ f32 mRemoteUpperRate2;
    /* 0xBD0 */ u16 mRemoteEquipItem;
    /* 0xBD4 */ int mRemoteSwordVariant;
    /* 0xBD8 */ int mRemoteShieldVariant;
    /* 0xBDC */ int mLoadedSwordVariant;
    /* 0xBE0 */ int mLoadedShieldVariant;
    /* 0xBE4 */ u16 mLoadedHeldItem;
    /* 0xBE8 */ bool mRemoteSwordDraw;
    /* 0xBE9 */ bool mRemoteShieldDraw;
    /* 0xBEA */ bool mRemoteSwordOut;
    /* 0xBEB */ bool mHeldItemMatrixValid;
    /* 0xBEC */ bool mHookTipMatrixValid;
    /* 0xBED */ bool mHookSubItemMatrixValid;
    /* 0xBEE */ bool mHookSubTipMatrixValid;
    /* 0xBEF */ bool mArrowMatrixValid;
    /* 0xBF0 */ bool mKanteraMatrixValid;
    /* 0xBF1 */ bool mKanteraGlowMatrixValid;
    /* 0xBF2 */ bool mItemActorMatrixValid;
    /* 0xBF3 */ bool mRideActorMatrixValid;
    /* 0xBF4 */ bool mMidnaMatrixValid;
    /* 0xBF5 */ bool mMidnaMaskMatrixValid;
    /* 0xBF6 */ bool mMidnaHandMatrixValid;
    /* 0xBF7 */ bool mMidnaHairMatrixValid;
    /* 0xBF8 */ bool mRemoteItemDraw;
    /* 0xBF9 */ bool mRemoteKanteraDraw;
    /* 0xBFA */ bool mHasRemotePose;
    /* 0xBFB */ bool mHasRemoteMatrices;
    /* 0xBFC */ int mClothesVariant;
    /* 0xC00 */ J3DShape* mpLeftBodyHandShape;
    /* 0xC04 */ J3DShape* mpRightBodyHandShape;
    /* 0xC08 */ int mRemoteItemActorKind;
    /* 0xC0C */ int mRemoteRideActorKind;
    /* 0xC10 */ int mLoadedItemActorKind;
    /* 0xC14 */ int mLoadedRideActorKind;
    /* 0xC18 */ int mRemoteBombFlashTicks;
    /* 0xC1C */ int mMidnaHairShape;
    /* 0xC20 */ bool mSlotReserved;
};

#endif /* D_A_REMOTE_LINK_H */
