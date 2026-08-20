#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"
#include "SSystem/SComponent/c_xyz.h"
#include "d/actor/d_a_alink.h"
#include "d/d_bg_s.h"
#include "d/d_bg_w.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"

#include "imgui.h"
#include "ImGuiConfig.hpp"
#include "ImGuiConsole.hpp"
#include "ImGuiMenuTools.hpp"
#include "dusk/detached_camera.h"
#include "dusk/frame_interpolation.h"
#include "dusk/io.hpp"
#include "dusk/load_position_overlay.hpp"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "m_Do/m_Do_controller_pad.h"

#include "JSystem/JKernel/JKRHeap.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace dusk {
    namespace {
        constexpr float kSupportPlatformHalfSize = 250.0f;
        constexpr float kRaiseLinkOffset = 5.0f;
        constexpr u32 kRepeatableGreenRupeeParams = 0x0000FF00;
        constexpr int kSlideMinCameraFrames = 12;
        constexpr int kSlideStableFrames = 4;
        constexpr int kSlideMaxCameraFrames = 120;
        constexpr int kSlideWaitTimeoutFrames = 300;
        constexpr int kSlideRecordingVersion = 1;

        struct RelativeCameraFrame {
            cXyz centerOffset;
            cXyz eyeOffset;
            cXyz up = cXyz(0.0f, 1.0f, 0.0f);
            f32 fovy = 50.0f;
            cSAngle bank;
        };

        enum class RupeeSlidePhase {
            Idle,
            CaptureWaiting,
            ReplayWaiting,
        };

        struct RupeeSlideState {
            RupeeSlidePhase phase = RupeeSlidePhase::Idle;
            RelativeCameraFrame initialCamera;
            std::array<RelativeCameraFrame, kSlideMaxCameraFrames> presentationCamera;
            cXyz replayPosition;
            s16 replayStartAngle = 0;
            int waitFrames = 0;
            int presentationFrames = 0;
            int recordedCameraFrames = 0;
            int stableCameraFrames = 0;
            bool snapshotValid = false;
            bool cameraMoved = false;
            bool aComboLatched = false;
            bool bComboLatched = false;
            bool persistenceLoaded = false;
            std::uint64_t persistenceSequence = 0;
        };

        struct SupportPlatformDzb {
            cBgD_t header;
            cBgD_Vtx_t vertices[4];
            cBgD_Tri_t triangles[2];
            cBgD_Blk_t blocks[1];
            cBgD_Tree_t trees[1];
            cBgD_Grp_t groups[1];
            cBgD_Ti_t info[1];
            char name[16];
        };

        bool s_supportPlatformEnabled = false;
        cXyz s_supportPlatformCenter;
        daAlink_c* s_supportPlatformPlayer = nullptr;
        SupportPlatformDzb s_supportPlatformDzb{};
        dBgW* s_supportPlatformBg = nullptr;
        RupeeSlideState s_rupeeSlide;

        bool nearlyEqual(float a, float b, float epsilon = 0.01f) {
            return std::fabs(a - b) <= epsilon;
        }

        bool cameraFramesMatch(const RelativeCameraFrame& a, const RelativeCameraFrame& b) {
            return nearlyEqual(a.centerOffset.x, b.centerOffset.x) &&
                   nearlyEqual(a.centerOffset.y, b.centerOffset.y) &&
                   nearlyEqual(a.centerOffset.z, b.centerOffset.z) &&
                   nearlyEqual(a.eyeOffset.x, b.eyeOffset.x) &&
                   nearlyEqual(a.eyeOffset.y, b.eyeOffset.y) &&
                   nearlyEqual(a.eyeOffset.z, b.eyeOffset.z) &&
                   nearlyEqual(a.fovy, b.fovy) && a.bank.Val() == b.bank.Val();
        }

        std::filesystem::path slideRecordingPath(int slot) {
            return ConfigPath / (slot == 0 ? "rupee_slide_0.json" : "rupee_slide_1.json");
        }

        nlohmann::json cameraFrameToJson(const RelativeCameraFrame& frame) {
            return {
                {"center", {frame.centerOffset.x, frame.centerOffset.y, frame.centerOffset.z}},
                {"eye", {frame.eyeOffset.x, frame.eyeOffset.y, frame.eyeOffset.z}},
                {"up", {frame.up.x, frame.up.y, frame.up.z}},
                {"fovy", frame.fovy},
                {"bank", frame.bank.Val()},
            };
        }

        std::optional<cXyz> xyzFromJson(const nlohmann::json& value) {
            if (!value.is_array() || value.size() != 3) {
                return std::nullopt;
            }
            try {
                const cXyz result(
                    value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
                if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
                    !std::isfinite(result.z)) {
                    return std::nullopt;
                }
                return result;
            } catch (...) {
                return std::nullopt;
            }
        }

        std::optional<RelativeCameraFrame> cameraFrameFromJson(const nlohmann::json& value) {
            if (!value.is_object()) {
                return std::nullopt;
            }
            const auto center = xyzFromJson(value.value("center", nlohmann::json{}));
            const auto eye = xyzFromJson(value.value("eye", nlohmann::json{}));
            const auto up = xyzFromJson(value.value("up", nlohmann::json{}));
            if (!center || !eye || !up) {
                return std::nullopt;
            }
            try {
                const float fovy = value.at("fovy").get<float>();
                const int bank = value.at("bank").get<int>();
                if (!std::isfinite(fovy) || fovy <= 0.0f || fovy >= 180.0f ||
                    bank < -32768 || bank > 32767) {
                    return std::nullopt;
                }
                RelativeCameraFrame frame;
                frame.centerOffset = *center;
                frame.eyeOffset = *eye;
                frame.up = *up;
                frame.fovy = fovy;
                frame.bank.Val(static_cast<s16>(bank));
                return frame;
            } catch (...) {
                return std::nullopt;
            }
        }

        struct LoadedSlideRecording {
            std::uint64_t sequence = 0;
            RelativeCameraFrame initialCamera;
            std::array<RelativeCameraFrame, kSlideMaxCameraFrames> cameraFrames;
            int cameraFrameCount = 0;
        };

        std::optional<LoadedSlideRecording> loadSlideRecordingSlot(int slot) {
            if (ConfigPath.empty()) {
                return std::nullopt;
            }
            try {
                const auto bytes = io::FileStream::ReadAllBytes(slideRecordingPath(slot));
                const auto json = nlohmann::json::parse(bytes);
                if (!json.is_object() || json.value("version", 0) != kSlideRecordingVersion) {
                    return std::nullopt;
                }

                LoadedSlideRecording recording;
                recording.sequence = json.at("sequence").get<std::uint64_t>();
                const auto initial = cameraFrameFromJson(json.at("initialCamera"));
                const auto& frames = json.at("cameraFrames");
                if (!initial || !frames.is_array() || frames.empty() ||
                    frames.size() > kSlideMaxCameraFrames) {
                    return std::nullopt;
                }
                recording.initialCamera = *initial;
                recording.cameraFrameCount = static_cast<int>(frames.size());
                for (int i = 0; i < recording.cameraFrameCount; ++i) {
                    const auto frame = cameraFrameFromJson(frames[i]);
                    if (!frame) {
                        return std::nullopt;
                    }
                    recording.cameraFrames[i] = *frame;
                }
                return recording;
            } catch (...) {
                return std::nullopt;
            }
        }

        void ensureSlideRecordingLoaded() {
            if (s_rupeeSlide.persistenceLoaded || ConfigPath.empty()) {
                return;
            }
            s_rupeeSlide.persistenceLoaded = true;
            const auto slot0 = loadSlideRecordingSlot(0);
            const auto slot1 = loadSlideRecordingSlot(1);
            const LoadedSlideRecording* newest = nullptr;
            if (slot0) {
                newest = &*slot0;
            }
            if (slot1 && (newest == nullptr || slot1->sequence > newest->sequence)) {
                newest = &*slot1;
            }
            if (newest == nullptr) {
                return;
            }

            s_rupeeSlide.initialCamera = newest->initialCamera;
            s_rupeeSlide.presentationCamera = newest->cameraFrames;
            s_rupeeSlide.recordedCameraFrames = newest->cameraFrameCount;
            s_rupeeSlide.persistenceSequence = newest->sequence;
            s_rupeeSlide.snapshotValid = true;
        }

        void persistSlideRecording() {
            if (ConfigPath.empty() || !s_rupeeSlide.snapshotValid ||
                s_rupeeSlide.recordedCameraFrames <= 0) {
                return;
            }
            try {
                std::error_code ec;
                std::filesystem::create_directories(ConfigPath, ec);
                if (ec) {
                    return;
                }

                const std::uint64_t sequence = s_rupeeSlide.persistenceSequence + 1;
                nlohmann::json frames = nlohmann::json::array();
                for (int i = 0; i < s_rupeeSlide.recordedCameraFrames; ++i) {
                    frames.push_back(cameraFrameToJson(s_rupeeSlide.presentationCamera[i]));
                }
                const nlohmann::json json = {
                    {"version", kSlideRecordingVersion},
                    {"sequence", sequence},
                    {"initialCamera", cameraFrameToJson(s_rupeeSlide.initialCamera)},
                    {"cameraFrames", std::move(frames)},
                };

                const int slot = static_cast<int>(sequence & 1u);
                io::FileStream::WriteAllText(slideRecordingPath(slot), json.dump());
                s_rupeeSlide.persistenceSequence = sequence;
            } catch (...) {
                // Keep the in-memory recording usable if persistence fails.
            }
        }

        RelativeCameraFrame captureRelativeCamera(dCamera_c* camera, const cXyz& linkPosition) {
            RelativeCameraFrame frame;
            if (camera == nullptr) {
                return frame;
            }

            camera_class* renderCamera = dCam_getCamera();
            if (renderCamera != nullptr) {
                frame.centerOffset = renderCamera->view.lookat.center - linkPosition;
                frame.eyeOffset = renderCamera->view.lookat.eye - linkPosition;
                frame.up = renderCamera->view.lookat.up;
                frame.fovy = renderCamera->view.fovy;
                frame.bank.Val(renderCamera->view.bank);
            } else {
                dCamera_c::dCamInfo_c absolute;
                camera->pushInfo(&absolute, 0);
                frame.centerOffset = absolute.mCenter - linkPosition;
                frame.eyeOffset = absolute.mEye - linkPosition;
                frame.fovy = absolute.mFovy;
                frame.bank = absolute.mBank;
            }
            return frame;
        }

        void applyRelativeCamera(dCamera_c* camera, const RelativeCameraFrame& frame,
                                 const cXyz& linkPosition) {
            if (camera == nullptr) {
                return;
            }

            dCamera_c::dCamInfo_c absolute;
            absolute.mCenter = linkPosition + frame.centerOffset;
            absolute.mEye = linkPosition + frame.eyeOffset;
            absolute.mFovy = frame.fovy;
            absolute.mBank = frame.bank;
            absolute.field_0x1e = 0;
            camera->popInfo(&absolute);

            camera_class* renderCamera = dCam_getCamera();
            if (renderCamera != nullptr) {
                fopCamM_SetCenter(
                    renderCamera, absolute.mCenter.x, absolute.mCenter.y, absolute.mCenter.z);
                fopCamM_SetEye(
                    renderCamera, absolute.mEye.x, absolute.mEye.y, absolute.mEye.z);
                fopCamM_SetUp(renderCamera, frame.up.x, frame.up.y, frame.up.z);
                fopCamM_SetFovy(renderCamera, absolute.mFovy);
                fopCamM_SetBank(renderCamera, absolute.mBank.Val());
            }
        }

        bool supportPlatformIsRegistered() {
            if (s_supportPlatformBg == nullptr || !s_supportPlatformBg->ChkUsed()) {
                return false;
            }
            const int id = s_supportPlatformBg->GetId();
            return id >= 0 && id < 0x100 &&
                   dComIfG_Bgsp().m_chk_element[id].ChkUsed() &&
                   dComIfG_Bgsp().m_chk_element[id].m_bgw_base_ptr == s_supportPlatformBg;
        }

        void destroySupportPlatform() {
            if (s_supportPlatformBg == nullptr) {
                return;
            }
            if (supportPlatformIsRegistered()) {
                dComIfG_Bgsp().Release(s_supportPlatformBg);
            } else {
                s_supportPlatformBg->Release();
            }
            JKR_DELETE(s_supportPlatformBg);
            s_supportPlatformBg = nullptr;
        }

        void setRawVertex(cBgD_Vtx_t& vertex, float x, float y, float z) {
            vertex.x = RES_F32(x);
            vertex.y = RES_F32(y);
            vertex.z = RES_F32(z);
        }

        bool createSupportPlatform(const cXyz& center, daAlink_c* owner) {
            destroySupportPlatform();
            if (owner == nullptr) {
                return false;
            }
            s_supportPlatformDzb = {};
            s_supportPlatformCenter = center;

            auto& dzb = s_supportPlatformDzb;
            dzb.header.m_v_num = 4;
            dzb.header.m_v_tbl.value.value = static_cast<s32>(offsetof(SupportPlatformDzb, vertices));
            dzb.header.m_t_num = 2;
            dzb.header.m_t_tbl.value.value = static_cast<s32>(offsetof(SupportPlatformDzb, triangles));
            dzb.header.m_b_num = 1;
            dzb.header.m_b_tbl.value.value = static_cast<s32>(offsetof(SupportPlatformDzb, blocks));
            dzb.header.m_tree_num = 1;
            dzb.header.m_tree_tbl.value.value = static_cast<s32>(offsetof(SupportPlatformDzb, trees));
            dzb.header.m_g_num = 1;
            dzb.header.m_g_tbl.value.value = static_cast<s32>(offsetof(SupportPlatformDzb, groups));
            dzb.header.m_ti_num = 1;
            dzb.header.m_ti_tbl.value.value = static_cast<s32>(offsetof(SupportPlatformDzb, info));

            setRawVertex(dzb.vertices[0], center.x - kSupportPlatformHalfSize,
                         center.y,
                         center.z - kSupportPlatformHalfSize);
            setRawVertex(dzb.vertices[1], center.x + kSupportPlatformHalfSize,
                         center.y,
                         center.z - kSupportPlatformHalfSize);
            setRawVertex(dzb.vertices[2], center.x + kSupportPlatformHalfSize,
                         center.y,
                         center.z + kSupportPlatformHalfSize);
            setRawVertex(dzb.vertices[3], center.x - kSupportPlatformHalfSize,
                         center.y,
                         center.z + kSupportPlatformHalfSize);

            dzb.triangles[0].m_vtx_idx0 = 0;
            dzb.triangles[0].m_vtx_idx1 = 2;
            dzb.triangles[0].m_vtx_idx2 = 1;
            dzb.triangles[0].m_id = 0;
            dzb.triangles[0].m_grp = 0;
            dzb.triangles[1].m_vtx_idx0 = 0;
            dzb.triangles[1].m_vtx_idx1 = 3;
            dzb.triangles[1].m_vtx_idx2 = 2;
            dzb.triangles[1].m_id = 0;
            dzb.triangles[1].m_grp = 0;

            dzb.blocks[0].field_0x0 = 0;

            dzb.trees[0].m_flag = 1;
            dzb.trees[0].m_parent_id = 0xFFFF;
            dzb.trees[0].m_id[0] = 0;
            for (int i = 1; i < 8; ++i) {
                dzb.trees[0].m_id[i] = 0xFFFF;
            }

            dzb.groups[0].m_name.value.value = static_cast<s32>(offsetof(SupportPlatformDzb, name));
            dzb.groups[0].m_scale = cXyz(1.0f, 1.0f, 1.0f);
            dzb.groups[0].m_rotation = csXyz(0, 0, 0);
            dzb.groups[0].m_translation = cXyz::Zero;
            dzb.groups[0].m_parent = 0xFFFF;
            dzb.groups[0].m_next_sibling = 0xFFFF;
            dzb.groups[0].m_first_child = 0xFFFF;
            dzb.groups[0].m_room_id = 0xFF;
            dzb.groups[0].m_first_vtx_idx = 0;
            dzb.groups[0].m_tree_idx = 0;
            dzb.groups[0].m_info = 0;
            std::strcpy(dzb.name, "DuskSupport");

            cBgS::ConvDzb(&dzb);
            s_supportPlatformBg = dBgW_NewSet(&dzb.header, cBgW::GLOBAL_e, nullptr);
            if (s_supportPlatformBg == nullptr) {
                return false;
            }
            if (static_cast<cBgS&>(dComIfG_Bgsp()).Regist(
                    s_supportPlatformBg, fpcM_ERROR_PROCESS_ID_e, owner)) {
                JKR_DELETE(s_supportPlatformBg);
                s_supportPlatformBg = nullptr;
                return false;
            }
            return true;
        }

        void updateSupportPlatform(daAlink_c* player) {
            if (!s_supportPlatformEnabled) {
                destroySupportPlatform();
                s_supportPlatformPlayer = nullptr;
                return;
            }
            if (player == nullptr) {
                destroySupportPlatform();
                s_supportPlatformPlayer = nullptr;
                return;
            }
            if (player != s_supportPlatformPlayer || !supportPlatformIsRegistered()) {
                s_supportPlatformPlayer = player;
                createSupportPlatform(cXyz(player->current.pos.x, player->current.pos.y - 1.0f,
                                           player->current.pos.z), player);
            }
        }

        bool canStartRupeeSlide(daAlink_c* player) {
            return player != nullptr && !dComIfGp_event_runCheck() && !dComIfGp_isPauseFlag() &&
                   fopAcM_SearchByName(fpcNm_Obj_LifeContainer_e) == nullptr &&
                   fopAcM_SearchByName(fpcNm_Demo_Item_e) == nullptr;
        }

        bool spawnRepeatableGreenRupee(daAlink_c* player) {
            if (player == nullptr) {
                return false;
            }

            const cXyz position = player->current.pos;
            const csXyz angle = player->shape_angle;
            const cXyz scale(1.0f, 1.0f, 1.0f);

            layer_class* savedLayer = fpcLy_CurrentLayer();
            base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
            if (playScene != nullptr) {
                fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
            }
            const fpc_ProcID id = fopAcM_create(
                fpcNm_Obj_LifeContainer_e, kRepeatableGreenRupeeParams,
                &position, fopAcM_GetRoomNo(player), &angle, &scale, -1);
            fpcLy_SetCurrentLayer(savedLayer);
            return id != fpcM_ERROR_PROCESS_ID_e;
        }

        void beginSlideCapture(daAlink_c* player, dCamera_c* camera) {
            s_rupeeSlide.initialCamera = captureRelativeCamera(camera, cXyz::Zero);
            s_rupeeSlide.waitFrames = 0;
            s_rupeeSlide.presentationFrames = 0;
            s_rupeeSlide.recordedCameraFrames = 0;
            s_rupeeSlide.stableCameraFrames = 0;
            s_rupeeSlide.cameraMoved = false;

            if (spawnRepeatableGreenRupee(player)) {
                s_rupeeSlide.phase = RupeeSlidePhase::CaptureWaiting;
            }
        }

        void beginSlideReplay(daAlink_c* player, dCamera_c* camera,
                              const cXyz& loggedPosition, s16 loggedAngle) {
            s_rupeeSlide.replayPosition = loggedPosition;
            s_rupeeSlide.replayStartAngle = loggedAngle;
            player->setPlayerPosAndAngle(
                &s_rupeeSlide.replayPosition, s_rupeeSlide.replayStartAngle, TRUE);
            const RelativeCameraFrame previousCamera =
                captureRelativeCamera(camera, cXyz::Zero);
            applyRelativeCamera(camera, s_rupeeSlide.initialCamera, cXyz::Zero);

            s_rupeeSlide.waitFrames = 0;
            s_rupeeSlide.presentationFrames = 0;
            if (spawnRepeatableGreenRupee(player)) {
                s_rupeeSlide.phase = RupeeSlidePhase::ReplayWaiting;
            } else {
                applyRelativeCamera(camera, previousCamera, cXyz::Zero);
            }
        }

        void updateSlidePresentation(daAlink_c* player, dCamera_c* camera) {
            if (s_rupeeSlide.phase == RupeeSlidePhase::Idle || player == nullptr || camera == nullptr) {
                return;
            }

            if (!frame_interp::get_ui_tick_pending()) {
                if (s_rupeeSlide.phase == RupeeSlidePhase::ReplayWaiting) {
                    if (s_rupeeSlide.presentationFrames == 0) {
                        applyRelativeCamera(camera, s_rupeeSlide.initialCamera, cXyz::Zero);
                    } else {
                        const int cameraFrame = std::min(
                            s_rupeeSlide.presentationFrames,
                            s_rupeeSlide.recordedCameraFrames) - 1;
                        applyRelativeCamera(
                            camera, s_rupeeSlide.presentationCamera[cameraFrame], cXyz::Zero);
                    }
                }
                return;
            }

            s_rupeeSlide.waitFrames++;
            const bool presentationActive =
                dComIfGp_event_runCheck() && player->checkGetItemMode();
            if (presentationActive) {
                s_rupeeSlide.presentationFrames++;
            }

            if (s_rupeeSlide.phase == RupeeSlidePhase::CaptureWaiting) {
                if (presentationActive) {
                    const RelativeCameraFrame currentCamera =
                        captureRelativeCamera(camera, cXyz::Zero);
                    const int cameraFrame = std::min(
                        s_rupeeSlide.presentationFrames - 1,
                        kSlideMaxCameraFrames - 1);
                    s_rupeeSlide.presentationCamera[cameraFrame] = currentCamera;
                    s_rupeeSlide.recordedCameraFrames = std::min(
                        s_rupeeSlide.presentationFrames, kSlideMaxCameraFrames);

                    if (!cameraFramesMatch(currentCamera, s_rupeeSlide.initialCamera)) {
                        s_rupeeSlide.cameraMoved = true;
                    }

                    if (cameraFrame > 0 && cameraFramesMatch(
                            currentCamera,
                            s_rupeeSlide.presentationCamera[cameraFrame - 1])) {
                        s_rupeeSlide.stableCameraFrames++;
                    } else {
                        s_rupeeSlide.stableCameraFrames = 0;
                    }
                }

                const bool cameraSettled =
                    s_rupeeSlide.cameraMoved &&
                    s_rupeeSlide.presentationFrames >= kSlideMinCameraFrames &&
                    s_rupeeSlide.stableCameraFrames >= kSlideStableFrames;
                const bool captureLimitReached =
                    s_rupeeSlide.presentationFrames >= kSlideMaxCameraFrames;
                if (cameraSettled || captureLimitReached) {
                    s_rupeeSlide.snapshotValid = true;
                    persistSlideRecording();
                    s_rupeeSlide.phase = RupeeSlidePhase::Idle;
                } else if (s_rupeeSlide.waitFrames >= kSlideWaitTimeoutFrames) {
                    s_rupeeSlide.phase = RupeeSlidePhase::Idle;
                }
                return;
            }

            if (s_rupeeSlide.presentationFrames == 0) {
                applyRelativeCamera(camera, s_rupeeSlide.initialCamera, cXyz::Zero);
            } else {
                const int cameraFrame = std::min(
                    s_rupeeSlide.presentationFrames,
                    s_rupeeSlide.recordedCameraFrames) - 1;
                applyRelativeCamera(
                    camera, s_rupeeSlide.presentationCamera[cameraFrame], cXyz::Zero);
                if (presentationActive) {
                    player->setPlayerPosAndAngle(
                        &s_rupeeSlide.replayPosition, s_rupeeSlide.replayStartAngle, TRUE);
                }
            }

            if (s_rupeeSlide.presentationFrames > 0 && !dComIfGp_event_runCheck()) {
                s_rupeeSlide.phase = RupeeSlidePhase::Idle;
            } else if (s_rupeeSlide.presentationFrames == 0 &&
                       s_rupeeSlide.waitFrames >= kSlideWaitTimeoutFrames) {
                s_rupeeSlide.phase = RupeeSlidePhase::Idle;
            }
        }

        void updateRepeatableGreenRupeeShortcuts(daAlink_c* player) {
            ensureSlideRecordingLoaded();

            constexpr u32 captureCombo = PAD_BUTTON_A | PAD_BUTTON_LEFT | PAD_TRIGGER_Z;
            constexpr u32 replayCombo = PAD_BUTTON_B | PAD_BUTTON_LEFT | PAD_TRIGGER_Z;
            const u32 physicalHold = mDoCPd_c::getUnfilteredHold(PAD_1);
            const bool captureHeld = (physicalHold & captureCombo) == captureCombo;
            const bool replayHeld = (physicalHold & replayCombo) == replayCombo;
            dCamera_c* camera = dCam_getBody();
            cXyz loggedPosition;
            s16 loggedAngle = 0;
            const bool hasLoggedPosition =
                GetLoggedRupeeSlidePosition(loggedPosition, loggedAngle);

            updateSlidePresentation(player, camera);

            if (!frame_interp::get_ui_tick_pending()) {
                return;
            }

            if (captureHeld && !s_rupeeSlide.aComboLatched &&
                canStartRupeeSlide(player)) {
                beginSlideCapture(player, camera);
            } else if (replayHeld && !s_rupeeSlide.bComboLatched &&
                       s_rupeeSlide.snapshotValid && hasLoggedPosition &&
                       canStartRupeeSlide(player)) {
                beginSlideReplay(player, camera, loggedPosition, loggedAngle);
            }

            s_rupeeSlide.aComboLatched = captureHeld;
            s_rupeeSlide.bComboLatched = replayHeld;
        }
    }

    void ImGuiMenuTools::ShowCameraOverlay() {
        daAlink_c* player = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
        updateSupportPlatform(player);
        updateRepeatableGreenRupeeShortcuts(player);
        detached_camera::updateControls(std::max(ImGui::GetIO().DeltaTime, 0.0f));

        if (!getSettings().backend.enableAdvancedSettings ||
            !ImGuiConsole::CheckMenuViewToggle(getSettings().hotkeys.debugCamera, m_showCameraOverlay))
        {
            return;
        }

        auto* cam = (camera_process_class*)dCam_getCamera();

        if (!m_showCameraOverlay || cam == nullptr)
            return;

        auto* dCam = &cam->mCamera;

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (m_cameraOverlayCorner != -1) {
            SetOverlayWindowLocation(m_cameraOverlayCorner);
            windowFlags |= ImGuiWindowFlags_NoMove;
        }

        // ImGui::SetNextWindowBgAlpha(0.65f);

        if (!ImGui::Begin("Camera Debug", nullptr, windowFlags)) {
            ImGui::End();
            return;
        }

        ImGui::SeparatorText("Camera Transform Data");

        cXyz center = dCam->mCenter;
        cXyz eye = dCam->mEye;

        if (ImGui::InputFloat3("Camera Center", &center.x)) {
            dCam->Reset(center, eye);
        }
        if (ImGui::InputFloat3("Camera Eye", &eye.x)) {
            dCam->Reset(center, eye);
        }

        if (ImGui::InputFloat("Camera FOV", &dCam->mFovy)) {
            dCam->mFovy = std::clamp(dCam->mFovy, 0.1f, 179.9f);
        }

        ImGui::SeparatorText("Options");

        bool eventRunning = (dComIfGp_event_runCheck() || dComIfGp_isPauseFlag()) && !getSettings().game.debugFlyCam;
        if (eventRunning) {
            ImGui::BeginDisabled();
        }
        config::ImGuiCheckbox("Fly Mode", getSettings().game.debugFlyCam);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (eventRunning) {
                ImGui::SetTooltip("Cannot enable while paused or during an active event.");
            } else {
                ImGui::SetTooltip("Detach camera and fly freely.\n"
                                  "WASD/Arrows/Left stick: move, Mouse/C-stick: look\n"
                                  "Ctrl/L: down, Space/R: up, Shift/Z: fast\n"
                                "Q Key/Y: roll left, R Key/X: roll right");
            }
        }
        if (eventRunning) {
            ImGui::EndDisabled();
        }

        if (!getSettings().game.debugFlyCam) {
            ImGui::BeginDisabled();
        }
        config::ImGuiCheckbox("Freeze Time", getSettings().game.debugFlyCamLockEvents);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!getSettings().game.debugFlyCam) {
                ImGui::SetTooltip("Enable Fly Mode first.");
            } else {
                ImGui::SetTooltip("Freezes the game while flying.");
            }
        }
        if (!getSettings().game.debugFlyCam) {
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText("Detached Presentation Camera");

        bool detachedEnabled = detached_camera::isEnabled();
        if (ImGui::Checkbox("Enable render-only camera", &detachedEnabled)) {
            if (detachedEnabled) {
                detached_camera::copyFromView(&cam->view);
            }
            detached_camera::setEnabled(detachedEnabled);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Moves only the rendered viewpoint. Gameplay and the normal camera continue.");
        }

        if (!detached_camera::isEnabled()) {
            ImGui::BeginDisabled();
        }

        bool controlsEnabled = detached_camera::areControlsEnabled();
        if (ImGui::Checkbox("Control detached camera", &controlsEnabled)) {
            detached_camera::setControlsEnabled(controlsEnabled);
        }

        bool mouseLook = detached_camera::isMouseLookEnabled();
        if (ImGui::Checkbox("Mouse look (P)", &mouseLook)) {
            detached_camera::setMouseLookEnabled(mouseLook);
        }

        float moveSpeed = detached_camera::getMoveSpeed();
        if (ImGui::SliderFloat("Move speed", &moveSpeed, 10.0f, 5000.0f, "%.0f")) {
            detached_camera::setMoveSpeed(moveSpeed);
        }

        float detachedFov = detached_camera::getFov();
        if (ImGui::SliderFloat("Detached FOV", &detachedFov, 10.0f, 150.0f, "%.1f")) {
            detached_camera::setFov(detachedFov);
        }

        float detachedBank = detached_camera::getBankDegrees();
        if (ImGui::SliderFloat("Detached bank", &detachedBank, -180.0f, 180.0f, "%.1f deg")) {
            detached_camera::setBankDegrees(detachedBank);
        }

        if (ImGui::Button("Copy Gameplay Camera")) {
            detached_camera::copyFromView(&cam->view);
        }

        ImGui::SameLine();
        if (player == nullptr) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Find Link") && player != nullptr) {
            cXyz target = player->current.pos;
            target.y += 90.0f;
            detached_camera::focusOn(target);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Frames Link using only the detached presentation camera.");
        }
        if (player == nullptr) {
            ImGui::EndDisabled();
        }

        ImGui::TextDisabled(
            "WASD: move  Space/Ctrl: up/down  Shift: fast\n"
            "Mouse: look  Q/E: bank  P: toggle mouse look");

        if (!detached_camera::isEnabled()) {
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText("Link Transform Editor");

        if (player == nullptr) {
            ImGui::TextDisabled("Link is not currently available.");
        } else {
            cXyz position = player->current.pos;
            if (ImGui::InputFloat3("Link position", &position.x, "%.4f")) {
                const cXyz velocity = player->speed;
                player->setPlayerPosAndAngle(&position, player->shape_angle.y, TRUE);
                player->speed = velocity;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Writes Link's synchronized position fields directly; movement mode is unchanged.");
            }

            int angle = static_cast<u16>(player->shape_angle.y);
            if (ImGui::InputInt("Link angle", &angle, 1, 256)) {
                player->setPlayerPosAndAngle(nullptr, static_cast<s16>(angle), TRUE);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Unsigned game angle (0-65535). Position and velocity are unchanged.");
            }

            bool supportPlatformEnabled = s_supportPlatformEnabled;
            if (ImGui::Checkbox("Solid floor under Link", &supportPlatformEnabled)) {
                s_supportPlatformEnabled = supportPlatformEnabled;
                s_supportPlatformPlayer = supportPlatformEnabled ? player : nullptr;
                if (supportPlatformEnabled &&
                    !createSupportPlatform(cXyz(player->current.pos.x, player->current.pos.y - 1.0f,
                                                player->current.pos.z), player)) {
                    s_supportPlatformEnabled = false;
                } else if (!supportPlatformEnabled) {
                    destroySupportPlatform();
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Creates an invisible flat 500 x 500 collision floor beneath Link.\n"
                    "Link uses the game's normal grounded movement and can walk off it.");
            }

            if (!s_supportPlatformEnabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Move floor under Link") && s_supportPlatformEnabled) {
                createSupportPlatform(cXyz(player->current.pos.x, player->current.pos.y - 1.0f,
                                           player->current.pos.z), player);
            }
            ImGui::SameLine();
            if (ImGui::Button("Raise Link onto platform") && s_supportPlatformEnabled) {
                const cXyz velocity = player->speed;
                const cXyz raisedPosition(
                    s_supportPlatformCenter.x,
                    s_supportPlatformCenter.y + kRaiseLinkOffset,
                    s_supportPlatformCenter.z);
                player->setPlayerPosAndAngle(&raisedPosition, player->shape_angle.y, TRUE);
                player->speed.x = velocity.x;
                player->speed.y = 0.0f;
                player->speed.z = velocity.z;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Places Link 5 units above the stored platform top at its centre.");
            }
            if (!s_supportPlatformEnabled) {
                ImGui::EndDisabled();
            }

            if (s_supportPlatformEnabled) {
                ImGui::TextDisabled(
                    "Platform centre: %.1f, %.1f, %.1f  Flat 500 x 500 floor",
                    s_supportPlatformCenter.x, s_supportPlatformCenter.y,
                    s_supportPlatformCenter.z);
            }
        }

        ShowCornerContextMenu(m_cameraOverlayCorner, 0);

        ImGui::End();
    }
}
