// track_state.cc — feature computation for one tracked target.

#include "track_state.h"

#include <algorithm>
#include <cmath>
#include <cstring>

TrackState::TrackState(uint32_t track_id, const char* class_name, uint64_t first_seen_us)
    : track_id_(track_id), first_seen_us_(first_seen_us)
{
    std::strncpy(class_name_, class_name, sizeof(class_name_) - 1);
}

uint32_t TrackState::duration_ms() const {
    if (last_seen_us_ <= first_seen_us_) return 0;
    return static_cast<uint32_t>((last_seen_us_ - first_seen_us_) / 1000ULL);
}

float TrackState::visibility() const {
    if (frames_since_first_ == 0) return 0.f;
    return static_cast<float>(frames_seen_) / static_cast<float>(frames_since_first_);
}

float TrackState::speed() const {
    return std::sqrt(vx_ * vx_ + vy_ * vy_);
}

float TrackState::heading_deg() const {
    if (std::abs(vx_) < 1e-3f && std::abs(vy_) < 1e-3f) return 0.f;
    return std::atan2(vy_, vx_) * 180.0f / 3.14159265358979323846f;
}

uint8_t TrackState::status() const {
    if (frames_missed_ > 0)                  return 2;   // lost
    if (frames_seen_   < kTentativeFrames)   return 0;   // tentative
    return 1;                                            // active
}

void TrackState::mark_missed() {
    frames_missed_       += 1;
    frames_since_first_  += 1;
    roi_entered_          = false;
    roi_exited_           = false;
}

void TrackState::update(const Observation& obs, const Roi& roi) {
    // ── Bookkeeping ───────────────────────────────────────────────────────
    const bool first = (frames_seen_ == 0);
    frames_seen_        += 1;
    frames_since_first_ += 1;
    frames_missed_       = 0;
    last_seen_us_        = obs.timestamp_us;

    // ── Velocity (px/s) via finite-difference, EMA-smoothed ───────────────
    if (!first) {
        const float dt = (obs.timestamp_us - last_.timestamp_us) * 1e-6f;
        if (dt > 1e-4f) {
            const float instant_vx = (obs.cx - last_.cx) / dt;
            const float instant_vy = (obs.cy - last_.cy) / dt;
            vx_ = kEmaAlpha * instant_vx + (1.f - kEmaAlpha) * vx_;
            vy_ = kEmaAlpha * instant_vy + (1.f - kEmaAlpha) * vy_;
        }
    }

    // ── Acceleration (Δspeed / dt) ────────────────────────────────────────
    const float cur_speed = std::sqrt(vx_ * vx_ + vy_ * vy_);
    if (!first && last_seen_us_ > last_.timestamp_us) {
        const float dt = (obs.timestamp_us - last_.timestamp_us) * 1e-6f;
        accel_ = dt > 1e-4f ? (cur_speed - prev_speed_) / dt : 0.f;
    }
    prev_speed_ = cur_speed;

    // ── Append to history ring ────────────────────────────────────────────
    last_ = obs;
    history_.push_back(obs);
    if (history_.size() > kHistoryMax) history_.pop_front();

    // ── Loiter score: low spread of centroids in the recent window ────────
    // Use bbox of centroids in history; score = 1 if all within kLoiterRadius.
    if (history_.size() >= 4) {
        int32_t xmin = history_.front().cx, xmax = xmin;
        int32_t ymin = history_.front().cy, ymax = ymin;
        for (const auto& h : history_) {
            xmin = std::min(xmin, h.cx); xmax = std::max(xmax, h.cx);
            ymin = std::min(ymin, h.cy); ymax = std::max(ymax, h.cy);
        }
        const float spread = std::max(xmax - xmin, ymax - ymin);
        const float ratio  = spread / static_cast<float>(kLoiterRadius);
        loiter_score_ = std::max(0.f, std::min(1.f, 1.0f - ratio));
    } else {
        loiter_score_ = 0.f;
    }

    // ── ROI membership + transition events ────────────────────────────────
    prev_in_roi_ = in_roi_;
    in_roi_      = roi.contains(obs.cx, obs.cy);
    roi_entered_ =  in_roi_ && !prev_in_roi_;
    roi_exited_  = !in_roi_ &&  prev_in_roi_;
}
