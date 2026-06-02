// tools/events_receiver.cc — Demo consumer for the temporal events stream.
//
// Connects to /tmp/yolo_events_<dev>.sock and prints one line per track per
// frame summarizing its temporal features.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static std::string ipc_events_path(const std::string& dev) {
    return "/tmp/yolo_events_" + dev + ".sock";
}

#pragma pack(push, 1)
struct WireFrameHeader {
    uint64_t frame_id;
    uint64_t timestamp_us;
    uint16_t count;
    uint8_t  reserved[6];
};
struct WireTrackSummary {
    char     class_name[16];
    uint32_t track_id;
    uint32_t frames_seen;
    uint64_t first_seen_us;
    uint64_t last_seen_us;
    uint32_t duration_ms;
    float    visibility;
    int32_t  cx, cy;
    int32_t  left, top, right, bottom;
    float    vx, vy;
    float    speed;
    float    heading_deg;
    float    accel;
    float    loiter_score;
    uint8_t  in_roi;
    uint8_t  roi_entered;
    uint8_t  roi_exited;
    uint8_t  status;
    uint8_t  reserved[4];
};
#pragma pack(pop)

static bool readExact(int fd, void* buf, uint32_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) { ssize_t r = ::read(fd, p, n); if (r <= 0) return false; p += r; n -= (uint32_t)r; }
    return true;
}

static const char* status_str(uint8_t s) {
    switch (s) { case 0: return "TENT"; case 1: return "ACTV"; case 2: return "LOST"; }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device>\n", argv[0]);
        return 1;
    }
    const std::string sock = ipc_events_path(argv[1]);

    int fd = -1;
    while (fd < 0) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }
        sockaddr_un addr{}; addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;
        ::close(fd); fd = -1;
        fprintf(stderr, "[events_receiver] waiting for %s ...\n", sock.c_str());
        sleep(1);
    }
    printf("[events_receiver] connected to %s\n", sock.c_str());

    while (true) {
        uint32_t body_len = 0;
        if (!readExact(fd, &body_len, sizeof(body_len))) break;
        WireFrameHeader hdr{};
        if (!readExact(fd, &hdr, sizeof(hdr))) break;

        for (uint16_t i = 0; i < hdr.count; ++i) {
            WireTrackSummary s{};
            if (!readExact(fd, &s, sizeof(s))) { fprintf(stderr, "read err\n"); return 1; }
            const char roi_tag = s.roi_entered ? 'E' : s.roi_exited ? 'X' : (s.in_roi ? 'I' : '-');
            printf("f=%-6llu id=%-4u %-12s %s dur=%5ums vis=%.2f "
                   "c=(%4d,%4d) v=(%+6.1f,%+6.1f) sp=%5.1f hd=%+6.1f° "
                   "a=%+6.1f loit=%.2f roi=%c\n",
                   (unsigned long long)hdr.frame_id, s.track_id, s.class_name,
                   status_str(s.status), s.duration_ms, s.visibility,
                   s.cx, s.cy, s.vx, s.vy, s.speed, s.heading_deg,
                   s.accel, s.loiter_score, roi_tag);
        }
        fflush(stdout);
    }
    ::close(fd);
    return 0;
}
