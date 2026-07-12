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
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxLineBytes = 512 * 1024;
constexpr size_t kMaxRoomClients = 8;

const std::set<std::string> kStateBroadcastTypes = {
    "event_bit",
    "tbox_bit",
    "switch_bit",
    "room_switch_bit",
    "item_bit",
    "dungeon_item_bit",
    "save_snapshot",
    "key_num",
    "light_drop_num",
    "max_life_update",
    "bottle_slots",
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
};

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
            std::strcmp(value, "ON") == 0);
}

bool env_disabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr &&
           (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
            std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
            std::strcmp(value, "OFF") == 0);
}

bool relay_packet_trace_enabled() {
    static const bool enabled = !env_disabled("DUSK_MP_RELAY_PACKET_TRACE");
    return enabled;
}

const char* packet_category(const std::string& type) {
    if (type == "pose") {
        return "pose";
    }
    if (type == "hello" || type == "welcome" || type == "peer_joined" ||
        type == "peer_left" || type == "name_labels")
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
    std::string rxBuffer;
    std::set<uint32_t> reliableSeen;
    uint32_t poseCount = 0;
};

struct Room {
    std::string id;
    std::string password;
    std::set<std::string> clientIds;
};

struct Options {
    std::string host = "127.0.0.1";
    int port = 34197;
    bool verbose = false;
};

class Relay {
public:
    explicit Relay(Options options) : mOptions(std::move(options)) {}

    bool run() {
#if _WIN32
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed\n";
            return false;
        }
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

        std::cout << "TP relay listening on " << mOptions.host << ":" << mOptions.port << "\n";
        while (true) {
            tick();
        }
    }

private:
    void tick() {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(mListenSock, &readfds);
        socket_t maxSock = mListenSock;

        for (const auto& entry : mClients) {
            FD_SET(entry.second.sock, &readfds);
            if (entry.second.sock > maxSock) {
                maxSock = entry.second.sock;
            }
        }

        timeval timeout{0, 100000};
#if _WIN32
        const int result = select(0, &readfds, nullptr, nullptr, &timeout);
#else
        const int result = select(maxSock + 1, &readfds, nullptr, nullptr, &timeout);
#endif
        if (result <= 0) {
            return;
        }

        if (FD_ISSET(mListenSock, &readfds)) {
            accept_client();
        }

        std::vector<std::string> disconnected;
        for (auto& entry : mClients) {
            Client& client = entry.second;
            if (FD_ISSET(client.sock, &readfds) && !read_from_client(client)) {
                disconnected.push_back(client.id);
            }
        }

        for (const std::string& clientId : disconnected) {
            remove_client(clientId);
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

            if (!set_nonblocking(accepted)) {
                close_socket(accepted);
                continue;
            }

            Client client;
            client.sock = accepted;
            client.id = make_id("client");
            const std::string clientId = client.id;
            mClients.emplace(clientId, std::move(client));
        }
    }

    bool read_from_client(Client& client) {
        std::array<char, 4096> buffer{};
        while (true) {
            const int read = recv(client.sock, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (read > 0) {
                client.rxBuffer.append(buffer.data(), static_cast<size_t>(read));
                if (client.rxBuffer.size() > kMaxLineBytes) {
                    send_error(client, "message_too_large");
                    return false;
                }

                size_t newline = std::string::npos;
                while ((newline = client.rxBuffer.find('\n')) != std::string::npos) {
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

        if (kStateBroadcastTypes.find(type) != kStateBroadcastTypes.end()) {
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
        const std::string password = hello.value("password", "");
        if (roomId.empty()) {
            send_error(client, "missing_lobby");
            return;
        }
        if (name.empty()) {
            send_error(client, "missing_username");
            return;
        }
        if (name.size() > 32) {
            name.resize(32);
        }

        auto roomIt = mRooms.find(roomId);
        if (roomIt == mRooms.end()) {
            Room room;
            room.id = roomId;
            room.password = password;
            roomIt = mRooms.emplace(roomId, std::move(room)).first;
        } else if (roomIt->second.password != password) {
            send_error(client, "bad_password");
            return;
        }

        if (roomIt->second.clientIds.size() >= kMaxRoomClients) {
            send_error(client, "lobby_full");
            return;
        }

        for (const std::string& peerId : roomIt->second.clientIds) {
            const auto peerIt = mClients.find(peerId);
            if (peerIt != mClients.end() && lower(peerIt->second.name) == lower(name)) {
                send_error(client, "duplicate_username");
                return;
            }
        }

        client.roomId = roomId;
        client.name = name;

        json peers = json::array();
        for (const std::string& peerId : roomIt->second.clientIds) {
            const auto peerIt = mClients.find(peerId);
            if (peerIt != mClients.end()) {
                peers.push_back({{"client_id", peerIt->second.id}, {"name", peerIt->second.name}});
            }
        }

        roomIt->second.clientIds.insert(client.id);
        log("join room=" + roomId + " client=" + client.id + " name=" + client.name);

        send_json(client, {
            {"type", "welcome"},
            {"protocol_version", kProtocolVersion},
            {"room_id", roomId},
            {"client_id", client.id},
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
                send_json(peerIt->second, message);
            }
        }
    }

    bool send_to_client(const Client& sender, const std::string& targetClientId,
                        const json& message) {
        const auto roomIt = mRooms.find(sender.roomId);
        if (roomIt == mRooms.end() ||
            roomIt->second.clientIds.find(targetClientId) == roomIt->second.clientIds.end())
        {
            return false;
        }

        auto peerIt = mClients.find(targetClientId);
        if (peerIt == mClients.end()) {
            return false;
        }

        send_json(peerIt->second, message);
        return true;
    }

    void remove_client(const std::string& clientId) {
        auto clientIt = mClients.find(clientId);
        if (clientIt == mClients.end()) {
            return;
        }

        const Client client = clientIt->second;
        close_socket(clientIt->second.sock);
        mClients.erase(clientIt);

        if (!client.roomId.empty()) {
            auto roomIt = mRooms.find(client.roomId);
            if (roomIt != mRooms.end()) {
                roomIt->second.clientIds.erase(clientId);
                log("leave room=" + client.roomId + " client=" + clientId);
                broadcast(client, {{"type", "peer_left"}, {"client_id", clientId}});
                if (roomIt->second.clientIds.empty()) {
                    mRooms.erase(roomIt);
                }
            }
        }
    }

    bool send_json(Client& client, const json& message) {
        std::string bytes = message.dump();
        bytes.push_back('\n');
        trace_packet_tx(client.id, message, bytes.size());
        const char* cursor = bytes.data();
        int remaining = static_cast<int>(bytes.size());
        while (remaining > 0) {
            const int sent = send(client.sock, cursor, remaining, kSendFlags);
            if (sent > 0) {
                cursor += sent;
                remaining -= sent;
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

    static std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    void log(const std::string& message) const {
        if (mOptions.verbose) {
            std::cout << message << "\n";
        }
    }

    Options mOptions;
    socket_t mListenSock = INVALID_SOCKET;
    std::map<std::string, Client> mClients;
    std::map<std::string, Room> mRooms;
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
        } else if (arg == "--port") {
            options.port = std::stoi(next_value("--port"));
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: tp_multiplayer_relay [--host 127.0.0.1] [--port 34197] [--verbose]\n";
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            std::exit(2);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    Relay relay(parse_options(argc, argv));
    return relay.run() ? 0 : 1;
}
