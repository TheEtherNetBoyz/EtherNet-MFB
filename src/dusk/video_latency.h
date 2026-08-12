#pragma once

namespace dusk::video_latency {

// Captures the completed GX framebuffer and replaces it with the frame whose
// capture time best matches the configured presentation delay.
void process();

}  // namespace dusk::video_latency
