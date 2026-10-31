#include <Editor/NetSocket.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>
// iphlpapi.h needs the winsock headers first.
#include <iphlpapi.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <fstream>
#include <sstream>
#endif

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Vortex {

// ================================================================================================
// Platform helpers.

#ifdef _WIN32
typedef SOCKET NativeSocket;
typedef int SockLen;
static const uintptr_t INVALID_HANDLE = static_cast<uintptr_t>(INVALID_SOCKET);
static int LastError() { return WSAGetLastError(); }
static bool IsWouldBlock(int err) {
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}
static bool IsInterrupted(int err) { return err == WSAEINTR; }
static void CloseNative(NativeSocket s) { closesocket(s); }
#else
typedef int NativeSocket;
typedef socklen_t SockLen;
static const uintptr_t INVALID_HANDLE = ~static_cast<uintptr_t>(0);
static int LastError() { return errno; }
// A signal interrupting a call (EINTR) is treated like a call that would
// block; the socket is simply polled again.
static bool IsWouldBlock(int err) {
    return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS ||
           err == EINTR;
}
static bool IsInterrupted(int err) { return err == EINTR; }
static void CloseNative(NativeSocket s) { ::close(s); }
#endif

static NativeSocket ToNative(uintptr_t handle) {
    return static_cast<NativeSocket>(handle);
}

static uintptr_t FromNative(NativeSocket s) {
#ifdef _WIN32
    return static_cast<uintptr_t>(s);
#else
    return s < 0 ? INVALID_HANDLE : static_cast<uintptr_t>(s);
#endif
}

static std::string ErrorText(int err) {
#ifdef _WIN32
    return "error " + std::to_string(err);
#else
    return std::string(std::strerror(err));
#endif
}

static std::string ErrorText(const char* what) {
    return std::string(what) + " (" + ErrorText(LastError()) + ")";
}

static sockaddr_in ToSockAddr(const NetEndpoint& ep) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ep.ip);
    addr.sin_port = htons(ep.port);
    return addr;
}

static NetEndpoint FromSockAddr(const sockaddr_in& addr) {
    NetEndpoint ep;
    ep.ip = ntohl(addr.sin_addr.s_addr);
    ep.port = ntohs(addr.sin_port);
    return ep;
}

// ================================================================================================
// NetEndpoint.

std::string NetEndpoint::ipToString(uint32_t ip) {
    return std::to_string((ip >> 24) & 0xFF) + "." +
           std::to_string((ip >> 16) & 0xFF) + "." +
           std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

std::string NetEndpoint::toString() const {
    return ipToString(ip) + ":" + std::to_string(port);
}

bool NetEndpoint::parseIp(const std::string& text, uint32_t& out) {
    in_addr addr = {};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
    out = ntohl(addr.s_addr);
    return true;
}

bool NetEndpoint::resolve(const std::string& host, int port, NetEndpoint& out) {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || !list) {
        return false;
    }
    out = FromSockAddr(*reinterpret_cast<const sockaddr_in*>(list->ai_addr));
    out.port = static_cast<uint16_t>(port);
    freeaddrinfo(list);
    return true;
}

bool NetEndpoint::isPrivateIp(uint32_t ip) {
    uint32_t a = ip >> 24, b = (ip >> 16) & 0xFF;
    return a == 10 || a == 127 || a == 0 || (a == 172 && b >= 16 && b < 32) ||
           (a == 192 && b == 168) || (a == 100 && b >= 64 && b < 128) ||
           (a == 169 && b == 254);
}

// ================================================================================================
// Library setup.

bool NetSocket::startup() {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void NetSocket::shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

static void AddAddress(std::vector<std::string>& out, const sockaddr* addr) {
    if (!addr || addr->sa_family != AF_INET) return;
    char buf[INET_ADDRSTRLEN] = {};
    const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
    if (!inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf))) return;
    std::string ip = buf;
    if (ip.rfind("127.", 0) == 0) return;
    if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
}

std::vector<std::string> NetSocket::localAddresses() {
    std::vector<std::string> out;
#ifndef _WIN32
    // The host name usually resolves to a loopback address on Linux (e.g.
    // 127.0.1.1 in /etc/hosts), so ask the network interfaces directly.
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* it = ifs; it; it = it->ifa_next) {
            if ((it->ifa_flags & IFF_UP) && !(it->ifa_flags & IFF_LOOPBACK)) {
                AddAddress(out, it->ifa_addr);
            }
        }
        freeifaddrs(ifs);
    }
    if (!out.empty()) return out;
#endif
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) return out;

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &list) != 0) return out;
    for (addrinfo* it = list; it; it = it->ai_next) {
        AddAddress(out, it->ai_addr);
    }
    freeaddrinfo(list);
    return out;
}

std::vector<uint32_t> NetSocket::defaultGateways() {
    std::vector<uint32_t> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG res = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 3 && res == ERROR_BUFFER_OVERFLOW; ++i) {
        buffer.resize(size);
        res = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &size);
    }
    if (res != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a;
         a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            const sockaddr* addr = g->Address.lpSockaddr;
            if (!addr || addr->sa_family != AF_INET) continue;
            NetEndpoint ep =
                FromSockAddr(*reinterpret_cast<const sockaddr_in*>(addr));
            if (ep.ip &&
                std::find(out.begin(), out.end(), ep.ip) == out.end()) {
                out.push_back(ep.ip);
            }
        }
    }
#else
    // Default routes have destination 0; addresses are in network order.
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);  // Header.
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string iface, dest, gateway;
        if (!(fields >> iface >> dest >> gateway) || dest != "00000000") {
            continue;
        }
        uint32_t ip =
            ntohl(static_cast<uint32_t>(std::stoul(gateway, nullptr, 16)));
        if (ip && std::find(out.begin(), out.end(), ip) == out.end()) {
            out.push_back(ip);
        }
    }
#endif
    return out;
}

bool NetSocket::pollAll(std::vector<PollItem>& items, int timeoutMs) {
    fd_set readSet, writeSet, errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    NativeSocket maxSocket = 0;
    bool any = false;
    for (auto& item : items) {
        item.result = 0;
        if (!item.socket || !item.socket->isValid()) {
            item.result = POLL_ERROR;
            continue;
        }
        NativeSocket s = ToNative(item.socket->myHandle);
        FD_SET(s, &readSet);
        FD_SET(s, &errorSet);
        if (item.wantWrite) FD_SET(s, &writeSet);
        maxSocket = std::max(maxSocket, s);
        any = true;
    }

    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef _WIN32
    // Winsock rejects a select without sockets.
    if (!any) {
        Sleep(static_cast<DWORD>(timeoutMs));
        return true;
    }
    int res = select(0, &readSet, &writeSet, &errorSet, &tv);
#else
    int res =
        select(any ? maxSocket + 1 : 0, &readSet, &writeSet, &errorSet, &tv);
#endif
    if (res < 0) return IsInterrupted(LastError());

    for (auto& item : items) {
        if (item.result != 0) continue;
        NativeSocket s = ToNative(item.socket->myHandle);
        if (FD_ISSET(s, &readSet)) item.result |= POLL_READ;
        if (FD_ISSET(s, &writeSet)) item.result |= POLL_WRITE;
        if (FD_ISSET(s, &errorSet)) item.result |= POLL_ERROR;
    }
    return true;
}

// ================================================================================================
// NetSocket.

NetSocket::~NetSocket() { close(); }

NetSocket::NetSocket() : myHandle(INVALID_HANDLE) {}

NetSocket::NetSocket(uintptr_t handle) : myHandle(handle) {}

NetSocket::NetSocket(NetSocket&& other) noexcept : myHandle(other.myHandle) {
    other.myHandle = INVALID_HANDLE;
}

NetSocket& NetSocket::operator=(NetSocket&& other) noexcept {
    if (this != &other) {
        close();
        myHandle = other.myHandle;
        other.myHandle = INVALID_HANDLE;
    }
    return *this;
}

bool NetSocket::isValid() const { return myHandle != INVALID_HANDLE; }

void NetSocket::close() {
    if (isValid()) {
        CloseNative(ToNative(myHandle));
        myHandle = INVALID_HANDLE;
    }
}

void NetSocket::mySetNonBlocking(bool tcp) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(ToNative(myHandle), FIONBIO, &mode);
#else
    int flags = fcntl(ToNative(myHandle), F_GETFL, 0);
    fcntl(ToNative(myHandle), F_SETFL, flags | O_NONBLOCK);
#endif
    if (!tcp) return;
    // Note edits are tiny; send them immediately.
    int noDelay = 1;
    setsockopt(ToNative(myHandle), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
}

bool NetSocket::listen(int port, std::string& error) {
    close();
    myHandle = FromNative(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!isValid()) {
        error = ErrorText("Could not create socket");
        return false;
    }

    int reuse = 1;
#ifdef _WIN32
    // On Windows SO_REUSEADDR lets another program take over the port; make
    // sure a second session on the same port fails instead.
    setsockopt(ToNative(myHandle), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(ToNative(myHandle), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(ToNative(myHandle), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        error =
            ErrorText(("Could not bind port " + std::to_string(port)).c_str());
        close();
        return false;
    }
    if (::listen(ToNative(myHandle), 8) != 0) {
        error = ErrorText("Could not listen");
        close();
        return false;
    }
    mySetNonBlocking(true);
    return true;
}

bool NetSocket::openUdp(int port, std::string& error) {
    close();
    myHandle = FromNative(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!isValid()) {
        error = ErrorText("Could not create socket");
        return false;
    }
#ifdef _WIN32
    // Without this, an ICMP "port unreachable" makes the next recvfrom fail
    // with WSAECONNRESET, which happens all the time while hole punching.
    BOOL reportReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(ToNative(myHandle), SIO_UDP_CONNRESET, &reportReset,
             sizeof(reportReset), nullptr, 0, &bytes, nullptr, nullptr);
#endif
    // Song downloads arrive in bursts; give them some room.
    int bufferSize = 1 << 20;
    setsockopt(ToNative(myHandle), SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));
    setsockopt(ToNative(myHandle), SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));

    sockaddr_in addr = ToSockAddr({0, static_cast<uint16_t>(port)});
    if (bind(ToNative(myHandle), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        error = ErrorText(
            ("Could not bind UDP port " + std::to_string(port)).c_str());
        close();
        return false;
    }
    mySetNonBlocking(false);
    return true;
}

int NetSocket::sendTo(const void* data, int size, const NetEndpoint& to) {
    sockaddr_in addr = ToSockAddr(to);
#ifdef _WIN32
    int res = ::sendto(ToNative(myHandle), static_cast<const char*>(data), size,
                       0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#else
    int res = static_cast<int>(
        ::sendto(ToNative(myHandle), data, size, MSG_NOSIGNAL,
                 reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
#endif
    if (res < 0) return IsWouldBlock(LastError()) ? 0 : -1;
    return res;
}

int NetSocket::recvFrom(void* data, int size, NetEndpoint& from) {
    sockaddr_in addr = {};
    SockLen len = sizeof(addr);
#ifdef _WIN32
    int res = ::recvfrom(ToNative(myHandle), static_cast<char*>(data), size, 0,
                         reinterpret_cast<sockaddr*>(&addr), &len);
#else
    int res =
        static_cast<int>(::recvfrom(ToNative(myHandle), data, size, 0,
                                    reinterpret_cast<sockaddr*>(&addr), &len));
#endif
    if (res < 0) {
        int err = LastError();
#ifdef _WIN32
        // Oversized datagrams and stray ICMP errors are not fatal.
        if (err == WSAEMSGSIZE || err == WSAECONNRESET) return 0;
#else
        if (err == ECONNREFUSED) return 0;
#endif
        return IsWouldBlock(err) ? 0 : -1;
    }
    from = FromSockAddr(addr);
    return res;
}

NetEndpoint NetSocket::localEndpoint() const {
    sockaddr_in addr = {};
    SockLen len = sizeof(addr);
    if (getsockname(ToNative(myHandle), reinterpret_cast<sockaddr*>(&addr),
                    &len) != 0) {
        return NetEndpoint();
    }
    return FromSockAddr(addr);
}

bool NetSocket::beginConnect(const NetEndpoint& to, std::string& error) {
    close();
    myHandle = FromNative(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!isValid()) {
        error = ErrorText("Could not create socket");
        return false;
    }
    mySetNonBlocking(true);
    sockaddr_in addr = ToSockAddr(to);
    int res = ::connect(ToNative(myHandle), reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr));
    if (res != 0 && !IsWouldBlock(LastError())) {
        error = ErrorText("Could not connect");
        close();
        return false;
    }
    return true;
}

bool NetSocket::beginConnect(const std::string& host, int port,
                             std::string& error) {
    close();

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 ||
        !list) {
        error = "Could not resolve address \"" + host + "\"";
        return false;
    }

    myHandle = FromNative(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!isValid()) {
        freeaddrinfo(list);
        error = ErrorText("Could not create socket");
        return false;
    }
    mySetNonBlocking(true);

    int res = ::connect(ToNative(myHandle), list->ai_addr,
                        static_cast<SockLen>(list->ai_addrlen));
    freeaddrinfo(list);
    if (res != 0 && !IsWouldBlock(LastError())) {
        error = ErrorText("Could not connect");
        close();
        return false;
    }
    return true;
}

bool NetSocket::finishConnect(std::string& error) {
    int err = 0;
    SockLen len = sizeof(err);
    getsockopt(ToNative(myHandle), SOL_SOCKET, SO_ERROR,
               reinterpret_cast<char*>(&err), &len);
    if (err != 0) {
        error = "Could not connect (" + ErrorText(err) + ")";
        return false;
    }
    return true;
}

NetSocket NetSocket::accept() {
    NativeSocket s = ::accept(ToNative(myHandle), nullptr, nullptr);
    NetSocket out(FromNative(s));
    if (out.isValid()) out.mySetNonBlocking(true);
    return out;
}

int NetSocket::poll(bool wantWrite, int timeoutMs) const {
    if (!isValid()) return POLL_ERROR;

    fd_set readSet, writeSet, errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    NativeSocket s = ToNative(myHandle);
    FD_SET(s, &readSet);
    FD_SET(s, &errorSet);
    if (wantWrite) FD_SET(s, &writeSet);

    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef _WIN32
    int res = select(0, &readSet, &writeSet, &errorSet, &tv);
#else
    int res = select(s + 1, &readSet, &writeSet, &errorSet, &tv);
#endif
    if (res < 0) return IsInterrupted(LastError()) ? 0 : POLL_ERROR;

    int flags = 0;
    if (FD_ISSET(s, &readSet)) flags |= POLL_READ;
    if (FD_ISSET(s, &writeSet)) flags |= POLL_WRITE;
    if (FD_ISSET(s, &errorSet)) flags |= POLL_ERROR;
    return flags;
}

int NetSocket::send(const void* data, int size) {
#ifdef _WIN32
    int res =
        ::send(ToNative(myHandle), static_cast<const char*>(data), size, 0);
#else
    int res =
        static_cast<int>(::send(ToNative(myHandle), data, size, MSG_NOSIGNAL));
#endif
    if (res < 0) return IsWouldBlock(LastError()) ? 0 : -1;
    return res;
}

int NetSocket::recv(void* data, int size) {
#ifdef _WIN32
    int res = ::recv(ToNative(myHandle), static_cast<char*>(data), size, 0);
#else
    int res = static_cast<int>(::recv(ToNative(myHandle), data, size, 0));
#endif
    if (res == 0) return -1;  // Orderly shutdown by the peer.
    if (res < 0) return IsWouldBlock(LastError()) ? 0 : -1;
    return res;
}

};  // namespace Vortex
