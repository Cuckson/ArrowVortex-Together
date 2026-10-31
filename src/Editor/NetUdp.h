#pragma once

#include <Editor/NetSocket.h>

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace Vortex {

/// Finds the public address of a UDP socket by asking public STUN servers
/// (RFC 5389 binding requests). The requests are sent from the socket that
/// will be used for hole punching, so the router keeps the same mapping.
class StunClient {
   public:
    typedef std::chrono::steady_clock Clock;

    /// Resolves the STUN servers. Blocks for the DNS lookups.
    void start();

    /// Sends (or repeats) requests to the servers that have not answered.
    void update(NetSocket& socket);

    /// Handles a received datagram. Returns true if it was a STUN message,
    /// which should not be handled further.
    bool onPacket(const uint8_t* data, int size, const NetEndpoint& from);

    /// True once enough servers answered, or the time ran out.
    bool isDone() const;

    /// The public address of the socket, if any server answered.
    const NetEndpoint& getResult() const { return myResult; }

    /// True if two servers saw different public ports. Hole punching usually
    /// fails behind such a ("symmetric") NAT.
    bool isSymmetric() const { return mySymmetric; }

   private:
    struct Server {
        NetEndpoint address;
        bool answered = false;
    };
    std::vector<Server> myServers;
    uint8_t myTransaction[12] = {};
    Clock::time_point myStart, myLastSend;
    int myAnswers = 0;
    bool myStarted = false;
    bool mySymmetric = false;
    NetEndpoint myResult;
};

/// Reliable, ordered byte stream over UDP, used when the host can only be
/// reached through hole punching. Data is split into numbered segments that
/// are acknowledged by the other side and resent when lost, with a simple
/// congestion window.
///
/// Datagrams: [u8 UDP_MAGIC][u8 kind][u32 connection][body], little-endian.
class UdpStream {
   public:
    typedef std::chrono::steady_clock Clock;
    typedef std::vector<uint8_t> Bytes;

    enum Kind : uint8_t {
        KIND_PUNCH = 1,   ///< body: u32 session token.
        KIND_ACCEPT = 2,  ///< body: none.
        KIND_DATA = 3,    ///< body: u32 seq, payload.
        KIND_ACK = 4,     ///< body: u32 next expected seq, u8 n, n x [u32
                          ///< first, u16 count] segments received after it.
        KIND_CLOSE = 5,   ///< body: none.
        /// body: u32 session token. Guests broadcast it to find a host on
        /// the LAN; the host answers with connection 0.
        KIND_DISCOVER = 6,
    };

    static const uint8_t UDP_MAGIC = 0xA7;
    static const int HEADER_SIZE = 6;

    /// Returns true if the datagram looks like one of ours, and extracts the
    /// header.
    static bool parseHeader(const uint8_t* data, int size, Kind& kind,
                            uint32_t& connection);

    /// Sends a datagram without body (or with a u32 token body, for punches
    /// and discovery).
    static void sendControl(NetSocket& socket, const NetEndpoint& to, Kind kind,
                            uint32_t connection);
    static void sendPunch(NetSocket& socket, const NetEndpoint& to,
                          uint32_t connection, uint32_t token,
                          Kind kind = KIND_PUNCH);

    UdpStream(uint32_t connection, const NetEndpoint& peer);

    uint32_t getConnection() const { return myConnection; }
    const NetEndpoint& getPeer() const { return myPeer; }

    /// Queues bytes to send.
    void write(const uint8_t* data, size_t size);

    /// True while queued bytes are not sent or not acknowledged yet.
    bool hasPendingWrites() const;

    /// Handles a DATA or ACK datagram body. Bytes that arrived in order are
    /// appended to received.
    void onPacket(Kind kind, const uint8_t* body, int size, Bytes& received);

    /// Marks the connection as alive (any datagram from the peer).
    void onAlive() { myLastReceive = Clock::now(); }

    /// Sends acknowledgements, new segments, resends and keep-alives.
    void update(NetSocket& socket);

    /// True if nothing was received for a long time.
    bool isDead() const;

   private:
    struct Segment {
        uint32_t seq = 0;
        Bytes data;
        Clock::time_point sentAt;
        int sends = 0;
        bool acked = false;
    };

    void mySendSegment(NetSocket& socket, Segment& seg, Clock::time_point now);
    void mySendAck(NetSocket& socket, Clock::time_point now);
    void myOnLoss(Clock::time_point now);
    double myRto() const;

    uint32_t myConnection;
    NetEndpoint myPeer;

    // Sending.
    Bytes myQueue;
    size_t myQueuePos = 0;
    std::deque<Segment> mySegments;  // Unacknowledged, in seq order.
    uint32_t myNextSeq = 0;
    double myWindow = 16.0;
    double myThreshold = 512.0;
    double mySrtt = 0.0, myRttVar = 0.0;  // Seconds.
    bool myHasRtt = false;
    Clock::time_point myLastLoss, myLastSend;

    // Receiving.
    uint32_t myRecvNext = 0;
    std::map<uint32_t, Bytes> myOutOfOrder;
    bool myAckDue = false;
    Clock::time_point myLastReceive;
};

};  // namespace Vortex
