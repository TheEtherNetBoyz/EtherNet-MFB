/**
 * @file d_a_remote_link.cpp
 *
 * Remote Link visual actor.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "d/dolzel_rel.h"  // IWYU pragma: keep

#include "d/actor/d_a_remote_link.h"

#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphLoader/J3DAnmLoader.h"
#include "JSystem/J3DGraphLoader/J3DModelLoader.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_midna.h"
#include "d/actor/d_a_nbomb.h"
#include "d/d_bomb.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_particle.h"
#include "d/d_s_play.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_draw_priority.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"
#include "res/Object/AlAnm.h"
#include "res/Object/Alink.h"
#include "res/Object/Always.h"
#include "res/Object/Wmdl.h"
#include "Z2AudioLib/Z2AudioMgr.h"
#include "Z2AudioLib/Z2Audience.h"
#include "Z2AudioLib/Z2SoundInfo.h"
#include "dusk/frame_interpolation.h"
#include "dusk/logging.h"
#include "dusk/multiplayer/multiplayer.hpp"
#include <dvd.h>

namespace {

static int const l_maxRemoteLinkActors = 8;
static u32 const l_remoteLinkAnimBufferSize = 0x10800;
static f32 const l_walkSpeedThreshold = 0.5f;
static f32 const l_dashSpeedThreshold = 4.0f;
static int const l_eqArcAlink = 0;
static int const l_eqArcKmdl = 1;
static int const l_eqArcCarvingShield = 2;
static int const l_eqArcOrdonShield = 3;
static int const l_eqArcHylianShield = 4;
static int sCreateLogCount;
static int sDrawLogCount;
static int sCalcLogCount;
static int sItemActorRejectLogCount;
static int sRemoteBombFuseLogCount;
static int sLiveRemoteLinkActors;
static int sPvpTargetLogCount;
static uint32_t sPvpTargetLogTicks;
static f32 const l_remoteMotionAudioVolumeScale = 0.75f;

constexpr f32 remoteMatrixFallbackInterpolationStep(u32 i_elapsedTicks,
                                                    u32 i_durationTicks) {
    if (i_durationTicks == 0 || i_elapsedTicks >= i_durationTicks) {
        return 1.0f;
    }
    return static_cast<f32>(i_elapsedTicks) / static_cast<f32>(i_durationTicks);
}

static_assert(remoteMatrixFallbackInterpolationStep(0, 1) == 0.0f);
static_assert(remoteMatrixFallbackInterpolationStep(1, 1) == 1.0f);
static_assert(remoteMatrixFallbackInterpolationStep(1, 2) == 0.5f);
static_assert(remoteMatrixFallbackInterpolationStep(2, 2) == 1.0f);
static_assert(remoteMatrixFallbackInterpolationStep(3, 2) == 1.0f);
static_assert(remoteMatrixFallbackInterpolationStep(0, 0) == 1.0f);

static dCcD_SrcCyl l_pvpTargetCylSrc = {
    {
        {0, {{AT_TYPE_0, 0, 0}, {0xD8FBFDFF, 5}, 0x73}},
        {dCcD_SE_NONE, 0, 0, 0, {0}},
        {dCcD_SE_NONE, 0, 0, 0, {0}},
        {0},
    },
    {
        {
            {0.0f, 0.0f, 0.0f},
            35.0f,
            180.0f,
        },
    }
};

static void daRemoteLink_pvpTargetHitCallback(fopAc_ac_c* i_targetActor,
                                              dCcD_GObjInf* i_targetObj,
                                              fopAc_ac_c* i_attackActor,
                                              dCcD_GObjInf* i_attackObj) {
    (void)i_targetObj;
    dusk::multiplayer::report_remote_link_pvp_target_hit(i_targetActor, i_attackActor,
                                                         i_attackObj);
}

static bool isValidRemoteBck(u16 i_resId) {
    return i_resId != 0 && i_resId != 0xFFFF;
}

static bool isBottleItem(u16 i_itemNo) {
    return i_itemNo >= dItemNo_EMPTY_BOTTLE_e && i_itemNo <= dItemNo_OIL_e;
}

static bool isRemoteBombActorKind(int i_kind) {
    return i_kind >= 2 && i_kind <= 4;
}

static f32 remoteBombPulse(int i_exTime, int i_flash, int i_fallbackTicks) {
    daAlink_c* player = daAlink_getAlinkActorClass();
    const int explodeTime = player != NULL ? player->getBombExplodeTime() : 0;
    if (i_exTime >= 0 && explodeTime > 8) {
        if (i_exTime > explodeTime) {
            return 1.0f - std::fabs(std::cos(((static_cast<f32>(i_exTime - explodeTime)) * M_PI) /
                                             static_cast<f32>(std::max(1, explodeTime / 2))));
        }
        if (i_exTime > explodeTime / 2) {
            return 1.0f - std::fabs(std::cos(((static_cast<f32>(i_exTime - explodeTime / 2)) * M_PI) /
                                             static_cast<f32>(std::max(1, explodeTime / 4))));
        }
        if (i_exTime > explodeTime / 4) {
            return std::fabs(std::sin(((static_cast<f32>(i_exTime - explodeTime / 4)) * M_PI) /
                                      static_cast<f32>(std::max(1, explodeTime / 7))));
        }
        return std::fabs(std::sin(((static_cast<f32>(i_exTime - explodeTime / 7)) * M_PI) /
                                  static_cast<f32>(std::max(1, explodeTime / 8))));
    }
    if (i_flash >= 0) {
        return static_cast<f32>(std::clamp(i_flash, 0, 15)) / 15.0f;
    }
    return std::fabs(std::sin(static_cast<f32>(i_fallbackTicks) * 0.22f));
}

static bool remoteBombGameplayEffectsEnabled() {
    return true;
}

static bool remoteBombRealActorEnabled() {
    return true;
}

static bool envValueIsOff(const char* i_value) {
    if (i_value == NULL || i_value[0] == '\0') {
        return false;
    }

    char value[8] = {};
    size_t i = 0;
    for (; i < sizeof(value) - 1 && i_value[i] != '\0'; ++i) {
        const char c = i_value[i];
        value[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    value[i] = '\0';

    return value[0] == '0' ||
           (value[0] == 'n' && value[1] == 'o' && value[2] == '\0') ||
           (value[0] == 'o' && value[1] == 'f' && value[2] == 'f' && value[3] == '\0') ||
           (value[0] == 'f' && value[1] == 'a' && value[2] == 'l' && value[3] == 's' &&
            value[4] == 'e' && value[5] == '\0');
}

static bool remoteMatrixLocalInterpolationEnabled() {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* value = std::getenv("DUSK_MP_MATRIX_LOCAL_INTERP");
        s_enabled = envValueIsOff(value) ? 0 : 1;
        if (s_enabled != 0) {
            DuskLog.info("RemoteLink: local-space matrix interpolation enabled");
        } else {
            DuskLog.info("RemoteLink: local-space matrix interpolation force disabled");
        }
    }
    return s_enabled != 0;
}

static u32 remoteBombCreateParam(int i_kind) {
    switch (i_kind) {
    case 3:
        return dBomb_c::PRM_WATER_BOMB_PLAYER;
    case 4:
        return dBomb_c::PRM_INSECT_BOMB_PLAYER;
    case 2:
    default:
        return dBomb_c::PRM_BOMB_WAIT;
    }
}

static u32 remoteBombExplosionParam(int i_kind) {
    return i_kind == 3 ? dBomb_c::PRM_WATER_BOMB_EXPLODE : dBomb_c::PRM_NORMAL_BOMB_EXPLODE;
}

static bool isRemoteTransformProc(int i_procId) {
    return i_procId == daAlink_c::PROC_METAMORPHOSE ||
           i_procId == daAlink_c::PROC_METAMORPHOSE_ONLY;
}

static bool isRemotePvpKnockdownProc(int i_procId) {
    return i_procId == daAlink_c::PROC_LARGE_DAMAGE ||
           i_procId == daAlink_c::PROC_LARGE_DAMAGE_WALL ||
           i_procId == daAlink_c::PROC_LARGE_DAMAGE_UP ||
           i_procId == daAlink_c::PROC_WOLF_LARGE_DAMAGE_UP;
}

static bool isRemoteDeadProc(int i_procId) {
    return i_procId == daAlink_c::PROC_DEAD;
}

static bool isRemoteLinkSceneUnsafe() {
    return dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() || fopOvlpM_IsDoingReq() ||
           dComIfGp_event_runCheck();
}

static void zeroColor(GXColorS10* o_color) {
    o_color->r = 0;
    o_color->g = 0;
    o_color->b = 0;
    o_color->a = 0;
}

static void setRemoteMatrixWorldAxisRot(daRemoteLink_c* i_actor, MtxP i_mtx, s16 i_rotX,
                                        s16 i_rotY, s16 i_rotZ) {
    cXyz origin;
    mDoMtx_multVecZero(i_mtx, &origin);
    mDoMtx_stack_c::transS(origin);
    mDoMtx_stack_c::YrotM(i_actor->shape_angle.y);
    mDoMtx_stack_c::ZXYrotM(i_rotX, i_rotY, i_rotZ);
    mDoMtx_stack_c::YrotM(-i_actor->shape_angle.y);
    mDoMtx_stack_c::transM(-origin.x, -origin.y, -origin.z);
    mDoMtx_stack_c::concat(i_mtx);
    mDoMtx_copy(mDoMtx_stack_c::get(), i_mtx);
}

static int daRemoteLink_headModelCallBack(J3DJoint* i_joint, int i_after) {
    if (i_after != 0 || i_joint == NULL) {
        return 1;
    }

    daRemoteLink_c* link =
        reinterpret_cast<daRemoteLink_c*>(j3dSys.getModel()->getUserArea());
    if (link != NULL) {
        link->headModelCallBack(i_joint->getJntNo());
    }
    return 1;
}

static bool isRemoteLinkMotionAudio(const dusk::multiplayer::RemoteAudioEvent& i_event) {
    switch (i_event.sourceKind) {
    case dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_SOUND:
    case dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_SWORD:
    case dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_COLLISION:
    case dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_HIT_ITEM:
        return true;
    default:
        break;
    }

    switch (i_event.soundId) {
    case Z2SE_AL_KAITENGIRI:
    case Z2SE_AL_KAITEN_L_SLASH:
    case Z2SE_AL_LTN_KAITENGIRI:
    case Z2SE_FN_WALK_DUMMY:
    case Z2SE_FN_JUMP_DUMMY:
    case Z2SE_FN_BOUND_DUMMY:
    case Z2SE_FN_HAND_DUMMY:
    case Z2SE_WL_WALK_L_DUMMY:
    case Z2SE_WL_WALK_R_DUMMY:
    case Z2SE_WL_RUN_L_DUMMY:
    case Z2SE_WL_RUN_R_DUMMY:
        return true;
    default:
        return false;
    }
}

static u32 getSoundDistVolBit(JAISoundID i_soundId) {
    const JAUAudibleParam params = Z2GetSoundInfo()->getAudibleSwFull(i_soundId);
    const u16 distFlags = params.field_0x0.half.f1;
    if (distFlags != 0) {
        if ((distFlags & 0x7) != 0) {
            return distFlags & 0x7;
        }
        if ((distFlags & 0x70) != 0) {
            return ((distFlags & 0x70) >> 4) + 7;
        }
    }

    return 0;
}

static f32 calcRemoteLinkVoiceMatchedVolume(Z2Audience* i_audience, const Vec& i_relPos) {
    Vec volumePos = i_relPos;
    volumePos.z += i_audience->getAudioCamera(0)->getVolCenterZ();

    const u32 voiceDistVolBit = getSoundDistVolBit(Z2SE_AL_V_ATTACK_S);
    const f32 distance = sqrtf(volumePos.x * volumePos.x + volumePos.y * volumePos.y +
                               volumePos.z * volumePos.z);
    return i_audience->calcVolume_(distance, voiceDistVolBit);
}

static void calcRemoteLinkAudioMix(const cXyz& i_pos,
                                   const dusk::multiplayer::RemoteAudioEvent& i_event,
                                   f32* o_volume, f32* o_pan, f32* o_dolby) {
    *o_volume = 1.0f;
    *o_pan = -1.0f;
    *o_dolby = -1.0f;

    Z2Audience* audience = Z2GetAudience();
    if (audience == NULL || !isRemoteLinkMotionAudio(i_event)) {
        return;
    }

    Vec absPos = i_pos;
    Vec relPos;
    audience->convertAbsToRel(absPos, &relPos, 0);

    *o_volume = std::clamp(calcRemoteLinkVoiceMatchedVolume(audience, relPos) *
                               l_remoteMotionAudioVolumeScale,
                           0.0f, 1.0f);
    *o_pan = audience->calcRelPosPan(relPos, 0);
    *o_dolby = audience->calcRelPosDolby(relPos, 0);
}

static J3DShape* getMaterialShape(J3DModelData* i_modelData, u16 i_materialNo) {
    if (i_modelData == NULL || i_materialNo >= i_modelData->getMaterialNum()) {
        return NULL;
    }

    J3DMaterial* material = i_modelData->getMaterialNodePointer(i_materialNo);
    if (material == NULL) {
        return NULL;
    }

    return material->getShape();
}

static void hideMaterialShape(J3DModelData* i_modelData, u16 i_materialNo) {
    if (i_modelData == NULL || i_materialNo >= i_modelData->getMaterialNum()) {
        return;
    }

    J3DShape* shape = getMaterialShape(i_modelData, i_materialNo);
    if (shape != NULL) {
        shape->hide();
    }
}

static void showOnlyMaterialShape(J3DModelData* i_modelData, u16 i_materialNo, u16 i_count) {
    if (i_modelData == NULL) {
        return;
    }

    u16 materialCount = i_modelData->getMaterialNum();
    for (u16 i = 0; i < i_count && i < materialCount; ++i) {
        J3DShape* shape = getMaterialShape(i_modelData, i);
        if (shape == NULL) {
            continue;
        }

        if (i == i_materialNo) {
            shape->show();
        } else {
            shape->hide();
        }
    }
}

static void applyRideMidnaShapeVisibility(J3DModel* i_handModel, J3DModel* i_hairModel,
                                          int i_hairShape) {
    if (i_handModel != NULL) {
        J3DModelData* modelData = i_handModel->getModelData();
        for (u16 i = 0; i < 4; ++i) {
            hideMaterialShape(modelData, i);
        }
    }

    if (i_hairModel != NULL) {
        if (i_hairShape < 0 || i_hairShape > 2) {
            i_hairShape = 0;
        }
        showOnlyMaterialShape(i_hairModel->getModelData(), static_cast<u16>(i_hairShape), 3);
    }
}

static void flatMatrixToMtx(const float* i_values, Mtx o_mtx) {
    size_t cursor = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            o_mtx[row][col] = i_values[cursor++];
        }
    }
}

struct RemoteLocalJointTransform {
    cXyz translation;
    cXyz scale;
    Quaternion rotation;
};

struct RemoteInterpMtx {
    Mtx value;
};

static bool matrixTranslationFinite(CMtxP i_mtx) {
    return std::isfinite(i_mtx[0][3]) && std::isfinite(i_mtx[1][3]) &&
           std::isfinite(i_mtx[2][3]);
}

static f32 matrixTranslationDistanceSq(CMtxP i_a, CMtxP i_b) {
    const f32 dx = i_a[0][3] - i_b[0][3];
    const f32 dy = i_a[1][3] - i_b[1][3];
    const f32 dz = i_a[2][3] - i_b[2][3];
    return dx * dx + dy * dy + dz * dz;
}

static void setMatrixTranslation(Mtx io_mtx, const cXyz& i_pos) {
    io_mtx[0][3] = i_pos.x;
    io_mtx[1][3] = i_pos.y;
    io_mtx[2][3] = i_pos.z;
}

static void setMatrixHorizontalTranslation(Mtx io_mtx, const cXyz& i_pos) {
    io_mtx[0][3] = i_pos.x;
    io_mtx[2][3] = i_pos.z;
}

static bool snapshotMatrixToMtx(const std::array<float, 12>& i_source, Mtx o_mtx) {
    flatMatrixToMtx(i_source.data(), o_mtx);
    return matrixTranslationFinite(o_mtx);
}

static bool snapshotJointMatrixToMtx(const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source,
                                     u16 i_joint, Mtx o_mtx) {
    const size_t offset = static_cast<size_t>(i_joint) * 12;
    if (offset + 12 > i_source.joints.size()) {
        return false;
    }
    flatMatrixToMtx(i_source.joints.data() + offset, o_mtx);
    return matrixTranslationFinite(o_mtx);
}

static bool matrixValuesNearlyEqual(const float* i_a, const float* i_b, size_t i_count) {
    constexpr float kDuplicateMatrixEpsilon = 0.000001f;
    for (size_t i = 0; i < i_count; ++i) {
        if (std::fabs(i_a[i] - i_b[i]) > kDuplicateMatrixEpsilon) {
            return false;
        }
    }
    return true;
}

static bool remoteModelMatrixSnapshotNearlyEqual(
    const dusk::multiplayer::RemoteModelMatrixSnapshot& i_a,
    const dusk::multiplayer::RemoteModelMatrixSnapshot& i_b) {
    if (!i_a.valid || !i_b.valid || i_a.jointCount != i_b.jointCount ||
        i_a.weightCount != i_b.weightCount || i_a.joints.size() != i_b.joints.size() ||
        i_a.weights.size() != i_b.weights.size())
    {
        return false;
    }

    return matrixValuesNearlyEqual(i_a.base.data(), i_b.base.data(), i_a.base.size()) &&
           (i_a.joints.empty() ||
            matrixValuesNearlyEqual(i_a.joints.data(), i_b.joints.data(), i_a.joints.size())) &&
           (i_a.weights.empty() ||
            matrixValuesNearlyEqual(i_a.weights.data(), i_b.weights.data(), i_a.weights.size()));
}

static bool remoteModelSnapshotHasUsableWeights(
    const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source) {
    return i_source.weights.size() == static_cast<size_t>(i_source.weightCount) * 12 ||
           (i_source.weightsOmitted && i_source.weights.empty());
}

static f32 columnLength(CMtxP i_mtx, int i_col) {
    return std::sqrt(i_mtx[0][i_col] * i_mtx[0][i_col] +
                     i_mtx[1][i_col] * i_mtx[1][i_col] +
                     i_mtx[2][i_col] * i_mtx[2][i_col]);
}

static bool decomposeMatrixTRS(CMtxP i_mtx, RemoteLocalJointTransform* o_transform) {
    if (o_transform == NULL || !matrixTranslationFinite(i_mtx)) {
        return false;
    }

    o_transform->translation.set(i_mtx[0][3], i_mtx[1][3], i_mtx[2][3]);
    o_transform->scale.set(columnLength(i_mtx, 0), columnLength(i_mtx, 1),
                           columnLength(i_mtx, 2));
    if (o_transform->scale.x <= 0.00001f || o_transform->scale.y <= 0.00001f ||
        o_transform->scale.z <= 0.00001f)
    {
        return false;
    }

    Mtx rot;
    for (int row = 0; row < 3; ++row) {
        rot[row][0] = i_mtx[row][0] / o_transform->scale.x;
        rot[row][1] = i_mtx[row][1] / o_transform->scale.y;
        rot[row][2] = i_mtx[row][2] / o_transform->scale.z;
        rot[row][3] = 0.0f;
    }
    QUATMtx(&o_transform->rotation, rot);
    return std::isfinite(o_transform->rotation.x) &&
           std::isfinite(o_transform->rotation.y) &&
           std::isfinite(o_transform->rotation.z) &&
           std::isfinite(o_transform->rotation.w);
}

static void composeMatrixTRS(const RemoteLocalJointTransform& i_transform, Mtx o_mtx) {
    MTXQuat(o_mtx, &i_transform.rotation);
    for (int row = 0; row < 3; ++row) {
        o_mtx[row][0] *= i_transform.scale.x;
        o_mtx[row][1] *= i_transform.scale.y;
        o_mtx[row][2] *= i_transform.scale.z;
    }
    o_mtx[0][3] = i_transform.translation.x;
    o_mtx[1][3] = i_transform.translation.y;
    o_mtx[2][3] = i_transform.translation.z;
}

static RemoteLocalJointTransform interpolateLocalTransform(
    const RemoteLocalJointTransform& i_prev, const RemoteLocalJointTransform& i_curr, f32 i_alpha) {
    const f32 invAlpha = 1.0f - i_alpha;
    RemoteLocalJointTransform out;
    out.translation.set(i_prev.translation.x * invAlpha + i_curr.translation.x * i_alpha,
                        i_prev.translation.y * invAlpha + i_curr.translation.y * i_alpha,
                        i_prev.translation.z * invAlpha + i_curr.translation.z * i_alpha);
    out.scale.set(i_prev.scale.x * invAlpha + i_curr.scale.x * i_alpha,
                  i_prev.scale.y * invAlpha + i_curr.scale.y * i_alpha,
                  i_prev.scale.z * invAlpha + i_curr.scale.z * i_alpha);
    QUATSlerp(&i_prev.rotation, &i_curr.rotation, &out.rotation, i_alpha);
    return out;
}

static void fillJointParentMap(J3DJoint* i_joint, int i_parent,
                               std::vector<int16_t>& o_parents) {
    if (i_joint == NULL) {
        return;
    }

    const u16 jointNo = i_joint->getJntNo();
    if (jointNo < o_parents.size()) {
        o_parents[jointNo] = static_cast<int16_t>(i_parent);
        for (J3DJoint* child = i_joint->getChild(); child != NULL; child = child->getYounger()) {
            fillJointParentMap(child, jointNo, o_parents);
        }
    }
}

static int daRemoteLink_Create(fopAc_ac_c* i_this) {
    daRemoteLink_c* actor = static_cast<daRemoteLink_c*>(i_this);
    fopAcM_RegisterCreateID(actor, "RemoteLink");
    return actor->create();
}

static int daRemoteLink_Delete(daRemoteLink_c* i_this) {
    fopAcM_RegisterDeleteID(i_this, "RemoteLink");
    return i_this->Delete();
}

static int daRemoteLink_Execute(daRemoteLink_c* i_this) {
    return i_this->Execute();
}

static int daRemoteLink_IsDelete(daRemoteLink_c*) {
    return TRUE;
}

static int daRemoteLink_Draw(daRemoteLink_c* i_this) {
    return i_this->Draw();
}

static void daRemoteLink_matrixInterpCallback(bool i_isSimFrame, void* i_userWork) {
    if (i_isSimFrame || i_userWork == NULL) {
        return;
    }

    static_cast<daRemoteLink_c*>(i_userWork)
        ->applyRemoteBodyMatrixInterpolationForPresentation();
}

static actor_method_class l_daRemoteLink_Method = {
    (process_method_func)daRemoteLink_Create,
    (process_method_func)daRemoteLink_Delete,
    (process_method_func)daRemoteLink_Execute,
    (process_method_func)daRemoteLink_IsDelete,
    (process_method_func)daRemoteLink_Draw,
};

}  // namespace

daRemoteLink_c::daRemoteLink_c()
    : mpArcHeap(NULL),
      mpOwnedArchive(NULL),
      mOwnedResourceCache(),
      mOwnedArchiveEntry(-1),
      mOwnedArchiveMounted(false),
      mEquipmentArchives(),
      mVisualState(),
      mpWarpTexData(NULL),
      mpBodyModel(NULL),
      mpHeadModel(NULL),
      mpHandModel(NULL),
      mpFaceModel(NULL),
      mpSwordModel(NULL),
      mpSheathModel(NULL),
      mpShieldModel(NULL),
      mpHeldItemModel(NULL),
      mpHookTipModel(NULL),
      mpHookSubItemModel(NULL),
      mpHookSubTipModel(NULL),
      mpArrowModel(NULL),
      mpKanteraModel(NULL),
      mpKanteraGlowModel(NULL),
      mpItemActorModel(NULL),
      mpRideActorModel(NULL),
      mpMidnaModel(NULL),
      mpMidnaMaskModel(NULL),
      mpMidnaHandModel(NULL),
      mpMidnaHairModel(NULL),
      mpShadowMidnaModel(NULL),
      mpShadowMidnaMaskModel(NULL),
      mpShadowMidnaHandModel(NULL),
      mpShadowMidnaHairModel(NULL),
      mpMidnaGlowModel(NULL),
      mpHeavyBootModels{NULL, NULL},
      mpHeldItemBrk(NULL),
      mAramResourceCache(),
      mpMagicArmorBodyBrk(NULL),
      mpMagicArmorHeadBrk(NULL),
      mpMotionBck(NULL),
      mBckCache(),
      mMotionBckResId(0),
      mRemoteMoveSpeed(0.0f),
      mLastRemotePos(),
      mRemoteProcId(daAlink_c::PROC_WAIT),
      mRemoteProcVar0(0),
      mRemoteProcVar1(0),
      mRemoteProcVar2(0),
      mRemoteProcVar3(0),
      mRemoteProcVar5(0),
      mRemoteUnderFrame(0.0f),
      mRemoteUnderBck0(0),
      mRemoteUnderFrame0(0.0f),
      mRemoteUnderRate0(1.0f),
      mRemoteUpperBck2(0),
      mRemoteUpperFrame2(0.0f),
      mRemoteUpperRate2(1.0f),
      mRemoteHatRotA(),
      mRemoteHatRotB(),
      mRemoteHatSwing(),
      mRemoteHatShapeY(0),
      mRemoteTransformFrame(0.0f),
      mRemoteTransformFrameValid(false),
      mRemoteEquipItem(0xFFFF),
      mRemoteSwordVariant(0),
      mRemoteShieldVariant(0),
      mLoadedSwordVariant(-1),
      mLoadedShieldVariant(-1),
      mLoadedHeldItem(0xFFFF),
      mRemoteSwordDraw(false),
      mRemoteShieldDraw(false),
      mRemoteShieldGuardActive(false),
      mRemoteSwordOut(false),
      mRemoteHeavyBoots(false),
      mRemoteMidnaDraw(false),
      mRemoteMidnaMaskDraw(false),
      mRemoteMidnaHandDraw(false),
      mRemoteMidnaHairDraw(false),
      mRemoteMidnaShadowForm(false),
      mHeldItemMatrixValid(false),
      mHookTipMatrixValid(false),
      mHookSubItemMatrixValid(false),
      mHookSubTipMatrixValid(false),
      mArrowMatrixValid(false),
      mKanteraMatrixValid(false),
      mKanteraGlowMatrixValid(false),
      mItemActorMatrixValid(false),
      mRideActorMatrixValid(false),
      mMidnaMatrixValid(false),
      mMidnaMaskMatrixValid(false),
      mMidnaHandMatrixValid(false),
      mMidnaHairMatrixValid(false),
      mMidnaGlowMatrixValid(false),
      mRemoteItemDraw(false),
      mRemoteKanteraDraw(false),
      mHasRemotePose(false),
      mHasRemoteMatrices(false),
      mClothesVariant(0),
      mpLeftBodyHandShape(NULL),
      mpRightBodyHandShape(NULL),
      mRemoteItemActorKind(0),
      mRemoteRideActorKind(0),
      mLoadedItemActorKind(-1),
      mLoadedRideActorKind(-1),
      mRemoteBombFlashTicks(0),
      mRemoteBombExTime(-1),
      mRemoteBombFlash(-1),
      mRemoteBombLastPosValid(false),
      mRemoteBombWasWater(false),
      mRemoteBombExplosionSpawned(false),
      mRemoteBombActorId(fpcM_ERROR_PROCESS_ID_e),
      mRemoteBombActorKind(0),
      mRemoteBombAngleY(0),
      mRemoteBombLastPos(cXyz::Zero),
      mPrevBodyMatrixSnapshot(),
      mCurrBodyMatrixSnapshot(),
      mPrevBodyMatrixSnapshotValid(false),
      mCurrBodyMatrixSnapshotValid(false),
      mRemoteMatrixTicksSinceCapture(0),
      mRemoteMatrixBlendDurationTicks(1),
      mFaceMatrixInterp(),
      mHandMatrixInterp(),
      mSwordMatrixInterp(),
      mSheathMatrixInterp(),
      mShieldMatrixInterp(),
      mMidnaMatrixInterp(),
      mMidnaMaskMatrixInterp(),
      mMidnaHandMatrixInterp(),
      mMidnaHairMatrixInterp(),
      mMidnaGlowMatrixInterp(),
      mPvpTargetStts(),
      mPvpTargetCyl(),
      mPvpTargetCollisionInitialized(false),
      mPvpShieldFrontAngle(0),
      mPvpMidnaBindIds(),
      mPvpMidnaBindActive(false),
      mActiveSoundObj(),
      mActiveSounds(),
      mMidnaHairShape(0),
      mSlotReserved(false) {
    mVisualState.form = FORM_HUMAN_KOKIRI;
    for (u32 i = 0; i < ARRAY_SIZE(mOwnedResourceCache); ++i) {
        mOwnedResourceCache[i].index = -1;
        mOwnedResourceCache[i].resource = NULL;
    }
    for (u32 i = 0; i < ARRAY_SIZE(mAramResourceCache); ++i) {
        mAramResourceCache[i].resId = 0;
        mAramResourceCache[i].resource = NULL;
    }
    initArchiveSlot(mEquipmentArchives[l_eqArcAlink], "Alink");
    initArchiveSlot(mEquipmentArchives[l_eqArcKmdl], "Kmdl");
    initArchiveSlot(mEquipmentArchives[l_eqArcCarvingShield], "CWShd");
    initArchiveSlot(mEquipmentArchives[l_eqArcOrdonShield], "SWShd");
    initArchiveSlot(mEquipmentArchives[l_eqArcHylianShield], "HyShd");
    mShadowMidnaInvModel.mModel = NULL;
    mShadowMidnaInvModel.mpPackets = NULL;
    mShadowMidnaMaskInvModel.mModel = NULL;
    mShadowMidnaMaskInvModel.mpPackets = NULL;
    mShadowMidnaHandInvModel.mModel = NULL;
    mShadowMidnaHandInvModel.mpPackets = NULL;
    mShadowMidnaHairInvModel.mModel = NULL;
    mShadowMidnaHairInvModel.mpPackets = NULL;
    mActiveSoundObj.init(&current.pos, 8);
    for (ActiveRemoteSound& sound : mActiveSounds) {
        sound.active = false;
        sound.ageTicks = 0;
    }
}

int daRemoteLink_c::createHeapCallBack(fopAc_ac_c* i_this) {
    daRemoteLink_c* actor = static_cast<daRemoteLink_c*>(i_this);
    return actor->CreateHeap();
}

void daRemoteLink_c::setOriginalHeap(JKRExpHeap** i_ppheap, u32 i_size) {
    if (*i_ppheap == NULL) {
        u32 size = ROUND(i_size, 16);
        *i_ppheap = JKRExpHeap::create(size + 0xA0, mDoExt_getGameHeap(), true);
    }
}

bool daRemoteLink_c::mountOwnedArchive() {
    if (mOwnedArchiveMounted) {
        return true;
    }

    char archivePath[64];
    const char* arcName = getCurrentArcName();
    std::snprintf(archivePath, sizeof(archivePath), "/res/Object/%s.arc", arcName);
    mOwnedArchiveEntry = DVDConvertPathToEntrynum(archivePath);
    if (mOwnedArchiveEntry < 0) {
        DuskLog.warn("RemoteLink: owned archive entry missing path={}", archivePath);
        return false;
    }

    mpOwnedArchive =
        JKR_NEW_ARGS(mpArcHeap, -4) JKRMemArchive(mOwnedArchiveEntry,
                                                  JKRArchive::MOUNT_DIRECTION_TAIL);
    mOwnedArchiveMounted = mpOwnedArchive != NULL && mpOwnedArchive->isMounted();
    if (!mOwnedArchiveMounted) {
        DuskLog.warn("RemoteLink: owned archive mount failed entry={} archive={}",
                     mOwnedArchiveEntry, (void*)mpOwnedArchive);
        mpOwnedArchive = NULL;
        return false;
    }

    DuskLog.info("RemoteLink: owned archive mounted arc={} entry={} archive={} files={}",
                 arcName, mOwnedArchiveEntry, (void*)mpOwnedArchive,
                 mpOwnedArchive->countFile());
    return true;
}

void daRemoteLink_c::initArchiveSlot(OwnedArchiveSlot& i_slot, const char* i_arcName) {
    i_slot.arcName = i_arcName;
    i_slot.archive = NULL;
    i_slot.entry = -1;
    i_slot.mounted = false;
    for (u32 i = 0; i < ARRAY_SIZE(i_slot.cache); ++i) {
        i_slot.cache[i].index = -1;
        i_slot.cache[i].resource = NULL;
    }
}

bool daRemoteLink_c::mountArchiveSlot(OwnedArchiveSlot& i_slot) {
    if (i_slot.mounted) {
        return true;
    }

    char archivePath[64];
    std::snprintf(archivePath, sizeof(archivePath), "/res/Object/%s.arc", i_slot.arcName);
    i_slot.entry = DVDConvertPathToEntrynum(archivePath);
    if (i_slot.entry < 0) {
        DuskLog.warn("RemoteLink: equipment archive entry missing arc={} path={}",
                     i_slot.arcName, archivePath);
        return false;
    }

    i_slot.archive =
        JKR_NEW_ARGS(mpArcHeap, -4) JKRMemArchive(i_slot.entry,
                                                  JKRArchive::MOUNT_DIRECTION_TAIL);
    i_slot.mounted = i_slot.archive != NULL && i_slot.archive->isMounted();
    if (!i_slot.mounted) {
        DuskLog.warn("RemoteLink: equipment archive mount failed arc={} entry={} archive={}",
                     i_slot.arcName, i_slot.entry, (void*)i_slot.archive);
        i_slot.archive = NULL;
        return false;
    }

    DuskLog.info("RemoteLink: equipment archive mounted arc={} entry={} archive={} files={}",
                 i_slot.arcName, i_slot.entry, (void*)i_slot.archive,
                 i_slot.archive->countFile());
    return true;
}

void daRemoteLink_c::deleteArchiveSlot(OwnedArchiveSlot& i_slot) {
    if (i_slot.archive != NULL) {
        JKR_DELETE(i_slot.archive);
        i_slot.archive = NULL;
    }
    i_slot.entry = -1;
    i_slot.mounted = false;
    for (u32 i = 0; i < ARRAY_SIZE(i_slot.cache); ++i) {
        i_slot.cache[i].index = -1;
        i_slot.cache[i].resource = NULL;
    }
}

const char* daRemoteLink_c::getCurrentArcName() const {
    if (mVisualState.form == FORM_WOLF) {
        return "Wmdl";
    }

    switch (mClothesVariant) {
    case 1:
        return "Bmdl";
    case 2:
        return "Zmdl";
    case 3:
        return "Mmdl";
    default:
        return "Kmdl";
    }
}

const char* daRemoteLink_c::getBodyResName() const {
    switch (mClothesVariant) {
    case 1:
        return "bl.bmd";
    case 2:
        return "zl.bmd";
    case 3:
        return "ml.bmd";
    default:
        return "al.bmd";
    }
}

const char* daRemoteLink_c::getHeadResName() const {
    switch (mClothesVariant) {
    case 1:
        return "bl_head.bmd";
    case 2:
        return "zl_head.bmd";
    case 3:
        return "ml_head.bmd";
    default:
        return "al_head.bmd";
    }
}

const char* daRemoteLink_c::getFaceResName() const {
    switch (mClothesVariant) {
    case 2:
        return "zl_face.bmd";
    default:
        return "al_face.bmd";
    }
}

const char* daRemoteLink_c::getHandResName() const {
    switch (mClothesVariant) {
    case 1:
        return "bl_hands.bmd";
    default:
        return "al_hands.bmd";
    }
}

u32 daRemoteLink_c::getOwnedArchiveNodeType(u32 i_fileIndex) const {
    return getArchiveNodeType(mpOwnedArchive, i_fileIndex);
}

u32 daRemoteLink_c::getArchiveNodeType(JKRMemArchive* i_archive, u32 i_fileIndex) const {
    if (i_archive == NULL) {
        return 0;
    }

    JKRArchive::SDIDirEntry* node = i_archive->mNodes;
    for (int i = 0; i < i_archive->countDirectory(); ++i) {
        const u32 firstFileIndex = node->first_file_index;
        const u32 endFileIndex = firstFileIndex + node->num_entries;
        if (i_fileIndex >= firstFileIndex && i_fileIndex < endFileIndex) {
            return node->type;
        }
        ++node;
    }

    return 0;
}

void* daRemoteLink_c::convertOwnedObjectRes(s32 i_index) {
    for (u32 i = 0; i < ARRAY_SIZE(mOwnedResourceCache); ++i) {
        if (mOwnedResourceCache[i].index == i_index) {
            return mOwnedResourceCache[i].resource;
        }
    }

    if (mpOwnedArchive == NULL || i_index < 0 || i_index >= mpOwnedArchive->countFile()) {
        return NULL;
    }

    void* raw = mpOwnedArchive->getIdxResource(i_index);
    const u32 nodeType = getOwnedArchiveNodeType(static_cast<u32>(i_index));
    if (raw == NULL || nodeType == 0) {
        DuskLog.warn("RemoteLink: owned resource unavailable index={} raw={} nodeType={}",
                     i_index, raw, nodeType);
        return NULL;
    }

    JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
    void* converted = NULL;
    if (nodeType == 'BMDR' || nodeType == 'BMDV' || nodeType == 'BMDE' ||
        nodeType == 'BMWR' || nodeType == 'BMWE')
    {
        converted = dRes_info_c::loaderBasicBmd(nodeType, raw);
    } else if (nodeType == 'BMDP') {
        converted = J3DModelLoaderDataBase::load(raw, 0x59020030);
    } else if (nodeType == 'BMDG' || nodeType == 'BMDA') {
        converted = J3DModelLoaderDataBase::load(raw, 0x59020010);
        J3DModelData* modelData = static_cast<J3DModelData*>(converted);
        if (modelData != NULL &&
            modelData->newSharedDisplayList(J3DMdlFlag_UseSingleDL) == kJ3DError_Success)
        {
            modelData->simpleCalcMaterial(const_cast<MtxP>(j3dDefaultMtx));
            modelData->makeSharedDL();
        }
    } else if (nodeType == 'BTP ' || nodeType == 'BTK ' || nodeType == 'BPK ' ||
               nodeType == 'BRK ' || nodeType == 'BLK ' || nodeType == 'BVA ' ||
               nodeType == 'BXA ')
    {
        converted = J3DAnmLoaderDataBase::load(raw);
    }
    mDoExt_setCurrentHeap(previousHeap);

    if (converted == NULL) {
        DuskLog.warn("RemoteLink: owned resource conversion failed index={} nodeType={}", i_index,
                     nodeType);
        return NULL;
    }

    for (u32 i = 0; i < ARRAY_SIZE(mOwnedResourceCache); ++i) {
        if (mOwnedResourceCache[i].index < 0) {
            mOwnedResourceCache[i].index = i_index;
            mOwnedResourceCache[i].resource = converted;
            break;
        }
    }

    DuskLog.info("RemoteLink: owned resource converted index={} nodeType={} resource={}", i_index,
                 nodeType, converted);
    return converted;
}

void* daRemoteLink_c::convertArchiveObjectRes(OwnedArchiveSlot& i_slot, s32 i_index) {
    for (u32 i = 0; i < ARRAY_SIZE(i_slot.cache); ++i) {
        if (i_slot.cache[i].index == i_index) {
            return i_slot.cache[i].resource;
        }
    }

    if (i_slot.archive == NULL || i_index < 0 || i_index >= i_slot.archive->countFile()) {
        return NULL;
    }

    void* raw = i_slot.archive->getIdxResource(i_index);
    const u32 nodeType = getArchiveNodeType(i_slot.archive, static_cast<u32>(i_index));
    if (raw == NULL || nodeType == 0) {
        DuskLog.warn("RemoteLink: equipment resource unavailable arc={} index={} raw={} nodeType={}",
                     i_slot.arcName, i_index, raw, nodeType);
        return NULL;
    }

    JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
    void* converted = NULL;
    if (nodeType == 'BMDR' || nodeType == 'BMDV' || nodeType == 'BMDE' ||
        nodeType == 'BMWR' || nodeType == 'BMWE')
    {
        converted = dRes_info_c::loaderBasicBmd(nodeType, raw);
    } else if (nodeType == 'BMDP') {
        converted = J3DModelLoaderDataBase::load(raw, 0x59020030);
    } else if (nodeType == 'BMDG' || nodeType == 'BMDA') {
        converted = J3DModelLoaderDataBase::load(raw, 0x59020010);
        J3DModelData* modelData = static_cast<J3DModelData*>(converted);
        if (modelData != NULL &&
            modelData->newSharedDisplayList(J3DMdlFlag_UseSingleDL) == kJ3DError_Success)
        {
            modelData->simpleCalcMaterial(const_cast<MtxP>(j3dDefaultMtx));
            modelData->makeSharedDL();
        }
    } else if (nodeType == 'BTP ' || nodeType == 'BTK ' || nodeType == 'BPK ' ||
               nodeType == 'BRK ' || nodeType == 'BLK ' || nodeType == 'BVA ' ||
               nodeType == 'BXA ')
    {
        converted = J3DAnmLoaderDataBase::load(raw);
    }
    mDoExt_setCurrentHeap(previousHeap);

    if (converted == NULL) {
        DuskLog.warn("RemoteLink: equipment conversion failed arc={} index={} nodeType={}",
                     i_slot.arcName, i_index, nodeType);
        return NULL;
    }

    for (u32 i = 0; i < ARRAY_SIZE(i_slot.cache); ++i) {
        if (i_slot.cache[i].index < 0) {
            i_slot.cache[i].index = i_index;
            i_slot.cache[i].resource = converted;
            break;
        }
    }

    DuskLog.info("RemoteLink: equipment resource converted arc={} index={} nodeType={} resource={}",
                 i_slot.arcName, i_index, nodeType, converted);
    return converted;
}

void* daRemoteLink_c::getOwnedObjectRes(const char* i_resName) {
    if (!mountOwnedArchive() || mpOwnedArchive == NULL) {
        return NULL;
    }

    JKRArchive::SDIFileEntry* entry = mpOwnedArchive->findNameResource(i_resName);
    if (entry == NULL) {
        DuskLog.warn("RemoteLink: owned resource missing arc={} name={}", getCurrentArcName(),
                     i_resName);
        return NULL;
    }

    return convertOwnedObjectRes(static_cast<s32>(entry - mpOwnedArchive->mFiles));
}

void* daRemoteLink_c::getArchiveObjectRes(OwnedArchiveSlot& i_slot, const char* i_resName) {
    if (!mountArchiveSlot(i_slot) || i_slot.archive == NULL) {
        return NULL;
    }

    JKRArchive::SDIFileEntry* entry = i_slot.archive->findNameResource(i_resName);
    if (entry == NULL) {
        DuskLog.warn("RemoteLink: equipment resource missing arc={} name={}", i_slot.arcName,
                     i_resName);
        return NULL;
    }

    return convertArchiveObjectRes(i_slot, static_cast<s32>(entry - i_slot.archive->mFiles));
}

void* daRemoteLink_c::getArchiveObjectRes(OwnedArchiveSlot& i_slot, s32 i_index) {
    if (!mountArchiveSlot(i_slot) || i_slot.archive == NULL) {
        return NULL;
    }

    return convertArchiveObjectRes(i_slot, i_index);
}

J3DModel* daRemoteLink_c::initModel(J3DModelData* i_modelData, u32 i_mdlFlags,
                                    u32 i_diffFlags) {
    if (i_modelData == NULL) {
        return NULL;
    }

    bool warpMaterial = false;
    J3DTexture* tex = i_modelData->getTexture();
    int texNo = tex != NULL ? tex->getNum() - 1 : -1;
    if (texNo >= 0) {
#if TARGET_PC
        warpMaterial = tex->getImgDataPtr(texNo) == mpWarpTexData;
#else
        ResTIMG* timg = tex->getResTIMG(texNo);
        warpMaterial = mpWarpTexData == (void*)((uintptr_t)timg + timg->imageOffset);
#endif
    }

    if (warpMaterial) {
        dRes_info_c::onWarpMaterial(i_modelData);
        i_diffFlags |= 0x2000400;
    }

    J3DModel* model = mDoExt_J3DModel__create(i_modelData, i_mdlFlags,
                                              i_diffFlags | 0x11000084);

    if (warpMaterial) {
        dRes_info_c::offWarpMaterial(i_modelData);
    }

    return model;
}

J3DModel* daRemoteLink_c::initModel(J3DModelData* i_modelData, u32 i_diffFlags) {
    return initModel(i_modelData, 0x80000, i_diffFlags);
}

void daRemoteLink_c::hideAllHandShapes() {
    J3DModelData* modelData = mpHandModel->getModelData();
    for (u16 i = 0; i < modelData->getMaterialNum(); i++) {
        hideMaterialShape(modelData, i);
    }
}

void daRemoteLink_c::setupHumanKokiriModel() {
    const u32 armorDiffFlags = mClothesVariant == 3 ? 0x1000000 : 0;
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic setup begin");
    }
    mpBodyModel =
        initModel(static_cast<J3DModelData*>(getOwnedObjectRes(getBodyResName())),
                  armorDiffFlags);
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic body model={}", (void*)mpBodyModel);
    }
    mpHeadModel =
        initModel(static_cast<J3DModelData*>(getOwnedObjectRes(getHeadResName())),
                  armorDiffFlags);
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic head model={}", (void*)mpHeadModel);
    }
    mpHandModel =
        initModel(static_cast<J3DModelData*>(getOwnedObjectRes(getHandResName())), 0);
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic hand model={}", (void*)mpHandModel);
    }

    if (mpBodyModel == NULL || mpHeadModel == NULL || mpHandModel == NULL) {
        return;
    }

    J3DModelData* bodyData = mpBodyModel->getModelData();
    hideMaterialShape(bodyData, 16);
    mpLeftBodyHandShape = getMaterialShape(bodyData, 11);
    mpRightBodyHandShape = getMaterialShape(bodyData, 12);
    hideAllHandShapes();

    mpBodyModel->setUserArea(0);
    mpHeadModel->setUserArea((uintptr_t)this);
    J3DModelData* headData = mpHeadModel->getModelData();
    for (u16 i = 1; headData != NULL && i < headData->getJointNum(); ++i) {
        headData->getJointNodePointer(i)->setCallBack(daRemoteLink_headModelCallBack);
    }

    if (mClothesVariant == 3 && !setupMagicArmorBrk()) {
        DuskLog.warn("RemoteLink: Magic Armor BRK setup failed");
        return;
    }

    mpFaceModel =
        initModel(static_cast<J3DModelData*>(getOwnedObjectRes(getFaceResName())), 0x20200);
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic face model={}", (void*)mpFaceModel);
    }
    if (mpFaceModel == NULL) {
        return;
    }

    setupHeavyBootModels();
    setupShadowMidnaModels();

    if (sCreateLogCount < 2 || mClothesVariant == 3) {
        daAlink_c* localLink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
        J3DModelData* localBodyData =
            localLink != NULL && localLink->mpLinkModel != NULL
                ? localLink->mpLinkModel->getModelData()
                : NULL;
        DuskLog.info(
            "RemoteLink: models clothes={} arc={} body={} head={} hands={} face={} bodyData={} "
            "localBodyData={} sharesLocalBodyData={} bodyJoints={} bodyMaterials={}",
            mClothesVariant, getCurrentArcName(),
            (void*)mpBodyModel, (void*)mpHeadModel, (void*)mpHandModel, (void*)mpFaceModel,
            (void*)bodyData, (void*)localBodyData, bodyData == localBodyData,
            bodyData->getJointNum(), bodyData->getMaterialNum());
        if (sCreateLogCount < 2) {
            sCreateLogCount++;
        }
    }
}

void daRemoteLink_c::setupHeavyBootModels() {
    if (mVisualState.form == FORM_WOLF) {
        return;
    }

    J3DModelData* bootData =
        static_cast<J3DModelData*>(getOwnedObjectRes("al_bootsh.bmd"));
    if (bootData == NULL) {
        DuskLog.warn("RemoteLink: heavy boots model missing arc={}", getCurrentArcName());
        return;
    }

    for (int i = 0; i < 2; ++i) {
        mpHeavyBootModels[i] = initModel(bootData, 0);
    }

    if (mpHeavyBootModels[0] == NULL || mpHeavyBootModels[1] == NULL) {
        mpHeavyBootModels[0] = NULL;
        mpHeavyBootModels[1] = NULL;
        DuskLog.warn("RemoteLink: heavy boots model create failed arc={}", getCurrentArcName());
        return;
    }

    DuskLog.info("RemoteLink: heavy boots models loaded left={} right={} arc={}",
                 (void*)mpHeavyBootModels[0], (void*)mpHeavyBootModels[1],
                 getCurrentArcName());
}

void daRemoteLink_c::setupWolfModel() {
    mpBodyModel = initModel(static_cast<J3DModelData*>(
                                mountOwnedArchive() ? convertOwnedObjectRes(14) : NULL),
                            0x20200);
    if (mpBodyModel == NULL) {
        return;
    }

    mpBodyModel->setUserArea(0);
    mpMidnaModel = initModel(static_cast<J3DModelData*>(
                                 mountOwnedArchive()
                                     ? convertOwnedObjectRes(dRes_ID_WMDL_BMD_MD_e)
                                     : NULL),
                             0x1020200);
    mpMidnaMaskModel = initModel(static_cast<J3DModelData*>(
                                     mountOwnedArchive()
                                         ? convertOwnedObjectRes(dRes_ID_WMDL_BMD_MD_MASK_e)
                                         : NULL),
                                 0x1000000);
    mpMidnaHandModel = initModel(static_cast<J3DModelData*>(
                                     mountOwnedArchive()
                                         ? convertOwnedObjectRes(dRes_ID_WMDL_BMD_MD_HANDS_e)
                                         : NULL),
                                 0x1000000);
    mpMidnaHairModel = initModel(static_cast<J3DModelData*>(
                                     mountOwnedArchive()
                                         ? convertOwnedObjectRes(dRes_ID_WMDL_BMD_MD_HAIR_HAND_e)
                                         : NULL),
                                 0x1000000);
    applyRideMidnaShapeVisibility(mpMidnaHandModel, mpMidnaHairModel, mMidnaHairShape);
    setupShadowMidnaModels();

    if (sCreateLogCount < 2 || mVisualState.form == FORM_WOLF) {
        daAlink_c* localLink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
        J3DModelData* localBodyData =
            localLink != NULL && localLink->mpLinkModel != NULL
                ? localLink->mpLinkModel->getModelData()
                : NULL;
        J3DModelData* bodyData = mpBodyModel->getModelData();
        DuskLog.info(
            "RemoteLink: wolf model arc={} body={} bodyData={} localBodyData={} "
            "sharesLocalBodyData={} bodyJoints={} bodyMaterials={} midna={} mask={} hand={} hair={}",
            getCurrentArcName(), (void*)mpBodyModel, (void*)bodyData, (void*)localBodyData,
            bodyData == localBodyData, bodyData->getJointNum(), bodyData->getMaterialNum(),
            (void*)mpMidnaModel, (void*)mpMidnaMaskModel, (void*)mpMidnaHandModel,
            (void*)mpMidnaHairModel);
        if (sCreateLogCount < 2) {
            sCreateLogCount++;
        }
    }
}

void daRemoteLink_c::setupShadowMidnaModels() {
    J3DModelData* shadowData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 14));
    J3DModelData* maskData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 8));
    J3DModelData* handData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 7));
    J3DModelData* hairData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 15));
    J3DModelData* glowData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 11));

    mpShadowMidnaModel = initModel(shadowData, 0, 0x200);
    mpShadowMidnaMaskModel = initModel(maskData, 0, 0x200);
    mpShadowMidnaHandModel = initModel(handData, 0, 0x200);
    mpShadowMidnaHairModel = initModel(hairData, 0, 0x200);
    mpMidnaGlowModel = initModel(glowData, 0);

    if (mpShadowMidnaModel != NULL) {
        mShadowMidnaInvModel.create(mpShadowMidnaModel, 1);
    }
    if (mpShadowMidnaMaskModel != NULL) {
        mShadowMidnaMaskInvModel.create(mpShadowMidnaMaskModel, 1);
    }
    if (mpShadowMidnaHandModel != NULL) {
        mShadowMidnaHandInvModel.create(mpShadowMidnaHandModel, 1);
    }
    if (mpShadowMidnaHairModel != NULL) {
        mShadowMidnaHairInvModel.create(mpShadowMidnaHairModel, 1);
    }

    DuskLog.info("RemoteLink: shadow Midna models body={} mask={} hand={} hair={} glow={}",
                 (void*)mpShadowMidnaModel, (void*)mpShadowMidnaMaskModel,
                 (void*)mpShadowMidnaHandModel, (void*)mpShadowMidnaHairModel,
                 (void*)mpMidnaGlowModel);
}

bool daRemoteLink_c::setupMagicArmorBrk() {
    mpMagicArmorBodyBrk =
        static_cast<J3DAnmTevRegKey*>(getOwnedObjectRes("ml_body_power_up_a.brk"));
    mpMagicArmorHeadBrk =
        static_cast<J3DAnmTevRegKey*>(getOwnedObjectRes("ml_head_power_up_a.brk"));
    if (mpMagicArmorBodyBrk == NULL || mpMagicArmorHeadBrk == NULL ||
        mpBodyModel == NULL || mpHeadModel == NULL)
    {
        return false;
    }

    J3DModelData* bodyData = mpBodyModel->getModelData();
    mpMagicArmorBodyBrk->searchUpdateMaterialID(bodyData);
    bodyData->entryTevRegAnimator(mpMagicArmorBodyBrk);
    mpMagicArmorBodyBrk->setFrame(mpMagicArmorBodyBrk->getFrameMax());

    J3DModelData* headData = mpHeadModel->getModelData();
    mpMagicArmorHeadBrk->searchUpdateMaterialID(headData);
    headData->entryTevRegAnimator(mpMagicArmorHeadBrk);
    mpMagicArmorHeadBrk->setFrame(mpMagicArmorHeadBrk->getFrameMax());

    DuskLog.info("RemoteLink: Magic Armor BRK initialized body={} head={}",
                 (void*)mpMagicArmorBodyBrk, (void*)mpMagicArmorHeadBrk);
    return true;
}

void daRemoteLink_c::setupSwordMaterialAnm(J3DModel* i_model, int i_swordVariant) {
    if (i_model == NULL || i_model->getModelData() == NULL) {
        return;
    }

    J3DModelData* modelData = i_model->getModelData();
    if (i_swordVariant == 1) {
        J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
            getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink], 0x44));
        if (btk != NULL) {
            btk->searchUpdateMaterialID(modelData);
            modelData->entryTexMtxAnimator(btk);
        }
    } else if (i_swordVariant == 3) {
        J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
            getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink], 0x45));
        if (btk != NULL) {
            btk->searchUpdateMaterialID(modelData);
            modelData->entryTexMtxAnimator(btk);
        }

        J3DAnmTevRegKey* brk = static_cast<J3DAnmTevRegKey*>(
            getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink], 0x3F));
        if (brk != NULL) {
            brk->searchUpdateMaterialID(modelData);
            modelData->entryTevRegAnimator(brk);
        }
    }
}

void daRemoteLink_c::applySwordShapeVisibility() {
    if (mpSwordModel == NULL || mpSwordModel->getModelData() == NULL) {
        return;
    }

    const bool woodSword = mRemoteSwordVariant == 2;
    const u16 materialNo = woodSword ? 1 : 0;
    J3DModelData* data = mpSwordModel->getModelData();
    if (materialNo >= data->getMaterialNum()) {
        return;
    }

    J3DMaterial* material = data->getMaterialNodePointer(materialNo);
    if (material == NULL || material->getShape() == NULL) {
        return;
    }

    const bool shouldHide = woodSword ? mRemoteSwordOut : !mRemoteSwordOut;
    if (shouldHide) {
        material->getShape()->hide();
    } else {
        material->getShape()->show();
    }
}

void daRemoteLink_c::destroyEquipmentModels() {
    mpSwordModel = NULL;
    mpSheathModel = NULL;
    mpShieldModel = NULL;
    mLoadedSwordVariant = -1;
    mLoadedShieldVariant = -1;
}

void daRemoteLink_c::setupEquipmentModels() {
    if (!mRemoteSwordDraw && mLoadedSwordVariant != -1) {
        mpSwordModel = NULL;
        mpSheathModel = NULL;
        mLoadedSwordVariant = -1;
    }

    if (!mRemoteShieldDraw && mLoadedShieldVariant != -1) {
        mpShieldModel = NULL;
        mLoadedShieldVariant = -1;
    }

    if (mRemoteSwordDraw && mLoadedSwordVariant != mRemoteSwordVariant) {
        mpSwordModel = NULL;
        mpSheathModel = NULL;
        mLoadedSwordVariant = -1;

        switch (mRemoteSwordVariant) {
        case 1:
            mpSwordModel = initModel(static_cast<J3DModelData*>(
                                         getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                             0x3C)),
                                     0x80000, 0x200);
            mpSheathModel = initModel(static_cast<J3DModelData*>(
                                          getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                              0x3B)),
                                      0);
            break;
        case 2:
            mpSwordModel = initModel(static_cast<J3DModelData*>(
                                         getArchiveObjectRes(mEquipmentArchives[l_eqArcKmdl],
                                                             "al_SWB.bmd")),
                                     0);
            mpSheathModel = initModel(static_cast<J3DModelData*>(
                                          getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                              0x37)),
                                      0, 0);
            break;
        case 3:
            mpSwordModel = initModel(static_cast<J3DModelData*>(
                                         getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                             0x38)),
                                     0, 0x1000200);
            mpSheathModel = initModel(static_cast<J3DModelData*>(
                                          getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                              0x37)),
                                      0, 0);
            break;
        default:
            break;
        }

        if (mpSwordModel != NULL && mpSheathModel != NULL) {
            setupSwordMaterialAnm(mpSwordModel, mRemoteSwordVariant);
            applySwordShapeVisibility();
            mLoadedSwordVariant = mRemoteSwordVariant;
            DuskLog.info("RemoteLink: equipment sword loaded variant={} sword={} sheath={}",
                         mLoadedSwordVariant, (void*)mpSwordModel, (void*)mpSheathModel);
        } else {
            mpSwordModel = NULL;
            mpSheathModel = NULL;
        }
    } else if (mRemoteSwordDraw) {
        applySwordShapeVisibility();
    }

    if (mRemoteShieldDraw && mLoadedShieldVariant != mRemoteShieldVariant) {
        mpShieldModel = NULL;
        mLoadedShieldVariant = -1;

        OwnedArchiveSlot* shieldArc = NULL;
        switch (mRemoteShieldVariant) {
        case 1:
            shieldArc = &mEquipmentArchives[l_eqArcCarvingShield];
            break;
        case 2:
            shieldArc = &mEquipmentArchives[l_eqArcOrdonShield];
            break;
        case 3:
            shieldArc = &mEquipmentArchives[l_eqArcHylianShield];
            break;
        default:
            break;
        }

        if (shieldArc != NULL) {
            mpShieldModel = initModel(static_cast<J3DModelData*>(
                                          getArchiveObjectRes(*shieldArc, 3)),
                                      0x80000, 0x100000);
        }

        if (mpShieldModel != NULL) {
            mLoadedShieldVariant = mRemoteShieldVariant;
            DuskLog.info("RemoteLink: equipment shield loaded variant={} shield={}",
                         mLoadedShieldVariant, (void*)mpShieldModel);
        }
    }
}

const daRemoteLink_c::HeldItemVisualDesc* daRemoteLink_c::getHeldItemVisualDesc(
    u16 i_itemNo) const {
    static const HeldItemVisualDesc l_descs[] = {
        {dItemNo_IRONBALL_e, dRes_ID_ALANM_BMD_AL_IB_e, 0x2800, 0x80000, 0,
         0},
        {dItemNo_BOW_e, dRes_ID_ALANM_BMD_AL_BOW_e, 0x4C00, 0x80000, 0,
         0},
        {dItemNo_HOOKSHOT_e, dRes_ID_ALANM_BMD_AL_HS_e, 0x5C00, 0x80000, 0,
         0},
        {dItemNo_W_HOOKSHOT_e, dRes_ID_ALANM_BMD_AL_HS_e, 0x5C00, 0x80000, 0,
         0},
        {dItemNo_COPY_ROD_e, dRes_ID_ALANM_BMD_AL_CROD_e, 0x5400, 0x80000, 0x1000000,
         dRes_ID_ALANM_BRK_AL_CROD_CHANGE_COLOR_e},
        {dItemNo_COPY_ROD_2_e, dRes_ID_ALANM_BMD_AL_CROD_e, 0x5400, 0x80000, 0x1000000,
         dRes_ID_ALANM_BRK_AL_CROD_CHANGE_COLOR_e},
        {dItemNo_PACHINKO_e, dRes_ID_ALANM_BMD_AL_PACHI_e, 0x2C00, 0x80000, 0,
         0},
    };

    if (isBottleItem(i_itemNo)) {
        static const HeldItemVisualDesc l_bottleDesc = {
            dItemNo_EMPTY_BOTTLE_e, dRes_ID_ALANM_BMD_AL_BOTTLE_e, 0x5C00, 0,
            0x1020200, dRes_ID_ALANM_BRK_AL_BOTTLE_e};
        return &l_bottleDesc;
    }

    for (u32 i = 0; i < ARRAY_SIZE(l_descs); ++i) {
        if (l_descs[i].itemNo == i_itemNo) {
            return &l_descs[i];
        }
    }

    return NULL;
}

void* daRemoteLink_c::loadAramResource(u16 i_resId, u32 i_bufSize, bool i_isModel) {
    for (u32 i = 0; i < ARRAY_SIZE(mAramResourceCache); ++i) {
        if (mAramResourceCache[i].resId == i_resId) {
            return mAramResourceCache[i].resource;
        }
    }

    JKRArchive* anmArchive = dComIfGp_getAnmArchive();
    if (anmArchive == NULL || i_resId == 0 || i_bufSize == 0) {
        return NULL;
    }

    JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
    u8* tmpBuffer = JKR_NEW_ARRAY_ARGS(u8, i_bufSize, 0x20);
    void* converted = NULL;
    if (tmpBuffer != NULL) {
        const u32 readSize = JKRReadIdxResource(tmpBuffer, i_bufSize, i_resId, anmArchive);
        if (readSize == 0) {
            DuskLog.warn("RemoteLink: ARAM resource read failed resId={} isModel={}",
                         i_resId, i_isModel);
        } else if (i_isModel) {
            u32 type = 'BMWR';
            JKRArchive::SDIDirEntry* dir = anmArchive->mNodes;
            for (int i = 0; i < anmArchive->countDirectory(); i++, dir++) {
                if (i_resId >= dir->first_file_index &&
                    i_resId < dir->first_file_index + dir->num_entries)
                {
                    type = dir->type;
                    break;
                }
            }
            converted = dRes_info_c::loaderBasicBmd(type, tmpBuffer);
        } else {
            converted = J3DAnmLoaderDataBase::load(tmpBuffer);
        }
    }
    mDoExt_setCurrentHeap(previousHeap);

    if (converted == NULL) {
        DuskLog.warn("RemoteLink: ARAM resource conversion failed resId={} isModel={}",
                     i_resId, i_isModel);
        return NULL;
    }

    for (u32 i = 0; i < ARRAY_SIZE(mAramResourceCache); ++i) {
        if (mAramResourceCache[i].resId == 0) {
            mAramResourceCache[i].resId = i_resId;
            mAramResourceCache[i].resource = converted;
            break;
        }
    }

    DuskLog.info("RemoteLink: ARAM resource converted resId={} isModel={} resource={}",
                 i_resId, i_isModel, converted);
    return converted;
}

J3DModelData* daRemoteLink_c::loadAramBmd(u16 i_resId, u32 i_bufSize) {
    return static_cast<J3DModelData*>(loadAramResource(i_resId, i_bufSize, true));
}

J3DAnmTevRegKey* daRemoteLink_c::loadAramItemBrk(u16 i_resId, J3DModel* i_model) {
    if (i_resId == 0 || i_model == NULL || i_model->getModelData() == NULL) {
        return NULL;
    }

    J3DAnmTevRegKey* brk =
        static_cast<J3DAnmTevRegKey*>(loadAramResource(i_resId, 0x400, false));
    if (brk == NULL) {
        return NULL;
    }

    J3DModelData* modelData = i_model->getModelData();
    brk->setFrame(0.0f);
    brk->searchUpdateMaterialID(modelData);
    modelData->entryTevRegAnimator(brk);
    return brk;
}

void daRemoteLink_c::clearHeldItemExtras() {
    mpHookTipModel = NULL;
    mpHookSubItemModel = NULL;
    mpHookSubTipModel = NULL;
    mpArrowModel = NULL;
    mHookTipMatrixValid = false;
    mHookSubItemMatrixValid = false;
    mHookSubTipMatrixValid = false;
    mArrowMatrixValid = false;
}

void daRemoteLink_c::setupHeldItemModel() {
    const HeldItemVisualDesc* desc = getHeldItemVisualDesc(mRemoteEquipItem);
    if (desc == NULL || mVisualState.form == FORM_WOLF) {
        mpHeldItemModel = NULL;
        mpHeldItemBrk = NULL;
        clearHeldItemExtras();
        mLoadedHeldItem = 0xFFFF;
        mHeldItemMatrixValid = false;
        return;
    }

    if (mLoadedHeldItem == desc->itemNo && mpHeldItemModel != NULL) {
        return;
    }

    mpHeldItemModel = NULL;
    mpHeldItemBrk = NULL;
    clearHeldItemExtras();
    mLoadedHeldItem = 0xFFFF;
    mHeldItemMatrixValid = false;

    mpHeldItemModel = initModel(loadAramBmd(desc->bmdResId, desc->bmdBufferSize),
                                desc->modelFlags, desc->diffFlags);
    if (mpHeldItemModel == NULL) {
        DuskLog.warn("RemoteLink: held item model load failed item={} resId={}",
                     mRemoteEquipItem, desc->bmdResId);
        return;
    }

    if (desc->brkResId != 0) {
        mpHeldItemBrk = loadAramItemBrk(desc->brkResId, mpHeldItemModel);
    }

    if (mRemoteEquipItem == dItemNo_HOOKSHOT_e || mRemoteEquipItem == dItemNo_W_HOOKSHOT_e) {
        mpHookTipModel = initModel(loadAramBmd(dRes_ID_ALANM_BMD_AL_HS_TIP_e, 0x3800), 0);
        if (mRemoteEquipItem == dItemNo_W_HOOKSHOT_e) {
            if (mpHeldItemModel != NULL) {
                mpHookSubItemModel = initModel(mpHeldItemModel->getModelData(), 0);
            }
            if (mpHookTipModel != NULL) {
                mpHookSubTipModel = initModel(mpHookTipModel->getModelData(), 0);
            }
        }
    } else if (mRemoteEquipItem == dItemNo_BOW_e) {
        mpArrowModel = initModel(static_cast<J3DModelData*>(
                                     getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                         dRes_ID_ALINK_BMD_AL_ARROW_e)),
                                 0x80000, 0);
    }

    mLoadedHeldItem = desc->itemNo;
    DuskLog.info("RemoteLink: held item loaded item={} canonicalItem={} model={} resId={} "
                 "brk={} hookTip={} hookSubItem={} hookSubTip={} arrow={}",
                 mRemoteEquipItem, mLoadedHeldItem, (void*)mpHeldItemModel, desc->bmdResId,
                 (void*)mpHeldItemBrk, (void*)mpHookTipModel, (void*)mpHookSubItemModel,
                 (void*)mpHookSubTipModel, (void*)mpArrowModel);
}

void daRemoteLink_c::setupLinkedItemModels() {
    if (mVisualState.form == FORM_WOLF) {
        mpKanteraModel = NULL;
        mpKanteraGlowModel = NULL;
        mpItemActorModel = NULL;
        mpRideActorModel = NULL;
        mLoadedItemActorKind = -1;
        mLoadedRideActorKind = -1;
        mKanteraMatrixValid = false;
        mKanteraGlowMatrixValid = false;
        mItemActorMatrixValid = false;
        mRideActorMatrixValid = false;
        mMidnaMatrixValid = false;
        mMidnaMaskMatrixValid = false;
        mMidnaHandMatrixValid = false;
        mMidnaHairMatrixValid = false;
        mMidnaGlowMatrixValid = false;
        return;
    }

    if (mpKanteraModel == NULL) {
        mpKanteraModel =
            initModel(static_cast<J3DModelData*>(getOwnedObjectRes("al_kantera.bmd")), 0);
    }
    if (mpKanteraGlowModel == NULL) {
        mpKanteraGlowModel =
            initModel(static_cast<J3DModelData*>(getOwnedObjectRes("ef_ktGlow.bmd")), 0x200);
    }

    if (mLoadedItemActorKind != mRemoteItemActorKind) {
        mpItemActorModel = NULL;
        mLoadedItemActorKind = -1;
        mItemActorMatrixValid = false;

        s32 resId = -1;
        switch (mRemoteItemActorKind) {
        case 1: resId = dRes_ID_ALINK_BMD_AL_BOOM_e; break;
        case 2: resId = dRes_ID_ALINK_BMD_AL_BOMB_e; break;
        case 3: resId = dRes_ID_ALINK_BMD_PG_e; break;
        case 4: resId = dRes_ID_ALINK_BMD_PB_e; break;
        default: break;
        }

        if (resId >= 0) {
            mpItemActorModel = initModel(static_cast<J3DModelData*>(
                                             getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                                 resId)),
                                         0x80000, 0);
        }

        if (mpItemActorModel != NULL || mRemoteItemActorKind == 0) {
            mLoadedItemActorKind = mRemoteItemActorKind;
        }
    }

    if (mLoadedRideActorKind != mRemoteRideActorKind) {
        mpRideActorModel = NULL;
        mLoadedRideActorKind = -1;
        mRideActorMatrixValid = false;

        if (mRemoteRideActorKind == 1) {
            mpRideActorModel = initModel(static_cast<J3DModelData*>(
                                             getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                                 dRes_ID_ALINK_BMD_AL_SP_e)),
                                         0x80000, 0);
        }

        if (mpRideActorModel != NULL || mRemoteRideActorKind == 0) {
            mLoadedRideActorKind = mRemoteRideActorKind;
        }
    }

    if (mpItemActorModel != NULL || mpRideActorModel != NULL) {
        DuskLog.info("RemoteLink: linked actor models loaded equip={} itemKind={} rideKind={} "
                     "itemActor={} rideActor={}",
                     mRemoteEquipItem, mLoadedItemActorKind, mLoadedRideActorKind,
                     (void*)mpItemActorModel, (void*)mpRideActorModel);
    }
}

J3DAnmTransform* daRemoteLink_c::loadMotionBck(u16 i_resId) {
    JKRArchive* anmArchive = dComIfGp_getAnmArchive();
    if (anmArchive == NULL || i_resId == 0) {
        return NULL;
    }

    void* bckBuffer = JKR_NEW_ARRAY_ARGS(u8, l_remoteLinkAnimBufferSize, 0x20);
    if (bckBuffer == NULL) {
        return NULL;
    }

    const u32 readSize =
        JKRReadIdxResource(bckBuffer, l_remoteLinkAnimBufferSize, i_resId, anmArchive);
    if (readSize == 0) {
        DuskLog.warn("RemoteLink: motion BCK read failed resId={}", i_resId);
        return NULL;
    }
    return static_cast<J3DAnmTransform*>(J3DAnmLoaderDataBase::load(bckBuffer));
}

J3DAnmTransform* daRemoteLink_c::getMotionBck(u16 i_resId) {
    for (u32 i = 0; i < ARRAY_SIZE(mBckCache); ++i) {
        if (mBckCache[i].resId == i_resId) {
            return mBckCache[i].bck;
        }
    }

    for (u32 i = 0; i < ARRAY_SIZE(mBckCache); ++i) {
        if (mBckCache[i].resId == 0) {
            J3DAnmTransform* bck = loadMotionBck(i_resId);
            if (bck == NULL) {
                return NULL;
            }
            mBckCache[i].resId = i_resId;
            mBckCache[i].bck = bck;
            return bck;
        }
    }

    DuskLog.warn("RemoteLink: BCK cache full, could not load resId={}", i_resId);
    return NULL;
}

bool daRemoteLink_c::setMotionBck(u16 i_resId, f32 i_speed) {
    J3DAnmTransform* bck = getMotionBck(i_resId);
    if (bck == NULL || mpMotionBck == NULL) {
        return false;
    }

    if (mMotionBckResId != i_resId) {
        if (!mpMotionBck->init(bck, TRUE, J3DFrameCtrl::EMode_LOOP, i_speed, 0, -1, true)) {
            return false;
        }
        mMotionBckResId = i_resId;
        DuskLog.info("RemoteLink: motion BCK changed resId={} speed={}", i_resId, i_speed);
    } else {
        mpMotionBck->setPlaySpeed(i_speed);
    }

    return true;
}

void daRemoteLink_c::setupMotionAnimation() {
    const u16 waitResId = mVisualState.form == FORM_WOLF ? dRes_ID_ALANM_BCK_WL_WAITA_e :
                                                            dRes_ID_ALANM_BCK_WAITS_e;
    J3DAnmTransform* waitBck = getMotionBck(waitResId);
    if (mVisualState.form == FORM_WOLF) {
        getMotionBck(dRes_ID_ALANM_BCK_WL_WALKA_e);
        getMotionBck(dRes_ID_ALANM_BCK_WL_DASHA_e);
    } else {
        getMotionBck(dRes_ID_ALANM_BCK_WALKS_e);
        getMotionBck(dRes_ID_ALANM_BCK_DASHS_e);
    }
    if (waitBck == NULL) {
        return;
    }

    mpMotionBck = JKR_NEW mDoExt_bckAnm();
    if (mpMotionBck == NULL ||
        !mpMotionBck->init(waitBck, TRUE, J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1, false))
    {
        mpMotionBck = NULL;
        return;
    }
    mMotionBckResId = waitResId;

    DuskLog.info("RemoteLink: motion BCK cache initialized form={} wait={}",
                 mVisualState.form == FORM_WOLF ? "wolf" : "human", (void*)waitBck);
}

u16 daRemoteLink_c::selectActionBck(f32* o_speed) {
    if (o_speed != NULL) {
        *o_speed = 1.0f;
    }

    const bool preferUpper =
        mRemoteProcId == daAlink_c::PROC_BOW_SUBJECT ||
        mRemoteProcId == daAlink_c::PROC_BOW_MOVE ||
        mRemoteProcId == daAlink_c::PROC_BOOMERANG_SUBJECT ||
        mRemoteProcId == daAlink_c::PROC_BOOMERANG_MOVE ||
        mRemoteProcId == daAlink_c::PROC_BOOMERANG_CATCH ||
        mRemoteProcId == daAlink_c::PROC_COPY_ROD_SUBJECT ||
        mRemoteProcId == daAlink_c::PROC_COPY_ROD_MOVE ||
        mRemoteProcId == daAlink_c::PROC_COPY_ROD_SWING ||
        mRemoteProcId == daAlink_c::PROC_COPY_ROD_REVIVE ||
        mRemoteProcId == daAlink_c::PROC_GRAB_READY ||
        mRemoteProcId == daAlink_c::PROC_GRAB_UP ||
        mRemoteProcId == daAlink_c::PROC_GRAB_MISS ||
        mRemoteProcId == daAlink_c::PROC_GRAB_THROW ||
        mRemoteProcId == daAlink_c::PROC_GRAB_PUT ||
        mRemoteProcId == daAlink_c::PROC_GRAB_WAIT ||
        mRemoteProcId == daAlink_c::PROC_PICK_UP ||
        mRemoteProcId == daAlink_c::PROC_PICK_PUT ||
        mRemoteProcId == daAlink_c::PROC_BOTTLE_DRINK ||
        mRemoteProcId == daAlink_c::PROC_BOTTLE_OPEN ||
        mRemoteProcId == daAlink_c::PROC_BOTTLE_SWING ||
        mRemoteProcId == daAlink_c::PROC_BOTTLE_GET ||
        mRemoteProcId == daAlink_c::PROC_KANDELAAR_SWING ||
        mRemoteProcId == daAlink_c::PROC_KANDELAAR_POUR ||
        mRemoteProcId == daAlink_c::PROC_SWORD_UNEQUIP_SP ||
        mRemoteProcId == daAlink_c::PROC_BOOTS_EQUIP;

    if (preferUpper && isValidRemoteBck(mRemoteUpperBck2)) {
        if (o_speed != NULL) {
            *o_speed = mRemoteUpperRate2;
        }
        return mRemoteUpperBck2;
    }

    if (isValidRemoteBck(mRemoteUnderBck0)) {
        if (o_speed != NULL) {
            *o_speed = mRemoteUnderRate0;
        }
        return mRemoteUnderBck0;
    }

    switch (mRemoteProcId) {
    case daAlink_c::PROC_SIDESTEP:
    case daAlink_c::PROC_SIDESTEP_LAND:
        return mRemoteProcVar2 < 0 ? dRes_ID_ALANM_BCK_STEPL_e : dRes_ID_ALANM_BCK_STEPR_e;
    case daAlink_c::PROC_SLIDE:
    case daAlink_c::PROC_SLIDE_LAND:
    case daAlink_c::PROC_SLIP:
        return dRes_ID_ALANM_BCK_SLIDEF_e;
    case daAlink_c::PROC_FRONT_ROLL:
        return dRes_ID_ALANM_BCK_ROLLF_e;
    case daAlink_c::PROC_FRONT_ROLL_CRASH:
        return dRes_ID_ALANM_BCK_ROLLFMIS_e;
    case daAlink_c::PROC_FRONT_ROLL_SUCCESS:
        return dRes_ID_ALANM_BCK_ROLLFGOOD_e;
    case daAlink_c::PROC_SIDE_ROLL:
    case daAlink_c::PROC_BACK_JUMP:
        return dRes_ID_ALANM_BCK_ROLLBST_e;
    case daAlink_c::PROC_BACK_JUMP_LAND:
        return dRes_ID_ALANM_BCK_ROLLBED_e;
    case daAlink_c::PROC_AUTO_JUMP:
    case daAlink_c::PROC_DIVE_JUMP:
    case daAlink_c::PROC_ROLL_JUMP:
    case daAlink_c::PROC_SMALL_JUMP:
        return dRes_ID_ALANM_BCK_JUMPS_e;
    case daAlink_c::PROC_FALL:
        return dRes_ID_ALANM_BCK_JUMPSED_e;
    case daAlink_c::PROC_LAND:
        return dRes_ID_ALANM_BCK_LANDINGUP_e;
    case daAlink_c::PROC_CROUCH:
    case daAlink_c::PROC_CRAWL_START:
    case daAlink_c::PROC_CRAWL_MOVE:
    case daAlink_c::PROC_CRAWL_AUTO_MOVE:
    case daAlink_c::PROC_CRAWL_END:
        return dRes_ID_ALANM_BCK_CROUCH_e;
    case daAlink_c::PROC_GUARD_SLIP:
    case daAlink_c::PROC_GUARD_ATTACK:
    case daAlink_c::PROC_GUARD_BREAK:
        return dRes_ID_ALANM_BCK_CROUCHDEFS_e;
    case daAlink_c::PROC_CUT_NORMAL:
        return dRes_ID_ALANM_BCK_CUTA_e;
    case daAlink_c::PROC_CUT_FINISH:
        return dRes_ID_ALANM_BCK_CUTL_e;
    case daAlink_c::PROC_CUT_REVERSE:
        return dRes_ID_ALANM_BCK_CUTREL_e;
    case daAlink_c::PROC_CUT_JUMP:
        return dRes_ID_ALANM_BCK_CUTJST_e;
    case daAlink_c::PROC_CUT_JUMP_LAND:
        return dRes_ID_ALANM_BCK_CUTJED_e;
    case daAlink_c::PROC_CUT_TURN:
    case daAlink_c::PROC_CUT_TURN_CHARGE:
    case daAlink_c::PROC_CUT_TURN_MOVE:
        return dRes_ID_ALANM_BCK_CUTT_e;
    case daAlink_c::PROC_CUT_DOWN:
    case daAlink_c::PROC_CUT_DOWN_LAND:
        return dRes_ID_ALANM_BCK_CUTDTP_e;
    case daAlink_c::PROC_CUT_HEAD:
    case daAlink_c::PROC_CUT_HEAD_LAND:
        return dRes_ID_ALANM_BCK_CUTHTB_e;
    case daAlink_c::PROC_DAMAGE:
        return dRes_ID_ALANM_BCK_WAITB_e;
    case daAlink_c::PROC_LARGE_DAMAGE_UP:
    case daAlink_c::PROC_LAND_DAMAGE:
        return dRes_ID_ALANM_BCK_LANDDAMA_e;
    case daAlink_c::PROC_HANG_START:
    case daAlink_c::PROC_HANG_FALL_START:
    case daAlink_c::PROC_HANG_WAIT:
    case daAlink_c::PROC_HANG_READY:
    case daAlink_c::PROC_HANG_WALL_CATCH:
        return dRes_ID_ALANM_BCK_HANG_e;
    case daAlink_c::PROC_HANG_UP:
    case daAlink_c::PROC_HANG_CLIMB:
        return dRes_ID_ALANM_BCK_HANGUP_e;
    case daAlink_c::PROC_HANG_MOVE:
        return mRemoteProcVar2 < 0 ? dRes_ID_ALANM_BCK_HANGL_e : dRes_ID_ALANM_BCK_HANGR_e;
    case daAlink_c::PROC_BOW_SUBJECT:
    case daAlink_c::PROC_BOW_MOVE:
        return dRes_ID_ALANM_BCK_WAITHS_e;
    case daAlink_c::PROC_BOOMERANG_SUBJECT:
    case daAlink_c::PROC_BOOMERANG_MOVE:
    case daAlink_c::PROC_COPY_ROD_SUBJECT:
    case daAlink_c::PROC_COPY_ROD_MOVE:
        return dRes_ID_ALANM_BCK_BOOMWAIT_e;
    case daAlink_c::PROC_BOOMERANG_CATCH:
        return dRes_ID_ALANM_BCK_BOOMCATCH_e;
    case daAlink_c::PROC_COPY_ROD_SWING:
        return dRes_ID_ALANM_BCK_BOOMTHROW_e;
    case daAlink_c::PROC_LADDER_UP_START:
    case daAlink_c::PROC_LADDER_DOWN_START:
    case daAlink_c::PROC_LADDER_MOVE:
    case daAlink_c::PROC_CLIMB_MOVE_UPDOWN:
    case daAlink_c::PROC_CLIMB_MOVE_SIDE:
        return dRes_ID_ALANM_BCK_CLIMBL_e;
    case daAlink_c::PROC_LADDER_UP_END:
    case daAlink_c::PROC_LADDER_DOWN_END:
    case daAlink_c::PROC_CLIMB_WAIT:
    case daAlink_c::PROC_CLIMB_UP_START:
    case daAlink_c::PROC_CLIMB_DOWN_START:
        return dRes_ID_ALANM_BCK_CLIMBHANG_e;
    case daAlink_c::PROC_GRAB_READY:
    case daAlink_c::PROC_GRAB_WAIT:
    case daAlink_c::PROC_PICK_UP:
    case daAlink_c::PROC_PICK_PUT:
        return dRes_ID_ALANM_BCK_WAITST_e;
    case daAlink_c::PROC_GRAB_UP:
    case daAlink_c::PROC_GRAB_THROW:
    case daAlink_c::PROC_GRAB_PUT:
    case daAlink_c::PROC_GRAB_MISS:
        return dRes_ID_ALANM_BCK_GETAWAIT_e;
    case daAlink_c::PROC_SWIM_WAIT:
        return dRes_ID_ALANM_BCK_SWIMWAIT_e;
    case daAlink_c::PROC_SWIM_MOVE:
    case daAlink_c::PROC_SWIM_UP:
        return dRes_ID_ALANM_BCK_SWIMINGA_e;
    case daAlink_c::PROC_SWIM_DIVE:
        return dRes_ID_ALANM_BCK_SWIMDIVE_e;
    case daAlink_c::PROC_BOTTLE_DRINK:
    case daAlink_c::PROC_BOTTLE_OPEN:
    case daAlink_c::PROC_BOTTLE_SWING:
    case daAlink_c::PROC_BOTTLE_GET:
        return dRes_ID_ALANM_BCK_WAITD_e;
    case daAlink_c::PROC_KANDELAAR_SWING:
    case daAlink_c::PROC_KANDELAAR_POUR:
        return dRes_ID_ALANM_BCK_WAITK_e;
    case daAlink_c::PROC_GRASS_WHISTLE_WAIT:
    case daAlink_c::PROC_GRASS_WHISTLE_GET:
        return dRes_ID_ALANM_BCK_WAITPP_e;
    case daAlink_c::PROC_SPINNER_READY:
    case daAlink_c::PROC_SPINNER_WAIT:
        return dRes_ID_ALANM_BCK_WAITST_e;
    case daAlink_c::PROC_BOOTS_EQUIP:
    case daAlink_c::PROC_SWORD_UNEQUIP_SP:
        return dRes_ID_ALANM_BCK_WAITATOS_e;
    default:
        break;
    }

    if (mRemoteSwordDraw || mRemoteSwordOut || mRemoteEquipItem == 0x103) {
        if (mRemoteMoveSpeed >= l_dashSpeedThreshold) {
            return dRes_ID_ALANM_BCK_DASHHBS_e;
        }
        if (mRemoteMoveSpeed >= l_walkSpeedThreshold) {
            return dRes_ID_ALANM_BCK_WALKHBS_e;
        }
        return dRes_ID_ALANM_BCK_WAITHS_e;
    }

    if (mVisualState.form == FORM_WOLF) {
        if (mRemoteMoveSpeed >= l_dashSpeedThreshold) {
            return dRes_ID_ALANM_BCK_WL_DASHA_e;
        }
        if (mRemoteMoveSpeed >= l_walkSpeedThreshold) {
            return dRes_ID_ALANM_BCK_WL_WALKA_e;
        }
        return dRes_ID_ALANM_BCK_WL_WAITA_e;
    }

    if (mRemoteMoveSpeed >= l_dashSpeedThreshold) {
        return dRes_ID_ALANM_BCK_DASHS_e;
    }
    if (mRemoteMoveSpeed >= l_walkSpeedThreshold) {
        return dRes_ID_ALANM_BCK_WALKS_e;
    }
    return dRes_ID_ALANM_BCK_WAITS_e;
}

void daRemoteLink_c::updateMotionAnimation() {
    f32 speed = 1.0f;
    const u16 bckResId = selectActionBck(&speed);
    const bool transformProc = isRemoteTransformProc(mRemoteProcId);
    const bool bckChanged = mMotionBckResId != bckResId;
    if (setMotionBck(bckResId, speed)) {
        if (transformProc) {
            const f32 targetFrame =
                bckResId == mRemoteUpperBck2 ? mRemoteUpperFrame2 : mRemoteUnderFrame0;
            f32 frameStep = std::fabs(bckResId == mRemoteUpperBck2 ? mRemoteUpperRate2 :
                                                                      mRemoteUnderRate0);
            if (frameStep < 0.01f) {
                frameStep = 1.0f;
            }

            if (!mRemoteTransformFrameValid || bckChanged ||
                targetFrame + 8.0f < mRemoteTransformFrame)
            {
                mRemoteTransformFrame = targetFrame;
                mRemoteTransformFrameValid = true;
            } else {
                const bool wolfToHumanHumanPhase =
                    mVisualState.form != FORM_WOLF && mRemoteProcVar5 != 0;
                const f32 maxPredictAhead = wolfToHumanHumanPhase ? 0.0f : 2.0f;
                if (mRemoteTransformFrame < targetFrame) {
                    mRemoteTransformFrame += frameStep;
                    if (mRemoteTransformFrame > targetFrame) {
                        mRemoteTransformFrame = targetFrame;
                    }
                } else if (mRemoteTransformFrame < targetFrame + maxPredictAhead) {
                    mRemoteTransformFrame += frameStep;
                    if (mRemoteTransformFrame > targetFrame + maxPredictAhead) {
                        mRemoteTransformFrame = targetFrame + maxPredictAhead;
                    }
                }
            }

            const f32 endFrame = mpMotionBck->getEndFrame();
            if (endFrame > 0.0f && mRemoteTransformFrame > endFrame) {
                mRemoteTransformFrame = endFrame;
            }
            mpMotionBck->setFrame(mRemoteTransformFrame);
            return;
        }

        mRemoteTransformFrameValid = false;
        if (bckResId == mRemoteUpperBck2) {
            mpMotionBck->setFrame(mRemoteUpperFrame2);
        } else if (bckResId == mRemoteUnderBck0) {
            mpMotionBck->setFrame(mRemoteUnderFrame0);
        }
    }
}

bool daRemoteLink_c::reserveSlot() {
    if (mSlotReserved) {
        return true;
    }

    if (!canReserveSlot()) {
        DuskLog.warn("RemoteLink: refusing create, live actor cap reached ({}/{})",
                     sLiveRemoteLinkActors, l_maxRemoteLinkActors);
        return false;
    }

    sLiveRemoteLinkActors++;
    mSlotReserved = true;
    DuskLog.info("RemoteLink: reserved slot live={}/{}", sLiveRemoteLinkActors,
                 l_maxRemoteLinkActors);
    return true;
}

bool daRemoteLink_c::canReserveSlot() {
    return sLiveRemoteLinkActors < l_maxRemoteLinkActors;
}

void daRemoteLink_c::releaseSlot() {
    if (!mSlotReserved) {
        return;
    }

    if (sLiveRemoteLinkActors > 0) {
        sLiveRemoteLinkActors--;
    }
    mSlotReserved = false;
    DuskLog.info("RemoteLink: released slot live={}/{}", sLiveRemoteLinkActors,
                 l_maxRemoteLinkActors);
}

int daRemoteLink_c::CreateHeap() {
    ResTIMG* warpTex =
        static_cast<ResTIMG*>(dComIfG_getObjectRes("Always", dRes_ID_ALWAYS_BTI_WARP_TEX_e));
    if (warpTex != NULL) {
        mpWarpTexData = (void*)((uintptr_t)warpTex + warpTex->imageOffset);
    }

    if (mVisualState.form == FORM_WOLF) {
        setupWolfModel();
        if (mpBodyModel == NULL) {
            DuskLog.warn("RemoteLink: failed to create wolf visual model");
            return FALSE;
        }
    } else {
        setupHumanKokiriModel();
        if (mpBodyModel == NULL || mpHeadModel == NULL || mpHandModel == NULL ||
            mpFaceModel == NULL)
        {
            DuskLog.warn("RemoteLink: failed to create human visual model set");
            return FALSE;
        }
    }

    setupMotionAnimation();
    if (mpMotionBck == NULL) {
        DuskLog.warn("RemoteLink: failed to create motion animation");
        return FALSE;
    }

    return TRUE;
}

void daRemoteLink_c::initPvpTargetCollision() {
    mPvpTargetStts.Init(0xFF, 0, this);
    mPvpTargetCyl.Set(l_pvpTargetCylSrc);
    mPvpTargetCyl.SetStts(&mPvpTargetStts);
    mPvpTargetCyl.SetTgGrp(0x1E);
    mPvpTargetCyl.OnTgNoConHit();
    mPvpTargetCyl.SetTgShieldFrontRangeYAngle(&mPvpShieldFrontAngle);
    mPvpTargetCyl.SetTgHitCallback(daRemoteLink_pvpTargetHitCallback);
    mPvpTargetCyl.OffAtSetBit();
    mPvpTargetCyl.OffCoSetBit();
    mPvpTargetCollisionInitialized = true;
}

cPhs_Step daRemoteLink_c::create() {
    fopAcM_ct(this, daRemoteLink_c);

    const u32 params = fopAcM_GetParam(this);
    mClothesVariant = static_cast<int>(params & 0xFF);
    mVisualState.form = (params & 0x100) != 0 ? FORM_WOLF : FORM_HUMAN_KOKIRI;
    setOriginalHeap(&mpArcHeap, 0x400000);
    cPhs_Step step = cPhs_COMPLEATE_e;
    if (step == cPhs_COMPLEATE_e) {
        if (!reserveSlot()) {
            return cPhs_ERROR_e;
        }

        u32 heapSize = 0x180000 | 0x80000000 | 0x40000000;
        if (!fopAcM_entrySolidHeap(this, createHeapCallBack, heapSize)) {
            DuskLog.warn("RemoteLink: heap entry failed");
            releaseSlot();
            return cPhs_ERROR_e;
        }

        if (scale.x == 0.0f && scale.y == 0.0f && scale.z == 0.0f) {
            scale.set(1.0f, 1.0f, 1.0f);
        }

        setBaseMtx();
        calcModels();
        fopAcM_SetMtx(this, mpBodyModel->getBaseTRMtx());
        model = mpBodyModel;
        fopAcM_setCullSizeBox2(this, mpBodyModel->getModelData());
        initPvpTargetCollision();
        DuskLog.info("RemoteLink: visual actor created form={} pos=({}, {}, {}) room={} "
                     "scale=({}, {}, {})",
                     mVisualState.form == FORM_WOLF ? "wolf" : "human", current.pos.x,
                     current.pos.y, current.pos.z, fopAcM_GetRoomNo(this), scale.x, scale.y,
                     scale.z);
    }

    return step;
}

void daRemoteLink_c::setBaseMtx() {
    mDoMtx_stack_c::transS(current.pos);
    mDoMtx_stack_c::ZXYrotM(shape_angle);
    mpBodyModel->setBaseScale(scale);
    mpBodyModel->setBaseTRMtx(mDoMtx_stack_c::get());
}

void daRemoteLink_c::updatePvpTargetCollision() {
    if (!mPvpTargetCollisionInitialized) {
        return;
    }

    if (!dusk::multiplayer::pvp_enabled() || isRemoteLinkSceneUnsafe() ||
        isRemotePvpKnockdownProc(mRemoteProcId) || isRemoteDeadProc(mRemoteProcId))
    {
        mPvpTargetCyl.OffTgSetBit();
        mPvpTargetCyl.ResetTgHit();
        return;
    }

    const f32 height = mVisualState.form == FORM_WOLF ? 95.0f : 180.0f;
    const f32 radius = mVisualState.form == FORM_WOLF ? 35.0f : 35.0f;
    cXyz center = current.pos;
    center.y -= 20.0f;
    mPvpTargetCyl.SetC(center);
    mPvpTargetCyl.SetH(height);
    mPvpTargetCyl.SetR(radius);
    cXyz shieldForward;
    if (mRemoteShieldGuardActive && mVisualState.form != FORM_WOLF && mpShieldModel != NULL) {
        mDoMtx_multVecSR(mpShieldModel->getBaseTRMtx(), &cXyz::BaseZ, &shieldForward);
        if (shieldForward.absXZ() > 0.01f) {
            mPvpShieldFrontAngle = shieldForward.atan2sX_Z();
        } else {
            mPvpShieldFrontAngle = shape_angle.y;
        }
        mPvpTargetCyl.OnTgShield();
        mPvpTargetCyl.OnTgShieldFrontRange();
    } else {
        mPvpShieldFrontAngle = shape_angle.y;
        mPvpTargetCyl.OffTgShield();
        mPvpTargetCyl.OffTgShieldFrontRange();
    }
    mPvpTargetCyl.OnTgSetBit();
    mPvpTargetCyl.OffAtSetBit();
    mPvpTargetCyl.OffCoSetBit();
    dComIfG_Ccsp()->Set(&mPvpTargetCyl);

    if (sPvpTargetLogCount < 20 || (++sPvpTargetLogTicks % 120) == 0) {
        ++sPvpTargetLogCount;
        DuskLog.info("RemoteLink: PvP target active id={} form={} pos=({}, {}, {}) "
                     "room={} tg_type={:#x} tg_grp={:#x} radius={} height={}",
                     fopAcM_GetID(this),
                     mVisualState.form == FORM_WOLF ? "wolf" : "human", center.x, center.y,
                     center.z, fopAcM_GetRoomNo(this), mPvpTargetCyl.GetTgType(),
                     mPvpTargetCyl.GetTgGrp(), radius, height);
    }
}

void daRemoteLink_c::updatePvpAttentionTarget() {
    if (mpBodyModel != NULL && mpBodyModel->getModelData() != NULL &&
        mpBodyModel->getModelData()->getJointNum() > 4)
    {
        MtxP neckMtx = mpBodyModel->getAnmMtx(4);
        Mtx interpNeckMtx;
        if (dusk::frame_interp::lookup_replacement(neckMtx, interpNeckMtx)) {
            neckMtx = interpNeckMtx;
        }
        eyePos.set(neckMtx[0][3], neckMtx[1][3], neckMtx[2][3]);
    } else {
        eyePos = current.pos;
        eyePos.y += mVisualState.form == FORM_WOLF ? 46.0f : 82.0f;
    }

    attention_info.position = eyePos;
    attention_info.position.y += mVisualState.form == FORM_WOLF ? 22.0f : 50.0f;
    attention_info.field_0xa = 30;
    // Match ordinary ground enemies such as Bulblins and ReDeads instead of the
    // very long-range preset previously used here.
    attention_info.distances[fopAc_attn_BATTLE_e] = 3;

    if (mHasRemotePose && dusk::multiplayer::pvp_enabled() && !isRemoteLinkSceneUnsafe() &&
        !isRemoteDeadProc(mRemoteProcId))
    {
        attention_info.flags = fopAc_AttnFlag_BATTLE_e;
    } else {
        attention_info.flags = 0;
    }
}

void daRemoteLink_c::updatePvpMidnaBindEffect() {
    daPy_py_c* player = daPy_getPlayerActorClass();
    daMidna_c* midna = player != NULL ? player->getMidnaActor() : NULL;
    const bool isLocked = mHasRemotePose && dusk::multiplayer::pvp_enabled() &&
                          !isRemoteLinkSceneUnsafe() && midna != NULL &&
                          player->checkWolfLock(this);
    if (!isLocked) {
        if (mPvpMidnaBindActive) {
            mActiveSoundObj.stopSound(Z2SE_MIDNA_BIND_LOCK_SUS, 2);
        }
        mPvpMidnaBindActive = false;
        return;
    }

    static const GXColor primColors[] = {
        {0xFF, 0x78, 0x00, 0x00},
        {0xFF, 0x64, 0x78, 0x00},
    };
    static const GXColor envColors[] = {
        {0x5A, 0x2D, 0x2D, 0x00},
        {0x3C, 0x1E, 0x1E, 0x00},
    };
    static const u16 bindEffectIds[] = {0x29D, 0x29E, 0x29F};

    const int colorIndex = dKy_darkworld_check() ? 1 : 0;
    cXyz effectPos = eyePos;
    cXyz effectScale(1.0f, 1.0f, 1.0f);

    if (!mPvpMidnaBindActive) {
        csXyz effectAngle;
        cXyz hairPos;
        MTXCopy(midna->getMtxHairTop(), mDoMtx_stack_c::get());
        cXyz hairOffset(nREG_F(8) + 100.0f, nREG_F(9), nREG_F(10));
        mDoMtx_stack_c::multVec(&hairOffset, &hairPos);

        cXyz direction = hairPos - effectPos;
        effectAngle.y = cM_atan2s(direction.x, direction.z);
        effectAngle.x =
            -cM_atan2s(direction.y,
                       JMAFastSqrt(direction.x * direction.x + direction.z * direction.z));
        effectAngle.z = 0;

        JPABaseEmitter* emitter = dComIfGp_particle_set(
            0x29B, &effectPos, &tevStr, &effectAngle, &effectScale, 0xFF, NULL,
            fopAcM_GetRoomNo(this), &primColors[colorIndex], &envColors[colorIndex], NULL);
        if (emitter != NULL) {
            emitter->setGlobalParticleHeightScale((JREG_F(7) + 0.01f) * direction.abs());
        }

        dComIfGp_particle_set(0x29C, &effectPos, &tevStr, &shape_angle, &effectScale, 0xFF,
                              NULL, fopAcM_GetRoomNo(this), &primColors[colorIndex],
                              &envColors[colorIndex], NULL);
        mActiveSoundObj.startSound(Z2SE_MIDNA_BIND_LOCK_ON, 0,
                                   dComIfGp_getReverb(fopAcM_GetRoomNo(this)));
        mPvpMidnaBindActive = true;
    }

    mActiveSoundObj.startLevelSound(Z2SE_MIDNA_BIND_LOCK_SUS, 0,
                                    dComIfGp_getReverb(fopAcM_GetRoomNo(this)));

    for (int i = 0; i < 3; ++i) {
        mPvpMidnaBindIds[i] = dComIfGp_particle_set(
            mPvpMidnaBindIds[i], bindEffectIds[i], &effectPos, &tevStr, &shape_angle,
            &effectScale, 0xFF, NULL, fopAcM_GetRoomNo(this), &primColors[colorIndex],
            &envColors[colorIndex], NULL);
    }
}

void daRemoteLink_c::setupDrawHands() {
    if (mpHandModel == NULL || mpBodyModel == NULL) {
        return;
    }

    if (mpLeftBodyHandShape != NULL) {
        mpLeftBodyHandShape->show();
    }
    if (mpRightBodyHandShape != NULL) {
        mpRightBodyHandShape->show();
    }

    mpHandModel->setBaseTRMtx(mpBodyModel->getBaseTRMtx());
    mpHandModel->calc();
    mpHandModel->setAnmMtx(1, mpBodyModel->getAnmMtx(9));
    mpHandModel->setAnmMtx(2, mpBodyModel->getAnmMtx(0xE));
}

void daRemoteLink_c::applyHeavyBootMatrices() {
    if (!mRemoteHeavyBoots || mVisualState.form == FORM_WOLF || mpBodyModel == NULL ||
        mpHeavyBootModels[0] == NULL || mpHeavyBootModels[1] == NULL)
    {
        return;
    }

    J3DModelData* bodyData = mpBodyModel->getModelData();
    J3DModelData* bootData0 = mpHeavyBootModels[0]->getModelData();
    J3DModelData* bootData1 = mpHeavyBootModels[1]->getModelData();
    if (bodyData == NULL || bodyData->getJointNum() <= 0x1A ||
        bootData0 == NULL || bootData0->getJointNum() <= 3 ||
        bootData1 == NULL || bootData1->getJointNum() <= 3)
    {
        return;
    }

    for (int i = 0; i < 2; ++i) {
        mpHeavyBootModels[i]->setBaseTRMtx(mpBodyModel->getBaseTRMtx());
        mpHeavyBootModels[i]->calc();
    }

    mpHeavyBootModels[0]->setAnmMtx(1, mpBodyModel->getAnmMtx(0x13));
    mpHeavyBootModels[0]->setAnmMtx(2, mpBodyModel->getAnmMtx(0x14));
    mpHeavyBootModels[0]->setAnmMtx(3, mpBodyModel->getAnmMtx(0x15));

    mDoMtx_stack_c::XrotS(-0x8000);
    Mtx bootMtx;
    mDoMtx_concat(mpBodyModel->getAnmMtx(0x18), mDoMtx_stack_c::get(), bootMtx);
    mpHeavyBootModels[1]->setAnmMtx(1, bootMtx);
    mDoMtx_concat(mpBodyModel->getAnmMtx(0x19), mDoMtx_stack_c::get(), bootMtx);
    mpHeavyBootModels[1]->setAnmMtx(2, bootMtx);
    mDoMtx_concat(mpBodyModel->getAnmMtx(0x1A), mDoMtx_stack_c::get(), bootMtx);
    mpHeavyBootModels[1]->setAnmMtx(3, bootMtx);
}

void daRemoteLink_c::applyWolfEquipmentMatrices() {
    if (mVisualState.form != FORM_WOLF || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 2)
    {
        return;
    }

    if (mpSwordModel != NULL) {
        mDoMtx_stack_c::copy(mpBodyModel->getAnmMtx(2));
        mDoMtx_stack_c::transM(31.0f, -29.0f, 19.0f);
        mDoMtx_stack_c::XYZrotM(0, cM_deg2s(32.0f), cM_deg2s(157.0f));
        mpSwordModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpSwordModel->calc();
    }

    if (mpSheathModel != NULL) {
        mDoMtx_stack_c::copy(mpBodyModel->getAnmMtx(2));
        mDoMtx_stack_c::transM(13.0f, -21.0f, 7.0f);
        mDoMtx_stack_c::XYZrotM(0, cM_deg2s(-0.8f), cM_deg2s(157.0f));
        mpSheathModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpSheathModel->calc();
    }

    if (mpShieldModel != NULL) {
        mDoMtx_stack_c::copy(mpBodyModel->getAnmMtx(2));
        mDoMtx_stack_c::transM(11.0f, -18.0f, -13.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(90.0f), cM_deg2s(58.0f), cM_deg2s(-24.0f));
        mpShieldModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpShieldModel->calc();
    }
}

int daRemoteLink_c::headModelCallBack(int i_jointNo) {
    if (mpHeadModel == NULL || i_jointNo < 0 ||
        i_jointNo >= static_cast<int>(mRemoteHatRotA.size()))
    {
        return 1;
    }

    if (i_jointNo >= 6) {
        mDoMtx_stack_c::copy(J3DSys::mCurrentMtx);

        if (i_jointNo == 6) {
            mDoMtx_stack_c::XYZrotM(0, (mRemoteHatRotB[7] >> 1),
                                    (mRemoteHatRotA[7] >> 1));
        } else {
            const int swingIndex = i_jointNo - 7;
            if (swingIndex == 0) {
                mDoMtx_stack_c::XYZrotM(0, (mRemoteHatRotB[7] >> 1),
                                        (mRemoteHatRotA[7] >> 1) + mRemoteHatSwing[0]);
            } else if (swingIndex >= 0 &&
                       swingIndex < static_cast<int>(mRemoteHatSwing.size()))
            {
                mDoMtx_stack_c::XYZrotM(0, mRemoteHatRotB[i_jointNo],
                                        mRemoteHatRotA[i_jointNo] +
                                            mRemoteHatSwing[swingIndex]);
            }
        }

        mpHeadModel->setAnmMtx(i_jointNo, mDoMtx_stack_c::get());
        mDoMtx_copy(mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);
    } else {
        const s16 prevShapeY = shape_angle.y;
        shape_angle.y = mRemoteHatShapeY;
        setRemoteMatrixWorldAxisRot(this, mpHeadModel->getAnmMtx(i_jointNo),
                                    mRemoteHatRotA[i_jointNo], 0,
                                    mRemoteHatRotB[i_jointNo]);
        shape_angle.y = prevShapeY;
    }

    return 1;
}

void daRemoteLink_c::calcModels() {
    if (mHasRemoteMatrices) {
        return;
    }

    if (mpMotionBck != NULL) {
        updateMotionAnimation();
        if (!isRemoteTransformProc(mRemoteProcId)) {
            mpMotionBck->play();
        }
        mpMotionBck->entry(mpBodyModel->getModelData());
    }
    mpBodyModel->calc();

    if (mVisualState.form == FORM_WOLF) {
        if (mpMidnaModel != NULL) {
            mpMidnaModel->calc();
        }
        if (mpMidnaMaskModel != NULL) {
            mpMidnaMaskModel->calc();
        }
        if (mpMidnaHandModel != NULL) {
            mpMidnaHandModel->calc();
        }
        if (mpMidnaHairModel != NULL) {
            mpMidnaHairModel->calc();
        }
        applyWolfEquipmentMatrices();
        return;
    }

    setupDrawHands();
    applyHeavyBootMatrices();

    if (mpFaceModel != NULL) {
        mpFaceModel->setBaseTRMtx(mpBodyModel->getAnmMtx(4));
        mpFaceModel->calc();
    }

    if (mpHeadModel != NULL) {
        mpHeadModel->setBaseTRMtx(mpBodyModel->getAnmMtx(4));
        mpHeadModel->calc();
    }

    if (mpHeldItemModel != NULL) {
        mpHeldItemModel->calc();
    }
    if (mpHookTipModel != NULL) {
        mpHookTipModel->calc();
    }
    if (mpHookSubItemModel != NULL) {
        mpHookSubItemModel->calc();
    }
    if (mpHookSubTipModel != NULL) {
        mpHookSubTipModel->calc();
    }
    if (mpArrowModel != NULL) {
        mpArrowModel->calc();
    }
    if (mpKanteraModel != NULL) {
        mpKanteraModel->calc();
    }
    if (mpKanteraGlowModel != NULL) {
        mpKanteraGlowModel->calc();
    }
    if (mpItemActorModel != NULL) {
        mpItemActorModel->calc();
    }
    if (mpRideActorModel != NULL) {
        mpRideActorModel->calc();
    }

    if (sCalcLogCount < 5 && mpHeadModel != NULL) {
        MtxP root = mpBodyModel->getAnmMtx(0);
        MtxP head = mpHeadModel->getBaseTRMtx();
        DuskLog.info("RemoteLink: calc root=({}, {}, {}) head=({}, {}, {})", root[0][3],
                     root[1][3], root[2][3], head[0][3], head[1][3], head[2][3]);
        sCalcLogCount++;
    }
}

int daRemoteLink_c::Execute() {
    updatePvpAttentionTarget();
    if (isRemoteLinkSceneUnsafe()) {
        mPvpMidnaBindActive = false;
        stopRemoteActiveSounds();
        return TRUE;
    }

    if (remoteMatrixLocalInterpolationEnabled()) {
        dusk::frame_interp::add_interpolation_callback(&daRemoteLink_matrixInterpCallback, this);
        if (mRemoteMatrixTicksSinceCapture < 120) {
            ++mRemoteMatrixTicksSinceCapture;
        }
    }

    if (isRemoteBombActorKind(mRemoteItemActorKind) && mItemActorMatrixValid) {
        ++mRemoteBombFlashTicks;
        if (mRemoteBombExTime > 0) {
            --mRemoteBombExTime;
        }
    } else {
        mRemoteBombFlashTicks = 0;
    }

    if (!mHasRemoteMatrices) {
        setBaseMtx();
        calcModels();
    }
    updatePvpTargetCollision();
    updatePvpMidnaBindEffect();
    updateRemoteBombActor();
    mActiveSoundObj.framework(0, dComIfGp_getReverb(fopAcM_GetRoomNo(this)));
    for (ActiveRemoteSound& sound : mActiveSounds) {
        if (!sound.active) {
            continue;
        }
        if (++sound.ageTicks > 8) {
            mActiveSoundObj.stopSound(sound.event.soundId, 2);
            sound.active = false;
        }
    }
    return TRUE;
}

void daRemoteLink_c::setRemotePose(const cXyz& i_pos, s16 i_angleY, s8 i_roomNo) {
    if (mHasRemotePose) {
        const f32 dx = i_pos.x - mLastRemotePos.x;
        const f32 dz = i_pos.z - mLastRemotePos.z;
        const f32 distance = std::sqrt((dx * dx) + (dz * dz));
        mRemoteMoveSpeed = distance > 200.0f ? 0.0f : distance;
    } else {
        mRemoteMoveSpeed = 0.0f;
        mHasRemotePose = true;
    }
    mLastRemotePos = i_pos;

    old.pos = current.pos;
    current.pos = i_pos;
    old.angle = current.angle;
    current.angle.set(0, i_angleY, 0);
    shape_angle.set(0, i_angleY, 0);
    fopAcM_SetRoomNo(this, i_roomNo);
}

void daRemoteLink_c::setRemoteActionState(int i_procId, int i_procVar0, int i_procVar1,
                                          int i_procVar2, int i_procVar3, int i_procVar5,
                                          f32 i_underFrame, u16 i_underBck0, f32 i_underFrame0,
                                          f32 i_underRate0, u16 i_upperBck2, f32 i_upperFrame2,
                                          f32 i_upperRate2, u16 i_equipItem,
                                          int i_swordVariant, int i_shieldVariant,
                                          bool i_swordDraw, bool i_shieldDraw,
                                          bool i_shieldGuardActive, bool i_swordOut,
                                          bool i_heavyBoots, bool i_itemDraw, bool i_kanteraDraw,
                                          bool i_midnaDraw, bool i_midnaMaskDraw,
                                          bool i_midnaHandDraw, bool i_midnaHairDraw,
                                          bool i_midnaShadowForm,
                                          int i_itemActorKind, int i_itemActorBombExTime,
                                          int i_itemActorBombFlash, int i_rideActorKind) {
    mRemoteProcId = i_procId;
    mRemoteProcVar0 = i_procVar0;
    mRemoteProcVar1 = i_procVar1;
    mRemoteProcVar2 = i_procVar2;
    mRemoteProcVar3 = i_procVar3;
    mRemoteProcVar5 = i_procVar5;
    mRemoteUnderFrame = i_underFrame;
    mRemoteUnderBck0 = i_underBck0;
    mRemoteUnderFrame0 = i_underFrame0;
    mRemoteUnderRate0 = i_underRate0;
    mRemoteUpperBck2 = i_upperBck2;
    mRemoteUpperFrame2 = i_upperFrame2;
    mRemoteUpperRate2 = i_upperRate2;
    mRemoteEquipItem = i_equipItem;
    mRemoteSwordVariant = i_swordVariant;
    mRemoteShieldVariant = i_shieldVariant;
    mRemoteSwordDraw = i_swordDraw;
    mRemoteShieldDraw = i_shieldDraw;
    mRemoteShieldGuardActive = i_shieldGuardActive;
    mRemoteSwordOut = i_swordOut;
    mRemoteHeavyBoots = i_heavyBoots;
    mRemoteItemDraw = i_itemDraw;
    mRemoteKanteraDraw = i_kanteraDraw;
    mRemoteMidnaDraw = i_midnaDraw;
    mRemoteMidnaMaskDraw = i_midnaMaskDraw;
    mRemoteMidnaHandDraw = i_midnaHandDraw;
    mRemoteMidnaHairDraw = i_midnaHairDraw;
    mRemoteMidnaShadowForm = i_midnaShadowForm;
    if (mRemoteItemActorKind != i_itemActorKind && isRemoteBombActorKind(i_itemActorKind)) {
        mRemoteBombFlashTicks = 0;
        mRemoteBombExplosionSpawned = false;
    }
    maybeSpawnRemoteBombExplosion(i_itemActorKind);
    mRemoteItemActorKind = i_itemActorKind;
    mRemoteBombExTime = i_itemActorBombExTime;
    mRemoteBombFlash = i_itemActorBombFlash;
    if (isRemoteBombActorKind(mRemoteItemActorKind) &&
        (sRemoteBombFuseLogCount < 24 ||
         (mRemoteBombExTime >= 0 && mRemoteBombExTime <= 20)))
    {
        ++sRemoteBombFuseLogCount;
        DuskLog.info("RemoteLink: bomb fuse rx kind={} ex_time={} flash={} "
                     "matrix_valid={} last_pos_valid={}",
                     mRemoteItemActorKind, mRemoteBombExTime, mRemoteBombFlash,
                     mItemActorMatrixValid, mRemoteBombLastPosValid);
    }
    mRemoteRideActorKind = i_rideActorKind;
    if (isRemoteTransformProc(mRemoteProcId)) {
        mRemoteSwordDraw = false;
        mRemoteShieldDraw = false;
        mRemoteSwordOut = false;
        mRemoteHeavyBoots = false;
        mRemoteItemDraw = false;
        mRemoteKanteraDraw = false;
        mRemoteMidnaDraw = false;
        mRemoteMidnaMaskDraw = false;
        mRemoteMidnaHandDraw = false;
        mRemoteMidnaHairDraw = false;
        mRemoteMidnaShadowForm = false;
        mRemoteItemActorKind = 0;
        mRemoteRideActorKind = 0;
        mHeldItemMatrixValid = false;
        mHookTipMatrixValid = false;
        mHookSubItemMatrixValid = false;
        mHookSubTipMatrixValid = false;
        mArrowMatrixValid = false;
        mKanteraMatrixValid = false;
        mKanteraGlowMatrixValid = false;
        mItemActorMatrixValid = false;
        mRemoteBombExTime = -1;
        mRemoteBombFlash = -1;
        mRideActorMatrixValid = false;
        return;
    }

    mRemoteTransformFrameValid = false;
    setupEquipmentModels();
    setupHeldItemModel();
    setupLinkedItemModels();
}

void daRemoteLink_c::maybeSpawnRemoteBombExplosion(int i_nextItemActorKind) {
    if (remoteBombRealActorEnabled()) {
        return;
    }

    if (!isRemoteBombActorKind(mRemoteItemActorKind) || !mRemoteBombLastPosValid ||
        mRemoteBombExplosionSpawned)
    {
        return;
    }

    if (mRemoteBombExTime <= 0 ||
        (!isRemoteBombActorKind(i_nextItemActorKind) && mRemoteBombExTime <= 20))
     {
        spawnRemoteBombExplosion();
        mRemoteBombExplosionSpawned = true;
    }

    if (!isRemoteBombActorKind(i_nextItemActorKind)) {
        mRemoteBombLastPosValid = false;
        mRemoteBombExplosionSpawned = false;
    }
}

void daRemoteLink_c::spawnRemoteBombExplosion() {
    g_env_light.settingTevStruct(0, &mRemoteBombLastPos, &tevStr);

    static const u16 normalNameID[] = {0x161, 0x162, 0x163, 0x164, 0x165,
                                       0x166, 0x167, 0x168, 0x1EC};
    static const u16 waterNameID[] = {0xA05, 0xA06, 0xA07, 0xA08, 0xA09, 0xA0A, 0xA0B, 0xA0C};

    const u16* effects = mRemoteBombWasWater ? waterNameID : normalNameID;
    const int count = mRemoteBombWasWater ? ARRAY_SIZE(waterNameID) : ARRAY_SIZE(normalNameID);
    csXyz angle;
    angle.set(0, shape_angle.y, 0);
    cXyz scale(1.0f, 1.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        dComIfGp_particle_setColor(effects[i], &mRemoteBombLastPos, &tevStr, NULL, NULL, 0.0f,
                                   0xFF, &angle, &scale, NULL, -1, NULL);
    }
}

void daRemoteLink_c::stopRemoteBombActor(bool i_explode) {
    const fpc_ProcID oldBombId = mRemoteBombActorId;
    const int bombKind = mRemoteBombActorKind;

    fopAc_ac_c* actor = oldBombId != fpcM_ERROR_PROCESS_ID_e ? fopAcM_SearchByID(oldBombId) : NULL;
    if (actor != NULL && fopAcM_GetName(actor) == fpcNm_NBOMB_e) {
        fopAcM_delete(actor);
    }

    if (i_explode && !mRemoteBombExplosionSpawned && remoteBombGameplayEffectsEnabled()) {
        csXyz angle;
        angle.set(0, mRemoteBombAngleY, 0);
        const int roomNo = fopAcM_GetRoomNo(this);
        fpc_ProcID explosionId =
            fopAcM_create(fpcNm_NBOMB_e, remoteBombExplosionParam(bombKind), &mRemoteBombLastPos,
                          roomNo, &angle, NULL, -1);
        dusk::multiplayer::register_remote_bomb_actor_id(static_cast<int32_t>(explosionId));
        fopAc_ac_c* explosion = fopAcM_SearchByID(explosionId);
        if (explosion != NULL && fopAcM_GetName(explosion) == fpcNm_NBOMB_e) {
            fopAcM_SetRoomNo(explosion, roomNo);
            static_cast<daNbomb_c*>(explosion)->mCcStts.SetRoomId(roomNo);
        }
        mRemoteBombExplosionSpawned = true;
        DuskLog.info("RemoteLink: local bomb explosion create id={} old_id={} kind={} actor={} pos=({}, {}, {})",
                     explosionId, oldBombId, bombKind, (void*)explosion, mRemoteBombLastPos.x,
                     mRemoteBombLastPos.y, mRemoteBombLastPos.z);
    }

    mRemoteBombActorId = fpcM_ERROR_PROCESS_ID_e;
    mRemoteBombActorKind = 0;
}

void daRemoteLink_c::updateRemoteBombActor() {
    if (!remoteBombRealActorEnabled() || !isRemoteBombActorKind(mRemoteItemActorKind) ||
        !mRemoteBombLastPosValid)
    {
        stopRemoteBombActor(false);
        return;
    }

    fopAc_ac_c* actor = mRemoteBombActorId != fpcM_ERROR_PROCESS_ID_e
                            ? fopAcM_SearchByID(mRemoteBombActorId)
                            : NULL;
    if (actor != NULL && fopAcM_GetName(actor) != fpcNm_NBOMB_e) {
        actor = NULL;
        mRemoteBombActorId = fpcM_ERROR_PROCESS_ID_e;
        mRemoteBombActorKind = 0;
    }

    if (actor != NULL && mRemoteBombActorKind != mRemoteItemActorKind) {
        stopRemoteBombActor(false);
        actor = NULL;
    }

    if (mRemoteBombExTime <= 0) {
        stopRemoteBombActor(true);
        return;
    }

    if (actor == NULL) {
        csXyz angle;
        angle.set(0, mRemoteBombAngleY, 0);
        mRemoteBombActorId =
            fopAcM_create(fpcNm_NBOMB_e, remoteBombCreateParam(mRemoteItemActorKind),
                          &mRemoteBombLastPos, fopAcM_GetRoomNo(this), &angle, NULL, -1);
        dusk::multiplayer::register_remote_bomb_actor_id(static_cast<int32_t>(mRemoteBombActorId));
        mRemoteBombActorKind = mRemoteItemActorKind;
        actor = fopAcM_SearchByID(mRemoteBombActorId);
        DuskLog.info("RemoteLink: real bomb spawn id={} kind={} actor={} ex_time={}",
                     mRemoteBombActorId, mRemoteBombActorKind, (void*)actor, mRemoteBombExTime);
    }

    if (actor == NULL || fopAcM_GetName(actor) != fpcNm_NBOMB_e) {
        return;
    }

    daNbomb_c* bomb = static_cast<daNbomb_c*>(actor);
    bomb->old.pos = bomb->current.pos;
    bomb->current.pos = mRemoteBombLastPos;
    bomb->shape_angle.y = mRemoteBombAngleY;
    bomb->current.angle.y = mRemoteBombAngleY;
    fopAcM_SetRoomNo(bomb, fopAcM_GetRoomNo(this));
    bomb->mCcStts.SetRoomId(fopAcM_GetRoomNo(this));
    if (mRemoteBombExTime >= 0) {
        bomb->mExTime = std::max(mRemoteBombExTime, 2);
    }
    bomb->speed = cXyz::Zero;
    bomb->speedF = 0.0f;

    if (bomb->mpModel != NULL) {
        mDoMtx_stack_c::transS(bomb->current.pos);
        mDoMtx_stack_c::ZXYrotM(0, bomb->shape_angle.y, bomb->shape_angle.z);
        bomb->mpModel->setBaseTRMtx(mDoMtx_stack_c::get());
        fopAcM_SetMtx(bomb, bomb->mpModel->getBaseTRMtx());
    }
}

bool daRemoteLink_c::copyRemoteModelMatrices(
    J3DModel* i_model, const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source) {
    if (i_model == NULL || i_model->getModelData() == NULL || !i_source.valid) {
        return false;
    }

    J3DModelData* data = i_model->getModelData();
    if (i_source.jointCount != data->getJointNum() ||
        i_source.weightCount != data->getWEvlpMtxNum() ||
        i_source.joints.size() != static_cast<size_t>(i_source.jointCount) * 12 ||
        !remoteModelSnapshotHasUsableWeights(i_source))
    {
        return false;
    }

    Mtx mtx;
    flatMatrixToMtx(i_source.base.data(), mtx);
    i_model->setBaseTRMtx(mtx);

    for (u16 i = 0; i < i_source.jointCount; ++i) {
        flatMatrixToMtx(&i_source.joints[static_cast<size_t>(i) * 12], mtx);
        i_model->setAnmMtx(i, mtx);
    }

    if (!i_source.weightsOmitted) {
        for (u16 i = 0; i < i_source.weightCount; ++i) {
            flatMatrixToMtx(&i_source.weights[static_cast<size_t>(i) * 12], mtx);
            mDoMtx_copy(mtx, i_model->getWeightAnmMtx(i));
        }
    }

    return true;
}

void daRemoteLink_c::clearRemoteModelMatrixInterpolation(
    RemoteModelMatrixInterpState& io_state) {
    io_state = {};
}

void daRemoteLink_c::captureRemoteModelMatrixSnapshot(
    J3DModel* i_model, const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source,
    RemoteModelMatrixInterpState& io_state) {
    if (!remoteMatrixLocalInterpolationEnabled()) {
        return;
    }

    if (i_model == NULL || i_model->getModelData() == NULL || !i_source.valid) {
        clearRemoteModelMatrixInterpolation(io_state);
        return;
    }

    J3DModelData* data = i_model->getModelData();
    dusk::multiplayer::RemoteModelMatrixSnapshot source = i_source;
    if (source.weightsOmitted) {
        if (io_state.currValid && io_state.curr.weightCount == source.weightCount &&
            io_state.curr.weights.size() == static_cast<size_t>(source.weightCount) * 12)
        {
            source.weights = io_state.curr.weights;
            source.weightsOmitted = false;
        }
    }

    if (source.jointCount != data->getJointNum() ||
        source.weightCount != data->getWEvlpMtxNum() ||
        source.joints.size() != static_cast<size_t>(source.jointCount) * 12 ||
        source.weights.size() != static_cast<size_t>(source.weightCount) * 12)
    {
        static int sAttachmentCaptureRejectLogCount = 0;
        if (sAttachmentCaptureRejectLogCount < 20) {
            DuskLog.info("RemoteLink: local matrix interp attachment capture skipped "
                         "model_joints={} matrix_joints={} model_weights={} matrix_weights={} "
                         "weight_values={} weights_omitted={} curr_valid={}",
                         data->getJointNum(), source.jointCount, data->getWEvlpMtxNum(),
                         source.weightCount, source.weights.size(), i_source.weightsOmitted,
                         io_state.currValid);
            ++sAttachmentCaptureRejectLogCount;
        }
        clearRemoteModelMatrixInterpolation(io_state);
        return;
    }

    if (io_state.currValid && remoteModelMatrixSnapshotNearlyEqual(io_state.curr, source)) {
        io_state.prev = io_state.curr;
        io_state.prevValid = true;
        return;
    }

    if (io_state.currValid) {
        io_state.prev = io_state.curr;
        io_state.prevValid = true;
    } else {
        io_state.prevValid = false;
    }
    io_state.curr = source;
    io_state.currValid = true;
}

void daRemoteLink_c::overrideRemoteModelMatrices(J3DModel* i_model) {
    if (i_model == NULL || i_model->getModelData() == NULL) {
        return;
    }

    J3DModelData* data = i_model->getModelData();
    for (u16 i = 0; i < data->getJointNum(); ++i) {
        dusk::frame_interp::override_replacement(i_model->getAnmMtx(i), i_model->getAnmMtx(i));
    }
    for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i) {
        dusk::frame_interp::override_replacement(i_model->getWeightAnmMtx(i),
                                                 i_model->getWeightAnmMtx(i));
    }
}

bool daRemoteLink_c::applyInterpolatedRemoteModelMatrices(
    J3DModel* i_model, RemoteModelMatrixInterpState& io_state, const char* i_label) {
    if (!remoteMatrixLocalInterpolationEnabled() || !io_state.prevValid || !io_state.currValid ||
        i_model == NULL || i_model->getModelData() == NULL ||
        (dusk::frame_interp::is_enabled() && dusk::frame_interp::is_sim_frame()))
    {
        return false;
    }

    J3DModelData* data = i_model->getModelData();
    const u16 jointCount = data->getJointNum();
    if (jointCount == 0 || io_state.prev.jointCount != jointCount ||
        io_state.curr.jointCount != jointCount ||
        io_state.prev.weightCount != data->getWEvlpMtxNum() ||
        io_state.curr.weightCount != data->getWEvlpMtxNum())
    {
        return false;
    }

    Mtx prevBase;
    Mtx currBase;
    if (!snapshotMatrixToMtx(io_state.prev.base, prevBase) ||
        !snapshotMatrixToMtx(io_state.curr.base, currBase))
    {
        return false;
    }

    static f32 const kMaxInterpolatedRootMoveSq = 400.0f * 400.0f;
    if (matrixTranslationDistanceSq(prevBase, currBase) > kMaxInterpolatedRootMoveSq) {
        return false;
    }

    J3DJoint* rootJoint = data->getJointTree().getRootNode();
    if (rootJoint == NULL) {
        return false;
    }

    std::vector<int16_t> parents(jointCount, -1);
    fillJointParentMap(rootJoint, -1, parents);

    std::vector<RemoteInterpMtx> prevWorld(jointCount);
    std::vector<RemoteInterpMtx> currWorld(jointCount);
    for (u16 i = 0; i < jointCount; ++i) {
        if (parents[i] == -1 && data->getJointNodePointer(i) != rootJoint) {
            return false;
        }
        if (!snapshotJointMatrixToMtx(io_state.prev, i, prevWorld[i].value) ||
            !snapshotJointMatrixToMtx(io_state.curr, i, currWorld[i].value))
        {
            return false;
        }
    }

    const f32 alpha = getRemoteMatrixInterpolationStep();
    static int sAttachmentInterpLogCount = 0;
    if (sAttachmentInterpLogCount < 16) {
        DuskLog.info("RemoteLink: local matrix interp attachment={} alpha={} joints={} weights={}",
                     i_label, alpha, jointCount, data->getWEvlpMtxNum());
        ++sAttachmentInterpLogCount;
    }

    RemoteLocalJointTransform prevBaseTransform;
    RemoteLocalJointTransform currBaseTransform;
    if (!decomposeMatrixTRS(prevBase, &prevBaseTransform) ||
        !decomposeMatrixTRS(currBase, &currBaseTransform))
    {
        return false;
    }

    Mtx worldOut;
    composeMatrixTRS(interpolateLocalTransform(prevBaseTransform, currBaseTransform, alpha),
                     worldOut);
    if (mHasRemotePose && mPrevBodyMatrixSnapshotValid && mCurrBodyMatrixSnapshotValid) {
        Mtx prevBodyBase;
        Mtx currBodyBase;
        RemoteLocalJointTransform prevBodyBaseTransform;
        RemoteLocalJointTransform currBodyBaseTransform;
        if (snapshotMatrixToMtx(mPrevBodyMatrixSnapshot.base, prevBodyBase) &&
            snapshotMatrixToMtx(mCurrBodyMatrixSnapshot.base, currBodyBase) &&
            matrixTranslationDistanceSq(prevBodyBase, currBodyBase) <=
                kMaxInterpolatedRootMoveSq &&
            decomposeMatrixTRS(prevBodyBase, &prevBodyBaseTransform) &&
            decomposeMatrixTRS(currBodyBase, &currBodyBaseTransform))
        {
            Mtx rawBodyBaseOut;
            composeMatrixTRS(
                interpolateLocalTransform(prevBodyBaseTransform, currBodyBaseTransform, alpha),
                rawBodyBaseOut);

            const f32 invAlpha = 1.0f - alpha;
            cXyz anchoredPos;
            anchoredPos.set(old.pos.x * invAlpha + current.pos.x * alpha,
                            old.pos.y * invAlpha + current.pos.y * alpha,
                            old.pos.z * invAlpha + current.pos.z * alpha);
            if ((current.pos - old.pos).abs2() <= kMaxInterpolatedRootMoveSq) {
                worldOut[0][3] += anchoredPos.x - rawBodyBaseOut[0][3];
                worldOut[2][3] += anchoredPos.z - rawBodyBaseOut[2][3];
            } else {
                worldOut[0][3] += current.pos.x - rawBodyBaseOut[0][3];
                worldOut[1][3] += current.pos.y - rawBodyBaseOut[1][3];
                worldOut[2][3] += current.pos.z - rawBodyBaseOut[2][3];
            }

            static int sAttachmentRootCorrectionLogCount = 0;
            if (sAttachmentRootCorrectionLogCount < 12 && i_label != NULL &&
                std::strcmp(i_label, "face") == 0)
            {
                DuskLog.info("RemoteLink: local matrix interp attachment root correction={} "
                             "alpha={} dx={} dz={}",
                             i_label, alpha, anchoredPos.x - rawBodyBaseOut[0][3],
                             anchoredPos.z - rawBodyBaseOut[2][3]);
                ++sAttachmentRootCorrectionLogCount;
            }
        }
    }
    i_model->setBaseTRMtx(worldOut);

    Mtx invParentPrev;
    Mtx invParentCurr;
    Mtx localPrev;
    Mtx localCurr;
    Mtx localOut;
    RemoteLocalJointTransform prevTransform;
    RemoteLocalJointTransform currTransform;
    std::vector<RemoteInterpMtx> rebuiltWorld(jointCount);

    for (u16 i = 0; i < jointCount; ++i) {
        const int parent = parents[i];
        if (parent >= 0) {
            if (!MTXInverse(prevWorld[parent].value, invParentPrev) ||
                !MTXInverse(currWorld[parent].value, invParentCurr))
            {
                return false;
            }
        } else if (!MTXInverse(prevBase, invParentPrev) || !MTXInverse(currBase, invParentCurr)) {
            return false;
        }

        MTXConcat(invParentPrev, prevWorld[i].value, localPrev);
        MTXConcat(invParentCurr, currWorld[i].value, localCurr);
        if (!decomposeMatrixTRS(localPrev, &prevTransform) ||
            !decomposeMatrixTRS(localCurr, &currTransform))
        {
            return false;
        }

        composeMatrixTRS(interpolateLocalTransform(prevTransform, currTransform, alpha), localOut);
        if (parent >= 0) {
            MTXConcat(rebuiltWorld[parent].value, localOut, worldOut);
        } else {
            MTXConcat(i_model->getBaseTRMtx(), localOut, worldOut);
        }
        MTXCopy(worldOut, rebuiltWorld[i].value);
        i_model->setAnmMtx(i, worldOut);
    }

    i_model->calcWeightEnvelopeMtx();
    overrideRemoteModelMatrices(i_model);
    return true;
}

void daRemoteLink_c::applyInterpolatedRemoteAttachments() {
    if (mpHeadModel != NULL && mpBodyModel != NULL) {
        mpHeadModel->setBaseTRMtx(mpBodyModel->getAnmMtx(4));
        mpHeadModel->calc();
        overrideRemoteModelMatrices(mpHeadModel);
    }

    applyInterpolatedRemoteModelMatrices(mpFaceModel, mFaceMatrixInterp, "face");
    applyInterpolatedRemoteModelMatrices(mpHandModel, mHandMatrixInterp, "hand");
    if (mVisualState.form == FORM_WOLF) {
        // Wolf equipment is attached to the back joint by a fixed local
        // offset. Rebuild it after the wolf body has been interpolated so the
        // sword, sheath, and shield follow the render-frame body pose instead
        // of remaining at their last simulation-frame matrices.
        applyWolfEquipmentMatrices();
    } else {
        applyInterpolatedRemoteModelMatrices(mpSwordModel, mSwordMatrixInterp, "sword");
        applyInterpolatedRemoteModelMatrices(mpSheathModel, mSheathMatrixInterp, "sheath");
        applyInterpolatedRemoteModelMatrices(mpShieldModel, mShieldMatrixInterp, "shield");
    }

    if (mRemoteMidnaShadowForm) {
        applyInterpolatedRemoteModelMatrices(mpShadowMidnaModel, mMidnaMatrixInterp, "midna");
        applyInterpolatedRemoteModelMatrices(mpShadowMidnaMaskModel, mMidnaMaskMatrixInterp,
                                             "midna_mask");
        applyInterpolatedRemoteModelMatrices(mpShadowMidnaHandModel, mMidnaHandMatrixInterp,
                                             "midna_hand");
        applyInterpolatedRemoteModelMatrices(mpShadowMidnaHairModel, mMidnaHairMatrixInterp,
                                             "midna_hair");
    } else {
        applyInterpolatedRemoteModelMatrices(mpMidnaModel, mMidnaMatrixInterp, "midna");
        applyInterpolatedRemoteModelMatrices(mpMidnaMaskModel, mMidnaMaskMatrixInterp,
                                             "midna_mask");
        applyInterpolatedRemoteModelMatrices(mpMidnaHandModel, mMidnaHandMatrixInterp,
                                             "midna_hand");
        applyInterpolatedRemoteModelMatrices(mpMidnaHairModel, mMidnaHairMatrixInterp,
                                             "midna_hair");
    }
    applyInterpolatedRemoteModelMatrices(mpMidnaGlowModel, mMidnaGlowMatrixInterp, "midna_glow");
}

void daRemoteLink_c::captureRemoteBodyMatrixSnapshot(
    const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source) {
    if (!remoteMatrixLocalInterpolationEnabled()) {
        return;
    }

    static int sCaptureRejectLogCount = 0;
    const auto logCaptureReject = [&](const char* reason) {
        if (sCaptureRejectLogCount < 16) {
            DuskLog.info("RemoteLink: local matrix interp capture skipped reason={} valid={} "
                         "body={} data={}",
                         reason, i_source.valid, (void*)mpBodyModel,
                         mpBodyModel != NULL ? (void*)mpBodyModel->getModelData() : NULL);
            ++sCaptureRejectLogCount;
        }
    };

    if (!i_source.valid || mpBodyModel == NULL || mpBodyModel->getModelData() == NULL) {
        logCaptureReject(!i_source.valid ? "source_invalid" : "model_missing");
        clearRemoteBodyMatrixInterpolation();
        return;
    }

    J3DModelData* data = mpBodyModel->getModelData();
    if (i_source.jointCount != data->getJointNum() ||
        i_source.weightCount != data->getWEvlpMtxNum() ||
        i_source.joints.size() != static_cast<size_t>(i_source.jointCount) * 12)
    {
        logCaptureReject("shape_mismatch");
        clearRemoteBodyMatrixInterpolation();
        return;
    }

    if (mCurrBodyMatrixSnapshotValid &&
        remoteModelMatrixSnapshotNearlyEqual(mCurrBodyMatrixSnapshot, i_source))
    {
        mPrevBodyMatrixSnapshot = mCurrBodyMatrixSnapshot;
        mPrevBodyMatrixSnapshotValid = true;
        mRemoteMatrixBlendDurationTicks = std::max<u32>(mRemoteMatrixTicksSinceCapture, 1);
        mRemoteMatrixTicksSinceCapture = 0;
        static int sDuplicateCaptureLogCount = 0;
        if (sDuplicateCaptureLogCount < 12) {
            DuskLog.info("RemoteLink: local matrix interp duplicate body snapshot collapsed");
            ++sDuplicateCaptureLogCount;
        }
        return;
    }

    if (mCurrBodyMatrixSnapshotValid) {
        mPrevBodyMatrixSnapshot = mCurrBodyMatrixSnapshot;
        mPrevBodyMatrixSnapshotValid = true;
    } else {
        mPrevBodyMatrixSnapshotValid = false;
    }
    mCurrBodyMatrixSnapshot = i_source;
    mCurrBodyMatrixSnapshotValid = true;
    mRemoteMatrixBlendDurationTicks = std::max<u32>(mRemoteMatrixTicksSinceCapture, 1);
    mRemoteMatrixTicksSinceCapture = 0;

    static int sCaptureLogCount = 0;
    if (sCaptureLogCount < 12) {
        DuskLog.info("RemoteLink: local matrix interp capture prev={} curr={} joints={} weights={}",
                     mPrevBodyMatrixSnapshotValid, mCurrBodyMatrixSnapshotValid,
                     i_source.jointCount, i_source.weightCount);
        ++sCaptureLogCount;
    }
}

void daRemoteLink_c::clearRemoteBodyMatrixInterpolation() {
    mPrevBodyMatrixSnapshot = {};
    mCurrBodyMatrixSnapshot = {};
    mPrevBodyMatrixSnapshotValid = false;
    mCurrBodyMatrixSnapshotValid = false;
    mRemoteMatrixTicksSinceCapture = 0;
    mRemoteMatrixBlendDurationTicks = 1;
    clearRemoteModelMatrixInterpolation(mFaceMatrixInterp);
    clearRemoteModelMatrixInterpolation(mHandMatrixInterp);
    clearRemoteModelMatrixInterpolation(mSwordMatrixInterp);
    clearRemoteModelMatrixInterpolation(mSheathMatrixInterp);
    clearRemoteModelMatrixInterpolation(mShieldMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaMaskMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaHandMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaHairMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaGlowMatrixInterp);
}

void daRemoteLink_c::applyRemoteBodyMatrixInterpolationForPresentation() {
    applyInterpolatedRemoteBodyMatrices();
    applyInterpolatedRemoteAttachments();
}

f32 daRemoteLink_c::getRemoteMatrixInterpolationStep() const {
    if (dusk::frame_interp::is_enabled()) {
        return std::clamp(dusk::frame_interp::get_interpolation_step(), 0.0f, 1.0f);
    }

    return remoteMatrixFallbackInterpolationStep(mRemoteMatrixTicksSinceCapture,
                                                 mRemoteMatrixBlendDurationTicks);
}

bool daRemoteLink_c::getNameLabelPosition(cXyz* o_pos) const {
    if (o_pos == NULL || !mHasRemotePose) {
        return false;
    }

    if (mpBodyModel != NULL && mpBodyModel->getModelData() != NULL &&
        mpBodyModel->getModelData()->getJointNum() > 4)
    {
        MtxP neckMtx = mpBodyModel->getAnmMtx(4);
        Mtx interpNeckMtx;
        if (dusk::frame_interp::lookup_replacement(neckMtx, interpNeckMtx)) {
            neckMtx = interpNeckMtx;
        }
        o_pos->set(neckMtx[0][3], neckMtx[1][3], neckMtx[2][3]);
        o_pos->y += mVisualState.form == FORM_WOLF ? 22.0f : 30.0f;
        return true;
    }

    *o_pos = current.pos;
    o_pos->y += mVisualState.form == FORM_WOLF ? 46.0f : 82.0f;
    return true;
}

void daRemoteLink_c::applyInterpolatedRemoteBodyMatrices() {
    if (!remoteMatrixLocalInterpolationEnabled()) {
        return;
    }

    static int sApplyRejectLogCount = 0;
    const auto logApplyReject = [&](const char* reason) {
        if (sApplyRejectLogCount < 24) {
            DuskLog.info("RemoteLink: local matrix interp apply skipped reason={} prev={} curr={} "
                         "body={} data={} fi_enabled={} sim_frame={}",
                         reason, mPrevBodyMatrixSnapshotValid, mCurrBodyMatrixSnapshotValid,
                         (void*)mpBodyModel,
                         mpBodyModel != NULL ? (void*)mpBodyModel->getModelData() : NULL,
                         dusk::frame_interp::is_enabled(), dusk::frame_interp::is_sim_frame());
            ++sApplyRejectLogCount;
        }
    };

    if (!mPrevBodyMatrixSnapshotValid || !mCurrBodyMatrixSnapshotValid) {
        logApplyReject("snapshot_missing");
        return;
    }

    if (mpBodyModel == NULL || mpBodyModel->getModelData() == NULL) {
        logApplyReject("model_missing");
        return;
    }

    if (dusk::frame_interp::is_enabled() && dusk::frame_interp::is_sim_frame()) {
        logApplyReject("sim_frame");
        return;
    }

    J3DModelData* data = mpBodyModel->getModelData();
    const u16 jointCount = data->getJointNum();
    if (jointCount == 0 || mPrevBodyMatrixSnapshot.jointCount != jointCount ||
        mCurrBodyMatrixSnapshot.jointCount != jointCount ||
        mPrevBodyMatrixSnapshot.weightCount != data->getWEvlpMtxNum() ||
        mCurrBodyMatrixSnapshot.weightCount != data->getWEvlpMtxNum())
    {
        logApplyReject("shape_mismatch");
        return;
    }

    Mtx prevBase;
    Mtx currBase;
    if (!snapshotMatrixToMtx(mPrevBodyMatrixSnapshot.base, prevBase) ||
        !snapshotMatrixToMtx(mCurrBodyMatrixSnapshot.base, currBase))
    {
        logApplyReject("base_invalid");
        return;
    }

    static f32 const kMaxInterpolatedRootMoveSq = 400.0f * 400.0f;
    if (matrixTranslationDistanceSq(prevBase, currBase) > kMaxInterpolatedRootMoveSq) {
        logApplyReject("root_teleport");
        return;
    }

    J3DJoint* rootJoint = data->getJointTree().getRootNode();
    if (rootJoint == NULL) {
        logApplyReject("root_missing");
        return;
    }

    std::vector<int16_t> parents(jointCount, -1);
    fillJointParentMap(rootJoint, -1, parents);

    std::vector<RemoteInterpMtx> prevWorld(jointCount);
    std::vector<RemoteInterpMtx> currWorld(jointCount);
    for (u16 i = 0; i < jointCount; ++i) {
        if (parents[i] == -1 && data->getJointNodePointer(i) != rootJoint) {
            logApplyReject("parent_map_incomplete");
            return;
        }
        if (!snapshotJointMatrixToMtx(mPrevBodyMatrixSnapshot, i, prevWorld[i].value) ||
            !snapshotJointMatrixToMtx(mCurrBodyMatrixSnapshot, i, currWorld[i].value))
        {
            logApplyReject("joint_invalid");
            return;
        }
    }

    f32 alpha = getRemoteMatrixInterpolationStep();
    static int sLocalInterpLogCount = 0;
    if (sLocalInterpLogCount < 12) {
        DuskLog.info("RemoteLink: local matrix interp apply alpha={} joints={} weights={}",
                     alpha, jointCount, data->getWEvlpMtxNum());
        sLocalInterpLogCount++;
    }
    Mtx invParentPrev;
    Mtx invParentCurr;
    Mtx localPrev;
    Mtx localCurr;
    Mtx localOut;
    Mtx worldOut;

    RemoteLocalJointTransform prevTransform;
    RemoteLocalJointTransform currTransform;
    std::vector<RemoteInterpMtx> rebuiltWorld(jointCount);

    RemoteLocalJointTransform prevBaseTransform;
    RemoteLocalJointTransform currBaseTransform;
    if (!decomposeMatrixTRS(prevBase, &prevBaseTransform) ||
        !decomposeMatrixTRS(currBase, &currBaseTransform))
    {
        logApplyReject("base_decompose_failed");
        return;
    }
    composeMatrixTRS(interpolateLocalTransform(prevBaseTransform, currBaseTransform, alpha),
                     worldOut);
    if (mHasRemotePose) {
        const f32 invAlpha = 1.0f - alpha;
        cXyz anchoredPos;
        anchoredPos.set(old.pos.x * invAlpha + current.pos.x * alpha,
                        old.pos.y * invAlpha + current.pos.y * alpha,
                        old.pos.z * invAlpha + current.pos.z * alpha);
        if ((current.pos - old.pos).abs2() <= 400.0f * 400.0f) {
            setMatrixHorizontalTranslation(worldOut, anchoredPos);
        } else {
            setMatrixTranslation(worldOut, current.pos);
        }
    }
    mpBodyModel->setBaseTRMtx(worldOut);

    for (u16 i = 0; i < jointCount; ++i) {
        const int parent = parents[i];
        if (parent >= 0) {
            if (!MTXInverse(prevWorld[parent].value, invParentPrev) ||
                !MTXInverse(currWorld[parent].value, invParentCurr))
            {
                logApplyReject("parent_inverse_failed");
                return;
            }
        } else {
            if (!MTXInverse(prevBase, invParentPrev) || !MTXInverse(currBase, invParentCurr)) {
                logApplyReject("base_inverse_failed");
                return;
            }
        }

        MTXConcat(invParentPrev, prevWorld[i].value, localPrev);
        MTXConcat(invParentCurr, currWorld[i].value, localCurr);
        if (!decomposeMatrixTRS(localPrev, &prevTransform) ||
            !decomposeMatrixTRS(localCurr, &currTransform))
        {
            logApplyReject("joint_decompose_failed");
            return;
        }

        composeMatrixTRS(interpolateLocalTransform(prevTransform, currTransform, alpha), localOut);
        if (parent >= 0) {
            MTXConcat(rebuiltWorld[parent].value, localOut, worldOut);
        } else {
            MTXConcat(mpBodyModel->getBaseTRMtx(), localOut, worldOut);
        }
        MTXCopy(worldOut, rebuiltWorld[i].value);
        mpBodyModel->setAnmMtx(i, worldOut);
    }

    mpBodyModel->calcWeightEnvelopeMtx();
    for (u16 i = 0; i < jointCount; ++i) {
        dusk::frame_interp::override_replacement(mpBodyModel->getAnmMtx(i),
                                                 mpBodyModel->getAnmMtx(i));
    }
    for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i) {
        dusk::frame_interp::override_replacement(mpBodyModel->getWeightAnmMtx(i),
                                                 mpBodyModel->getWeightAnmMtx(i));
    }
    fopAcM_SetMtx(this, mpBodyModel->getBaseTRMtx());
    model = mpBodyModel;
}

void daRemoteLink_c::setRemoteMatrices(
    const dusk::multiplayer::RemoteLinkMatrixSnapshot& i_matrices) {
    if (!i_matrices.valid) {
        mHasRemoteMatrices = false;
        clearRemoteBodyMatrixInterpolation();
        mHeldItemMatrixValid = false;
        mHookTipMatrixValid = false;
        mHookSubItemMatrixValid = false;
        mHookSubTipMatrixValid = false;
        mArrowMatrixValid = false;
        mKanteraMatrixValid = false;
        mKanteraGlowMatrixValid = false;
        mItemActorMatrixValid = false;
        mRideActorMatrixValid = false;
        mMidnaMatrixValid = false;
        mMidnaMaskMatrixValid = false;
        mMidnaHandMatrixValid = false;
        mMidnaHairMatrixValid = false;
        mMidnaGlowMatrixValid = false;
        mMidnaHairShape = 0;
        return;
    }

    const bool bodyCopied = copyRemoteModelMatrices(mpBodyModel, i_matrices.body);
    if (!bodyCopied) {
        if (sCalcLogCount < 8) {
            DuskLog.warn("RemoteLink: remote matrix copy rejected body modelJoints={} "
                         "matrixJoints={} modelWeights={} matrixWeights={}",
                         mpBodyModel != NULL && mpBodyModel->getModelData() != NULL
                             ? mpBodyModel->getModelData()->getJointNum()
                             : 0,
                         i_matrices.body.jointCount,
                         mpBodyModel != NULL && mpBodyModel->getModelData() != NULL
                             ? mpBodyModel->getModelData()->getWEvlpMtxNum()
                             : 0,
                         i_matrices.body.weightCount);
            sCalcLogCount++;
        }
        mHasRemoteMatrices = false;
        clearRemoteBodyMatrixInterpolation();
        mHeldItemMatrixValid = false;
        mHookTipMatrixValid = false;
        mHookSubItemMatrixValid = false;
        mHookSubTipMatrixValid = false;
        mArrowMatrixValid = false;
        mKanteraMatrixValid = false;
        mKanteraGlowMatrixValid = false;
        mItemActorMatrixValid = false;
        mRideActorMatrixValid = false;
        mMidnaMatrixValid = false;
        mMidnaMaskMatrixValid = false;
        mMidnaHandMatrixValid = false;
        mMidnaHairMatrixValid = false;
        mMidnaGlowMatrixValid = false;
        mMidnaHairShape = 0;
        return;
    }
    captureRemoteBodyMatrixSnapshot(i_matrices.body);

    applyHeavyBootMatrices();
    if (mVisualState.form == FORM_WOLF) {
        applyWolfEquipmentMatrices();
    }
    if (mpHeadModel != NULL) {
        mpHeadModel->setBaseTRMtx(mpBodyModel->getAnmMtx(4));
        mpHeadModel->calc();
    }
    if (copyRemoteModelMatrices(mpFaceModel, i_matrices.face)) {
        captureRemoteModelMatrixSnapshot(mpFaceModel, i_matrices.face, mFaceMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mFaceMatrixInterp);
    }
    if (copyRemoteModelMatrices(mpHandModel, i_matrices.hand)) {
        captureRemoteModelMatrixSnapshot(mpHandModel, i_matrices.hand, mHandMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mHandMatrixInterp);
    }
    if (mVisualState.form != FORM_WOLF) {
        if (copyRemoteModelMatrices(mpSwordModel, i_matrices.sword)) {
            captureRemoteModelMatrixSnapshot(mpSwordModel, i_matrices.sword, mSwordMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mSwordMatrixInterp);
        }
        if (copyRemoteModelMatrices(mpSheathModel, i_matrices.sheath)) {
            captureRemoteModelMatrixSnapshot(mpSheathModel, i_matrices.sheath, mSheathMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mSheathMatrixInterp);
        }
        if (copyRemoteModelMatrices(mpShieldModel, i_matrices.shield)) {
            captureRemoteModelMatrixSnapshot(mpShieldModel, i_matrices.shield, mShieldMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mShieldMatrixInterp);
        }
    } else {
        clearRemoteModelMatrixInterpolation(mSwordMatrixInterp);
        clearRemoteModelMatrixInterpolation(mSheathMatrixInterp);
        clearRemoteModelMatrixInterpolation(mShieldMatrixInterp);
    }
    mHeldItemMatrixValid =
        mRemoteItemDraw && copyRemoteModelMatrices(mpHeldItemModel, i_matrices.heldItem);
    mHookTipMatrixValid =
        mRemoteItemDraw && copyRemoteModelMatrices(mpHookTipModel, i_matrices.hookTip);
    mHookSubItemMatrixValid =
        mRemoteItemDraw && copyRemoteModelMatrices(mpHookSubItemModel, i_matrices.hookSubItem);
    mHookSubTipMatrixValid =
        mRemoteItemDraw && copyRemoteModelMatrices(mpHookSubTipModel, i_matrices.hookSubTip);
    mArrowMatrixValid =
        mRemoteItemDraw && copyRemoteModelMatrices(mpArrowModel, i_matrices.arrow);
    mKanteraMatrixValid =
        mRemoteKanteraDraw && copyRemoteModelMatrices(mpKanteraModel, i_matrices.kantera);
    mKanteraGlowMatrixValid =
        mRemoteKanteraDraw && copyRemoteModelMatrices(mpKanteraGlowModel,
                                                      i_matrices.kanteraGlow);
    mItemActorMatrixValid = copyRemoteModelMatrices(mpItemActorModel,
                                                    i_matrices.itemActor);
    if (mItemActorMatrixValid && isRemoteBombActorKind(mRemoteItemActorKind)) {
        mRemoteBombLastPos.set(i_matrices.itemActor.base[3], i_matrices.itemActor.base[7],
                               i_matrices.itemActor.base[11]);
        mRemoteBombLastPosValid = true;
        mRemoteBombWasWater = mRemoteItemActorKind == 3;
        if (!remoteBombRealActorEnabled() && mRemoteBombExTime <= 0 &&
            !mRemoteBombExplosionSpawned)
        {
            spawnRemoteBombExplosion();
            mRemoteBombExplosionSpawned = true;
        }
        if (mRemoteBombExTime <= 0) {
            stopRemoteBombActor(true);
            mItemActorMatrixValid = false;
        }
    }
    mRideActorMatrixValid = copyRemoteModelMatrices(mpRideActorModel,
                                                    i_matrices.rideActor);
    if (mRemoteMidnaShadowForm) {
        if (mpShadowMidnaModel == NULL) {
            JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
            setupShadowMidnaModels();
            mDoExt_setCurrentHeap(previousHeap);
        }
        mMidnaMatrixValid =
            mRemoteMidnaDraw && copyRemoteModelMatrices(mpShadowMidnaModel, i_matrices.midna);
        if (mMidnaMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpShadowMidnaModel, i_matrices.midna,
                                             mMidnaMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaMatrixInterp);
        }
        mMidnaMaskMatrixValid = mRemoteMidnaMaskDraw &&
                                copyRemoteModelMatrices(mpShadowMidnaMaskModel,
                                                        i_matrices.midnaMask);
        if (mMidnaMaskMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpShadowMidnaMaskModel, i_matrices.midnaMask,
                                             mMidnaMaskMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaMaskMatrixInterp);
        }
        mMidnaHandMatrixValid = mRemoteMidnaHandDraw &&
                                copyRemoteModelMatrices(mpShadowMidnaHandModel,
                                                        i_matrices.midnaHand);
        if (mMidnaHandMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpShadowMidnaHandModel, i_matrices.midnaHand,
                                             mMidnaHandMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaHandMatrixInterp);
        }
        mMidnaHairMatrixValid = mRemoteMidnaHairDraw &&
                                copyRemoteModelMatrices(mpShadowMidnaHairModel,
                                                        i_matrices.midnaHair);
        if (mMidnaHairMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpShadowMidnaHairModel, i_matrices.midnaHair,
                                             mMidnaHairMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaHairMatrixInterp);
        }
        mMidnaGlowMatrixValid = copyRemoteModelMatrices(mpMidnaGlowModel,
                                                        i_matrices.midnaGlow);
        if (mMidnaGlowMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpMidnaGlowModel, i_matrices.midnaGlow,
                                             mMidnaGlowMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaGlowMatrixInterp);
        }
    } else {
        mMidnaMatrixValid = mVisualState.form == FORM_WOLF && mRemoteMidnaDraw &&
                            copyRemoteModelMatrices(mpMidnaModel, i_matrices.midna);
        if (mMidnaMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpMidnaModel, i_matrices.midna, mMidnaMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaMatrixInterp);
        }
        mMidnaMaskMatrixValid = mVisualState.form == FORM_WOLF && mRemoteMidnaMaskDraw &&
                                copyRemoteModelMatrices(mpMidnaMaskModel,
                                                        i_matrices.midnaMask);
        if (mMidnaMaskMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpMidnaMaskModel, i_matrices.midnaMask,
                                             mMidnaMaskMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaMaskMatrixInterp);
        }
        mMidnaHandMatrixValid = mVisualState.form == FORM_WOLF && mRemoteMidnaHandDraw &&
                                copyRemoteModelMatrices(mpMidnaHandModel,
                                                        i_matrices.midnaHand);
        if (mMidnaHandMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpMidnaHandModel, i_matrices.midnaHand,
                                             mMidnaHandMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaHandMatrixInterp);
        }
        mMidnaHairMatrixValid = mVisualState.form == FORM_WOLF && mRemoteMidnaHairDraw &&
                                copyRemoteModelMatrices(mpMidnaHairModel,
                                                        i_matrices.midnaHair);
        if (mMidnaHairMatrixValid) {
            captureRemoteModelMatrixSnapshot(mpMidnaHairModel, i_matrices.midnaHair,
                                             mMidnaHairMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mMidnaHairMatrixInterp);
        }
        mMidnaGlowMatrixValid = false;
        clearRemoteModelMatrixInterpolation(mMidnaGlowMatrixInterp);
    }
    mMidnaHairShape = i_matrices.midnaHairShape;
    if (!mItemActorMatrixValid && (mRemoteItemActorKind != 0 || i_matrices.itemActor.valid) &&
        sItemActorRejectLogCount < 12)
    {
        ++sItemActorRejectLogCount;
        J3DModelData* data =
            mpItemActorModel != NULL ? mpItemActorModel->getModelData() : NULL;
        DuskLog.warn("RemoteLink: linked item matrix rejected kind={} model={} modelJoints={} "
                     "matrixJoints={} modelWeights={} matrixWeights={} matrixValid={}",
                     mRemoteItemActorKind, (void*)mpItemActorModel,
                     data != NULL ? data->getJointNum() : 0,
                     i_matrices.itemActor.jointCount,
                     data != NULL ? data->getWEvlpMtxNum() : 0,
                     i_matrices.itemActor.weightCount, i_matrices.itemActor.valid);
    }
    fopAcM_SetMtx(this, mpBodyModel->getBaseTRMtx());
    model = mpBodyModel;
    mHasRemoteMatrices = true;
}

void daRemoteLink_c::setRemoteBombObjectState(
    const dusk::multiplayer::RemoteBombObjectSnapshot& i_bomb) {
    if (!i_bomb.valid || !i_bomb.active || !isRemoteBombActorKind(i_bomb.kind)) {
        if (i_bomb.valid && i_bomb.exploding && isRemoteBombActorKind(i_bomb.kind)) {
            mRemoteBombLastPos.set(i_bomb.x, i_bomb.y, i_bomb.z);
            mRemoteBombLastPosValid = true;
            mRemoteBombWasWater = i_bomb.kind == 3;
            mRemoteBombAngleY = static_cast<s16>(i_bomb.angleY);
            if (!isRemoteBombActorKind(mRemoteBombActorKind)) {
                mRemoteBombActorKind = i_bomb.kind;
            }
            stopRemoteBombActor(true);
        }
        else if (isRemoteBombActorKind(mRemoteItemActorKind)) {
            stopRemoteBombActor(i_bomb.valid && i_bomb.exploding);
        }
        mRemoteItemActorKind = 0;
        mRemoteBombExTime = -1;
        mRemoteBombFlash = -1;
        mRemoteBombLastPosValid = false;
        return;
    }

    if (mRemoteItemActorKind != i_bomb.kind) {
        mRemoteBombFlashTicks = 0;
        mRemoteBombExplosionSpawned = false;
    }

    mRemoteItemActorKind = i_bomb.kind;
    mRemoteBombExTime = i_bomb.exTime;
    mRemoteBombFlash = -1;
    mRemoteBombLastPos.set(i_bomb.x, i_bomb.y, i_bomb.z);
    mRemoteBombLastPosValid = true;
    mRemoteBombWasWater = i_bomb.kind == 3;
    mRemoteBombAngleY = static_cast<s16>(i_bomb.angleY);
}

void daRemoteLink_c::setRemoteHatState(const std::array<int16_t, 10>& i_rotA,
                                       const std::array<int16_t, 10>& i_rotB,
                                       const std::array<int16_t, 3>& i_swing,
                                       s16 i_shapeY) {
    mRemoteHatRotA = i_rotA;
    mRemoteHatRotB = i_rotB;
    mRemoteHatSwing = i_swing;
    mRemoteHatShapeY = i_shapeY;
}

void daRemoteLink_c::drawModel(J3DModel* i_model) {
    if (i_model == NULL) {
        return;
    }

    g_env_light.setLightTevColorType_MAJI(i_model, &tevStr);
    mDoExt_modelEntryDL(i_model);
}

void daRemoteLink_c::drawLinkedItemActorModel() {
    if (mpItemActorModel == NULL) {
        return;
    }

    if (remoteBombRealActorEnabled() && isRemoteBombActorKind(mRemoteItemActorKind)) {
        return;
    }

    g_env_light.setLightTevColorType_MAJI(mpItemActorModel, &tevStr);

    J3DGXColorS10 bombColor;
    bombColor.r = 0;
    bombColor.g = 0;
    bombColor.b = 0;
    bombColor.a = 0;

    const bool bombActor = isRemoteBombActorKind(mRemoteItemActorKind);
    J3DMaterial* bombPrimaryMaterial = NULL;
    J3DMaterial* bombSecondaryMaterial = NULL;
    if (bombActor && mpItemActorModel->getModelData() != NULL) {
        J3DModelData* modelData = mpItemActorModel->getModelData();
        const bool waterBomb = mRemoteItemActorKind == 3;
        const f32 pulse =
            remoteBombPulse(mRemoteBombExTime, mRemoteBombFlash, mRemoteBombFlashTicks);
        bombColor.r = static_cast<s16>(pulse * 15.0f) & 0xFF;
        bombPrimaryMaterial =
            modelData->getMaterialNum() > 0 ? modelData->getMaterialNodePointer(0) : NULL;
        bombSecondaryMaterial =
            modelData->getMaterialNum() > 1 ? modelData->getMaterialNodePointer(1) : NULL;
        if (waterBomb) {
            if (bombPrimaryMaterial != NULL) {
                bombPrimaryMaterial->setTevColor(0, &bombColor);
            }
            if (bombSecondaryMaterial != NULL) {
                bombSecondaryMaterial->setTevColor(0, &bombColor);
            }
        } else if (bombPrimaryMaterial != NULL) {
            bombPrimaryMaterial->setTevColor(1, &bombColor);
        }
    }

    mDoExt_modelEntryDL(mpItemActorModel);

    if (bombActor) {
        bombColor.r = 0;
        bombColor.g = 0;
        bombColor.b = 0;
        if (mRemoteItemActorKind == 3) {
            if (bombPrimaryMaterial != NULL) {
                bombPrimaryMaterial->setTevColor(0, &bombColor);
            }
            if (bombSecondaryMaterial != NULL) {
                bombSecondaryMaterial->setTevColor(0, &bombColor);
            }
        } else if (bombPrimaryMaterial != NULL) {
            bombPrimaryMaterial->setTevColor(1, &bombColor);
        }
    }
}

void daRemoteLink_c::drawShadowMidnaModels() {
    if (!mRemoteMidnaShadowForm || !mMidnaMatrixValid) {
        return;
    }

    mRemoteMidnaTevStr = tevStr;
    zeroColor(&mRemoteMidnaTevStr.TevColor);
    mRemoteMidnaTevStr.TevKColor.r = 0;
    mRemoteMidnaTevStr.TevKColor.g = 0;
    mRemoteMidnaTevStr.TevKColor.b = 0;
    mRemoteMidnaTevStr.TevKColor.a = 0;

    g_env_light.settingTevStruct(1, &current.pos, &mRemoteMidnaTevStr);
    if (mMidnaGlowMatrixValid) {
        g_env_light.setLightTevColorType_MAJI(mpMidnaGlowModel, &mRemoteMidnaTevStr);
        mDoExt_modelEntryDL(mpMidnaGlowModel);
    }

    MtxP root = mpShadowMidnaModel != NULL ? mpShadowMidnaModel->getAnmMtx(0) : NULL;
    cXyz offsetPos(current.pos);
    if (root != NULL) {
        offsetPos.set(root[0][3], root[1][3], root[2][3]);
        MTXMultVec(dComIfGd_getViewMtx(), &offsetPos, &offsetPos);
        offsetPos.z -= 200.0f;
        MTXMultVec(dComIfGd_getInvViewMtx(), &offsetPos, &offsetPos);
    }

    g_env_light.setLightTevColorType_MAJI(mpShadowMidnaModel, &mRemoteMidnaTevStr);
    if (mShadowMidnaInvModel.mModel != NULL) {
        mShadowMidnaInvModel.entryDL(&offsetPos);
    } else {
        mDoExt_modelEntryDL(mpShadowMidnaModel);
    }

    if (mMidnaMaskMatrixValid) {
        g_env_light.setLightTevColorType_MAJI(mpShadowMidnaMaskModel, &mRemoteMidnaTevStr);
        if (mShadowMidnaMaskInvModel.mModel != NULL) {
            mShadowMidnaMaskInvModel.entryDL(&offsetPos);
        } else {
            mDoExt_modelEntryDL(mpShadowMidnaMaskModel);
        }
    }

    if (mMidnaHandMatrixValid) {
        g_env_light.setLightTevColorType_MAJI(mpShadowMidnaHandModel, &mRemoteMidnaTevStr);
        if (mShadowMidnaHandInvModel.mModel != NULL) {
            mShadowMidnaHandInvModel.entryDL(&offsetPos);
        } else {
            mDoExt_modelEntryDL(mpShadowMidnaHandModel);
        }
    }

    if (mMidnaHairMatrixValid) {
        showOnlyMaterialShape(mpShadowMidnaHairModel->getModelData(),
                              static_cast<u16>(mMidnaHairShape), 3);
        g_env_light.setLightTevColorType_MAJI(mpShadowMidnaHairModel, &mRemoteMidnaTevStr);
        if (mShadowMidnaHairInvModel.mModel != NULL) {
            mShadowMidnaHairInvModel.entryDL(&offsetPos);
        } else {
            mDoExt_modelEntryDL(mpShadowMidnaHairModel);
        }
    }
}

void daRemoteLink_c::playRemoteSound(const dusk::multiplayer::RemoteAudioEvent& i_event) {
    Z2AudioMgr* audioMgr = Z2GetAudioMgr();
    if (audioMgr == NULL || i_event.soundId == 0) {
        return;
    }

    const bool motionAudio = isRemoteLinkMotionAudio(i_event);
    f32 volume;
    f32 pan;
    f32 dolby;
    calcRemoteLinkAudioMix(current.pos, i_event, &volume, &pan, &dolby);

    const s8 reverb = i_event.reverb < 0 ? 0 : i_event.reverb;
    if (motionAudio && !i_event.level) {
        audioMgr->seStartNoCull(i_event.soundId, i_event.mapInfo, reverb, 1.0f, volume, pan,
                                dolby);
    } else if (i_event.level) {
        audioMgr->seStartLevel(i_event.soundId, &current.pos, i_event.mapInfo, reverb, 1.0f,
                               volume, pan, dolby, 0);
    } else {
        audioMgr->seStart(i_event.soundId, &current.pos, i_event.mapInfo, reverb, 1.0f, volume,
                          pan, dolby, 0);
    }
}

void daRemoteLink_c::syncRemoteActiveSounds(
    const std::vector<dusk::multiplayer::RemoteAudioEvent>& i_events) {
    for (const dusk::multiplayer::RemoteAudioEvent& event : i_events) {
        if (event.soundId == 0) {
            continue;
        }

        ActiveRemoteSound* slot = NULL;
        ActiveRemoteSound* freeSlot = NULL;
        for (ActiveRemoteSound& sound : mActiveSounds) {
            if (sound.active && sound.event.soundId == event.soundId &&
                sound.event.sourceKind == event.sourceKind && sound.event.mapInfo == event.mapInfo)
            {
                slot = &sound;
                break;
            }
            if (!sound.active && freeSlot == NULL) {
                freeSlot = &sound;
            }
        }

        if (slot == NULL) {
            slot = freeSlot;
        }
        if (slot == NULL) {
            continue;
        }

        slot->event = event;
        slot->event.level = true;
        slot->ageTicks = 0;
        slot->active = true;

        const s8 reverb = event.reverb < 0 ? dComIfGp_getReverb(fopAcM_GetRoomNo(this)) :
                                             event.reverb;
        mActiveSoundObj.startLevelSound(event.soundId, event.mapInfo, reverb);
    }
}

void daRemoteLink_c::stopRemoteActiveSounds() {
    mActiveSoundObj.stopAllSounds(0);
    for (ActiveRemoteSound& sound : mActiveSounds) {
        sound.active = false;
        sound.ageTicks = 0;
    }
}

int daRemoteLink_c::Draw() {
    if (isRemoteLinkSceneUnsafe()) {
        return TRUE;
    }

    // Remote matrix packets can arrive less often than the 30 Hz simulation.
    // Only use the fallback after an observed multi-tick packet gap, leaving the
    // normal 30 Hz path byte-for-byte on its existing matrices.
    if (remoteMatrixLocalInterpolationEnabled() && !dusk::frame_interp::is_enabled() &&
        mRemoteMatrixBlendDurationTicks > 1)
    {
        applyRemoteBodyMatrixInterpolationForPresentation();
    }

    if (sDrawLogCount < 5) {
        DuskLog.info("RemoteLink: Draw visual body={} head={} hands={} face={} pos=({}, {}, {})",
                     (void*)mpBodyModel, (void*)mpHeadModel, (void*)mpHandModel, (void*)mpFaceModel,
                     current.pos.x, current.pos.y, current.pos.z);
        sDrawLogCount++;
    }

    tevStr.mLightInf.a = 0;
    zeroColor(&tevStr.TevColor);
    tevStr.TevKColor.r = 0;
    tevStr.TevKColor.g = 0;
    tevStr.TevKColor.b = 0;
    tevStr.TevKColor.a = 0;

    const u8 savedLightInitTimer = g_env_light.light_init_timer;
    const bool forceRemoteLightInit = tevStr.mInitTimer != 0;
    if (forceRemoteLightInit) {
        g_env_light.light_init_timer = 1;
    }

    if (mVisualState.form == FORM_WOLF) {
        g_env_light.settingTevStruct(9, &current.pos, &tevStr);
        dComIfGd_setListDark();
    } else {
        g_env_light.settingTevStruct(10, &current.pos, &tevStr);
        dComIfGd_setList();
    }

    if (forceRemoteLightInit) {
        g_env_light.light_init_timer = savedLightInitTimer;
    }
    const bool drawHumanShadowMidna =
        mRemoteMidnaShadowForm && mVisualState.form != FORM_WOLF;

    drawModel(mpBodyModel);
    if (mRemoteMidnaShadowForm && mVisualState.form == FORM_WOLF) {
        drawShadowMidnaModels();
    } else if (mVisualState.form == FORM_WOLF) {
        applyRideMidnaShapeVisibility(mpMidnaHandModel, mpMidnaHairModel, mMidnaHairShape);
        if (mMidnaMatrixValid) {
            drawModel(mpMidnaModel);
        }
        if (mMidnaMaskMatrixValid) {
            drawModel(mpMidnaMaskModel);
        }
        if (mMidnaHandMatrixValid) {
            drawModel(mpMidnaHandModel);
        }
        if (mMidnaHairMatrixValid) {
            drawModel(mpMidnaHairModel);
        }
    }
    drawModel(mpHandModel);
    drawModel(mpHeadModel);
    drawModel(mpFaceModel);
    if (isRemoteTransformProc(mRemoteProcId)) {
        if (mVisualState.form == FORM_WOLF) {
            dComIfGd_setList();
        }
        return TRUE;
    }

    if (mRemoteHeavyBoots && mVisualState.form != FORM_WOLF) {
        drawModel(mpHeavyBootModels[0]);
        drawModel(mpHeavyBootModels[1]);
    }

    if (mHeldItemMatrixValid) {
        drawModel(mpHeldItemModel);
    }
    if (mHookTipMatrixValid) {
        drawModel(mpHookTipModel);
    }
    if (mHookSubItemMatrixValid) {
        drawModel(mpHookSubItemModel);
    }
    if (mHookSubTipMatrixValid) {
        drawModel(mpHookSubTipModel);
    }
    if (mArrowMatrixValid) {
        drawModel(mpArrowModel);
    }
    if (mKanteraMatrixValid) {
        drawModel(mpKanteraModel);
    }
    if (mKanteraGlowMatrixValid) {
        drawModel(mpKanteraGlowModel);
    }
    if (mItemActorMatrixValid) {
        drawLinkedItemActorModel();
    }
    if (mRideActorMatrixValid) {
        drawModel(mpRideActorModel);
    }
    drawModel(mpSwordModel);
    drawModel(mpSheathModel);
    drawModel(mpShieldModel);

    if (drawHumanShadowMidna) {
        dComIfGd_setListDark();
        drawShadowMidnaModels();
        dComIfGd_setList();
    }

    if (mVisualState.form == FORM_WOLF) {
        dComIfGd_setList();
    }
    return TRUE;
}

int daRemoteLink_c::Delete() {
    DuskLog.info(
        "RemoteLink: deleting body={} bodyData={} headData={} handData={} faceData={} "
        "swordData={} sheathData={} shieldData={} heldData={} itemActorData={} arcHeap={}",
        (void*)mpBodyModel,
        mpBodyModel != NULL ? (void*)mpBodyModel->getModelData() : NULL,
        mpHeadModel != NULL ? (void*)mpHeadModel->getModelData() : NULL,
        mpHandModel != NULL ? (void*)mpHandModel->getModelData() : NULL,
        mpFaceModel != NULL ? (void*)mpFaceModel->getModelData() : NULL,
        mpSwordModel != NULL ? (void*)mpSwordModel->getModelData() : NULL,
        mpSheathModel != NULL ? (void*)mpSheathModel->getModelData() : NULL,
        mpShieldModel != NULL ? (void*)mpShieldModel->getModelData() : NULL,
        mpHeldItemModel != NULL ? (void*)mpHeldItemModel->getModelData() : NULL,
        mpItemActorModel != NULL ? (void*)mpItemActorModel->getModelData() : NULL,
        (void*)mpArcHeap);
    stopRemoteBombActor(false);
    stopRemoteActiveSounds();
    releaseSlot();
    destroyEquipmentModels();
    for (u32 i = 0; i < ARRAY_SIZE(mEquipmentArchives); ++i) {
        deleteArchiveSlot(mEquipmentArchives[i]);
    }
    if (mpOwnedArchive != NULL) {
        JKR_DELETE(mpOwnedArchive);
        mpOwnedArchive = NULL;
    }
    mOwnedArchiveMounted = false;
    if (mpArcHeap != NULL) {
        mDoExt_destroyExpHeap(mpArcHeap);
        mpArcHeap = NULL;
    }
    return TRUE;
}

DUSK_PROFILE actor_process_profile_definition DUSK_CONST g_profile_REMOTE_LINK = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 7,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_REMOTE_LINK_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daRemoteLink_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_ALINK_e,
    /* Actor SubMtd */ &l_daRemoteLink_Method,
    /* Status       */ fopAcStts_CULL_e | fopAcStts_UNK_0x40000_e,
    /* Group        */ fopAc_ACTOR_e,
    /* Cull Type    */ fopAc_CULLBOX_CUSTOM_e,
};
