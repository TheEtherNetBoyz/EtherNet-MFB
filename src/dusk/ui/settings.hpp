#pragma once
#include "window.hpp"

namespace dusk::ui {

class SettingsWindow : public Window {
public:
    SettingsWindow(bool prelaunch = false);
    ~SettingsWindow() override;

    void update() override;
    void hide(bool close) override;

protected:
    bool mPrelaunch;
};

}  // namespace dusk::ui
