// track_manager.cc — lifecycle and per-frame orchestration.

#include "track_manager.h"

TrackManager::TrackManager(Roi roi) : roi_(roi) {}

void TrackManager::begin_frame() {
    seen_this_frame_.clear();
    for (const auto& kv : tracks_) seen_this_frame_[kv.first] = false;
}

void TrackManager::observe(const WireTrackRecord& rec, uint64_t timestamp_us) {
    if (rec.track_id == 0) return;

    Observation obs{
        timestamp_us,
        (rec.left + rec.right)  / 2,
        (rec.top  + rec.bottom) / 2,
        rec.left, rec.top, rec.right, rec.bottom,
    };

    auto it = tracks_.find(rec.track_id);
    if (it == tracks_.end()) {
        it = tracks_.emplace(rec.track_id,
                             TrackState(rec.track_id, rec.class_name, timestamp_us)).first;
    }
    it->second.update(obs, roi_);
    seen_this_frame_[rec.track_id] = true;
}

const std::vector<const TrackState*>& TrackManager::end_frame() {
    // 1. Mark unseen tracks as missed; drop those past kMaxFramesLost.
    for (auto it = tracks_.begin(); it != tracks_.end(); ) {
        const bool seen = seen_this_frame_.count(it->first) && seen_this_frame_[it->first];
        if (!seen) {
            it->second.mark_missed();
            if (it->second.frames_since_last_seen() > kMaxFramesLost) {
                it = tracks_.erase(it);
                continue;
            }
        }
        ++it;
    }

    // 2. Build flat view (active + lost).
    view_.clear();
    view_.reserve(tracks_.size());
    for (const auto& kv : tracks_) view_.push_back(&kv.second);
    return view_;
}
