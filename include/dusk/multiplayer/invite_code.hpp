#pragma once

#include <optional>
#include <string>

namespace dusk::multiplayer {

struct InviteCodePayload {
    int version = 1;
    std::string transport = "direct";
    std::string host = "127.0.0.1";
    int port = 34197;
    std::string room = "dev";
    std::string sessionId;
    std::string sessionKey;
};

std::string create_invite_code(const InviteCodePayload& payload);
std::optional<InviteCodePayload> decode_invite_code(const std::string& code, std::string* errorOut);
std::string make_session_token(int bytes);

}  // namespace dusk::multiplayer

