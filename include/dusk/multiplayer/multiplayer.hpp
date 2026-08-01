#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

class daNbomb_c;
class daAlink_c;
class dCcD_GObjInf;
class fopAc_ac_c;

namespace dusk::multiplayer {

// One switch for the unfinished remote-Midna matrix stream. Keeping this
// public lets the Online UI reflect the transport capability. Change this to
// true when the stream is ready to be tested again.
inline constexpr bool kRemoteMidnaStreamingEnabled = false;

struct PlayerColor {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;
};

struct MinimapPeerMarker {
    std::string peerId;
    int room = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    PlayerColor color;
};

struct RemoteModelMatrixSnapshot {
    bool valid = false;
    uint16_t jointCount = 0;
    uint16_t weightCount = 0;
    bool weightsOmitted = false;
    std::array<float, 12> base{};
    std::vector<float> joints;
    std::vector<float> weights;
};

struct RemoteLinkMatrixSnapshot {
    bool valid = false;
    RemoteModelMatrixSnapshot body;
    RemoteModelMatrixSnapshot hat;
    RemoteModelMatrixSnapshot face;
    RemoteModelMatrixSnapshot hand;
    RemoteModelMatrixSnapshot sword;
    RemoteModelMatrixSnapshot sheath;
    RemoteModelMatrixSnapshot shield;
    RemoteModelMatrixSnapshot heldItem;
    RemoteModelMatrixSnapshot hookTip;
    RemoteModelMatrixSnapshot hookSubItem;
    RemoteModelMatrixSnapshot hookSubTip;
    RemoteModelMatrixSnapshot arrow;
    RemoteModelMatrixSnapshot kantera;
    RemoteModelMatrixSnapshot kanteraGlow;
    RemoteModelMatrixSnapshot itemActor;
    RemoteModelMatrixSnapshot rideActor;
    RemoteModelMatrixSnapshot midna;
    RemoteModelMatrixSnapshot midnaMask;
    RemoteModelMatrixSnapshot midnaHand;
    RemoteModelMatrixSnapshot midnaHair;
    RemoteModelMatrixSnapshot midnaGlow;
    int midnaHairShape = 0;
};

struct RemoteAudioEvent {
    uint32_t sequence = 0;
    uint32_t soundId = 0;
    uint32_t mapInfo = 0;
    int8_t reverb = -1;
    uint8_t sourceKind = 0;
    bool level = false;
};

enum RemoteObjectKind : uint8_t {
    REMOTE_OBJECT_NONE = 0,
    REMOTE_OBJECT_BOMB = 1,
};

// Generic future world-sync payload. Visual remote Links still use streamed
// puppet matrices; this lane is for real remote-side actors/objects that need
// game logic, collisions, damage, or authoritative world state. Bombs are the
// first proof-of-concept adapter, not the final scope.
struct RemoteObjectSnapshot {
    bool valid = false;
    std::string peerId;
    uint8_t objectKind = REMOTE_OBJECT_NONE;
    int32_t objectId = -1;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    std::string stage;
    int room = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    int kind = 0;
    int exTime = -1;
    bool active = false;
    bool exploding = false;
};

using RemoteBombObjectSnapshot = RemoteObjectSnapshot;

enum RemoteAudioSourceKind : uint8_t {
    REMOTE_AUDIO_SOURCE_GENERIC = 0,
    REMOTE_AUDIO_SOURCE_LINK_SOUND = 1,
    REMOTE_AUDIO_SOURCE_LINK_SOUND_LEVEL = 2,
    REMOTE_AUDIO_SOURCE_LINK_VOICE = 3,
    REMOTE_AUDIO_SOURCE_LINK_VOICE_LEVEL = 4,
    REMOTE_AUDIO_SOURCE_LINK_SWORD = 5,
    REMOTE_AUDIO_SOURCE_LINK_COLLISION = 6,
    REMOTE_AUDIO_SOURCE_LINK_HIT_ITEM = 7,
};

struct PeerPoseSnapshot {
    bool valid = false;
    // Identifies which peer this pose belongs to: the relay-assigned
    // client_id when connected via the relay (tools/multiplayer_relay),
    // or a fixed placeholder in direct mode (which only ever has one
    // peer today). Stored per-pose, not just as a map key, so callers
    // that copy a snapshot out of the map don't lose the association.
    std::string peerId;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    std::string stage;
    int room = -1;
    int layer = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    bool finalGanondorfReady = false;
    int procId = 0;
    int procVar0 = 0;
    int procVar1 = 0;
    int procVar2 = 0;
    int procVar3 = 0;
    int procVar5 = 0;
    int cutType = 0;
    int cutCount = 0;
    bool jumpCancelTurn = false;
    bool manualSyncReady = false;
    float underFrame = 0.0f;
    int underBck0 = 0;
    float underFrame0 = 0.0f;
    float underRate0 = 1.0f;
    int upperBck2 = 0;
    float upperFrame2 = 0.0f;
    float upperRate2 = 1.0f;
    std::array<int16_t, 10> hatRotA{};
    std::array<int16_t, 10> hatRotB{};
    std::array<int16_t, 3> hatSwing{};
    int hatShapeY = 0;
    bool isWolf = false;
    bool isTransforming = false;
    bool transformFromWolf = false;
    bool transformToWolf = false;
    int transformProcVar0 = 0;
    int transformProcVar5 = 0;
    int transformClothesWait = 0;
    float transformFrame = 0.0f;
    int transformProcVar2 = 0;
    int transformProcVar3 = 0;
    int transformShapeX = 0;
    uint16_t equipItem = 0xFFFF;
    int swordVariant = 0;
    int shieldVariant = 0;
    int clothesVariant = 0;
    bool swordDraw = false;
    bool shieldDraw = false;
    bool shieldGuardActive = false;
    bool swordOut = false;
    bool midnaDraw = false;
    bool midnaMaskDraw = false;
    bool midnaHandDraw = false;
    bool midnaHairDraw = false;
    bool midnaShadowForm = false;
    bool heavyBoots = false;
    bool itemDraw = false;
    bool kanteraDraw = false;
    int itemActorKind = 0;
    int itemActorBombExTime = -1;
    int itemActorBombFlash = -1;
    int rideActorKind = 0;
    RemoteLinkMatrixSnapshot linkMatrices;
    bool linkMatricesFresh = false;
    std::vector<RemoteAudioEvent> audioEvents;
    std::vector<RemoteAudioEvent> activeAudioEvents;
};

struct GanondorfMpTargetSnapshot {
    bool valid = false;
    bool local = false;
    std::string peerId;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float eyeX = 0.0f;
    float eyeY = 0.0f;
    float eyeZ = 0.0f;
    float distXZ = 0.0f;
    int angleYFromGanon = 0;
    int shapeAngleY = 0;
};

struct GanondorfRemoteHitSnapshot {
    bool valid = false;
    std::string peerId;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    int procId = 0;
    int cutType = 0;
    int cutCount = 0;
    bool jumpCancelTurn = false;
    bool confirmedHit = false;
    float attackFrame = 0.0f;
    int linkAngleY = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float distXZ = 0.0f;
    int angleYFromGanon = 0;
};

struct GanondorfRemoteReactionSnapshot {
    bool valid = false;
    std::string peerId;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    int actionMode = 0;
    int moveMode = 0;
    int damageInvulnerabilityTimer = 0;
    int downHitTimer = 0;
    int downHitCount = 0;
    int downGate = 0;
    int health = 0;
    int procId = 0;
    int cutType = 0;
    int cutCount = 0;
    bool jumpCancelTurn = false;
    int linkAngleY = 0;
    float linkX = 0.0f;
    float linkY = 0.0f;
    float linkZ = 0.0f;
    float ganonX = 0.0f;
    float ganonY = 0.0f;
    float ganonZ = 0.0f;
    float distXZ = 0.0f;
    int angleYFromGanon = 0;
};

struct GanondorfSyncSnapshot {
    bool valid = false;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    std::string ownerPeerId;
    std::string stage;
    int room = -1;
    int health = 0;
    int actionMode = 0;
    int moveMode = 0;
    bool drawHorse = false;
    int damageInvulnerabilityTimer = 0;
    int downHitTimer = 0;
    int downHitCount = 0;
    int downGate = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float oldX = 0.0f;
    float oldY = 0.0f;
    float oldZ = 0.0f;
    int shapeAngleX = 0;
    int shapeAngleY = 0;
    int shapeAngleZ = 0;
    int currentAngleX = 0;
    int currentAngleY = 0;
    int currentAngleZ = 0;
    float speedX = 0.0f;
    float speedY = 0.0f;
    float speedZ = 0.0f;
    float speedF = 0.0f;
    int anmId = 0;
    int anmPlayMode = 0;
    float anmFrame = 0.0f;
    float anmRate = 1.0f;
};

struct DirectHostOptions {
    std::string name = "Host";
    std::string room = "dev";
    std::string bindHost = "0.0.0.0";
    std::string publicHost = "127.0.0.1";
    int port = 34197;
    bool dummyModel = true;
    bool nameLabels = true;
    bool syncFlags = true;
    bool syncWorld = false;
    bool displayMidna = true;
    bool remoteCollision = true;
    bool pvp = false;
};

struct DirectJoinOptions {
    std::string name = "Joiner";
    std::string inviteCode;
    bool dummyModel = true;
    bool nameLabels = true;
    bool syncFlags = true;
    bool syncWorld = false;
    bool displayMidna = true;
    bool remoteCollision = true;
    bool pvp = false;
};

struct RelayHostOptions {
    std::string name = "Host";
    std::string room = "dev";
    std::string password;
    std::string relayCode;
    bool connectLocally = false;
    bool dummyModel = true;
    bool nameLabels = true;
    bool syncFlags = true;
    bool syncWorld = false;
    bool displayMidna = true;
    bool remoteCollision = true;
    bool pvp = false;
};

struct RelayJoinOptions {
    std::string name = "Player";
    std::string room = "dev";
    std::string password;
    std::string relayCode;
    bool connectLocally = false;
    bool dummyModel = true;
    bool nameLabels = true;
    bool syncFlags = true;
    bool syncWorld = false;
    bool displayMidna = true;
    bool remoteCollision = true;
    bool pvp = false;
};

struct SessionStatus {
    bool enabled = false;
    std::string mode = "disabled";
    std::string state = "disabled";
    std::string name;
    std::string room;
    std::string host;
    std::string bindHost;
    std::string publicHost;
    std::string inviteCode;
    std::string connectionError;
    std::string ownerClientId;
    int port = 0;
    bool isOwner = false;
    bool dummyModel = false;
    bool dummyModelHostControlled = false;
    bool nameLabels = false;
    bool nameLabelsHostControlled = false;
    bool syncFlags = true;
    bool syncFlagsHostControlled = false;
    bool syncWorld = false;
    bool syncWorldHostControlled = false;
    bool displayMidna = true;
    bool remoteCollision = true;
    bool remoteCollisionHostControlled = false;
    bool pvp = false;
    bool pvpHostControlled = false;
    bool hasRecentPeerPose = false;
};

enum class ManualSyncRequestState {
    Idle,
    Waiting,
    Succeeded,
    Failed,
};

struct ManualSyncRequestStatus {
    ManualSyncRequestState state = ManualSyncRequestState::Idle;
    bool flagsOnly = false;
    std::string peerId;
    std::string peerName;
};

struct PlayerListEntry {
    std::string id;
    std::string name;
    std::string status;
    std::string area;
    uint32_t ageTicks = 0;
    bool local = false;
    bool recentPose = false;
};

void initialize();
void update();
void consume_progression_prompt_accept_button();
void shutdown();
bool is_enabled();
void record_local_link_audio_event(uint32_t soundId, bool level, uint32_t mapInfo = 0,
                                   int reverb = -1, uint8_t sourceKind = 0);
void record_local_link_active_audio_event(uint32_t soundId, uint32_t mapInfo = 0,
                                          int reverb = -1, uint8_t sourceKind = 0);
void notify_bomb_post_execute(daNbomb_c* bomb);
void report_local_pvp_attack_hit(daAlink_c* link, dCcD_GObjInf* attackInfo,
                                 fopAc_ac_c* sourceActor = nullptr,
                                 bool allowSwordReaction = true);
void report_remote_link_pvp_target_hit(fopAc_ac_c* remoteLinkActor, fopAc_ac_c* attackActor,
                                       dCcD_GObjInf* attackInfo);
void register_remote_bomb_actor(daNbomb_c* bomb);
void register_remote_bomb_actor_id(int32_t actorId);
void unregister_remote_bomb_actor_id(int32_t actorId);
bool get_remote_bomb_object_for_peer(const std::string& peerId,
                                     RemoteBombObjectSnapshot* out);
bool host_direct(const DirectHostOptions& options, std::string* errorOut = nullptr);
bool join_direct(const DirectJoinOptions& options, std::string* errorOut = nullptr);
bool host_relay(const RelayHostOptions& options, std::string* errorOut = nullptr);
bool join_relay(const RelayJoinOptions& options, std::string* errorOut = nullptr);
void disconnect_session();
bool request_manual_sync(const std::string& peerId, std::string* errorOut = nullptr);
bool request_manual_flags_sync(const std::string& peerId, std::string* errorOut = nullptr);
ManualSyncRequestStatus get_manual_sync_request_status();
SessionStatus get_session_status();
std::vector<PlayerListEntry> get_player_list();
void set_name_labels_enabled(bool enabled);
void set_sync_flags_enabled(bool enabled);
bool sync_flags_enabled();
void set_sync_world_enabled(bool enabled);
bool sync_world_enabled();
void set_remote_link_model_enabled(bool enabled);
void set_display_remote_midna_enabled(bool enabled);
bool display_remote_midna_enabled();
void set_remote_collision_enabled(bool enabled);
bool remote_collision_enabled();
void set_pvp_enabled(bool enabled);
bool pvp_enabled();
bool has_recent_peer_pose(uint32_t maxAgeTicks);
PeerPoseSnapshot get_latest_peer_pose();
bool get_ganondorf_final_mp_target(float ganonX, float ganonY, float ganonZ,
                                   GanondorfMpTargetSnapshot* out);
void report_ganondorf_final_local_hit(float linkX, float linkY, float linkZ, int cutType,
                                      int cutCount, bool jumpCancelTurn, int procId,
                                      float attackFrame, bool confirmedHit = false);
bool consume_ganondorf_final_remote_hit(float ganonX, float ganonY, float ganonZ,
                                        GanondorfRemoteHitSnapshot* out);
bool consume_ganondorf_final_remote_finish(float ganonX, float ganonY, float ganonZ,
                                           GanondorfRemoteHitSnapshot* out);
bool ganondorf_final_remote_attack_active(float ganonX, float ganonY, float ganonZ,
                                          float maxRange,
                                          GanondorfRemoteHitSnapshot* out = nullptr);
void report_ganondorf_final_local_reaction(int actionMode, int moveMode,
                                           int damageInvulnerabilityTimer, int downHitTimer,
                                           int downHitCount, int downGate, int health,
                                           float ganonX, float ganonY, float ganonZ,
                                           float linkX, float linkY, float linkZ,
                                           int procId, int cutType, int cutCount,
                                           bool jumpCancelTurn);
bool consume_ganondorf_final_remote_reaction(float ganonX, float ganonY, float ganonZ,
                                             GanondorfRemoteReactionSnapshot* out);
void report_ganondorf_final_remote_player_attack(float attackX, float attackY, float attackZ,
                                                 float radius, int damageAmount, int attackSpl);
void note_ganondorf_final_local_player_damage_handled();
bool ganondorf_final_sync_local_is_owner();
void send_ganondorf_final_sync_state(const GanondorfSyncSnapshot& snapshot);
bool get_ganondorf_final_sync_state(GanondorfSyncSnapshot* out);
void draw_debug_peer_marker();
void draw_notifications_overlay();
void draw_peer_name_labels_native();
uint8_t get_player_color_slot(const std::string& peerId);
PlayerColor get_player_color(const std::string& peerId);
std::vector<MinimapPeerMarker> get_minimap_peer_markers(uint32_t maxAgeTicks = 30);
bool was_switch_recently_remote_set(int stage, int flag, uint32_t* ageTicks = nullptr);

}  // namespace dusk::multiplayer
