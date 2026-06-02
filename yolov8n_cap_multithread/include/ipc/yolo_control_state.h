#pragma once

// yolo_control_state.h — Shared atomic state between the inference pipeline
// and the IPC control server.
//
// All fields are std::atomic so they can be read/written from different
// threads without any explicit locking.
//
// Ownership model:
//   Writer                 │ Field
//   ───────────────────────┼──────────────────────────────────
//   UnixControlServer      │ inference_enabled, dump_enabled,
//                          │ shutdown_requested
//   main loop              │ frame_id, fps
//   ───────────────────────┼──────────────────────────────────
//   Reader                 │ Field
//   ───────────────────────┼──────────────────────────────────
//   inference_thread(s)    │ inference_enabled
//   main loop              │ shutdown_requested, dump_enabled
//   UnixControlServer      │ all (for get_status)
//
// Memory ordering: relaxed loads/stores are used everywhere because:
//   1. Commands are low-frequency; a few nanoseconds of stale state is fine.
//   2. No sequential-consistency guarantee is needed between state fields.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

struct YoloControlState {
    /// When false, inference threads skip the RKNN call and emit 0-detection
    /// results, keeping the pipeline (capture, display, recycling) running.
    std::atomic<bool>     inference_enabled{true};

    /// When true, future diagnostics (e.g. frame dumps) will be active.
    /// Currently read in main.cc; the hook is wired but no-op.
    std::atomic<bool>     dump_enabled{false};

    /// Set by the "shutdown" control command. The main loop checks this flag
    /// to exit the while(1) loop cleanly rather than running indefinitely.
    std::atomic<bool>     shutdown_requested{false};

    /// Incremented once per captured frame (before the InferJob is pushed).
    std::atomic<uint64_t> frame_id{0};

    /// Smoothed FPS of the main loop, updated every 10 frames.
    std::atomic<float>    fps{0.f};

    // Blackout lifecycle — complete resource release so the LLM can use the
    // NPU at full throughput.  Flow:
    //   IPC "blackout" → inference_enabled=false, blackout_requested=true
    //                  → blocks on lifecycle_cv until blackout_active==true
    //   main loop      → drains jobs, destroys RKNN, closes rknpu fds
    //                  → sets blackout_active, notifies cv
    //                  → sleeps (no camera/RGA/stream) until resume_requested
    //   IPC "resume"   → resume_requested=true, notifies cv
    //                  → blocks on lifecycle_cv until resume_done==true
    //   main loop      → re-inits RKNN, re-fills pipeline
    //                  → sets resume_done, notifies cv
    std::atomic<bool>       blackout_requested{false};
    std::atomic<bool>       blackout_active{false};
    std::atomic<bool>       resume_requested{false};
    std::atomic<bool>       resume_done{false};
    std::mutex              lifecycle_mutex;
    std::condition_variable lifecycle_cv;

    // Atomics are not copy-constructible.
    YoloControlState()                                   = default;
    YoloControlState(const YoloControlState&)            = delete;
    YoloControlState& operator=(const YoloControlState&) = delete;
};
