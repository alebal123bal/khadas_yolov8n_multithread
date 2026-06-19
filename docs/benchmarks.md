# Benchmarks

Performance measurements for the multi-threaded YOLOv8n pipeline on the Khadas
Edge2 (RK3588S).

> **Status:** measured on Khadas Edge2 (RK3588S), 1080p OS08A10 camera. The
> latency breakdown is reported for a single representative config
> (`best_yolov8n_800_relu.rknn`, single stream).

---

## Test environment

| Field | Value |
|-------|-------|
| Board | Khadas Edge2 (RK3588S) — see [hardware_specs.md](hardware_specs.md) |
| OS / firmware / libs | see [software_specs.md](software_specs.md) |
| CPU pinning | see [usage.md](usage.md) |
| Build | `Release` (`build.sh`) |

### Methodology

- **Input source:** 1080p OS08A10 MIPI camera. FPS/CPU/RAM runs use the live
  camera; the latency breakdown was captured with HDMI output (`hdmi`).
- **CPU pinning:** process pinned to the four big A76 cores with
  `taskset -c 4-7` (see [usage.md](usage.md)).
- **Build:** `Release` (`build.sh`).
- **FPS:** measured at the **main loop** (end-to-end: capture+preprocess →
  display), reported by the program's built-in counter, averaged over rolling
  10-frame windows once the reading had stabilized. NPU inference/postprocess
  run in parallel and are off the main loop's critical path.
- **End-to-end latency:** measured in-process by the pipeline's built-in probe.
  A capture timestamp is stamped in `capture_and_submit()` and carried through
  `InferJob → InferResult`; the main loop computes `display_time − capture_time`
  and reports it (`end-to-end lat.`) in the same rolling 10-frame window as FPS.
  This covers the full software pipeline (userspace capture → display push) but
  excludes sensor exposure/readout and downstream sink buffering.
- **Inference time (NPU):** measured **on the worker thread** around the RKNN
  block (`rknn_inputs_set` → `rknn_run` → `rknn_outputs_get` → `post_process` →
  `rknn_outputs_release`), carried back in `InferResult::infer_us`, and reported
  as `inference (NPU)` in the same 10-frame window. It is the per-frame wall
  time on **one** core (the three cores run in parallel), so it is hidden from
  throughput but is a component of end-to-end latency. The figure includes the
  CPU-side decode/NMS (`post_process`), not just `rknn_run`.
- **CPU utilization:** average busy% across cores 4–7, e.g.
  `mpstat -P 4-7 5 1` (`100 − %idle`).
- **NPU utilization:** per-core load from `/sys/kernel/debug/rknpu/load`
  (cores 0/1/2), balanced across cores by design.
- **RAM:** process **RSS** via `pidstat -r` / `ps -o rss`. For dual-stream the
  figure is the **sum of both processes' RSS** (note: shared RKNN/RGA library
  pages are double-counted; use PSS via `smem` for a stricter footprint).
- **Lighting:** well-lit scene to avoid AEC-induced exposure lengthening (see
  caveat under [Latency breakdown](#latency-breakdown)).

---

## Throughput (FPS)

FPS by thread/core configuration. Input source: 1080p camera (OS08A10),
YOLOv8n at 640×640.

**🚀Performance breakthrough:** moving from the naïve single-threaded
capture → preprocess → inference → postprocess → display loop to the
multi-threaded pipeline lifted throughput from **~31.2 FPS** all the way to the
**46 FPS** hardware ceiling — the maximum frame-rate the OS08A10 camera sensor
can deliver — at the same 1080p input and 640×640 detection size.

| Configuration | FPS |
|---|---|
| Naïve single-threaded loop (cap→pre→infer→post→display) | ~31.2 |
| Multi-threaded pipeline (this version) | 46 (camera-limited) |

Notes: at 46 FPS the pipeline is bound by the OS08A10 sensor's frame-rate
ceiling, not by NPU inference or any pipeline stage.

---

## Per-model results

Models live in `data/model/`. Models with a `_relu` suffix use ReLU
activations. NPU utilization is reported per core but is balanced across cores
0/1/2 (one inference context each); CPU utilization refers to the four big A76
cores the pipeline is pinned to.

| Model | Input size | Stream | FPS | Inference (ms) | End-to-end (ms) | NPU cores 0/1/2 | CPU (4x A76) | RAM (RSS) |
|---|---|---|---|---|---|---|---|---|
| `best_yolov8n_768.rknn` | 768 | single | 46.5 | 25.3 | 64.5 | 35% | 29.2% | 138 MB |
| `best_yolov8n_768.rknn` | 768 | dual | 41.0 | 55.2 | 73.3 | 75% | 59.3% | 276 MB |
| `best_yolov8n_768_relu.rknn` | 768 | single | 46.5 | 18.0 | 64.6 | 25% | 28.7% | 137 MB |
| `best_yolov8n_768_relu.rknn` | 768 | dual | 46.5 | 40.5 | 64.7 | 65% | 69.1% | 281 MB |
| `best_yolov8n_800_relu.rknn` | 800 | single | 46.5 | 20.3 | 64.7 | 29% | 27.5% | 142 MB |
| `best_yolov8n_800_relu.rknn` | 800 | dual | 41.5 | 51.8 | 72.8 | 69% | 63.1% | 283 MB |
| `best_yolov8n_896_relu.rknn` | 896 | single | 46.5 | 25 | 64.8 | 35% | 28.4% | 152 MB |
| `best_yolov8n_896_relu.rknn` | 896 | dual | 38 | 57 |76.9 | 73% | 60.9% | 304 MB |
| `best_yolov8n_1024_relu.rknn` | 1024 | single | — | — | — | — | — | _exercise for the reader_ 😉 |
| `best_yolov8n_1280_relu.rknn` | 1280 | single | 43.6 | 55.1 | 68.8 | 73% | 30% | 218 MB |
| `best_yolov8n_1280_relu.rknn` | 1280 | dual | 19.8 | 112.8 | 139 | 82% | 65% | 440 MB |

> **💡Inference time scales quadratically with input size.** NPU inference cost is
> proportional to the number of input pixels, i.e. to `input_size²`. So once you
> have measured inference time at one resolution you can predict another:
>
> ```
> t(size_b)  ≈  t(size_a) × (size_b² / size_a²)
> ```
>
> Example: a 1280×1280 model measured at **55.5 ms** predicts the 768×768 model at
>
> ```
> 55.5 ms × (768² / 1280²)  =  55.5 ms × (589824 / 1638400)  ≈  19.9 ms
> ```
>
> which matches the measured ~18 ms closely. Use this rule of thumb to estimate
> the inference column for resolutions you have not benchmarked directly.

---

## Latency breakdown

### Two different numbers: throughput vs. end-to-end latency

It is essential to separate two metrics that are easy to conflate in a
pipelined system:

- **Throughput (pipeline frame-rate)** — how many frames per second leave the
  pipeline in steady state. This is what the program's built-in counter reports
  and what every FPS figure in this document refers to. It is the reciprocal of
  the **main-loop period**, *not* the time any single frame spends in the
  pipeline.
- **End-to-end latency ("glass-to-glass")** — for one specific frame, the wall
  time from the moment it is captured to the moment its annotated version is
  pushed to the display/RTSP sink. This is **larger** than `1 / throughput` and
  is now reported by the pipeline's built-in probe (`end-to-end lat.`,
  see [Measuring end-to-end latency](#measuring-end-to-end-latency)).

The two diverge precisely *because* the pipeline overlaps stages. A frame is
captured, then handed off to an NPU thread while the main thread immediately
moves on to capture the next frame. Several frames are therefore "in flight"
at once: throughput is set by the slowest single stage, while latency is the
*sum* of all stages a frame traverses plus the time it waits in queues.

### Why this pipeline's latency ≈ depth × period

The main thread runs a fixed sequence per iteration (see `main()` in
[main.cc](../yolov8n_cap_multithread/src/main.cc)):

1. `result_q.pop()` — take the oldest finished result.
2. draw boxes + push to display/RTSP.
3. `capture_and_submit()` — capture + RGA-preprocess the next frame and push an
   `InferJob` onto `infer_q`.

Capture + preprocess (~20.3 ms) and display (~1.1 ms) run **serially on the
main thread**, so they fix the main-loop period at ~21.5 ms → ~46.5 FPS. NPU
inference and post-process run on the three worker threads (one RKNN context
per core) and overlap entirely with the main thread's next capture, so they
contribute **nothing** to throughput at **this** operating point 
(...at 1280, inference **does affect** throughput).

The pipeline depth is held constant: the pre-fill loop submits `N_THREADS`
frames before the main loop starts, and every iteration pops one result and
submits one new job, so `in_flight ≡ N_THREADS = 3` in steady state. A freshly
captured frame is therefore third in line, and is not displayed until roughly
three main-loop periods later. Concretely:

```
end_to_end_latency  ≈  pipeline_depth × main_loop_period
                    ≈  N_THREADS × period
                    ≈  3 × 21.5 ms  ≈  ~65 ms
```

So even though the pipeline emits a frame every ~21.5 ms, any individual frame
is roughly **~65 ms old** by the time it is displayed. Throughput hides
inference; latency does not hide the queueing.

### Per-stage breakdown

Measured on a single stream, `best_yolov8n_800_relu.rknn` (800×800, HDMI
output, `taskset -c 4-7`, averaged over 10-frame windows):

| Stage | Thread | Mean (ms) | Counts toward throughput? | Counts toward latency? |
|---|---|---|---|---|
| Sensor exposure + MIPI/ISP capture (`read_mipi_frame_nv12`) | main | included in ↓ | yes | yes |
| RGA preprocess (NV12→BGR, letterbox) | main | ~20.3 (with capture) | yes | yes |
| Queue wait in `infer_q` | — | variable | no | yes |
| NPU inference (`rknn_run`) | worker 0/1/2 | _measured (see `inference (NPU)`)_ | no (hidden) | yes |
| Postprocess (NMS / decode) | worker 0/1/2 | _included in `inference (NPU)`_ | no (hidden) | yes |
| Queue wait in `result_q` | — | variable | no | yes |
| Draw boxes (`draw_box_sw`) | main | small | yes | yes |
| Display / RTSP push | main | ~1.1 | yes | yes |
| **Inference (NPU + decode), per core** | worker 0/1/2 | 20.3 | no (parallel) | yes |
| **Main-loop total (throughput)** | main | **~21.5** | — | — |
| **End-to-end (latency, est.)** | — | **~65** | — | — |

Notes: at this operating point the **main loop is bound by capture+preprocess
(~20 ms)**, so the throughput figure tracks the camera/preproc stage rather
than inference. The NPU inference + decode time is measured on the worker and
printed as `inference (NPU)`; since the three cores run in parallel it stays
off the throughput critical path but still contributes to end-to-end latency.
The "queue wait" rows are exactly the gap that makes end-to-end latency (~65 ms)
much larger than `1 / throughput` (~21.5 ms). Per-window throughput samples:
21.39 ms (46.7 FPS), 21.48 ms (46.6 FPS), 21.53 ms (46.5 FPS).

> **⚠️ Caveat — camera AEC affects capture latency.** The OS08A10 is a 
> professional sensor with **Automatic Exposure Control (AEC)**. In dark or
> low-light scenes the sensor lengthens its exposure (integration) time to
> gather more light, which **increases capture latency and lowers the effective
> frame rate** — independently of the NPU/pipeline. The numbers above were
> measured in well-lit conditions; expect capture+preprocess time to grow (and
> FPS to drop toward the longer exposure ceiling) as the scene gets darker.
> When benchmarking, keep lighting controlled and consistent, and report the
> ambient/exposure conditions alongside FPS.

### Measuring end-to-end latency

The pipeline ships with a built-in glass-to-glass latency probe. It reuses the
plumbing that already carries a per-frame value from capture to display:
`frame_id` is stamped at capture time, copied `InferJob → InferResult`, and read
back in the main loop. The probe adds a capture timestamp that rides the same
path:

1. A `capture_us` field on both job/result structs:
   ```cpp
   struct InferJob    { /* … */ uint64_t frame_id; uint64_t capture_us; };
   struct InferResult { /* … */ uint64_t frame_id; uint64_t capture_us; };
   ```
2. Stamped where the frame is captured, in `capture_and_submit()`:
   ```cpp
   struct timeval cap_tv; gettimeofday(&cap_tv, NULL);
   infer_q.push({idx, sw, sh, frame_counter++, (uint64_t)__get_us(cap_tv)});
   ```
3. Passed through unchanged in `inference_thread()` (exactly like `frame_id`):
   ```cpp
   res.capture_us = job.capture_us;
   ```
4. Differenced in the main loop, right after the display push, and folded into
   the existing 10-frame rolling average alongside the FPS print:
   ```cpp
   time_e2e += (__get_us(t1) - res.capture_us) / 1000;   // t1 = post-display
   ...
   printf("  end-to-end lat. : %6.2f ms  (capture→display)\n", time_e2e / 10);
   ```

This measures from the **userspace capture call** (`read_mipi_frame_nv12`) to
the display push, which covers the entire software pipeline. It does **not**
include the sensor's internal exposure/readout time before the frame reaches
`read_mipi_frame_nv12`, nor the display/RTSP sink's downstream buffering — for a
true sensor-photon-to-screen number you would need an external method (e.g.
filming a high-speed timer/LED alongside the rendered output and counting
frames). For pipeline tuning, the in-process probe is the relevant and
sufficient measurement.

---
