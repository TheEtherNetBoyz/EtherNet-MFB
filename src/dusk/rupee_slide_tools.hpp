#pragma once

class daAlink_c;

namespace dusk {

bool IsRupeeSlideHoldAnimationActive(const daAlink_c* player);
void UpdateRupeeSlideSimulation();
void UpdateRupeeSlidePresentation();
void UpdateRupeeSlideTools();

}  // namespace dusk
