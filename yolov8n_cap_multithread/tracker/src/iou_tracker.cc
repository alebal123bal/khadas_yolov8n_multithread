// iou_tracker.cc — Minimal IOU-greedy tracker.
//
// This is a placeholder implementation of `IByteTracker` so the service runs
// end-to-end out of the box. To switch to a real ByteTrack port (e.g.
// https://github.com/Vertical-Beach/ByteTrack-cpp), replace this file with
// a wrapper that defines `make_tracker()` and link Eigen if required.
//
// Algorithm:
//   - Each active track stores its last bbox, class, and frames_unseen counter.
//   - Per frame: compute IOU between every active track and every new
//     detection; greedily match the highest IOU pairs above kIouThreshold,
//     preferring same-class matches.
//   - Unmatched detections start new tracks.
//   - Unmatched tracks increment frames_unseen and are dropped after kMaxAge.

#include "bytetrack_adapter.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr float kIouThreshold = 0.3f;
constexpr int   kMaxAge       = 30;   // frames a track can be missed before deletion

struct Track {
    uint32_t         id;
    TrackerDetection det;
    int              frames_unseen;
    uint64_t         last_seen_frame;
};

static float iou(const TrackerDetection& a, const TrackerDetection& b) {
    const int xL = std::max(a.left,  b.left);
    const int yT = std::max(a.top,   b.top);
    const int xR = std::min(a.right, b.right);
    const int yB = std::min(a.bottom, b.bottom);
    if (xR <= xL || yB <= yT) return 0.f;
    const float inter = static_cast<float>(xR - xL) * (yB - yT);
    const float areaA = static_cast<float>(a.right - a.left) * (a.bottom - a.top);
    const float areaB = static_cast<float>(b.right - b.left) * (b.bottom - b.top);
    const float uni   = areaA + areaB - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

class IouTracker : public IByteTracker {
public:
    std::vector<TrackerOutput> update(
        uint64_t frame_id,
        const std::vector<TrackerDetection>& dets) override
    {
        const size_t T = tracks_.size();
        const size_t D = dets.size();

        std::vector<int>  det_to_track(D, -1);
        std::vector<bool> track_used  (T, false);

        // Greedy IOU matching: repeatedly pick the highest unmatched pair.
        while (true) {
            float best_iou = kIouThreshold;
            int   best_d = -1, best_t = -1;
            for (size_t d = 0; d < D; ++d) {
                if (det_to_track[d] >= 0) continue;
                for (size_t t = 0; t < T; ++t) {
                    if (track_used[t]) continue;
                    // Optional: skip cross-class matches.
                    if (std::strncmp(dets[d].class_name, tracks_[t].det.class_name, 16) != 0)
                        continue;
                    const float v = iou(dets[d], tracks_[t].det);
                    if (v > best_iou) { best_iou = v; best_d = (int)d; best_t = (int)t; }
                }
            }
            if (best_d < 0) break;
            det_to_track[best_d] = best_t;
            track_used  [best_t] = true;
        }

        // Update matched tracks; spawn new ones for unmatched detections.
        std::vector<TrackerOutput> outputs;
        outputs.reserve(D);

        for (size_t d = 0; d < D; ++d) {
            if (det_to_track[d] >= 0) {
                Track& tr = tracks_[det_to_track[d]];
                tr.det             = dets[d];
                tr.frames_unseen   = 0;
                tr.last_seen_frame = frame_id;
                outputs.push_back({dets[d], tr.id});
            } else {
                Track tr;
                tr.id              = next_id_++;
                tr.det             = dets[d];
                tr.frames_unseen   = 0;
                tr.last_seen_frame = frame_id;
                tracks_.push_back(tr);
                outputs.push_back({dets[d], tr.id});
            }
        }

        // Age out unmatched tracks.
        for (size_t t = 0; t < T; ++t) {
            if (!track_used[t]) tracks_[t].frames_unseen++;
        }
        tracks_.erase(
            std::remove_if(tracks_.begin(), tracks_.end(),
                [](const Track& tr){ return tr.frames_unseen > kMaxAge; }),
            tracks_.end());

        return outputs;
    }

private:
    std::vector<Track> tracks_;
    uint32_t           next_id_{1};  // 0 reserved for "no track"
};

} // namespace

std::unique_ptr<IByteTracker> make_tracker() {
    return std::unique_ptr<IByteTracker>(new IouTracker());
}
