#include "rga_func.h"
#include <cstddef>
#include "im2d.h"
#include "rga.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// NV12 → BGR  (same dimensions, pure colour-space conversion)
// ---------------------------------------------------------------------------
void rga_nv12_to_bgr(void* src_nv12, int width, int height, void* dst_bgr)
{
    rga_buffer_t src = wrapbuffer_virtualaddr(src_nv12, width, height,
                                              RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_bgr,  width, height,
                                              RK_FORMAT_BGR_888);
    IM_STATUS ret = imcvtcolor(src, dst,
                               RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
    if (ret != IM_STATUS_SUCCESS)
        fprintf(stderr, "rga_nv12_to_bgr: %s\n", imStrError(ret));
}

// ---------------------------------------------------------------------------
// YUYV → BGR  (same dimensions, pure colour-space conversion)
// ---------------------------------------------------------------------------
void rga_yuyv_to_bgr(void* src_yuyv, int width, int height, void* dst_bgr)
{
    rga_buffer_t src = wrapbuffer_virtualaddr(src_yuyv, width, height,
                                              RK_FORMAT_YUYV_422);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_bgr,  width, height,
                                              RK_FORMAT_BGR_888);
    IM_STATUS ret = imcvtcolor(src, dst,
                               RK_FORMAT_YUYV_422, RK_FORMAT_BGR_888);
    if (ret != IM_STATUS_SUCCESS)
        fprintf(stderr, "rga_yuyv_to_bgr: %s\n", imStrError(ret));
}

// ---------------------------------------------------------------------------
// BGR resize + letterbox → RGB model input buffer
// ---------------------------------------------------------------------------
void rga_letterbox_rgb(void* src_bgr, int src_w, int src_h,
                       void* dst_buf, int model_w, int model_h,
                       float* out_scale_w, float* out_scale_h)
{
    float scale;
    int resize_w, resize_h;

    if (src_w > src_h) {
        scale    = (float)model_w / src_w;
        resize_w = model_w;
        resize_h = (int)(scale * src_h);
    } else {
        scale    = (float)model_h / src_h;
        resize_h = model_h;
        resize_w = (int)(scale * src_w);
    }
    *out_scale_w = scale;
    *out_scale_h = scale;

    // Pass 1: fill the whole destination with black (letterbox background).
    // memset is used instead of RGA imfill because RGA3 (RK3588S) does not
    // support colour fill for 24-bit BGR/RGB 888 formats.
    memset(dst_buf, 0, (size_t)model_w * model_h * 3);

    // Pass 2: resize source into the top-left of the destination.
    //
    // We use wrapbuffer_virtualaddr_t with explicit wstride=model_w so that
    // RGA advances model_w*3 bytes per output row.  This places the resized
    // image flush against the top edge of the model_w × model_h buffer,
    // leaving the remaining rows black from pass 1.
    //
    // Assumption: for landscape input (src_w > src_h), resize_w == model_w,
    // so the stride equals the image width and the sub-buffer trick is always
    // valid (no horizontal gap).  Portrait mode is handled the same way but
    // leaves a right-side gap that is covered by the black fill above.
    rga_buffer_t src     = wrapbuffer_virtualaddr(src_bgr, src_w, src_h,
                                                  RK_FORMAT_BGR_888);
    rga_buffer_t dst_sub = wrapbuffer_virtualaddr_t(dst_buf,
                                                    resize_w, resize_h,
                                                    model_w, model_h,
                                                    RK_FORMAT_RGB_888);
    IM_STATUS ret = imresize(src, dst_sub);
    if (ret != IM_STATUS_SUCCESS)
        fprintf(stderr, "rga_letterbox_rgb: %s\n", imStrError(ret));
}

// rga_draw_box is not implemented: RGA3 (RK3588S) does not support imfill
// for 24-bit BGR/RGB 888 formats.  Use draw_box_sw instead.

// ---------------------------------------------------------------------------
// Software bounding-box drawing (BGR888)
// ---------------------------------------------------------------------------
void draw_box_sw(uint8_t* bgr, int img_w, int img_h,
                 int x, int y, int w, int h,
                 uint32_t colour_rgb, int thickness)
{
    uint8_t r = (colour_rgb >> 16) & 0xFF;
    uint8_t g = (colour_rgb >>  8) & 0xFF;
    uint8_t b =  colour_rgb        & 0xFF;

    int x0 = x < 0       ? 0       : x;
    int y0 = y < 0       ? 0       : y;
    int x1 = (x + w) > img_w ? img_w : (x + w);
    int y1 = (y + h) > img_h ? img_h : (y + h);

    if (x0 >= x1 || y0 >= y1) return;

    for (int t = 0; t < thickness; t++) {
        // top edge
        int row = y0 + t;
        if (row < img_h)
            for (int col = x0; col < x1; col++) {
                uint8_t* p = bgr + (row * img_w + col) * 3;
                p[0] = b; p[1] = g; p[2] = r;
            }
        // bottom edge
        row = y1 - 1 - t;
        if (row >= 0 && row < img_h)
            for (int col = x0; col < x1; col++) {
                uint8_t* p = bgr + (row * img_w + col) * 3;
                p[0] = b; p[1] = g; p[2] = r;
            }
        // left edge
        int col = x0 + t;
        if (col < img_w)
            for (int row2 = y0; row2 < y1; row2++) {
                uint8_t* p = bgr + (row2 * img_w + col) * 3;
                p[0] = b; p[1] = g; p[2] = r;
            }
        // right edge
        col = x1 - 1 - t;
        if (col >= 0 && col < img_w)
            for (int row2 = y0; row2 < y1; row2++) {
                uint8_t* p = bgr + (row2 * img_w + col) * 3;
                p[0] = b; p[1] = g; p[2] = r;
            }
    }
}
