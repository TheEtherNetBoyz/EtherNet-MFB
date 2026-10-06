#pragma once

#include "SSystem/SComponent/c_xyz.h"

namespace dusk {

void UpdateLoadPositionOverlayInput();
void UpdateLoadPositionDriftNative();
void QueueRupeeSlideDriftArrow();
bool GetLoggedRupeeSlidePosition(cXyz& position, s16& angle);
void DrawLoadPositionOverlayImGui();
void DrawLoadPositionOverlayNative();

}
