# Software Specs

## Operating System

- **OS**: Ubuntu 24.04.3 LTS
- **Kernel**: Linux 6.1.118
- **Fenix**: 1.7.5

## Firmware

| Component | Version |
|-----------|--------|
| BL31 (ARM TF) | `v1.53` |
| BL32 (OP-TEE) | `v1.20` |
| U-Boot | `09/25/2025` |

## Runtime Libraries

Deployed on the board at `~/programs/yolov8n_cap_multithread/lib/`.

| Library | Version | Build date |
|---------|---------|------------|
| `librga.so` | `1.10.5_[8]` | — |
| `librknnrt.so` | `2.3.2` (`429f97ae6b`) | 2025-04-09 |

### librga

- API version: `1.10.5_[8]`
- Source: [github.com/airockchip/librga](https://github.com/airockchip/librga) — `main` branch (no dedicated tag for this version)
- Headers: `include/`
- Precompiled lib (aarch64): `libs/Linux/gcc-aarch64/`

### librknnrt (RKNN Runtime)

- Version: `2.3.2 (429f97ae6b@2025-04-09T09:09:27)`
- Source: [github.com/airockchip/rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) — tag `v2.3.2`
- Runtime path in repo: `rknpu2/runtime/Linux/librknn_api/`
- Supports RKNN model version ≤ runtime version
- Requires matching NPU driver; current driver: `rknpu v0.9.8` (see hardware_specs.md)
