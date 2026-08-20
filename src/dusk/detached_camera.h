#pragma once

#include "dolphin/types.h"

struct view_class;
struct cXyz;

namespace dusk::detached_camera {

void setEnabled(bool enabled);
bool isEnabled();

void setControlsEnabled(bool enabled);
bool areControlsEnabled();

void setMouseLookEnabled(bool enabled);
bool isMouseLookEnabled();

float getMoveSpeed();
void setMoveSpeed(float speed);

float getFov();
void setFov(float fov);

float getBankDegrees();
void setBankDegrees(float degrees);

void copyFromView(const view_class* view);
void focusOn(const cXyz& target, float distance = 300.0f);
void updateControls(float deltaSeconds);

void apply(view_class* view);
void restore();

}  // namespace dusk::detached_camera
