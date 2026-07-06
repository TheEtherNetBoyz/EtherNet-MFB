#include "dusk/multiplayer/remote_link_dummy.hpp"

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <string>

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_remote_link.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "f_pc/f_pc_node.h"

namespace dusk::multiplayer {
namespace {

struct RemoteLinkActorDummy {
    fpc_ProcID actorId = fpcM_ERROR_PROCESS_ID_e;
    uint32_t logCount = 0;
    uint32_t traceApplyTicks = 0;
    uint32_t lastTraceSequence = 0;
    int clothesVariant = -1;
    int pendingClothesVariant = -1;
    bool isWolf = false;
    bool pendingIsWolf = false;
    bool transformAnchorValid = false;
    cXyz transformAnchorPos;
    uint32_t lastAudioEventSequence = 0;
    uint8_t recreateDelayTicks = 0;
    uint8_t visibleStableTicks = 0;
    bool recreatePending = false;
    uint32_t traceSeqGapCount = 0;
    uint32_t traceMatrixFalseCount = 0;
    uint32_t traceAgeSpikeCount = 0;
    uint32_t traceRepeatCount = 0;
    uint32_t traceMaxSeqDelta = 0;
    uint32_t traceMaxAge = 0;
};

struct RemoteBodyCollision {
    bool initialized = false;
};

std::map<std::string, RemoteLinkActorDummy> sActorDummies;
std::map<std::string, RemoteBodyCollision> sBodyCollision;
uint32_t sActorSyncLogCount = 0;

constexpr f32 kRemoteHumanBodyRadius = 35.0f;
constexpr f32 kRemoteHumanBodyHeight = 180.0f;
constexpr f32 kRemoteWolfBodyRadius = 35.0f;
constexpr f32 kRemoteWolfBodyHeight = 95.0f;
constexpr f32 kRemoteWolfBodyHalfLength = 45.0f;
constexpr f32 kLocalLinkBodyRadius = 35.0f;
constexpr uint8_t kRemoteSpawnVisibleWarmupTicks = 3;

bool dummy_trace_enabled() {
    const char* value = std::getenv("DUSK_MP_DUMMY_TRACE");
    return value == nullptr ||
           !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
             std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
             std::strcmp(value, "OFF") == 0);
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

void update_actor_dummy_collision(const std::string& peerId, const PeerPoseSnapshot& pose) {
    if (!remote_collision_enabled()) {
        sBodyCollision.erase(peerId);
        return;
    }

    fopAc_ac_c* playerActor = dComIfGp_getPlayer(0);
    if (playerActor == nullptr) {
        return;
    }

    sBodyCollision[peerId].initialized = true;
    apply_remote_body_push(pose, playerActor);
}

bool remote_link_actor_pose_supported(const PeerPoseSnapshot& pose) {
    return pose.valid && pose.ageTicks <= 30;
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
        sBodyCollision.erase(peerId);
        return;
    }

    if (it->second.actorId != fpcM_ERROR_PROCESS_ID_e) {
        fopAcM_delete(it->second.actorId);
    }
    sActorDummies.erase(it);
    sBodyCollision.erase(peerId);
}

fpc_ProcID create_remote_link_actor(u32 actorParams, cXyz* pos, s8 room, csXyz* angle,
                                    cXyz* scale) {
    if (!daRemoteLink_c::canReserveSlot()) {
        static uint32_t sRemoteLinkCapWaitLogCount = 0;
        if (sRemoteLinkCapWaitLogCount < 40) {
            ++sRemoteLinkCapWaitLogCount;
            DuskLog.warn("Multiplayer remote Link actor: spawn delayed, live actor cap full");
        }
        return fpcM_ERROR_PROCESS_ID_e;
    }

    layer_class* savedLayer = fpcLy_CurrentLayer();
    base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
    }

    fpc_ProcID id =
        fopAcM_create(fpcNm_REMOTE_LINK_e, actorParams, pos, room, angle, scale, -1);

    fpcLy_SetCurrentLayer(savedLayer);
    return id;
}

}  // namespace

void sync_remote_link_actor_dummies(const std::map<std::string, PeerPoseSnapshot>& poses) {
    const char* localStage = dComIfGp_getStartStageName();
    if (localStage == nullptr || dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() ||
        fopOvlpM_IsDoingReq())
    {
        destroy_all_remote_link_dummies();
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
        if (dummy.visibleStableTicks < kRemoteSpawnVisibleWarmupTicks) {
            ++dummy.visibleStableTicks;
        }
        daRemoteLink_c* actor = find_remote_link_actor(dummy);
        const bool anchorWolfToHumanTransform =
            pose.isTransforming && pose.transformFromWolf && !pose.transformToWolf;
        if (anchorWolfToHumanTransform && !dummy.transformAnchorValid) {
            dummy.transformAnchorPos.set(pose.x, pose.y, pose.z);
            dummy.transformAnchorValid = true;
            if (sActorSyncLogCount < 20) {
                ++sActorSyncLogCount;
                DuskLog.info("Multiplayer remote Link actor: wolf->human transform anchor peer={} "
                             "pos=({}, {}, {}) seq={}",
                             peerId, pose.x, pose.y, pose.z, pose.sequence);
            }
        } else if (!pose.isTransforming) {
            dummy.transformAnchorValid = false;
        }

        const cXyz actorPos =
            anchorWolfToHumanTransform && dummy.transformAnchorValid
                ? dummy.transformAnchorPos
                : cXyz(pose.x, pose.y, pose.z);
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
            cXyz spawnPos = actorPos;
            if (dummy.visibleStableTicks < kRemoteSpawnVisibleWarmupTicks) {
                if (sActorSyncLogCount < 20) {
                    ++sActorSyncLogCount;
                    DuskLog.info("Multiplayer remote Link actor: spawn warming peer={} "
                                 "ticks={}/{} stage={} room={} seq={}",
                                 peerId, dummy.visibleStableTicks,
                                 kRemoteSpawnVisibleWarmupTicks, pose.stage, pose.room,
                                 pose.sequence);
                }
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            dummy.actorId = create_remote_link_actor(actorParams, &spawnPos,
                                                     static_cast<s8>(pose.room), &angle,
                                                     &scale);
            if (sActorSyncLogCount < 20) {
                ++sActorSyncLogCount;
                DuskLog.info(
                    "Multiplayer remote Link actor: spawn requested peer={} id={} clothes={} "
                    "wolf={} pos=({}, {}, {}) room={} angleY={}",
                    peerId, dummy.actorId, pose.clothesVariant, pose.isWolf, actorPos.x,
                    actorPos.y, actorPos.z, pose.room, pose.angleY);
            }
            continue;
        }

        update_actor_dummy_collision(peerId, pose);
        if (!pose.linkMatricesFresh) {
            if (dummy_trace_enabled()) {
                const uint32_t sequenceDelta =
                    dummy.lastTraceSequence != 0 ? pose.sequence - dummy.lastTraceSequence : 0;
                dummy.lastTraceSequence = pose.sequence;
                if (sequenceDelta > 1) {
                    ++dummy.traceSeqGapCount;
                    if (sequenceDelta > dummy.traceMaxSeqDelta) {
                        dummy.traceMaxSeqDelta = sequenceDelta;
                    }
                } else if (sequenceDelta == 0) {
                    ++dummy.traceRepeatCount;
                }
                if (pose.ageTicks > 1) {
                    ++dummy.traceAgeSpikeCount;
                    if (pose.ageTicks > dummy.traceMaxAge) {
                        dummy.traceMaxAge = pose.ageTicks;
                    }
                }

                ++dummy.traceApplyTicks;
                if ((dummy.traceApplyTicks % 60) == 1) {
                    DuskLog.info(
                        "Multiplayer dummy trace summary peer={} id={} applies={} seq={} "
                        "last_delta={} gaps={} max_gap={} repeats={} age={} age_spikes={} "
                        "max_age={} matrix={} matrix_fresh=false matrix_false={} "
                        "pos=({}, {}, {}) room={} proc={} under_bck={} under_frame={} "
                        "upper_bck={} upper_frame={}",
                        peerId, dummy.actorId, dummy.traceApplyTicks, pose.sequence,
                        sequenceDelta, dummy.traceSeqGapCount, dummy.traceMaxSeqDelta,
                        dummy.traceRepeatCount, pose.ageTicks, dummy.traceAgeSpikeCount,
                        dummy.traceMaxAge, pose.linkMatrices.valid, dummy.traceMatrixFalseCount,
                        pose.x, pose.y, pose.z, pose.room, pose.procId, pose.underBck0,
                        pose.underFrame0, pose.upperBck2, pose.upperFrame2);
                }
            }
            continue;
        }
        actor->setRemotePose(actorPos, static_cast<s16>(pose.angleY), static_cast<s8>(pose.room));
        const bool displayMidna = display_remote_midna_enabled();
        actor->setRemoteActionState(pose.procId, pose.procVar0, pose.procVar1, pose.procVar2,
                                    pose.procVar3, pose.procVar5, pose.underFrame,
                                    static_cast<u16>(pose.underBck0), pose.underFrame0,
                                    pose.underRate0, static_cast<u16>(pose.upperBck2),
                                    pose.upperFrame2, pose.upperRate2, pose.equipItem,
                                    pose.swordVariant, pose.shieldVariant, pose.swordDraw,
                                    pose.shieldDraw, pose.swordOut, pose.heavyBoots,
                                    pose.itemDraw, pose.kanteraDraw,
                                    displayMidna && pose.midnaDraw,
                                    displayMidna && pose.midnaMaskDraw,
                                    displayMidna && pose.midnaHandDraw,
                                    displayMidna && pose.midnaHairDraw,
                                    displayMidna && pose.midnaShadowForm, pose.itemActorKind,
                                    pose.itemActorBombExTime, pose.itemActorBombFlash,
                                    pose.rideActorKind);
        actor->setRemoteHatState(pose.hatRotA, pose.hatRotB, pose.hatSwing,
                                 static_cast<s16>(pose.hatShapeY));
        actor->setRemoteMatrices(pose.linkMatrices);
        RemoteBombObjectSnapshot bombObject;
        if (get_remote_bomb_object_for_peer(peerId, &bombObject)) {
            actor->setRemoteBombObjectState(bombObject);
        } else {
            actor->setRemoteBombObjectState(RemoteBombObjectSnapshot{});
        }
        if (dummy_trace_enabled()) {
            const uint32_t sequenceDelta =
                dummy.lastTraceSequence != 0 ? pose.sequence - dummy.lastTraceSequence : 0;
            dummy.lastTraceSequence = pose.sequence;
            if (sequenceDelta > 1) {
                ++dummy.traceSeqGapCount;
                if (sequenceDelta > dummy.traceMaxSeqDelta) {
                    dummy.traceMaxSeqDelta = sequenceDelta;
                }
            } else if (sequenceDelta == 0) {
                ++dummy.traceRepeatCount;
            }
            if (!pose.linkMatrices.valid) {
                ++dummy.traceMatrixFalseCount;
            }
            if (pose.ageTicks > 1) {
                ++dummy.traceAgeSpikeCount;
                if (pose.ageTicks > dummy.traceMaxAge) {
                    dummy.traceMaxAge = pose.ageTicks;
                }
            }

            ++dummy.traceApplyTicks;
            if ((dummy.traceApplyTicks % 60) == 1) {
                DuskLog.info(
                    "Multiplayer dummy trace summary peer={} id={} applies={} seq={} "
                    "last_delta={} gaps={} max_gap={} repeats={} age={} age_spikes={} "
                    "max_age={} matrix={} matrix_fresh={} matrix_false={} pos=({}, {}, {}) "
                    "room={} proc={} under_bck={} under_frame={} upper_bck={} upper_frame={}",
                    peerId, dummy.actorId, dummy.traceApplyTicks, pose.sequence, sequenceDelta,
                    dummy.traceSeqGapCount, dummy.traceMaxSeqDelta, dummy.traceRepeatCount,
                    pose.ageTicks, dummy.traceAgeSpikeCount, dummy.traceMaxAge,
                    pose.linkMatrices.valid, pose.linkMatricesFresh,
                    dummy.traceMatrixFalseCount, pose.x, pose.y, pose.z, pose.room, pose.procId,
                    pose.underBck0, pose.underFrame0, pose.upperBck2, pose.upperFrame2);
            }
        }
        for (const RemoteAudioEvent& event : pose.audioEvents) {
            if (event.sequence > dummy.lastAudioEventSequence) {
                actor->playRemoteSound(event);
                dummy.lastAudioEventSequence = event.sequence;
                static uint32_t sAudioRxLogCount = 0;
                if (sAudioRxLogCount < 20) {
                    ++sAudioRxLogCount;
                    DuskLog.info("Multiplayer audio rx peer={} seq={} sound={:#x} mapinfo={} "
                                 "reverb={} source={} level={}",
                                 peerId, event.sequence, event.soundId, event.mapInfo,
                                 static_cast<int>(event.reverb),
                                 static_cast<int>(event.sourceKind), event.level);
                }
            }
        }
        actor->syncRemoteActiveSounds(pose.activeAudioEvents);
        if (dummy.logCount < 5) {
            ++dummy.logCount;
            DuskLog.info("Multiplayer remote Link actor: updated peer={} id={} pos=({}, {}, {}) "
                         "room={} angleY={}",
                         peerId, dummy.actorId, pose.x, pose.y, pose.z, pose.room, pose.angleY);
        }
    }
}

bool get_remote_link_dummy_label_position(const std::string& peerId, cXyz* outPos) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        return false;
    }

    daRemoteLink_c* actor = find_remote_link_actor(it->second);
    return actor != nullptr && actor->getNameLabelPosition(outPos);
}

bool get_remote_link_dummy_peer_id_for_actor(fopAc_ac_c* actor, std::string* outPeerId) {
    if (actor == nullptr || outPeerId == nullptr || fopAcM_GetName(actor) != fpcNm_REMOTE_LINK_e) {
        return false;
    }

    const fpc_ProcID actorId = fopAcM_GetID(actor);
    for (const auto& entry : sActorDummies) {
        if (entry.second.actorId == actorId) {
            *outPeerId = entry.first;
            return !outPeerId->empty();
        }
    }

    return false;
}

void destroy_remote_link_dummy(const std::string& peerId) {
    destroy_remote_link_actor_dummy(peerId);
}

void destroy_all_remote_link_dummies() {
    for (auto it = sActorDummies.begin(); it != sActorDummies.end();) {
        const std::string peerId = it->first;
        ++it;
        destroy_remote_link_actor_dummy(peerId);
    }
    sBodyCollision.clear();
}

void draw_remote_link_dummy(const std::string& peerId, const PeerPoseSnapshot& pose) {
    if (pose.valid && sActorDummies.find(peerId) != sActorDummies.end()) {
        update_actor_dummy_collision(peerId, pose);
    }
}

}  // namespace dusk::multiplayer
