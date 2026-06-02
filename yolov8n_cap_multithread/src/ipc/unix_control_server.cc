// unix_control_server.cc — Implementation of UnixControlServer.
//
// JSON parsing strategy:
//   The control command set is small and fixed; we do not need a full JSON
//   library. A minimal approach (find "key":"value" / "key":bool patterns
//   with std::string::find) is used for both parsing requests and building
//   responses. This keeps the implementation dependency-free and trivially
//   portable to any target.

#include "ipc/unix_control_server.h"
#include "ipc/i_data_publisher.h"
#include "ipc/wire_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// ─── Minimal JSON helpers ──────────────────────────────────────────────────
namespace {

/// Extract a string value: `"key":"value"` → "value".
static std::string extractString(const std::string& json, const char* key) {
    std::string pattern = std::string("\"") + key + "\":\"";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) return {};
    pos += pattern.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

/// Extract a boolean value: `"key":true` or `"key":false` → bool.
static bool extractBool(const std::string& json, const char* key, bool& out) {
    std::string kt = std::string("\"") + key + "\":true";
    std::string kf = std::string("\"") + key + "\":false";
    if (json.find(kt) != std::string::npos) { out = true;  return true; }
    if (json.find(kf) != std::string::npos) { out = false; return true; }
    return false;
}

static std::string okResponse() {
    return R"({"ok":true})";
}

static std::string errResponse(const char* msg) {
    std::string r = R"({"ok":false,"error":")";
    r += msg;
    r += "\"}";
    return r;
}

// ─── Length-prefixed framing helpers ──────────────────────────────────────

/// Read exactly `n` bytes into `buf`. Returns false on error or EOF.
static bool readExact(int fd, void* buf, uint32_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<uint32_t>(r);
    }
    return true;
}

/// Write exactly `n` bytes from `buf`. Uses MSG_NOSIGNAL to avoid SIGPIPE
/// when the client has already disconnected.
static bool writeExact(int fd, const void* buf, uint32_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<uint32_t>(w);
    }
    return true;
}

/// Read one length-prefixed message. Returns empty string on error.
static std::string recvMsg(int fd) {
    uint32_t len = 0;
    if (!readExact(fd, &len, sizeof(len))) return {};
    // Sanity cap: 1 MB — no control message should ever be this large.
    if (len == 0 || len > (1u << 20)) return {};
    std::string body(len, '\0');
    if (!readExact(fd, &body[0], len)) return {};
    return body;
}

/// Send one length-prefixed message. Returns false on error.
static bool sendMsg(int fd, const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    if (!writeExact(fd, &len, sizeof(len))) return false;
    return writeExact(fd, msg.data(), len);
}

} // anonymous namespace

// ─── UnixControlServer ────────────────────────────────────────────────────

UnixControlServer::UnixControlServer(std::string       socket_path,
                                     YoloControlState& state,
                                     IDataPublisher*   dataPub)
    : socket_path_(std::move(socket_path)), state_(state), dataPub_(dataPub) {}

UnixControlServer::~UnixControlServer() {
    stop();
}

void UnixControlServer::start() {
    // Remove a stale socket file left by a previous (crashed) run.
    ::unlink(socket_path_.c_str());

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("[ctrl] socket");
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[ctrl] bind");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    // Backlog of 1: only one control client is expected at a time.
    if (::listen(listen_fd_, 1) < 0) {
        perror("[ctrl] listen");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    running_ = true;
    thread_  = std::thread(&UnixControlServer::listenerLoop, this);
    printf("[ctrl] listening on %s\n", socket_path_.c_str());
}

void UnixControlServer::stop() {
    running_ = false;
    // Closing listen_fd_ causes accept() to return with an error, unblocking
    // the listener thread.
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    ::unlink(socket_path_.c_str());
}

void UnixControlServer::listenerLoop() {
    while (running_) {
        sockaddr_un peer{};
        socklen_t   peer_len = sizeof(peer);
        int conn = ::accept(listen_fd_,
                            reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (conn < 0) {
            if (running_) perror("[ctrl] accept");
            break;
        }
        printf("[ctrl] client connected\n");
        handleConnection(conn);
        ::close(conn);
        printf("[ctrl] client disconnected\n");
    }
}

void UnixControlServer::handleConnection(int conn_fd) {
    // Loop: read command → dispatch → send response → repeat.
    // A zero-length recvMsg() indicates EOF (client closed connection).
    while (running_) {
        std::string req = recvMsg(conn_fd);
        if (req.empty()) break;

        std::string resp = dispatchCommand(req);
        if (!sendMsg(conn_fd, resp)) break;
    }
}

std::string UnixControlServer::dispatchCommand(const std::string& json) {
    std::string cmd = extractString(json, "cmd");

    if (cmd == "pause_yolo") {
        state_.inference_enabled.store(false, std::memory_order_relaxed);
        printf("[ctrl] inference paused\n");
        return okResponse();
    }

    if (cmd == "resume_yolo") {
        state_.inference_enabled.store(true, std::memory_order_relaxed);
        printf("[ctrl] inference resumed\n");
        return okResponse();
    }

    if (cmd == "get_status") {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"payload\":{"
            "\"inference_enabled\":%s,"
            "\"dump_enabled\":%s,"
            "\"frame_id\":%llu,"
            "\"queue_depth\":%zu,"
            "\"fps\":%.1f"
            "}}",
            state_.inference_enabled.load(std::memory_order_relaxed) ? "true" : "false",
            state_.dump_enabled.load(std::memory_order_relaxed)      ? "true" : "false",
            static_cast<unsigned long long>(state_.frame_id.load(std::memory_order_relaxed)),
            dataPub_ ? dataPub_->queueDepth() : static_cast<std::size_t>(0),
            static_cast<double>(state_.fps.load(std::memory_order_relaxed)));
        return std::string(buf);
    }

    if (cmd == "set_dump_enabled") {
        bool enabled = false;
        // The "enabled" key may appear anywhere in the JSON body, including
        // inside a "params" sub-object — std::string::find handles both.
        if (!extractBool(json, "enabled", enabled))
            return errResponse("missing 'enabled' param (expected true/false)");
        state_.dump_enabled.store(enabled, std::memory_order_relaxed);
        printf("[ctrl] dump_enabled = %s\n", enabled ? "true" : "false");
        return okResponse();
    }

    if (cmd == "shutdown") {
        state_.shutdown_requested.store(true, std::memory_order_relaxed);
        printf("[ctrl] shutdown requested\n");
        return okResponse();
    }

    if (cmd == "blackout") {
        // Stop inference immediately, then signal the main loop to destroy
        // all RKNN contexts and close the /dev/rknpu fds.  Blocks until the
        // main loop confirms — the LLM can start as soon as this returns.
        state_.inference_enabled.store(false, std::memory_order_relaxed);
        state_.blackout_requested.store(true, std::memory_order_relaxed);
        printf("[ctrl] blackout requested\n");
        std::unique_lock<std::mutex> lk(state_.lifecycle_mutex);
        if (!state_.lifecycle_cv.wait_for(lk, std::chrono::seconds(10),
                [this]{ return state_.blackout_active.load(std::memory_order_relaxed); }))
            return errResponse("blackout timeout");
        printf("[ctrl] blackout active\n");
        return okResponse();
    }

    if (cmd == "resume") {
        if (!state_.blackout_active.load(std::memory_order_relaxed))
            return errResponse("not in blackout");
        state_.resume_requested.store(true, std::memory_order_relaxed);
        state_.lifecycle_cv.notify_all();
        printf("[ctrl] resume requested\n");
        std::unique_lock<std::mutex> lk(state_.lifecycle_mutex);
        if (!state_.lifecycle_cv.wait_for(lk, std::chrono::seconds(15),
                [this]{ return state_.resume_done.load(std::memory_order_relaxed); }))
            return errResponse("resume timeout");
        printf("[ctrl] resumed\n");
        return okResponse();
    }

    return errResponse("unknown command");
}
