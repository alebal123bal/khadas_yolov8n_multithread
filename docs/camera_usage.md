
## Dual Stream Preview (HDMI)

Two independent windows at 1920×1080, each at 46 fps. Run in two separate terminals.

Key design choices:
- `io-mode=dmabuf min-buffers=64` — zero-copy DMA capture, large ring buffer to absorb jitter
- NV12 kept throughout — no color conversion needed
- Scale 3840×2160 → 1920×1080 (exact 2× ratio, most efficient for `videoscale`)
- `queue max-size-buffers=2 leaky=downstream` — drops stale frames, keeps latency at one frame
- `sync=false async=false` on `waylandsink` — disables clock sync for minimum display latency

First deactivate the conda environment:

```bash
conda deactivate
```

**Terminal 1 — camera `/dev/video51`:**

```bash
gst-launch-1.0 \
  v4l2src device=/dev/video51 io-mode=dmabuf min-buffers=64 ! \
  video/x-raw,format=NV12,width=3840,height=2160,framerate=46/1 ! \
  queue max-size-buffers=2 leaky=downstream ! \
  videoscale ! video/x-raw,format=NV12,width=1920,height=1080 ! \
  waylandsink sync=false async=false
```

**Terminal 2 — camera `/dev/video33`:**

```bash
gst-launch-1.0 \
  v4l2src device=/dev/video33 io-mode=dmabuf min-buffers=64 ! \
  video/x-raw,format=NV12,width=3840,height=2160,framerate=46/1 ! \
  queue max-size-buffers=2 leaky=downstream ! \
  videoscale ! video/x-raw,format=NV12,width=1920,height=1080 ! \
  waylandsink sync=false async=false
```
