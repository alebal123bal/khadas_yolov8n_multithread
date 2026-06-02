#pragma once

// i_data_publisher.h — Pure interface for the data-plane publisher.
//
// The inference pipeline calls publish() after each frame. The implementation
// is responsible for buffering, serialization, and transmission on its own
// thread without blocking the caller.

#include <cstddef>
#include "ipc/messages.h"

class IDataPublisher {
public:
    virtual ~IDataPublisher() = default;

    /// Bind the socket, start the background sender thread, and return.
    virtual void start() = 0;

    /// Signal the sender to stop; close the socket; join the thread.
    virtual void stop() = 0;

    /// Non-blocking: enqueue a detection frame for transmission.
    ///
    /// @return true  — frame was accepted without evicting anything.
    /// @return false — a frame was dropped per the queue's drop policy.
    ///                 The NEW frame is still enqueued (kDropOldest policy).
    ///
    /// This must never block the caller for more than a mutex acquisition.
    virtual bool publish(DetectionMessage msg) = 0;

    /// Number of frames currently waiting to be sent.
    /// Used by the control server's get_status response.
    virtual std::size_t queueDepth() const = 0;
};
