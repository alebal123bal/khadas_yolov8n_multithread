// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Multi-threaded inference pipeline:
//   - N_THREADS rknn contexts, each pinned to a separate NPU core via
//     rknn_dup_context() + rknn_set_core_mask().
//   - Each inference thread owns one context and processes jobs from a shared
//     infer_queue, posting results to result_queue.
//   - Main thread: capture → pre-process → push job, then pop result → draw → display.
//   - A small pool of N_BUF frame buffers (display + model-input pairs) avoids
//     per-frame allocation; ownership follows the pipeline stages.


#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <atomic>
#include <chrono>
#include <dirent.h>

#define _BASETSD_H

#include "RgaUtils.h"
#include "im2d.h"
#include "postprocess.h"
#include "camera_util.h"
#include "rga.h"
#include "rknn_api.h"
#include "rtsp_stream.h"
#include "local_display.h"
#include "model_utils.h"
#include "rga_func.h"

// IPC layer — control plane + data plane.
// See include/ipc/ for the full design.
#include "ipc/wire_protocol.h"
#include "ipc/yolo_control_state.h"
#include "ipc/messages.h"
#include "ipc/unix_control_server.h"
#include "ipc/unix_data_publisher.h"

#define WIDTH     1920
#define HEIGHT    1080
#define N_THREADS 3
#define N_BUF     (N_THREADS + 2)   // slots in the frame-buffer pool

// ---------- thread-safe blocking FIFO ----------
template<typename T>
class TSQueue {
    std::queue<T>           q_;
    std::mutex              mu_;
    std::condition_variable cv_;
public:
    void push(T v) {
        { std::lock_guard<std::mutex> lk(mu_); q_.push(std::move(v)); }
        cv_.notify_one();
    }
    T pop() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this]{ return !q_.empty(); });
        T v = std::move(q_.front()); q_.pop(); return v;
    }
};

// ---------- job pushed by main thread into infer_queue ----------
struct InferJob {
    int      buf_idx;      // index into BufPool; < 0 is a shutdown sentinel
    float    scale_w, scale_h;
    uint64_t frame_id;     // monotonic counter stamped at capture time
};

// ---------- result pushed by inference threads into result_queue ----------
struct InferResult {
    int                   buf_idx;
    detect_result_group_t group;
    uint64_t              frame_id;  // passed through from InferJob
};

// ---------- fixed pool of pre-allocated frame buffers ----------
struct BufPool {
    uint8_t* display[N_BUF];   // full-resolution BGR (WIDTH×HEIGHT×3)
    uint8_t* model[N_BUF];     // model-input RGB (model_w×model_h×3)
    std::queue<int>         free_list;
    std::mutex              mu;
    std::condition_variable cv;

    void init(int model_w, int model_h, int model_ch) {
        for (int i = 0; i < N_BUF; i++) {
            display[i] = new uint8_t[(size_t)WIDTH * HEIGHT * 3];
            model[i]   = new uint8_t[(size_t)model_w * model_h * model_ch];
            free_list.push(i);
        }
    }
    // Blocks until a free slot is available.
    int acquire() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this]{ return !free_list.empty(); });
        int idx = free_list.front(); free_list.pop();
        return idx;
    }
    void release(int idx) {
        { std::lock_guard<std::mutex> lk(mu); free_list.push(idx); }
        cv.notify_one();
    }
    void deinit() {
        for (int i = 0; i < N_BUF; i++) { delete[] display[i]; delete[] model[i]; }
    }
};

// ---------- per-thread state passed to inference_thread ----------
struct ThreadCtx {
    rknn_context             ctx;
    rknn_input               inputs[1];
    std::vector<rknn_output> outputs;
    uint32_t                 n_output;
    int                      model_w, model_h;
    std::vector<float>       out_scales;
    std::vector<int32_t>     out_zps;
    float                    conf_thresh, nms_thresh;
    BufPool*                 pool;
    TSQueue<InferJob>*       infer_q;
    TSQueue<InferResult>*    result_q;
    const YoloControlState*  state;   // read-only: pause/resume flag
};

// Enumerates /proc/self/fd and closes every fd that points to a /dev/rknpu
// device node.  After rknn_destroy(), librknnrt.so keeps that fd open at the
// process level.  The kernel NPU scheduler counts every process with an open
// rknpu fd as an active peer and time-slices between them, so two idle YOLO
// processes plus one LLM each get ~1/3 of NPU time (~13 tok/s for the LLM).
// Closing the fd here removes this process from the scheduler's active set.
static void release_rknpu_fds() {
    std::vector<int> to_close;
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) { perror("opendir /proc/self/fd"); return; }
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        int fd = atoi(de->d_name);
        if (fd <= 2) continue;
        char link[64], target[256];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n > 0) { target[n] = '\0'; if (strstr(target, "rknpu")) to_close.push_back(fd); }
    }
    closedir(dir);
    for (int fd : to_close) { printf("[main] closing rknpu fd %d\n", fd); close(fd); }
    printf("[main] closed %zu rknpu fd(s)\n", to_close.size());
}

// ---------- inference thread (one per NPU core) ----------
// Pops jobs from infer_q, runs RKNN, pushes results to result_q.
// Threads share model weights via rknn_dup_context but each owns an
// independent runtime context pinned to its NPU core.
static void inference_thread(ThreadCtx* tc)
{
    while (true) {
        InferJob job = tc->infer_q->pop();
        if (job.buf_idx < 0) break;    // shutdown sentinel

        InferResult res;
        res.buf_idx  = job.buf_idx;
        res.frame_id = job.frame_id;

        if (tc->state->inference_enabled.load(std::memory_order_relaxed)) {
            // Normal path: run RKNN inference and decode YOLO outputs.
            tc->inputs[0].buf = tc->pool->model[job.buf_idx];  // point descriptor at pre-processed buffer
            rknn_inputs_set(tc->ctx, 1, tc->inputs);           // upload input to NPU
            rknn_run(tc->ctx, NULL);                           // run inference
            rknn_outputs_get(tc->ctx, tc->n_output, tc->outputs.data(), NULL);  // fetch quantised output tensors

            // Decode YOLO anchors into bounding boxes; scale back to display resolution.
            post_process(
                (int8_t*)tc->outputs[0].buf,
                (int8_t*)tc->outputs[1].buf,
                (int8_t*)tc->outputs[2].buf,
                tc->model_h, tc->model_w,
                tc->conf_thresh, tc->nms_thresh,
                job.scale_w, job.scale_h,
                tc->out_zps, tc->out_scales,
                &res.group);

            rknn_outputs_release(tc->ctx, tc->n_output, tc->outputs.data());  // return output buffers to RKNN runtime
        } else {
            // Paused path: skip RKNN, emit an empty result so the pipeline
            // (display, buffer recycling) keeps flowing without stalling.
            res.group = {};
        }

        tc->result_q->push(std::move(res));  // hand result to main thread
    }
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        printf("Usage: %s <rknn model> <device number> <rtsp port|hdmi>\n", argv[0]);
        printf("  RTSP : %s data/model/yolov8n_UAV_640.rknn 33 8554\n", argv[0]);
        printf("  HDMI : %s data/model/yolov8n_UAV_640.rknn 33 hdmi\n", argv[0]);
        return -1;
    }

    char*       model_name    = argv[1];
    std::string device_number = argv[2];
    bool        use_hdmi      = (strcmp(argv[3], "hdmi") == 0);
    int         rtsp_port     = use_hdmi ? 0 : atoi(argv[3]);
    int         ret;

    // ── IPC layer: create shared state and servers ───────────────────────
    // Both servers are started after the model is loaded and before the main
    // loop, so get_status always returns valid data.
    //
    // Socket paths are derived from device_number so that two simultaneous
    // pipeline instances (e.g. camera 33 and camera 51) each own their own
    // independent socket pair and can be controlled independently:
    //   camera 33 → /tmp/yolo_control_33.sock  /tmp/yolo_data_33.sock
    //   camera 51 → /tmp/yolo_control_51.sock  /tmp/yolo_data_51.sock
    YoloControlState state;
    UnixDataPublisher data_pub(ipc_data_path(device_number),    /*queueCapacity=*/32);
    UnixControlServer ctrl_server(ipc_control_path(device_number), state, &data_pub);

    // ---------- load rknn model ----------
    const float nms_threshold      = NMS_THRESH;
    const float box_conf_threshold = BOX_THRESH;
    printf("box_conf_threshold = %.2f, nms_threshold = %.2f\n", box_conf_threshold, nms_threshold);

    printf("Loading model...\n");
    int            model_data_size = 0;
    unsigned char* model_data      = load_model(model_name, &model_data_size);

    // ---------- init base context, pin to core 0 ----------
    rknn_context base_ctx;
    ret = rknn_init(&base_ctx, model_data, model_data_size, 0, NULL);
    if (ret < 0) { printf("rknn_init error ret=%d\n", ret); return -1; }
    rknn_set_core_mask(base_ctx, RKNN_NPU_CORE_0);

    rknn_sdk_version version;
    ret = rknn_query(base_ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    if (ret < 0) { printf("rknn_query SDK_VERSION error ret=%d\n", ret); return -1; }
    printf("sdk version: %s  driver version: %s\n", version.api_version, version.drv_version);

    // ---------- query model i/o ----------
    rknn_input_output_num io_num;
    ret = rknn_query(base_ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) { printf("rknn_query IN_OUT_NUM error ret=%d\n", ret); return -1; }
    printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (uint32_t i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        ret = rknn_query(base_ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret < 0) { printf("rknn_query INPUT_ATTR error ret=%d\n", ret); return -1; }
        dump_tensor_attr(&input_attrs[i]);
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(base_ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        dump_tensor_attr(&output_attrs[i]);
    }

    // ---------- derive model dimensions ----------
    int model_ch = 3, model_w = 0, model_h = 0;
    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        printf("model is NCHW input fmt\n");
        model_ch = input_attrs[0].dims[1];   // NCHW: N,C,H,W
        model_h  = input_attrs[0].dims[2];
        model_w  = input_attrs[0].dims[3];
    } else {
        printf("model is NHWC input fmt\n");
        model_h  = input_attrs[0].dims[1];   // NHWC: N,H,W,C
        model_w  = input_attrs[0].dims[2];
        model_ch = input_attrs[0].dims[3];
    }
    printf("model input height=%d, width=%d, channel=%d\n", model_h, model_w, model_ch);

    // ---------- dequant params (constant per model) ----------
    std::vector<float>   out_scales;
    std::vector<int32_t> out_zps;
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        out_scales.push_back(output_attrs[i].scale);
        out_zps.push_back(output_attrs[i].zp);
    }

    // ---------- duplicate contexts, one per NPU core ----------
    // rknn_dup_context shares model weights; each copy gets its own runtime state.
    static const rknn_core_mask core_masks[N_THREADS] = {
        RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2
    };
    rknn_context ctxs[N_THREADS];
    ctxs[0] = base_ctx;   // already set to RKNN_NPU_CORE_0
    for (int i = 1; i < N_THREADS; i++) {
        ret = rknn_dup_context(&base_ctx, &ctxs[i]);
        if (ret < 0) { printf("rknn_dup_context[%d] error ret=%d\n", i, ret); return -1; }
        rknn_set_core_mask(ctxs[i], core_masks[i]);
    }
    printf("Created %d inference contexts on NPU cores 0/1/2\n", N_THREADS);

    // ---------- buffer pool ----------
    BufPool pool;
    pool.init(model_w, model_h, model_ch);

    // ---------- shared queues ----------
    TSQueue<InferJob>    infer_q;
    TSQueue<InferResult> result_q;

    // ---------- set up per-thread contexts ----------
    ThreadCtx tctxs[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        ThreadCtx& tc   = tctxs[i];
        tc.ctx          = ctxs[i];
        tc.n_output     = io_num.n_output;
        tc.model_w      = model_w;
        tc.model_h      = model_h;
        tc.out_scales   = out_scales;
        tc.out_zps      = out_zps;
        tc.conf_thresh  = box_conf_threshold;
        tc.nms_thresh   = nms_threshold;
        tc.pool         = &pool;
        tc.infer_q      = &infer_q;
        tc.result_q     = &result_q;
        tc.state        = &state;

        // Input descriptor — buf pointer is set per-job inside inference_thread.
        memset(tc.inputs, 0, sizeof(tc.inputs));
        tc.inputs[0].index        = 0;          // single input tensor
        tc.inputs[0].type         = RKNN_TENSOR_UINT8;              // matches pre-processed buffer format
        tc.inputs[0].size         = (uint32_t)(model_w * model_h * model_ch);  // bytes per frame
        tc.inputs[0].fmt          = RKNN_TENSOR_NHWC;               // layout expected by the model
        tc.inputs[0].pass_through = 0;          // let RKNN apply quantisation (1 would bypass it)

        // Output descriptors — want_float=0: keep outputs as int8; post_process dequantises itself.
        tc.outputs.resize(io_num.n_output);
        memset(tc.outputs.data(), 0, io_num.n_output * sizeof(rknn_output));
        for (uint32_t j = 0; j < io_num.n_output; j++)
            tc.outputs[j].want_float = 0;
    }

    // ---------- spawn inference threads ----------
    std::thread threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        threads[i] = std::thread(inference_thread, &tctxs[i]);

    // ── Start IPC servers (non-blocking; each spawns one background thread) ─
    ctrl_server.start();
    data_pub.start();

    // ---------- camera + output ----------
    ret = load_mipi_camera(device_number, WIDTH, HEIGHT);
    if (use_hdmi)
        local_display_init(WIDTH, HEIGHT, ("cam" + device_number).c_str());
    else
        rtsp_stream_init(WIDTH, HEIGHT, 30, rtsp_port);

    // Capture one frame into a pool slot and submit it to the infer queue.
    // frame_counter is owned by the main thread; no synchronisation needed.
    uint64_t frame_counter = 0;
    auto capture_and_submit = [&]() {
        int idx = pool.acquire();
        void* nv12_ptr;
        read_mipi_frame_nv12(&nv12_ptr);
        rga_nv12_to_bgr(nv12_ptr, WIDTH, HEIGHT, pool.display[idx]);
        release_mipi_frame();
        float sw = 0, sh = 0;
        rga_letterbox_rgb(pool.display[idx], WIDTH, HEIGHT,
                          pool.model[idx], model_w, model_h, &sw, &sh);
        infer_q.push({idx, sw, sh, frame_counter++});
        state.frame_id.store(frame_counter, std::memory_order_relaxed);
    };

    // ---------- pre-fill pipeline with N_THREADS frames ----------
    // This keeps all inference threads busy from the first result onward.
    int in_flight = 0;
    for (int i = 0; i < N_THREADS; i++) {
        capture_and_submit();
        ++in_flight;
    }

    // ---------- main loop ----------
    // Pattern (mirrors the Python rknnPoolExecutor):
    //   1. Wait for the oldest inference result.
    //   2. Draw detections and push to output.
    //   3. Recycle the buffer, capture next frame, submit new job.
    float total_time = 0;
    float time_capture = 0, time_display = 0;
    struct timeval start_time, stop_time, t0, t1;
    int n = 0;

    while (!state.shutdown_requested.load(std::memory_order_relaxed)) {

        // ── Blackout ─────────────────────────────────────────────────────────
        // Triggered by the "blackout" IPC command.  Destroys all RKNN contexts
        // and closes the rknpu fd so the LLM gets 100% NPU time.  The main
        // thread then sleeps with zero camera/RGA/stream activity until
        // "resume" is received.
        if (state.blackout_requested.load(std::memory_order_relaxed)) {

            // Reset the handshake-complete flags from the previous cycle under
            // the lifecycle mutex.  resume_done is intentionally left set after
            // a resume (see below), so clear it here — while holding the mutex —
            // before the control thread can wait on the next resume.
            {
                std::lock_guard<std::mutex> lk(state.lifecycle_mutex);
                state.blackout_active.store(false, std::memory_order_relaxed);
                state.resume_done.store(false, std::memory_order_relaxed);
            }

            // Drain in-flight jobs.  inference_enabled is already false, so
            // each thread returns an empty result immediately.
            while (in_flight > 0) {
                InferResult drain = result_q.pop();
                pool.release(drain.buf_idx);
                --in_flight;
            }

            for (int i = 0; i < N_THREADS; i++) rknn_destroy(ctxs[i]);
            release_rknpu_fds();
            printf("[main] blackout: NPU free, stream dark\n");

            {
                std::lock_guard<std::mutex> lk(state.lifecycle_mutex);
                state.blackout_active.store(true, std::memory_order_relaxed);
            }
            state.lifecycle_cv.notify_all();   // unblock IPC "blackout" handler

            // Zero-resource idle loop.  V4L2 keeps buffering frames in the
            // kernel (ISP DMA is unavoidable) but we do nothing in userspace.
            // RTSP/HDMI freezes on the last frame pushed before blackout.
            while (!state.resume_requested.load(std::memory_order_relaxed)
                && !state.shutdown_requested.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (state.shutdown_requested.load(std::memory_order_relaxed)) break;

            // Re-init RKNN contexts.
            ret = rknn_init(&ctxs[0], model_data, model_data_size, 0, NULL);
            if (ret < 0) {
                printf("[main] rknn_init (resume) failed: %d\n", ret);
                state.shutdown_requested.store(true, std::memory_order_relaxed);
                break;
            }
            rknn_set_core_mask(ctxs[0], RKNN_NPU_CORE_0);
            for (int i = 1; i < N_THREADS; i++) {
                rknn_dup_context(&ctxs[0], &ctxs[i]);
                rknn_set_core_mask(ctxs[i], core_masks[i]);
            }
            for (int i = 0; i < N_THREADS; i++) tctxs[i].ctx = ctxs[i];

            // Drain stale V4L2 frames that buffered during blackout.
            { void* tmp; for (int i = 0; i < 4; i++) { read_mipi_frame_nv12(&tmp); release_mipi_frame(); } }

            // Publish the resume handshake under the lifecycle mutex and LEAVE
            // resume_done set until the next blackout clears it (above).
            //
            // Previously resume_done was set true and then immediately reset to
            // false right here, outside the mutex.  That raced the control
            // thread: the "resume" handler is parked in wait_for(15s,
            // [resume_done]); when it woke and re-checked the predicate, this
            // reset had often already fired, so it missed the wakeup and blocked
            // for the full 15 s timeout.  Because event_summarizer issues
            // resumes sequentially (device, then each --also-device), that
            // 15 s stall on the FIRST resume delayed the second camera by ~15 s
            // even though its pipeline was ready immediately.
            {
                std::lock_guard<std::mutex> lk(state.lifecycle_mutex);
                state.blackout_active.store(false, std::memory_order_relaxed);
                state.inference_enabled.store(true, std::memory_order_relaxed);
                state.resume_done.store(true, std::memory_order_relaxed);
            }
            state.lifecycle_cv.notify_all();   // unblock IPC "resume" handler

            // Clear only the consumed request flags; the handshake-complete
            // flags (resume_done / blackout_active) are reset at the next
            // blackout entry, so the control thread reliably observes them.
            state.blackout_requested.store(false, std::memory_order_relaxed);
            state.resume_requested.store(false, std::memory_order_relaxed);

            for (int i = 0; i < N_THREADS; i++) { capture_and_submit(); ++in_flight; }
            printf("[main] resumed\n");
            continue;
        }

        gettimeofday(&start_time, NULL);

        // 1. wait for an inference result (blocks until one thread finishes)
        InferResult res = result_q.pop();
        --in_flight;

        // 2. draw detections on the display buffer
        for (int i = 0; i < res.group.count; i++) {
            detect_result_t* det = &res.group.results[i];
            draw_box_sw(pool.display[res.buf_idx], WIDTH, HEIGHT,
                        det->box.left,  det->box.top,
                        det->box.right  - det->box.left,
                        det->box.bottom - det->box.top,
                        0xFFA500, 6);
        }

        // 3. push frame to output
        gettimeofday(&t0, NULL);
        if (use_hdmi)
            local_display_push_frame(pool.display[res.buf_idx], WIDTH, HEIGHT);
        else
            rtsp_stream_push_frame(pool.display[res.buf_idx], WIDTH, HEIGHT);
        gettimeofday(&t1, NULL);
        time_display += (__get_us(t1) - __get_us(t0)) / 1000;

        // 4. publish detections to the data plane (non-blocking).
        {
            DetectionMessage dmsg;
            dmsg.frame_id     = res.frame_id;
            dmsg.timestamp_us = static_cast<uint64_t>(__get_us(start_time));
            dmsg.group        = res.group;
            data_pub.publish(std::move(dmsg));
        }

        // 5. recycle buffer, capture + preprocess next frame, submit
        pool.release(res.buf_idx);
        gettimeofday(&t0, NULL);
        capture_and_submit();
        ++in_flight;
        gettimeofday(&t1, NULL);
        time_capture += (__get_us(t1) - __get_us(t0)) / 1000;

        // print perf summary every 10 frames
        gettimeofday(&stop_time, NULL);
        total_time += (__get_us(stop_time) - __get_us(start_time)) / 1000;
        if (++n == 10) {
            const float avg_ms = total_time / 10;
            const float fps    = 1000.0f / avg_ms;
            state.fps.store(fps, std::memory_order_relaxed);
            printf("--- avg over 10 frames ---\n");
            printf("  capture+preproc : %6.2f ms\n", time_capture / 10);
            printf("  display         : %6.2f ms\n", time_display / 10);
            printf("  total (main)    : %6.2f ms  (%.1f FPS)\n", avg_ms, fps);
            total_time = 0;
            time_capture = time_display = 0;
            n = 0;
        }
    }

    // ---------- teardown ----------
    // Stop ctrl_server before data_pub: a concurrent get_status handler calls
    // dataPub_->queueDepth(), so the control thread must be joined before
    // data_pub is torn down.
    ctrl_server.stop();
    data_pub.stop();

    for (int i = 0; i < N_THREADS; i++)
        infer_q.push({-1, 0.f, 0.f, 0});
    for (int i = 0; i < N_THREADS; i++)
        threads[i].join();

    close_mipi_camera();
    if (use_hdmi)
        local_display_deinit();
    else
        rtsp_stream_deinit();

    pool.deinit();
    deinitPostProcess();
    for (int i = 0; i < N_THREADS; i++) rknn_destroy(ctxs[i]);
    free(model_data);

    return 0;
}
