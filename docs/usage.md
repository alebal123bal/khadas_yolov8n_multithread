# YOLOv8n pipeline — usage guide

All commands run on the Khadas board. The `lib/` directory contains
`librknnrt.so` and `librga.so`; set `LD_LIBRARY_PATH` before launching.

> Pin `yolov8n_cap_multithread` to the **big A76 cores (`4-7`)** for best
> throughput. For the full CPU-pinning rationale, the IPC control plane, the
> downstream tracking/temporal/LLM stages, and full-stack orchestration, see
> [usage_advanced.md](usage_advanced.md).

---

## HDMI mode (recommended — lowest latency, no encode overhead)

Two cameras use separate terminals; each opens its own window on the monitor.

```sh
# Terminal 1 — camera /dev/video33
cd ~/programs/yolov8n_cap_multithread
export LD_LIBRARY_PATH=./lib
taskset -c 4-7 ./yolov8n_cap_multithread ./data/model/best_yolov8n_800_relu.rknn 33 hdmi

# Terminal 2 — camera /dev/video51
cd ~/programs/yolov8n_cap_multithread
export LD_LIBRARY_PATH=./lib
taskset -c 4-7 ./yolov8n_cap_multithread ./data/model/best_yolov8n_800_relu.rknn 51 hdmi
```

Each instance opens a window titled `cam33` / `cam51`.
If launching over SSH, export the Wayland socket first:

```sh
export WAYLAND_DISPLAY=wayland-0
```

---

## RTSP mode (network streaming — capped at 30 fps)

Use RTSP when you need to view the stream from another machine.
Note: the encode/decode path limits you to ~30 fps regardless of NPU throughput.

```sh
# Terminal 1 — camera /dev/video33, stream on port 8554
cd ~/programs/yolov8n_cap_multithread
export LD_LIBRARY_PATH=./lib
taskset -c 4-7 ./yolov8n_cap_multithread ./data/model/best_yolov8n_800_relu.rknn 33 8554

# Terminal 2 — camera /dev/video51, stream on port 8555
cd ~/programs/yolov8n_cap_multithread
export LD_LIBRARY_PATH=./lib
taskset -c 4-7 ./yolov8n_cap_multithread ./data/model/best_yolov8n_800_relu.rknn 51 8555
```

View both streams side-by-side from a workstation (see `fast_stream.sh`):

```
rtsp://<board-ip>:8554/stream
rtsp://<board-ip>:8555/stream
```

---

## Next steps

- **Control a running pipeline** (status, pause, blackout for the LLM) →
  [usage_advanced.md › IPC control plane](usage_advanced.md#ipc--control-plane-and-data-plane)
- **Tracking, temporal features, and the event summarizer / LLM** →
  [usage_advanced.md › Tracking + temporal features](usage_advanced.md#tracking--temporal-features-separate-processes)
- **Run the whole stack at once** →
  [usage_advanced.md › Full stack](usage_advanced.md#full-stack--one-camera)