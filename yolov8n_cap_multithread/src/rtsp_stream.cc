#include "rtsp_stream.h"

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

#include <pthread.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static GstElement*      g_appsrc   = nullptr;
static GstRTSPServer*   g_server   = nullptr;
static GMainLoop*       g_loop     = nullptr;
static pthread_t        g_thread;
static pthread_mutex_t  g_mutex    = PTHREAD_MUTEX_INITIALIZER;

static int   g_width  = 0;
static int   g_height = 0;
static int   g_fps    = 30; // TODO: is this the max FPS we can push?  

// ---------------------------------------------------------------------------
// GLib main loop runs in a background thread so it doesn't block the pipeline
// ---------------------------------------------------------------------------
static void* glib_loop_thread(void*) {
    g_main_loop_run(g_loop);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Called once when the first client connects and the media pipeline is built.
// We grab the appsrc element and configure its caps.
// ---------------------------------------------------------------------------
static void on_media_configure(GstRTSPMediaFactory*, GstRTSPMedia* media, gpointer) {
    GstElement* pipeline = gst_rtsp_media_get_element(media);
    GstElement* appsrc   = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "src");

    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format",    G_TYPE_STRING,     "BGR",
        "width",     G_TYPE_INT,        g_width,
        "height",    G_TYPE_INT,        g_height,
        "framerate", GST_TYPE_FRACTION, g_fps, 1,
        NULL);

    g_object_set(G_OBJECT(appsrc),
        "caps",         caps,
        "format",       GST_FORMAT_TIME,
        "is-live",      TRUE,
        "do-timestamp", TRUE,
        NULL);

    gst_caps_unref(caps);
    gst_object_unref(pipeline);

    pthread_mutex_lock(&g_mutex);
    if (g_appsrc) gst_object_unref(g_appsrc);
    g_appsrc = appsrc;   // takes ownership of the ref from gst_bin_get_by_name_recurse_up
    pthread_mutex_unlock(&g_mutex);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void rtsp_stream_init(int width, int height, int fps, int port, const char* path) {
    g_width  = width;
    g_height = height;
    g_fps    = fps;

    gst_init(nullptr, nullptr);

    g_loop   = g_main_loop_new(nullptr, FALSE);
    g_server = gst_rtsp_server_new();

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    gst_rtsp_server_set_service(g_server, port_str);

    GstRTSPMountPoints*  mounts  = gst_rtsp_server_get_mount_points(g_server);
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();

    // Hardware H264 via RK3588S MPP.
    // header-mode=1  → prepend SPS/PPS before every IDR (helps decoders reconnect)
    // gop=30         → one IDR per second at 30fps; lower = less latency on connect
    // bps=4000000    → ~4 Mbps ceiling
    // To use software encoding instead:
    //   replace "mpph264enc ..." with "x264enc tune=zerolatency"
    gst_rtsp_media_factory_set_launch(factory,
        "( appsrc name=src is-live=true ! "
        "  videoconvert ! video/x-raw,format=I420 ! "
        "  mpph264enc header-mode=1 gop=30 bps=4000000 ! "
        "  rtph264pay name=pay0 pt=96 config-interval=1 )");

    // Share one pipeline across all clients
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), nullptr);

    gst_rtsp_mount_points_add_factory(mounts, path, factory);
    g_object_unref(mounts);

    gst_rtsp_server_attach(g_server, nullptr);
    pthread_create(&g_thread, nullptr, glib_loop_thread, nullptr);

    printf("RTSP server ready  →  rtsp://<board-ip>:%d%s\n", port, path);
    printf("  play with: ffplay rtsp://<board-ip>:%d%s\n", port, path);
}

void rtsp_stream_push_frame(const uint8_t* bgr, int width, int height) {
    pthread_mutex_lock(&g_mutex);
    GstElement* appsrc = g_appsrc;
    if (appsrc) gst_object_ref(appsrc);
    pthread_mutex_unlock(&g_mutex);

    if (!appsrc) return;   // no client connected yet

    gsize size = (gsize)width * height * 3;
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);

    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    memcpy(map.data, bgr, size);
    gst_buffer_unmap(buf, &map);

    // do-timestamp=TRUE on the appsrc handles PTS automatically;
    // we only need to set a nominal duration for the decoder's jitter buffer.
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale_int(GST_SECOND, 1, g_fps);

    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);
    gst_object_unref(appsrc);
}

void rtsp_stream_deinit() {
    pthread_mutex_lock(&g_mutex);
    if (g_appsrc) {
        GstFlowReturn ret;
        g_signal_emit_by_name(g_appsrc, "end-of-stream", &ret);
        gst_object_unref(g_appsrc);
        g_appsrc = nullptr;
    }
    pthread_mutex_unlock(&g_mutex);

    if (g_loop) {
        g_main_loop_quit(g_loop);
        pthread_join(g_thread, nullptr);
        g_main_loop_unref(g_loop);
        g_loop = nullptr;
    }
    if (g_server) {
        gst_object_unref(g_server);
        g_server = nullptr;
    }
}
