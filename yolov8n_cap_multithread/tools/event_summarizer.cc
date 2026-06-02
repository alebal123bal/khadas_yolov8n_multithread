// tools/event_summarizer.cc — Final downstream consumer of the temporal events
// stream. Replaces events_receiver as the production sink.
//
// Responsibilities:
//   1. Connect to /tmp/yolo_events_<dev>.sock (single consumer slot served by
//      temporal_service) and aggregate per-track WireTrackSummary records into
//      one rolling record per track_id for the current "presence episode".
//   2. Run a dead-simple 4-state FSM driven by UAV presence:
//        NO_DRONE → DRONE_PRESENT → COOLDOWN → LLM_WINDOW → NO_DRONE
//      A UAV is "present" when at least one summary in the current frame has
//      class_name == <drone-class> and status != LOST.
//   3. When the UAV has been gone for >= cooldown seconds, blackout the YOLO
//      pipeline(s) (control plane) so the NPU is fully released, render an
//      LLM-friendly snapshot .txt, run Qwen once via an external command, then
//      resume YOLO and reset.
//
//      blackout (not pause_yolo) is required: pausing stops inference but keeps
//      the RKNN contexts loaded, which does not free enough NPU throughput for
//      the LLM. blackout destroys the RKNN contexts and closes /dev/rknpu so
//      Qwen gets the full NPU (~35 tok/s). With two cameras sharing the NPU,
//      use --also-device to release the second camera too.
//
// Self-contained: only standard C++ + POSIX. No RKNN/RGA/GStreamer deps, so it
// cross-compiles for aarch64 alongside the other tools.
//
// Usage:
//   ./event_summarizer <device> [options]
// Options:
//   --cooldown-s N     Seconds UAV must be absent before triggering (default 30)
//   --drone-class S    Class label that counts as a UAV (default "UAV")
//   --out PATH         Snapshot text file (default /tmp/yolo_summary_<dev>.txt)
//   --qwen-cmd CMD     Shell command run once after the snapshot is written.
//                      The snapshot path is appended as the final argument.
//                      Empty string disables the Qwen step (snapshot only).
//   --also-device D    Additional camera device to blackout/resume around the
//                      LLM run (repeatable). Use this for the other camera so
//                      the LLM gets the whole NPU. e.g. --also-device 51
//   --no-control       Do not send blackout/resume (debug; just summarize)

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <map>
#include <string>
#include <vector>

// ─── Socket path helpers (inlined; no SDK deps) ─────────────────────────────
static std::string ipc_events_path(const std::string& dev) {
    return "/tmp/yolo_events_" + dev + ".sock";
}
static std::string ipc_control_path(const std::string& dev) {
    return "/tmp/yolo_control_" + dev + ".sock";
}

// ─── Wire structs (must match wire_protocol.h byte-for-byte) ────────────────
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

// ─── Framing helpers ────────────────────────────────────────────────────────
static bool readExact(int fd, void* buf, uint32_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= static_cast<uint32_t>(r);
    }
    return true;
}
static bool writeExact(int fd, const void* buf, uint32_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        p += w; n -= static_cast<uint32_t>(w);
    }
    return true;
}

// ─── Control client (length-prefixed JSON, matches control_client.cc) ───────
static bool send_control(const std::string& device, const std::string& cmd_json) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = ipc_control_path(device);
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        fprintf(stderr, "[summarizer] control connect failed (%s)\n", path.c_str());
        return false;
    }
    uint32_t len = static_cast<uint32_t>(cmd_json.size());
    bool ok = writeExact(fd, &len, sizeof(len)) &&
              writeExact(fd, cmd_json.data(), len);
    // Read and discard the response so the server completes the exchange cleanly.
    uint32_t rlen = 0;
    if (ok && readExact(fd, &rlen, sizeof(rlen)) && rlen > 0 && rlen < (1u << 20)) {
        std::string body(rlen, '\0');
        readExact(fd, &body[0], rlen);
    }
    ::close(fd);
    return ok;
}

// ─── Per-track rolling aggregate for the current presence episode ───────────
struct AggTrack {
    std::string class_name;
    uint32_t    track_id      = 0;
    uint32_t    frames_seen   = 0;
    uint64_t    first_seen_us = 0;
    uint64_t    last_seen_us  = 0;
    uint32_t    duration_ms   = 0;
    int32_t     first_cx = 0, first_cy = 0;
    int32_t     last_cx = 0,  last_cy = 0;
    float       max_speed     = 0.f;
    float       sum_heading_x = 0.f;  // for circular-mean of heading
    float       sum_heading_y = 0.f;
    float       min_loiter    = 1.f;
    float       max_loiter    = 0.f;
    bool        ever_in_roi   = false;
    bool        roi_entered   = false;
    bool        roi_exited    = false;
    bool        first_set     = false;
    uint8_t     last_status   = 0;
};

static void update_aggregate(std::map<uint32_t, AggTrack>& agg,
                             const WireTrackSummary& s) {
    AggTrack& a = agg[s.track_id];
    a.class_name   = s.class_name;
    a.track_id     = s.track_id;
    a.frames_seen  = s.frames_seen > a.frames_seen ? s.frames_seen : a.frames_seen;
    if (!a.first_set) {
        a.first_seen_us = s.first_seen_us;
        a.first_cx = s.cx; a.first_cy = s.cy;
        a.first_set = true;
    }
    a.last_seen_us = s.last_seen_us;
    a.duration_ms  = s.duration_ms > a.duration_ms ? s.duration_ms : a.duration_ms;
    a.last_cx = s.cx; a.last_cy = s.cy;
    if (s.speed > a.max_speed) a.max_speed = s.speed;
    const float rad = s.heading_deg * static_cast<float>(M_PI) / 180.f;
    a.sum_heading_x += std::cos(rad);
    a.sum_heading_y += std::sin(rad);
    if (s.loiter_score < a.min_loiter) a.min_loiter = s.loiter_score;
    if (s.loiter_score > a.max_loiter) a.max_loiter = s.loiter_score;
    if (s.in_roi)      a.ever_in_roi = true;
    if (s.roi_entered) a.roi_entered = true;
    if (s.roi_exited)  a.roi_exited  = true;
    a.last_status = s.status;
}

// ─── Snapshot renderer: deterministic, LLM-friendly plain text ──────────────
static bool write_snapshot(const std::string& path,
                           const std::string& device,
                           const std::map<uint32_t, AggTrack>& agg) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) { perror("[summarizer] fopen snapshot"); return false; }

    char ts[32];
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    gmtime_r(&now, &tmv);
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    // Instruction FIRST: small models (Qwen 0.5B) anchor far better when the
    // task precedes the data, especially once the file is flattened to a single
    // line for llm_demo's getline-based prompt.
    std::fprintf(f,
        "You are a security analyst. Using ONLY the scene data below, reply with "
        "exactly two sentences describing the UAV activity and whether it looks "
        "routine or suspicious. Do not ask questions or invent details.\n\n");

    std::fprintf(f, "SCENE SUMMARY\n");
    std::fprintf(f, "device: %s\n", device.c_str());
    std::fprintf(f, "window_end_utc: %s\n", ts);
    std::fprintf(f, "event: UAV departed; scene clear for the configured cooldown\n");
    std::fprintf(f, "tracks_observed: %zu\n\n", agg.size());

    int idx = 0;
    for (const auto& kv : agg) {
        const AggTrack& a = kv.second;
        const double dur_s = a.duration_ms / 1000.0;
        float heading = std::atan2(a.sum_heading_y, a.sum_heading_x) * 180.f
                        / static_cast<float>(M_PI);
        const char* motion = a.max_loiter > 0.6f ? "loitering"
                           : a.max_speed  < 5.f  ? "stationary" : "moving";
        std::fprintf(f, "TRACK %d\n", ++idx);
        std::fprintf(f, "  class: %s\n", a.class_name.c_str());
        std::fprintf(f, "  track_id: %u\n", a.track_id);
        std::fprintf(f, "  duration_s: %.1f\n", dur_s);
        std::fprintf(f, "  frames_seen: %u\n", a.frames_seen);
        std::fprintf(f, "  entered_at_px: (%d,%d)\n", a.first_cx, a.first_cy);
        std::fprintf(f, "  last_seen_at_px: (%d,%d)\n", a.last_cx, a.last_cy);
        std::fprintf(f, "  max_speed_px_s: %.1f\n", a.max_speed);
        std::fprintf(f, "  avg_heading_deg: %.1f\n", heading);
        std::fprintf(f, "  motion: %s (loiter %.2f)\n", motion, a.max_loiter);
        std::fprintf(f, "  roi_entered: %s\n", a.roi_entered ? "yes" : "no");
        std::fprintf(f, "  roi_exited: %s\n", a.roi_exited ? "yes" : "no");
        std::fprintf(f, "\n");
    }

    std::fprintf(f,
        "END OF DATA. Now write the two-sentence assessment.\n");
    std::fclose(f);

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        perror("[summarizer] rename snapshot");
        return false;
    }
    return true;
}

// ─── Receive one events frame ───────────────────────────────────────────────
enum class RecvResult { Frame, Timeout, Disconnect };

static RecvResult recv_frame(int fd, std::vector<WireTrackSummary>& out) {
    uint32_t body_len = 0;
    ssize_t r = ::read(fd, &body_len, sizeof(body_len));
    if (r == 0) return RecvResult::Disconnect;
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return RecvResult::Timeout;
        return RecvResult::Disconnect;
    }
    // Got the first 4 bytes; the rest must arrive (blocking until complete).
    if (r != static_cast<ssize_t>(sizeof(body_len))) {
        if (!readExact(fd, reinterpret_cast<uint8_t*>(&body_len) + r,
                       sizeof(body_len) - static_cast<uint32_t>(r)))
            return RecvResult::Disconnect;
    }
    if (body_len < sizeof(WireFrameHeader)) return RecvResult::Disconnect;

    WireFrameHeader hdr{};
    if (!readExact(fd, &hdr, sizeof(hdr))) return RecvResult::Disconnect;

    out.clear();
    out.resize(hdr.count);
    for (uint16_t i = 0; i < hdr.count; ++i) {
        if (!readExact(fd, &out[i], sizeof(WireTrackSummary)))
            return RecvResult::Disconnect;
    }
    return RecvResult::Frame;
}

// ─── FSM ────────────────────────────────────────────────────────────────────
enum class State { NoDrone, DronePresent, Cooldown, LlmWindow };

static const char* state_str(State s) {
    switch (s) {
        case State::NoDrone:      return "NO_DRONE";
        case State::DronePresent: return "DRONE_PRESENT";
        case State::Cooldown:     return "COOLDOWN";
        case State::LlmWindow:    return "LLM_WINDOW";
    }
    return "?";
}

static uint64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + tv.tv_usec / 1000ULL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <device> [--cooldown-s N] [--drone-class S]\n"
            "                 [--out PATH] [--qwen-cmd CMD]\n"
            "                 [--also-device D ...] [--no-control]\n",
            argv[0]);
        return 1;
    }
    const std::string device = argv[1];
    uint64_t    cooldown_ms  = 30 * 1000ULL;
    std::string drone_class  = "UAV";
    std::string out_path     = "/tmp/yolo_summary_" + device + ".txt";
    std::string qwen_cmd;     // empty → snapshot only
    bool        use_control  = true;
    std::vector<std::string> also_devices;  // other cameras to blackout/resume

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cooldown-s"  && i + 1 < argc) cooldown_ms = std::strtoull(argv[++i], nullptr, 10) * 1000ULL;
        else if (a == "--drone-class" && i + 1 < argc) drone_class = argv[++i];
        else if (a == "--out"         && i + 1 < argc) out_path    = argv[++i];
        else if (a == "--qwen-cmd"    && i + 1 < argc) qwen_cmd    = argv[++i];
        else if (a == "--also-device" && i + 1 < argc) also_devices.push_back(argv[++i]);
        else if (a == "--no-control") use_control = false;
        else { fprintf(stderr, "[summarizer] unknown arg: %s\n", a.c_str()); return 1; }
    }

    const std::string ev_path = ipc_events_path(device);
    fprintf(stderr, "[summarizer] device=%s cooldown=%llus class=%s out=%s\n",
            device.c_str(), (unsigned long long)(cooldown_ms / 1000),
            drone_class.c_str(), out_path.c_str());

    std::map<uint32_t, AggTrack> agg;
    State    state         = State::NoDrone;
    bool     present       = false;   // last observed presence (from a real frame)
    uint64_t departed_ms   = 0;

    // Outer reconnect loop.
    while (true) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("[summarizer] socket"); return 1; }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, ev_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            fprintf(stderr, "[summarizer] waiting for %s ...\n", ev_path.c_str());
            sleep(1);
            continue;
        }
        // 200 ms recv timeout so the FSM keeps ticking when the stream is quiet
        // (e.g. UAV has left and temporal_service emits empty frames, or YOLO
        // is paused during the LLM window).
        struct timeval rcv_to{0, 200 * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
        fprintf(stderr, "[summarizer] connected to %s\n", ev_path.c_str());

        std::vector<WireTrackSummary> frame;
        bool connected = true;
        while (connected) {
            RecvResult rr = recv_frame(fd, frame);
            if (rr == RecvResult::Disconnect) { connected = false; break; }
            if (rr == RecvResult::Frame) {
                present = false;
                for (const auto& s : frame) {
                    const bool is_drone = (drone_class == s.class_name) && s.status != 2;
                    // Aggregate every UAV record while in an active episode.
                    if (is_drone && (state == State::DronePresent ||
                                     state == State::Cooldown)) {
                        update_aggregate(agg, s);
                    }
                    if (is_drone) present = true;
                }
            }
            // rr == Timeout → keep last `present`; just tick timers below.

            const uint64_t t = now_ms();
            const State prev = state;
            switch (state) {
                case State::NoDrone:
                    if (present) { agg.clear(); state = State::DronePresent; }
                    break;
                case State::DronePresent:
                    if (!present) { departed_ms = t; state = State::Cooldown; }
                    break;
                case State::Cooldown:
                    if (present)                       state = State::DronePresent;
                    else if (t - departed_ms >= cooldown_ms) state = State::LlmWindow;
                    break;
                case State::LlmWindow:
                    break;  // handled below, outside the switch
            }
            if (state != prev)
                fprintf(stderr, "[summarizer] %s → %s\n", state_str(prev), state_str(state));

            if (state == State::LlmWindow) {
                fprintf(stderr, "[summarizer] UAV gone %llus; running LLM cycle (%zu tracks)\n",
                        (unsigned long long)(cooldown_ms / 1000), agg.size());

                // Blackout every camera so the LLM gets the whole NPU.
                // pause_yolo is NOT enough: it keeps the RKNN contexts loaded
                // and the LLM cannot reach full throughput. blackout destroys
                // the RKNN contexts and closes /dev/rknpu.
                if (use_control) {
                    send_control(device, "{\"cmd\":\"blackout\"}");
                    for (const auto& d : also_devices)
                        send_control(d, "{\"cmd\":\"blackout\"}");
                }

                if (write_snapshot(out_path, device, agg)) {
                    fprintf(stderr, "[summarizer] snapshot written: %s\n", out_path.c_str());
                    if (!qwen_cmd.empty()) {
                        std::string full = qwen_cmd + " " + out_path;
                        fprintf(stderr, "[summarizer] running: %s\n", full.c_str());
                        int rc = std::system(full.c_str());
                        fprintf(stderr, "[summarizer] qwen exit code: %d\n", rc);
                    }
                }

                // Hand the NPU back to the cameras.
                if (use_control) {
                    send_control(device, "{\"cmd\":\"resume\"}");
                    for (const auto& d : also_devices)
                        send_control(d, "{\"cmd\":\"resume\"}");
                }

                agg.clear();
                present = false;
                state = State::NoDrone;
                fprintf(stderr, "[summarizer] LLM_WINDOW → NO_DRONE\n");
            }
        }

        ::close(fd);
        fprintf(stderr, "[summarizer] events stream disconnected; reconnecting\n");
        // Preserve FSM/aggregate across a temporary temporal_service restart.
        sleep(1);
    }
    return 0;
}
