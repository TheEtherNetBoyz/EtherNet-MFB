#ifndef DUSK_IMGUI_PRACTICESAVES_HPP
#define DUSK_IMGUI_PRACTICESAVES_HPP

#include "d/d_save.h"

#include <optional>
#include <array>
#include <string>
#include <vector>

namespace dusk {

class ImGuiPracticeSaves {
public:
    void draw(bool& open);
    void suppressControllerInput();

    struct PracticeSaveEntry {
        std::string name;
        std::string description;
        std::string filename;
    };

    enum class MainCategory : int {
        Cheats,
        Flags,
        Inventory,
        Memory,
        Practice,
        Scene,
        Settings,
        Tools,
        Warping,
        Count,
    };

    enum class SaveCategory : int {
        Any,
        NoSq,
        Hundred,
        AllDungeons,
        Glitchless,
        Count,
    };

private:
    void loadMetadata();
    void loadCategoryMetadata(SaveCategory category);
    bool loadPracticeSave(const PracticeSaveEntry& entry);
    void tickPendingApply();
    void handleController(bool& open);
    void consumeControllerInput();
    void drawCategoryList();
    void drawPracticePanel(bool& open);
    void drawGenericPanel();
    std::vector<PracticeSaveEntry>& currentSaves();
    const std::vector<PracticeSaveEntry>& currentSaves() const;

    std::array<std::vector<PracticeSaveEntry>, static_cast<int>(SaveCategory::Count)> m_saves;
    std::string m_statusMsg;
    std::optional<dSv_save_c> m_pendingSavedata;
    std::optional<u8> m_pendingVibration;
    MainCategory m_mainCategory = MainCategory::Practice;
    SaveCategory m_saveCategory = SaveCategory::Any;
    bool m_focusSaveList = false;
    int m_selectedSave = 0;
    double m_nextControllerInputTime = 0.0;
    u32 m_lastControllerHold = 0;
    bool m_scrollSelectedSave = false;
    bool m_loaded = false;
    bool m_loadInProgress = false;
    bool m_loadPeekSeen = false;
};

}

#endif
