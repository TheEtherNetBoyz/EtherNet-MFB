#include "dusk/multiplayer/invite_code.hpp"
#include "nlohmann/json.hpp"

#if _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    static constexpr int kSendFlags = 0;
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    static constexpr int kSendFlags =
    #if defined(__APPLE__)
        0;
    #else
        MSG_NOSIGNAL;
    #endif
    #ifndef INVALID_SOCKET
        #define INVALID_SOCKET -1
    #endif
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr int kProtocolVersion = 2;
constexpr size_t kMaxLineBytes = 512 * 1024;
constexpr size_t kMaxQueuedBytes = 8 * 1024 * 1024;
constexpr size_t kMaxRoomClients = 8;
constexpr size_t kMaxRoomIdBytes = 64;
constexpr size_t kMaxPasswordBytes = 128;
constexpr size_t kMaxUsernameBytes = 32;
constexpr size_t kMaxReliableSequences = 4096;
constexpr size_t kMaxReadBytesPerTick = 256 * 1024;
constexpr size_t kMinPasswordBytes = 6;
constexpr double kHelloTimeoutSeconds = 10.0;
constexpr size_t kUdpSenderIdBytes = 32;
constexpr size_t kMaxUdpDatagramBytes = 2048;
constexpr uint8_t kUdpPacketTypePoseJson = 1;
constexpr uint8_t kUdpPacketTypePoseMsgpack = 2;
constexpr uint8_t kUdpPacketTypeRemoteObject = 3;
constexpr uint8_t kUdpPacketTypeMidnaMsgpack = 4;
constexpr uint8_t kUdpPacketTypePoseAck = 5;
constexpr uint8_t kUdpPacketTypeRelayRegister = 6;
constexpr size_t kMaxUdpBytesPerClientSecond = 4 * 1024 * 1024;

#pragma pack(push, 1)
struct UdpRelayHeader {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint16_t headerSize;
    uint32_t sequence;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint32_t uncompressedSize;
    uint32_t compressedSize;
    uint16_t payloadSize;
    char senderId[kUdpSenderIdBytes];
};

struct UdpPoseAckPacket {
    uint32_t sequence;
    uint8_t ackedType;
    char ackedSenderId[kUdpSenderIdBytes];
    uint8_t stressFlags;
};
#pragma pack(pop)

static_assert(sizeof(UdpRelayHeader) == 58);

const std::set<std::string> kGameplayRouteTypes = {
    "event_bit",
    "tbox_bit",
    "switch_bit",
    "room_switch_bit",
    "item_bit",
    "dungeon_item_bit",
    "save_snapshot",
    "key_num",
    "light_drop_num",
    "light_drop_get_flag",
    "max_life_update",
    "bottle_slots",
    "bomb_bag_slot",
    "rupee_count",
    "item_get",
    "rando_item_get",
    "item_first_bit",
    "collect_crystal",
    "collect_mirror",
    "dark_clear_lv",
    "transform_lv",
    "region_bit",
    "collect",
    "visited_room",
    "letter_get",
    "presence",
    "progression_state",
    "puppet_preference",
    "midna_preference",
    "midna_pose",
    "pvp_hit",
    "ganondorf_owner_claim",
    "ganondorf_owner",
    "ganondorf_hit",
    "ganondorf_reaction",
    "ganondorf_player_damage",
    "ganondorf_state",
    "ooccoo_state",
};

using SteadyClock = std::chrono::steady_clock;

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
            std::strcmp(value, "ON") == 0);
}

bool relay_packet_trace_enabled() {
    static const bool enabled = env_enabled("DUSK_MP_RELAY_PACKET_TRACE");
    return enabled;
}

const char* packet_category(const std::string& type) {
    if (type == "pose") {
        return "pose";
    }
    if (type == "hello" || type == "welcome" || type == "peer_joined" ||
        type == "peer_left" || type == "owner_changed" ||
        type == "room_settings" || type == "name_labels")
    {
        return "session";
    }
    if (type == "ping" || type == "pong" || type == "error" || type == "ack") {
        return "control";
    }
    if (type == "sync_request") {
        return "manual_sync_request";
    }
    if (type == "save_snapshot") {
        return "save_snapshot";
    }
    if (type == "event_bit" || type == "tbox_bit" || type == "switch_bit" ||
        type == "room_switch_bit" || type == "item_bit" || type == "dungeon_item_bit")
    {
        return "world_state";
    }
    if (type == "item_get" || type == "item_first_bit" ||
        type == "collect_crystal" || type == "collect_mirror" ||
        type == "dark_clear_lv" || type == "transform_lv" || type == "region_bit" ||
        type == "collect" || type == "visited_room" || type == "letter_get")
    {
        return "inventory_progress";
    }
    if (type == "key_num" || type == "light_drop_num" || type == "light_drop_get_flag" ||
        type == "max_life_update" || type == "bottle_slots" || type == "rupee_count")
    {
        return "counters";
    }
    if (type == "reliable") {
        return "reliable_envelope";
    }
    return "other";
}

size_t json_field_bytes(const json& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? 0 : it->dump().size();
}

size_t pose_base_state_bytes(const json& state) {
    if (!state.is_object()) {
        return 0;
    }

    json base = state;
    base.erase("link_matrices");
    base.erase("audio_events");
    return base.dump().size();
}

void trace_packet_tx(const std::string& clientId, const json& message, size_t bytes) {
    if (!relay_packet_trace_enabled()) {
        return;
    }

    const std::string type = message.value("type", "");
    const char* category = packet_category(type);
    if (type == "pose") {
        const json state = message.value("state", json::object());
        std::cout << "MP_RELAY_PACKET_TX client=" << clientId << " category=" << category
                  << " type=" << type << " bytes=" << bytes
                  << " sequence=" << message.value("sequence", 0U)
                  << " base_state=" << pose_base_state_bytes(state)
                  << " link_matrices=" << json_field_bytes(state, "link_matrices")
                  << " audio_events=" << json_field_bytes(state, "audio_events") << "\n";
        return;
    }

    if (type == "save_snapshot") {
        std::cout << "MP_RELAY_PACKET_TX client=" << clientId << " category=" << category
                  << " type=" << type << " bytes=" << bytes
                  << " manual_sync=" << message.value("manual_sync", false)
                  << " full_state=" << json_field_bytes(message, "full_state")
                  << " event_flags=" << json_field_bytes(message, "event_flags")
                  << " chests=" << json_field_bytes(message, "chests")
                  << " switches=" << json_field_bytes(message, "switches")
                  << " items=" << json_field_bytes(message, "items")
                  << " dungeon_items=" << json_field_bytes(message, "dungeon_items") << "\n";
        return;
    }

    std::cout << "MP_RELAY_PACKET_TX client=" << clientId << " category=" << category
              << " type=" << type << " bytes=" << bytes << "\n";
}

struct Client {
    socket_t sock = INVALID_SOCKET;
    std::string id;
    std::string roomId;
    std::string name;
    std::string stage;
    std::string rxBuffer;
    std::deque<std::string> txQueue;
    size_t txOffset = 0;
    size_t txQueuedBytes = 0;
    std::deque<uint32_t> reliableOrder;
    std::unordered_set<uint32_t> reliableSeen;
    std::string udpToken;
    sockaddr_in udpAddr{};
    SteadyClock::time_point acceptedAt = SteadyClock::now();
    SteadyClock::time_point udpRateWindowStarted = SteadyClock::now();
    size_t udpRateWindowBytes = 0;
    uint32_t poseCount = 0;
    bool closeAfterFlush = false;
    bool disconnectRequested = false;
    bool wantsPuppet = true;
    bool wantsMidna = false;
    bool udpAddrKnown = false;
};

struct Room {
    std::string id;
    std::string password;
    std::vector<std::string> clientIds;
    std::string ownerClientId;
    bool dummyModel = true;
    bool syncFlags = true;
    bool syncWorld = false;
    bool remoteCollision = true;
    bool pvp = false;
};

struct Options {
    std::string host = "127.0.0.1";
    std::string publicHost = "127.0.0.1";
    int port = 34197;
    int publicPort = 0;
    double helloTimeoutSeconds = kHelloTimeoutSeconds;
    bool verbose = false;
};

json room_settings_json(const Room& room) {
    return {
        {"dummy_model", room.dummyModel},
        {"sync_flags", room.syncFlags},
        {"sync_world", room.syncWorld},
        {"remote_collision", room.remoteCollision},
        {"pvp", room.pvp},
    };
}

bool apply_room_settings(Room& room, const json& settings) {
    if (!settings.is_object()) {
        return false;
    }

    auto read_bool = [&](const char* key, bool& value) {
        const auto it = settings.find(key);
        if (it == settings.end()) {
            return true;
        }
        if (!it->is_boolean()) {
            return false;
        }
        value = it->get<bool>();
        return true;
    };

    bool dummyModel = room.dummyModel;
    bool syncFlags = room.syncFlags;
    bool syncWorld = room.syncWorld;
    bool remoteCollision = room.remoteCollision;
    bool pvp = room.pvp;
    if (!read_bool("dummy_model", dummyModel) ||
        !read_bool("sync_flags", syncFlags) ||
        !read_bool("sync_world", syncWorld) ||
        !read_bool("remote_collision", remoteCollision) ||
        !read_bool("pvp", pvp))
    {
        return false;
    }

    room.dummyModel = dummyModel;
    room.syncFlags = syncFlags;
    room.syncWorld = syncWorld;
    room.remoteCollision = remoteCollision;
    room.pvp = remoteCollision && pvp;
    return true;
}

class Relay {
public:
    explicit Relay(Options options) : mOptions(std::move(options)) {}

    ~Relay() {
        for (auto& entry : mClients) {
            close_socket(entry.second.sock);
        }
        close_socket(mListenSock);
        close_socket(mUdpSock);
#if _WIN32
        if (mWinsockStarted) {
            WSACleanup();
        }
#endif
    }

    bool run() {
#if _WIN32
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed\n";
            return false;
        }
        mWinsockStarted = true;
#endif

        mListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (mListenSock == INVALID_SOCKET) {
            std::cerr << "socket failed\n";
            return false;
        }

        int reuse = 1;
        setsockopt(mListenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                   sizeof(reuse));

        if (!set_nonblocking(mListenSock)) {
            std::cerr << "failed to set listen socket nonblocking\n";
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(mOptions.port));
        if (inet_pton(AF_INET, mOptions.host.c_str(), &addr.sin_addr) != 1) {
            std::cerr << "invalid host: " << mOptions.host << "\n";
            return false;
        }

        if (bind(mListenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(mListenSock, SOMAXCONN) != 0)
        {
            std::cerr << "bind/listen failed\n";
            return false;
        }

        mUdpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (mUdpSock == INVALID_SOCKET || !set_nonblocking(mUdpSock) ||
            bind(mUdpSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            std::cerr << "UDP bind failed\n";
            return false;
        }

        std::cout << "TP relay listening on TCP+UDP " << mOptions.host << ":"
                  << mOptions.port << "\n";
        while (true) {
            tick();
        }
    }

private:
    void tick() {
        fd_set readfds;
        fd_set writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(mListenSock, &readfds);
        FD_SET(mUdpSock, &readfds);
        socket_t maxSock = std::max(mListenSock, mUdpSock);

        for (const auto& entry : mClients) {
            const Client& client = entry.second;
            if (!client.closeAfterFlush && !client.disconnectRequested) {
                FD_SET(client.sock, &readfds);
            }
            if (!client.txQueue.empty() && !client.disconnectRequested) {
                FD_SET(client.sock, &writefds);
            }
            if (entry.second.sock > maxSock) {
                maxSock = entry.second.sock;
            }
        }

        timeval timeout{0, 100000};
#if _WIN32
        const int result = select(0, &readfds, &writefds, nullptr, &timeout);
#else
        const int result = select(maxSock + 1, &readfds, &writefds, nullptr, &timeout);
#endif
        if (result < 0) {
            return;
        }

        if (result > 0 && FD_ISSET(mListenSock, &readfds)) {
            accept_client();
        }
        if (result > 0 && FD_ISSET(mUdpSock, &readfds)) {
            receive_udp_datagrams();
        }

        std::vector<std::string> disconnected;
        for (auto& entry : mClients) {
            Client& client = entry.second;
            if (client.disconnectRequested) {
                disconnected.push_back(client.id);
                continue;
            }
            if (result > 0 && FD_ISSET(client.sock, &readfds) &&
                !read_from_client(client))
            {
                disconnected.push_back(client.id);
                continue;
            }
            if (result > 0 && FD_ISSET(client.sock, &writefds) &&
                !flush_client(client))
            {
                disconnected.push_back(client.id);
                continue;
            }
            if (client.closeAfterFlush && client.txQueue.empty()) {
                disconnected.push_back(client.id);
            }
        }

        const auto now = SteadyClock::now();
        for (auto& entry : mClients) {
            Client& client = entry.second;
            if (client.roomId.empty() && !client.closeAfterFlush &&
                std::chrono::duration<double>(now - client.acceptedAt).count() >=
                    mOptions.helloTimeoutSeconds)
            {
                reject_and_close(client, "hello_timeout");
            }
        }

        std::sort(disconnected.begin(), disconnected.end());
        disconnected.erase(std::unique(disconnected.begin(), disconnected.end()),
                           disconnected.end());
        for (const std::string& clientId : disconnected) {
            remove_client(clientId);
        }
        if (mOptions.verbose || relay_packet_trace_enabled()) {
            std::cout.flush();
        }
    }

    void accept_client() {
        while (true) {
            sockaddr_in peerAddr{};
#if _WIN32
            int peerLen = sizeof(peerAddr);
#else
            socklen_t peerLen = sizeof(peerAddr);
#endif
            socket_t accepted = accept(mListenSock, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen);
            if (accepted == INVALID_SOCKET) {
                return;
            }

#if !_WIN32
            if (accepted >= FD_SETSIZE) {
                close_socket(accepted);
                log("connection rejected: descriptor_out_of_range");
                continue;
            }
#endif
            if (mClients.size() >= static_cast<size_t>(FD_SETSIZE - 2)) {
                close_socket(accepted);
                log("connection rejected: server_full");
                continue;
            }

            if (!set_nonblocking(accepted)) {
                close_socket(accepted);
                continue;
            }

            Client client;
            client.sock = accepted;
            client.id = make_id("client");
            client.udpToken = make_id("udp");
            const std::string clientId = client.id;
            mClients.emplace(clientId, std::move(client));
        }
    }

    bool read_from_client(Client& client) {
        std::array<char, 4096> buffer{};
        size_t readThisTick = 0;
        while (true) {
            const int read = recv(client.sock, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (read > 0) {
                readThisTick += static_cast<size_t>(read);
                client.rxBuffer.append(buffer.data(), static_cast<size_t>(read));

                size_t newline = std::string::npos;
                while ((newline = client.rxBuffer.find('\n')) != std::string::npos) {
                    if (newline > kMaxLineBytes) {
                        client.rxBuffer.clear();
                        reject_and_close(client, "message_too_large");
                        return true;
                    }
                    std::string line = client.rxBuffer.substr(0, newline);
                    client.rxBuffer.erase(0, newline + 1);
                    if (line.empty()) {
                        continue;
                    }

                    try {
                        route_message(client, json::parse(line));
                    } catch (const json::exception&) {
                        send_error(client, "invalid_json");
                    }
                    if (client.closeAfterFlush || client.disconnectRequested) {
                        return true;
                    }
                }
                if (client.rxBuffer.size() > kMaxLineBytes) {
                    client.rxBuffer.clear();
                    reject_and_close(client, "message_too_large");
                    return true;
                }
                if (readThisTick >= kMaxReadBytesPerTick) {
                    return true;
                }
                continue;
            }

            if (read == 0) {
                return false;
            }
            if (would_block()) {
                return true;
            }
            return false;
        }
    }

    void route_message(Client& client, const json& message) {
        if (!message.is_object()) {
            send_error(client, "invalid_message");
            return;
        }

        const std::string type = message.value("type", "");
        if (type == "hello") {
            if (!client.roomId.empty()) {
                send_error(client, "already_joined");
                return;
            }
            handle_hello(client, message);
            return;
        }

        if (client.roomId.empty()) {
            send_error(client, "expected_hello");
            return;
        }

        if (type == "ping") {
            send_json(client, {{"type", "pong"}, {"time", now_seconds()}});
            return;
        }

        if (type == "puppet_preference") {
            client.wantsPuppet = message.value("want_puppet", client.wantsPuppet);
            client.wantsMidna = message.value("want_midna", client.wantsMidna);
        } else if (type == "midna_preference") {
            client.wantsMidna = message.value("want_midna", client.wantsMidna);
        }
        if (type == "presence" || type == "progression_state") {
            client.stage = message.value("stage", client.stage);
        }

        if (type == "room_settings") {
            auto roomIt = mRooms.find(client.roomId);
            if (roomIt == mRooms.end()) {
                send_error(client, "lobby_not_found");
                return;
            }
            Room& room = roomIt->second;
            if (room.ownerClientId != client.id) {
                send_error(client, "owner_only");
                return;
            }
            const json settings = message.value("settings", json{});
            if (!apply_room_settings(room, settings)) {
                send_error(client, "invalid_settings");
                return;
            }

            const json routed = {
                {"type", "room_settings"},
                {"owner_client_id", room.ownerClientId},
                {"settings", room_settings_json(room)},
            };
            send_json(client, routed);
            broadcast(client, routed);
            return;
        }

        if (type == "pose") {
            json routed = {
                {"type", "pose"},
                {"client_id", client.id},
                {"sequence", message.value("sequence", 0U)},
                {"state", message.value("state", json::object())},
            };
            broadcast(client, routed);
            ++client.poseCount;
            if (mOptions.verbose && client.poseCount % 30 == 1) {
                const json state = message.value("state", json::object());
                std::cout << "pose client=" << client.id
                          << " seq=" << message.value("sequence", 0U)
                          << " stage=" << state.value("stage", "")
                          << " room=" << state.value("room", -1) << "\n";
            }
            return;
        }

        if (type == "reliable") {
            const uint32_t sequence = message.value("sequence", 0U);
            if (!client.reliableSeen.insert(sequence).second) {
                return;
            }
            client.reliableOrder.push_back(sequence);
            if (client.reliableOrder.size() > kMaxReliableSequences) {
                client.reliableSeen.erase(client.reliableOrder.front());
                client.reliableOrder.pop_front();
            }
            broadcast(client, {
                {"type", "reliable"},
                {"client_id", client.id},
                {"sequence", sequence},
                {"state", message.value("state", json::object())},
            });
            send_json(client, {{"type", "ack"}, {"sequence", sequence}});
            return;
        }

        if (type == "sync_request") {
            const std::string targetClientId = message.value("target_client_id", "");
            if (targetClientId.empty()) {
                send_error(client, "missing_target");
                return;
            }

            json routed = message;
            routed["client_id"] = client.id;
            if (!send_to_client(client, targetClientId, routed)) {
                send_error(client, "unknown_target");
            }
            return;
        }

        if (kGameplayRouteTypes.find(type) != kGameplayRouteTypes.end()) {
            json routed = message;
            routed["client_id"] = client.id;
            const std::string targetClientId = message.value("target_client_id", "");
            if (!targetClientId.empty()) {
                if (!send_to_client(client, targetClientId, routed)) {
                    send_error(client, "unknown_target");
                }
            } else {
                broadcast(client, routed);
            }
            return;
        }

        send_error(client, "unknown_message");
    }

    void handle_hello(Client& client, const json& hello) {
        if (hello.value("protocol_version", -1) != kProtocolVersion) {
            send_error(client, "protocol_version");
            return;
        }

        std::string roomId = trim(hello.value("room_id", ""));
        std::string name = trim(hello.value("name", ""));
        const std::string action = hello.value("action", "");
        const std::string password = hello.value("password", "");
        if (roomId.empty()) {
            send_error(client, "missing_lobby");
            return;
        }
        if (name.empty()) {
            send_error(client, "missing_username");
            return;
        }
        if (roomId.size() > kMaxRoomIdBytes) {
            send_error(client, "lobby_too_long");
            return;
        }
        if (name.size() > kMaxUsernameBytes) {
            send_error(client, "username_too_long");
            return;
        }
        if (password.size() > kMaxPasswordBytes) {
            send_error(client, "password_too_long");
            return;
        }
        if (password.size() < kMinPasswordBytes) {
            send_error(client, "password_too_short");
            return;
        }
        if (action != "create" && action != "join") {
            send_error(client, "invalid_action");
            return;
        }

        auto roomIt = mRooms.find(roomId);
        if (action == "create") {
            if (roomIt != mRooms.end()) {
                send_error(client, "lobby_exists");
                return;
            }
            Room room;
            room.id = roomId;
            room.password = password;
            room.ownerClientId = client.id;
            if (!apply_room_settings(room, hello.value("settings", json::object()))) {
                send_error(client, "invalid_settings");
                return;
            }
            roomIt = mRooms.emplace(roomId, std::move(room)).first;
        } else if (roomIt == mRooms.end()) {
            send_error(client, "lobby_not_found");
            return;
        } else if (roomIt->second.password != password) {
            send_error(client, "bad_password");
            return;
        }

        if (roomIt->second.clientIds.size() >= kMaxRoomClients) {
            send_error(client, "lobby_full");
            return;
        }

        client.roomId = roomId;
        client.name = name;
        client.wantsPuppet = hello.value("want_puppet", true);
        client.wantsMidna = hello.value("want_midna", false);

        json peers = json::array();
        for (const std::string& peerId : roomIt->second.clientIds) {
            const auto peerIt = mClients.find(peerId);
            if (peerIt != mClients.end()) {
                peers.push_back({{"client_id", peerIt->second.id}, {"name", peerIt->second.name}});
            }
        }

        roomIt->second.clientIds.push_back(client.id);
        log("join room=" + roomId + " client=" + client.id + " name=" + client.name);

        send_json(client, {
            {"type", "welcome"},
            {"protocol_version", kProtocolVersion},
            {"room_id", roomId},
            {"client_id", client.id},
            {"udp_token", client.udpToken},
            {"owner_client_id", roomIt->second.ownerClientId},
            {"settings", room_settings_json(roomIt->second)},
            {"peers", peers},
        });
        broadcast(client, {
            {"type", "peer_joined"},
            {"client_id", client.id},
            {"name", client.name},
        });
    }

    void broadcast(const Client& sender, const json& message) {
        const auto roomIt = mRooms.find(sender.roomId);
        if (roomIt == mRooms.end()) {
            return;
        }

        for (const std::string& peerId : roomIt->second.clientIds) {
            if (peerId == sender.id) {
                continue;
            }
            auto peerIt = mClients.find(peerId);
            if (peerIt != mClients.end()) {
                if (!send_json(peerIt->second, message)) {
                    peerIt->second.disconnectRequested = true;
                }
            }
        }
    }

    static std::string udp_sender_id(const UdpRelayHeader& header) {
        size_t length = 0;
        while (length < sizeof(header.senderId) && header.senderId[length] != '\0') {
            ++length;
        }
        return std::string(header.senderId, length);
    }

    static std::string udp_acked_sender_id(const UdpPoseAckPacket& ack) {
        size_t length = 0;
        while (length < sizeof(ack.ackedSenderId) && ack.ackedSenderId[length] != '\0') {
            ++length;
        }
        return std::string(ack.ackedSenderId, length);
    }

    static bool same_udp_endpoint(const sockaddr_in& left, const sockaddr_in& right) {
        return left.sin_family == right.sin_family &&
               left.sin_port == right.sin_port &&
               left.sin_addr.s_addr == right.sin_addr.s_addr;
    }

    void send_udp_to_client(Client& client, const uint8_t* bytes, size_t size) {
        if (!client.udpAddrKnown) {
            return;
        }
        sendto(mUdpSock, reinterpret_cast<const char*>(bytes), static_cast<int>(size), 0,
               reinterpret_cast<const sockaddr*>(&client.udpAddr), sizeof(client.udpAddr));
    }

    void receive_udp_datagrams() {
        std::array<uint8_t, kMaxUdpDatagramBytes> packet{};
        for (size_t receivedThisTick = 0; receivedThisTick < 512; ++receivedThisTick) {
            sockaddr_in from{};
#if _WIN32
            int fromLength = sizeof(from);
#else
            socklen_t fromLength = sizeof(from);
#endif
            const int received =
                recvfrom(mUdpSock, reinterpret_cast<char*>(packet.data()),
                         static_cast<int>(packet.size()), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received < 0) {
                return;
            }
            if (static_cast<size_t>(received) < sizeof(UdpRelayHeader)) {
                continue;
            }

            UdpRelayHeader header{};
            std::memcpy(&header, packet.data(), sizeof(header));
            const size_t packetSize = static_cast<size_t>(received);
            if (std::memcmp(header.magic, "DMPU", 4) != 0 || header.version != 1 ||
                header.headerSize != sizeof(UdpRelayHeader) ||
                sizeof(UdpRelayHeader) + header.payloadSize != packetSize)
            {
                continue;
            }

            const std::string senderId = udp_sender_id(header);
            auto senderIt = mClients.find(senderId);
            if (senderIt == mClients.end() || senderIt->second.roomId.empty()) {
                continue;
            }
            Client& sender = senderIt->second;
            const uint8_t* payload = packet.data() + sizeof(UdpRelayHeader);

            if (header.type == kUdpPacketTypeRelayRegister) {
                const std::string token(reinterpret_cast<const char*>(payload),
                                        header.payloadSize);
                if (token == sender.udpToken) {
                    const bool wasKnown = sender.udpAddrKnown;
                    const bool endpointChanged =
                        !wasKnown || !same_udp_endpoint(sender.udpAddr, from);
                    sender.udpAddr = from;
                    sender.udpAddrKnown = true;
                    if (!wasKnown) {
                        send_json(sender, {{"type", "udp_ready"}});
                    }
                    if (endpointChanged) {
                        log("udp register client=" + sender.id);
                    }
                }
                continue;
            }

            if (!sender.udpAddrKnown || !same_udp_endpoint(sender.udpAddr, from) ||
                (header.type != kUdpPacketTypePoseJson &&
                 header.type != kUdpPacketTypePoseMsgpack &&
                 header.type != kUdpPacketTypeRemoteObject &&
                 header.type != kUdpPacketTypeMidnaMsgpack &&
                 header.type != kUdpPacketTypePoseAck))
            {
                continue;
            }

            const auto now = SteadyClock::now();
            if (now - sender.udpRateWindowStarted >= std::chrono::seconds(1)) {
                sender.udpRateWindowStarted = now;
                sender.udpRateWindowBytes = 0;
            }
            if (packetSize > kMaxUdpBytesPerClientSecond -
                                 std::min(sender.udpRateWindowBytes,
                                          kMaxUdpBytesPerClientSecond))
            {
                continue;
            }
            sender.udpRateWindowBytes += packetSize;

            const auto roomIt = mRooms.find(sender.roomId);
            if (roomIt == mRooms.end()) {
                continue;
            }

            if (header.type == kUdpPacketTypePoseAck) {
                if (header.payloadSize != sizeof(UdpPoseAckPacket)) {
                    continue;
                }
                UdpPoseAckPacket ack{};
                std::memcpy(&ack, payload, sizeof(ack));
                const std::string targetId = udp_acked_sender_id(ack);
                auto targetIt = mClients.find(targetId);
                if (targetIt != mClients.end() &&
                    targetIt->second.roomId == sender.roomId)
                {
                    send_udp_to_client(targetIt->second, packet.data(), packetSize);
                }
                continue;
            }

            for (const std::string& peerId : roomIt->second.clientIds) {
                if (peerId == sender.id) {
                    continue;
                }
                auto peerIt = mClients.find(peerId);
                if (peerIt == mClients.end()) {
                    continue;
                }
                Client& peer = peerIt->second;
                if ((header.type == kUdpPacketTypePoseJson ||
                     header.type == kUdpPacketTypePoseMsgpack) &&
                    !peer.wantsPuppet)
                {
                    continue;
                }
                if (header.type == kUdpPacketTypeMidnaMsgpack &&
                    (!peer.wantsPuppet || !peer.wantsMidna))
                {
                    continue;
                }
                if ((header.type == kUdpPacketTypePoseJson ||
                     header.type == kUdpPacketTypePoseMsgpack ||
                     header.type == kUdpPacketTypeMidnaMsgpack) &&
                    !sender.stage.empty() && !peer.stage.empty() &&
                    sender.stage != peer.stage)
                {
                    continue;
                }
                send_udp_to_client(peer, packet.data(), packetSize);
            }
        }
    }

    bool send_to_client(const Client& sender, const std::string& targetClientId,
                        const json& message) {
        const auto roomIt = mRooms.find(sender.roomId);
        if (roomIt == mRooms.end() ||
            std::find(roomIt->second.clientIds.begin(), roomIt->second.clientIds.end(),
                      targetClientId) == roomIt->second.clientIds.end())
        {
            return false;
        }

        auto peerIt = mClients.find(targetClientId);
        if (peerIt == mClients.end()) {
            return false;
        }

        if (!send_json(peerIt->second, message)) {
            peerIt->second.disconnectRequested = true;
        }
        return true;
    }

    void remove_client(const std::string& clientId) {
        auto clientIt = mClients.find(clientId);
        if (clientIt == mClients.end()) {
            return;
        }

        const std::string roomId = clientIt->second.roomId;
        Client departed;
        departed.id = clientId;
        departed.roomId = roomId;
        close_socket(clientIt->second.sock);
        mClients.erase(clientIt);

        if (!roomId.empty()) {
            auto roomIt = mRooms.find(roomId);
            if (roomIt != mRooms.end()) {
                Room& room = roomIt->second;
                room.clientIds.erase(
                    std::remove(room.clientIds.begin(), room.clientIds.end(), clientId),
                    room.clientIds.end());
                log("leave room=" + roomId + " client=" + clientId);
                broadcast(departed, {{"type", "peer_left"}, {"client_id", clientId}});
                if (room.clientIds.empty()) {
                    mRooms.erase(roomIt);
                } else if (room.ownerClientId == clientId) {
                    room.ownerClientId = room.clientIds.front();
                    log("owner transfer room=" + roomId + " client=" + room.ownerClientId);
                    broadcast(departed, {
                        {"type", "owner_changed"},
                        {"owner_client_id", room.ownerClientId},
                    });
                }
            }
        }
    }

    bool send_json(Client& client, const json& message) {
        std::string bytes = message.dump();
        bytes.push_back('\n');
        trace_packet_tx(client.id, message, bytes.size());
        if (bytes.size() > kMaxQueuedBytes ||
            client.txQueuedBytes > kMaxQueuedBytes - bytes.size())
        {
            log("disconnect slow client=" + client.id + " queued_bytes=" +
                std::to_string(client.txQueuedBytes));
            client.disconnectRequested = true;
            return false;
        }
        client.txQueuedBytes += bytes.size();
        client.txQueue.push_back(std::move(bytes));
        return true;
    }

    bool flush_client(Client& client) {
        while (!client.txQueue.empty()) {
            const std::string& bytes = client.txQueue.front();
            const char* cursor = bytes.data() + client.txOffset;
            const size_t remainingBytes = bytes.size() - client.txOffset;
            const int sendBytes = static_cast<int>(
                std::min(remainingBytes, static_cast<size_t>(std::numeric_limits<int>::max())));
            const int sent = send(client.sock, cursor, sendBytes, kSendFlags);
            if (sent > 0) {
                client.txOffset += static_cast<size_t>(sent);
                client.txQueuedBytes -= static_cast<size_t>(sent);
                if (client.txOffset == bytes.size()) {
                    client.txQueue.pop_front();
                    client.txOffset = 0;
                }
                continue;
            }
            if (would_block()) {
                return true;
            }
            return false;
        }
        return true;
    }

    void send_error(Client& client, const std::string& error) {
        send_json(client, {{"type", "error"}, {"error", error}});
    }

    void reject_and_close(Client& client, const std::string& error) {
        send_error(client, error);
        client.closeAfterFlush = true;
        if (client.txQueue.empty()) {
            client.disconnectRequested = true;
        }
    }

    static bool set_nonblocking(socket_t sock) {
#if _WIN32
        u_long nonblocking = 1;
        return ioctlsocket(sock, FIONBIO, &nonblocking) == 0;
#else
        const int flags = fcntl(sock, F_GETFL, 0);
        return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    static void close_socket(socket_t& sock) {
        if (sock == INVALID_SOCKET) {
            return;
        }
#if _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        sock = INVALID_SOCKET;
    }

    static bool would_block() {
#if _WIN32
        const int err = WSAGetLastError();
        return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
    }

    static std::string make_id(std::string_view prefix) {
        static std::mt19937_64 rng{std::random_device{}()};
        const uint64_t value = rng();
        return std::string(prefix) + "_" + std::to_string(value);
    }

    static double now_seconds() {
        using clock = std::chrono::system_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    static std::string trim(const std::string& value) {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }).base();
        return first < last ? std::string(first, last) : std::string();
    }

    void log(const std::string& message) const {
        if (mOptions.verbose) {
            std::cout << message << "\n";
        }
    }

    Options mOptions;
    socket_t mListenSock = INVALID_SOCKET;
    socket_t mUdpSock = INVALID_SOCKET;
    std::map<std::string, Client> mClients;
    std::map<std::string, Room> mRooms;
#if _WIN32
    bool mWinsockStarted = false;
#endif
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--host") {
            options.host = next_value("--host");
        } else if (arg == "--public-host") {
            options.publicHost = next_value("--public-host");
        } else if (arg == "--port") {
            options.port = std::stoi(next_value("--port"));
        } else if (arg == "--public-port") {
            options.publicPort = std::stoi(next_value("--public-port"));
        } else if (arg == "--hello-timeout-ms") {
            const int timeoutMs = std::stoi(next_value("--hello-timeout-ms"));
            if (timeoutMs < 1) {
                std::cerr << "--hello-timeout-ms must be positive\n";
                std::exit(2);
            }
            options.helloTimeoutSeconds = static_cast<double>(timeoutMs) / 1000.0;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: tp_multiplayer_relay [--host 127.0.0.1] "
                         "[--port 34197] [--public-host 127.0.0.1] "
                         "[--public-port 34197] [--hello-timeout-ms 10000] [--verbose]\n";
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            std::exit(2);
        }
    }
    if (options.port < 1 || options.port > 65535) {
        std::cerr << "--port must be between 1 and 65535\n";
        std::exit(2);
    }
    if (options.publicHost.empty()) {
        std::cerr << "--public-host cannot be empty\n";
        std::exit(2);
    }
    if (options.publicPort == 0) {
        options.publicPort = options.port;
    }
    if (options.publicPort < 1 || options.publicPort > 65535) {
        std::cerr << "--public-port must be between 1 and 65535\n";
        std::exit(2);
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        dusk::multiplayer::InviteCodePayload endpoint;
        endpoint.transport = "relay";
        endpoint.host = options.publicHost;
        endpoint.port = options.publicPort;
        endpoint.room = "relay-endpoint";
        endpoint.sessionId = "relay";
        endpoint.sessionKey = "endpoint";
        std::cout << "Relay code: " << dusk::multiplayer::create_invite_code(endpoint)
                  << std::endl;

        Relay relay(options);
        return relay.run() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "invalid relay option: " << error.what() << "\n";
        return 2;
    }
}
