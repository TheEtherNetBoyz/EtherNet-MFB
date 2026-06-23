#include "dusk/multiplayer/remote_link_dummy.hpp"

#include <map>

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"

namespace dusk::multiplayer {
namespace {

struct RemoteLinkModel {
    J3DModel* model = nullptr;
    J3DModelData* data = nullptr;
    JKRSolidHeap* heap = nullptr;
};

struct RemoteLinkDummy {
    RemoteLinkModel body;
    RemoteLinkModel hat;
    RemoteLinkModel face;
    RemoteLinkModel hand;
    RemoteLinkModel sword;
    RemoteLinkModel shield;
    std::string stage;
    int room = -128;
    uint32_t stableFrames = 0;
    uint32_t drawLogCount = 0;
};

// Keyed by PeerPoseSnapshot::peerId. Only one peer exists in practice today
// (see remote_link_dummy.hpp), but storage is peer-keyed so adding real
// multi-peer rendering later is a drawing-loop change, not a data-model one.
std::map<std::string, RemoteLinkDummy> sDummies;

void destroy_model(RemoteLinkModel& model) {
    if (model.heap != nullptr) {
        mDoExt_destroySolidHeap(model.heap);
    }

    model.model = nullptr;
    model.data = nullptr;
    model.heap = nullptr;
}

J3DModel* get_or_create_model(RemoteLinkModel& dst, J3DModel* source, u32 heapSize) {
    if (source == nullptr || source->getModelData() == nullptr) {
        destroy_model(dst);
        return nullptr;
    }

    J3DModelData* sourceData = source->getModelData();
    if (dst.model != nullptr && dst.data == sourceData) {
        return dst.model;
    }

    destroy_model(dst);

    dst.heap = mDoExt_createSolidHeapFromGameToCurrent(heapSize, 0x20);
    if (dst.heap == nullptr) {
        DuskLog.warn("Multiplayer remote Link dummy: heap allocation failed size={}", heapSize);
        return nullptr;
    }

    dst.model = mDoExt_J3DModel__create(sourceData, 0x80000, 0x11000084);
    if (dst.model == nullptr) {
        DuskLog.warn("Multiplayer remote Link dummy: J3DModel creation failed");
        mDoExt_destroySolidHeap(dst.heap);
        dst.heap = nullptr;
        mDoExt_restoreCurrentHeap();
        return nullptr;
    }

    dst.data = sourceData;
    dst.model->setBaseScale(*source->getBaseScale());
    dst.model->setUserArea(0);
    mDoExt_adjustSolidHeapToSystem(dst.heap);
    DuskLog.info("Multiplayer remote Link dummy: cloned model={} joints={}",
                 static_cast<void*>(dst.model), sourceData->getJointNum());
    return dst.model;
}

void copy_model_matrices(J3DModel* dst, J3DModel* source, CMtxP localToRemote) {
    if (dst == nullptr || source == nullptr || source->getModelData() == nullptr) {
        return;
    }

    Mtx outMtx;
    mDoMtx_concat(localToRemote, source->getBaseTRMtx(), outMtx);
    dst->setBaseTRMtx(outMtx);
    dst->setBaseScale(*source->getBaseScale());

    const u16 jointNum = source->getModelData()->getJointNum();
    for (u16 i = 0; i < jointNum; i++) {
        mDoMtx_concat(localToRemote, source->getAnmMtx(i), outMtx);
        dst->setAnmMtx(i, outMtx);
    }

    const u16 weightMtxNum = source->getModelData()->getWEvlpMtxNum();
    for (u16 i = 0; i < weightMtxNum; i++) {
        mDoMtx_concat(localToRemote, source->getWeightAnmMtx(i), outMtx);
        mDoMtx_copy(outMtx, dst->getWeightAnmMtx(i));
    }
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
    if (source.jointCount > data->getJointNum() ||
        source.weightCount > data->getWEvlpMtxNum() ||
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

void entry_model_without_calc(J3DModel* model) {
    if (model == nullptr) {
        return;
    }

    dComIfGd_setList();
    model->unlock();
    model->entry();
    model->lock();
    model->viewCalc();
}

bool build_local_to_remote_mtx(daAlink_c* link, const PeerPoseSnapshot& pose, Mtx outMtx) {
    if (link == nullptr || link->mpLinkModel == nullptr) {
        return false;
    }

    Mtx remoteBase;
    Mtx invLocalBase;

    mDoMtx_stack_c::transS(pose.x, pose.y, pose.z);
    mDoMtx_stack_c::YrotM(static_cast<s16>(pose.angleY));
    mDoMtx_copy(mDoMtx_stack_c::get(), remoteBase);

    if (!MTXInverse(link->mpLinkModel->getBaseTRMtx(), invLocalBase)) {
        return false;
    }

    mDoMtx_concat(remoteBase, invLocalBase, outMtx);
    return true;
}

}  // namespace

void destroy_remote_link_dummy(const std::string& peerId) {
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
    destroy_model(dummy.shield);
    dummy.stage.clear();
    dummy.room = -128;
    dummy.stableFrames = 0;
    dummy.drawLogCount = 0;
    sDummies.erase(it);
}

void destroy_all_remote_link_dummies() {
    for (auto& entry : sDummies) {
        RemoteLinkDummy& dummy = entry.second;
        destroy_model(dummy.body);
        destroy_model(dummy.hat);
        destroy_model(dummy.face);
        destroy_model(dummy.hand);
        destroy_model(dummy.sword);
        destroy_model(dummy.shield);
    }
    sDummies.clear();
}

void draw_remote_link_dummy(const std::string& peerId, const PeerPoseSnapshot& pose) {
    fopAc_ac_c* playerActor = dComIfGp_getPlayer(0);
    if (playerActor == nullptr) {
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
        return;
    }

    if (link->mpLinkModel == nullptr) {
        return;
    }

    // checkWolf() returns the raw masked flag (mNoResetFlg1 & FLG1_IS_WOLF),
    // i.e. 0 or 0x2000000, not a normalized 0/1 -- must explicitly convert
    // to bool before comparing against pose.isWolf, or this would treat
    // every wolf-form match as a mismatch.
    if (static_cast<bool>(link->checkWolf()) != pose.isWolf) {
        // The dummy clones whatever model/skeleton the LOCAL player currently
        // has (mpLinkModel etc.), then either copies the peer's matrices onto
        // it or drives it from the local player's own matrices as a fallback.
        // Wolf and human bodies have different joint counts/hierarchies
        // (see architecture.md), so if the peer's reported form doesn't match
        // the local player's current form, neither path can produce a
        // correct pose: remote matrices get rejected by the joint-count guard
        // in copy_remote_model_matrices, and the local fallback would render
        // the peer in the WRONG form using the local skeleton. Skip drawing
        // entirely rather than show a guaranteed-wrong clone -- and tear down
        // any existing clone so it doesn't linger as a frozen ghost for
        // however long the mismatch lasts.
        destroy_remote_link_dummy(peerId);
        return;
    }

    if (pose.room != static_cast<int>(fopAcM_GetRoomNo(link))) {
        return;
    }

    auto dummyIt = sDummies.find(peerId);
    if (dummyIt != sDummies.end() &&
        (dummyIt->second.stage != pose.stage || dummyIt->second.room != pose.room))
    {
        destroy_remote_link_dummy(peerId);
        dummyIt = sDummies.end();
    }

    if (dummyIt == sDummies.end()) {
        RemoteLinkDummy& newDummy = sDummies[peerId];
        newDummy.stage = pose.stage;
        newDummy.room = pose.room;
        return;
    }

    RemoteLinkDummy& dummy = dummyIt->second;

    if (dummy.stableFrames < 30) {
        ++dummy.stableFrames;
        return;
    }

    J3DModel* body = get_or_create_model(dummy.body, link->mpLinkModel, 0x200000);
    J3DModel* hat = get_or_create_model(dummy.hat, link->mpLinkHatModel, 0x100000);
    J3DModel* face = get_or_create_model(dummy.face, link->mpLinkFaceModel, 0x100000);
    J3DModel* hand = get_or_create_model(dummy.hand, link->mpLinkHandModel, 0x100000);
    J3DModel* sword = get_or_create_model(dummy.sword, link->mSwordModel, 0x100000);
    J3DModel* shield = get_or_create_model(dummy.shield, link->mShieldModel, 0x100000);
    if (body == nullptr) {
        return;
    }

    const bool usedRemoteMatrices =
        pose.linkMatrices.valid &&
        copy_remote_model_matrices(body, pose.linkMatrices.body);
    if (!usedRemoteMatrices) {
        Mtx localToRemote;
        if (!build_local_to_remote_mtx(link, pose, localToRemote)) {
            return;
        }
        copy_model_matrices(body, link->mpLinkModel, localToRemote);
    }
    entry_model_without_calc(body);

    if (hat != nullptr) {
        bool drewHat = false;
        if (usedRemoteMatrices && pose.linkMatrices.hat.valid) {
            drewHat = copy_remote_model_matrices(hat, pose.linkMatrices.hat);
        } else if (!usedRemoteMatrices) {
            Mtx localToRemote;
            if (build_local_to_remote_mtx(link, pose, localToRemote)) {
                copy_model_matrices(hat, link->mpLinkHatModel, localToRemote);
                drewHat = true;
            }
        }
        if (drewHat) {
            entry_model_without_calc(hat);
        }
    }

    if (face != nullptr) {
        bool drewFace = false;
        if (usedRemoteMatrices && pose.linkMatrices.face.valid) {
            drewFace = copy_remote_model_matrices(face, pose.linkMatrices.face);
        } else if (!usedRemoteMatrices) {
            Mtx localToRemote;
            if (build_local_to_remote_mtx(link, pose, localToRemote)) {
                copy_model_matrices(face, link->mpLinkFaceModel, localToRemote);
                drewFace = true;
            }
        }
        if (drewFace) {
            entry_model_without_calc(face);
        }
    }

    if (hand != nullptr) {
        bool drewHand = false;
        if (usedRemoteMatrices && pose.linkMatrices.hand.valid) {
            drewHand = copy_remote_model_matrices(hand, pose.linkMatrices.hand);
        } else if (!usedRemoteMatrices) {
            Mtx localToRemote;
            if (build_local_to_remote_mtx(link, pose, localToRemote)) {
                copy_model_matrices(hand, link->mpLinkHandModel, localToRemote);
                drewHand = true;
            }
        }
        if (drewHand) {
            entry_model_without_calc(hand);
        }
    }

    if (sword != nullptr) {
        bool drewSword = false;
        if (usedRemoteMatrices && pose.linkMatrices.sword.valid) {
            drewSword = copy_remote_model_matrices(sword, pose.linkMatrices.sword);
        } else if (!usedRemoteMatrices) {
            Mtx localToRemote;
            if (build_local_to_remote_mtx(link, pose, localToRemote)) {
                copy_model_matrices(sword, link->mSwordModel, localToRemote);
                drewSword = true;
            }
        }
        if (drewSword) {
            entry_model_without_calc(sword);
        }
    }

    if (shield != nullptr) {
        bool drewShield = false;
        if (usedRemoteMatrices && pose.linkMatrices.shield.valid) {
            drewShield = copy_remote_model_matrices(shield, pose.linkMatrices.shield);
        } else if (!usedRemoteMatrices) {
            Mtx localToRemote;
            if (build_local_to_remote_mtx(link, pose, localToRemote)) {
                copy_model_matrices(shield, link->mShieldModel, localToRemote);
                drewShield = true;
            }
        }
        if (drewShield) {
            entry_model_without_calc(shield);
        }
    }

    if (dummy.drawLogCount < 5) {
        ++dummy.drawLogCount;
        DuskLog.info(
            "Multiplayer remote Link dummy: drew copy #{} peer={} matrices={} pos=({}, {}, {}) angleY={}",
            dummy.drawLogCount, peerId, usedRemoteMatrices ? "remote" : "local-fallback",
            pose.x, pose.y, pose.z, pose.angleY);
    }
}

}  // namespace dusk::multiplayer
