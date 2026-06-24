#ifndef DUSK_IMGUI_ONLINE_HPP
#define DUSK_IMGUI_ONLINE_HPP

#include <string>

namespace dusk {

class ImGuiOnline {
public:
    void draw(bool& open);

private:
    char m_hostName[64] = "Host";
    char m_joinName[64] = "Joiner";
    char m_room[64] = "dev";
    char m_bindHost[64] = "0.0.0.0";
    char m_publicHost[128] = "127.0.0.1";
    char m_inviteCode[512] = "";
    int m_port = 34197;
    bool m_debugMarker = true;
    bool m_dummyModel = true;
    std::string m_statusMessage;
};

}  // namespace dusk

#endif  // DUSK_IMGUI_ONLINE_HPP
