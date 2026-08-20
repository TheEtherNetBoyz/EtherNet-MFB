#ifndef DUSK_TRIGGER_VIEW_H
#define DUSK_TRIGGER_VIEW_H

#include <string>
#include <vector>

namespace dusk::trigger_view {

enum class Shape {
    Cylinder,
    Box,
    Sphere,
    GroundCircle,
    GroundRectangle,
};

struct Definition {
    bool enabled = true;
    std::string actorName;
    Shape shape = Shape::Cylinder;
    float color[4] = {0.2f, 0.9f, 1.0f, 0.8f};
    bool useActorScale = true;
    bool useActorYaw = false;
    float size[3] = {100.0f, 100.0f, 100.0f};
    float offset[3] = {0.0f, 0.0f, 0.0f};
    int liveMatches = 0;
    int visibleMatches = 0;
    float nearestVolumeDistance = -1.0f;
    float nearestActorPosition[3] = {};
    float nearestResolvedSize[3] = {};
};

std::vector<Definition>& GetDefinitions();
void EnsureLoaded();
void Save();
bool IsActorNameValid(const std::string& name);
void Draw();

}  // namespace dusk::trigger_view

#endif
