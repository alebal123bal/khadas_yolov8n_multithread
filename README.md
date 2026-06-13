# khadas_yolov8n_multithread - Real-time UAV detection on the RK3588S NPU

<!-- TODO: replace with the demo GIF -->
<p align="center">
  <img src="res/gifs/Dual_Track.gif" alt="Real-time YOLOv8n UAV detection running on the RK3588S NPU" width="720">
</p>

**Real-time YOLOv8n UAV detection at the sensor's 46 FPS ceiling, in ~140 MB of
RAM.** A high-throughput, low-footprint computer-vision pipeline
for the **Rockchip RK3588S** SoC: it captures live 1080p MIPI frames, runs
YOLOv8n across all **3 NPU cores** in parallel (lifting throughput from ~31 to
**46 FPS** — the camera, not the pipeline, is now the limit), and streams the
annotated result to HDMI or RTSP. Capture, color-convert/resize and inference
run entirely on fixed-function silicon (ISP, RGA, NPU), so the CPU stays free
and memory holds **flat at ~140 MB per stream** — small enough to run on even
the cheapest 2 GB RK3588S boards, not just high-end dev kits. Targets any
RK3588S board; built and tested on the **Khadas Edge2**.

Then it goes a step further: when a tracked UAV leaves the scene, an on-device LLM
(Qwen2.5-0.5B, on the *same* NPU) writes a natural-language assessment of what
just happened. The whole thing is a chain of small, independent processes
connected by Unix-domain sockets — detections flow downstream into multi-object
tracking, temporal-feature extraction, a presence FSM, and the on-demand LLM
summary.

**Highlights**

- **Saturates the sensor:** 3-thread NPU inference lifts throughput from ~31 FPS
  to the 46 FPS camera ceiling — the pipeline is no longer the bottleneck.
- **Fully hardware-accelerated:** capture (ISP), color-convert/resize (RGA), and
  inference (NPU) never touch the CPU, giving a flat ~140 MB RSS per stream.
- **Runs on any RK3588S board:** because the footprint is so small (~140 MB for
  one stream, ~290 MB for two), it fits comfortably on the cheapest RK3588S
  boards on the market — even 2 GB models that sell for **as little as ~€90** —
  not just high-end dev kits.
- **Two cameras at once:** independent per-device sockets let two streams run and
  be controlled side by side.
- **Composable pipeline:** detection → ByteTrack → temporal features → presence
  FSM → on-demand LLM summary, each a separate process.
- **NPU hand-off for the LLM:** a `blackout`/`resume` control plane frees the
  whole NPU so the LLM runs at full speed, then hands it back to the cameras.

**Target hardware:** any RK3588S-based board, aarch64 Linux, with an OS08A10
MIPI camera. Developed and tested on the **Khadas Edge2**. Cross-compiles from
x86-64/WSL or builds natively on the board.

For the full software architecture (Mermaid diagrams of the internal pipeline
and the multi-process topology) see [docs/architecture.md](docs/architecture.md);
for launch commands see [docs/usage.md](docs/usage.md).

**Related repositories**

- **[RKNN_TRAIN_YOLO](https://github.com/alebal123bal/RKNN_TRAIN_YOLO)** — the
  entire pipeline for training, converting, and exporting the YOLO model into
  the Rockchip NPU `.rknn` format used here.
- **[RKLLM_LLAMA_QWEN](https://github.com/alebal123bal/RKLLM_LLAMA_QWEN)** — the
  entire pipeline for running optimized LLM models on the RK3588S, either on the
  NPU (RKLLM) or the CPU (llama).

---

## Performance

A 3-thread inference pool runs one RKNN context per NPU core
(`rknn_dup_context` + `rknn_set_core_mask`), pipelining capture, inference, and
display across all three cores. At 1080p with YOLOv8n 640×640 this lifts
throughput from **~31.2 FPS** (naïve single-threaded loop) to the **46 FPS**
OS08A10 camera ceiling — the pipeline is no longer the bottleneck, the sensor
is. Full per-model FPS, latency, and CPU/NPU/RAM numbers are in
[docs/benchmarks.md](docs/benchmarks.md).

---

## Fully hardware-accelerated → tiny RAM footprint

Every heavy per-frame operation runs on a dedicated fixed-function block of the
RK3588S (camera ISP, RGA, NPU), never on the CPU — so there are no large
intermediate framebuffers or scratch tensors CPU-side. A fixed pool of
pre-allocated buffers (`N_BUF`, see `BufPool` in [`src/main.cc`](yolov8n_cap_multithread/src/main.cc))
is recycled instead of allocating per frame, so memory stays **flat and
bounded**: **~137–152 MB RSS for one 1080p stream**, **~276–304 MB for two**
(and that double-counts the shared `librknnrt.so` / `librga.so` pages).

Because the NPU, ISP and RGA are identical across the whole RK3588S range, the
same binary runs at full speed on the cheapest 2 GB boards (**~€90**) — no
8/16 GB dev kit required. See [docs/architecture.md](docs/architecture.md)
for the per-frame offload table and pipeline diagram.

---

## Build & deploy

**Native (on the board):**

```sh
cd yolov8n_cap_multithread
bash build.sh
```

**Cross-compile (WSL / x86-64 Linux):**

```sh
# one-time setup
sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
bash setup_sdk.sh          # fetches librga v1.10.5_[8] + librknnrt v2.3.2

cd yolov8n_cap_multithread
bash build.sh              # uses toolchain-aarch64.cmake (aarch64-linux-gnu-g++)
scp -r install/yolov8n_cap_multithread/ khadas@<board-ip>:~/programs/
```

Run: `./yolov8n_cap_multithread <rknn model> <device number> <rtsp port | hdmi>`

See [docs/usage.md](docs/usage.md) for launch commands, and
[docs/usage_advanced.md](docs/usage_advanced.md) for the IPC control/data
plane, the downstream tracking/temporal/LLM stages, and RTSP streaming setup.

---

## Repository structure

```
yolov8n_cap_multithread/
├── CMakeLists.txt              # builds main pipeline + all auxiliary processes
├── build.sh                    # convenience wrapper around CMake
├── toolchain-aarch64.cmake     # cross-compile toolchain (WSL / x86 → aarch64)
├── data/
│   ├── coco_1_labels_list.txt
│   └── model/                  # .rknn model files
│
├── include/                    # YOLO pipeline headers
│   ├── camera_util.h
│   ├── drm_func.h
│   ├── local_display.h         # HDMI output via DRM / Wayland
│   ├── model_utils.h
│   ├── postprocess.h           # YOLOv8 decode + NMS
│   ├── rga_func.h              # Rockchip RGA color-space conversion / resize
│   ├── rtsp_stream.h           # GStreamer RTSP publisher
│   └── ipc/                    # shared IPC layer (control + data planes)
│       ├── bounded_queue.h     # drop-oldest queue used by all publishers
│       ├── i_control_server.h
│       ├── i_data_publisher.h
│       ├── messages.h          # in-process DetectionMessage type
│       ├── unix_control_server.h
│       ├── unix_data_publisher.h
│       ├── wire_protocol.h     # ALL on-the-wire structs + socket paths
│       └── yolo_control_state.h
│
├── src/                        # YOLO pipeline implementation
│   ├── main.cc                 # multi-threaded RKNN pipeline (3 NPU cores)
│   ├── camera_util.cc
│   ├── local_display.cc
│   ├── model_utils.cc
│   ├── postprocess.cc
│   ├── rga_func.cc
│   ├── rtsp_stream.cc
│   └── ipc/
│       ├── unix_control_server.cc      # JSON control plane over AF_UNIX
│       └── unix_data_publisher.cc      # binary detection stream over AF_UNIX
│
├── tracker/                    # ByteTrack stage (separate process)
│   ├── include/
│   │   └── bytetrack_adapter.h         # IByteTracker interface
│   └── src/
│       ├── bytetrack_service.cc        # main() — reads data, writes tracks
│       └── iou_tracker.cc              # default IOU-greedy implementation
│
├── temporal/                   # Temporal-features stage (separate process)
│   ├── include/
│   │   ├── track_state.h               # per-track history + feature math
│   │   └── track_manager.h             # lifecycle + per-frame orchestration
│   └── src/
│       ├── temporal_service.cc         # main() — reads tracks, writes events
│       ├── track_state.cc
│       └── track_manager.cc
│
├── tools/                      # Standalone client / debug binaries
│   ├── control_client.cc       # send pause/resume/blackout/status commands
│   ├── data_receiver.cc        # consume raw detections   (yolo_data socket)
│   ├── tracks_receiver.cc      # consume tracked dets     (yolo_tracks socket)
│   ├── events_receiver.cc      # consume temporal events  (yolo_events socket)
│   └── event_summarizer.cc     # presence FSM + on-demand LLM (production sink)
│
├── utility_board_scripts/      # board-side helpers (deployed to install tree)
│   └── run_qwen.sh             # feeds a snapshot to Qwen2.5-0.5B via llm_demo
│
├── build/                      # CMake out-of-source build tree
└── install/                    # `make install` deploy tree (scp this to board)
    └── yolov8n_cap_multithread/
        ├── yolov8n_cap_multithread
        ├── bytetrack_service
        ├── temporal_service
        ├── control_client
        ├── data_receiver
        ├── tracks_receiver
        ├── events_receiver
        ├── event_summarizer
        ├── data/                       # models + labels
        ├── utility_board_scripts/      # run_qwen.sh
        └── lib/                        # librknnrt.so, librga.so
```

### Process topology

Each stage is an independent OS process; they communicate via per-device
Unix-domain sockets (`<device>` = V4L2 device number, e.g. `33`). The full
software architecture — the internal `main.cc` pipeline and the multi-process
topology, both as Mermaid diagrams — is documented in
[docs/architecture.md](docs/architecture.md).

---

## License & disclaimer

Licensed under the **Apache License 2.0** — see [LICENSE](LICENSE).

This is an **independent, personal project** built for **educational and
research purposes only**. It is **not affiliated with or endorsed by any
employer or client** of the author, and is **not intended for production,
operational, safety-critical, surveillance, or defense use**. The "UAV" class
is only a sample detection target for benchmarking the inference pipeline. The
software is provided **"AS IS", without warranty of any kind**, and **you are
solely responsible** for complying with all applicable export-control and other
regulations. See [DISCLAIMER.md](DISCLAIMER.md) for the full text.
