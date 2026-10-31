#include <Editor/NetUdp.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>

namespace Vortex {
namespace {

typedef std::chrono::steady_clock Clock;

// ================================================================================================
// STUN.

const char* const STUN_SERVERS[][2] = {
    {"stun.l.google.com", "19302"},
    {"stun1.l.google.com", "19302"},
    {"stun.cloudflare.com", "3478"},
};

const uint32_t STUN_COOKIE = 0x2112A442;
const int STUN_RETRY_MS = 400;
const int STUN_TIMEOUT_MS = 4000;

// ================================================================================================
// UdpStream.

const int MAX_PAYLOAD = 1100;  // Keeps datagrams below common MTUs.
const double MAX_WINDOW = 1024.0;
const double MIN_WINDOW = 8.0;
const int MAX_ACK_RANGES = 64;
const int KEEPALIVE_MS = 1000;
const int DEAD_TIMEOUT_MS = 20000;
const uint32_t RECEIVE_WINDOW = 4096;

double Seconds(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

int ElapsedMs(Clock::time_point since, Clock::time_point now) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - since)
            .count());
}

// Sequence numbers wrap; compare them as a signed difference.
bool SeqLess(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

uint16_t ReadU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t ReadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void WriteU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void WriteHeader(uint8_t* p, UdpStream::Kind kind, uint32_t connection) {
    p[0] = UdpStream::UDP_MAGIC;
    p[1] = kind;
    WriteU32(p + 2, connection);
}

};  // namespace

// ================================================================================================
// StunClient.

void StunClient::start() {
    myServers.clear();
    for (const auto& server : STUN_SERVERS) {
        Server s;
        if (NetEndpoint::resolve(server[0], std::atoi(server[1]), s.address)) {
            myServers.push_back(s);
        }
    }
    std::random_device rd;
    for (uint8_t& b : myTransaction) b = static_cast<uint8_t>(rd());
    myStart = Clock::now();
    myLastSend = Clock::time_point();
    myAnswers = 0;
    mySymmetric = false;
    myResult = NetEndpoint();
    myStarted = true;
}

bool StunClient::isDone() const {
    if (!myStarted) return false;
    if (myServers.empty() || myAnswers >= 2) return true;
    return ElapsedMs(myStart, Clock::now()) > STUN_TIMEOUT_MS;
}

void StunClient::update(NetSocket& socket) {
    if (!myStarted || isDone()) return;
    auto now = Clock::now();
    if (ElapsedMs(myLastSend, now) < STUN_RETRY_MS) return;
    myLastSend = now;

    uint8_t request[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    memcpy(request + 8, myTransaction, sizeof(myTransaction));
    for (const auto& server : myServers) {
        if (!server.answered) {
            socket.sendTo(request, sizeof(request), server.address);
        }
    }
}

bool StunClient::onPacket(const uint8_t* data, int size,
                          const NetEndpoint& from) {
    // STUN messages start with two zero bits and contain the magic cookie.
    if (size < 20 || (data[0] & 0xC0) != 0 ||
        ReadU32BE(data + 4) != STUN_COOKIE) {
        return false;
    }
    if (ReadU16BE(data) != 0x0101 ||
        memcmp(data + 8, myTransaction, sizeof(myTransaction)) != 0) {
        return true;  // Not a success response to our request.
    }
    Server* server = nullptr;
    for (auto& s : myServers) {
        if (s.address == from) server = &s;
    }
    if (!server || server->answered) return true;

    int length = std::min<int>(ReadU16BE(data + 2), size - 20);
    const uint8_t* attr = data + 20;
    const uint8_t* end = attr + length;
    NetEndpoint mapped;
    while (end - attr >= 4) {
        uint16_t type = ReadU16BE(attr);
        uint16_t len = ReadU16BE(attr + 2);
        const uint8_t* value = attr + 4;
        if (end - value < len) break;
        // MAPPED-ADDRESS or XOR-MAPPED-ADDRESS, IPv4 family.
        if ((type == 0x0001 || type == 0x0020) && len >= 8 && value[1] == 1) {
            NetEndpoint ep;
            ep.port = ReadU16BE(value + 2);
            ep.ip = ReadU32BE(value + 4);
            if (type == 0x0020) {
                ep.port ^= static_cast<uint16_t>(STUN_COOKIE >> 16);
                ep.ip ^= STUN_COOKIE;
            }
            mapped = ep;
            if (type == 0x0020) break;  // Preferred.
        }
        attr = value + ((len + 3) & ~3);
    }
    if (!mapped.isValid()) return true;

    server->answered = true;
    if (myAnswers > 0 && mapped.port != myResult.port) mySymmetric = true;
    if (myAnswers == 0) myResult = mapped;
    ++myAnswers;
    return true;
}

// ================================================================================================
// UdpStream: static helpers.

bool UdpStream::parseHeader(const uint8_t* data, int size, Kind& kind,
                            uint32_t& connection) {
    if (size < HEADER_SIZE || data[0] != UDP_MAGIC) return false;
    if (data[1] < KIND_PUNCH || data[1] > KIND_DISCOVER) return false;
    kind = static_cast<Kind>(data[1]);
    connection = ReadU32(data + 2);
    return true;
}

void UdpStream::sendControl(NetSocket& socket, const NetEndpoint& to, Kind kind,
                            uint32_t connection) {
    uint8_t packet[HEADER_SIZE];
    WriteHeader(packet, kind, connection);
    socket.sendTo(packet, sizeof(packet), to);
}

void UdpStream::sendPunch(NetSocket& socket, const NetEndpoint& to,
                          uint32_t connection, uint32_t token, Kind kind) {
    uint8_t packet[HEADER_SIZE + 4];
    WriteHeader(packet, kind, connection);
    WriteU32(packet + HEADER_SIZE, token);
    socket.sendTo(packet, sizeof(packet), to);
}

// ================================================================================================
// UdpStream.

UdpStream::UdpStream(uint32_t connection, const NetEndpoint& peer)
    : myConnection(connection), myPeer(peer) {
    myLastReceive = Clock::now();
    myLastSend = myLastReceive;
}

void UdpStream::write(const uint8_t* data, size_t size) {
    // Drop bytes that were already moved into segments.
    if (myQueuePos > (1u << 20)) {
        myQueue.erase(myQueue.begin(),
                      myQueue.begin() + static_cast<ptrdiff_t>(myQueuePos));
        myQueuePos = 0;
    }
    myQueue.insert(myQueue.end(), data, data + size);
}

bool UdpStream::hasPendingWrites() const {
    return myQueuePos < myQueue.size() || !mySegments.empty();
}

bool UdpStream::isDead() const {
    return ElapsedMs(myLastReceive, Clock::now()) > DEAD_TIMEOUT_MS;
}

double UdpStream::myRto() const {
    double rto = myHasRtt ? mySrtt + 4.0 * myRttVar : 0.5;
    return std::clamp(rto, 0.15, 3.0);
}

void UdpStream::myOnLoss(Clock::time_point now) {
    // React at most once per round trip.
    double rtt = myHasRtt ? std::max(mySrtt, 0.02) : 0.2;
    if (Seconds(now - myLastLoss) < rtt) return;
    myLastLoss = now;
    myThreshold = std::max(myWindow * 0.7, MIN_WINDOW);
    myWindow = myThreshold;
}

void UdpStream::onPacket(Kind kind, const uint8_t* body, int size,
                         Bytes& received) {
    auto now = Clock::now();
    myLastReceive = now;

    if (kind == KIND_DATA) {
        if (size < 4) return;
        uint32_t seq = ReadU32(body);
        myAckDue = true;
        if (SeqLess(seq, myRecvNext) ||
            !SeqLess(seq, myRecvNext + RECEIVE_WINDOW)) {
            return;  // Duplicate, or too far ahead.
        }
        Bytes data(body + 4, body + size);
        if (seq != myRecvNext) {
            myOutOfOrder.emplace(seq, std::move(data));
            return;
        }
        received.insert(received.end(), data.begin(), data.end());
        ++myRecvNext;
        for (auto it = myOutOfOrder.find(myRecvNext); it != myOutOfOrder.end();
             it = myOutOfOrder.find(myRecvNext)) {
            received.insert(received.end(), it->second.begin(),
                            it->second.end());
            myOutOfOrder.erase(it);
            ++myRecvNext;
        }
        return;
    }

    if (kind != KIND_ACK || size < 5) return;
    uint32_t next = ReadU32(body);
    int numRanges = std::min<int>(body[4], (size - 5) / 6);
    const uint8_t* ranges = body + 5;
    auto inRanges = [&](uint32_t seq) {
        for (int i = 0; i < numRanges; ++i) {
            uint32_t first = ReadU32(ranges + i * 6);
            uint32_t count = ranges[i * 6 + 4] | (ranges[i * 6 + 5] << 8);
            if (!SeqLess(seq, first) && SeqLess(seq, first + count)) {
                return true;
            }
        }
        return false;
    };

    // The newest segment the other side has.
    uint32_t highest = next;
    for (int i = 0; i < numRanges; ++i) {
        uint32_t first = ReadU32(ranges + i * 6);
        uint32_t count = ranges[i * 6 + 4] | (ranges[i * 6 + 5] << 8);
        if (count > 0 && SeqLess(highest, first + count - 1)) {
            highest = first + count - 1;
        }
    }

    for (auto& seg : mySegments) {
        if (seg.acked) continue;
        if (!SeqLess(seg.seq, next) && !inRanges(seg.seq)) continue;
        seg.acked = true;
        // Grow the window: quickly at first, then by one segment per round
        // trip.
        if (myWindow < myThreshold) {
            myWindow += 1.0;
        } else {
            myWindow += 1.0 / myWindow;
        }
        myWindow = std::min(myWindow, MAX_WINDOW);
        // Only segments that were sent once give a clear round trip time.
        if (seg.sends == 1) {
            double sample = Seconds(now - seg.sentAt);
            if (!myHasRtt) {
                mySrtt = sample;
                myRttVar = sample / 2.0;
                myHasRtt = true;
            } else {
                myRttVar = 0.75 * myRttVar + 0.25 * std::abs(mySrtt - sample);
                mySrtt = 0.875 * mySrtt + 0.125 * sample;
            }
        }
    }
    while (!mySegments.empty() && mySegments.front().acked) {
        mySegments.pop_front();
    }

    // Fast resend: segments well before the newest acknowledged one were
    // most likely lost.
    for (auto& seg : mySegments) {
        if (!SeqLess(seg.seq + 3, highest)) break;
        if (!seg.acked && seg.sends > 0 &&
            Seconds(now - seg.sentAt) > mySrtt * 1.25) {
            seg.sentAt = Clock::time_point();  // Resent in update().
            myOnLoss(now);
        }
    }
}

void UdpStream::mySendSegment(NetSocket& socket, Segment& seg,
                              Clock::time_point now) {
    uint8_t packet[HEADER_SIZE + 4 + MAX_PAYLOAD];
    WriteHeader(packet, KIND_DATA, myConnection);
    WriteU32(packet + HEADER_SIZE, seg.seq);
    memcpy(packet + HEADER_SIZE + 4, seg.data.data(), seg.data.size());
    int size = HEADER_SIZE + 4 + static_cast<int>(seg.data.size());
    if (socket.sendTo(packet, size, myPeer) == size) {
        seg.sentAt = now;
        ++seg.sends;
        myLastSend = now;
    }
}

void UdpStream::mySendAck(NetSocket& socket, Clock::time_point now) {
    uint8_t packet[HEADER_SIZE + 5 + MAX_ACK_RANGES * 6];
    WriteHeader(packet, KIND_ACK, myConnection);
    WriteU32(packet + HEADER_SIZE, myRecvNext);

    // Ranges of segments that arrived after a missing one.
    uint8_t* ranges = packet + HEADER_SIZE + 5;
    int numRanges = 0;
    uint32_t first = 0, count = 0;
    auto flush = [&] {
        if (count == 0 || numRanges == MAX_ACK_RANGES) return;
        WriteU32(ranges + numRanges * 6, first);
        ranges[numRanges * 6 + 4] = static_cast<uint8_t>(count);
        ranges[numRanges * 6 + 5] = static_cast<uint8_t>(count >> 8);
        ++numRanges;
    };
    for (const auto& entry : myOutOfOrder) {
        if (count > 0 && entry.first == first + count && count < 0xFFFF) {
            ++count;
        } else {
            flush();
            first = entry.first;
            count = 1;
        }
    }
    flush();
    packet[HEADER_SIZE + 4] = static_cast<uint8_t>(numRanges);
    socket.sendTo(packet, HEADER_SIZE + 5 + numRanges * 6, myPeer);
    myAckDue = false;
    myLastSend = now;
}

void UdpStream::update(NetSocket& socket) {
    auto now = Clock::now();
    if (myAckDue) mySendAck(socket, now);

    // Resend segments whose acknowledgement is overdue; back off when the
    // same segment keeps getting lost.
    double rto = myRto();
    size_t inFlight = 0;
    for (auto& seg : mySegments) {
        if (seg.acked) continue;
        ++inFlight;
        if (seg.sends > 0) {
            double timeout =
                rto * static_cast<double>(1 << std::min(seg.sends - 1, 3));
            if (Seconds(now - seg.sentAt) < timeout) continue;
            // A fast resend clears sentAt and has already counted the loss.
            if (seg.sentAt != Clock::time_point()) myOnLoss(now);
        }
        mySendSegment(socket, seg, now);
    }

    // Send new data while the window allows.
    while (myQueuePos < myQueue.size() &&
           static_cast<double>(inFlight) < myWindow) {
        Segment seg;
        seg.seq = myNextSeq++;
        size_t n = std::min<size_t>(MAX_PAYLOAD, myQueue.size() - myQueuePos);
        auto begin = myQueue.begin() + static_cast<ptrdiff_t>(myQueuePos);
        seg.data.assign(begin, begin + static_cast<ptrdiff_t>(n));
        myQueuePos += n;
        mySegments.push_back(std::move(seg));
        mySendSegment(socket, mySegments.back(), now);
        ++inFlight;
    }
    if (myQueuePos == myQueue.size()) {
        myQueue.clear();
        myQueuePos = 0;
    }

    // Keep the connection (and the routers' mappings) alive.
    if (ElapsedMs(myLastSend, now) > KEEPALIVE_MS) mySendAck(socket, now);
}

};  // namespace Vortex
