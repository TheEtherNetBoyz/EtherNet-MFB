#include "imgui.h"

#include "ImGuiMenuTools.hpp"
#include "dusk/input_macro.h"
#include "dusk/io.hpp"
#include "dusk/main.h"

#include "absl/strings/escaping.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <exception>
#include <string>
#include <vector>

namespace dusk {
    using json = nlohmann::json;

    namespace {
        struct SavedMacroEntry {
            std::string name;
            std::string encoded;
        };

        constexpr auto kInputMacrosFilename = "input_macros.json";
        bool sLoaded = false;
        std::vector<SavedMacroEntry> sSavedMacros;
        std::string sStatusMessage;

        std::filesystem::path InputMacrosFilePath() {
            return ConfigPath / kInputMacrosFilename;
        }

        const char* StateName(input_macro::State state) {
            switch (state) {
            case input_macro::State::Recording:
                return "Recording";
            case input_macro::State::Playing:
                return "Playing";
            case input_macro::State::Idle:
            default:
                return "Idle";
            }
        }

        bool DecodeMacro(const std::string& encoded, std::string& decoded) {
            if (!absl::Base64Unescape(encoded, &decoded)) {
                return false;
            }
            return input_macro::validateSerializedRecording(decoded);
        }

        void SaveMacrosFile() {
            json j = json::array();
            for (const auto& macro : sSavedMacros) {
                j.push_back(json{{"name", macro.name}, {"data", macro.encoded}});
            }

            try {
                io::FileStream::WriteAllText(InputMacrosFilePath(), j.dump(2));
            } catch (const std::exception& e) {
                sStatusMessage = fmt::format("Failed to save macros: {}", e.what());
            }
        }

        void LoadMacrosFile() {
            sLoaded = true;
            const auto filePath = InputMacrosFilePath();
            if (!std::filesystem::exists(filePath)) {
                return;
            }

            try {
                const auto data = io::FileStream::ReadAllBytes(filePath);
                const auto j = json::parse(data);
                if (!j.is_array()) {
                    return;
                }

                for (const auto& entry : j) {
                    if (!entry.contains("name") || !entry.contains("data")) {
                        continue;
                    }

                    SavedMacroEntry macro;
                    macro.name = entry["name"].get<std::string>();
                    macro.encoded = entry["data"].get<std::string>();
                    std::string decoded;
                    if (DecodeMacro(macro.encoded, decoded)) {
                        sSavedMacros.push_back(std::move(macro));
                    }
                }
            } catch (const std::exception& e) {
                sStatusMessage = fmt::format("Failed to load macros: {}", e.what());
            }
        }
    }

    void ImGuiMenuTools::ShowInputMacro() {
        if (!m_showInputMacro) {
            return;
        }

        if (ImGui::Begin("Input Macro", &m_showInputMacro)) {
            const auto state = input_macro::state();
            const bool recording = state == input_macro::State::Recording;
            const bool playing = state == input_macro::State::Playing;
            const bool hasRecording = input_macro::hasRecording();
            const bool busy = recording || playing;

            if (!sLoaded) {
                LoadMacrosFile();
            }

            ImGui::Text("State: %s", StateName(state));
            ImGui::Text("Frames: %zu", input_macro::recordedFrames());
            if (playing) {
                ImGui::Text("Playback: %zu", input_macro::playbackFrame());
            }

            bool loop = input_macro::looping();
            if (ImGui::Checkbox("Loop", &loop)) {
                input_macro::setLooping(loop);
            }

            ImGui::Separator();

            const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
            ImGui::BeginChild("##inputmacros", ImVec2(0, rowHeight * 5), true);
            if (sSavedMacros.empty()) {
                ImGui::TextDisabled("No saved macros.");
            }

            int toDelete = -1;
            for (int i = 0; i < static_cast<int>(sSavedMacros.size()); ++i) {
                ImGui::PushID(i);
                ImGui::Selectable(sSavedMacros[i].name.c_str(), false, ImGuiSelectableFlags_None, ImVec2(150, 0));

                ImGui::SameLine();
                if (busy) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Load")) {
                    std::string decoded;
                    if (DecodeMacro(sSavedMacros[i].encoded, decoded) &&
                        input_macro::loadSerializedRecording(decoded))
                    {
                        sStatusMessage = fmt::format("Loaded '{}'.", sSavedMacros[i].name);
                    } else {
                        sStatusMessage = fmt::format("'{}' is not a valid macro.", sSavedMacros[i].name);
                    }
                }
                if (busy) {
                    ImGui::EndDisabled();
                }

                ImGui::SameLine();
                if (ImGui::Button("Copy")) {
                    ImGui::SetClipboardText(sSavedMacros[i].encoded.c_str());
                    sStatusMessage = fmt::format("Copied '{}'.", sSavedMacros[i].name);
                }

                ImGui::SameLine();
                if (busy) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Del")) {
                    toDelete = i;
                }
                if (busy) {
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            if (toDelete >= 0) {
                sStatusMessage = fmt::format("Deleted '{}'.", sSavedMacros[toDelete].name);
                sSavedMacros.erase(sSavedMacros.begin() + toDelete);
                SaveMacrosFile();
            }
            ImGui::EndChild();

            if (!hasRecording || busy) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Save")) {
                const std::string data = input_macro::serializeRecording();
                if (data.empty()) {
                    sStatusMessage = "No macro is ready to save.";
                } else {
                    SavedMacroEntry macro;
                    macro.name = fmt::format("Macro {}", sSavedMacros.size() + 1);
                    macro.encoded = absl::Base64Escape(data);
                    sSavedMacros.push_back(std::move(macro));
                    SaveMacrosFile();
                    sStatusMessage = fmt::format("Saved '{}'.", sSavedMacros.back().name);
                }
            }
            if (!hasRecording || busy) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            if (busy) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Import Clipboard")) {
                const char* clip = ImGui::GetClipboardText();
                if (clip == nullptr || clip[0] == '\0') {
                    sStatusMessage = "Clipboard is empty.";
                } else {
                    std::string decoded;
                    std::string encoded = clip;
                    if (!DecodeMacro(encoded, decoded)) {
                        sStatusMessage = "Clipboard does not contain a valid macro.";
                    } else {
                        SavedMacroEntry macro;
                        macro.name = fmt::format("Imported {}", sSavedMacros.size() + 1);
                        macro.encoded = std::move(encoded);
                        sSavedMacros.push_back(std::move(macro));
                        SaveMacrosFile();
                        sStatusMessage = fmt::format("Imported '{}'.", sSavedMacros.back().name);
                    }
                }
            }
            if (busy) {
                ImGui::EndDisabled();
            }

            ImGui::Separator();

            if (recording) {
                if (ImGui::Button("Stop Recording")) {
                    input_macro::stopRecording();
                }
            } else {
                if (playing) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Start Recording")) {
                    input_macro::startRecording();
                }
                if (playing) {
                    ImGui::EndDisabled();
                }
            }

            ImGui::SameLine();
            if (playing) {
                if (ImGui::Button("Stop Playback")) {
                    input_macro::stopPlayback();
                }
            } else {
                if (!hasRecording || recording) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Play")) {
                    input_macro::startPlayback();
                }
                if (!hasRecording || recording) {
                    ImGui::EndDisabled();
                }
            }

            ImGui::SameLine();
            if (recording || playing || !hasRecording) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Clear")) {
                input_macro::clear();
            }
            if (recording || playing || !hasRecording) {
                ImGui::EndDisabled();
            }

            if (!sStatusMessage.empty()) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", sStatusMessage.c_str());
            }
        }

        ImGui::End();
    }
}
