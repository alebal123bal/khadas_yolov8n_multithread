#pragma once

// unix_control_server.h — Unix-domain-socket implementation of IControlServer.
//
// ── Transport ──────────────────────────────────────────────────────────────
//   One AF_UNIX SOCK_STREAM socket at kControlSocketPath.
//   Accepts one client at a time; handles commands sequentially on a single
//   thread. This is deliberate: control messages are rare (human-frequency),
//   so a simple sequential model is easier to reason about than per-connection
//   threads.
//
// ── Protocol ───────────────────────────────────────────────────────────────
//   Every message (request and response) is framed as:
//     [uint32_t LE length][UTF-8 JSON body]
//
//   The connection stays open until the client closes it, allowing a client
//   to send multiple commands in one session.
//
// ── Thread model ───────────────────────────────────────────────────────────
//   start()  spawns listenerLoop() on its own std::thread.
//   stop()   sets running_=false, closes listen_fd_ (unblocks accept()),
//            joins the thread.
//   Command dispatch (dispatchCommand) only writes std::atomic fields and
//   therefore never blocks the inference loop.

#include "ipc/i_control_server.h"
#include "ipc/yolo_control_state.h"

#include <atomic>
#include <string>
#include <thread>

// Forward declaration: avoid pulling IDataPublisher into every TU that
// includes this header; we only need the pointer for queueDepth().
class IDataPublisher;

class UnixControlServer : public IControlServer {
public:
    /// @param socket_path  Full path for the Unix domain socket, e.g.
    ///                     ipc_control_path(device_number).  Each running
    ///                     pipeline instance must use a unique path.
    /// @param state        Shared pipeline state; written by command handlers.
    /// @param dataPub      Optional; used only to report queue_depth in get_status.
    explicit UnixControlServer(std::string       socket_path,
                               YoloControlState& state,
                               IDataPublisher*   dataPub = nullptr);
    ~UnixControlServer() override;

    void start() override;
    void stop()  override;

private:
    void        listenerLoop();
    void        handleConnection(int conn_fd);
    std::string dispatchCommand(const std::string& json);

    std::string       socket_path_;
    YoloControlState& state_;
    IDataPublisher*   dataPub_;
    int               listen_fd_{-1};
    std::thread       thread_;
    std::atomic<bool> running_{false};
};
