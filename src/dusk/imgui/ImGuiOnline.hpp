#ifndef DUSK_IMGUI_ONLINE_HPP
#define DUSK_IMGUI_ONLINE_HPP

#include <string>

namespace dusk {

class ImGuiOnline {
public:
    void draw(bool& open);

private:
    void loadRememberedNames();
    void rememberPlayerName(const char* name);
    void rememberLobbyName(const char* name);

    char m_hostName[64] = "Player";
    char m_joinName[64] = "Player";
    char m_room[64] = "Lobby";
    char m_bindHost[64] = "0.0.0.0";
    char m_publicHost[128] = "127.0.0.1";
    char m_inviteCode[512] = "";
    char m_relayName[64] = "Player";
    char m_relayRoom[64] = "Lobby";
    char m_relayPassword[128] = "";
    char m_relayCode[512] = "";
    int m_port = 34197;
    bool m_dummyModel = true;
    bool m_nameLabels = true;
    bool m_syncFlags = true;
    bool m_syncWorld = false;
    bool m_displayMidna = true;
    bool m_remoteCollision = true;
    bool m_pvp = false;
    bool m_relayLocal = false;
    int m_syncPeerIndex = 0;
    int m_lastManualSyncRequestState = -1;
    bool m_namesLoaded = false;
    std::string m_statusMessage;
};

}  // namespace dusk

#endif  // DUSK_IMGUI_ONLINE_HPP
