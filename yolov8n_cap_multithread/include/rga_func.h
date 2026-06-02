#pragma once

#include <stdint.h>

// Convert NV12 (YUV420SP) to BGR888 using RGA hardware.
// src_nv12: contiguous Y+UV buffer (width × height × 3/2 bytes, V4L2 mmap).
// dst_bgr:  pre-allocated BGR buffer (width × height × 3 bytes).
void rga_nv12_to_bgr(void* src_nv12, int width, int height, void* dst_bgr);

// Convert YUYV (YUV422 packed) to BGR888 using RGA hardware.
// src_yuyv: YUYV buffer (width × height × 2 bytes, V4L2 mmap).
// dst_bgr:  pre-allocated BGR buffer (width × height × 3 bytes).
// Requires: width 8-aligned, height 2-aligned (RGA3 constraint).
// Note: use YUYV V4L2 format instead of MJPEG so that RGA can be used;
//       MJPEG requires CPU JPEG decode (cv::imdecode) and cannot use RGA.
void rga_yuyv_to_bgr(void* src_yuyv, int width, int height, void* dst_bgr);

// Resize + letterbox a BGR888 frame into a flat RGB888 model-input buffer
// using RGA.  RGA performs the BGR→RGB channel swap during the resize pass.
// Fills unused area (bottom for landscape, right for portrait) with black to
// match the padding convention used by post_process().
// *out_scale_w == *out_scale_h (uniform scale); set each call (constant for
// fixed camera dimensions).
void rga_letterbox_rgb(void* src_bgr, int src_w, int src_h,
                       void* dst_buf, int model_w, int model_h,
                       float* out_scale_w, float* out_scale_h);

// NOTE: RGA3 on RK3588S does not support colour fill (imfill) for 24-bit
// BGR/RGB 888 formats.  Use draw_box_sw for bounding-box drawing.

// Draw a solid-colour bounding box on a BGR888 buffer (software fallback).
// x,y: top-left corner; w,h: box dimensions; thickness: line width in pixels.
// colour_rgb: 0xRRGGBB packed.
void draw_box_sw(uint8_t* bgr, int img_w, int img_h,
                 int x, int y, int w, int h,
                 uint32_t colour_rgb, int thickness);
