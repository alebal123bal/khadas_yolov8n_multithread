#pragma once

// bytetrack_adapter.h — thin tracker abstraction.
//
// The ByteTrack service depends only on this interface, not on any concrete
// tracking library. Swap the implementation (e.g. drop in a real ByteTrack
// port) by linking a different .cc that defines `make_tracker()`.

#include <cstdint>
#include <memory>
#include <vector>

struct TrackerDetection {
    char    class_name[16]{};
    float   confidence{0.f};
    int32_t left{0}, top{0}, right{0}, bottom{0};
};

struct TrackerOutput {
    TrackerDetection det;
    uint32_t         track_id{0};
};

class IByteTracker {
public:
    virtual ~IByteTracker() = default;

    /// Feed one frame's detections; return the set of currently active tracks.
    /// `frame_id` is informational (used for age bookkeeping inside the impl).
    virtual std::vector<TrackerOutput> update(
        uint64_t frame_id,
        const std::vector<TrackerDetection>& dets) = 0;
};

/// Factory — defined by the concrete implementation .cc file.
std::unique_ptr<IByteTracker> make_tracker();
