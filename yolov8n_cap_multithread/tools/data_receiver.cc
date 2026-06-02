// tools/data_receiver.cc — Example data receiver for the YOLO detection stream.
//
// Connects to the data socket, reads length-prefixed binary frames, decodes
// WireFrameHeader + WireDetectionRecord[], and prints each detection to stdout.
//
// This is a minimal reference implementation. A real consumer could:
//   - Forward detections to a GUI / tracker
//   - Write to a time-series database
//   - Trigger events on specific class detections
//
// Cross-compiled for aarch64 together with the main pipeline via CMakeLists.txt.
// The binary is installed into install/yolov8n_cap_multithread/ by 'make install'
// and scp'd to the board by build_and_load.sh.
//
// Run:
//   ./data_receiver <device>           # streams until Ctrl-C or pipeline stops
//   ./data_receiver <device> --count  # also prints per-frame detection count
//
// <device> is the V4L2 device number used to launch the pipeline (e.g. 33, 51):
//   ./data_receiver 33
//   ./data_receiver 51 --count

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

// ─── Wire types (copied inline to keep the tool self-contained) ────────────
// If built as part of the main project, include wire_protocol.h instead.

static std::string ipc_data_path(const std::string& dev) {
    return "/tmp/yolo_data_" + dev + ".sock";
}

#pragma pack(push, 1)
struct WireFrameHeader {
    uint64_t frame_id;
    uint64_t timestamp_us;
    uint16_t count;
    uint8_t  reserved[6];
};

struct WireDetectionRecord {
    char    class_name[16];
    float   confidence;
    int32_t left, top, right, bottom;
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
        // 1. Read 4-byte length prefix.
        uint32_t body_len = 0;
        if (!readExact(fd, &body_len, sizeof(body_len))) {
            fprintf(stderr, "[data_receiver] connection closed after %llu frames\n",
                    static_cast<unsigned long long>(frames_received));
            return;
        }

        // Sanity check: header must always be present.
        if (body_len < sizeof(WireFrameHeader)) {
            fprintf(stderr, "[data_receiver] body too small (%u bytes), aborting\n", body_len);
            return;
        }

        // 2. Read header.
        WireFrameHeader hdr{};
        if (!readExact(fd, &hdr, sizeof(hdr))) return;

        // 3. Read detection records.
        const uint32_t expected_det_bytes =
            static_cast<uint32_t>(hdr.count) * sizeof(WireDetectionRecord);
        const uint32_t expected_body      =
            static_cast<uint32_t>(sizeof(WireFrameHeader)) + expected_det_bytes;

        if (body_len != expected_body) {
            fprintf(stderr, "[data_receiver] body len mismatch (got %u, expected %u)\n",
                    body_len, expected_body);
            return;
        }

        WireDetectionRecord dets[64];  // kMaxDetections = 64
        if (hdr.count > 0) {
            if (!readExact(fd, dets, expected_det_bytes)) return;
        }

        ++frames_received;

        // 4. Print frame.
        // Convert timestamp_us to a human-readable time.
        time_t   sec  = static_cast<time_t>(hdr.timestamp_us / 1000000ULL);
        uint32_t usec = static_cast<uint32_t>(hdr.timestamp_us % 1000000ULL);
        struct tm* tm_info = localtime(&sec);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);

        if (show_count || hdr.count > 0) {
            printf("frame %6llu  [%s.%06u]  %u detection(s)\n",
                   static_cast<unsigned long long>(hdr.frame_id),
                   timebuf, usec, hdr.count);
        }

        for (uint16_t i = 0; i < hdr.count; ++i) {
            const WireDetectionRecord& d = dets[i];
            printf("  [%u] %-15s  conf=%.2f  box=(%d,%d,%d,%d)\n",
                   i, d.class_name, d.confidence,
                   d.left, d.top, d.right, d.bottom);
        }

        fflush(stdout);
    }
}

int main(int argc, char** argv) {
    if (argc < 2 || argv[1][0] == '-') {
        fprintf(stderr,
            "Usage: %s <device> [--count]\n"
            "  <device>  V4L2 device number used to launch the pipeline (e.g. 33, 51)\n"
            "  --count   print a line for every frame even when 0 detections\n"
            "Examples:\n"
            "  %s 33\n"
            "  %s 51 --count\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const std::string device = argv[1];
    bool show_count = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--count") show_count = true;
    }

    const std::string sock_path = ipc_data_path(device);

    // ── Connect (retry until the pipeline is up) ───────────────────────────
    int fd = -1;
    while (fd < 0) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;

        // Not yet available — wait and retry.
        ::close(fd);
        fd = -1;
        fprintf(stderr, "[data_receiver] waiting for %s ...\n", sock_path.c_str());
        sleep(1);
    }

    printf("[data_receiver] connected to %s\n", sock_path.c_str());
    receiveLoop(fd, show_count);
    ::close(fd);
    return 0;
}
