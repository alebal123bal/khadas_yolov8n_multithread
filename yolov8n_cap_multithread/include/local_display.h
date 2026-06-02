#pragma once

#include <stdint.h>

/**
 * Open a local GStreamer window on the connected display (HDMI/Wayland/X11).
 * Uses autovideosink so it works on both Wayland and X11 sessions.
 * Near-zero latency — no encode/decode/network round trip.
 */
void local_display_init(int width, int height, const char* title = "yolov8");

/**
 * Blit one BGR frame to the window.
 */
void local_display_push_frame(const uint8_t* bgr, int width, int height);

/** Tear down the GStreamer pipeline and window. */
void local_display_deinit();
