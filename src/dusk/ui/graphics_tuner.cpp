#include "graphics_tuner.hpp"

#include "button.hpp"

#include "dusk/config.hpp"
#include "dusk/logging.h"
#include "dusk/settings.h"
#include "m_Do/m_Do_audio.h"

#include <dolphin/gx/GXAurora.h>
#include <fmt/format.h>

#include <algorithm>
#include <string>
#include <type_traits>

namespace dusk::ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/tuner.rcss" />
</head>
<body>
    <tuner-root id="root">
        <graphics-tuner>
            <tuner-header>
                <tuner-title id="title" />
                <carousel-container id="carousel-container" />
            </tuner-header>
            <tuner-description id="description" />
            <tuner-divider />
            <tuner-footer id="footer" />
        </graphics-tuner>
    </tuner-root>
</body>
</rml>
)RML";

Rml::String format_internal_resolution(int value) {
    u32 width = 0;
    u32 height = 0;
    AuroraGetRenderSize(&width, &height);
    if (value <= 0) {
        return fmt::format("Auto ({}×{})", width, height);
    }
    return fmt::format("{}× ({}×{})", value, width, height);
}

Rml::String format_resampler(int value) {
    switch (static_cast<Resampler>(value)) {
    case Resampler::Bilinear:
        return "Bilinear";
    case Resampler::Area:
        return "Area";
    default:
        return "";
    }
}

Rml::String format_post_process_mode(int value) {
    switch (static_cast<BloomMode>(value)) {
    case BloomMode::Off:
        return "Off";
    case BloomMode::Classic:
        return "Classic";
    case BloomMode::Dusk:
        return "Dusklight";
    case BloomMode::Legacy:
        return "Legacy";
    default:
        return "";
    }
}

Rml::String format_times(int value) { return fmt::format("{}×", value); }

Rml::String format_percent(int value) { return fmt::format("{}%", value); }

Rml::String format_bool(int value) { return value ? "On" : "Off"; }

Rml::String format_rupee_slide_pickup_animation(int value) {
    switch (static_cast<RupeeSlidePickupAnimation>(value)) {
    case RupeeSlidePickupAnimation::Vanilla: return "Vanilla rupee hold";
    case RupeeSlidePickupAnimation::Idle: return "Idle";
    case RupeeSlidePickupAnimation::AttentionLeft: return "Attention left";
    case RupeeSlidePickupAnimation::AttentionRight: return "Attention right";
    case RupeeSlidePickupAnimation::AlternateGetItem: return "Alternate get-item hold";
    case RupeeSlidePickupAnimation::SixSeven: return "6-7 gesture";
    case RupeeSlidePickupAnimation::AlternateIdle: return "Alternate idle";
    case RupeeSlidePickupAnimation::Tired: return "Tired stance";
    case RupeeSlidePickupAnimation::Crouch: return "Crouch";
    case RupeeSlidePickupAnimation::GuardLeft: return "Guard left";
    case RupeeSlidePickupAnimation::GuardRight: return "Guard right";
    case RupeeSlidePickupAnimation::SmallGuard: return "Small guard";
    case RupeeSlidePickupAnimation::TiredGuard: return "Tired guard";
    case RupeeSlidePickupAnimation::DefensiveCrouch: return "Defensive crouch";
    case RupeeSlidePickupAnimation::PushPullReady: return "Push/pull ready";
    case RupeeSlidePickupAnimation::Crawl: return "Crawl";
    case RupeeSlidePickupAnimation::ServiceWait: return "Service wait";
    case RupeeSlidePickupAnimation::Carry: return "Carry pose";
    case RupeeSlidePickupAnimation::Talk: return "Talk";
    case RupeeSlidePickupAnimation::Holdout: return "Holdout";
    case RupeeSlidePickupAnimation::Sit: return "Sit";
    case RupeeSlidePickupAnimation::Walk: return "Walk";
    case RupeeSlidePickupAnimation::Run: return "Run";
    case RupeeSlidePickupAnimation::StepTurn: return "Step turn";
    case RupeeSlidePickupAnimation::FrontRoll: return "Front roll";
    case RupeeSlidePickupAnimation::Backflip: return "Backflip";
    case RupeeSlidePickupAnimation::Jump: return "Jump";
    case RupeeSlidePickupAnimation::JumpLand: return "Jump land";
    case RupeeSlidePickupAnimation::ShieldAttack: return "Shield attack";
    case RupeeSlidePickupAnimation::GuardBreak: return "Guard break";
    case RupeeSlidePickupAnimation::Push: return "Push";
    case RupeeSlidePickupAnimation::Pull: return "Pull";
    case RupeeSlidePickupAnimation::Throw: return "Throw";
    case RupeeSlidePickupAnimation::HeavyThrow: return "Heavy throw";
    case RupeeSlidePickupAnimation::LargePickup: return "Large pickup";
    case RupeeSlidePickupAnimation::BoomerangCatch: return "Boomerang catch";
    case RupeeSlidePickupAnimation::SlideForward: return "Slide forward";
    case RupeeSlidePickupAnimation::SlideBackward: return "Slide backward";
    case RupeeSlidePickupAnimation::GetItemStart: return "Get-item start";
    case RupeeSlidePickupAnimation::TurnBack: return "Turn back";
    case RupeeSlidePickupAnimation::PickUp: return "Pick up";
    case RupeeSlidePickupAnimation::RollJump: return "Roll jump";
    case RupeeSlidePickupAnimation::WaitWind: return "Wind idle";
    case RupeeSlidePickupAnimation::BottleDrink: return "Bottle drink";
    case RupeeSlidePickupAnimation::BottlePour: return "Bottle pour";
    case RupeeSlidePickupAnimation::GrassWhistle: return "Grass whistle";
    case RupeeSlidePickupAnimation::HawkWait: return "Hawk wait";
    case RupeeSlidePickupAnimation::CowStroke: return "Goat stroke";
    case RupeeSlidePickupAnimation::Fall: return "Fall";
    case RupeeSlidePickupAnimation::HeavyBoots: return "Heavy boots";
    case RupeeSlidePickupAnimation::SumoWait: return "Sumo stance";
    case RupeeSlidePickupAnimation::SumoWin: return "Sumo win";
    case RupeeSlidePickupAnimation::SumoShiko: return "Sumo shiko";
    case RupeeSlidePickupAnimation::IronBallWait: return "Ball and chain idle";
    case RupeeSlidePickupAnimation::CopyRodSwing: return "Dominion Rod swing";
    case RupeeSlidePickupAnimation::AlternateWait: return "Alternate wait";
    case RupeeSlidePickupAnimation::GroundStun: return "Electric stun";
    case RupeeSlidePickupAnimation::KeyCatch: return "Key catch";
    case RupeeSlidePickupAnimation::InsectWait: return "Insect wait";
    case RupeeSlidePickupAnimation::TradeItemWait: return "Trade-item wait";
    case RupeeSlidePickupAnimation::DemoKamae: return "Demo stance";
    case RupeeSlidePickupAnimation::Dive: return "Dive";
    case RupeeSlidePickupAnimation::Surprised: return "Surprised";
    case RupeeSlidePickupAnimation::LookDown: return "Look down";
    case RupeeSlidePickupAnimation::SwimWait: return "Swim idle";
    case RupeeSlidePickupAnimation::SideJumpLeft: return "Side jump left";
    case RupeeSlidePickupAnimation::SideJumpRight: return "Side jump right";
    case RupeeSlidePickupAnimation::BackJump: return "Back jump";
    case RupeeSlidePickupAnimation::RollCrash: return "Roll crash";
    case RupeeSlidePickupAnimation::CrouchStart: return "Crouch start";
    case RupeeSlidePickupAnimation::JumpStart: return "Jump start";
    case RupeeSlidePickupAnimation::CrawlStart: return "Crawl start";
    case RupeeSlidePickupAnimation::PushLight: return "Light push";
    case RupeeSlidePickupAnimation::PullLight: return "Light pull";
    case RupeeSlidePickupAnimation::SideRollLeft: return "Side roll left";
    case RupeeSlidePickupAnimation::SideRollRight: return "Side roll right";
    case RupeeSlidePickupAnimation::BottleOpen: return "Bottle open";
    case RupeeSlidePickupAnimation::LanternSwing: return "Lantern swing";
    case RupeeSlidePickupAnimation::GrassWhistleStart: return "Grass whistle start";
    case RupeeSlidePickupAnimation::HawkCatch: return "Hawk catch";
    case RupeeSlidePickupAnimation::Entrance: return "Entrance";
    case RupeeSlidePickupAnimation::Appearance: return "Appearance";
    default: return "Unknown";
    }
}

Rml::String format_rupee_slide_attachment(int value) {
    switch (static_cast<RupeeSlideRupeeAttachment>(value)) {
    case RupeeSlideRupeeAttachment::Vanilla: return "Vanilla position";
    case RupeeSlideRupeeAttachment::LeftHand: return "Link's left hand";
    case RupeeSlideRupeeAttachment::RightHand: return "Link's right hand";
    case RupeeSlideRupeeAttachment::Head: return "Above Link's head";
    case RupeeSlideRupeeAttachment::World: return "Fixed world position";
    default: return "Unknown";
    }
}

Rml::String format_rupee_slide_units(int value) { return fmt::format("{} units", value); }

Rml::String format_rupee_slide_degrees(int value) { return fmt::format("{}°", value); }

Rml::String format_rupee_slide_scale(int value) { return fmt::format("{}%", value); }

const char* rupee_slide_rupee_title(RupeeSlideRupeeOption option) {
    switch (option) {
    case RupeeSlideRupeeOption::Attachment: return "Rupee Attachment";
    case RupeeSlideRupeeOption::OffsetX: return "Rupee X Offset";
    case RupeeSlideRupeeOption::OffsetY: return "Rupee Y Offset";
    case RupeeSlideRupeeOption::OffsetZ: return "Rupee Z Offset";
    case RupeeSlideRupeeOption::RotationX: return "Rupee X Rotation";
    case RupeeSlideRupeeOption::RotationY: return "Rupee Y Rotation";
    case RupeeSlideRupeeOption::RotationZ: return "Rupee Z Rotation";
    case RupeeSlideRupeeOption::Scale: return "Rupee Scale";
    case RupeeSlideRupeeOption::DuplicateAttachment: return "Duplicate Rupee Attachment";
    case RupeeSlideRupeeOption::DuplicateOffsetX: return "Duplicate Rupee X Offset";
    case RupeeSlideRupeeOption::DuplicateOffsetY: return "Duplicate Rupee Y Offset";
    case RupeeSlideRupeeOption::DuplicateOffsetZ: return "Duplicate Rupee Z Offset";
    default: return "Pickup Rupee";
    }
}

const char* rupee_slide_rupee_help(RupeeSlideRupeeOption option) {
    switch (option) {
    case RupeeSlideRupeeOption::Attachment:
        return "Chooses the pickup rupee's anchor. Vanilla preserves the original game position.";
    case RupeeSlideRupeeOption::OffsetX:
        return "Moves the pickup rupee on the world X axis.";
    case RupeeSlideRupeeOption::OffsetY:
        return "Moves the pickup rupee vertically.";
    case RupeeSlideRupeeOption::OffsetZ:
        return "Moves the pickup rupee on the world Z axis.";
    case RupeeSlideRupeeOption::RotationX:
        return "Adds rotation around the pickup rupee's X axis.";
    case RupeeSlideRupeeOption::RotationY:
        return "Adds rotation around the pickup rupee's spinning Y axis.";
    case RupeeSlideRupeeOption::RotationZ:
        return "Adds rotation around the pickup rupee's Z axis.";
    case RupeeSlideRupeeOption::Scale:
        return "Scales the pickup rupee without changing Link or slide displacement.";
    case RupeeSlideRupeeOption::DuplicateAttachment:
        return "Chooses an anchor for the duplicate independently of the original rupee.";
    case RupeeSlideRupeeOption::DuplicateOffsetX:
        return "Moves the visual duplicate on the world X axis.";
    case RupeeSlideRupeeOption::DuplicateOffsetY:
        return "Moves the visual duplicate vertically.";
    case RupeeSlideRupeeOption::DuplicateOffsetZ:
        return "Moves the visual duplicate on the world Z axis.";
    default:
        return "Adjusts the pickup rupee during the rupee slide.";
    }
}

GraphicsSetting rupee_slide_rupee_setting(RupeeSlideRupeeOption option) {
    switch (option) {
    case RupeeSlideRupeeOption::Attachment:
        return GraphicsSetting{
            .min = static_cast<int>(RupeeSlideRupeeAttachment::Vanilla),
            .max = static_cast<int>(RupeeSlideRupeeAttachment::World),
            .defaultValue = static_cast<int>(RupeeSlideRupeeAttachment::Vanilla),
            .step = 1,
            .read = []() -> int {
                return static_cast<int>(getSettings().game.rupeeSlideRupeeAttachment.getValue());
            },
            .write = [](int value) {
                getSettings().game.rupeeSlideRupeeAttachment.setValue(
                    static_cast<RupeeSlideRupeeAttachment>(value));
            },
            .label = format_rupee_slide_attachment,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeAttachment.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeAttachment;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::OffsetX:
        return GraphicsSetting{
            .min = -500,
            .max = 500,
            .defaultValue = 0,
            .step = 1,
            .read = []() -> int { return getSettings().game.rupeeSlideRupeeOffsetX.getValue(); },
            .write = [](int value) { getSettings().game.rupeeSlideRupeeOffsetX.setValue(value); },
            .label = format_rupee_slide_units,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeOffsetX.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeOffsetX;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::OffsetY:
        return GraphicsSetting{
            .min = -500,
            .max = 500,
            .defaultValue = 0,
            .step = 1,
            .read = []() -> int { return getSettings().game.rupeeSlideRupeeOffsetY.getValue(); },
            .write = [](int value) { getSettings().game.rupeeSlideRupeeOffsetY.setValue(value); },
            .label = format_rupee_slide_units,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeOffsetY.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeOffsetY;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::OffsetZ:
        return GraphicsSetting{
            .min = -500,
            .max = 500,
            .defaultValue = 0,
            .step = 1,
            .read = []() -> int { return getSettings().game.rupeeSlideRupeeOffsetZ.getValue(); },
            .write = [](int value) { getSettings().game.rupeeSlideRupeeOffsetZ.setValue(value); },
            .label = format_rupee_slide_units,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeOffsetZ.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeOffsetZ;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::RotationX:
        return GraphicsSetting{
            .min = 0,
            .max = 359,
            .defaultValue = 0,
            .step = 5,
            .read = []() -> int { return getSettings().game.rupeeSlideRupeeRotationX.getValue(); },
            .write = [](int value) {
                getSettings().game.rupeeSlideRupeeRotationX.setValue(value);
            },
            .label = format_rupee_slide_degrees,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeRotationX.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeRotationX;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::RotationY:
        return GraphicsSetting{
            .min = 0,
            .max = 359,
            .defaultValue = 0,
            .step = 5,
            .read = []() -> int { return getSettings().game.rupeeSlideRupeeRotationY.getValue(); },
            .write = [](int value) {
                getSettings().game.rupeeSlideRupeeRotationY.setValue(value);
            },
            .label = format_rupee_slide_degrees,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeRotationY.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeRotationY;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::RotationZ:
        return GraphicsSetting{
            .min = 0,
            .max = 359,
            .defaultValue = 0,
            .step = 5,
            .read = []() -> int { return getSettings().game.rupeeSlideRupeeRotationZ.getValue(); },
            .write = [](int value) {
                getSettings().game.rupeeSlideRupeeRotationZ.setValue(value);
            },
            .label = format_rupee_slide_degrees,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeRotationZ.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeRotationZ;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::Scale:
        return GraphicsSetting{
            .min = 10,
            .max = 500,
            .defaultValue = 100,
            .step = 5,
            .read = []() -> int { return getSettings().game.rupeeSlideRupeeScale.getValue(); },
            .write = [](int value) { getSettings().game.rupeeSlideRupeeScale.setValue(value); },
            .label = format_rupee_slide_scale,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeScale.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeScale;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::DuplicateAttachment:
        return GraphicsSetting{
            .min = static_cast<int>(RupeeSlideRupeeAttachment::Vanilla),
            .max = static_cast<int>(RupeeSlideRupeeAttachment::World),
            .defaultValue = static_cast<int>(RupeeSlideRupeeAttachment::Vanilla),
            .step = 1,
            .read = []() -> int {
                return static_cast<int>(
                    getSettings().game.rupeeSlideRupeeDuplicateAttachment.getValue());
            },
            .write = [](int value) {
                getSettings().game.rupeeSlideRupeeDuplicateAttachment.setValue(
                    static_cast<RupeeSlideRupeeAttachment>(value));
            },
            .label = format_rupee_slide_attachment,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeDuplicateAttachment.getName();
            },
            .isModified = []() -> bool {
                const auto& setting =
                    getSettings().game.rupeeSlideRupeeDuplicateAttachment;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::DuplicateOffsetX:
        return GraphicsSetting{
            .min = -500,
            .max = 500,
            .defaultValue = 100,
            .step = 1,
            .read = []() -> int {
                return getSettings().game.rupeeSlideRupeeDuplicateOffsetX.getValue();
            },
            .write = [](int value) {
                getSettings().game.rupeeSlideRupeeDuplicateOffsetX.setValue(value);
            },
            .label = format_rupee_slide_units,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeDuplicateOffsetX.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeDuplicateOffsetX;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::DuplicateOffsetY:
        return GraphicsSetting{
            .min = -500,
            .max = 500,
            .defaultValue = 0,
            .step = 1,
            .read = []() -> int {
                return getSettings().game.rupeeSlideRupeeDuplicateOffsetY.getValue();
            },
            .write = [](int value) {
                getSettings().game.rupeeSlideRupeeDuplicateOffsetY.setValue(value);
            },
            .label = format_rupee_slide_units,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeDuplicateOffsetY.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeDuplicateOffsetY;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    case RupeeSlideRupeeOption::DuplicateOffsetZ:
        return GraphicsSetting{
            .min = -500,
            .max = 500,
            .defaultValue = 0,
            .step = 1,
            .read = []() -> int {
                return getSettings().game.rupeeSlideRupeeDuplicateOffsetZ.getValue();
            },
            .write = [](int value) {
                getSettings().game.rupeeSlideRupeeDuplicateOffsetZ.setValue(value);
            },
            .label = format_rupee_slide_units,
            .cvarName = []() -> const char* {
                return getSettings().game.rupeeSlideRupeeDuplicateOffsetZ.getName();
            },
            .isModified = []() -> bool {
                const auto& setting = getSettings().game.rupeeSlideRupeeDuplicateOffsetZ;
                return setting.getValue() != setting.getDefaultValue();
            },
        };
    }

    return {};
}

template <typename T>
int read_cvar(const ConfigVar<T>& var) {
    if constexpr (std::is_same_v<T, float>) {
        return static_cast<int>(var.getValue() * 100.0f + 0.5f);
    } else {
        return static_cast<int>(var.getValue());
    }
}

template <typename T>
void write_cvar(ConfigVar<T>& var, int value) {
    if constexpr (std::is_same_v<T, float>) {
        var.setValue(static_cast<float>(value) / 100.0f);
    } else if constexpr (std::is_same_v<T, bool>) {
        var.setValue(static_cast<bool>(value));
    } else {
        var.setValue(static_cast<T>(value));
    }
}

template <auto Var, typename Min, typename Max, typename Def>
const GraphicsSetting& bind(Min min, Max max, Def def, int step, Rml::String (*label)(int),
    bool watchSize = false) {
    static const GraphicsSetting desc{
        .min = static_cast<int>(min),
        .max = static_cast<int>(max),
        .defaultValue = static_cast<int>(def),
        .step = step,
        .watchesRenderSize = watchSize,
        .read = []() -> int { return read_cvar(Var()); },
        .write = [](int value) { write_cvar(Var(), value); },
        .label = label,
        .cvarName = []() -> const char* { return Var().getName(); },
        .isModified = []() -> bool { return Var().getValue() != Var().getDefaultValue(); },
    };
    return desc;
}

Rml::Element* create_stepped_carousel_root(Rml::Element* parent) {
    auto* doc = parent->GetOwnerDocument();
    auto root = doc->CreateElement("stepped-carousel");
    root->SetAttribute("tabindex", "0");
    return parent->AppendChild(std::move(root));
}

Rml::Element* create_stepped_carousel_arrow(
    Rml::Element* parent, const Rml::String& className, const Rml::String& label) {
    auto* button = append(parent, "button");
    button->SetClass("stepped-carousel-arrow", true);
    button->SetClass(className, true);
    append_text(button, label);
    return button;
}

void update_carousel_arrow_color(Rml::Element* arrow, bool dim) {
    const Rml::Colourb& color = Rml::Colourb(255, 255, 255, dim ? 128 : 255);
    arrow->SetProperty(Rml::PropertyId::Color, Rml::Property(color, Rml::Unit::COLOUR));
}

}  // namespace

const GraphicsSetting& GraphicsSetting::of(GraphicsOption option) {
    switch (option) {
    case GraphicsOption::InternalResolution:
        return bind<[]() -> auto& { return getSettings().game.internalResolutionScale; }>(
            0, 12, 0, 1, format_internal_resolution, true);
    case GraphicsOption::ShadowResolution:
        return bind<[]() -> auto& { return getSettings().game.shadowResolutionMultiplier; }>(
            1, 8, 1, 1, format_times);
    case GraphicsOption::Resampler:
        return bind<[]() -> auto& { return getSettings().game.resampler; }>(
            Resampler::Bilinear, Resampler::Area, Resampler::Bilinear, 1, format_resampler);
    case GraphicsOption::BloomMode:
        return bind<[]() -> auto& { return getSettings().game.bloomMode; }>(
            BloomMode::Off, BloomMode::Legacy, BloomMode::Classic, 1, format_post_process_mode);
    case GraphicsOption::BloomMultiplier:
        return bind<[]() -> auto& { return getSettings().game.bloomMultiplier; }>(
            0, 100, 100, 10, format_percent);
    case GraphicsOption::DepthOfFieldMode:
        return bind<[]() -> auto& { return getSettings().game.depthOfFieldMode; }>(
            DepthOfFieldMode::Off, DepthOfFieldMode::Dusk, DepthOfFieldMode::Classic, 1,
            format_post_process_mode);
    case GraphicsOption::TextureReplacements:
        return bind<[]() -> auto& { return getSettings().game.enableTextureReplacements; }>(
            0, 1, 0, 1, format_bool);
    }
    DuskLog.error("{} is an invalid GraphicsOption", static_cast<int>(option));
    abort();
}

SteppedCarousel::SteppedCarousel(Rml::Element* parent, Props props)
    : Component(create_stepped_carousel_root(parent)), mProps(std::move(props)) {
    mPrevElem = create_stepped_carousel_arrow(mRoot, "prev", "\uE5CB");
    mValueElem = append(mRoot, "stepped-carousel-value");
    mNextElem = create_stepped_carousel_arrow(mRoot, "next", "\uE5CC");

    listen(mPrevElem, Rml::EventId::Click,
        [this](Rml::Event&) { handle_nav_command(NavCommand::Left); });
    listen(mNextElem, Rml::EventId::Click,
        [this](Rml::Event&) { handle_nav_command(NavCommand::Right); });
    listen(mRoot, Rml::EventId::Keydown, [this](Rml::Event& event) {
        const auto cmd = map_nav_event(event);
        if (cmd != NavCommand::None && handle_nav_command(cmd)) {
            event.StopPropagation();
        }
    });
}

bool SteppedCarousel::focus() {
    return Component::focus();
}

void SteppedCarousel::update() {}

void SteppedCarousel::refresh() {
    if (mValueElem == nullptr) {
        return;
    }
    const int value = std::clamp(mProps.getValue ? mProps.getValue() : 0, mProps.min, mProps.max);
    if (mProps.formatValue) {
        set_text_content(mValueElem, mProps.formatValue(value));
    } else {
        set_text_content(mValueElem, std::to_string(value));
    }

    update_carousel_arrow_color(mPrevElem, value == mProps.min);
    update_carousel_arrow_color(mNextElem, value == mProps.max);
}

bool SteppedCarousel::handle_nav_command(NavCommand cmd) {
    if (cmd == NavCommand::Left) {
        const int value = mProps.getValue ? mProps.getValue() : 0;
        apply(std::clamp(value - mProps.step, mProps.min, mProps.max));
        return true;
    }
    if (cmd == NavCommand::Right) {
        const int value = mProps.getValue ? mProps.getValue() : 0;
        apply(std::clamp(value + mProps.step, mProps.min, mProps.max));
        return true;
    }
    return false;
}

void SteppedCarousel::apply(int value) {
    const int nextValue = std::clamp(value, mProps.min, mProps.max);
    const int currentValue =
        std::clamp(mProps.getValue ? mProps.getValue() : 0, mProps.min, mProps.max);
    if (nextValue == currentValue) {
        return;
    }
    mDoAud_seStartMenu(kSoundItemChange);
    if (mProps.onChange) {
        mProps.onChange(nextValue);
    }
}

GraphicsTuner::GraphicsTuner(GraphicsTunerProps props)
    : GraphicsTuner(std::move(props.title), std::move(props.helpText),
          GraphicsSetting::of(props.option), DocumentScope::GraphicsTuner) {}

GraphicsTuner::GraphicsTuner(Rml::String tunerTitle, Rml::String helpText,
    GraphicsSetting setting, DocumentScope scope)
    : Document(kDocumentSource, false, scope), mSetting(setting) {
    if (mDocument == nullptr) {
        return;
    }

    if (auto* title = mDocument->GetElementById("title")) {
        append_text(title, tunerTitle);
    }
    if (auto* description = mDocument->GetElementById("description")) {
        append_text(description, helpText);
    }
    if (auto* carouselParent = mDocument->GetElementById("carousel-container")) {
        mCarousel = &add_component<SteppedCarousel>(carouselParent,
            SteppedCarousel::Props{
                .min = mSetting.min,
                .max = mSetting.max,
                .step = mSetting.step,
                .getValue = [this] { return mSetting.read(); },
                .onChange = [this](int value) { mSetting.set(value); },
                .formatValue = [this](int value) { return mSetting.label(value); },
            });
    }

    if (auto* footer = mDocument->GetElementById("footer")) {
        auto& returnButton = add_component<Button>(footer, "\xE2\x86\x90 Return", "footer-button")
                                 .on_pressed([this] { pop(); });
        returnButton.root()->SetClass("return", true);
        auto& resetButton =
            add_component<Button>(footer, "Reset to default", "footer-button").on_pressed([this] {
                mDoAud_seStartMenu(kSoundItemChange);
                reset_default();
            });
        resetButton.root()->SetClass("reset", true);
    }

    if (mCarousel != nullptr) {
        if (const char* name = mSetting.cvarName()) {
            mSubscription = config::subscribe(name,
                [this](config::ConfigVarBase&, const void*) { mCarousel->refresh(); });
        }
        mCarousel->refresh();
        if (mSetting.watchesRenderSize) {
            AuroraGetRenderSize(&mLastRenderWidth, &mLastRenderHeight);
        }
    }

    // Hide document after transition completion
    mRoot = mDocument->GetElementById("root");
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") &&
            Document::visible())
        {
            Document::hide(mPendingClose);
        }
    });
}

GraphicsTuner::~GraphicsTuner() {
    if (mSubscription != 0) {
        config::unsubscribe(mSubscription);
    }
}

void GraphicsTuner::show() {
    Document::show();
    mRoot->SetAttribute("open", "");
    mDoAud_seStartMenu(kSoundWindowOpen);
}

void GraphicsTuner::hide(bool close) {
    config::save();
    mRoot->RemoveAttribute("open");
    if (close) {
        mPendingClose = true;
        mDoAud_seStartMenu(kSoundWindowClose);
    }
}

void GraphicsTuner::update() {
    if (mSetting.watchesRenderSize && mCarousel != nullptr) {
        u32 width = 0;
        u32 height = 0;
        AuroraGetRenderSize(&width, &height);
        if (width != mLastRenderWidth || height != mLastRenderHeight) {
            mLastRenderWidth = width;
            mLastRenderHeight = height;
            mCarousel->refresh();
        }
    }
    for (const auto& component : mComponents) {
        component->update();
    }
    Document::update();
}

bool GraphicsTuner::focus() {
    for (const auto& component : mComponents) {
        if (component->focus()) {
            return true;
        }
    }
    return false;
}

bool GraphicsTuner::visible() const {
    return mRoot->HasAttribute("open");
}

bool GraphicsTuner::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel) {
        pop();
        return true;
    }

    if (mCarousel && mCarousel->handle_nav_command(cmd)) {
        return true;
    }

    return Document::handle_nav_command(event, cmd);
}

void GraphicsTuner::reset_default() {
    mSetting.set(mSetting.defaultValue);
}

RupeeSlideAnimationTuner::RupeeSlideAnimationTuner()
    : GraphicsTuner("Pickup Slide Animation",
          "Changes Link's holding pose during the pickup slide only. Vanilla preserves the "
          "original rupee-holding animation.",
          GraphicsSetting{
              .min = static_cast<int>(RupeeSlidePickupAnimation::Vanilla),
              .max = static_cast<int>(RupeeSlidePickupAnimation::Appearance),
              .defaultValue = static_cast<int>(RupeeSlidePickupAnimation::Vanilla),
              .step = 1,
              .watchesRenderSize = false,
              .read = []() -> int {
                  return static_cast<int>(
                      getSettings().game.rupeeSlidePickupAnimation.getValue());
              },
              .write = [](int value) {
                  getSettings().game.rupeeSlidePickupAnimation.setValue(
                      static_cast<RupeeSlidePickupAnimation>(value));
              },
              .label = format_rupee_slide_pickup_animation,
              .cvarName = []() -> const char* {
                  return getSettings().game.rupeeSlidePickupAnimation.getName();
              },
              .isModified = []() -> bool {
                  const auto& setting = getSettings().game.rupeeSlidePickupAnimation;
                  return setting.getValue() != setting.getDefaultValue();
              },
          },
          DocumentScope::RupeeSlideAnimationTuner) {}

RupeeSlideRupeeTuner::RupeeSlideRupeeTuner(RupeeSlideRupeeOption option)
    : GraphicsTuner(rupee_slide_rupee_title(option), rupee_slide_rupee_help(option),
          rupee_slide_rupee_setting(option), DocumentScope::RupeeSlideRupeeTuner) {}

}  // namespace dusk::ui
