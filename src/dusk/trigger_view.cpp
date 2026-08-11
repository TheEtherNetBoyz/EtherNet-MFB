#include "dusk/trigger_view.h"

#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_debug_viewer.h"
#include "dusk/config.hpp"
#include "dusk/settings.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace dusk::trigger_view {
namespace {

std::vector<Definition> sDefinitions;
std::string sLoadedConfig;
bool sLoaded = false;

std::string NormalizeName(std::string name) {
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), name.end());
    if (name.rfind("fpcNm_", 0) != 0) {
        name.insert(0, "fpcNm_");
    }
    if (name.size() < 2 || name.compare(name.size() - 2, 2, "_e") != 0) {
        name += "_e";
    }
    return name;
}

int ResolveName(const std::string& name) {
    const auto normalized = NormalizeName(name);
    for (const auto& procName : procNames) {
        if (normalized == procName.name) {
            return static_cast<int>(procName.id);
        }
    }
    return -1;
}

const char* ShapeName(Shape shape) {
    switch (shape) {
    case Shape::GroundCircle:
        return "ground_circle";
    case Shape::GroundRectangle:
        return "ground_rectangle";
    case Shape::Box:
        return "box";
    case Shape::Sphere:
        return "sphere";
    case Shape::Cylinder:
    default:
        return "cylinder";
    }
}

Shape ParseShape(const std::string& shape) {
    if (shape == "ground_circle") {
        return Shape::GroundCircle;
    }
    if (shape == "ground_rectangle") {
        return Shape::GroundRectangle;
    }
    if (shape == "box") {
        return Shape::Box;
    }
    if (shape == "sphere") {
        return Shape::Sphere;
    }
    return Shape::Cylinder;
}

void Load(const std::string& encoded) {
    sDefinitions.clear();
    try {
        const auto values = nlohmann::json::parse(encoded);
        if (!values.is_array()) {
            return;
        }
        for (const auto& value : values) {
            Definition definition;
            definition.enabled = value.value("enabled", true);
            definition.actorName = value.value("actor", "");
            definition.shape = ParseShape(value.value("shape", "cylinder"));
            definition.useActorScale = value.value("useActorScale", true);
            definition.useActorYaw = value.value("useActorYaw", false);

            const auto readArray = [&](const char* key, float* output, size_t count) {
                const auto found = value.find(key);
                if (found == value.end() || !found->is_array()) {
                    return;
                }
                for (size_t i = 0; i < count && i < found->size(); ++i) {
                    output[i] = (*found)[i].get<float>();
                }
            };
            readArray("color", definition.color, 4);
            readArray("size", definition.size, 3);
            readArray("offset", definition.offset, 3);
            if (!definition.actorName.empty()) {
                sDefinitions.push_back(std::move(definition));
            }
        }
    } catch (const nlohmann::json::exception&) {}
}

GXColor GetColor(const Definition& definition, float opacity) {
    const auto byte = [](float value) {
        return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
    };
    return GXColor{
        byte(definition.color[0]),
        byte(definition.color[1]),
        byte(definition.color[2]),
        byte(definition.color[3] * opacity),
    };
}

cXyz RotatedOffset(const Definition& definition, s16 yaw) {
    const float angle = static_cast<float>(yaw) * (3.14159265358979323846f / 32768.0f);
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    return cXyz(
        definition.offset[0] * cosine + definition.offset[2] * sine,
        definition.offset[1],
        definition.offset[2] * cosine - definition.offset[0] * sine);
}

void DrawWireCylinder(cXyz base, float radius, float height, const GXColor& color) {
    cXyz top = base;
    top.y += height;
    dDbVw_drawCircleXlu(base, radius, color, TRUE, 3);
    dDbVw_drawCircleXlu(top, radius, color, TRUE, 3);
    cXyz sides[] = {
        cXyz(base.x + radius, base.y, base.z),
        cXyz(base.x - radius, base.y, base.z),
        cXyz(base.x, base.y, base.z + radius),
        cXyz(base.x, base.y, base.z - radius),
    };
    for (auto& start : sides) {
        cXyz end = start;
        end.y += height;
        dDbVw_drawLineXlu(start, end, color, TRUE, 3);
    }
}

void DrawGroundCircle(cXyz center, float radius, const GXColor& color,
                      bool wireframe)
{
    // A depth-tested line almost exactly on the floor disappears into the
    // floor. In filled mode use a shallow disc; in wireframe mode disable the
    // depth test so the footprint remains readable on uneven ground.
    if (!wireframe) {
        // Straddle the ground plane with only one unit of thickness. This
        // remains visible without presenting as a hovering slab.
        center.y -= 0.5f;
        dDbVw_drawCylinderXlu(center, radius, 1.0f, color, TRUE);
        return;
    }

    dDbVw_drawCircleXlu(center, radius, color, FALSE, 8);
    cXyz xStart(center.x - radius, center.y, center.z);
    cXyz xEnd(center.x + radius, center.y, center.z);
    cXyz zStart(center.x, center.y, center.z - radius);
    cXyz zEnd(center.x, center.y, center.z + radius);
    dDbVw_drawLineXlu(xStart, xEnd, color, FALSE, 3);
    dDbVw_drawLineXlu(zStart, zEnd, color, FALSE, 3);
}

void DrawGroundRectangle(const cXyz& center, const cXyz& size, s16 yaw,
                         const GXColor& color, bool wireframe)
{
    cXyz points[4];
    const float angle = static_cast<float>(yaw) * (3.14159265358979323846f / 32768.0f);
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    for (int i = 0; i < 4; ++i) {
        const float x = ((i == 1 || i == 2) ? 0.5f : -0.5f) * size.x;
        const float z = ((i >= 2) ? 0.5f : -0.5f) * size.z;
        points[i].set(
            center.x + x * cosine + z * sine,
            center.y + 2.0f,
            center.z + z * cosine - x * sine);
    }
    if (!wireframe) {
        dDbVw_drawQuadXlu(points, color, FALSE);
    } else {
        for (int i = 0; i < 4; ++i) {
            dDbVw_drawLineXlu(points[i], points[(i + 1) & 3], color, FALSE, 6);
        }
    }
}

void DrawWireBox(const cXyz& center, const cXyz& size, s16 yaw, const GXColor& color) {
    cXyz points[8];
    const float angle = static_cast<float>(yaw) * (3.14159265358979323846f / 32768.0f);
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    for (int i = 0; i < 8; ++i) {
        const float x = ((i & 1) ? 0.5f : -0.5f) * size.x;
        const float y = ((i & 4) ? 0.5f : -0.5f) * size.y;
        const float z = ((i & 2) ? 0.5f : -0.5f) * size.z;
        points[i].set(
            center.x + x * cosine + z * sine,
            center.y + y,
            center.z + z * cosine - x * sine);
    }
    constexpr int edges[][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
        {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    for (const auto& edge : edges) {
        dDbVw_drawLineXlu(points[edge[0]], points[edge[1]], color, TRUE, 3);
    }
}

void DrawWireSphere(const cXyz& center, float radius, const GXColor& color) {
    constexpr int segments = 24;
    for (int plane = 0; plane < 3; ++plane) {
        for (int i = 0; i < segments; ++i) {
            const float a = static_cast<float>(i) * 2.0f * 3.14159265358979323846f / segments;
            const float b = static_cast<float>(i + 1) * 2.0f * 3.14159265358979323846f / segments;
            cXyz start = center;
            cXyz end = center;
            float* startA = plane == 0 ? &start.x : &start.y;
            float* startB = plane == 2 ? &start.z : (plane == 1 ? &start.x : &start.z);
            float* endA = plane == 0 ? &end.x : &end.y;
            float* endB = plane == 2 ? &end.z : (plane == 1 ? &end.x : &end.z);
            *startA += std::cos(a) * radius;
            *startB += std::sin(a) * radius;
            *endA += std::cos(b) * radius;
            *endB += std::sin(b) * radius;
            dDbVw_drawLineXlu(start, end, color, TRUE, 3);
        }
    }
}

struct DrawContext {
    const CollisionViewSettings* settings;
    const cXyz* playerPosition;
};

float DistanceToVolume(Shape shape, const cXyz& position,
                       const cXyz& size, const cXyz& player)
{
    cXyz minimum;
    cXyz maximum;
    if (shape == Shape::GroundCircle) {
        minimum.set(position.x - size.x, position.y, position.z - size.x);
        maximum.set(position.x + size.x, position.y, position.z + size.x);
    } else if (shape == Shape::GroundRectangle) {
        const float horizontalExtent = std::sqrt(
            size.x * size.x + size.z * size.z) * 0.5f;
        minimum.set(position.x - horizontalExtent, position.y,
                    position.z - horizontalExtent);
        maximum.set(position.x + horizontalExtent, position.y,
                    position.z + horizontalExtent);
    } else if (shape == Shape::Cylinder) {
        minimum.set(position.x - size.x, position.y, position.z - size.x);
        maximum.set(position.x + size.x, position.y + size.y, position.z + size.x);
    } else if (shape == Shape::Sphere) {
        minimum.set(position.x - size.x, position.y - size.x, position.z - size.x);
        maximum.set(position.x + size.x, position.y + size.x, position.z + size.x);
    } else {
        // A yawed box fits inside this conservative world-space AABB. It can
        // draw slightly early near a corner, but it will never disappear while
        // the player is actually near or inside it.
        const float horizontalExtent = std::sqrt(
            size.x * size.x + size.z * size.z) * 0.5f;
        minimum.set(
            position.x - horizontalExtent,
            position.y - size.y * 0.5f,
            position.z - horizontalExtent);
        maximum.set(
            position.x + horizontalExtent,
            position.y + size.y * 0.5f,
            position.z + horizontalExtent);
    }

    const auto axisDistance = [](float value, float minValue, float maxValue) {
        if (value < minValue) {
            return minValue - value;
        }
        if (value > maxValue) {
            return value - maxValue;
        }
        return 0.0f;
    };
    return std::max({
        axisDistance(player.x, minimum.x, maximum.x),
        axisDistance(player.y, minimum.y, maximum.y),
        axisDistance(player.z, minimum.z, maximum.z),
    });
}

int DrawActor(void* rawActor, void* rawContext) {
    auto* actor = static_cast<fopAc_ac_c*>(rawActor);
    const auto& context = *static_cast<DrawContext*>(rawContext);

    const int actorName = fopAcM_GetProfName(actor);
    for (auto& definition : sDefinitions) {
        if (!definition.enabled || ResolveName(definition.actorName) != actorName) {
            continue;
        }
        ++definition.liveMatches;

        s16 yaw = definition.useActorYaw ? actor->shape_angle.y : 0;
        cXyz position = actor->current.pos + RotatedOffset(definition, yaw);
        cXyz size(
            std::fabs(definition.size[0]),
            std::fabs(definition.size[1]),
            std::fabs(definition.size[2]));
        if (definition.useActorScale) {
            size.x *= std::fabs(actor->scale.x);
            size.y *= std::fabs(actor->scale.y);
            size.z *= std::fabs(actor->scale.z);
        }
        const Shape resolvedShape = definition.shape;
        if (context.playerPosition != nullptr) {
            const float distance =
                DistanceToVolume(resolvedShape, position, size, *context.playerPosition);
            if (definition.nearestVolumeDistance < 0.0f ||
                distance < definition.nearestVolumeDistance)
            {
                definition.nearestVolumeDistance = distance;
                definition.nearestActorPosition[0] = position.x;
                definition.nearestActorPosition[1] = position.y;
                definition.nearestActorPosition[2] = position.z;
                definition.nearestResolvedSize[0] = size.x;
                definition.nearestResolvedSize[1] = size.y;
                definition.nearestResolvedSize[2] = size.z;
            }
        }
        ++definition.visibleMatches;
        const GXColor color =
            GetColor(definition, context.settings->colliderViewOpacity / 100.0f);

        if (resolvedShape == Shape::GroundCircle) {
            DrawGroundCircle(
                position, size.x, color, context.settings->enableWireframe);
        } else if (resolvedShape == Shape::GroundRectangle) {
            DrawGroundRectangle(
                position, size, yaw, color, context.settings->enableWireframe);
        } else if (context.settings->enableWireframe) {
            if (resolvedShape == Shape::Cylinder) {
                DrawWireCylinder(position, size.x, size.y, color);
            } else if (resolvedShape == Shape::Box) {
                DrawWireBox(position, size, yaw, color);
            } else {
                DrawWireSphere(position, size.x, color);
            }
        } else if (resolvedShape == Shape::Cylinder) {
            dDbVw_drawCylinderXlu(position, size.x, size.y, color, TRUE);
        } else if (resolvedShape == Shape::Box) {
            csXyz angle(0, yaw, 0);
            dDbVw_drawCubeXlu(position, size, angle, color);
        } else {
            dDbVw_drawSphereXlu(position, size.x, color, TRUE);
        }
    }
    return 1;
}

}  // namespace

std::vector<Definition>& GetDefinitions() {
    EnsureLoaded();
    return sDefinitions;
}

void EnsureLoaded() {
    const auto& encoded = getSettings().game.triggerViewDefinitions.getValue();
    if (!sLoaded || encoded != sLoadedConfig) {
        Load(encoded);
        sLoadedConfig = encoded;
        sLoaded = true;
    }
}

void Save() {
    nlohmann::json values = nlohmann::json::array();
    for (auto& definition : sDefinitions) {
        definition.actorName = NormalizeName(definition.actorName);
        values.push_back({
            {"enabled", definition.enabled},
            {"actor", definition.actorName},
            {"shape", ShapeName(definition.shape)},
            {"color", {definition.color[0], definition.color[1],
                       definition.color[2], definition.color[3]}},
            {"useActorScale", definition.useActorScale},
            {"useActorYaw", definition.useActorYaw},
            {"size", {definition.size[0], definition.size[1], definition.size[2]}},
            {"offset", {definition.offset[0], definition.offset[1], definition.offset[2]}},
        });
    }
    sLoadedConfig = values.dump();
    getSettings().game.triggerViewDefinitions.setValue(sLoadedConfig);
    config::save();
}

bool IsActorNameValid(const std::string& name) {
    return ResolveName(name) >= 0;
}

void Draw() {
    const auto& settings = getTransientSettings().collisionView;
    if (!settings.enableTriggerView) {
        return;
    }
    EnsureLoaded();
    for (auto& definition : sDefinitions) {
        definition.liveMatches = 0;
        definition.visibleMatches = 0;
        definition.nearestVolumeDistance = -1.0f;
    }
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    DrawContext context{
        &settings,
        player != nullptr ? &player->current.pos : nullptr,
    };
    fopAcIt_Executor(DrawActor, &context);
}

}  // namespace dusk::trigger_view
