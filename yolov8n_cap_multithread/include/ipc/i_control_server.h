#pragma once

// i_control_server.h — Pure interface for the control-plane server.
//
// Decouples main.cc from any specific transport. A future implementation
// could swap Unix sockets for a TCP server, a shared-memory ring, or a
// simple stdin reader without touching main.cc.

class IControlServer {
public:
    virtual ~IControlServer() = default;

    /// Bind the socket, start the background listener thread, and return.
    /// May be called at most once.
    virtual void start() = 0;

    /// Signal the listener to stop; close the socket; join the thread.
    /// Safe to call even if start() was never called or already stopped.
    virtual void stop() = 0;
};
