// unix_data_publisher.cc — Implementation of UnixDataPublisher.

#include "ipc/unix_data_publisher.h"
#include "ipc/wire_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>

// ─── I/O helpers ──────────────────────────────────────────────────────────
namespace {

/// Write exactly `n` bytes to `fd`.
/// Uses MSG_NOSIGNAL to suppress SIGPIPE when the consumer has disconnected;
/// EPIPE is returned as a write error instead of raising a signal.
static bool writeExact(int fd, const void* buf, uint32_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return false;  // EPIPE or other error → consumer gone
        p += w;
        n -= static_cast<uint32_t>(w);
    }
    return true;
}

} // anonymous namespace

// ─── UnixDataPublisher ────────────────────────────────────────────────────

UnixDataPublisher::UnixDataPublisher(std::string socket_path, std::size_t queueCapacity)
    : socket_path_(std::move(socket_path)),
      queue_(queueCapacity, BoundedQueue<DetectionMessage>::DropPolicy::kDropOldest) {}

UnixDataPublisher::~UnixDataPublisher() {
    stop();
}

void UnixDataPublisher::start() {
    ::unlink(socket_path_.c_str());

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("[data] socket");
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[data] bind");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    // Backlog of 1: we accept one consumer at a time.
    if (::listen(listen_fd_, 1) < 0) {
        perror("[data] listen");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    running_ = true;
    thread_  = std::thread(&UnixDataPublisher::senderLoop, this);
    printf("[data] listening on %s\n", socket_path_.c_str());
}

void UnixDataPublisher::stop() {
    running_ = false;
    // Unblock pop() in senderLoop so the thread can exit the inner drain loop.
    queue_.close();
    // Unblock accept() in senderLoop so the thread can exit the outer loop.
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    ::unlink(socket_path_.c_str());
}

bool UnixDataPublisher::publish(DetectionMessage msg) {
    // Non-blocking: returns immediately after enqueuing (or dropping).
    return queue_.push(std::move(msg));
}

std::size_t UnixDataPublisher::queueDepth() const {
    return queue_.size();
}

void UnixDataPublisher::senderLoop() {
    while (running_) {
        // ── Wait for a consumer ──────────────────────────────────────────
        sockaddr_un peer{};
        socklen_t   peer_len = sizeof(peer);
        int conn = ::accept(listen_fd_,
                            reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (conn < 0) {
            if (running_) perror("[data] accept");
            break;
        }
        printf("[data] consumer connected\n");

        // ── Drain the queue into the connected consumer ──────────────────
        // If sendFrame() fails (EPIPE, etc.), we break out and go back to
        // accept() to wait for a new consumer. Frames keep accumulating in
        // the bounded queue during the wait; the oldest are dropped if it
        // fills up.
        while (running_) {
            DetectionMessage msg;
            if (!queue_.pop(msg, std::chrono::milliseconds(200))) {
                // Timed out waiting for a frame — check running_ and retry.
                continue;
            }
            if (!sendFrame(conn, msg)) {
                printf("[data] consumer disconnected (send error)\n");
                break;
            }
        }
        ::close(conn);
    }
}

bool UnixDataPublisher::sendFrame(int fd, const DetectionMessage& msg) {
    // Compute actual body size: only serialize the detections that exist.
    const uint16_t count      = static_cast<uint16_t>(msg.group.count);
    const uint32_t body_size  = static_cast<uint32_t>(sizeof(WireFrameHeader))
                              + static_cast<uint32_t>(count) * sizeof(WireDetectionRecord);

    // 1. Length prefix (4 bytes, LE uint32).
    if (!writeExact(fd, &body_size, sizeof(body_size))) return false;

    // 2. Frame header (24 bytes).
    WireFrameHeader hdr{};
    hdr.frame_id     = msg.frame_id;
    hdr.timestamp_us = msg.timestamp_us;
    hdr.count        = count;
    if (!writeExact(fd, &hdr, sizeof(hdr))) return false;

    // 3. Per-detection records (36 bytes each).
    for (int i = 0; i < msg.group.count; ++i) {
        const detect_result_t& src = msg.group.results[i];
        WireDetectionRecord rec{};
        std::strncpy(rec.class_name, src.name, sizeof(rec.class_name) - 1);
        rec.confidence = src.prop;
        rec.left       = src.box.left;
        rec.top        = src.box.top;
        rec.right      = src.box.right;
        rec.bottom     = src.box.bottom;
        if (!writeExact(fd, &rec, sizeof(rec))) return false;
    }

    return true;
}
