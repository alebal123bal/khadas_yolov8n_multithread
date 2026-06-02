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

**Performance breakthrough:** moving from the naïve single-threaded
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

| Model | Input size | Stream | FPS | NPU cores 0/1/2 | CPU (4x A76) | RAM (RSS) |
|---|---|---|---|---|---|---|
| `best_yolov8n_768.rknn` | 768 | single | 46.5 | 35% | 29.2% | 138 MB |
| `best_yolov8n_768.rknn` | 768 | dual | 41.0 | 75% | 59.3% | 276 MB |
| `best_yolov8n_768_relu.rknn` | 768 | single | 46.5 | 25% | 28.7% | 137 MB |
| `best_yolov8n_768_relu.rknn` | 768 | dual | 46.5 | 65% | 69.1% | 281 MB |
| `best_yolov8n_800_relu.rknn` | 800 | single | 46.5 | 29% | 27.5% | 142 MB |
| `best_yolov8n_800_relu.rknn` | 800 | dual | 41.5 | 69% | 63.1% | 283 MB |
| `best_yolov8n_896_relu.rknn` | 896 | single | 46.5 | 35% | 28.4% | 152 MB |
| `best_yolov8n_896_relu.rknn` | 896 | dual | 38 | 73% | 60.9% | 304 MB |

---

## Latency breakdown

Because inference runs in parallel across the three NPU cores (one context per
core), NPU inference and postprocessing are pulled **off the main loop's
critical path** — they overlap with capture/preprocess of subsequent frames.
The end-to-end frame time the main loop observes is therefore dominated by
capture+preprocess and display, not by the sum of every stage.

Measured on a single stream, `best_yolov8n_800_relu.rknn` (800×800, HDMI
output, `taskset -c 4-7`, averaged over 10-frame windows):

| Stage | Mean (ms) | Notes |
|---|---|---|
| Capture + RGA preprocess | ~20.3 | main loop, on critical path |
| NPU inference | _parallel_ | overlapped across cores 0/1/2, off critical path |
| Postprocess (NMS / decode) | _parallel_ | overlapped, off critical path |
| Display | ~1.1 | main loop, on critical path |
| **Total (main loop)** | ~21.5 | **≈46.5 FPS** |

Notes: at this operating point the main loop is bound by capture+preprocess
(~20 ms), so total frame time tracks the camera/preproc stage rather than
inference. Per-window samples: 21.39 ms (46.7 FPS), 21.48 ms (46.6 FPS),
21.53 ms (46.5 FPS).

> **⚠️ Caveat — camera AEC affects capture latency.** The OS08A10 is a 
> professional sensor with **Automatic Exposure Control (AEC)**. In dark or
> low-light scenes the sensor lengthens its exposure (integration) time to
> gather more light, which **increases capture latency and lowers the effective
> frame rate** — independently of the NPU/pipeline. The numbers above were
> measured in well-lit conditions; expect capture+preprocess time to grow (and
> FPS to drop toward the longer exposure ceiling) as the scene gets darker.
> When benchmarking, keep lighting controlled and consistent, and report the
> ambient/exposure conditions alongside FPS.

---
