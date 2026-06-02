#include "local_display.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <pthread.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static GstElement*  g_pipeline = nullptr;
static GstElement*  g_appsrc   = nullptr;
static GMainLoop*   g_loop     = nullptr;
static pthread_t    g_thread;

static int  g_width  = 0;
static int  g_height = 0;

// ---------------------------------------------------------------------------
// GLib main loop in a background thread (drives GStreamer bus messages)
// ---------------------------------------------------------------------------
static void* glib_loop_thread(void*)
{
    g_main_loop_run(g_loop);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void local_display_init(int width, int height, const char* title)
{
    g_width  = width;
    g_height = height;

    gst_init(nullptr, nullptr);
    g_loop = g_main_loop_new(nullptr, FALSE);

    // appsrc feeds raw BGR directly to the display — no encode/decode at all.
    // autovideosink selects waylandsink on Wayland sessions, ximagesink on X11.
    // fpsdisplaysink wraps it to overlay the live FPS counter.
    char pipeline_str[512];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "appsrc name=src is-live=true do-timestamp=true format=time "
        "caps=\"video/x-raw,format=BGR,width=%d,height=%d,framerate=30/1\" ! "
        "videoconvert ! "
        "fpsdisplaysink name=sink video-sink=autovideosink "
        "text-overlay=true sync=false",
        width, height);

    GError* err = nullptr;
    g_pipeline = gst_parse_launch(pipeline_str, &err);
    if (!g_pipeline || err) {
        fprintf(stderr, "local_display_init: %s\n", err ? err->message : "unknown error");
        if (err) g_error_free(err);
        return;
    }

    // Set window title on the underlying sink
    GstElement* fpssink = gst_bin_get_by_name(GST_BIN(g_pipeline), "sink");
    if (fpssink) {
        GstElement* videosink = nullptr;
        g_object_get(fpssink, "video-sink", &videosink, nullptr);
        if (videosink) {
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(videosink), "window-title"))
                g_object_set(videosink, "window-title", title, nullptr);
            gst_object_unref(videosink);
        }
        gst_object_unref(fpssink);
    }

    g_appsrc = gst_bin_get_by_name(GST_BIN(g_pipeline), "src");

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    pthread_create(&g_thread, nullptr, glib_loop_thread, nullptr);

    printf("Local display active  →  %dx%d window \"%s\"\n", width, height, title);
}

void local_display_push_frame(const uint8_t* bgr, int width, int height)
{
    if (!g_appsrc) return;

    gsize size = (gsize)width * height * 3;
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);

    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    memcpy(map.data, bgr, size);
    gst_buffer_unmap(buf, &map);

    GstFlowReturn ret;
    g_signal_emit_by_name(g_appsrc, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);
}

void local_display_deinit()
{
    if (g_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(g_appsrc));
        gst_object_unref(g_appsrc);
        g_appsrc = nullptr;
    }
    if (g_pipeline) {
        gst_element_set_state(g_pipeline, GST_STATE_NULL);
        gst_object_unref(g_pipeline);
        g_pipeline = nullptr;
    }
    if (g_loop) {
        g_main_loop_quit(g_loop);
        pthread_join(g_thread, nullptr);
        g_main_loop_unref(g_loop);
        g_loop = nullptr;
    }
}
