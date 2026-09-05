#pragma once

#include <string>
#include <string_view>

namespace dusk::android {

// Convert an Android content URI into a regular file in the app cache. Non-Android
// paths and already-resolved filesystem paths are returned unchanged.
std::string materialize_content_uri(std::string_view location);

}  // namespace dusk::android
