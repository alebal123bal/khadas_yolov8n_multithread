// bytetrack_service.cc — Standalone tracker process.
//
// Reads raw detections from the YOLO data socket, runs IByteTracker, and
// republishes tracked detections on a separate "tracks" socket.
//
// Usage:
//   ./bytetrack_service <device>
//     <device>  V4L2 device number used to launch the YOLO pipeline (e.g. 33).
//
// Sockets:
//   Input  (consumer)  : /tmp/yolo_data_<device>.sock     [WireDetectionRecord]
//   Output (publisher) : /tmp/yolo_tracks_<device>.sock   [WireTrackRecord]
//
// Design notes:
//   - This service is intentionally independent of the YOLO process. If it
//     stops or crashes, the YOLO pipeline keeps running and just drops frames
//     into its bounded queue (kDropOldest), per existing behaviour.
//   - The output socket accepts ONE downstream consumer at a time, mirroring
//     UnixDataPublisher semantics. If no consumer is connected, frames are
//     simply not sent (but the tracker still updates internal state).
//   - No threads, no shared state: a single synchronous loop is sufficient
//     because tracking is cheap relative to detection.

#include "bytetrack_adapter.h"
#include "ipc/wire_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ─── I/O helpers ──────────────────────────────────────────────────────────
namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

bool readExact(int fd, void* buf, uint32_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= static_cast<uint32_t>(r);
    }
    return true;
}

bool writeExact(int fd, const void* buf, uint32_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        p += w; n -= static_cast<uint32_t>(w);
    }
    return true;
}

// ── Input: connect to the YOLO data socket (retry until available) ────────
int connect_input(const std::string& path) {
    while (g_running) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("[bytetrack] socket(input)"); return -1; }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            fprintf(stderr, "[bytetrack] connected to %s\n", path.c_str());
            return fd;
        }
        ::close(fd);
        fprintf(stderr, "[bytetrack] waiting for %s ...\n", path.c_str());
        sleep(1);
    }
    return -1;
}

// ── Output: bind+listen tracks socket; accept at most one consumer ────────
int bind_output(const std::string& path) {
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("[bytetrack] socket(output)"); return -1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[bytetrack] bind(output)"); ::close(fd); return -1;
    }
    if (::listen(fd, 1) < 0) {
        perror("[bytetrack] listen(output)"); ::close(fd); return -1;
    }
    // Non-blocking so we can opportunistically accept without stalling I/O.
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    fprintf(stderr, "[bytetrack] tracks socket listening on %s\n", path.c_str());
    return fd;
}

// Accept a new consumer if one is waiting. Returns >=0 on success, -1 if none.
int try_accept(int listen_fd) {
    sockaddr_un peer{};
    socklen_t   peer_len = sizeof(peer);
    int conn = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (conn >= 0) fprintf(stderr, "[bytetrack] tracks consumer connected\n");
    return conn;
}

// Send one tracked frame to the consumer; returns false if it disconnected.
bool send_tracks(int fd,
                 const WireFrameHeader& hdr_in,
                 const std::vector<TrackerOutput>& tracks)
{
    WireFrameHeader hdr = hdr_in;
    hdr.count           = static_cast<uint16_t>(tracks.size());

    const uint32_t body_size =
        static_cast<uint32_t>(sizeof(WireFrameHeader)) +
        static_cast<uint32_t>(tracks.size()) * sizeof(WireTrackRecord);

    if (!writeExact(fd, &body_size, sizeof(body_size))) return false;
    if (!writeExact(fd, &hdr,       sizeof(hdr)))       return false;

    for (const auto& t : tracks) {
        WireTrackRecord rec{};
        std::strncpy(rec.class_name, t.det.class_name, sizeof(rec.class_name) - 1);
        rec.confidence = t.det.confidence;
        rec.left       = t.det.left;
        rec.top        = t.det.top;
        rec.right      = t.det.right;
        rec.bottom     = t.det.bottom;
        rec.track_id   = t.track_id;
        if (!writeExact(fd, &rec, sizeof(rec))) return false;
    }
    return true;
}

} // namespace

// ─── main loop ────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <device>\n", argv[0]);
        return 1;
    }
    const std::string device      = argv[1];
    const std::string in_path     = ipc_data_path(device);
    const std::string out_path    = ipc_tracks_path(device);

    ::signal(SIGINT,  on_signal);
    ::signal(SIGTERM, on_signal);
    ::signal(SIGPIPE, SIG_IGN);

    auto tracker = make_tracker();

    const int out_listen = bind_output(out_path);
    if (out_listen < 0) return 1;
    int out_conn = -1;   // current downstream consumer fd, or -1

    while (g_running) {
        // (Re)connect to the YOLO data socket.
        const int in_fd = connect_input(in_path);
        if (in_fd < 0) break;

        // Frame loop.
        while (g_running) {
            // 1. Read length prefix + header.
            uint32_t body_len = 0;
            if (!readExact(in_fd, &body_len, sizeof(body_len))) break;
            if (body_len < sizeof(WireFrameHeader)) {
                fprintf(stderr, "[bytetrack] body too small (%u)\n", body_len);
                break;
            }
            WireFrameHeader hdr{};
            if (!readExact(in_fd, &hdr, sizeof(hdr))) break;

            // 2. Read detection records.
            const uint32_t det_bytes =
                static_cast<uint32_t>(hdr.count) * sizeof(WireDetectionRecord);
            if (body_len != sizeof(WireFrameHeader) + det_bytes) {
                fprintf(stderr, "[bytetrack] body length mismatch\n");
                break;
            }

            std::vector<TrackerDetection> dets(hdr.count);
            for (uint16_t i = 0; i < hdr.count; ++i) {
                WireDetectionRecord rec{};
                if (!readExact(in_fd, &rec, sizeof(rec))) { hdr.count = 0; break; }
                TrackerDetection& d = dets[i];
                std::memcpy(d.class_name, rec.class_name, sizeof(d.class_name));
                d.confidence = rec.confidence;
                d.left       = rec.left;
                d.top        = rec.top;
                d.right      = rec.right;
                d.bottom     = rec.bottom;
            }

            // 3. Update tracker.
            const auto tracks = tracker->update(hdr.frame_id, dets);

            // 4. Opportunistically accept a downstream consumer.
            if (out_conn < 0) out_conn = try_accept(out_listen);

            // 5. Publish if a consumer is attached.
            if (out_conn >= 0) {
                if (!send_tracks(out_conn, hdr, tracks)) {
                    fprintf(stderr, "[bytetrack] tracks consumer disconnected\n");
                    ::close(out_conn);
                    out_conn = -1;
                }
            }
        }

        ::close(in_fd);
        if (g_running) {
            fprintf(stderr, "[bytetrack] YOLO disconnected, reconnecting...\n");
        }
    }

    if (out_conn >= 0) ::close(out_conn);
    ::close(out_listen);
    ::unlink(out_path.c_str());
    return 0;
}
