#pragma once

// wire_protocol.h — Binary/JSON wire-format types for both IPC planes.
//
// This header has NO dependency on RKNN, RGA, or any board-specific SDK.
// It can be included by both the producer (yolov8n pipeline) and any consumer
// process or tool without carrying heavy SDK headers.
//
// ── Control plane ─────────────────────────────────────────────────────────
//   Transport : Unix domain stream socket at kControlSocketPath
//   Framing   : [uint32_t LE body_len][UTF-8 JSON body]
//
//   Requests  (client → server):
//     {"cmd":"pause_yolo"}
//     {"cmd":"resume_yolo"}
//     {"cmd":"get_status"}
//     {"cmd":"set_dump_enabled","params":{"enabled":true}}
//     {"cmd":"shutdown"}
//
//   Responses (server → client):
//     {"ok":true}
//     {"ok":true,"payload":{"inference_enabled":true,"dump_enabled":false,
//                           "frame_id":1234,"queue_depth":2,"fps":28.5}}
//     {"ok":false,"error":"unknown command"}
//
// ── Data plane ────────────────────────────────────────────────────────────
//   Transport : Unix domain stream socket at kDataSocketPath
//   Framing   : [uint32_t LE body_len][WireFrameHeader][WireDetectionRecord × count]
//
//   body_len = sizeof(WireFrameHeader) + count × sizeof(WireDetectionRecord)
//            = 24 + count × 36  bytes
//
//   At 30 fps, 0 detections  : 28 bytes/frame  ≈   840 B/s
//   At 30 fps, 10 detections : 388 bytes/frame ≈ 11.6 KB/s
//
// Endianness: all multi-byte fields are native-endian (LE on RK3588S / x86).
//             Add byte-swap if cross-endian transport is ever needed.

#include <cstdint>

// ─── Socket paths ──────────────────────────────────────────────────────────
// Paths are derived at runtime from the camera device number so that multiple
// pipeline instances (one per camera) each get their own unique socket pair.
//
//   device_number "33" → /tmp/yolo_control_33.sock  /tmp/yolo_data_33.sock
//   device_number "51" → /tmp/yolo_control_51.sock  /tmp/yolo_data_51.sock
//
// Use ipc_control_path() / ipc_data_path() everywhere instead of hardcoded
// string literals.

#include <string>

inline std::string ipc_control_path(const std::string& device_number) {
    return "/tmp/yolo_control_" + device_number + ".sock";
}
inline std::string ipc_data_path(const std::string& device_number) {
    return "/tmp/yolo_data_" + device_number + ".sock";
}
inline std::string ipc_tracks_path(const std::string& device_number) {
    return "/tmp/yolo_tracks_" + device_number + ".sock";
}
inline std::string ipc_events_path(const std::string& device_number) {
    return "/tmp/yolo_events_" + device_number + ".sock";
}

// Maximum number of detection objects per frame (matches OBJ_NUMB_MAX_SIZE).
static const int kMaxDetections = 64;

// ─── Data-plane packed structs ─────────────────────────────────────────────
#pragma pack(push, 1)

struct WireFrameHeader {
    uint64_t frame_id;       ///< Monotonic counter; 0 = first frame.
    uint64_t timestamp_us;   ///< gettimeofday epoch-microseconds.
    uint16_t count;          ///< Number of WireDetectionRecord entries following.
    uint8_t  reserved[6];    ///< Padding to 8-byte alignment; reserved for flags.
};
static_assert(sizeof(WireFrameHeader) == 24, "WireFrameHeader size changed");

struct WireDetectionRecord {
    char    class_name[16];  ///< Null-terminated class label (OBJ_NAME_MAX_SIZE).
    float   confidence;      ///< Detection score in [0.0, 1.0].
    int32_t left;            ///< Bounding box in original-image pixel coordinates.
    int32_t top;
    int32_t right;
    int32_t bottom;
};
static_assert(sizeof(WireDetectionRecord) == 36, "WireDetectionRecord size changed");

// Tracked-detection record published by the ByteTrack service on the
// tracks socket. Same framing as the raw data plane:
//   [uint32_t LE body_len][WireFrameHeader][WireTrackRecord × count]
// The `track_id` is a monotonically increasing per-process ID; 0 is reserved
// for "no track" (should not normally occur in tracker output).
struct WireTrackRecord {
    char    class_name[16];
    float   confidence;
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    uint32_t track_id;
};
static_assert(sizeof(WireTrackRecord) == 40, "WireTrackRecord size changed");

// ── Events plane (temporal_service → consumer) ────────────────────────────
// Per-track temporal summary, published once per input frame for every active
// track. Same framing as the other planes:
//   [uint32_t LE body_len][WireFrameHeader][WireTrackSummary × count]
//
// Status values:
//   0 = tentative (just appeared; not yet enough history for velocity)
//   1 = active    (full features available)
//   2 = lost      (not seen this frame, summary reflects last known state)
struct WireTrackSummary {
    char     class_name[16];   ///< Null-terminated class label.
    uint32_t track_id;         ///< From ByteTrack.
    uint32_t frames_seen;      ///< Number of frames this track was observed.

    uint64_t first_seen_us;    ///< Epoch-µs of first observation.
    uint64_t last_seen_us;     ///< Epoch-µs of most recent observation.
    uint32_t duration_ms;      ///< (last_seen - first_seen) / 1000.
    float    visibility;       ///< frames_seen / frames_since_first ∈ [0, 1].

    int32_t  cx, cy;           ///< Last bbox centroid (pixels).
    int32_t  left, top, right, bottom;   ///< Last bbox.

    float    vx, vy;           ///< Smoothed velocity (px/s).
    float    speed;            ///< sqrt(vx² + vy²) (px/s).
    float    heading_deg;      ///< atan2(vy, vx) in degrees, [-180, 180].
    float    accel;            ///< Magnitude of velocity delta (px/s²).
    float    loiter_score;     ///< 0 (moving) → 1 (stationary in small area).

    uint8_t  in_roi;           ///< 1 if centroid lies within the configured ROI.
    uint8_t  roi_entered;      ///< 1 on the frame the track crosses into ROI.
    uint8_t  roi_exited;       ///< 1 on the frame the track crosses out of ROI.
    uint8_t  status;           ///< 0 tentative, 1 active, 2 lost.
    uint8_t  reserved[4];      ///< Padding / future flags.
};
static_assert(sizeof(WireTrackSummary) == 104, "WireTrackSummary size changed");

#pragma pack(pop)
