#include "ImGuiOnline.hpp"

#include "dusk/imgui/ImGuiConsole.hpp"
#include "dusk/config.hpp"
#include "dusk/multiplayer/multiplayer.hpp"
#include "dusk/settings.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <vector>

namespace dusk {
namespace {

void copy_string(char* dest, size_t destSize, const std::string& source) {
    if (destSize == 0) {
        return;
    }
    const size_t count = std::min(destSize - 1, source.size());
    std::memcpy(dest, source.data(), count);
    dest[count] = '\0';
}

void draw_status_row(const char* label, const std::string& value) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(115.0f);
    ImGuiStringViewText(value);
}

std::string connection_error_message(const std::string& error) {
    if (error == "bad_password") return "Incorrect lobby password.";
    if (error == "lobby_not_found") return "Lobby not found on this relay.";
    if (error == "lobby_exists") return "A lobby with that name already exists.";
    if (error == "lobby_full") return "This lobby is full.";
    if (error == "password_too_short") return "Password must be at least 6 characters.";
    if (error == "password_too_long") return "Password is too long.";
    if (error == "missing_lobby") return "Enter a lobby name.";
    if (error == "lobby_too_long") return "Lobby name is too long.";
    if (error == "missing_username") return "Enter a nickname.";
    if (error == "username_too_long") return "Nickname is too long.";
    if (error == "protocol_version") {
        return "This game build is incompatible with the relay version.";
    }
    if (error == "invalid_action") return "The relay rejected the host/join request.";
    if (error == "invalid_settings") return "The lobby settings were invalid.";
    if (error == "owner_only") return "Only the lobby host can change these settings.";
    if (error == "already_joined") return "This connection has already joined a lobby.";
    if (error == "hello_timeout") return "The relay timed out during connection setup.";
    if (error == "message_too_large") return "The relay rejected an oversized network message.";
    if (error == "Relay room vanished; recreating it") return error + ".";
    if (error == "connect failed" || error == "connect poll failed") {
        return "Could not connect to the server. Check the code, address, port, and firewall.";
    }
    if (error == "remote closed") return "The server closed the connection.";
    if (error == "send failed") return "The connection was lost while sending data.";
    if (error == "nonblocking failed") return "Windows could not initialize the network socket.";
    if (error == "listen failed") {
        return "Could not host on this port. It may already be in use.";
    }
    if (error == "invalid host" || error == "invalid bind host") {
        return "The server address could not be resolved.";
    }
    if (error == "relay handshake rejected") return "The relay rejected the connection.";
    return error;
}

void draw_error_row(const std::string& error) {
    ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f), "Error");
    ImGui::SameLine(115.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGuiStringViewText(connection_error_message(error));
    ImGui::PopTextWrapPos();
}

}  // namespace

void ImGuiOnline::loadRememberedNames() {
    const auto& settings = getSettings().online;
    copy_string(m_hostName, sizeof(m_hostName), settings.playerName.getValue());
    copy_string(m_joinName, sizeof(m_joinName), settings.playerName.getValue());
    copy_string(m_relayName, sizeof(m_relayName), settings.playerName.getValue());
    copy_string(m_room, sizeof(m_room), settings.lobbyName.getValue());
    copy_string(m_relayRoom, sizeof(m_relayRoom), settings.lobbyName.getValue());
    m_namesLoaded = true;
}

void ImGuiOnline::rememberPlayerName(const char* name) {
    auto& playerName = getSettings().online.playerName;
    playerName.setValue(name);
    copy_string(m_hostName, sizeof(m_hostName), name);
    copy_string(m_joinName, sizeof(m_joinName), name);
    copy_string(m_relayName, sizeof(m_relayName), name);
    config::Save();
}

void ImGuiOnline::rememberLobbyName(const char* name) {
    auto& lobbyName = getSettings().online.lobbyName;
    lobbyName.setValue(name);
    copy_string(m_room, sizeof(m_room), name);
    copy_string(m_relayRoom, sizeof(m_relayRoom), name);
    config::Save();
}

void ImGuiOnline::draw(bool& open) {
    if (!open) {
        return;
    }
    if (!m_namesLoaded) {
        loadRememberedNames();
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(460, 0), ImVec2(700, FLT_MAX));
    if (!ImGui::Begin("Online", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::End();
        return;
    }

    const auto status = multiplayer::get_session_status();
    const auto manualSyncStatus = multiplayer::get_manual_sync_request_status();
    const int manualSyncState = static_cast<int>(manualSyncStatus.state);
    if (manualSyncState != m_lastManualSyncRequestState) {
        m_lastManualSyncRequestState = manualSyncState;
        const std::string action =
            manualSyncStatus.flagsOnly ? "Flag sync" : "Sync + warp";
        if (manualSyncStatus.state == multiplayer::ManualSyncRequestState::Waiting) {
            m_statusMessage =
                action + " requested from " + manualSyncStatus.peerName + ". Waiting for response...";
        } else if (manualSyncStatus.state == multiplayer::ManualSyncRequestState::Succeeded) {
            m_statusMessage = action + " received from " + manualSyncStatus.peerName + ".";
        } else if (manualSyncStatus.state == multiplayer::ManualSyncRequestState::Failed) {
            m_statusMessage =
                action + " failed or timed out. Wait for cutscenes to finish, then retry.";
        }
    }
    draw_status_row("Mode", status.mode);
    draw_status_row("State", status.state);
    if (!status.connectionError.empty()) {
        draw_error_row(status.connectionError);
    }
    if (!status.enabled) {
        ImGui::Checkbox("Name labels", &m_nameLabels);
        ImGui::Checkbox("Sync flags", &m_syncFlags);
        ImGui::SameLine();
        ImGui::Checkbox("Remote Link model", &m_dummyModel);
        ImGui::SameLine();
        ImGui::Checkbox("Sync world", &m_syncWorld);
        if (!multiplayer::kRemoteMidnaStreamingEnabled) {
            m_displayMidna = false;
        }
        const bool midnaControlDisabled =
            !multiplayer::kRemoteMidnaStreamingEnabled || !m_dummyModel;
        if (midnaControlDisabled) {
            ImGui::BeginDisabled();
        }
        ImGui::Checkbox("Display Midna", &m_displayMidna);
        if (midnaControlDisabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!m_dummyModel) {
            ImGui::BeginDisabled();
        }
        ImGui::Checkbox("Remote collision", &m_remoteCollision);
        if (!m_dummyModel) {
            ImGui::EndDisabled();
        }
        if (!m_remoteCollision) {
            m_pvp = false;
        }
        if (!m_remoteCollision) {
            ImGui::BeginDisabled();
        }
        ImGui::Checkbox("PvP", &m_pvp);
        if (!m_remoteCollision) {
            ImGui::EndDisabled();
        }
    }
    if (status.enabled) {
        draw_status_row("Room", status.room);
        draw_status_row("Player", status.name);
        if (status.mode == "relay" && !status.ownerClientId.empty()) {
            draw_status_row("Role", status.isOwner ? "lobby host" : "player");
        }
        ImGui::Text("Port");
        ImGui::SameLine(115.0f);
        ImGui::Text("%d", status.port);
        ImGui::Text("Peer pose");
        ImGui::SameLine(115.0f);
        ImGui::TextUnformatted(status.hasRecentPeerPose ? "recent" : "waiting");

        bool nameLabels = status.nameLabels;
        if (status.nameLabelsHostControlled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Name labels", &nameLabels) && !status.nameLabelsHostControlled) {
            multiplayer::set_name_labels_enabled(nameLabels);
            m_nameLabels = nameLabels;
        }
        if (status.nameLabelsHostControlled) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(host controlled)");
        }

        bool syncFlags = status.syncFlags;
        if (status.syncFlagsHostControlled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Sync flags", &syncFlags) && !status.syncFlagsHostControlled) {
            multiplayer::set_sync_flags_enabled(syncFlags);
            m_syncFlags = syncFlags;
        }
        if (status.syncFlagsHostControlled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        bool dummyModel = status.dummyModel;
        if (status.dummyModelHostControlled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Remote Link model##active", &dummyModel) &&
            !status.dummyModelHostControlled)
        {
            multiplayer::set_remote_link_model_enabled(dummyModel);
            m_dummyModel = dummyModel;
        }
        if (status.dummyModelHostControlled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        bool syncWorld = status.syncWorld;
        // Future preset hook: this is the real-object/world-state lane, kept
        // separate from visual puppets and progression flags.
        if (status.syncWorldHostControlled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Sync world", &syncWorld) && !status.syncWorldHostControlled) {
            multiplayer::set_sync_world_enabled(syncWorld);
            m_syncWorld = syncWorld;
        }
        if (status.syncWorldHostControlled) {
            ImGui::EndDisabled();
        }
        bool displayMidna =
            multiplayer::kRemoteMidnaStreamingEnabled && status.displayMidna;
        const bool midnaControlDisabled =
            !multiplayer::kRemoteMidnaStreamingEnabled || !status.dummyModel;
        if (midnaControlDisabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Display Midna", &displayMidna) &&
            multiplayer::kRemoteMidnaStreamingEnabled)
        {
            multiplayer::set_display_remote_midna_enabled(displayMidna);
            m_displayMidna = displayMidna;
        }
        if (midnaControlDisabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        bool remoteCollision = status.remoteCollision;
        if (!status.dummyModel || status.remoteCollisionHostControlled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Remote collision", &remoteCollision)) {
            multiplayer::set_remote_collision_enabled(remoteCollision);
            m_remoteCollision = remoteCollision;
        }
        if (!status.dummyModel || status.remoteCollisionHostControlled) {
            ImGui::EndDisabled();
        }
        bool pvp = status.pvp;
        if (!status.remoteCollision || status.pvpHostControlled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("PvP", &pvp) && status.remoteCollision && !status.pvpHostControlled) {
            multiplayer::set_pvp_enabled(pvp);
            m_pvp = pvp;
        }
        if (!status.remoteCollision || status.pvpHostControlled) {
            ImGui::EndDisabled();
        }
        if (!status.remoteCollision) {
            m_pvp = false;
        }

        const auto players = multiplayer::get_player_list();
        std::vector<const multiplayer::PlayerListEntry*> syncPeers;
        std::vector<const char*> syncPeerLabels;
        for (const multiplayer::PlayerListEntry& player : players) {
            if (!player.local) {
                syncPeers.push_back(&player);
                syncPeerLabels.push_back(player.name.c_str());
            }
        }
        if (m_syncPeerIndex >= static_cast<int>(syncPeers.size())) {
            m_syncPeerIndex = 0;
        }

        if (syncPeers.empty()) {
            ImGui::BeginDisabled();
        }
        ImGui::SetNextItemWidth(190.0f);
        ImGui::Combo("Sync From", &m_syncPeerIndex, syncPeerLabels.data(),
                     static_cast<int>(syncPeerLabels.size()));
        if (ImGui::Button("Sync + Warp")) {
            std::string error;
            const std::string peerId = syncPeers[m_syncPeerIndex]->id;
            const std::string peerName = syncPeers[m_syncPeerIndex]->name;
            if (multiplayer::request_manual_sync(peerId, &error)) {
                m_statusMessage =
                    error.empty() ? "Sync + warp requested from " + peerName + "." : error;
            } else {
                m_statusMessage = "Sync + warp failed: " + error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Sync Flags")) {
            std::string error;
            const std::string peerId = syncPeers[m_syncPeerIndex]->id;
            const std::string peerName = syncPeers[m_syncPeerIndex]->name;
            if (multiplayer::request_manual_flags_sync(peerId, &error)) {
                m_statusMessage =
                    error.empty() ? "Flag sync requested from " + peerName + "." : error;
            } else {
                m_statusMessage = "Flag sync failed: " + error;
            }
        }
        if (syncPeers.empty()) {
            ImGui::EndDisabled();
        }
    }

    if (!m_statusMessage.empty()) {
        ImGui::Separator();
        ImGuiStringViewText(m_statusMessage);
    }

    if (!status.inviteCode.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(status.mode == "relay" ? "Relay code" : "Invite code");
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGuiStringViewText(status.inviteCode);
        ImGui::PopTextWrapPos();
        if (ImGui::Button(status.mode == "relay" ? "Copy Relay Code" : "Copy Invite Code")) {
            ImGui::SetClipboardText(status.inviteCode.c_str());
            m_statusMessage =
                status.mode == "relay" ? "Relay code copied." : "Invite code copied.";
        }
    }

    ImGui::Separator();
    if (ImGui::BeginTabBar("OnlineModeTabs")) {
        if (ImGui::BeginTabItem("Direct")) {
            if (ImGui::CollapsingHeader("Host", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Name##hostName", m_hostName, sizeof(m_hostName));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    rememberPlayerName(m_hostName);
                }
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Room", m_room, sizeof(m_room));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    rememberLobbyName(m_room);
                }
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Public Host", m_publicHost, sizeof(m_publicHost));
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputInt("Port", &m_port);

                if (ImGui::Button("Host Lobby")) {
                    multiplayer::DirectHostOptions options;
                    options.name = m_hostName;
                    options.room = m_room;
                    options.bindHost = m_bindHost;
                    options.publicHost = m_publicHost;
                    options.port = m_port;
                    options.dummyModel = m_dummyModel;
                    options.nameLabels = m_nameLabels;
                    options.syncFlags = m_syncFlags;
                    options.syncWorld = m_syncWorld;
                    options.displayMidna = m_displayMidna;
                    options.remoteCollision = m_remoteCollision;
                    options.pvp = m_pvp;

                    std::string error;
                    if (multiplayer::host_direct(options, &error)) {
                        m_statusMessage = "Lobby hosted. Share the invite code.";
                    } else {
                        m_statusMessage = "Host failed: " + error;
                    }
                }
            }

            ImGui::Separator();
            if (ImGui::CollapsingHeader("Join", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Name##joinName", m_joinName, sizeof(m_joinName));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    rememberPlayerName(m_joinName);
                }
                ImGui::TextUnformatted("Invite code");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextMultiline(
                    "##directInviteCode", m_inviteCode, sizeof(m_inviteCode),
                    ImVec2(0.0f, ImGui::GetTextLineHeight() * 3.0f));

                if (ImGui::Button("Join Lobby")) {
                    multiplayer::DirectJoinOptions options;
                    options.name = m_joinName;
                    options.inviteCode = m_inviteCode;
                    options.dummyModel = m_dummyModel;
                    options.nameLabels = m_nameLabels;
                    options.syncFlags = m_syncFlags;
                    options.syncWorld = m_syncWorld;
                    options.displayMidna = m_displayMidna;
                    options.remoteCollision = m_remoteCollision;
                    options.pvp = m_pvp;

                    std::string error;
                    if (multiplayer::join_direct(options, &error)) {
                        m_statusMessage = "Joining lobby.";
                    } else {
                        m_statusMessage = "Join failed: " + error;
                    }
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Relay")) {
            if (ImGui::CollapsingHeader("Host", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextUnformatted("Relay code");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextMultiline(
                    "##relayHostCode", m_relayCode, sizeof(m_relayCode),
                    ImVec2(0.0f, ImGui::GetTextLineHeight() * 3.0f));
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Nickname##relayHostName", m_relayName, sizeof(m_relayName));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    rememberPlayerName(m_relayName);
                }
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Lobby name##relayHostRoom", m_relayRoom, sizeof(m_relayRoom));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    rememberLobbyName(m_relayRoom);
                }
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Password##relayHostPassword", m_relayPassword,
                                 sizeof(m_relayPassword), ImGuiInputTextFlags_Password);
                ImGui::TextDisabled("At least 6 characters");

                if (ImGui::Button("Host Relay Lobby")) {
                    multiplayer::RelayHostOptions options;
                    options.name = m_relayName;
                    options.room = m_relayRoom;
                    options.password = m_relayPassword;
                    options.relayCode = m_relayCode;
                    options.dummyModel = m_dummyModel;
                    options.nameLabels = m_nameLabels;
                    options.syncFlags = m_syncFlags;
                    options.syncWorld = m_syncWorld;
                    options.displayMidna = m_displayMidna;
                    options.remoteCollision = m_remoteCollision;
                    options.pvp = m_pvp;

                    std::string error;
                    if (multiplayer::host_relay(options, &error)) {
                        m_statusMessage = "Creating relay lobby.";
                    } else {
                        m_statusMessage = "Relay host failed: " + error;
                    }
                }
            }

            ImGui::Separator();
            if (ImGui::CollapsingHeader("Join", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextUnformatted("Relay code");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextMultiline(
                    "##relayJoinCode", m_relayCode, sizeof(m_relayCode),
                    ImVec2(0.0f, ImGui::GetTextLineHeight() * 3.0f));
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Nickname##relayJoinName", m_relayName, sizeof(m_relayName));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    rememberPlayerName(m_relayName);
                }
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Lobby name##relayJoinRoom", m_relayRoom,
                                 sizeof(m_relayRoom));
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    rememberLobbyName(m_relayRoom);
                }
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Password##relayJoinPassword", m_relayPassword,
                                 sizeof(m_relayPassword), ImGuiInputTextFlags_Password);

                if (ImGui::Button("Join Relay Lobby")) {
                    multiplayer::RelayJoinOptions options;
                    options.name = m_relayName;
                    options.room = m_relayRoom;
                    options.password = m_relayPassword;
                    options.relayCode = m_relayCode;
                    options.dummyModel = m_dummyModel;
                    options.nameLabels = m_nameLabels;
                    options.syncFlags = m_syncFlags;
                    options.syncWorld = m_syncWorld;
                    options.displayMidna = m_displayMidna;
                    options.remoteCollision = m_remoteCollision;
                    options.pvp = m_pvp;

                    std::string error;
                    if (multiplayer::join_relay(options, &error)) {
                        m_statusMessage = "Joining relay lobby.";
                    } else {
                        m_statusMessage = "Relay join failed: " + error;
                    }
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (!status.enabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Disconnect")) {
        multiplayer::disconnect_session();
        m_statusMessage = "Disconnected.";
    }
    if (!status.enabled) {
        ImGui::EndDisabled();
    }

    if (status.enabled && status.mode == "host" && !status.inviteCode.empty()) {
        copy_string(m_inviteCode, sizeof(m_inviteCode), status.inviteCode);
    }
    if (status.enabled && status.mode == "relay" && !status.inviteCode.empty()) {
        copy_string(m_relayCode, sizeof(m_relayCode), status.inviteCode);
    }

    ImGui::End();
}

}  // namespace dusk
