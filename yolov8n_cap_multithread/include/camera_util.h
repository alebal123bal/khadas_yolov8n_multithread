#ifndef _CAMERA_UTIL_H_
#define _CAMERA_UTIL_H_

#include <string>
#include <stddef.h>

#define REQ_COUNT 4

struct Buffer {
    void* start;
    size_t length;
};

int  load_mipi_camera(std::string device, int camera_width, int camera_height);

// Dequeue the next frame; *nv12 points into the MMAP buffer.
// Valid until release_mipi_frame() is called.
void read_mipi_frame_nv12(void** nv12);

// Re-queue the current frame buffer back to the kernel.
void release_mipi_frame();

void close_mipi_camera();

#endif
