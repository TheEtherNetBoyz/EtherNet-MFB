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

    struct PracticeSavePlacement {
        cXyz pos;
        s16 angle;
    };

    struct PracticeSaveEntry {
        std::string name;
        std::string description;
        std::string filename;
        int index = 0;
        std::optional<PracticeSavePlacement> placement;
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
    bool loadPracticeSave(SaveCategory category, const PracticeSaveEntry& entry);
    bool loadPracticeSaveByIndex(SaveCategory category, int index, bool checkerSetup = false);
    void tickPendingApply();
    void executeGorgeVoidChecker();
    void drawGorgeVoidCheckerResult();
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
    std::optional<PracticeSavePlacement> m_pendingPlacement;
    int m_pendingPlayerInitFrames = 0;
    int m_pendingStageInitCallback = 0;
    int m_pendingPlayerInitCallback = 0;
    MainCategory m_mainCategory = MainCategory::Practice;
    SaveCategory m_saveCategory = SaveCategory::Any;
    bool m_focusSaveList = false;
    int m_selectedSave = 0;
    double m_nextControllerInputTime = 0.0;
    u32 m_lastControllerHold = 0;
    bool m_scrollSelectedSave = false;
    int m_selectedGenericRow = 0;
    bool m_scrollSelectedGenericRow = false;
    bool m_loaded = false;
    bool m_loadInProgress = false;
    bool m_loadPeekSeen = false;
    int m_pendingPlacementFrames = 0;

    struct GorgeVoidCheckerState {
        bool comboHeld = false;
        bool timerStarted = false;
        bool gotIt = false;
        int previousFrame = 0;
        int counterDifference = 0;
        int afterCsVal = 0;
        int resultTimer = 0;
        int resultColor = 0;
        char resultText[20] = {};
    };

    GorgeVoidCheckerState m_gorgeVoidChecker;
};

}

#endif
