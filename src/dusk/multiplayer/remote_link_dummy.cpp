#include "dusk/multiplayer/remote_link_dummy.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include "SSystem/SComponent/c_phase.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_remote_link.h"
#include "d/d_cc_d.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_resorce.h"
#include "dusk/frame_interpolation.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/J3DGraphLoader/J3DAnmLoader.h"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/JUtility/JUTNameTab.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"
#include "res/Object/AlAnm.h"
#include "res/Object/Alink.h"

namespace dusk::multiplayer {
namespace {

struct RemoteLinkModel {
    J3DModel* model = nullptr;
    J3DModelData* data = nullptr;
    JKRSolidHeap* heap = nullptr;
};

struct RemoteFaceAnim {
    J3DModelData* data = nullptr;
    void* btpBuffer = nullptr;
    void* btkBuffer = nullptr;
};

struct RemoteEquipmentInterp {
    J3DModel* model = nullptr;
    dKy_tevstr_c tevStr{};
    bool suppressMaterialAnm = true;
};

struct RemoteTransformAnim {
    bool active = false;
    bool fromWolf = false;
    bool toWolf = false;
    bool secondPhase = false;
    bool initialized = false;
    bool holdFinal = false;
    f32 frame = 0.0f;
    f32 endFrame = 0.0f;
    uint32_t holdTicks = 0;
    daPy_anmHeap_c* anmHeap = nullptr;
    mDoExt_bckAnm bck;
    JKRSolidHeap* heap = nullptr;
    RemoteLinkModel wfModel;
    void* wfModelBuffer = nullptr;
    void* wfBckBuffer = nullptr;
    J3DAnmTransform* wfBckData = nullptr;
    bool wfBckToWolf = false;
    mDoExt_bckAnm wfBck;
    JKRSolidHeap* wfHeap = nullptr;
    RemoteLinkModel changeModel;
};

struct RemoteLinkDummy {
    RemoteLinkModel body;
    RemoteLinkModel hat;
    RemoteLinkModel face;
    RemoteLinkModel hand;
    RemoteLinkModel sword;
    RemoteLinkModel sheath;
    RemoteLinkModel shield;
    std::string stage;
    int room = -128;
    uint32_t drawLogCount = 0;
    uint32_t matrixRejectLogCount = 0;
    uint32_t swordLogCount = 0;
    uint32_t wolfDrawLogCount = 0;
    uint32_t transformLogCount = 0;
    bool lastObservedIsWolf = false;
    bool hasObservedForm = false;
    bool remoteTransformingObserved = false;
    int lastObservedClothesVariant = -1;
    int lastObservedShieldVariant = -1;
    bool lastObservedShieldDraw = false;
    RemoteLinkMatrixSnapshot lastLinkMatrices;
    bool lastLinkMatricesIsWolf = false;
    bool loggedReady = false;
    dEyeHL_c humanEyeHL;
    dEyeHL_c wolfEyeHL;
    daAlink_matAnm_c* humanEyeAnm[2] = {nullptr, nullptr};
    daAlink_matAnm_c* wolfEyeAnm[2] = {nullptr, nullptr};
    J3DModelData* humanEyeData = nullptr;
    J3DModelData* wolfEyeData = nullptr;
    J3DModelData* humanEyeAnmData = nullptr;
    J3DModelData* wolfEyeAnmData = nullptr;
    J3DModelData* initializedHumanFaceData = nullptr;
    J3DModelData* initializedWolfBodyData = nullptr;
    RemoteFaceAnim humanFaceAnim;
    RemoteFaceAnim wolfFaceAnim;
    J3DModelData* initializedOrdonSwordData = nullptr;
    J3DModelData* initializedMasterSwordData = nullptr;
    RemoteEquipmentInterp swordInterp;
    RemoteEquipmentInterp sheathInterp;
    RemoteEquipmentInterp shieldInterp;
    RemoteTransformAnim transform;
    dCcD_Stts bodyCcStts;
    dCcD_Cyl bodyCyl;
    bool bodyCollisionInitialized = false;
};

struct RemoteLinkActorDummy {
    fpc_ProcID actorId = fpcM_ERROR_PROCESS_ID_e;
    uint32_t logCount = 0;
    int clothesVariant = -1;
    int pendingClothesVariant = -1;
    bool isWolf = false;
    bool pendingIsWolf = false;
    uint8_t recreateDelayTicks = 0;
    bool recreatePending = false;
};

struct RemoteArcRequest {
    const char* arcName = nullptr;
    request_of_phase_process_class phase{};
    bool complete = false;
    uint32_t logCount = 0;
};

struct RemoteLinkSources {
    J3DModelData* body = nullptr;
    J3DModelData* hat = nullptr;
    J3DModelData* face = nullptr;
    J3DModelData* hand = nullptr;
    J3DModelData* sword = nullptr;
    J3DModelData* sheath = nullptr;
    J3DModelData* shield = nullptr;
    bool humanParts = false;
};

// Keyed by PeerPoseSnapshot::peerId. Direct host sessions can draw multiple
// remote peers at once (direct1/direct2/etc.), so every clone/model cache and
// log throttle in RemoteLinkDummy must stay per-peer.
std::map<std::string, RemoteLinkDummy> sDummies;
std::map<std::string, RemoteLinkActorDummy> sActorDummies;
RemoteArcRequest sHumanArc{"Kmdl"};
RemoteArcRequest sWolfArc{"Wmdl"};
RemoteArcRequest sAlinkArc{"Alink"};
// Casual/Ordon clothes (daAlink_c::checkCasualWearFlg(), "Bmdl") -- a
// different body archive than Hero's Clothes ("Kmdl") with a different
// joint-weight-envelope count. Every fresh save starts here (before the
// intro puts Link in Hero's Clothes), so resolve_remote_sources() always
// assuming "Kmdl" rejected every peer body matrix with a weight-count
// mismatch for the entire early game. See RemoteClothesVariant
// (multiplayer.cpp).
RemoteArcRequest sCasualArc{"Bmdl"};
RemoteArcRequest sZoraArc{"Zmdl"};
RemoteArcRequest sMagicArmorArc{"Mmdl"};
RemoteArcRequest sCarvingWoodShieldArc{"CWShd"};
RemoteArcRequest sOrdonShieldArc{"SWShd"};
RemoteArcRequest sHylianShieldArc{"HyShd"};
uint32_t sRoomSkipLogCount = 0;
uint32_t sActorSyncLogCount = 0;

constexpr f32 kRemoteHumanBodyRadius = 35.0f;
constexpr f32 kRemoteHumanBodyHeight = 180.0f;
constexpr f32 kRemoteWolfBodyRadius = 35.0f;
constexpr f32 kRemoteWolfBodyHeight = 95.0f;
constexpr f32 kRemoteWolfBodyHalfLength = 45.0f;
constexpr f32 kLocalLinkBodyRadius = 35.0f;
constexpr f32 kRemoteTransformAnimSpeed = 1.0f;
constexpr uint32_t kRemoteTransformFinalHoldTicks = 180;

static dCcD_SrcCyl l_remoteLinkBodyCylSrc = {
    {
        {0, {{(u32)AT_TYPE_WOLF_ATTACK, 3, 0x1A}, {0xD8FFFDFF, 5}, 0x73}},
        {dCcD_SE_WOLF_BITE, 3, 1, 0, {1}},
        {dCcD_SE_NONE, 6, 0, 0, {0}},
        {0},
    },
    {
        {
            {0.0f, 0.0f, 0.0f},
            kRemoteHumanBodyRadius,
            kRemoteHumanBodyHeight,
        },
    }
};

RemoteLinkSources resolve_remote_sources(const PeerPoseSnapshot& pose);
void destroy_remote_link_actor_dummy(const std::string& peerId);
bool remote_link_actor_dummy_active(const std::string& peerId);
void prepare_remote_form_resources(RemoteLinkDummy& dummy, const PeerPoseSnapshot& pose,
                                   daAlink_c* link, J3DModel* body, J3DModel* hat,
                                   J3DModel* face, J3DModel* sword);
dKy_tevstr_c build_dummy_tev_str(const PeerPoseSnapshot& pose);
void entry_model_without_calc(J3DModel* model, dKy_tevstr_c* tevStr, bool rebindLight,
                              bool useDarkList, bool suppressMaterialAnm,
                              bool entryOnNonSimFrame);
void log_draw_skip(RemoteLinkDummy& dummy, const char* reason, const std::string& peerId,
                   const PeerPoseSnapshot& pose, const RemoteLinkSources& sources,
                   J3DModel* body, bool localIsWolf, const RemoteLinkMatrixSnapshot* matrices,
                   bool usingCachedMatrices);
void* load_link_anm(u16 resId, u32 bufferSize, void** outBuffer);

// Temporary experiment for the "yellow sword affects the real local
// character too" report: g_env_light.setLightTevColorType_MAJI() (called
// from entry_model_without_calc() below) writes AmbColor/TevColor/
// TevKColor/Light/Fog directly onto the material objects living on the
// SHARED J3DModelData the sword was cloned from (confirmed by reading its
// implementation, d_kankyo.cpp:4535 -- it takes a J3DModelData*, not a
// per-instance object, and none of those writes are ever restored). If
// disabling just the remote sword's draw call stops the LOCAL player's own
// sword from turning yellow, that confirms this call is the corruption
// source. DUSK_MP_SKIP_REMOTE_SWORD=1 skips only the sword's draw (model
// creation/matrix copy still happen so sheath/shield positioning logic is
// unaffected); body/sheath/shield continue drawing normally.
bool skip_remote_sword_draw() {
    static const bool sSkip = [] {
        const char* value = std::getenv("DUSK_MP_SKIP_REMOTE_SWORD");
        return value != nullptr &&
               (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
    }();
    return sSkip;
}

bool ensure_arc_loaded(RemoteArcRequest& request) {
    if (request.complete) {
        return true;
    }

    const int phase = dComIfG_resLoad(&request.phase, request.arcName);
    // Diagnostic for the "Epona never spawns, permanently stuck at
    // cPhs_INIT_e" report (see daHorse_c::create()'s matching diagnostic,
    // d_a_horse.cpp): this preload runs every multiplayer tick and competes
    // for the same dComIfG_resLoad phase machinery every other actor's
    // archive load uses. Logged every call while not complete so it can be
    // frame-correlated against the horse's own stuck-load log in the same
    // file -- if both are stuck at the same time, that's contention; if
    // this resolves quickly while the horse stays stuck, it isn't.
    if (phase != cPhs_COMPLEATE_e) {
        DuskLog.warn(
            "Multiplayer remote Link dummy: resource arc {} not yet loaded phase={} "
            "(0=INIT 1=LOADING 4=COMPLEATE 5=ERROR)",
            request.arcName, phase);
    }
    if (phase == cPhs_COMPLEATE_e) {
        request.complete = true;
        if (request.logCount < 5) {
            ++request.logCount;
            DuskLog.info("Multiplayer remote Link dummy: resource arc {} loaded",
                         request.arcName);
        }
        return true;
    }

    if (phase == cPhs_ERROR_e && request.logCount < 5) {
        ++request.logCount;
        DuskLog.warn("Multiplayer remote Link dummy: resource arc {} failed to load",
                     request.arcName);
    }

    return false;
}

bool is_arc_ready(const RemoteArcRequest& request) {
    return request.complete;
}

void destroy_model(RemoteLinkModel& model) {
    if (model.heap != nullptr) {
        mDoExt_destroySolidHeap(model.heap);
    }

    model.model = nullptr;
    model.data = nullptr;
    model.heap = nullptr;
}

void destroy_transform_anim(RemoteTransformAnim& anim) {
    destroy_model(anim.changeModel);
    destroy_model(anim.wfModel);
    if (anim.wfBckBuffer != nullptr) {
        JKRFree(anim.wfBckBuffer);
    }
    if (anim.wfModelBuffer != nullptr) {
        JKRFree(anim.wfModelBuffer);
    }
    if (anim.wfHeap != nullptr) {
        mDoExt_destroySolidHeap(anim.wfHeap);
    }
    if (anim.anmHeap != nullptr) {
        if (anim.anmHeap->getBuffer() != nullptr) {
            JKR_DELETE_ARRAY(anim.anmHeap->getBuffer());
        }
        JKR_DELETE(anim.anmHeap);
    }
    if (anim.heap != nullptr) {
        mDoExt_destroySolidHeap(anim.heap);
    }

    anim = {};
}

J3DModel* get_or_create_model_from_data(RemoteLinkModel& dst, J3DModelData* sourceData,
                                        u32 heapSize, u32 differedDlistFlag = 0x11000084) {
    if (sourceData == nullptr) {
        destroy_model(dst);
        return nullptr;
    }

    if (dst.model != nullptr && dst.data == sourceData) {
        return dst.model;
    }

    destroy_model(dst);

    dst.heap = mDoExt_createSolidHeapFromGameToCurrent(heapSize, 0x20);
    if (dst.heap == nullptr) {
        DuskLog.warn("Multiplayer remote Link dummy: heap allocation failed size={}", heapSize);
        return nullptr;
    }

    dst.model = mDoExt_J3DModel__create(sourceData, 0x80000, differedDlistFlag);
    if (dst.model == nullptr) {
        DuskLog.warn("Multiplayer remote Link dummy: J3DModel creation from data failed");
        mDoExt_destroySolidHeap(dst.heap);
        dst.heap = nullptr;
        mDoExt_restoreCurrentHeap();
        return nullptr;
    }

    dst.data = sourceData;
    dst.model->setBaseScale(cXyz(1.0f, 1.0f, 1.0f));
    dst.model->setUserArea(0);
    mDoExt_adjustSolidHeapToSystem(dst.heap);
    DuskLog.info("Multiplayer remote Link dummy: cloned resource model={} joints={}",
                 static_cast<void*>(dst.model), sourceData->getJointNum());
    return dst.model;
}

void flat_matrix_to_mtx(const float* values, Mtx outMtx) {
    size_t cursor = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            outMtx[row][col] = values[cursor++];
        }
    }
}

bool copy_remote_model_matrices(J3DModel* dst, const RemoteModelMatrixSnapshot& source) {
    if (dst == nullptr || dst->getModelData() == nullptr || !source.valid) {
        return false;
    }

    J3DModelData* data = dst->getModelData();
    if (source.jointCount != data->getJointNum() ||
        source.weightCount != data->getWEvlpMtxNum() ||
        source.joints.size() != static_cast<size_t>(source.jointCount) * 12 ||
        source.weights.size() != static_cast<size_t>(source.weightCount) * 12)
    {
        return false;
    }

    Mtx mtx;
    flat_matrix_to_mtx(source.base.data(), mtx);
    dst->setBaseTRMtx(mtx);

    for (u16 i = 0; i < source.jointCount; ++i) {
        flat_matrix_to_mtx(&source.joints[static_cast<size_t>(i) * 12], mtx);
        dst->setAnmMtx(i, mtx);
    }

    for (u16 i = 0; i < source.weightCount; ++i) {
        flat_matrix_to_mtx(&source.weights[static_cast<size_t>(i) * 12], mtx);
        mDoMtx_copy(mtx, dst->getWeightAnmMtx(i));
    }

    return true;
}

u16 remote_transform_bck_res_id(const RemoteTransformAnim& anim) {
    if (anim.toWolf) {
        return anim.secondPhase ? dRes_ID_ALANM_BCK_WL_CHANGEATOW_e :
                                  dRes_ID_ALANM_BCK_CHANGEATOW_e;
    }

    return anim.secondPhase ? dRes_ID_ALANM_BCK_CHANGEWTOA_e :
                              dRes_ID_ALANM_BCK_WL_CHANGEWTOA_e;
}

bool remote_transform_display_is_wolf(const RemoteTransformAnim& anim) {
    return anim.secondPhase ? anim.toWolf : anim.fromWolf;
}

void start_remote_transform(RemoteLinkDummy& dummy, bool fromWolf, bool toWolf) {
    dummy.transformLogCount = 0;
    RemoteTransformAnim& anim = dummy.transform;
    anim.active = true;
    anim.fromWolf = fromWolf;
    anim.toWolf = toWolf;
    anim.secondPhase = false;
    anim.frame = 0.0f;
    anim.endFrame = 0.0f;
    anim.initialized = false;
    anim.holdFinal = false;
    anim.holdTicks = 0;
}

bool init_remote_transform_phase(RemoteTransformAnim& anim) {
    const u16 resId = remote_transform_bck_res_id(anim);
    if (anim.anmHeap == nullptr) {
        anim.anmHeap = JKR_NEW daPy_anmHeap_c(0x10800);
        if (anim.anmHeap == nullptr) {
            DuskLog.warn("Multiplayer remote Link dummy: transform animation data heap allocation failed");
            return false;
        }
        if (anim.anmHeap->mallocBuffer() == nullptr) {
            DuskLog.warn("Multiplayer remote Link dummy: transform animation data buffer allocation failed");
            JKR_DELETE(anim.anmHeap);
            anim.anmHeap = nullptr;
            return false;
        }
        anim.anmHeap->createHeap(daPy_anmHeap_c::HEAP_TYPE_3, "remote_transform_anm");
    }

    anim.anmHeap->setBufferSize(0x10800);
    anim.anmHeap->resetIdx();
    anim.anmHeap->resetArcNo();
    J3DAnmTransform* bck = static_cast<J3DAnmTransform*>(anim.anmHeap->loadDataIdx(resId));
    if (bck == nullptr) {
        DuskLog.info(
            "Multiplayer transform debug anim_phase_init_failed reason=bck_null res_id={} "
            "from_wolf={} to_wolf={} second_phase={}",
            resId, anim.fromWolf, anim.toWolf, anim.secondPhase);
        return false;
    }

    if (anim.heap == nullptr) {
        anim.heap = mDoExt_createSolidHeapFromGameToCurrent(0x3000, 0x20);
        if (anim.heap == nullptr) {
            DuskLog.warn("Multiplayer remote Link dummy: transform animation heap allocation failed");
            return false;
        }

        if (!anim.bck.init(bck, TRUE, J3DFrameCtrl::EMode_NONE, kRemoteTransformAnimSpeed, 0, -1, false)) {
            DuskLog.warn("Multiplayer remote Link dummy: transform animation init failed");
            mDoExt_destroySolidHeap(anim.heap);
            anim.heap = nullptr;
            mDoExt_restoreCurrentHeap();
            return false;
        }

        mDoExt_adjustSolidHeapToSystem(anim.heap);
    } else {
        anim.bck.changeBckOnly(bck);
        anim.bck.setPlayMode(J3DFrameCtrl::EMode_NONE);
        anim.bck.setPlaySpeed(kRemoteTransformAnimSpeed);
        anim.bck.setFrame(0.0f);
    }

    anim.frame = 0.0f;
    anim.endFrame = bck->getFrameMax();
    anim.initialized = true;
    return true;
}

void set_remote_transform_base_matrix(J3DModel* model, const PeerPoseSnapshot& pose) {
    mDoMtx_stack_c::transS(pose.x, pose.y, pose.z);
    mDoMtx_stack_c::ZXYrotM(static_cast<s16>(pose.transformShapeX),
                            static_cast<s16>(pose.angleY), 0);
    model->setBaseTRMtx(mDoMtx_stack_c::get());
}

struct JointCallbackSnapshot {
    std::vector<std::pair<J3DJoint*, J3DJointCallBack>> entries;
};

struct ShapeVisibilitySnapshot {
    std::vector<std::pair<J3DShape*, bool>> shapes;
};

struct SwordShapeVisibility {
    J3DShape* shape = nullptr;
    bool wasHidden = false;
};

ShapeVisibilitySnapshot show_model_shapes(J3DModel* model);
void restore_model_shapes(const ShapeVisibilitySnapshot& snapshot);
bool is_wood_sword_variant(int swordVariant);
SwordShapeVisibility apply_sword_shape_visibility(J3DModel* sword, bool swordOut, bool woodSword);
void restore_sword_shape_visibility(const SwordShapeVisibility& snapshot);

JointCallbackSnapshot suppress_joint_callbacks(J3DModelData* data) {
    JointCallbackSnapshot snapshot;
    if (data == nullptr) {
        return snapshot;
    }

    const u16 jointCount = data->getJointNum();
    snapshot.entries.reserve(jointCount);
    for (u16 i = 0; i < jointCount; ++i) {
        J3DJoint* joint = data->getJointNodePointer(i);
        if (joint == nullptr) {
            continue;
        }

        J3DJointCallBack callback = joint->getCallBack();
        if (callback != nullptr) {
            snapshot.entries.emplace_back(joint, callback);
            joint->setCallBack(nullptr);
        }
    }

    return snapshot;
}

void restore_joint_callbacks(const JointCallbackSnapshot& snapshot) {
    for (const auto& entry : snapshot.entries) {
        if (entry.first != nullptr) {
            entry.first->setCallBack(entry.second);
        }
    }
}

void calc_model_suppressing_joint_callbacks(J3DModel* model) {
    if (model == nullptr || model->getModelData() == nullptr) {
        return;
    }

    JointCallbackSnapshot jointCallbacks = suppress_joint_callbacks(model->getModelData());
    model->calc();
    restore_joint_callbacks(jointCallbacks);
}

void entry_remote_transform_model(J3DModel* model, dKy_tevstr_c* tevStr, bool useDarkList) {
    ShapeVisibilitySnapshot visibility = show_model_shapes(model);
    entry_model_without_calc(model, tevStr, /*rebindLight=*/true, useDarkList,
                             /*suppressMaterialAnm=*/true, /*entryOnNonSimFrame=*/true);
    restore_model_shapes(visibility);
}

void entry_remote_transform_equipment(J3DModel* model, dKy_tevstr_c* tevStr) {
    ShapeVisibilitySnapshot visibility = show_model_shapes(model);
    entry_model_without_calc(model, tevStr, /*rebindLight=*/true, /*useDarkList=*/false,
                             /*suppressMaterialAnm=*/true, /*entryOnNonSimFrame=*/false);
    restore_model_shapes(visibility);
}

J3DModelData* load_remote_transform_bmd(u16 resId, u32 bufferSize, void** outBuffer) {
    if (outBuffer == nullptr) {
        return nullptr;
    }
    if (*outBuffer == nullptr) {
        *outBuffer = JKRAlloc(bufferSize, 0x20);
        if (*outBuffer == nullptr) {
            return nullptr;
        }
        JKRReadIdxResource(*outBuffer, bufferSize, resId, dComIfGp_getAnmArchive());
    }
    return dRes_info_c::loaderBasicBmd('BMWR', *outBuffer);
}

bool ensure_remote_transform_wf(RemoteTransformAnim& anim, bool toWolf) {
    if (anim.wfModel.data == nullptr) {
        J3DModelData* data =
            load_remote_transform_bmd(dRes_ID_ALANM_BMD_AL_WF_e, 0x6000, &anim.wfModelBuffer);
        if (data == nullptr) {
            return false;
        }
        J3DModel* model = get_or_create_model_from_data(anim.wfModel, data, 0x100000);
        if (model == nullptr) {
            return false;
        }
    }

    const u16 bckResId = toWolf ? dRes_ID_ALANM_BCK_WFCHANGEATOW_e :
                                  dRes_ID_ALANM_BCK_WFCHANGEWTOA_e;
    if (anim.wfBckData == nullptr || anim.wfBck.getBckAnm() == nullptr ||
        anim.wfBckToWolf != toWolf)
    {
        if (anim.wfBckBuffer != nullptr && anim.wfBckToWolf != toWolf) {
            JKRFree(anim.wfBckBuffer);
            anim.wfBckBuffer = nullptr;
            anim.wfBckData = nullptr;
        }
        anim.wfBckData = static_cast<J3DAnmTransform*>(load_link_anm(bckResId, 0x1400,
                                                                      &anim.wfBckBuffer));
        if (anim.wfBckData == nullptr) {
            return false;
        }
        if (anim.wfHeap == nullptr) {
            anim.wfHeap = mDoExt_createSolidHeapFromGameToCurrent(0x1000, 0x20);
            if (anim.wfHeap == nullptr) {
                return false;
            }
            if (!anim.wfBck.init(anim.wfBckData, FALSE, J3DFrameCtrl::EMode_NONE,
                                 kRemoteTransformAnimSpeed, 0, -1, false))
            {
                mDoExt_destroySolidHeap(anim.wfHeap);
                anim.wfHeap = nullptr;
                return false;
            }
            mDoExt_adjustSolidHeapToSystem(anim.wfHeap);
        } else {
            anim.wfBck.changeBckOnly(anim.wfBckData);
            anim.wfBck.setPlayMode(J3DFrameCtrl::EMode_NONE);
            anim.wfBck.setPlaySpeed(kRemoteTransformAnimSpeed);
            anim.wfBck.setFrame(0.0f);
        }
        anim.wfBckToWolf = toWolf;
    }

    return true;
}

void draw_remote_transform_wf(RemoteTransformAnim& anim, J3DModel* body,
                              dKy_tevstr_c* tevStr) {
    if (body == nullptr || anim.wfModel.model == nullptr ||
        anim.wfModel.model->getModelData() == nullptr)
    {
        return;
    }

    J3DModel* model = anim.wfModel.model;
    model->setBaseTRMtx(body->getAnmMtx(4));
    anim.wfBck.entry(model->getModelData(), anim.frame);
    calc_model_suppressing_joint_callbacks(model);
    anim.wfBck.remove(model->getModelData());
    entry_remote_transform_model(model, tevStr, false);
}

J3DModel* ensure_remote_transform_change_model(RemoteTransformAnim& anim) {
    J3DModelData* data = static_cast<J3DModelData*>(
        dComIfG_getObjectRes("Alink", dRes_ID_ALINK_BMD_WL_CHANGE_e));
    if (data == nullptr) {
        return nullptr;
    }
    return get_or_create_model_from_data(anim.changeModel, data, 0x100000);
}

void advance_remote_transform_anim(RemoteTransformAnim& anim, RemoteLinkDummy& dummy,
                                   const std::string& peerId,
                                   const PeerPoseSnapshot& pose) {
    if (anim.holdFinal) {
        ++anim.holdTicks;
    } else {
        anim.frame += kRemoteTransformAnimSpeed;
    }
    if (!anim.holdFinal && anim.frame >= anim.endFrame) {
        if (!anim.secondPhase) {
            anim.secondPhase = true;
            anim.initialized = false;
            if (dummy.transformLogCount < 20) {
                ++dummy.transformLogCount;
                DuskLog.info("Multiplayer transform debug draw_transform_phase2 peer={}", peerId);
            }
        } else {
            anim.holdFinal = true;
            anim.holdTicks = 0;
            if (anim.endFrame > 1.0f) {
                anim.frame = anim.endFrame - 1.0f;
            } else {
                anim.frame = 0.0f;
            }
            if (dummy.transformLogCount < 20) {
                ++dummy.transformLogCount;
                DuskLog.info(
                    "Multiplayer transform debug draw_transform_hold peer={} target_wolf={} "
                    "pose_mtx_valid={} peer_wolf={}",
                    peerId, anim.toWolf, pose.linkMatrices.valid, pose.isWolf);
            }
        }
    }
}

bool draw_remote_transform_body(RemoteLinkDummy& dummy, const std::string& peerId,
                                const PeerPoseSnapshot& pose, daAlink_c* link,
                                bool localIsWolf) {
    RemoteTransformAnim& anim = dummy.transform;
    if (!anim.active) {
        return false;
    }

    const bool targetMatricesReady = pose.linkMatrices.valid && pose.isWolf == anim.toWolf;
    if (targetMatricesReady) {
        anim.active = false;
        if (dummy.transformLogCount < 24) {
            ++dummy.transformLogCount;
            DuskLog.info(
                "Multiplayer transform debug draw_transform_handoff peer={} target_wolf={} "
                "pose_mtx_valid={} peer_wolf={}",
                peerId, anim.toWolf, pose.linkMatrices.valid, pose.isWolf);
        }
        return false;
    }

    if (anim.holdFinal && anim.holdTicks >= kRemoteTransformFinalHoldTicks) {
        anim.active = false;
        if (dummy.transformLogCount < 24) {
            ++dummy.transformLogCount;
            DuskLog.info(
                "Multiplayer transform debug draw_transform_hold_timeout peer={} target_wolf={} "
                "pose_mtx_valid={} peer_wolf={}",
                peerId, anim.toWolf, pose.linkMatrices.valid, pose.isWolf);
        }
        return false;
    }

    if (!anim.initialized && !init_remote_transform_phase(anim)) {
        if (dummy.transformLogCount < 20) {
            ++dummy.transformLogCount;
            DuskLog.info(
                "Multiplayer transform debug draw_transform_failed peer={} reason=phase_init "
                "from_wolf={} to_wolf={} second_phase={}",
                peerId, anim.fromWolf, anim.toWolf, anim.secondPhase);
        }
        anim.active = false;
        return false;
    }

    PeerPoseSnapshot displayPose = pose;
    displayPose.isWolf = remote_transform_display_is_wolf(anim);
    if (anim.holdFinal) {
        displayPose.isWolf = anim.toWolf;
        if (anim.endFrame > 1.0f) {
            anim.frame = anim.endFrame - 1.0f;
        } else {
            anim.frame = 0.0f;
        }
    }
    displayPose.swordDraw = !displayPose.isWolf && pose.swordDraw;
    displayPose.shieldDraw = !displayPose.isWolf && pose.shieldDraw;
    displayPose.swordOut = !displayPose.isWolf && pose.swordOut;

    dKy_tevstr_c dummyTevStr = build_dummy_tev_str(displayPose);
    if (pose.isTransforming) {
        dummyTevStr.TevColor.r = pose.transformProcVar3;
        dummyTevStr.TevColor.g = pose.transformProcVar3;
        dummyTevStr.TevColor.b = pose.transformProcVar3;
    }
    const bool unsafeHumanTransformFallback =
        pose.isTransforming && !displayPose.isWolf && !targetMatricesReady;
    if (unsafeHumanTransformFallback && !anim.fromWolf && !anim.secondPhase &&
        dummy.lastLinkMatrices.valid && !dummy.lastLinkMatricesIsWolf)
    {
        if (dummy.transformLogCount < 24) {
            ++dummy.transformLogCount;
            DuskLog.info(
                "Multiplayer transform debug hold_cached_human peer={} frame={} "
                "sender_wolf={} proc_v5={} cache_wolf={}",
                peerId, anim.frame, pose.isWolf, pose.transformProcVar5,
                dummy.lastLinkMatricesIsWolf);
        }
        advance_remote_transform_anim(anim, dummy, peerId, pose);
        return false;
    }

    const bool drawChangeBridge =
        !anim.holdFinal &&
        ((anim.secondPhase && anim.frame < 4.0f) || unsafeHumanTransformFallback);
    if (drawChangeBridge) {
        J3DModel* changeModel = ensure_remote_transform_change_model(anim);
        if (changeModel != nullptr) {
            set_remote_transform_base_matrix(changeModel, pose);
            if (!anim.toWolf) {
                mDoMtx_stack_c::copy(changeModel->getBaseTRMtx());
                mDoMtx_stack_c::transM(0.0f, 0.0f, 30.0f);
                changeModel->setBaseTRMtx(mDoMtx_stack_c::get());
            }
            calc_model_suppressing_joint_callbacks(changeModel);
            entry_remote_transform_model(changeModel, &dummyTevStr, displayPose.isWolf);
            if (dummy.transformLogCount < 24) {
                ++dummy.transformLogCount;
                DuskLog.info(
                    "Multiplayer transform debug draw_transform_change_bridge peer={} "
                    "frame={} to_wolf={} proc_v0={} proc_v5={} clothes_wait={} model={}",
                    peerId, anim.frame, anim.toWolf, pose.transformProcVar0,
                    pose.transformProcVar5, pose.transformClothesWait,
                    static_cast<void*>(changeModel));
            }
            advance_remote_transform_anim(anim, dummy, peerId, pose);
            return true;
        }
    }

    const RemoteLinkSources sources = resolve_remote_sources(displayPose);
    const u32 bodyDiffFlags = displayPose.isWolf ? 0x11020284 : 0x11000084;
    J3DModel* body = get_or_create_model_from_data(dummy.body, sources.body, 0x200000, bodyDiffFlags);
    const bool drawHumanParts = !displayPose.isWolf && sources.humanParts;
    J3DModel* hat = drawHumanParts ? get_or_create_model_from_data(dummy.hat, sources.hat, 0x100000) : nullptr;
    J3DModel* face = drawHumanParts ? get_or_create_model_from_data(dummy.face, sources.face, 0x100000) : nullptr;
    J3DModel* hand = drawHumanParts ? get_or_create_model_from_data(dummy.hand, sources.hand, 0x100000) : nullptr;
    J3DModel* sword =
        drawHumanParts && displayPose.swordDraw ?
            get_or_create_model_from_data(dummy.sword, sources.sword, 0x100000) :
            nullptr;
    J3DModel* sheath =
        drawHumanParts && displayPose.swordDraw ?
            get_or_create_model_from_data(dummy.sheath, sources.sheath, 0x100000) :
            nullptr;
    J3DModel* shield =
        drawHumanParts && displayPose.shieldDraw ?
            get_or_create_model_from_data(dummy.shield, sources.shield, 0x100000) :
            nullptr;
    if (dummy.transformLogCount < 20) {
        ++dummy.transformLogCount;
            DuskLog.info(
                "Multiplayer transform debug draw_transform peer={} frame={} end={} from_wolf={} "
            "to_wolf={} second_phase={} display_wolf={} shape_x={} proc_v2={} proc_v3={} "
            "source_body={} body={} hat={} face={} hand={}",
            peerId, anim.frame, anim.endFrame, anim.fromWolf, anim.toWolf, anim.secondPhase,
            displayPose.isWolf, pose.transformShapeX, pose.transformProcVar2,
            pose.transformProcVar3, static_cast<void*>(sources.body),
            static_cast<void*>(body), static_cast<void*>(hat), static_cast<void*>(face),
            static_cast<void*>(hand));
    }
    prepare_remote_form_resources(dummy, displayPose, link, body, hat, face, sword);
    if (!drawHumanParts) {
        destroy_model(dummy.hat);
        destroy_model(dummy.face);
        destroy_model(dummy.hand);
    }
    if (!drawHumanParts || !displayPose.swordDraw) {
        destroy_model(dummy.sword);
        destroy_model(dummy.sheath);
    }
    if (!drawHumanParts || !displayPose.shieldDraw) {
        destroy_model(dummy.shield);
    }

    if (body == nullptr || body->getModelData() == nullptr) {
        log_draw_skip(dummy, "transform_body_null", peerId, displayPose, sources, body, localIsWolf,
                      nullptr, false);
        anim.active = false;
        return false;
    }

    const bool hasCachedHumanMatrices =
        dummy.lastLinkMatrices.valid && !dummy.lastLinkMatricesIsWolf;
    set_remote_transform_base_matrix(body, pose);
    anim.bck.entry(body->getModelData(), anim.frame);
    calc_model_suppressing_joint_callbacks(body);
    anim.bck.remove(body->getModelData());
    entry_remote_transform_model(body, &dummyTevStr, displayPose.isWolf);
    if (!anim.holdFinal && ensure_remote_transform_wf(anim, anim.toWolf)) {
        draw_remote_transform_wf(anim, body, &dummyTevStr);
    }
    if (drawHumanParts) {
        if (face != nullptr) {
            face->setBaseTRMtx(body->getAnmMtx(4));
            calc_model_suppressing_joint_callbacks(face);
            entry_remote_transform_model(face, &dummyTevStr, false);
        }
        if (hat != nullptr) {
            hat->setBaseTRMtx(body->getAnmMtx(4));
            calc_model_suppressing_joint_callbacks(hat);
            entry_remote_transform_model(hat, &dummyTevStr, false);
        }
        if (hand != nullptr) {
            hand->setBaseTRMtx(body->getBaseTRMtx());
            calc_model_suppressing_joint_callbacks(hand);
            hand->setAnmMtx(1, body->getAnmMtx(9));
            hand->setAnmMtx(2, body->getAnmMtx(0xE));
            entry_remote_transform_model(hand, &dummyTevStr, false);
        }
        if (sword != nullptr && hasCachedHumanMatrices && dummy.lastLinkMatrices.sword.valid &&
            !skip_remote_sword_draw())
        {
            copy_remote_model_matrices(sword, dummy.lastLinkMatrices.sword);
            const bool woodSword = is_wood_sword_variant(displayPose.swordVariant);
            SwordShapeVisibility visibility =
                apply_sword_shape_visibility(sword, displayPose.swordOut, woodSword);
            entry_remote_transform_equipment(sword, &dummyTevStr);
            restore_sword_shape_visibility(visibility);
        }
        if (sheath != nullptr && hasCachedHumanMatrices && dummy.lastLinkMatrices.sheath.valid) {
            copy_remote_model_matrices(sheath, dummy.lastLinkMatrices.sheath);
            entry_remote_transform_equipment(sheath, &dummyTevStr);
        }
        if (shield != nullptr && hasCachedHumanMatrices && dummy.lastLinkMatrices.shield.valid) {
            copy_remote_model_matrices(shield, dummy.lastLinkMatrices.shield);
            entry_remote_transform_equipment(shield, &dummyTevStr);
        }
    }

    advance_remote_transform_anim(anim, dummy, peerId, pose);

    return true;
}

void log_draw_skip(RemoteLinkDummy& dummy, const char* reason, const std::string& peerId,
                   const PeerPoseSnapshot& pose, const RemoteLinkSources& sources,
                   J3DModel* body, bool localIsWolf, const RemoteLinkMatrixSnapshot* matrices,
                   bool usingCachedMatrices) {
    if (dummy.matrixRejectLogCount >= 8) {
        return;
    }

    ++dummy.matrixRejectLogCount;
    DuskLog.info(
        "Multiplayer remote Link dummy: draw skip reason={} peer={} peer_wolf={} "
        "local_wolf={} "
        "pose_mtx_valid={} cache_valid={} cache_wolf={} using_cache={} "
        "draw_path_reached=false selected_body_model_null={} "
        "selected_body_resource_null={} wolf_arc_complete={} "
        "src_body_null={} body_null={} human_parts={} clothes_variant={} "
        "body_joints={} body_weights={} "
        "mtx_body_valid={} mtx_body_joints={} mtx_body_weights={} "
        "src_hat_null={} src_face_null={} src_hand_null={} src_sword_null={} "
        "src_sheath_null={} src_shield_null={}",
        reason, peerId, pose.isWolf, localIsWolf, pose.linkMatrices.valid, dummy.lastLinkMatrices.valid,
        dummy.lastLinkMatricesIsWolf, usingCachedMatrices, body == nullptr,
        sources.body == nullptr, sWolfArc.complete, sources.body == nullptr,
        body == nullptr, sources.humanParts, pose.clothesVariant,
        body != nullptr && body->getModelData() != nullptr ? body->getModelData()->getJointNum() : 0,
        body != nullptr && body->getModelData() != nullptr ? body->getModelData()->getWEvlpMtxNum() : 0,
        matrices != nullptr && matrices->body.valid,
        matrices != nullptr ? matrices->body.jointCount : 0,
        matrices != nullptr ? matrices->body.weightCount : 0,
        sources.hat == nullptr, sources.face == nullptr, sources.hand == nullptr,
        sources.sword == nullptr, sources.sheath == nullptr, sources.shield == nullptr);
}

void log_draw_reached(RemoteLinkDummy& dummy, const std::string& peerId,
                      const PeerPoseSnapshot& pose, const RemoteLinkSources& sources,
                      J3DModel* body, bool localIsWolf, const RemoteLinkMatrixSnapshot* matrices,
                      bool usingCachedMatrices) {
    if (pose.isWolf) {
        if (dummy.wolfDrawLogCount >= 8) {
            return;
        }
        ++dummy.wolfDrawLogCount;
    } else if (dummy.drawLogCount >= 8) {
        return;
    } else {
        ++dummy.drawLogCount;
    }

    DuskLog.info(
        "Multiplayer remote Link dummy: draw state reason=body_drawn peer={} peer_wolf={} "
        "local_wolf={} "
        "pose_mtx_valid={} cache_valid={} cache_wolf={} using_cache={} "
        "draw_path_reached=true selected_body_model_null={} "
        "selected_body_resource_null={} wolf_arc_complete={} clothes_variant={} "
        "body_weights={} "
        "mtx_body_valid={} mtx_body_joints={} mtx_body_weights={}",
        peerId, pose.isWolf, localIsWolf, pose.linkMatrices.valid, dummy.lastLinkMatrices.valid,
        dummy.lastLinkMatricesIsWolf, usingCachedMatrices, body == nullptr,
        sources.body == nullptr, sWolfArc.complete, pose.clothesVariant,
        body != nullptr && body->getModelData() != nullptr ? body->getModelData()->getWEvlpMtxNum() : 0,
        matrices != nullptr && matrices->body.valid,
        matrices != nullptr ? matrices->body.jointCount : 0,
        matrices != nullptr ? matrices->body.weightCount : 0);
}

// J3DModel::calcMaterial() (J3DModel.cpp:267) calls
// material->getMaterialAnm()->calc(material) for any material that has one
// bound. Those animator objects (e.g. field_0x2180 in d_a_alink_wolf.inc,
// bound via J3DMaterial::setMaterialAnm() onto a material node that lives
// on the SHARED J3DModelData, same as the J3DShape sword-visibility issue
// above) are owned and allocated by the real local daAlink_c, and are freed
// along with the rest of that form's resources whenever the local player
// is not currently in that form (changeWolf()/changeLink() free what the
// new form doesn't need). A real crash was confirmed in calcMaterial() at
// exactly this call (material->getMaterialAnm()->calc(...)) while drawing
// a peer's human-form sword/shield clone -- which shares its model data
// archive with the local player's own human-form resources -- while the
// local player was currently in wolf form, i.e. with those human-form
// animator objects already freed and the material node's now-dangling
// pointer to one of them never cleared by anything else, since the real
// actor has no reason to touch it while not in that form. This snapshots
// and clears every material's animator before calcMaterial() runs and
// restores it immediately after, so the dummy never dereferences a
// pointer the real engine isn't currently guaranteeing is valid.
//
// This is applied unconditionally, even when the peer's form matches the
// local player's own. An earlier version only suppressed on mismatch, on
// the theory that a matched archive's animator objects are owned by the
// local player's own currently-active daAlink_c, so they can't be dangling.
// That's true for the crash specifically, but reading/writing the SAME
// shared animator object from both the real local draw and this dummy's
// draw within the same frame produced new symptoms instead (flickering
// between two states, a neon color glitch on the sword, eyes intermittently
// disappearing) -- the animator and the material's own TEV/pattern
// registers it writes are genuinely shared state, not just a dangling-
// pointer risk. Suppressing it unconditionally trades that instability for
// a known, stable cosmetic gap (no blink animation, flat material color on
// the dummy) until the dummy can own fully independent material data
// instead of sharing the archive's J3DModelData at all -- a larger
// structural change, not a per-call guard.
struct MaterialAnmSnapshot {
    std::vector<std::pair<J3DMaterial*, J3DMaterialAnm*>> entries;
};

bool shares_active_local_link_model_data(J3DModel* model, const daAlink_c* link);

MaterialAnmSnapshot suppress_material_anm(J3DModel* model) {
    MaterialAnmSnapshot snapshot;
    if (model == nullptr || model->getModelData() == nullptr) {
        return snapshot;
    }

    J3DModelData* data = model->getModelData();
    const u16 matNum = data->getMaterialNum();
    for (u16 i = 0; i < matNum; ++i) {
        J3DMaterial* material = data->getMaterialNodePointer(i);
        if (material != nullptr && material->getMaterialAnm() != nullptr) {
            snapshot.entries.emplace_back(material, material->getMaterialAnm());
            material->setMaterialAnm(nullptr);
        }
    }
    return snapshot;
}

void restore_material_anm(const MaterialAnmSnapshot& snapshot) {
    for (const auto& entry : snapshot.entries) {
        entry.first->setMaterialAnm(entry.second);
    }
}

void remote_equipment_interp_callback(bool isSimFrame, void* userWork) {
    if (isSimFrame || userWork == nullptr) {
        return;
    }

    RemoteEquipmentInterp* work = static_cast<RemoteEquipmentInterp*>(userWork);
    if (work->model == nullptr) {
        return;
    }

    g_env_light.setLightTevColorType_MAJI(work->model, &work->tevStr);
    MaterialAnmSnapshot anmSnapshot = work->suppressMaterialAnm ? suppress_material_anm(work->model)
                                                                : MaterialAnmSnapshot{};
    work->model->calcMaterial();
    restore_material_anm(anmSnapshot);
    work->model->diff();
}

void register_remote_equipment_interp(RemoteEquipmentInterp& work, J3DModel* model,
                                      const dKy_tevstr_c& tevStr,
                                      bool suppressMaterialAnm) {
    work.model = model;
    work.tevStr = tevStr;
    work.suppressMaterialAnm = suppressMaterialAnm;
    dusk::frame_interp::add_interpolation_callback(&remote_equipment_interp_callback, &work);
}

void free_remote_face_anim(RemoteFaceAnim& anim) {
    if (anim.btpBuffer != nullptr) {
        JKRFree(anim.btpBuffer);
    }
    if (anim.btkBuffer != nullptr) {
        JKRFree(anim.btkBuffer);
    }
    anim = {};
}

void free_remote_eye_anims(daAlink_matAnm_c* (&anims)[2], J3DModelData*& data) {
    for (daAlink_matAnm_c*& anm : anims) {
        if (anm != nullptr) {
            JKR_DELETE(anm);
            anm = nullptr;
        }
    }
    data = nullptr;
}

bool ensure_remote_eye_anims(daAlink_matAnm_c* (&anims)[2]) {
    for (daAlink_matAnm_c*& anm : anims) {
        if (anm == nullptr) {
            anm = JKR_NEW daAlink_matAnm_c();
            if (anm == nullptr) {
                return false;
            }
        }
        anm->init();
        anm->setNowOffsetX(0.0f);
        anm->setNowOffsetY(0.0f);
    }
    return true;
}

void* load_link_anm(u16 resId, u32 bufferSize, void** outBuffer) {
    void* buffer = JKRAlloc(bufferSize, 0x20);
    if (buffer == nullptr) {
        DuskLog.warn("Multiplayer remote Link dummy: failed to allocate anm buffer size={}",
                     bufferSize);
        return nullptr;
    }

    JKRReadIdxResource(buffer, bufferSize, resId, dComIfGp_getAnmArchive());
    void* anm = J3DAnmLoaderDataBase::load(buffer, J3DLOADER_UNK_FLAG0);
    if (anm == nullptr) {
        JKRFree(buffer);
        DuskLog.warn("Multiplayer remote Link dummy: failed to load anm res={}", resId);
        return nullptr;
    }
    *outBuffer = buffer;
    return anm;
}

J3DAnmTexPattern* load_link_btp(u16 resId, void** outBuffer) {
    return static_cast<J3DAnmTexPattern*>(load_link_anm(resId, 0x400, outBuffer));
}

J3DAnmTextureSRTKey* load_link_btk(u16 resId, void** outBuffer) {
    return static_cast<J3DAnmTextureSRTKey*>(load_link_anm(resId, 0x400, outBuffer));
}

void set_texture_max_lod(J3DModelData* data, const char* const* names, size_t nameCount,
                         u8 maxLOD) {
    if (data == nullptr) {
        return;
    }

    J3DTexture* texture = data->getTexture();
    JUTNameTab* nameTable = data->getTextureName();
    if (texture == nullptr || nameTable == nullptr) {
        return;
    }

    for (u16 i = 0; i < texture->getNum(); ++i) {
        const char* textureName = nameTable->getName(i);
        if (textureName == nullptr) {
            continue;
        }
        for (size_t nameIndex = 0; nameIndex < nameCount; ++nameIndex) {
            if (std::strcmp(textureName, names[nameIndex]) == 0) {
                ResTIMG* timg = texture->getResTIMG(i);
                if (timg != nullptr) {
                    timg->maxLOD = maxLOD;
                }
                break;
            }
        }
    }
}

void bind_material_anm(J3DModelData* data, u16 materialIndex, J3DMaterialAnm* anm) {
    if (data == nullptr || anm == nullptr || materialIndex >= data->getMaterialNum()) {
        return;
    }

    J3DMaterial* material = data->getMaterialNodePointer(materialIndex);
    if (material != nullptr) {
        material->setMaterialAnm(anm);
    }
}

void bind_remote_eye_material_anms(J3DModelData* data, u16 leftMaterial, u16 rightMaterial,
                                   daAlink_matAnm_c* (&anims)[2], J3DModelData*& boundData) {
    if (data == nullptr || data == boundData) {
        return;
    }

    if (!ensure_remote_eye_anims(anims)) {
        DuskLog.warn("Multiplayer remote Link dummy: failed to allocate remote eye material anim");
        return;
    }

    bind_material_anm(data, leftMaterial, anims[0]);
    bind_material_anm(data, rightMaterial, anims[1]);
    boundData = data;
}

void clear_material_anm(J3DModelData* data, u16 materialIndex) {
    if (data == nullptr || materialIndex >= data->getMaterialNum()) {
        return;
    }

    J3DMaterial* material = data->getMaterialNodePointer(materialIndex);
    if (material != nullptr) {
        material->setMaterialAnm(nullptr);
    }
}

void set_material_tev_color1(J3DModelData* data, u16 materialIndex,
                             const J3DGXColorS10* color) {
    if (data == nullptr || color == nullptr || materialIndex >= data->getMaterialNum()) {
        return;
    }

    J3DMaterial* material = data->getMaterialNodePointer(materialIndex);
    if (material != nullptr) {
        material->setTevColor(1, color);
    }
}

void prepare_remote_human_body_materials(const PeerPoseSnapshot& pose, daAlink_c* link,
                                         J3DModel* body, J3DModel* hat) {
    if (pose.isWolf || link == nullptr || body == nullptr || body->getModelData() == nullptr ||
        shares_active_local_link_model_data(body, link))
    {
        return;
    }

    // Mirrors the neutral-color path through daAlink_c::setWaterDropColor().
    // In cross-form draws, the selected human body archive may not have been
    // initialized by the receiver's local Link draw path, leaving these shared
    // material color slots at archive defaults.
    const GXColorS10 neutralBaseColor = {0, 0, 0, 0};
    const J3DGXColorS10 neutralColor(neutralBaseColor);
    J3DModelData* bodyData = body->getModelData();
    J3DModelData* hatData = hat != nullptr ? hat->getModelData() : nullptr;

    if (pose.clothesVariant == 1) {
        set_material_tev_color1(bodyData, 7, &neutralColor);
        set_material_tev_color1(bodyData, 5, &neutralColor);
        set_material_tev_color1(hatData, 0, &neutralColor);
        return;
    }

    if (pose.clothesVariant == 2) {
        set_material_tev_color1(bodyData, 13, &neutralColor);
        set_material_tev_color1(bodyData, 0, &neutralColor);
        set_material_tev_color1(bodyData, 1, &neutralColor);
        set_material_tev_color1(hatData, 1, &neutralColor);
        return;
    }

    if (pose.clothesVariant == 3) {
        set_material_tev_color1(bodyData, 11, &neutralColor);
        set_material_tev_color1(bodyData, 10, &neutralColor);
        set_material_tev_color1(bodyData, 9, &neutralColor);
        set_material_tev_color1(bodyData, 8, &neutralColor);
        set_material_tev_color1(bodyData, 6, &neutralColor);
        set_material_tev_color1(hatData, 2, &neutralColor);
        set_material_tev_color1(hatData, 1, &neutralColor);
        return;
    }

    if (pose.clothesVariant == 0) {
        set_material_tev_color1(bodyData, 17, &neutralColor);
        set_material_tev_color1(bodyData, 9, &neutralColor);
        set_material_tev_color1(bodyData, 0, &neutralColor);
        set_material_tev_color1(bodyData, 1, &neutralColor);
        set_material_tev_color1(bodyData, 2, &neutralColor);
        set_material_tev_color1(bodyData, 16, &neutralColor);
        set_material_tev_color1(bodyData, 15, &neutralColor);
        set_material_tev_color1(bodyData, 14, &neutralColor);
        set_material_tev_color1(hatData, 0, &neutralColor);
    }
}

void prepare_remote_eye_materials(RemoteLinkDummy& dummy, const PeerPoseSnapshot& pose,
                                  J3DModel* body, J3DModel* face) {
    if (pose.isWolf) {
        if (body == nullptr || body->getModelData() == nullptr) {
            return;
        }

        J3DModelData* data = body->getModelData();
        const char* wolfEyeTextures[] = {"wl_eyeball"};
        set_texture_max_lod(data, wolfEyeTextures, 1, 0);
    clear_material_anm(data, 4);
    clear_material_anm(data, 5);
        if (data->getMaterialNum() > 1) {
            J3DMaterial* eyeMaterial = data->getMaterialNodePointer(1);
            if (eyeMaterial != nullptr && eyeMaterial->getShape() != nullptr) {
                eyeMaterial->getShape()->show();
            }
        }

        if (dummy.wolfEyeData != data) {
            dummy.wolfEyeHL.remove();
            dummy.wolfEyeHL.entry(data, "wl_eye_Hilight");
            dummy.wolfEyeData = data;
        }
        return;
    }

    if (face == nullptr || face->getModelData() == nullptr) {
        return;
    }

    J3DModelData* data = face->getModelData();
    const char* humanEyeTextures[] = {"al_eyeball", "highlight02", "eye_kage01"};
    set_texture_max_lod(data, humanEyeTextures, 3, 0);
    clear_material_anm(data, 2);
    clear_material_anm(data, 3);

    if (dummy.humanEyeData != data) {
        dummy.humanEyeHL.remove();
        dummy.humanEyeHL.entry(data, "highlight02");
        dummy.humanEyeData = data;
    }
}

void attach_default_face_animators(RemoteFaceAnim& anim, J3DModelData* data, bool wolf) {
    if (data == nullptr) {
        return;
    }

    if (anim.data != data) {
        free_remote_face_anim(anim);
        anim.data = data;
    }

    J3DAnmTexPattern* btp =
        load_link_btp(wolf ? dRes_ID_ALANM_BTP_WL_FA_e : dRes_ID_ALANM_BTP_FA_e,
                      &anim.btpBuffer);
    if (btp != nullptr) {
        btp->setFrame(0.0f);
        btp->searchUpdateMaterialID(data);
        data->entryTexNoAnimator(btp);
    }

    J3DAnmTextureSRTKey* btk =
        load_link_btk(wolf ? dRes_ID_ALANM_BTK_WL_FA_e : dRes_ID_ALANM_BTK_FA_e,
                      &anim.btkBuffer);
    if (btk != nullptr) {
        btk->setFrame(0.0f);
        btk->searchUpdateMaterialID(data);
        data->entryTexMtxAnimator(btk);
    }
}

void initialize_remote_sword_materials(RemoteLinkDummy& dummy, J3DModel* sword,
                                       int swordVariant) {
    if (sword == nullptr || sword->getModelData() == nullptr) {
        return;
    }

    J3DModelData* data = sword->getModelData();
    if (swordVariant == 1 && dummy.initializedOrdonSwordData != data) {
        J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
            dComIfG_getObjectRes("Alink", dRes_ID_ALINK_BTK_AL_SWA_e));
        if (btk != nullptr) {
            btk->searchUpdateMaterialID(data);
            data->entryTexMtxAnimator(btk);
            dummy.initializedOrdonSwordData = data;
        }
    } else if (swordVariant == 3 && dummy.initializedMasterSwordData != data) {
        J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
            dComIfG_getObjectRes("Alink", dRes_ID_ALINK_BTK_AL_SWM_e));
        J3DAnmTevRegKey* brk = static_cast<J3DAnmTevRegKey*>(
            dComIfG_getObjectRes("Alink", dRes_ID_ALINK_BRK_AL_SWM_e));
        if (btk != nullptr) {
            btk->searchUpdateMaterialID(data);
            data->entryTexMtxAnimator(btk);
        }
        if (brk != nullptr) {
            brk->searchUpdateMaterialID(data);
            data->entryTevRegAnimator(brk);
        }
        if (btk != nullptr || brk != nullptr) {
            dummy.initializedMasterSwordData = data;
        }
    }
}

void prepare_remote_form_resources(RemoteLinkDummy& dummy, const PeerPoseSnapshot& pose,
                                   daAlink_c* link, J3DModel* body, J3DModel* hat,
                                   J3DModel* face, J3DModel* sword) {
    if (pose.isWolf) {
        if (body != nullptr && body->getModelData() != nullptr &&
            dummy.initializedWolfBodyData != body->getModelData())
        {
            attach_default_face_animators(dummy.wolfFaceAnim, body->getModelData(), true);
            dummy.initializedWolfBodyData = body->getModelData();
        }
    } else if (face != nullptr && face->getModelData() != nullptr &&
               dummy.initializedHumanFaceData != face->getModelData())
    {
        attach_default_face_animators(dummy.humanFaceAnim, face->getModelData(), false);
        dummy.initializedHumanFaceData = face->getModelData();
    }

    prepare_remote_eye_materials(dummy, pose, body, face);
    prepare_remote_human_body_materials(pose, link, body, hat);
    initialize_remote_sword_materials(dummy, sword, pose.swordVariant);
}

// Tried snapshotting AmbColor(0)/TevColor(0)/TevKColor(0) before the light
// rebind below and restoring them after this dummy's own draw (the same
// pattern as restore_material_anm() above) -- reverted. Directly poking
// the J3DMaterial's color fields back to their old values after diff()/
// entry() already ran desyncs whatever internal "last value sent to the
// GPU" bookkeeping diff() relies on for the NEXT caller's diff(), since
// that caller (real Link, next simulation tick) computes its delta against
// what diff() last recorded, not against the live field value this
// directly overwrites. Empirically this traded the original symptom for a
// different one (an added green flicker, and the sword/sheath's shine
// going flat) rather than fixing it. The frame-interpolation guard below
// (skipping the rebind entirely on non-simulation frames) is kept --
// confirmed by testing to meaningfully help on its own. For sword/sheath
// specifically, see rebindLight's doc comment: the fix there is to never
// write this shared state in the first place, not to write-then-undo it.
void entry_model_without_calc(J3DModel* model, dKy_tevstr_c* tevStr, bool rebindLight = true,
                              bool useDarkList = false, bool suppressMaterialAnm = true,
                              bool entryOnNonSimFrame = true) {
    if (model == nullptr) {
        return;
    }

    if (useDarkList) {
        dComIfGd_setListDark();
    } else {
        dComIfGd_setList();
    }
    model->unlock();

#if TARGET_PC
    if (!dusk::frame_interp::is_sim_frame()) {
        // Mirrors mDoExt_modelEntryDL()'s own frame-interpolation guard
        // (m_Do_ext.cpp, "FRAME INTERP NOTE: fixes issue #355 where some
        // lights would flicker") -- and the same root cause applies here,
        // confirmed by testing: every reported color-corruption symptom in
        // this file (sword turning yellow, clothes going black, eyes
        // disappearing) reproduced only with frame interpolation enabled,
        // and disappeared entirely running flat at the simulation rate.
        //
        // g_env_light.setLightTevColorType_MAJI() below writes directly
        // onto the SHARED J3DMaterial objects living on this model's
        // J3DModelData (confirmed by reading its implementation,
        // d_kankyo.cpp:4535 -- it takes a J3DModelData*, not a per-instance
        // object). The real local player's own daAlink_c re-binds that same
        // shared material correctly exactly once per simulation tick. With
        // interpolation on, this dummy draw call runs once per presented
        // render frame, which is MORE often than once per sim tick -- so on
        // the extra (non-sim) frames in between, this call was over writing
        // the shared material with the dummy's own values, and nothing
        // else touched it again until the next real simulation tick, which
        // is the actual window the corruption was visible in. Skipping the
        // material/light rebind on non-sim frames (just like
        // mDoExt_modelEntryDL does) and only resubmitting the model is both
        // correct AND avoids the corruption, rather than just disguising
        // it.
        if (!entryOnNonSimFrame) {
            if (useDarkList) {
                dComIfGd_setList();
            }
            return;
        }
        model->diff();
        model->entry();
        model->lock();
        model->viewCalc();
        if (useDarkList) {
            dComIfGd_setList();
        }
        return;
    }
#endif

    // Deliberately never calls J3DModel::calc() here -- it runs the joint
    // tree's recursiveCalc(), which for al.bmd/Wmdl invokes a per-joint
    // callback (daAlink_modelCallBack/wolfModelCallBack, d_a_alink.cpp)
    // that reads/writes the real daAlink_c via getUserArea(), unsafe on a
    // model with no real actor owner. The joint/weight matrices this dummy
    // needs are instead set directly by the caller (copy_remote_model_
    // matrices before this runs.
    //
    // setLightTevColorType_MAJI()+calcMaterial()+diff() mirror exactly what
    // daAlink_c::basicModelDraw()/modelDraw() (d_a_alink.cpp) do for the
    // real Link models, minus calc(). setLightTevColorType_MAJI() binds the
    // environment-light TEV color registers computed into *tevStr (see
    // build_dummy_tev_str()) -- without it, calcMaterial() still runs but
    // has no light color to read, leaving materials in whatever GX register
    // state happened to be left over from the previous draw call. That was
    // the actual cause of the dummy rendering solid black except for
    // unlit/self-illuminated parts (e.g. eyes): the earlier fix added
    // calcMaterial()/diff() but not this light-binding call, which is a
    // separate, required step. None of these three touch the joint tree's
    // callback chain, so they're safe here. This whole block now only runs
    // on simulation frames (see the frame-interpolation guard above), which
    // is also exactly the cadence the real local player's own draw uses.
    // It's still a race for whichever model part shares its archive with
    // the local player's own currently-active form (real Link draws first
    // every tick, so this dummy's rebind is technically the "last word"
    // for that tick) -- rebindLight exists so a caller that knows its
    // source is confirmed shared with an actively-drawn local equivalent
    // (see the sword/sheath call sites in draw_remote_link_dummy()) can
    // skip writing this shared state at all and just rely on what the real
    // character's own draw already set moments earlier, rather than
    // writing it and trying to undo it afterward (see this function's
    // leading comment for why the undo approach didn't work out).
    // calcMaterial() and materialAnm suppression still run regardless --
    // calcMaterial() recomputes the per-instance-relevant texture-
    // coordinate matrix from THIS model's own current orientation (needed
    // for things like the blade's shine to track the dummy's own pose, not
    // freeze), and materialAnm suppression is a crash guard unrelated to
    // color/light.
    if (rebindLight && tevStr != nullptr) {
        g_env_light.setLightTevColorType_MAJI(model, tevStr);
    }
    MaterialAnmSnapshot anmSnapshot = suppressMaterialAnm ? suppress_material_anm(model)
                                                          : MaterialAnmSnapshot{};
    model->calcMaterial();
    model->diff();
    model->entry();
    model->lock();
    model->viewCalc();
    restore_material_anm(anmSnapshot);
    if (useDarkList) {
        dComIfGd_setList();
    }
}

// Mirrors daAlink_c::draw() (d_a_alink.cpp): the real Link actor computes
// its own tevStr member once per frame via
// g_env_light.settingTevStruct(checkWolf() ? 9 : 10, &current.pos, &tevStr)
// followed by daAlink_c::initTevCustomColor() zeroing the extra tint
// fields, before any of its models draw. The dummy has no actor of its own
// to own a persistent tevStr, so this builds an equivalent one fresh each
// draw from the peer's own reported position and form. Environment
// lighting is a property of where the peer is standing, not of which local
// player is asking, so this is exact, not an approximation.
dKy_tevstr_c build_dummy_tev_str(const PeerPoseSnapshot& pose) {
    dKy_tevstr_c tevStr{};
    cXyz pos(pose.x, pose.y, pose.z);
    g_env_light.settingTevStruct(pose.isWolf ? 9 : 10, &pos, &tevStr);
    tevStr.TevColor.r = 0;
    tevStr.TevColor.g = 0;
    tevStr.TevColor.b = 0;
    tevStr.TevKColor.r = 0;
    tevStr.TevKColor.b = 0;
    return tevStr;
}

void init_remote_body_collision(RemoteLinkDummy& dummy) {
    if (dummy.bodyCollisionInitialized) {
        return;
    }

    dummy.bodyCyl.Set(l_remoteLinkBodyCylSrc);
    dummy.bodyCyl.OnTgNoConHit();
    dummy.bodyCyl.OffAtSetBit();
    dummy.bodyCyl.OffTgSetBit();
    dummy.bodyCollisionInitialized = true;
}

f32 clamp_f32(f32 value, f32 min, f32 max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

void get_body_segment(f32 x, f32 z, s16 angleY, bool isWolf, f32* outAx, f32* outAz,
                      f32* outBx, f32* outBz) {
    if (!isWolf) {
        *outAx = x;
        *outAz = z;
        *outBx = x;
        *outBz = z;
        return;
    }

    const f32 forwardX = cM_ssin(angleY);
    const f32 forwardZ = cM_scos(angleY);
    *outAx = x - forwardX * kRemoteWolfBodyHalfLength;
    *outAz = z - forwardZ * kRemoteWolfBodyHalfLength;
    *outBx = x + forwardX * kRemoteWolfBodyHalfLength;
    *outBz = z + forwardZ * kRemoteWolfBodyHalfLength;
}

void closest_points_on_segments_xz(f32 aX, f32 aZ, f32 bX, f32 bZ, f32 cX, f32 cZ,
                                   f32 dX, f32 dZ, f32* outLocalX, f32* outLocalZ,
                                   f32* outRemoteX, f32* outRemoteZ) {
    const f32 ux = bX - aX;
    const f32 uz = bZ - aZ;
    const f32 vx = dX - cX;
    const f32 vz = dZ - cZ;
    const f32 wx = aX - cX;
    const f32 wz = aZ - cZ;

    const f32 aa = ux * ux + uz * uz;
    const f32 bb = ux * vx + uz * vz;
    const f32 cc = vx * vx + vz * vz;
    const f32 dd = ux * wx + uz * wz;
    const f32 ee = vx * wx + vz * wz;
    const f32 denom = aa * cc - bb * bb;

    f32 s = 0.0f;
    if (denom > 0.0001f) {
        s = clamp_f32((bb * ee - cc * dd) / denom, 0.0f, 1.0f);
    }

    f32 t = 0.0f;
    if (cc > 0.0001f) {
        t = clamp_f32((bb * s + ee) / cc, 0.0f, 1.0f);
    }

    if (aa > 0.0001f) {
        s = clamp_f32((bb * t - dd) / aa, 0.0f, 1.0f);
    }

    *outLocalX = aX + ux * s;
    *outLocalZ = aZ + uz * s;
    *outRemoteX = cX + vx * t;
    *outRemoteZ = cZ + vz * t;
}

void apply_remote_body_push(const PeerPoseSnapshot& pose, fopAc_ac_c* playerActor) {
    if (playerActor == nullptr) {
        return;
    }

    const bool localIsWolf =
        fopAcM_GetName(playerActor) == fpcNm_ALINK_e &&
        static_cast<daAlink_c*>(playerActor)->checkWolf();
    const f32 localHeight = localIsWolf ? kRemoteWolfBodyHeight : kRemoteHumanBodyHeight;
    const f32 remoteHeight = pose.isWolf ? kRemoteWolfBodyHeight : kRemoteHumanBodyHeight;
    const f32 localMinY = playerActor->current.pos.y - 20.0f;
    const f32 localMaxY = playerActor->current.pos.y + localHeight;
    const f32 remoteMinY = pose.y - 20.0f;
    const f32 remoteMaxY = pose.y + remoteHeight;
    if (localMaxY < remoteMinY || remoteMaxY < localMinY) {
        return;
    }

    f32 localAx;
    f32 localAz;
    f32 localBx;
    f32 localBz;
    f32 remoteAx;
    f32 remoteAz;
    f32 remoteBx;
    f32 remoteBz;
    get_body_segment(playerActor->current.pos.x, playerActor->current.pos.z,
                     playerActor->shape_angle.y, localIsWolf, &localAx, &localAz, &localBx,
                     &localBz);
    get_body_segment(pose.x, pose.z, static_cast<s16>(pose.angleY), pose.isWolf, &remoteAx,
                     &remoteAz, &remoteBx, &remoteBz);

    f32 localClosestX;
    f32 localClosestZ;
    f32 remoteClosestX;
    f32 remoteClosestZ;
    closest_points_on_segments_xz(localAx, localAz, localBx, localBz, remoteAx, remoteAz,
                                  remoteBx, remoteBz, &localClosestX, &localClosestZ,
                                  &remoteClosestX, &remoteClosestZ);

    const f32 combinedRadius =
        (localIsWolf ? kRemoteWolfBodyRadius : kLocalLinkBodyRadius) +
        (pose.isWolf ? kRemoteWolfBodyRadius : kRemoteHumanBodyRadius);
    f32 dx = localClosestX - remoteClosestX;
    f32 dz = localClosestZ - remoteClosestZ;
    f32 distSq = dx * dx + dz * dz;
    if (distSq >= combinedRadius * combinedRadius) {
        return;
    }

    if (distSq < 0.0001f) {
        dx = cM_ssin(playerActor->shape_angle.y);
        dz = cM_scos(playerActor->shape_angle.y);
        distSq = dx * dx + dz * dz;
    }

    const f32 dist = std::sqrt(distSq);
    if (dist < 0.0001f) {
        return;
    }

    const f32 push = combinedRadius - dist;
    playerActor->current.pos.x += (dx / dist) * push;
    playerActor->current.pos.z += (dz / dist) * push;
}

void update_remote_body_collision(RemoteLinkDummy& dummy, const PeerPoseSnapshot& pose,
                                  fopAc_ac_c* owner) {
    init_remote_body_collision(dummy);

    cXyz position(pose.x, pose.y, pose.z);
    dummy.bodyCyl.SetC(position);
    dummy.bodyCyl.SetR(pose.isWolf ? kRemoteWolfBodyRadius : kRemoteHumanBodyRadius);
    dummy.bodyCyl.SetH(pose.isWolf ? kRemoteWolfBodyHeight : kRemoteHumanBodyHeight);
    dummy.bodyCyl.OffAtSetBit();
    dummy.bodyCyl.OffTgSetBit();
    dummy.bodyCyl.OffCoSetBit();

    apply_remote_body_push(pose, owner);
}

bool shares_active_local_link_model_data(J3DModel* model, const daAlink_c* link) {
    if (model == nullptr || model->getModelData() == nullptr || link == nullptr) {
        return false;
    }

    J3DModelData* data = model->getModelData();
    return (link->mpLinkModel != nullptr && data == link->mpLinkModel->getModelData()) ||
           (link->mpLinkHatModel != nullptr && data == link->mpLinkHatModel->getModelData()) ||
           (link->mpLinkFaceModel != nullptr && data == link->mpLinkFaceModel->getModelData()) ||
           (link->mpLinkHandModel != nullptr && data == link->mpLinkHandModel->getModelData()) ||
           (link->mSwordModel != nullptr && data == link->mSwordModel->getModelData()) ||
           (link->mSheathModel != nullptr && data == link->mSheathModel->getModelData()) ||
           (link->mShieldModel != nullptr && data == link->mShieldModel->getModelData());
}

bool local_link_draws_matching_sword_item(J3DModel* model, J3DModel* localModel,
                                          daAlink_c* link) {
    return model != nullptr && model->getModelData() != nullptr && localModel != nullptr &&
           localModel->getModelData() == model->getModelData() && link != nullptr &&
           link->checkSwordDraw();
}

bool local_link_draws_matching_shield_item(J3DModel* model, daAlink_c* link) {
    return model != nullptr && model->getModelData() != nullptr && link != nullptr &&
           link->mShieldModel != nullptr &&
           link->mShieldModel->getModelData() == model->getModelData() && link->checkShieldDraw();
}

J3DModelData* choose_sword_source_data(int swordVariant) {
    if (swordVariant == 2) {
        if (!is_arc_ready(sHumanArc)) {
            return nullptr;
        }
        return static_cast<J3DModelData*>(dComIfG_getObjectRes("Kmdl", "al_SWB.bmd"));
    }

    if (!is_arc_ready(sAlinkArc)) {
        return nullptr;
    }

    switch (swordVariant) {
    case 3:  // RemoteSwordVariant::REMOTE_SWORD_MASTER (multiplayer.cpp)
        return static_cast<J3DModelData*>(dComIfG_getObjectRes("Alink", 0x38));  // AL_SWM
    case 1:  // REMOTE_SWORD_ORDON
        return static_cast<J3DModelData*>(dComIfG_getObjectRes("Alink", 0x3C));  // AL_SWA
    default:
        // REMOTE_SWORD_UNKNOWN (0) used to fall through to AL_SWM (0x38) here,
        // the Master Sword model -- which carries its own dedicated glow BRK
        // animation (dRes_ID_ALINK_BRK_AL_SWM_e, assets/.../Alink.h). Whenever
        // detect_sword_variant() (multiplayer.cpp) failed to recognize the
        // peer's sword (returns UNKNOWN, not just for an actual Master Sword),
        // the dummy would render the Master Sword's glowing blade regardless
        // of what the peer was actually holding -- the reported "sword
        // flickers/glows yellow" symptom. Don't guess a specific sword model
        // for an unrecognized variant; hide the sword instead.
        return nullptr;
    }
}

J3DModelData* choose_sheath_source_data(int swordVariant) {
    if (!is_arc_ready(sAlinkArc)) {
        return nullptr;
    }

    switch (swordVariant) {
    case 2:
    case 3:
        return static_cast<J3DModelData*>(dComIfG_getObjectRes("Alink", 0x37));  // AL_PODM
    case 1:
        return static_cast<J3DModelData*>(dComIfG_getObjectRes("Alink", 0x3B));  // AL_PODA
    default:
        // Same reasoning as choose_sword_source_data()'s default case: don't
        // guess a sheath model for an unrecognized (REMOTE_SWORD_UNKNOWN)
        // variant.
        return nullptr;
    }
}

bool is_wood_sword_variant(int swordVariant) {
    return swordVariant == 2;
}

J3DModelData* choose_shield_source_data(int shieldVariant) {
    switch (shieldVariant) {
    case 1:
        if (!is_arc_ready(sCarvingWoodShieldArc)) {
            return nullptr;
        }
        return static_cast<J3DModelData*>(dComIfG_getObjectRes("CWShd", 3));  // AL_SHB
    case 2:
        if (!is_arc_ready(sOrdonShieldArc)) {
            return nullptr;
        }
        return static_cast<J3DModelData*>(dComIfG_getObjectRes("SWShd", 3));  // AL_SHC
    case 3:
        if (!is_arc_ready(sHylianShieldArc)) {
            return nullptr;
        }
        return static_cast<J3DModelData*>(dComIfG_getObjectRes("HyShd", 3));  // AL_SHA
    default:
        return nullptr;
    }
}

// J3DShape::hide()/show() (J3DShape.h) toggle a flag stored on the J3DShape
// itself, which lives on the SHARED J3DModelData the sword model was cloned
// from -- not on our per-instance J3DModel clone. choose_sword_source_data()
// fetches that data via the same dComIfG_getObjectRes("Kmdl"/"Alink", ...)
// path the real game uses for the local player's own equipped sword, so if
// the local player happens to have the same sword variant equipped, this
// was the SAME J3DShape the local player's own real sword reads -- toggling
// it for the dummy's sword_out state could flip the local player's own
// sword's visibility too, intermittently and asymmetrically per client,
// exactly the "sometimes invisible depending on client side" symptom. There
// is no per-instance visibility flag available on J3DShapePacket/J3DShape in
// this engine to use instead, so apply_sword_shape_visibility() snapshots
// the prior flag and the caller (draw_remote_link_dummy) must call
// restore_sword_shape_visibility() immediately after this dummy's own sword
// draw call completes, scoping the mutation to only this dummy's own
// drawcall instead of leaking into whatever happens afterward.
ShapeVisibilitySnapshot show_model_shapes(J3DModel* model) {
    ShapeVisibilitySnapshot snapshot;
    if (model == nullptr || model->getModelData() == nullptr) {
        return snapshot;
    }

    J3DModelData* data = model->getModelData();
    for (u16 i = 0; i < data->getMaterialNum(); ++i) {
        J3DMaterial* material = data->getMaterialNodePointer(i);
        if (material == nullptr || material->getShape() == nullptr) {
            continue;
        }
        J3DShape* shape = material->getShape();
        snapshot.shapes.push_back({shape, shape->checkFlag(J3DShpFlag_Visible)});
        shape->show();
    }
    return snapshot;
}

void restore_model_shapes(const ShapeVisibilitySnapshot& snapshot) {
    for (const auto& entry : snapshot.shapes) {
        if (entry.second) {
            entry.first->hide();
        } else {
            entry.first->show();
        }
    }
}

SwordShapeVisibility apply_sword_shape_visibility(J3DModel* sword, bool swordOut, bool woodSword) {
    SwordShapeVisibility snapshot;
    if (sword == nullptr || sword->getModelData() == nullptr) {
        return snapshot;
    }

    J3DMaterial* material =
        sword->getModelData()->getMaterialNodePointer(woodSword ? 1 : 0);
    if (material == nullptr || material->getShape() == nullptr) {
        return snapshot;
    }

    snapshot.shape = material->getShape();
    snapshot.wasHidden = snapshot.shape->checkFlag(J3DShpFlag_Visible);

    // TP hides material 1 when the wooden sword is in-hand and shows it
    // again while sheathed. Non-wood swords use material 0 the opposite
    // way: shown in-hand, hidden while sheathed.
    const bool shouldHide = woodSword ? swordOut : !swordOut;
    if (shouldHide) {
        snapshot.shape->hide();
    } else {
        snapshot.shape->show();
    }

    return snapshot;
}

void restore_sword_shape_visibility(const SwordShapeVisibility& snapshot) {
    if (snapshot.shape == nullptr) {
        return;
    }

    if (snapshot.wasHidden) {
        snapshot.shape->hide();
    } else {
        snapshot.shape->show();
    }
}

RemoteLinkSources resolve_remote_sources(const PeerPoseSnapshot& pose) {
    RemoteLinkSources sources;

    if (pose.isWolf) {
        if (!is_arc_ready(sWolfArc)) {
            return sources;
        }
        sources.body = static_cast<J3DModelData*>(dComIfG_getObjectRes("Wmdl", 14));
        sources.humanParts = false;
        return sources;
    }

    switch (pose.clothesVariant) {
    case 1:  // RemoteClothesVariant::REMOTE_CLOTHES_CASUAL (multiplayer.cpp)
        if (!is_arc_ready(sCasualArc)) return sources;
        sources.body = static_cast<J3DModelData*>(dComIfG_getObjectRes("Bmdl", "bl.bmd"));
        sources.hat = static_cast<J3DModelData*>(dComIfG_getObjectRes("Bmdl", "bl_head.bmd"));
        sources.face = static_cast<J3DModelData*>(dComIfG_getObjectRes("Bmdl", "al_face.bmd"));
        sources.hand = static_cast<J3DModelData*>(dComIfG_getObjectRes("Bmdl", "bl_hands.bmd"));
        sources.humanParts = true;
        break;
    case 2:  // REMOTE_CLOTHES_ZORA
        if (!is_arc_ready(sZoraArc)) return sources;
        sources.body = static_cast<J3DModelData*>(dComIfG_getObjectRes("Zmdl", "zl.bmd"));
        sources.hat = static_cast<J3DModelData*>(dComIfG_getObjectRes("Zmdl", "zl_head.bmd"));
        sources.face = static_cast<J3DModelData*>(dComIfG_getObjectRes("Zmdl", "zl_face.bmd"));
        sources.hand = static_cast<J3DModelData*>(dComIfG_getObjectRes("Zmdl", "al_hands.bmd"));
        sources.humanParts = true;
        break;
    case 3:  // REMOTE_CLOTHES_ARMOR
        if (!is_arc_ready(sMagicArmorArc)) return sources;
        sources.body = static_cast<J3DModelData*>(dComIfG_getObjectRes("Mmdl", "ml.bmd"));
        sources.hat = static_cast<J3DModelData*>(dComIfG_getObjectRes("Mmdl", "ml_head.bmd"));
        sources.face = static_cast<J3DModelData*>(dComIfG_getObjectRes("Mmdl", "al_face.bmd"));
        sources.hand = static_cast<J3DModelData*>(dComIfG_getObjectRes("Mmdl", "al_hands.bmd"));
        sources.humanParts = true;
        break;
    default:
        if (!is_arc_ready(sHumanArc)) return sources;
        sources.body = static_cast<J3DModelData*>(dComIfG_getObjectRes("Kmdl", "al.bmd"));
        sources.hat = static_cast<J3DModelData*>(dComIfG_getObjectRes("Kmdl", "al_head.bmd"));
        sources.face = static_cast<J3DModelData*>(dComIfG_getObjectRes("Kmdl", "al_face.bmd"));
        sources.hand = static_cast<J3DModelData*>(dComIfG_getObjectRes("Kmdl", "al_hands.bmd"));
        sources.humanParts = true;
        break;
    }

    if (pose.swordDraw) {
        sources.sword = choose_sword_source_data(pose.swordVariant);
        sources.sheath = choose_sheath_source_data(pose.swordVariant);
    }

    if (pose.shieldDraw) {
        sources.shield = choose_shield_source_data(pose.shieldVariant);
    }

    return sources;
}

bool remote_link_actor_pose_supported(const PeerPoseSnapshot& pose) {
    return pose.valid && pose.ageTicks <= 30 && !pose.isTransforming;
}

daRemoteLink_c* find_remote_link_actor(RemoteLinkActorDummy& dummy) {
    if (dummy.actorId == fpcM_ERROR_PROCESS_ID_e) {
        return nullptr;
    }

    fopAc_ac_c* actor = fopAcM_SearchByID(dummy.actorId);
    if (actor == nullptr) {
        if (!fpcM_IsCreating(dummy.actorId)) {
            dummy.actorId = fpcM_ERROR_PROCESS_ID_e;
        }
        return nullptr;
    }

    if (fopAcM_GetName(actor) != fpcNm_REMOTE_LINK_e) {
        DuskLog.warn("Multiplayer remote Link actor: proc id {} resolved to unexpected actor {}",
                     dummy.actorId, fopAcM_GetName(actor));
        dummy.actorId = fpcM_ERROR_PROCESS_ID_e;
        return nullptr;
    }

    return static_cast<daRemoteLink_c*>(actor);
}

void destroy_remote_link_actor_dummy(const std::string& peerId) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        return;
    }

    if (it->second.actorId != fpcM_ERROR_PROCESS_ID_e) {
        fopAcM_delete(it->second.actorId);
    }
    sActorDummies.erase(it);
}

bool remote_link_actor_dummy_active(const std::string& peerId) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        return false;
    }

    if (find_remote_link_actor(it->second) != nullptr) {
        return true;
    }

    return it->second.actorId != fpcM_ERROR_PROCESS_ID_e;
}

bool remote_link_actor_dummy_claimed(const std::string& peerId) {
    return sActorDummies.find(peerId) != sActorDummies.end();
}

void update_actor_dummy_collision(const std::string& peerId, const PeerPoseSnapshot& pose) {
    fopAc_ac_c* playerActor = dComIfGp_getPlayer(0);
    if (playerActor == nullptr) {
        return;
    }

    RemoteLinkDummy& dummy = sDummies[peerId];
    update_remote_body_collision(dummy, pose, playerActor);
}

}  // namespace

void preload_remote_link_dummy_resources() {
    // Called from dusk::multiplayer::update() (the simulation tick), not the
    // draw phase. See the declaration comment in remote_link_dummy.hpp for
    // why this split exists. ensure_arc_loaded() itself is a no-op once a
    // request is already complete, so calling this every tick is cheap.
    ensure_arc_loaded(sHumanArc);
    ensure_arc_loaded(sWolfArc);
    ensure_arc_loaded(sAlinkArc);
    ensure_arc_loaded(sCasualArc);
    ensure_arc_loaded(sZoraArc);
    ensure_arc_loaded(sMagicArmorArc);
    ensure_arc_loaded(sCarvingWoodShieldArc);
    ensure_arc_loaded(sOrdonShieldArc);
    ensure_arc_loaded(sHylianShieldArc);
}

void sync_remote_link_actor_dummies(const std::map<std::string, PeerPoseSnapshot>& poses) {
    const char* localStage = dComIfGp_getStartStageName();
    if (localStage == nullptr) {
        for (auto it = sActorDummies.begin(); it != sActorDummies.end();) {
            const std::string peerId = it->first;
            ++it;
            destroy_remote_link_actor_dummy(peerId);
        }
        return;
    }

    for (auto it = sActorDummies.begin(); it != sActorDummies.end();) {
        const std::string peerId = it->first;
        if (poses.find(peerId) == poses.end()) {
            ++it;
            destroy_remote_link_actor_dummy(peerId);
            continue;
        }
        ++it;
    }

    for (const auto& entry : poses) {
        const std::string& peerId = entry.first;
        const PeerPoseSnapshot& pose = entry.second;
        const bool supported = remote_link_actor_pose_supported(pose) &&
                               pose.stage == localStage &&
                               dComIfGp_roomControl_checkRoomDisp(pose.room);
        if (!supported) {
            auto existing = sActorDummies.find(peerId);
            if (pose.valid && pose.isTransforming && existing != sActorDummies.end()) {
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            destroy_remote_link_actor_dummy(peerId);
            continue;
        }

        RemoteLinkActorDummy& dummy = sActorDummies[peerId];
        daRemoteLink_c* actor = find_remote_link_actor(dummy);
        if (dummy.clothesVariant != -1 &&
            (dummy.clothesVariant != pose.clothesVariant || dummy.isWolf != pose.isWolf))
        {
            if (!dummy.recreatePending) {
                if (dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                    fopAcM_delete(dummy.actorId);
                }
                dummy.pendingClothesVariant = pose.clothesVariant;
                dummy.pendingIsWolf = pose.isWolf;
                dummy.recreateDelayTicks = 3;
                dummy.recreatePending = true;
                if (sActorSyncLogCount < 20) {
                    ++sActorSyncLogCount;
                    DuskLog.info("Multiplayer remote Link actor: recreate requested peer={} "
                                 "old_clothes={} new_clothes={} old_wolf={} new_wolf={} "
                                 "old_id={}",
                                 peerId, dummy.clothesVariant, pose.clothesVariant,
                                 dummy.isWolf, pose.isWolf, dummy.actorId);
                }
            }

            if (actor != nullptr || dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                update_actor_dummy_collision(peerId, pose);
                continue;
            }

            dummy.clothesVariant = -1;
            dummy.isWolf = false;
            if (dummy.recreateDelayTicks > 0) {
                --dummy.recreateDelayTicks;
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            dummy.recreatePending = false;
        }

        if (actor == nullptr) {
            if (dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                continue;
            }

            cXyz pos(pose.x, pose.y, pose.z);
            csXyz angle(0, static_cast<s16>(pose.angleY), 0);
            cXyz scale(1.0f, 1.0f, 1.0f);
            dummy.clothesVariant = pose.clothesVariant;
            dummy.isWolf = pose.isWolf;
            dummy.pendingClothesVariant = -1;
            dummy.pendingIsWolf = false;
            dummy.recreateDelayTicks = 0;
            dummy.recreatePending = false;
            const u32 actorParams =
                static_cast<u32>(pose.clothesVariant & 0xFF) | (pose.isWolf ? 0x100 : 0);
            dummy.actorId = fopAcM_create(fpcNm_REMOTE_LINK_e,
                                          actorParams, &pos, pose.room, &angle, &scale, -1);
            if (sActorSyncLogCount < 20) {
                ++sActorSyncLogCount;
                DuskLog.info(
                    "Multiplayer remote Link actor: spawn requested peer={} id={} clothes={} "
                    "wolf={} pos=({}, {}, {}) room={} angleY={}",
                    peerId, dummy.actorId, pose.clothesVariant, pose.isWolf, pose.x, pose.y,
                    pose.z, pose.room, pose.angleY);
            }
            continue;
        }

        update_actor_dummy_collision(peerId, pose);
        cXyz pos(pose.x, pose.y, pose.z);
        actor->setRemotePose(pos, static_cast<s16>(pose.angleY), static_cast<s8>(pose.room));
        actor->setRemoteActionState(pose.procId, pose.procVar0, pose.procVar1, pose.procVar2,
                                    pose.procVar3, pose.procVar5, pose.underFrame,
                                    static_cast<u16>(pose.underBck0), pose.underFrame0,
                                    pose.underRate0, static_cast<u16>(pose.upperBck2),
                                    pose.upperFrame2, pose.upperRate2, pose.equipItem,
                                    pose.swordVariant, pose.shieldVariant, pose.swordDraw,
                                    pose.shieldDraw, pose.swordOut, pose.itemDraw,
                                    pose.kanteraDraw, pose.itemActorKind,
                                    pose.rideActorKind);
        actor->setRemoteMatrices(pose.linkMatrices);
        if (dummy.logCount < 5) {
            ++dummy.logCount;
            DuskLog.info("Multiplayer remote Link actor: updated peer={} id={} pos=({}, {}, {}) "
                         "room={} angleY={}",
                         peerId, dummy.actorId, pose.x, pose.y, pose.z, pose.room, pose.angleY);
        }
    }
}

void destroy_remote_link_dummy(const std::string& peerId) {
    destroy_remote_link_actor_dummy(peerId);

    auto it = sDummies.find(peerId);
    if (it == sDummies.end()) {
        return;
    }

    RemoteLinkDummy& dummy = it->second;
    destroy_model(dummy.body);
    destroy_model(dummy.hat);
    destroy_model(dummy.face);
    destroy_model(dummy.hand);
    destroy_model(dummy.sword);
    destroy_model(dummy.sheath);
    destroy_model(dummy.shield);
    dummy.swordInterp = {};
    dummy.sheathInterp = {};
    dummy.shieldInterp = {};
    free_remote_face_anim(dummy.humanFaceAnim);
    free_remote_face_anim(dummy.wolfFaceAnim);
    free_remote_eye_anims(dummy.humanEyeAnm, dummy.humanEyeAnmData);
    free_remote_eye_anims(dummy.wolfEyeAnm, dummy.wolfEyeAnmData);
    destroy_transform_anim(dummy.transform);
    dummy.bodyCollisionInitialized = false;
    dummy.stage.clear();
    dummy.room = -128;
    dummy.drawLogCount = 0;
    dummy.matrixRejectLogCount = 0;
    dummy.wolfDrawLogCount = 0;
    dummy.transformLogCount = 0;
    dummy.lastObservedIsWolf = false;
    dummy.hasObservedForm = false;
    dummy.remoteTransformingObserved = false;
    dummy.lastObservedClothesVariant = -1;
    dummy.lastObservedShieldVariant = -1;
    dummy.lastObservedShieldDraw = false;
    dummy.lastLinkMatrices = {};
    dummy.lastLinkMatricesIsWolf = false;
    sDummies.erase(it);
}

void destroy_all_remote_link_dummies() {
    for (auto it = sActorDummies.begin(); it != sActorDummies.end();) {
        const std::string peerId = it->first;
        ++it;
        destroy_remote_link_actor_dummy(peerId);
    }

    for (auto& entry : sDummies) {
        RemoteLinkDummy& dummy = entry.second;
        destroy_model(dummy.body);
        destroy_model(dummy.hat);
        destroy_model(dummy.face);
        destroy_model(dummy.hand);
        destroy_model(dummy.sword);
        destroy_model(dummy.sheath);
        destroy_model(dummy.shield);
        dummy.swordInterp = {};
        dummy.sheathInterp = {};
        dummy.shieldInterp = {};
        free_remote_face_anim(dummy.humanFaceAnim);
        free_remote_face_anim(dummy.wolfFaceAnim);
        free_remote_eye_anims(dummy.humanEyeAnm, dummy.humanEyeAnmData);
        free_remote_eye_anims(dummy.wolfEyeAnm, dummy.wolfEyeAnmData);
        destroy_transform_anim(dummy.transform);
        dummy.bodyCollisionInitialized = false;
    }
    sDummies.clear();
}

void draw_remote_link_dummy(const std::string& peerId, const PeerPoseSnapshot& pose) {
    if (pose.valid && remote_link_actor_dummy_claimed(peerId)) {
        update_actor_dummy_collision(peerId, pose);
        return;
    }

    fopAc_ac_c* playerActor = dComIfGp_getPlayer(0);
    if (playerActor == nullptr) {
        if (pose.isTransforming) {
            DuskLog.info("Multiplayer transform debug dummy_skip peer={} reason=no_player", peerId);
        }
        return;
    }

    // is_peer_dummy_gameplay_ready() already checks this before this function
    // is reached, but this is re-checked here defensively: this cast is only
    // safe when player(0) is actually Link's actor, and that has not held
    // true in every code path observed around cutscene/event transitions.
    if (fopAcM_GetName(playerActor) != fpcNm_ALINK_e) {
        DuskLog.warn("Multiplayer remote Link dummy: refusing daAlink_c cast, player(0) name={}",
                     fopAcM_GetName(playerActor));
        return;
    }

    daAlink_c* link = static_cast<daAlink_c*>(playerActor);
    if (link->mProcID == daAlink_c::PROC_METAMORPHOSE || link->mProcID == daAlink_c::PROC_METAMORPHOSE_ONLY) {
        // Same use-after-free risk as in add_link_matrices (multiplayer.cpp):
        // daAlink_c::changeWolf()/changeLink() free and reassign
        // mpLinkModel/mSwordModel/mShieldModel partway through this state.
        // link here is the receiving client's own local player -- the clone
        // source for the dummy's body/hat/face/hand/sword/shield models --
        // so this guards against touching it mid-transform regardless of
        // what the remote peer is doing.
        if (pose.isTransforming) {
            DuskLog.info(
                "Multiplayer transform debug dummy_skip peer={} reason=local_transforming "
                "local_proc={}",
                peerId, link->mProcID);
        }
        return;
    }
    if (link->mClothesChangeWaitTimer != 0) {
        // Same shape of bug as the metamorphosis guard above, different
        // trigger: a confirmed crash (JKRArchive::findNameResource, fault
        // addr 0x8) happened in resolve_remote_sources()'s
        // dComIfG_getObjectRes("Kmdl", "al_head.bmd") lookup while the
        // local player was mid clothes/armor change (Magic Armor toggle).
        // mClothesChangeWaitTimer is a public daAlink_c field, set to 4 and
        // counted down across several frames during any such change
        // (d_a_alink.cpp, e.g. line 17808/17906/17939) -- the same kind of
        // multi-frame transient state as mProcID == PROC_METAMORPHOSE,
        // during which the "Kmdl" archive's resources can apparently be
        // briefly invalid/reloading. Skip entirely while it's nonzero,
        // same tradeoff already accepted for transforms and cutscenes.
        if (pose.isTransforming) {
            DuskLog.info(
                "Multiplayer transform debug dummy_skip peer={} reason=local_clothes_change timer={}",
                peerId, link->mClothesChangeWaitTimer);
        }
        return;
    }
    const bool localIsWolf = static_cast<bool>(link->checkWolf());

    // Hyrule Field (and other open continuous areas) are split into many
    // room numbers purely for load-streaming/culling -- they are not
    // separate coordinate spaces, and adjacent rooms stay loaded
    // simultaneously specifically so terrain doesn't pop across the
    // boundary. Requiring an exact room match made the dummy disappear the
    // instant a peer crossed into a different (but still currently loaded
    // and visible) room of the same open area. dComIfGp_roomControl_
    // checkRoomDisp() is the same check doors/carry objects already use to
    // ask "is this other room currently displayed for me", as opposed to
    // "is this my current room" (d_a_door_shutter.cpp, d_a_obj_carry.cpp) --
    // using it here instead of an equality check still refuses to draw in a
    // genuinely unrelated/unloaded room elsewhere in the same stage.
    if (!dComIfGp_roomControl_checkRoomDisp(pose.room)) {
        // Temporary diagnostic: confirm whether checkRoomDisp() is actually
        // the thing refusing to draw here, and with what room numbers, since
        // a prior attempt at this fix reportedly made no visible difference.
        if (sRoomSkipLogCount < 10) {
            ++sRoomSkipLogCount;
            DuskLog.info(
                "Multiplayer remote Link dummy: skipping draw, peer room={} not "
                "displayed (local room={}, stage={})",
                pose.room, fopAcM_GetRoomNo(link), pose.stage);
        }
        if (pose.isTransforming) {
            DuskLog.info(
                "Multiplayer transform debug dummy_skip peer={} reason=room_not_displayed "
                "peer_room={} local_room={} stage={}",
                peerId, pose.room, fopAcM_GetRoomNo(link), pose.stage);
        }
        return;
    }

    auto dummyIt = sDummies.find(peerId);
    if (dummyIt != sDummies.end() && dummyIt->second.stage != pose.stage) {
        // Only the stage matters for whether the dummy's cloned model/arc
        // resources are still valid -- see the room-display comment above.
        // Resetting on every room-number change within the same open area
        // would just trade one visible glitch (disappearing entirely) for
        // another (stutter on every room crossing).
        destroy_remote_link_dummy(peerId);
        dummyIt = sDummies.end();
    }

    if (dummyIt == sDummies.end()) {
        RemoteLinkDummy& newDummy = sDummies[peerId];
        newDummy.stage = pose.stage;
        newDummy.room = pose.room;
        update_remote_body_collision(newDummy, pose, playerActor);
        if (pose.isTransforming) {
            DuskLog.info(
                "Multiplayer transform debug dummy_created peer={} seq={} from_wolf={} to_wolf={} "
                "is_wolf={} returning_until_next_draw=1",
                peerId, pose.sequence, pose.transformFromWolf, pose.transformToWolf, pose.isWolf);
        }
        return;
    }

    RemoteLinkDummy& dummy = dummyIt->second;
    update_remote_body_collision(dummy, pose, playerActor);

    if (pose.isTransforming && (!dummy.remoteTransformingObserved || !dummy.transform.active)) {
        const bool firstTransformPacket = !dummy.remoteTransformingObserved;
        dummy.remoteTransformingObserved = true;
        dummy.hasObservedForm = true;
        dummy.lastObservedIsWolf = pose.transformFromWolf;
        start_remote_transform(dummy, pose.transformFromWolf, pose.transformToWolf);
        if (pose.transformProcVar5 != 0 || pose.isWolf == pose.transformToWolf) {
            dummy.transform.secondPhase = true;
            dummy.transform.initialized = false;
            DuskLog.info(
                "Multiplayer transform debug start_at_sender_phase2 peer={} sender_wolf={} "
                "target_wolf={} proc_v5={} frame={}",
                peerId, pose.isWolf, pose.transformToWolf, pose.transformProcVar5,
                pose.transformFrame);
        }
        dummy.drawLogCount = 0;
        dummy.matrixRejectLogCount = 0;
        dummy.swordLogCount = 0;
        if (pose.transformToWolf) {
            dummy.wolfDrawLogCount = 0;
        }
        if (firstTransformPacket) {
            DuskLog.info(
                "Multiplayer remote Link dummy: peer transform started peer={} from_wolf={} "
                "to_wolf={} peer_wolf={} pose_mtx_valid={} cache_valid={} cache_wolf={}",
                peerId, pose.transformFromWolf, pose.transformToWolf, pose.isWolf,
                pose.linkMatrices.valid, dummy.lastLinkMatrices.valid, dummy.lastLinkMatricesIsWolf);
        }
    } else if (!pose.isTransforming) {
        dummy.remoteTransformingObserved = false;
    }

    if (!dummy.hasObservedForm) {
        dummy.lastObservedIsWolf = pose.isWolf;
        dummy.hasObservedForm = true;
    } else if (!pose.isTransforming && dummy.lastObservedIsWolf != pose.isWolf) {
        const bool fromWolf = dummy.lastObservedIsWolf;
        dummy.lastObservedIsWolf = pose.isWolf;
        dummy.drawLogCount = 0;
        dummy.matrixRejectLogCount = 0;
        dummy.swordLogCount = 0;
        if (pose.isWolf) {
            dummy.wolfDrawLogCount = 0;
        }
        if (!pose.linkMatrices.valid) {
            start_remote_transform(dummy, fromWolf, pose.isWolf);
        }
        DuskLog.info(
            "Multiplayer remote Link dummy: peer form changed peer={} peer_wolf={} "
            "pose_mtx_valid={} cache_valid={} cache_wolf={} replay_transform={}",
            peerId, pose.isWolf, pose.linkMatrices.valid, dummy.lastLinkMatrices.valid,
            dummy.lastLinkMatricesIsWolf, !pose.linkMatrices.valid);
    }

    if (draw_remote_transform_body(dummy, peerId, pose, link, localIsWolf)) {
        return;
    }

    if (dummy.lastObservedClothesVariant != pose.clothesVariant ||
        dummy.lastObservedShieldVariant != pose.shieldVariant ||
        dummy.lastObservedShieldDraw != pose.shieldDraw)
    {
        dummy.lastObservedClothesVariant = pose.clothesVariant;
        dummy.lastObservedShieldVariant = pose.shieldVariant;
        dummy.lastObservedShieldDraw = pose.shieldDraw;
        dummy.drawLogCount = 0;
        dummy.matrixRejectLogCount = 0;
        dummy.swordLogCount = 0;
        DuskLog.info(
            "Multiplayer remote Link dummy: equipment variant changed peer={} "
            "clothes_variant={} shield_variant={} shield_draw={} pose_mtx_valid={}",
            peerId, pose.clothesVariant, pose.shieldVariant, pose.shieldDraw,
            pose.linkMatrices.valid);
    }

    if (!dummy.loggedReady) {
        dummy.drawLogCount = 0;
        dummy.matrixRejectLogCount = 0;
        dummy.swordLogCount = 0;
        dummy.loggedReady = true;
        DuskLog.info(
            "Multiplayer remote Link dummy: peer form state ready peer_wolf={} peer={}",
            pose.isWolf, peerId);
    }

    const RemoteLinkSources sources = resolve_remote_sources(pose);
    const bool drawHumanParts = sources.humanParts;
    const u32 bodyDiffFlags = pose.isWolf ? 0x11020284 : 0x11000084;
    J3DModel* body = get_or_create_model_from_data(dummy.body, sources.body, 0x200000, bodyDiffFlags);
    J3DModel* hat = drawHumanParts ? get_or_create_model_from_data(dummy.hat, sources.hat, 0x100000) : nullptr;
    J3DModel* face = drawHumanParts ? get_or_create_model_from_data(dummy.face, sources.face, 0x100000) : nullptr;
    J3DModel* hand = drawHumanParts ? get_or_create_model_from_data(dummy.hand, sources.hand, 0x100000) : nullptr;
    J3DModel* sword = drawHumanParts && pose.swordDraw ? get_or_create_model_from_data(dummy.sword, sources.sword, 0x100000) : nullptr;
    J3DModel* sheath = drawHumanParts && pose.swordDraw ? get_or_create_model_from_data(dummy.sheath, sources.sheath, 0x100000) : nullptr;
    J3DModel* shield = drawHumanParts && pose.shieldDraw ? get_or_create_model_from_data(dummy.shield, sources.shield, 0x100000) : nullptr;
    prepare_remote_form_resources(dummy, pose, link, body, hat, face, sword);
    if (!drawHumanParts) {
        destroy_model(dummy.hat);
        destroy_model(dummy.face);
        destroy_model(dummy.hand);
    }
    if (!drawHumanParts || !pose.swordDraw) {
        destroy_model(dummy.sword);
        destroy_model(dummy.sheath);
    }
    if (!drawHumanParts || !pose.shieldDraw) {
        destroy_model(dummy.shield);
    }
    if (body == nullptr) {
        log_draw_skip(dummy, "body_null", peerId, pose, sources, body, localIsWolf, nullptr, false);
        return;
    }

    dKy_tevstr_c dummyTevStr = build_dummy_tev_str(pose);

    const RemoteLinkMatrixSnapshot* matrices = nullptr;
    const bool hasRemoteMatrices = pose.linkMatrices.valid;
    if (hasRemoteMatrices) {
        matrices = &pose.linkMatrices;
    } else if (dummy.lastLinkMatrices.valid && dummy.lastLinkMatricesIsWolf == pose.isWolf) {
        matrices = &dummy.lastLinkMatrices;
    }

    bool usedRemoteMatrices = false;
    const bool usingCachedMatrices = matrices != nullptr && !hasRemoteMatrices;
    if (matrices != nullptr) {
        usedRemoteMatrices = copy_remote_model_matrices(body, matrices->body);
        if (!usedRemoteMatrices) {
            log_draw_skip(dummy, "body_matrix_rejected", peerId, pose, sources, body, localIsWolf, matrices,
                          usingCachedMatrices);
            return;
        }
        if (hasRemoteMatrices) {
            dummy.lastLinkMatrices = pose.linkMatrices;
            dummy.lastLinkMatricesIsWolf = pose.isWolf;
        }
    } else {
        log_draw_skip(dummy, "no_matrices_for_form", peerId, pose, sources, body, localIsWolf, matrices,
                      usingCachedMatrices);
        return;
    }
    const bool bodySharesActiveLocalData =
        !pose.isWolf && shares_active_local_link_model_data(body, link);
    entry_model_without_calc(body, &dummyTevStr, /*rebindLight=*/true, /*useDarkList=*/pose.isWolf,
                             /*suppressMaterialAnm=*/true);
    log_draw_reached(dummy, peerId, pose, sources, body, localIsWolf, matrices, usingCachedMatrices);

#if TARGET_PC
    const bool drawSwordAndSheathThisFrame = dusk::frame_interp::is_sim_frame();
#else
    const bool drawSwordAndSheathThisFrame = true;
#endif

    if (hat != nullptr) {
        bool drewHat = false;
        if (usedRemoteMatrices && matrices->hat.valid) {
            drewHat = copy_remote_model_matrices(hat, matrices->hat);
        }
        if (drewHat) {
            entry_model_without_calc(hat, &dummyTevStr, /*rebindLight=*/true,
                                     /*useDarkList=*/false, /*suppressMaterialAnm=*/true);
        }
    }

    if (face != nullptr) {
        bool drewFace = false;
        if (usedRemoteMatrices && matrices->face.valid) {
            drewFace = copy_remote_model_matrices(face, matrices->face);
        }
        if (drewFace) {
            entry_model_without_calc(face, &dummyTevStr, /*rebindLight=*/true,
                                     /*useDarkList=*/false, /*suppressMaterialAnm=*/true);
        }
    }

    if (hand != nullptr) {
        bool drewHand = false;
        if (usedRemoteMatrices && matrices->hand.valid) {
            drewHand = copy_remote_model_matrices(hand, matrices->hand);
        }
        if (drewHand) {
            entry_model_without_calc(hand, &dummyTevStr, /*rebindLight=*/true,
                                     /*useDarkList=*/false, /*suppressMaterialAnm=*/true);
        }
    }

    if (sword != nullptr && !skip_remote_sword_draw() && drawSwordAndSheathThisFrame) {
        bool drewSword = false;
        if (usedRemoteMatrices && matrices->sword.valid) {
            drewSword = copy_remote_model_matrices(sword, matrices->sword);
        }
        if (drewSword) {
            const bool woodSword = is_wood_sword_variant(pose.swordVariant);
            SwordShapeVisibility visibilitySnapshot =
                apply_sword_shape_visibility(sword, pose.swordOut, woodSword);
            // Form mismatch (e.g. local client is Wolf, peer dummy is Human
            // holding a sword) must never be treated as shared state: the
            // pointer-identity check in local_link_draws_matching_sword_item()
            // can still match an idle/cached local sword model even when the
            // local player isn't actually drawing a sword this tick (Wolf
            // form draws no sword at all), in which case nothing ever calls
            // setLightTevColorType_MAJI() on that shared material and the
            // dummy's sword comes out with no light/color whatsoever. Require
            // the forms to match before trusting the shared-data path, same
            // guard pattern as bodySharesActiveLocalData's `!pose.isWolf`.
            const bool swordSharesActiveLocalData =
                pose.isWolf == localIsWolf &&
                local_link_draws_matching_sword_item(sword, link->mSwordModel, link);
            entry_model_without_calc(sword, &dummyTevStr,
                                     /*rebindLight=*/!swordSharesActiveLocalData,
                                     /*useDarkList=*/false, swordSharesActiveLocalData,
                                     /*entryOnNonSimFrame=*/false);
            if (!pose.isWolf && localIsWolf) {
                register_remote_equipment_interp(dummy.swordInterp, sword, dummyTevStr,
                                                 swordSharesActiveLocalData);
            }
            restore_sword_shape_visibility(visibilitySnapshot);
            if (dummy.swordLogCount < 8) {
                // Temporary diagnostic for the "flat yellow sword" report:
                // confirms which variant/material/matrix-source combination
                // is actually in play, AND whether the dummy's sword model
                // data is literally the same object as the local player's
                // own real sword (the foundational assumption every theory
                // about this bug rests on -- get direct pointer-identity
                // evidence instead of continuing to assume it).
                ++dummy.swordLogCount;
                const bool sharesLocalSwordData =
                    link->mSwordModel != nullptr &&
                    sword->getModelData() == link->mSwordModel->getModelData();
                DuskLog.info(
                    "Multiplayer remote Link dummy: sword draw peer={} variant={} "
                    "wood={} out={} dummy_joints={} dummy_weights={} matrix_joints={} "
                    "matrix_weights={} shares_local_sword_data={} "
                    "dummy_data={} local_data={}",
                    peerId, pose.swordVariant, woodSword, pose.swordOut,
                    sword->getModelData()->getJointNum(), sword->getModelData()->getWEvlpMtxNum(),
                    matrices->sword.jointCount, matrices->sword.weightCount,
                    sharesLocalSwordData, static_cast<void*>(sword->getModelData()),
                    static_cast<void*>(link->mSwordModel != nullptr
                                            ? link->mSwordModel->getModelData()
                                            : nullptr));
            }
        } else if (dummy.swordLogCount < 8) {
            ++dummy.swordLogCount;
            DuskLog.info(
                "Multiplayer remote Link dummy: sword present but NOT drawn peer={} "
                "variant={} out={} usedRemoteMatrices={} sword_matrices_valid={}",
                peerId, pose.swordVariant, pose.swordOut, usedRemoteMatrices,
                matrices != nullptr && matrices->sword.valid);
        }
    }

    if (sheath != nullptr && drawSwordAndSheathThisFrame) {
        bool drewSheath = false;
        if (usedRemoteMatrices && matrices->sheath.valid) {
            drewSheath = copy_remote_model_matrices(sheath, matrices->sheath);
        }
        if (drewSheath) {
            // Same form-mismatch guard as the sword case above.
            const bool sheathSharesActiveLocalData =
                pose.isWolf == localIsWolf &&
                local_link_draws_matching_sword_item(sheath, link->mSheathModel, link);
            entry_model_without_calc(sheath, &dummyTevStr,
                                     /*rebindLight=*/!sheathSharesActiveLocalData,
                                     /*useDarkList=*/false, sheathSharesActiveLocalData,
                                     /*entryOnNonSimFrame=*/false);
            if (!pose.isWolf && localIsWolf) {
                register_remote_equipment_interp(dummy.sheathInterp, sheath, dummyTevStr,
                                                 sheathSharesActiveLocalData);
            }
        }
    }

    if (shield != nullptr) {
        bool drewShield = false;
        if (usedRemoteMatrices && matrices->shield.valid) {
            drewShield = copy_remote_model_matrices(shield, matrices->shield);
        }
        if (drewShield) {
            const bool shieldSharesActiveLocalData =
                pose.isWolf == localIsWolf && local_link_draws_matching_shield_item(shield, link);
            ShapeVisibilitySnapshot shieldVisibility = show_model_shapes(shield);
            entry_model_without_calc(shield, &dummyTevStr,
                                     /*rebindLight=*/!shieldSharesActiveLocalData,
                                     /*useDarkList=*/false, /*suppressMaterialAnm=*/true);
            restore_model_shapes(shieldVisibility);
            if (!pose.isWolf && localIsWolf) {
                register_remote_equipment_interp(dummy.shieldInterp, shield, dummyTevStr,
                                                 shieldSharesActiveLocalData);
            }
        }
    }

    if (dummy.drawLogCount < 5) {
        ++dummy.drawLogCount;
        DuskLog.info(
            "Multiplayer remote Link dummy: drew copy #{} peer={} matrices={} pos=({}, {}, {}) angleY={}",
            dummy.drawLogCount, peerId,
            usingCachedMatrices ? "cached-remote" : "remote",
            pose.x, pose.y, pose.z, pose.angleY);
    }
}

}  // namespace dusk::multiplayer
