#pragma once

#include "dusk/custom_music/CustomMusicProject.h"

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dusk::custom_music {

enum class CustomMusicJobKind {
    None,
    ApplyReplacement,
    RandomizeSongs,
    CreateSnapshot,
    RestoreIso,
};

struct CustomMusicJobStatus {
    CustomMusicJobKind kind = CustomMusicJobKind::None;
    bool running = false;
    bool succeeded = false;
    float progress = 0.0f;
    std::string stage;
    std::string result;
    std::filesystem::path outputIso;
};

class CustomMusicService {
public:
    static CustomMusicService& instance();
    ~CustomMusicService();

    CustomMusicProject& project();
    const CustomMusicProject& project() const;
    void initialize();
    void saveProject();
    void importPath(const std::filesystem::path& path);
    void refreshLibrary();
    void saveSelectedSource();
    void packSelectedMidiToTprs();
    bool isStagedIsoActive() const;
    void switchToCleanIso();

    bool startApplyReplacement(bool replaceExisting);
    bool startRandomizeSongs(bool replaceExisting);
    bool startCreateSnapshot();
    bool startRestoreIso(const std::filesystem::path& sourceIso);
    void resetMusicRandomizer();
    void cancel();
    CustomMusicJobStatus status() const;
    std::vector<std::string> logLines() const;
    void clearLog();
    void report(const std::string& line);

    bool hasPendingIsoSwitch() const;
    std::filesystem::path pendingIsoSwitch() const;
    void acceptPendingIsoSwitch();
    void dismissPendingIsoSwitch();

private:
    CustomMusicService() = default;
    void launch(CustomMusicJobKind kind, std::function<void()> operation);
    void finish(bool success, const std::string& result, const std::filesystem::path& output = {});
    void log(const std::string& line);
    void setStage(const std::string& stage, float progress);
    std::filesystem::path projectPath() const;
    std::filesystem::path cleanSourceIso();

    CustomMusicProject m_project;
    mutable std::mutex m_mutex;
    CustomMusicJobStatus m_status;
    std::deque<std::string> m_log;
    std::atomic<bool> m_cancel{false};
    std::thread m_worker;
    bool m_initialized = false;
    std::filesystem::path m_pendingIsoSwitch;
};

}  // namespace dusk::custom_music
