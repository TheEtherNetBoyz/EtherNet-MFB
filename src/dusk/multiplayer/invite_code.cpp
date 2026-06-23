#include "dusk/multiplayer/invite_code.hpp"

#include "nlohmann/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

namespace dusk::multiplayer {
namespace {

using json = nlohmann::json;

constexpr std::string_view kPrefix = "TP1-";
constexpr std::string_view kDevSignatureSecret = "tp-multiplayer-dev-invite-secret";
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr uint8_t kCompactVersion = 1;
constexpr uint8_t kCompactTransportDirect = 1;

std::string base64url_encode(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);

    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        if (i + 1 < size) {
            out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        }
        if (i + 2 < size) {
            out.push_back(kAlphabet[triple & 0x3F]);
        }
    }

    return out;
}

int decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

std::optional<std::vector<uint8_t>> base64url_decode(std::string_view text) {
    std::vector<uint8_t> out;
    out.reserve((text.size() * 3) / 4);

    uint32_t accumulator = 0;
    int bits = 0;
    for (char c : text) {
        const int value = decode_char(c);
        if (value < 0) {
            return std::nullopt;
        }

        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }

    return out;
}

uint64_t signature_for_bytes(const uint8_t* payload, size_t size) {
    uint64_t signature = kFnvOffset;
    for (char c : kDevSignatureSecret) {
        signature ^= static_cast<uint8_t>(c);
        signature *= kFnvPrime;
    }
    for (size_t i = 0; i < size; ++i) {
        signature ^= payload[i];
        signature *= kFnvPrime;
    }

    return signature;
}

std::string sign_payload(std::string_view payload) {
    const uint64_t signature =
        signature_for_bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());

    std::array<uint8_t, sizeof(signature)> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>((signature >> ((bytes.size() - 1 - i) * 8)) & 0xFF);
    }
    return base64url_encode(bytes.data(), bytes.size());
}

void append_u16(std::vector<uint8_t>& bytes, int value) {
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u64(std::vector<uint8_t>& bytes, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

bool append_string(std::vector<uint8_t>& bytes, const std::string& value) {
    if (value.size() > 255) {
        return false;
    }

    bytes.push_back(static_cast<uint8_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
}

bool parse_ipv4(const std::string& host, std::array<uint8_t, 4>& out) {
    size_t offset = 0;
    for (size_t part = 0; part < out.size(); ++part) {
        if (offset >= host.size()) {
            return false;
        }

        int value = 0;
        size_t digits = 0;
        while (offset < host.size() && host[offset] >= '0' && host[offset] <= '9') {
            value = value * 10 + (host[offset] - '0');
            if (value > 255 || ++digits > 3) {
                return false;
            }
            ++offset;
        }

        if (digits == 0) {
            return false;
        }

        out[part] = static_cast<uint8_t>(value);
        if (part + 1 < out.size()) {
            if (offset >= host.size() || host[offset] != '.') {
                return false;
            }
            ++offset;
        }
    }

    return offset == host.size();
}

std::optional<std::string> create_compact_invite_code(const InviteCodePayload& payload) {
    if (payload.version != 1 || payload.transport != "direct" || payload.port <= 0 ||
        payload.port > 65535) {
        return std::nullopt;
    }

    std::array<uint8_t, 4> ip{};
    if (!parse_ipv4(payload.host, ip)) {
        return std::nullopt;
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(64 + payload.room.size() + payload.sessionId.size() + payload.sessionKey.size());
    bytes.push_back(kCompactVersion);
    bytes.push_back(kCompactTransportDirect);
    bytes.insert(bytes.end(), ip.begin(), ip.end());
    append_u16(bytes, payload.port);
    if (!append_string(bytes, payload.room) || !append_string(bytes, payload.sessionId) ||
        !append_string(bytes, payload.sessionKey)) {
        return std::nullopt;
    }

    append_u64(bytes, signature_for_bytes(bytes.data(), bytes.size()));
    return std::string(kPrefix.data(), kPrefix.size()) + base64url_encode(bytes.data(), bytes.size());
}

std::string create_legacy_invite_code(const InviteCodePayload& payload) {
    json object = {
        {"version", payload.version},
        {"transport", payload.transport},
        {"host", payload.host},
        {"port", payload.port},
        {"room", payload.room},
        {"session_id", payload.sessionId},
        {"session_key", payload.sessionKey},
    };

    const std::string bytes = object.dump();
    return std::string(kPrefix.data(), kPrefix.size()) +
           base64url_encode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) + "." +
           sign_payload(bytes);
}

uint16_t read_u16(const std::vector<uint8_t>& bytes, size_t& offset) {
    const uint16_t value =
        (static_cast<uint16_t>(bytes[offset]) << 8) | static_cast<uint16_t>(bytes[offset + 1]);
    offset += 2;
    return value;
}

uint64_t read_u64(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value = (value << 8) | bytes[offset + i];
    }
    return value;
}

std::optional<std::string> read_string(const std::vector<uint8_t>& bytes, size_t& offset) {
    if (offset >= bytes.size()) {
        return std::nullopt;
    }

    const size_t length = bytes[offset++];
    if (offset + length > bytes.size()) {
        return std::nullopt;
    }

    std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
    return value;
}

void set_error(std::string* errorOut, const char* error) {
    if (errorOut != nullptr) {
        *errorOut = error;
    }
}

}  // namespace

std::string make_session_token(int bytes) {
    std::vector<uint8_t> data(static_cast<size_t>(bytes));
    std::random_device random;
    for (uint8_t& byte : data) {
        byte = static_cast<uint8_t>(random());
    }
    return base64url_encode(data.data(), data.size());
}

std::string create_invite_code(const InviteCodePayload& payload) {
    if (std::optional<std::string> compact = create_compact_invite_code(payload)) {
        return *compact;
    }

    return create_legacy_invite_code(payload);
}

std::optional<InviteCodePayload> decode_invite_code(const std::string& code, std::string* errorOut) {
    if (!code.starts_with(kPrefix)) {
        set_error(errorOut, "invalid prefix");
        return std::nullopt;
    }

    const std::string_view body(code.data() + kPrefix.size(), code.size() - kPrefix.size());
    const size_t dot = body.find('.');
    if (dot == std::string_view::npos) {
        const std::optional<std::vector<uint8_t>> bytes = base64url_decode(body);
        if (!bytes) {
            set_error(errorOut, "invalid payload encoding");
            return std::nullopt;
        }

        constexpr size_t kSignatureSize = sizeof(uint64_t);
        if (bytes->size() < 1 + 1 + 4 + 2 + 1 + 1 + 1 + kSignatureSize) {
            set_error(errorOut, "compact payload too short");
            return std::nullopt;
        }

        const size_t payloadSize = bytes->size() - kSignatureSize;
        const uint64_t expected = signature_for_bytes(bytes->data(), payloadSize);
        const uint64_t actual = read_u64(*bytes, payloadSize);
        if (actual != expected) {
            set_error(errorOut, "invalid signature");
            return std::nullopt;
        }

        size_t offset = 0;
        const uint8_t version = (*bytes)[offset++];
        const uint8_t transport = (*bytes)[offset++];
        if (version != kCompactVersion || transport != kCompactTransportDirect) {
            set_error(errorOut, "unsupported compact payload");
            return std::nullopt;
        }

        InviteCodePayload payload;
        payload.version = version;
        payload.transport = "direct";
        payload.host = std::to_string((*bytes)[offset]) + "." +
                       std::to_string((*bytes)[offset + 1]) + "." +
                       std::to_string((*bytes)[offset + 2]) + "." +
                       std::to_string((*bytes)[offset + 3]);
        offset += 4;
        payload.port = read_u16(*bytes, offset);

        std::optional<std::string> room = read_string(*bytes, offset);
        std::optional<std::string> sessionId = read_string(*bytes, offset);
        std::optional<std::string> sessionKey = read_string(*bytes, offset);
        if (!room || !sessionId || !sessionKey || offset != payloadSize || payload.port <= 0) {
            set_error(errorOut, "invalid compact payload");
            return std::nullopt;
        }

        payload.room = *room;
        payload.sessionId = *sessionId;
        payload.sessionKey = *sessionKey;
        return payload;
    }

    const std::string_view payloadToken = body.substr(0, dot);
    const std::string_view signature = body.substr(dot + 1);
    const std::optional<std::vector<uint8_t>> payloadBytes = base64url_decode(payloadToken);
    if (!payloadBytes) {
        set_error(errorOut, "invalid payload encoding");
        return std::nullopt;
    }

    const std::string payloadString(payloadBytes->begin(), payloadBytes->end());
    if (signature != sign_payload(payloadString)) {
        set_error(errorOut, "invalid signature");
        return std::nullopt;
    }

    try {
        const json object = json::parse(payloadString);
        InviteCodePayload payload;
        payload.version = object.value("version", 0);
        payload.transport = object.value("transport", "");
        payload.host = object.value("host", "");
        payload.port = object.value("port", 0);
        payload.room = object.value("room", "");
        payload.sessionId = object.value("session_id", "");
        payload.sessionKey = object.value("session_key", "");

        if (payload.version != 1 || payload.transport.empty() || payload.host.empty() ||
            payload.port <= 0 || payload.port > 65535 || payload.sessionId.empty() ||
            payload.sessionKey.empty()) {
            set_error(errorOut, "missing required fields");
            return std::nullopt;
        }

        return payload;
    } catch (const json::exception&) {
        set_error(errorOut, "invalid payload json");
        return std::nullopt;
    }
}

}  // namespace dusk::multiplayer
