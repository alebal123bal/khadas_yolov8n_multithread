#pragma once

// track_state.h — per-track temporal state and feature computation.
//
// Holds a short ring buffer of recent observations for one track and derives
// compact temporal features (velocity, heading, acceleration, loiter score,
// ROI events). Designed as a small, self-contained value type owned by
// TrackManager.

#include <cstdint>
#include <deque>
#include <string>

struct Observation {
    uint64_t timestamp_us;
    int32_t  cx, cy;
    int32_t  left, top, right, bottom;
};

struct Roi {
    int32_t left{0}, top{0}, right{0}, bottom{0};
    bool    enabled{false};
    bool    contains(int32_t x, int32_t y) const {
        return enabled && x >= left && x < right && y >= top && y < bottom;
    }
};

class TrackState {
public:
    TrackState(uint32_t track_id,
               const char* class_name,
               uint64_t first_seen_us);

    /// Append a new observation; updates all derived features.
    void update(const Observation& obs, const Roi& roi);

    /// Called when this track was NOT seen in the current frame.
    /// Increments `frames_since_first` so visibility decays.
    void mark_missed();

    // ── Accessors used by serialization in temporal_service ──────────────
    uint32_t    track_id()         const { return track_id_; }
    const char* class_name()       const { return class_name_; }
    uint64_t    first_seen_us()    const { return first_seen_us_; }
    uint64_t    last_seen_us()     const { return last_seen_us_; }
    uint32_t    frames_seen()      const { return frames_seen_; }
    uint32_t    duration_ms()      const;
    float       visibility()       const;
    int32_t     cx()               const { return last_.cx; }
    int32_t     cy()               const { return last_.cy; }
    int32_t     left()             const { return last_.left; }
    int32_t     top()              const { return last_.top; }
    int32_t     right()            const { return last_.right; }
    int32_t     bottom()           const { return last_.bottom; }
    float       vx()               const { return vx_; }
    float       vy()               const { return vy_; }
    float       speed()            const;
    float       heading_deg()      const;
    float       accel()            const { return accel_; }
    float       loiter_score()     const { return loiter_score_; }
    bool        in_roi()           const { return in_roi_; }
    bool        roi_entered_now()  const { return roi_entered_; }
    bool        roi_exited_now()   const { return roi_exited_; }
    uint32_t    frames_since_last_seen() const { return frames_missed_; }

    /// 0 tentative, 1 active, 2 lost.
    uint8_t status() const;

private:
    static constexpr size_t kHistoryMax    = 16;
    static constexpr float  kEmaAlpha      = 0.5f;   // velocity smoothing
    static constexpr int    kLoiterRadius  = 25;     // px; below → loitering
    static constexpr int    kTentativeFrames = 3;    // need >=N obs for "active"

    uint32_t              track_id_;
    char                  class_name_[16]{};
    uint64_t              first_seen_us_;
    uint64_t              last_seen_us_{0};
    uint32_t              frames_seen_{0};
    uint32_t              frames_since_first_{0};
    uint32_t              frames_missed_{0};

    Observation           last_{};
    std::deque<Observation> history_;

    float                 vx_{0.f}, vy_{0.f};   // EMA-smoothed
    float                 prev_speed_{0.f};
    float                 accel_{0.f};
    float                 loiter_score_{0.f};

    bool                  in_roi_{false};
    bool                  prev_in_roi_{false};
    bool                  roi_entered_{false};
    bool                  roi_exited_{false};
};
