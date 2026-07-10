#ifndef D_A_REMOTE_LINK_H
#define D_A_REMOTE_LINK_H

#include "SSystem/SComponent/c_phase.h"
#include "d/d_resorce.h"
#include "d/d_cc_d.h"
#include "d/d_kankyo.h"
#include "dusk/multiplayer/multiplayer.hpp"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_ext.h"
#include "Z2AudioLib/Z2SoundObject.h"

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
                              bool i_swordDraw, bool i_shieldDraw, bool i_shieldGuardActive,
                              bool i_swordOut,
                              bool i_heavyBoots, bool i_itemDraw, bool i_kanteraDraw,
                              bool i_midnaDraw, bool i_midnaMaskDraw, bool i_midnaHandDraw,
                              bool i_midnaHairDraw, bool i_midnaShadowForm, int i_itemActorKind,
                              int i_itemActorBombExTime, int i_itemActorBombFlash, int i_rideActorKind);
    void setRemoteMatrices(const dusk::multiplayer::RemoteLinkMatrixSnapshot& i_matrices);
    void setRemoteBombObjectState(const dusk::multiplayer::RemoteBombObjectSnapshot& i_bomb);
    void setRemoteHatState(const std::array<int16_t, 10>& i_rotA,
                           const std::array<int16_t, 10>& i_rotB,
                           const std::array<int16_t, 3>& i_swing, s16 i_shapeY);
    void applyRemoteBodyMatrixInterpolationForPresentation();
    bool getNameLabelPosition(cXyz* o_pos) const;
    void playRemoteSound(const dusk::multiplayer::RemoteAudioEvent& i_event);
    void syncRemoteActiveSounds(const std::vector<dusk::multiplayer::RemoteAudioEvent>& i_events);
    int headModelCallBack(int i_jointNo);

    static int createHeapCallBack(fopAc_ac_c* i_this);
    static bool canReserveSlot();

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

    struct RemoteModelMatrixInterpState {
        dusk::multiplayer::RemoteModelMatrixSnapshot prev;
        dusk::multiplayer::RemoteModelMatrixSnapshot curr;
        bool prevValid;
        bool currValid;
    };

    struct HeldItemVisualDesc {
        u16 itemNo;
        u16 bmdResId;
        u32 bmdBufferSize;
        u32 modelFlags;
        u32 diffFlags;
        u16 brkResId;
    };

    struct ActiveRemoteSound {
        dusk::multiplayer::RemoteAudioEvent event;
        u8 ageTicks;
        bool active;
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
    void setupShadowMidnaModels();
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
    void captureRemoteModelMatrixSnapshot(
        J3DModel* i_model, const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source,
        RemoteModelMatrixInterpState& io_state);
    void clearRemoteModelMatrixInterpolation(RemoteModelMatrixInterpState& io_state);
    bool applyInterpolatedRemoteModelMatrices(
        J3DModel* i_model, RemoteModelMatrixInterpState& io_state, const char* i_label);
    void overrideRemoteModelMatrices(J3DModel* i_model);
    void applyInterpolatedRemoteAttachments();
    void captureRemoteBodyMatrixSnapshot(
        const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source);
    void clearRemoteBodyMatrixInterpolation();
    void applyInterpolatedRemoteBodyMatrices();
    void setBaseMtx();
    void calcModels();
    void drawModel(J3DModel* i_model);
    void drawLinkedItemActorModel();
    void drawShadowMidnaModels();
    void updateRemoteBombActor();
    void stopRemoteBombActor(bool i_explode);
    void maybeSpawnRemoteBombExplosion(int i_nextItemActorKind);
    void spawnRemoteBombExplosion();
    void initPvpTargetCollision();
    void updatePvpTargetCollision();
    void updatePvpAttentionTarget();
    void updatePvpMidnaBindEffect();
    void stopRemoteActiveSounds();
    void hideAllHandShapes();
    void setupDrawHands();
    void setupHeavyBootModels();
    void applyHeavyBootMatrices();
    void applyWolfEquipmentMatrices();

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
    dKy_tevstr_c mRemoteMidnaTevStr;
    /* 0x968 */ J3DModel* mpMidnaModel;
    /* 0x96C */ J3DModel* mpMidnaMaskModel;
    /* 0x970 */ J3DModel* mpMidnaHandModel;
    /* 0x974 */ J3DModel* mpMidnaHairModel;
    /* 0x978 */ J3DModel* mpShadowMidnaModel;
    /* 0x97C */ J3DModel* mpShadowMidnaMaskModel;
    /* 0x980 */ J3DModel* mpShadowMidnaHandModel;
    /* 0x984 */ J3DModel* mpShadowMidnaHairModel;
    /* 0x988 */ J3DModel* mpMidnaGlowModel;
    /* 0x98C */ mDoExt_invisibleModel mShadowMidnaInvModel;
    /* 0x994 */ mDoExt_invisibleModel mShadowMidnaMaskInvModel;
    /* 0x99C */ mDoExt_invisibleModel mShadowMidnaHandInvModel;
    /* 0x9A4 */ mDoExt_invisibleModel mShadowMidnaHairInvModel;
    /* 0x9AC */ J3DModel* mpHeavyBootModels[2];
    /* 0x988 */ J3DAnmTevRegKey* mpHeldItemBrk;
    /* 0x98C */ AramResourceCacheEntry mAramResourceCache[16];
    /* 0xA0C */ J3DAnmTevRegKey* mpMagicArmorBodyBrk;
    /* 0xA10 */ J3DAnmTevRegKey* mpMagicArmorHeadBrk;
    /* 0xA14 */ mDoExt_bckAnm* mpMotionBck;
    /* 0xA18 */ BckCacheEntry mBckCache[48];
    /* 0xB98 */ u16 mMotionBckResId;
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
    std::array<int16_t, 10> mRemoteHatRotA;
    std::array<int16_t, 10> mRemoteHatRotB;
    std::array<int16_t, 3> mRemoteHatSwing;
    s16 mRemoteHatShapeY;
    /* 0xBD0 */ f32 mRemoteTransformFrame;
    /* 0xBD4 */ bool mRemoteTransformFrameValid;
    /* 0xBD8 */ u16 mRemoteEquipItem;
    /* 0xBDC */ int mRemoteSwordVariant;
    /* 0xBE0 */ int mRemoteShieldVariant;
    /* 0xBE4 */ int mLoadedSwordVariant;
    /* 0xBE8 */ int mLoadedShieldVariant;
    /* 0xBEC */ u16 mLoadedHeldItem;
    /* 0xBEE */ bool mRemoteSwordDraw;
    /* 0xBEF */ bool mRemoteShieldDraw;
    bool mRemoteShieldGuardActive;
    /* 0xBF0 */ bool mRemoteSwordOut;
    /* 0xBF1 */ bool mRemoteHeavyBoots;
    /* 0xBF2 */ bool mRemoteMidnaDraw;
    /* 0xBF3 */ bool mRemoteMidnaMaskDraw;
    /* 0xBF4 */ bool mRemoteMidnaHandDraw;
    /* 0xBF5 */ bool mRemoteMidnaHairDraw;
    /* 0xBF6 */ bool mRemoteMidnaShadowForm;
    /* 0xBF1 */ bool mHeldItemMatrixValid;
    /* 0xBF2 */ bool mHookTipMatrixValid;
    /* 0xBF3 */ bool mHookSubItemMatrixValid;
    /* 0xBF4 */ bool mHookSubTipMatrixValid;
    /* 0xBF5 */ bool mArrowMatrixValid;
    /* 0xBF6 */ bool mKanteraMatrixValid;
    /* 0xBF7 */ bool mKanteraGlowMatrixValid;
    /* 0xBF8 */ bool mItemActorMatrixValid;
    /* 0xBF9 */ bool mRideActorMatrixValid;
    /* 0xBFA */ bool mMidnaMatrixValid;
    /* 0xBFB */ bool mMidnaMaskMatrixValid;
    /* 0xBFC */ bool mMidnaHandMatrixValid;
    /* 0xBFD */ bool mMidnaHairMatrixValid;
    /* 0xBFE */ bool mMidnaGlowMatrixValid;
    /* 0xC04 */ bool mRemoteItemDraw;
    /* 0xC05 */ bool mRemoteKanteraDraw;
    /* 0xC06 */ bool mHasRemotePose;
    /* 0xC07 */ bool mHasRemoteMatrices;
    /* 0xC14 */ int mClothesVariant;
    /* 0xC08 */ J3DShape* mpLeftBodyHandShape;
    /* 0xC0C */ J3DShape* mpRightBodyHandShape;
    /* 0xC10 */ int mRemoteItemActorKind;
    /* 0xC14 */ int mRemoteRideActorKind;
    /* 0xC18 */ int mLoadedItemActorKind;
    /* 0xC1C */ int mLoadedRideActorKind;
    /* 0xC20 */ int mRemoteBombFlashTicks;
    int mRemoteBombExTime;
    int mRemoteBombFlash;
    bool mRemoteBombLastPosValid;
    bool mRemoteBombWasWater;
    bool mRemoteBombExplosionSpawned;
    fpc_ProcID mRemoteBombActorId;
    int mRemoteBombActorKind;
    s16 mRemoteBombAngleY;
    cXyz mRemoteBombLastPos;
    dusk::multiplayer::RemoteModelMatrixSnapshot mPrevBodyMatrixSnapshot;
    dusk::multiplayer::RemoteModelMatrixSnapshot mCurrBodyMatrixSnapshot;
    bool mPrevBodyMatrixSnapshotValid;
    bool mCurrBodyMatrixSnapshotValid;
    RemoteModelMatrixInterpState mFaceMatrixInterp;
    RemoteModelMatrixInterpState mHandMatrixInterp;
    RemoteModelMatrixInterpState mSwordMatrixInterp;
    RemoteModelMatrixInterpState mSheathMatrixInterp;
    RemoteModelMatrixInterpState mShieldMatrixInterp;
    RemoteModelMatrixInterpState mMidnaMatrixInterp;
    RemoteModelMatrixInterpState mMidnaMaskMatrixInterp;
    RemoteModelMatrixInterpState mMidnaHandMatrixInterp;
    RemoteModelMatrixInterpState mMidnaHairMatrixInterp;
    RemoteModelMatrixInterpState mMidnaGlowMatrixInterp;
    dCcD_Stts mPvpTargetStts;
    dCcD_Cyl mPvpTargetCyl;
    bool mPvpTargetCollisionInitialized;
    s16 mPvpShieldFrontAngle;
    std::array<u32, 3> mPvpMidnaBindIds;
    bool mPvpMidnaBindActive;
    Z2SoundObjSimple mActiveSoundObj;
    std::array<ActiveRemoteSound, 8> mActiveSounds;
    /* 0xC24 */ int mMidnaHairShape;
    /* 0xC28 */ bool mSlotReserved;
};

#endif /* D_A_REMOTE_LINK_H */
