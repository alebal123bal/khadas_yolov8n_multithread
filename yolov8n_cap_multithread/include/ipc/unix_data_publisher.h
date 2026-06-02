#pragma once

// unix_data_publisher.h — Unix-domain-socket implementation of IDataPublisher.
//
// ── Transport ──────────────────────────────────────────────────────────────
//   One AF_UNIX SOCK_STREAM socket at kDataSocketPath.
//   Accepts one consumer at a time. If the consumer disconnects, the sender
//   thread loops back to accept() and waits for a new one. Frames accumulated
//   during the wait continue to be dropped per BoundedQueue::kDropOldest.
//
// ── Protocol ───────────────────────────────────────────────────────────────
//   Each frame is framed as:
//     [uint32_t LE body_len]
//     [WireFrameHeader           — 24 bytes]
//     [WireDetectionRecord × N   — 36 bytes each]
//   See wire_protocol.h for full field layout.
//
// ── Drop policy ────────────────────────────────────────────────────────────
//   kDropOldest: if the queue is full, the oldest (front) frame is discarded.
//   This ensures a slow consumer always receives recent detections rather than
//   stale data. The queue depth is configurable at construction time.
//
// ── Thread model ───────────────────────────────────────────────────────────
//   start() spawns senderLoop() on its own std::thread.
//   stop()  sets running_=false, closes the queue (unblocks pop()),
//           closes listen_fd_ (unblocks accept()), then joins.
//   publish() is non-blocking: it acquires the queue mutex, pushes, releases.

#include "ipc/i_data_publisher.h"
#include "ipc/bounded_queue.h"
#include "ipc/messages.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>

class UnixDataPublisher : public IDataPublisher {
public:
    /// @param socket_path    Full path for the Unix domain socket, e.g.
    ///                       ipc_data_path(device_number).  Each running
    ///                       pipeline instance must use a unique path.
    /// @param queueCapacity  Maximum frames buffered before dropping oldest.
    explicit UnixDataPublisher(std::string socket_path,
                               std::size_t queueCapacity = 32);
    ~UnixDataPublisher() override;

    void        start()                       override;
    void        stop()                        override;
    bool        publish(DetectionMessage msg) override;
    std::size_t queueDepth()            const override;

private:
    void senderLoop();
    bool sendFrame(int fd, const DetectionMessage& msg);

    std::string                    socket_path_;
    BoundedQueue<DetectionMessage> queue_;
    int                            listen_fd_{-1};
    std::thread                    thread_;
    std::atomic<bool>              running_{false};
};
