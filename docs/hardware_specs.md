# Hardware Specs

## Board

| Field | Value |
|-------|-------|
| SBC   | Khadas Edge2 |
| Chip  | RK3588S |
| Hardware revision | `EDGE2.V13` |
| Storage media | eMMC |
| Fan default | `low` |
| LCD panel | none (headless) |

## SSH Access

| Field    | Value         |
|----------|---------------|
| User     | `khadas`      |
| Password | `khadas`      |
| IP       | `192.168.1.58` |

## Camera Modules

| Device      | Sensor  |
|-------------|---------|
| `/dev/video33` | OS08A10 |
| `/dev/video51` | OS08A10 |

## CAM2 4K@60fps Overlay (V13 hardware)

CAM2 defaults to 4K@30fps for compatibility with older hardware revisions. On V13 (or later), enable 4K@60fps by applying the `cam2-4lane` overlay.

1. Confirm hardware revision:
   ```bash
   cat /proc/cmdline
   # Expected: hwver=EDGE2.V13
   ```

2. Edit the overlay env file:
   ```bash
   sudo nano /boot/dtb/rockchip/rk3588s-khadas-edge2.dtb.overlay.env
   ```
   Set (or add) the line:
   ```
   fdt_overlays=cam2-4lane
   ```

3. Reboot:
   ```bash
   sudo reboot
   ```

## Camera Pipeline (CIF path)

This describes the **raw CIF capture path** exposed by the first `rkcif-mipi-lvds` media controller. This path outputs unprocessed Bayer data and **bypasses the ISP**. The application uses the ISP-processed outputs (`/dev/video33`, `/dev/video51`) instead.

**Signal chain:**

```
OS08A10 (I2C 4-0036)  →  CSI-2 DPHY0  →  MIPI CSI-2 receiver  →  CIF capture nodes
/dev/v4l-subdev2           /dev/v4l-subdev1    /dev/v4l-subdev0
```

**Sensor native format:** `SBGGR10_1X10` (Bayer BGGR 10-bit) · `3840×2160` · `30 fps`

**CIF capture nodes (raw Bayer, no ISP):**

| Node | Type |
|------|------|
| `/dev/video0`–`/dev/video3` | Main stream channels (`stream_cif_mipi_id0`–`id3`) |
| `/dev/video4`–`/dev/video7` | Scaled channels (`rkcif_scale_ch0`–`ch3`) |
| `/dev/video8`–`/dev/video10` | Tools channels (`rkcif_tools_id0`–`id2`) |

## RGA (Raster Graphics Accelerator)

Two RGA3 cores and one RGA2 core:

```
rga3 fdb60000.rga: probe successfully, irq = 41,  hw_version: 3.0.76831
rga3 fdb70000.rga: probe successfully, irq = 42,  hw_version: 3.0.76831
rga2 fdb80000.rga: probe successfully, irq = 123, hw_version: 3.2.63318
rga: Module initialized. v1.3.9
```

## ISP (Image Signal Processor)

The ISP driver is built into the kernel (not a loadable module). Two hardware ISP blocks expose three virtual instances:

| Instance | Hardware | Status |
|----------|----------|--------|
| `rkisp0-vir0` | `fdcb0000.rkisp` | Active (camera `/dev/video33`) |
| `rkisp0-vir1` | `fdcb0000.rkisp` | Active (camera `/dev/video51`) |
| `rkisp1-vir0` | `fdcc0000.rkisp` | Active |

Driver version: `v03.00.00`

## Video Codec Hardware (MPP)

Managed by the Rockchip **MPP** (Media Process Platform) service (`mpp-srv`), driver version `994be34` (2025-06-03).

### Video Decoder — rkvdec2 (H.264 / H.265 / VP9)

CCU at `fdc30000.rkvdec-ccu`, ccu-mode 1, **2 cores**:

| Core | Address | SRAM start | SRAM size | RCB size |
|------|---------|-----------|-----------|---------|
| Core 0 | `fdc38100.rkvdec-core` | `0xff001000` | 480 KB | 1 MB |
| Core 1 | `fdc48100.rkvdec-core` | `0xff079000` | 476 KB | 1 MB |

### Video Encoder — rkvenc2 (H.264 / H.265)

CCU at `rkvenc-ccu`, **2 cores**:

| Core | Address | IOMMU group |
|------|---------|-------------|
| Core 0 | `fdbd0000.rkvenc-core` | 10 |
| Core 1 | `fdbe0000.rkvenc-core` | 11 |

### JPEG Encoder — mpp_vepu2 (jpege)

CCU at `jpege-ccu`, **4 cores**:

| Core | Address | IOMMU group |
|------|---------|-------------|
| Core 0 | `fdba0000.jpege-core` | 5 |
| Core 1 | `fdba4000.jpege-core` | 6 |
| Core 2 | `fdba8000.jpege-core` | 7 |
| Core 3 | `fdbac000.jpege-core` | 8 |

### Other Codec Units

| Unit | Driver | Address | Description |
|------|--------|---------|-------------|
| JPEG decoder | `mpp_jpgdec` | `fdb90000.jpegd` | Hardware JPEG decode |
| AV1 decoder | `mpp_av1dec` | `fdc70000.av1d` | Hardware AV1 decode |
| VPU decoder (legacy) | `mpp_vdpu2` | `fdb50400.vdpu` | Legacy VPU decode |
| AVSD decoder (legacy) | `mpp_vdpu1` | `fdb51000.avsd-plus` | AVSD/VC-1 decode |
| VPU encoder (legacy) | `mpp_vepu2` | `fdb50000.vepu` | Legacy VPU encode |
| IEP2 | `mpp-iep2` | `fdbb0000.iep` | Image Enhancement Processor |

## NPU (Neural Processing Unit)

Single RKNPU block with **three cores** (`fdab0000`–`fdad0000`), IOMMU enabled:

| Field | Value |
|-------|-------|
| Hardware | `fdab0000.npu` |
| Driver | `rknpu v0.9.8` (2024-08-28) |
| IOMMU | Enabled |
| Cores | 3 × (`fdab0000`, `fdac0000`, `fdad0000`) |
