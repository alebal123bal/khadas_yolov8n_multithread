// tools/control_client.cc — Command-line control client for the YOLO pipeline.
//
// Usage:
//   control_client <device> <command> [args...]
//
// <device> is the V4L2 device number used to launch the pipeline (e.g. 33, 51).
// It is used to derive the correct socket path so that, with two cameras
// running simultaneously, you target the right instance:
//
//   control_client 33 pause_yolo
//   control_client 33 resume_yolo
//   control_client 51 get_status
//   control_client 33 set_dump_enabled true
//   control_client 33 blackout
//   control_client 33 resume
//   control_client 51 shutdown
//
// The client connects, sends one JSON command (length-prefixed), reads the
// response, prints it, and exits. No external dependencies.
//
// Cross-compiled for aarch64 together with the main pipeline via CMakeLists.txt.
// The binary is installed into install/yolov8n_cap_multithread/ by 'make install'
// and scp'd to the board by build_and_load.sh.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Copy the socket path helper inline (no SDK deps).
static std::string ipc_control_path(const std::string& dev) {
    return "/tmp/yolo_control_" + dev + ".sock";
}

// ─── Framing helpers ───────────────────────────────────────────────────────

static bool writeExact(int fd, const void* buf, uint32_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) { perror("send"); return false; }
        p += w; n -= static_cast<uint32_t>(w);
    }
    return true;
}

static bool readExact(int fd, void* buf, uint32_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) { perror("read"); return false; }
        p += r; n -= static_cast<uint32_t>(r);
    }
    return true;
}

static bool sendMsg(int fd, const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    return writeExact(fd, &len, sizeof(len)) &&
           writeExact(fd, msg.data(), len);
}

static std::string recvMsg(int fd) {
    uint32_t len = 0;
    if (!readExact(fd, &len, sizeof(len))) return {};
    if (len == 0 || len > (1u << 20)) { fprintf(stderr, "bad length %u\n", len); return {}; }
    std::string body(len, '\0');
    if (!readExact(fd, &body[0], len)) return {};
    return body;
}

// ─── Build command JSON from argv ──────────────────────────────────────────

static std::string buildCommand(int argc, char** argv) {
    // argv[1] is device number, argv[2] is the command name, argv[3...] are params.
    std::string cmd = argv[2];

    if (cmd == "set_dump_enabled") {
        if (argc < 4) { fprintf(stderr, "usage: control_client <device> set_dump_enabled true|false\n"); return {}; }
        bool val = (std::string(argv[3]) == "true" || std::string(argv[3]) == "1");
        return std::string("{\"cmd\":\"set_dump_enabled\",\"params\":{\"enabled\":") +
               (val ? "true" : "false") + "}}";
    }

    // All other commands have no parameters.
    return std::string("{\"cmd\":\"") + cmd + "\"}";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <device> <command> [args...]\n"
            "  <device>  V4L2 device number used to launch the pipeline (e.g. 33, 51)\n"
            "Commands:\n"
            "  pause_yolo\n"
            "  resume_yolo\n"
            "  get_status\n"
            "  set_dump_enabled true|false\n"
            "  blackout\n"
            "  resume\n"
            "  shutdown\n"
            "Examples:\n"
            "  %s 33 get_status\n"
            "  %s 51 pause_yolo\n"
            "  %s 33 blackout\n"
            "  %s 33 resume\n",
            argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    const std::string device = argv[1];

    // ── Connect ────────────────────────────────────────────────────────────
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    const std::string sock_path = ipc_control_path(device);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        fprintf(stderr, "Is the pipeline for device %s running? (socket: %s)\n",
                device.c_str(), sock_path.c_str());
        ::close(fd);
        return 1;
    }

    // ── Send command ───────────────────────────────────────────────────────
    std::string cmd_json = buildCommand(argc, argv);
    if (cmd_json.empty()) { ::close(fd); return 1; }

    printf("→ %s\n", cmd_json.c_str());
    if (!sendMsg(fd, cmd_json)) { ::close(fd); return 1; }

    // ── Read response ──────────────────────────────────────────────────────
    std::string resp = recvMsg(fd);
    if (resp.empty()) { fprintf(stderr, "no response\n"); ::close(fd); return 1; }
    printf("← %s\n", resp.c_str());

    ::close(fd);
    return 0;
}
