#pragma once

// track_manager.h — owns the set of live TrackStates and drives one frame.
//
// Usage:
//   TrackManager mgr(roi);
//   for each frame:
//       mgr.begin_frame();
//       for each WireTrackRecord:
//           mgr.observe(record, timestamp_us);
//       const auto& tracks = mgr.end_frame();   // active + lost
//       publish(tracks);

#include "track_state.h"
#include "ipc/wire_protocol.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class TrackManager {
public:
    explicit TrackManager(Roi roi = {});

    /// Reset per-frame state (entered/exited flags, seen markers).
    void begin_frame();

    /// Consume one tracked detection. `track_id == 0` is ignored.
    void observe(const WireTrackRecord& rec, uint64_t timestamp_us);

    /// Finalize the frame: ages out stale tracks, marks missed ones as lost,
    /// returns a flat view of all tracks worth publishing this frame
    /// (active + recently-lost).
    const std::vector<const TrackState*>& end_frame();

    /// Number of currently-tracked IDs.
    std::size_t size() const { return tracks_.size(); }

private:
    static constexpr uint32_t kMaxFramesLost = 30;  // drop after this many

    Roi                                            roi_;
    std::unordered_map<uint32_t, TrackState>       tracks_;
    std::unordered_map<uint32_t, bool>             seen_this_frame_;
    std::vector<const TrackState*>                 view_;
};
