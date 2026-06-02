#pragma once

#include <stdint.h>

/**
 * Start a GStreamer RTSP server in a background GLib thread.
 *
 * Uses mpph264enc (RK3588S hardware H264) for encoding.
 * If mpph264enc is unavailable on the board, replace it with
 * "x264enc tune=zerolatency" in the pipeline string inside rtsp_stream.cc.
 *
 * View the stream with:
 *   ffplay rtsp://<board-ip>:<port><path>
 *   vlc    rtsp://<board-ip>:<port><path>
 */
void rtsp_stream_init(int width, int height, int fps,
                      int port = 8554, const char* path = "/stream");

/**
 * Push one BGR frame to all connected RTSP clients.
 * No-op if no client is connected yet.
 */
void rtsp_stream_push_frame(const uint8_t* bgr, int width, int height);

/** Flush, send EOS, and tear down the server. */
void rtsp_stream_deinit();
