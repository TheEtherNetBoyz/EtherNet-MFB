#pragma once

#include "SSystem/SComponent/c_xyz.h"

namespace dusk {

void UpdateLoadPositionDriftNative();
bool GetLoggedRupeeSlidePosition(cXyz& position, s16& angle);
void DrawLoadPositionOverlayNative();

}
