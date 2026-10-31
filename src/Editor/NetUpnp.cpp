#include <Editor/NetUpnp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

namespace Vortex {
namespace {

typedef std::chrono::steady_clock Clock;

const NetEndpoint SSDP_ADDRESS = {0xEFFFFFFA, 1900};  // 239.255.255.250
const int DISCOVER_TIMEOUT_MS = 2500;
const int HTTP_TIMEOUT_MS = 3000;
const int MAX_PORT_ATTEMPTS = 8;
const int LEASE_SECONDS = 3600;
const int UPNP_CONFLICT = 718;        // ConflictInMappingEntry.
const int UPNP_ONLY_PERMANENT = 725;  // OnlyPermanentLeasesSupported.
const char* const MAPPING_NAME = "ArrowVortex";

const char* const SEARCH_TARGETS[] = {
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
};

int ElapsedMs(Clock::time_point since) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                              since)
            .count());
}

std::string ToLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Returns the value of an HTTP header (case-insensitive name), or "".
std::string HeaderValue(const std::string& headers, const std::string& name) {
    std::string lower = ToLower(headers);
    std::string key = "\n" + ToLower(name) + ":";
    size_t pos = lower.find(key);
    if (pos == std::string::npos) return std::string();
    pos += key.size();
    size_t end = headers.find_first_of("\r\n", pos);
    std::string value = headers.substr(pos, end - pos);
    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value;
}

// Returns the text of the first element with the given name, ignoring any
// namespace prefix, or "".
std::string XmlValue(const std::string& xml, const std::string& name,
                     size_t from = 0, size_t to = std::string::npos) {
    std::string open = name + ">";
    size_t pos = from;
    while (true) {
        pos = xml.find(open, pos);
        if (pos == std::string::npos || pos >= to) return std::string();
        // Make sure this is an opening tag: "<name>" or "<prefix:name>".
        size_t lt = xml.rfind('<', pos);
        if (lt != std::string::npos && xml[lt + 1] != '/' &&
            (lt + 1 == pos || xml[pos - 1] == ':')) {
            break;
        }
        pos += open.size();
    }
    pos += open.size();
    size_t end = xml.find('<', pos);
    if (end == std::string::npos) return std::string();
    return xml.substr(pos, end - pos);
}

struct Url {
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool ParseUrl(const std::string& text, Url& out) {
    std::string s = text;
    if (ToLower(s.substr(0, 7)) != "http://") return false;
    s = s.substr(7);
    size_t slash = s.find('/');
    std::string hostPort = s.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : s.substr(slash);
    size_t colon = hostPort.rfind(':');
    out.host = hostPort.substr(0, colon);
    out.port = 80;
    if (colon != std::string::npos) {
        out.port = std::atoi(hostPort.c_str() + colon + 1);
    }
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

// Resolves a (possibly relative) URL from a device description.
std::string ResolveUrl(const std::string& base, const std::string& url) {
    if (ToLower(url.substr(0, 7)) == "http://") return url;
    Url b;
    if (!ParseUrl(base, b)) return url;
    std::string root = "http://" + b.host + ":" + std::to_string(b.port);
    if (!url.empty() && url[0] == '/') return root + url;
    return root + "/" + url;
}

std::string Dechunk(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find("\r\n", pos);
        if (eol == std::string::npos) break;
        size_t size = std::strtoul(body.c_str() + pos, nullptr, 16);
        pos = eol + 2;
        if (size == 0 || pos + size > body.size()) break;
        out += body.substr(pos, size);
        pos += size + 2;
    }
    return out;
}

struct HttpResponse {
    int status = 0;
    std::string body;
    NetEndpoint localAddress;  // The local end of the connection.
};

// Sends an HTTP request and reads the whole response. Blocks for at most
// HTTP_TIMEOUT_MS (or until stopped, if a stop token is given).
bool HttpRequest(const Url& url, const std::string& request, HttpResponse& out,
                 std::string& error, const std::stop_token* st) {
    NetEndpoint to;
    if (!NetEndpoint::resolve(url.host, url.port, to)) {
        error = "could not resolve " + url.host;
        return false;
    }
    NetSocket sock;
    if (!sock.beginConnect(to, error)) return false;

    auto start = Clock::now();
    auto timedOut = [&] {
        return ElapsedMs(start) > HTTP_TIMEOUT_MS ||
               (st && st->stop_requested());
    };
    while (true) {
        if (timedOut()) {
            error = "the router did not respond";
            return false;
        }
        int flags = sock.poll(true, 50);
        if (flags & (NetSocket::POLL_WRITE | NetSocket::POLL_ERROR)) {
            if (!sock.finishConnect(error)) return false;
            break;
        }
    }
    out.localAddress = sock.localEndpoint();

    size_t sent = 0;
    while (sent < request.size()) {
        if (timedOut()) {
            error = "the router did not respond";
            return false;
        }
        int n = sock.send(request.data() + sent,
                          static_cast<int>(request.size() - sent));
        if (n < 0) {
            error = "connection to the router failed";
            return false;
        }
        sent += static_cast<size_t>(n);
        if (n == 0) sock.poll(true, 20);
    }

    std::string response;
    char buf[4096];
    while (true) {
        if (timedOut()) break;
        int flags = sock.poll(false, 50);
        if (!(flags & (NetSocket::POLL_READ | NetSocket::POLL_ERROR))) continue;
        int n = sock.recv(buf, sizeof(buf));
        if (n < 0) break;  // Closed by the router: the response is complete.
        response.append(buf, static_cast<size_t>(n));

        // Stop early once Content-Length bytes have arrived.
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            std::string length =
                HeaderValue(response.substr(0, headerEnd), "Content-Length");
            if (!length.empty() &&
                response.size() - headerEnd - 4 >=
                    std::strtoul(length.c_str(), nullptr, 10)) {
                break;
            }
        }
    }

    size_t headerEnd = response.find("\r\n\r\n");
    if (response.compare(0, 5, "HTTP/") != 0 ||
        headerEnd == std::string::npos) {
        error = "invalid response from the router";
        return false;
    }
    size_t space = response.find(' ');
    out.status = std::atoi(response.c_str() + space + 1);
    std::string headers = response.substr(0, headerEnd);
    out.body = response.substr(headerEnd + 4);
    if (ToLower(HeaderValue(headers, "Transfer-Encoding")) == "chunked") {
        out.body = Dechunk(out.body);
    }
    return true;
}

// Finds gateway devices on the local network with SSDP, and returns the URLs
// of their device descriptions.
std::vector<std::string> Discover(const std::stop_token& st) {
    std::vector<std::string> locations;
    NetSocket sock;
    std::string error;
    if (!sock.openUdp(0, error)) return locations;

    // Also ask the default gateway directly: some routers answer from another
    // port than 1900, and firewalls drop such answers to a multicast search.
    std::vector<NetEndpoint> targets = {SSDP_ADDRESS};
    for (uint32_t ip : NetSocket::defaultGateways()) {
        targets.push_back({ip, SSDP_ADDRESS.port});
    }
    for (const char* target : SEARCH_TARGETS) {
        std::string msg =
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 2\r\n"
            "ST: " +
            std::string(target) + "\r\n\r\n";
        for (const auto& to : targets) {
            sock.sendTo(msg.data(), static_cast<int>(msg.size()), to);
        }
    }

    // Wait a little after the first answer, in case there are more routers.
    auto start = Clock::now();
    int deadline = DISCOVER_TIMEOUT_MS;
    char buf[2048];
    while (ElapsedMs(start) < deadline && !st.stop_requested()) {
        if (!(sock.poll(false, 50) & NetSocket::POLL_READ)) continue;
        NetEndpoint from;
        int n;
        while ((n = sock.recvFrom(buf, sizeof(buf) - 1, from)) > 0) {
            std::string location = HeaderValue(
                std::string(buf, static_cast<size_t>(n)), "Location");
            if (location.empty() ||
                std::find(locations.begin(), locations.end(), location) !=
                    locations.end()) {
                continue;
            }
            locations.push_back(location);
            deadline = std::min(deadline, ElapsedMs(start) + 300);
        }
    }
    return locations;
}

// A WAN connection service of a gateway, which handles port mappings.
struct Gateway {
    Url control;
    std::string serviceType;
    uint32_t localIp = 0;  // Our address on the gateway's network.
};

// Reads a device description and finds its WAN connection service.
bool FindService(const std::string& location, Gateway& out,
                 const std::stop_token& st) {
    Url url;
    if (!ParseUrl(location, url)) return false;
    std::string request = "GET " + url.path +
                          " HTTP/1.1\r\n"
                          "Host: " +
                          url.host + ":" + std::to_string(url.port) +
                          "\r\n"
                          "Connection: close\r\n\r\n";
    HttpResponse response;
    std::string error;
    if (!HttpRequest(url, request, response, error, &st) ||
        response.status != 200) {
        return false;
    }
    const std::string& xml = response.body;
    std::string base = XmlValue(xml, "URLBase");
    if (base.empty()) base = location;

    // Prefer an IP connection over a PPP one; take the first that exists.
    for (const char* kind : {"WANIPConnection", "WANPPPConnection"}) {
        size_t pos = 0;
        while ((pos = xml.find("<serviceType>", pos)) != std::string::npos) {
            size_t end = xml.find("</service>", pos);
            std::string type = XmlValue(xml, "serviceType", pos, end);
            pos += 13;
            if (type.find(kind) == std::string::npos) continue;
            std::string control = XmlValue(xml, "controlURL", pos, end);
            if (control.empty()) continue;
            if (!ParseUrl(ResolveUrl(base, control), out.control)) continue;
            out.serviceType = type;
            out.localIp = response.localAddress.ip;
            return true;
        }
    }
    return false;
}

// Calls a SOAP action on the gateway. Returns the HTTP status (0 on failure).
int SoapCall(const Gateway& gw, const std::string& action,
             const std::string& args, std::string& body,
             const std::stop_token* st) {
    std::string envelope =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:" +
        action + " xmlns:u=\"" + gw.serviceType + "\">" + args +
        "</u:" + action + "></s:Body></s:Envelope>\r\n";
    std::string request = "POST " + gw.control.path +
                          " HTTP/1.1\r\n"
                          "Host: " +
                          gw.control.host + ":" +
                          std::to_string(gw.control.port) +
                          "\r\n"
                          "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                          "SOAPAction: \"" +
                          gw.serviceType + "#" + action +
                          "\"\r\n"
                          "Content-Length: " +
                          std::to_string(envelope.size()) +
                          "\r\n"
                          "Connection: close\r\n\r\n" +
                          envelope;
    HttpResponse response;
    std::string error;
    if (!HttpRequest(gw.control, request, response, error, st)) return 0;
    body = response.body;
    return response.status;
}

int AddMapping(const Gateway& gw, const char* protocol, int externalPort,
               int internalPort, int lease, const std::stop_token& st) {
    std::string args =
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>" +
        std::to_string(externalPort) +
        "</NewExternalPort>"
        "<NewProtocol>" +
        protocol +
        "</NewProtocol>"
        "<NewInternalPort>" +
        std::to_string(internalPort) +
        "</NewInternalPort>"
        "<NewInternalClient>" +
        NetEndpoint::ipToString(gw.localIp) +
        "</NewInternalClient>"
        "<NewEnabled>1</NewEnabled>"
        "<NewPortMappingDescription>" +
        MAPPING_NAME +
        "</NewPortMappingDescription>"
        "<NewLeaseDuration>" +
        std::to_string(lease) + "</NewLeaseDuration>";
    std::string body;
    int status = SoapCall(gw, "AddPortMapping", args, body, &st);
    if (status == 200) return 0;
    int code = std::atoi(XmlValue(body, "errorCode").c_str());
    return code != 0 ? code : -1;
}

void DeleteMapping(const Gateway& gw, const char* protocol, int externalPort) {
    std::string args =
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>" +
        std::to_string(externalPort) +
        "</NewExternalPort>"
        "<NewProtocol>" +
        protocol + "</NewProtocol>";
    std::string body;
    SoapCall(gw, "DeletePortMapping", args, body, nullptr);
}

};  // namespace

// ================================================================================================
// PortMapper.

PortMapper::~PortMapper() { stop(); }

void PortMapper::start(int port) {
    stop();
    Result pending;
    pending.state = STATE_PENDING;
    mySetResult(pending);
    try {
        myThread =
            std::jthread([this, port](std::stop_token st) { myRun(st, port); });
    } catch (const std::system_error& e) {
        Result failed;
        failed.state = STATE_FAILED;
        failed.error = e.what();
        mySetResult(failed);
    }
}

void PortMapper::stop() {
    if (myThread.joinable()) {
        myThread.request_stop();
        myThread.join();
    }
    mySetResult(Result());
}

PortMapper::Result PortMapper::getResult() const {
    std::lock_guard<std::mutex> lock(myMutex);
    return myResult;
}

void PortMapper::mySetResult(const Result& result) {
    std::lock_guard<std::mutex> lock(myMutex);
    myResult = result;
}

void PortMapper::myRun(const std::stop_token& st, int port) {
    Result result;
    result.state = STATE_FAILED;

    Gateway gw;
    bool found = false;
    for (const auto& location : Discover(st)) {
        if (st.stop_requested()) return;
        if (FindService(location, gw, st)) {
            found = true;
            break;
        }
    }
    if (!found) {
        result.error = "no UPnP router found";
        mySetResult(result);
        return;
    }

    std::string body;
    if (SoapCall(gw, "GetExternalIPAddress", "", body, &st) == 200) {
        NetEndpoint::parseIp(XmlValue(body, "NewExternalIPAddress"),
                             result.external.ip);
    }

    // Use the same port outside if possible. The mapping expires, so that it
    // does not stay behind if we crash, and is renewed while hosting; some
    // routers only accept permanent mappings.
    int mapped = 0;
    int lease = LEASE_SECONDS;
    for (int i = 0; i < MAX_PORT_ATTEMPTS && !mapped; ++i) {
        int external = port + i;
        if (external > 65535 || st.stop_requested()) break;
        int err = AddMapping(gw, "TCP", external, port, lease, st);
        if (err == UPNP_ONLY_PERMANENT) {
            lease = 0;
            err = AddMapping(gw, "TCP", external, port, lease, st);
        }
        if (err == UPNP_CONFLICT) continue;  // Taken by another computer.
        if (err != 0) {
            result.error = "the router refused the port mapping (error " +
                           std::to_string(err) + ")";
            break;
        }
        if (AddMapping(gw, "UDP", external, port, lease, st) != 0) {
            DeleteMapping(gw, "TCP", external);
            continue;
        }
        mapped = external;
    }

    if (mapped) {
        result.state = STATE_MAPPED;
        result.external.port = static_cast<uint16_t>(mapped);
        result.error.clear();
    } else if (result.error.empty()) {
        result.error = "no free port on the router";
    }
    mySetResult(result);
    if (!mapped) return;

    // Keep the mapping until stopped.
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_lock<std::mutex> lock(mutex);
    while (!st.stop_requested()) {
        if (lease == 0) {
            cv.wait(lock, st, [] { return false; });
        } else {
            cv.wait_for(lock, st, std::chrono::seconds(lease / 2),
                        [] { return false; });
            if (st.stop_requested()) break;
            AddMapping(gw, "TCP", mapped, port, lease, st);
            AddMapping(gw, "UDP", mapped, port, lease, st);
        }
    }

    DeleteMapping(gw, "TCP", mapped);
    DeleteMapping(gw, "UDP", mapped);
}

};  // namespace Vortex
