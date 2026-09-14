#pragma once

#include "samples.h"

#include <array>

namespace dusk::interp {

struct MenuValues {
    Samples<f32> values;
};

template <typename... Values>
void capture_menu_values(const void* owner, Values... values) {
    if (!should_capture()) {
        return;
    }

    const f32 pose[] = {static_cast<f32>(values)...};
    get<MenuValues>(owner).values.capture(pose, sizeof...(Values));
}

template <size_t Count>
class ScopedMenuValues {
public:
    template <typename... Values>
    ScopedMenuValues(const void* owner, Values&... values) : mValues{&values...}, mSaved{values...} {
        if (is_enabled() && is_presentation_active()) {
            const auto& samples = get<MenuValues>(owner).values;
            for (size_t i = 0; i < Count; ++i) {
                *mValues[i] = samples.read(i, mSaved[i]);
            }
        }
    }

    ~ScopedMenuValues() {
        for (size_t i = 0; i < Count; ++i) {
            *mValues[i] = mSaved[i];
        }
    }

    ScopedMenuValues(const ScopedMenuValues&) = delete;
    ScopedMenuValues& operator=(const ScopedMenuValues&) = delete;

private:
    std::array<f32*, Count> mValues;
    std::array<f32, Count> mSaved;
};

template <typename... Values>
ScopedMenuValues(const void*, Values&...) -> ScopedMenuValues<sizeof...(Values)>;

}  // namespace dusk::interp
