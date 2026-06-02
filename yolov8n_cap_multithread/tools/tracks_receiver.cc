// tools/tracks_receiver.cc — Example consumer for the ByteTrack tracks stream.
//
// Connects to /tmp/yolo_tracks_<device>.sock, reads length-prefixed binary
// frames published by bytetrack_service, decodes WireFrameHeader +
// WireTrackRecord[], and prints each tracked detection (with track_id).
//
// Run:
//   ./tracks_receiver <device>           # streams until Ctrl-C or service stops
//   ./tracks_receiver <device> --count   # also prints per-frame count
//
// Examples:
//   ./tracks_receiver 33
//   ./tracks_receiver 51 --count

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

// ─── Wire types (copied inline to keep the tool self-contained) ────────────

static std::string ipc_tracks_path(const std::string& dev) {
    return "/tmp/yolo_tracks_" + dev + ".sock";
}

#pragma pack(push, 1)
struct WireFrameHeader {
    uint64_t frame_id;
    uint64_t timestamp_us;
    uint16_t count;
    uint8_t  reserved[6];
};

struct WireTrackRecord {
    char    class_name[16];
    float   confidence;
    int32_t left, top, right, bottom;
    uint32_t track_id;
};
#pragma pack(pop)

// ─── I/O helpers ──────────────────────────────────────────────────────────

static bool readExact(int fd, void* buf, uint32_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= static_cast<uint32_t>(r);
    }
    return true;
}

// ─── Main receive loop ─────────────────────────────────────────────────────

static void receiveLoop(int fd, bool show_count) {
    uint64_t frames_received = 0;

    while (true) {
        uint32_t body_len = 0;
        if (!readExact(fd, &body_len, sizeof(body_len))) {
            fprintf(stderr, "[tracks_receiver] connection closed after %llu frames\n",
                    static_cast<unsigned long long>(frames_received));
            return;
        }
        if (body_len < sizeof(WireFrameHeader)) {
            fprintf(stderr, "[tracks_receiver] body too small (%u bytes), aborting\n", body_len);
            return;
        }

        WireFrameHeader hdr{};
        if (!readExact(fd, &hdr, sizeof(hdr))) return;

        const uint32_t expected_bytes =
            static_cast<uint32_t>(hdr.count) * sizeof(WireTrackRecord);
        const uint32_t expected_body  =
            static_cast<uint32_t>(sizeof(WireFrameHeader)) + expected_bytes;

        if (body_len != expected_body) {
            fprintf(stderr, "[tracks_receiver] body len mismatch (got %u, expected %u)\n",
                    body_len, expected_body);
            return;
        }

        WireTrackRecord recs[64];  // kMaxDetections = 64
        if (hdr.count > 0) {
            if (!readExact(fd, recs, expected_bytes)) return;
        }

        ++frames_received;

        time_t   sec  = static_cast<time_t>(hdr.timestamp_us / 1000000ULL);
        uint32_t usec = static_cast<uint32_t>(hdr.timestamp_us % 1000000ULL);
        struct tm* tm_info = localtime(&sec);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);

        if (show_count || hdr.count > 0) {
            printf("frame %6llu  [%s.%06u]  %u track(s)\n",
                   static_cast<unsigned long long>(hdr.frame_id),
                   timebuf, usec, hdr.count);
        }

        for (uint16_t i = 0; i < hdr.count; ++i) {
            const WireTrackRecord& r = recs[i];
            printf("  id=%-4u %-15s  conf=%.2f  box=(%d,%d,%d,%d)\n",
                   r.track_id, r.class_name, r.confidence,
                   r.left, r.top, r.right, r.bottom);
        }

        fflush(stdout);
    }
}

int main(int argc, char** argv) {
    if (argc < 2 || argv[1][0] == '-') {
        fprintf(stderr,
            "Usage: %s <device> [--count]\n"
            "  <device>  V4L2 device number used to launch the pipeline (e.g. 33, 51)\n"
            "  --count   print a line for every frame even when 0 tracks\n",
            argv[0]);
        return 1;
    }

    const std::string device    = argv[1];
    bool show_count = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--count") show_count = true;
    }

    const std::string sock_path = ipc_tracks_path(device);

    int fd = -1;
    while (fd < 0) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;

        ::close(fd);
        fd = -1;
        fprintf(stderr, "[tracks_receiver] waiting for %s ...\n", sock_path.c_str());
        sleep(1);
    }

    printf("[tracks_receiver] connected to %s\n", sock_path.c_str());
    receiveLoop(fd, show_count);
    ::close(fd);
    return 0;
}
