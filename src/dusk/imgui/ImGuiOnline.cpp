#include "ImGuiOnline.hpp"

#include "dusk/imgui/ImGuiConsole.hpp"
#include "dusk/multiplayer/multiplayer.hpp"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

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

}  // namespace

void ImGuiOnline::draw(bool& open) {
    if (!open) {
        return;
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(460, 0), ImVec2(700, FLT_MAX));
    if (!ImGui::Begin("Online", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::End();
        return;
    }

    const auto status = multiplayer::get_session_status();
    draw_status_row("Mode", status.mode);
    draw_status_row("State", status.state);
    if (status.enabled) {
        draw_status_row("Room", status.room);
        draw_status_row("Player", status.name);
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
    }

    if (!m_statusMessage.empty()) {
        ImGui::Separator();
        ImGuiStringViewText(m_statusMessage);
    }

    if (!status.inviteCode.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Invite code");
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGuiStringViewText(status.inviteCode);
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Copy Invite Code")) {
            ImGui::SetClipboardText(status.inviteCode.c_str());
            m_statusMessage = "Invite code copied.";
        }
    }

    ImGui::Separator();
    if (ImGui::BeginTabBar("OnlineModeTabs")) {
        if (ImGui::BeginTabItem("Direct")) {
            if (ImGui::CollapsingHeader("Host", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Name##hostName", m_hostName, sizeof(m_hostName));
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Room", m_room, sizeof(m_room));
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Bind", m_bindHost, sizeof(m_bindHost));
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputText("Public Host", m_publicHost, sizeof(m_publicHost));
                ImGui::SetNextItemWidth(190.0f);
                ImGui::InputInt("Port", &m_port);
                ImGui::Checkbox("Debug marker", &m_debugMarker);
                ImGui::SameLine();
                ImGui::Checkbox("Remote Link model", &m_dummyModel);
                ImGui::SameLine();
                ImGui::Checkbox("Name labels", &m_nameLabels);

                if (ImGui::Button("Host Lobby")) {
                    multiplayer::DirectHostOptions options;
                    options.name = m_hostName;
                    options.room = m_room;
                    options.bindHost = m_bindHost;
                    options.publicHost = m_publicHost;
                    options.port = m_port;
                    options.debugMarker = m_debugMarker;
                    options.dummyModel = m_dummyModel;
                    options.nameLabels = m_nameLabels;

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
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextMultiline("Invite Code", m_inviteCode, sizeof(m_inviteCode), ImVec2(0.0f, ImGui::GetTextLineHeight() * 3.0f));
                ImGui::Checkbox("Debug marker##join", &m_debugMarker);
                ImGui::SameLine();
                ImGui::Checkbox("Remote Link model##join", &m_dummyModel);
                ImGui::SameLine();
                ImGui::Checkbox("Name labels##join", &m_nameLabels);

                if (ImGui::Button("Join Lobby")) {
                    multiplayer::DirectJoinOptions options;
                    options.name = m_joinName;
                    options.inviteCode = m_inviteCode;
                    options.debugMarker = m_debugMarker;
                    options.dummyModel = m_dummyModel;
                    options.nameLabels = m_nameLabels;

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
            ImGui::SetNextItemWidth(190.0f);
            ImGui::InputText("Username##relayName", m_relayName, sizeof(m_relayName));
            ImGui::SetNextItemWidth(190.0f);
            ImGui::InputText("Lobby", m_relayRoom, sizeof(m_relayRoom));
            ImGui::SetNextItemWidth(190.0f);
            ImGui::InputText("Password", m_relayPassword, sizeof(m_relayPassword), ImGuiInputTextFlags_Password);
            ImGui::SetNextItemWidth(190.0f);
            ImGui::InputText("Relay Host", m_relayHost, sizeof(m_relayHost));
            ImGui::SetNextItemWidth(190.0f);
            ImGui::InputInt("Relay Port", &m_relayPort);
            ImGui::Checkbox("Debug marker##relay", &m_debugMarker);
            ImGui::SameLine();
            ImGui::Checkbox("Remote Link model##relay", &m_dummyModel);
            ImGui::SameLine();
            ImGui::Checkbox("Name labels##relay", &m_nameLabels);

            if (ImGui::Button("Join Relay Lobby")) {
                multiplayer::RelayJoinOptions options;
                options.name = m_relayName;
                options.room = m_relayRoom;
                options.password = m_relayPassword;
                options.host = m_relayHost;
                options.port = m_relayPort;
                options.debugMarker = m_debugMarker;
                options.dummyModel = m_dummyModel;
                options.nameLabels = m_nameLabels;

                std::string error;
                if (multiplayer::join_relay(options, &error)) {
                    m_statusMessage = "Joining relay lobby.";
                } else {
                    m_statusMessage = "Relay join failed: " + error;
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

    ImGui::End();
}

}  // namespace dusk
