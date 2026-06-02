// temporal_service.cc — Standalone temporal-features service.
//
// Reads tracked detections from bytetrack_service on /tmp/yolo_tracks_<dev>.sock,
// maintains per-track temporal state via TrackManager, and republishes a
// compact WireTrackSummary stream on /tmp/yolo_events_<dev>.sock.
//
// Usage:
//   ./temporal_service <device> [--roi L,T,R,B]
//
// Example:
//   ./temporal_service 33 --roi 400,200,1500,800

#include "ipc/wire_protocol.h"
#include "track_manager.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

bool readExact(int fd, void* buf, uint32_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) { ssize_t r = ::read(fd, p, n); if (r <= 0) return false; p += r; n -= (uint32_t)r; }
    return true;
}
bool writeExact(int fd, const void* buf, uint32_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) { ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return false; p += w; n -= (uint32_t)w; }
    return true;
}

int connect_input(const std::string& path) {
    while (g_running) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("[temporal] socket(in)"); return -1; }
        sockaddr_un addr{}; addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            fprintf(stderr, "[temporal] connected to %s\n", path.c_str());
            return fd;
        }
        ::close(fd);
        fprintf(stderr, "[temporal] waiting for %s ...\n", path.c_str());
        sleep(1);
    }
    return -1;
}

int bind_output(const std::string& path) {
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("[temporal] socket(out)"); return -1; }
    sockaddr_un addr{}; addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[temporal] bind(out)"); ::close(fd); return -1;
    }
    if (::listen(fd, 1) < 0) { perror("[temporal] listen"); ::close(fd); return -1; }
    int flags = ::fcntl(fd, F_GETFL, 0); ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fprintf(stderr, "[temporal] events socket listening on %s\n", path.c_str());
    return fd;
}

int try_accept(int listen_fd) {
    sockaddr_un peer{}; socklen_t pl = sizeof(peer);
    int c = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &pl);
    if (c >= 0) fprintf(stderr, "[temporal] events consumer connected\n");
    return c;
}

void fill_summary(const TrackState& s, WireTrackSummary& out) {
    std::memset(&out, 0, sizeof(out));
    std::strncpy(out.class_name, s.class_name(), sizeof(out.class_name) - 1);
    out.track_id      = s.track_id();
    out.frames_seen   = s.frames_seen();
    out.first_seen_us = s.first_seen_us();
    out.last_seen_us  = s.last_seen_us();
    out.duration_ms   = s.duration_ms();
    out.visibility    = s.visibility();
    out.cx            = s.cx();
    out.cy            = s.cy();
    out.left          = s.left();
    out.top           = s.top();
    out.right         = s.right();
    out.bottom        = s.bottom();
    out.vx            = s.vx();
    out.vy            = s.vy();
    out.speed         = s.speed();
    out.heading_deg   = s.heading_deg();
    out.accel         = s.accel();
    out.loiter_score  = s.loiter_score();
    out.in_roi        = s.in_roi()          ? 1 : 0;
    out.roi_entered   = s.roi_entered_now() ? 1 : 0;
    out.roi_exited    = s.roi_exited_now()  ? 1 : 0;
    out.status        = s.status();
}

bool publish_frame(int fd, const WireFrameHeader& hdr_in,
                   const std::vector<const TrackState*>& tracks)
{
    WireFrameHeader hdr = hdr_in;
    hdr.count = static_cast<uint16_t>(tracks.size());
    const uint32_t body_size = sizeof(WireFrameHeader)
                             + static_cast<uint32_t>(tracks.size()) * sizeof(WireTrackSummary);
    if (!writeExact(fd, &body_size, sizeof(body_size))) return false;
    if (!writeExact(fd, &hdr,       sizeof(hdr)))       return false;
    for (const TrackState* t : tracks) {
        WireTrackSummary s;
        fill_summary(*t, s);
        if (!writeExact(fd, &s, sizeof(s))) return false;
    }
    return true;
}

bool parse_roi(const char* arg, Roi& roi) {
    int l, t, r, b;
    if (std::sscanf(arg, "%d,%d,%d,%d", &l, &t, &r, &b) != 4) return false;
    roi.left = l; roi.top = t; roi.right = r; roi.bottom = b; roi.enabled = true;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device> [--roi L,T,R,B]\n", argv[0]);
        return 1;
    }
    const std::string device   = argv[1];
    const std::string in_path  = ipc_tracks_path(device);
    const std::string out_path = ipc_events_path(device);

    Roi roi;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--roi") == 0 && i + 1 < argc) {
            if (!parse_roi(argv[++i], roi)) {
                fprintf(stderr, "[temporal] bad --roi value\n"); return 1;
            }
            fprintf(stderr, "[temporal] ROI = (%d,%d)-(%d,%d)\n",
                    roi.left, roi.top, roi.right, roi.bottom);
        }
    }

    ::signal(SIGINT,  on_signal);
    ::signal(SIGTERM, on_signal);
    ::signal(SIGPIPE, SIG_IGN);

    TrackManager mgr(roi);

    const int out_listen = bind_output(out_path);
    if (out_listen < 0) return 1;
    int out_conn = -1;

    while (g_running) {
        const int in_fd = connect_input(in_path);
        if (in_fd < 0) break;

        while (g_running) {
            uint32_t body_len = 0;
            if (!readExact(in_fd, &body_len, sizeof(body_len))) break;
            if (body_len < sizeof(WireFrameHeader)) {
                fprintf(stderr, "[temporal] body too small (%u)\n", body_len); break;
            }
            WireFrameHeader hdr{};
            if (!readExact(in_fd, &hdr, sizeof(hdr))) break;

            const uint32_t expected = sizeof(WireFrameHeader)
                                    + static_cast<uint32_t>(hdr.count) * sizeof(WireTrackRecord);
            if (body_len != expected) {
                fprintf(stderr, "[temporal] body length mismatch\n"); break;
            }

            mgr.begin_frame();
            for (uint16_t i = 0; i < hdr.count; ++i) {
                WireTrackRecord rec{};
                if (!readExact(in_fd, &rec, sizeof(rec))) { hdr.count = 0; break; }
                mgr.observe(rec, hdr.timestamp_us);
            }
            const auto& view = mgr.end_frame();

            if (out_conn < 0) out_conn = try_accept(out_listen);
            if (out_conn >= 0) {
                if (!publish_frame(out_conn, hdr, view)) {
                    fprintf(stderr, "[temporal] events consumer disconnected\n");
                    ::close(out_conn); out_conn = -1;
                }
            }
        }

        ::close(in_fd);
        if (g_running) fprintf(stderr, "[temporal] tracks disconnected, reconnecting...\n");
    }

    if (out_conn >= 0) ::close(out_conn);
    ::close(out_listen);
    ::unlink(out_path.c_str());
    return 0;
}
