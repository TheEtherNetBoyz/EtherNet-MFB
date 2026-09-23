#pragma once

#include "SSystem/SComponent/c_xyz.h"

namespace dusk {

void UpdateLoadPositionOverlayInput();
void UpdateLoadPositionDriftNative();
bool GetLoggedRupeeSlidePosition(cXyz& position, s16& angle);
void DrawLoadPositionOverlayImGui();
void DrawLoadPositionOverlayNative();

}
