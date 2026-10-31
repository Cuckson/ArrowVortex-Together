#pragma once

#include <Core/Core.h>

#include <string>
#include <vector>

namespace Vortex {

/// An IPv4 address and port, both in host byte order.
struct NetEndpoint {
    uint32_t ip = 0;
    uint16_t port = 0;

    bool operator==(const NetEndpoint& other) const = default;

    bool isValid() const { return ip != 0 && port != 0; }

    /// Returns "a.b.c.d:port".
    std::string toString() const;

    /// Returns "a.b.c.d".
    static std::string ipToString(uint32_t ip);

    /// Parses a dotted IPv4 address.
    static bool parseIp(const std::string& text, uint32_t& out);

    /// Resolves a host name (or dotted address) to an IPv4 endpoint. Blocks.
    static bool resolve(const std::string& host, int port, NetEndpoint& out);

    /// Returns true for loopback, private (10/8, 172.16/12, 192.168/16),
    /// carrier-grade NAT (100.64/10) and link-local addresses.
    static bool isPrivateIp(uint32_t ip);
};

/// Minimal non-blocking IPv4 TCP/UDP socket (Winsock on Windows, BSD sockets
/// elsewhere). Not thread-safe; owned by a single thread at a time.
class NetSocket {
   public:
    enum PollFlags {
        POLL_READ = 1 << 0,
        POLL_WRITE = 1 << 1,
        POLL_ERROR = 1 << 2,
    };

    /// Initializes the socket library. Must be called before any other use.
    static bool startup();

    /// Releases the socket library.
    static void shutdown();

    /// Returns the IPv4 addresses of this machine (excluding loopback).
    static std::vector<std::string> localAddresses();

    /// Returns the IPv4 addresses of the default gateways (routers).
    static std::vector<uint32_t> defaultGateways();

    /// A socket to wait on with pollAll().
    struct PollItem {
        const NetSocket* socket = nullptr;
        bool wantWrite = false;
        int result = 0;  ///< Combination of PollFlags, set by pollAll().
    };

    /// Waits up to timeoutMs for any of the sockets to become readable (or
    /// writable, if requested). Returns false if waiting failed.
    static bool pollAll(std::vector<PollItem>& items, int timeoutMs);

    ~NetSocket();
    NetSocket();
    NetSocket(NetSocket&& other) noexcept;
    NetSocket& operator=(NetSocket&& other) noexcept;
    NetSocket(const NetSocket&) = delete;
    NetSocket& operator=(const NetSocket&) = delete;

    /// Opens a listening socket on all interfaces at the given port.
    bool listen(int port, std::string& error);

    /// Opens a UDP socket bound to all interfaces at the given port, or at
    /// any free port if port is 0.
    bool openUdp(int port, std::string& error);

    /// UDP: sends one datagram. Returns the number of bytes sent, 0 if the
    /// call would block, or -1 on error.
    int sendTo(const void* data, int size, const NetEndpoint& to);

    /// UDP: receives one datagram. Returns its size, 0 if there is none, or
    /// -1 on error.
    int recvFrom(void* data, int size, NetEndpoint& from);

    /// Returns the local address and port the socket is bound to.
    NetEndpoint localEndpoint() const;

    /// Starts a non-blocking connect. Use poll(POLL_WRITE) and
    /// finishConnect() to complete it.
    bool beginConnect(const std::string& host, int port, std::string& error);
    bool beginConnect(const NetEndpoint& to, std::string& error);

    /// Returns true if a pending connect succeeded, false if it failed.
    bool finishConnect(std::string& error);

    /// Accepts a pending connection. Returns an invalid socket if none.
    NetSocket accept();

    /// Waits up to timeoutMs for the socket to become readable (and writable,
    /// if requested). Returns a combination of PollFlags.
    int poll(bool wantWrite, int timeoutMs) const;

    /// Returns the number of bytes sent, 0 if the call would block, or -1 if
    /// the connection is broken.
    int send(const void* data, int size);

    /// Returns the number of bytes received, 0 if the call would block, or -1
    /// if the connection is closed or broken.
    int recv(void* data, int size);

    bool isValid() const;
    void close();

   private:
    explicit NetSocket(uintptr_t handle);
    void mySetNonBlocking(bool tcp);

    uintptr_t myHandle;
};

};  // namespace Vortex
