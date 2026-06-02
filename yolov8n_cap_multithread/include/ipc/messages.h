#pragma once

// messages.h — In-memory message types that flow through the pipeline.
//
// DetectionMessage is the in-process representation of one inference result.
// It is a thin wrapper around detect_result_group_t so that no re-packing
// occurs on the hot path; serialization to the wire format (WireFrameHeader +
// WireDetectionRecord[]) happens only in UnixDataPublisher::sendFrame().
//
// This header depends on postprocess.h (RKNN-SDK types). Consumer tools that
// only need the wire format should include wire_protocol.h instead.

#include <cstdint>
#include "postprocess.h"       // detect_result_group_t

struct DetectionMessage {
    uint64_t              frame_id     = 0;   ///< Monotonic counter from YoloControlState.
    uint64_t              timestamp_us = 0;   ///< gettimeofday epoch-us; set at result pop.
    detect_result_group_t group        = {};  ///< Raw postprocess output (boxes + labels).
};
