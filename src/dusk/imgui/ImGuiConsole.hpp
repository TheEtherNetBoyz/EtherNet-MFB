#ifndef DUSK_IMGUI_HPP
#define DUSK_IMGUI_HPP
#include "ImGuiMenuTools.hpp"

#include "dusk/main.h"

#include <aurora/aurora.h>
#include <imgui.h>

#include "dusk/settings.h"
#include <deque>
#include <string>
#include <string_view>

union SDL_Event;
struct ImGuiWindow;

namespace dusk {
class ImGuiConsole {
public:
    ImGuiConsole();
    void HandleSDLEvent(const SDL_Event& event);
    void UpdateSettings();
    void PreDraw();
    void PostDraw();

    // Renders the practice-saves menu natively (J2D/GX) instead of through
    // imgui, so it scales with output resolution. Called from the game's 2D
    // draw pass (m_Do_graphic). No-ops when the menu is closed.
    void DrawPracticeSavesNative();

    static bool CheckMenuViewToggle(ImGuiKey key, bool& active);
    static bool CheckMenuViewToggle(const UserSettings::HotkeyBinding& binding, bool& active);
    void AddToast(std::string_view message, float duration = 3.f);

private:
    struct Toast {
        std::string message;
        float remain;
        float current = 0.f;
        Toast(std::string message, float duration) noexcept : message(std::move(message)),
                                                              remain(duration) {}
    };

    bool m_isHidden = true;
    bool m_isLaunchInitialized = false;
    ImGuiWindow* m_dragScrollWindow = nullptr;
    ImVec2 m_dragScrollLastMousePos = {};
    std::deque<Toast> m_toasts;

    // Keep always last
    ImGuiMenuTools m_menuTools;

    void ShowToasts();
    void ShowPipelineProgress();
    void UpdateDragScroll();
};

extern ImGuiConsole g_imguiConsole;

std::string_view backend_name(AuroraBackend backend);
std::string_view backend_id(AuroraBackend backend);
bool try_parse_backend(std::string_view backend, AuroraBackend& outBackend);
std::string BytesToString(size_t bytes);
void SetOverlayWindowLocation(int corner);
bool ShowCornerContextMenu(int& corner, int avoidCorner);
void ImGuiStringViewText(std::string_view text);
void DuskToast(std::string_view message, float duration = 3.f);
void ImGuiBeginGroupPanel(const char* name, const ImVec2& size);
void ImGuiEndGroupPanel();
void ImGuiTextCenter(std::string_view text);
bool ImGuiButtonCenter(std::string_view text);
float ImGuiScale();
}  // namespace dusk

#endif  // DUSK_IMGUI_HPP
