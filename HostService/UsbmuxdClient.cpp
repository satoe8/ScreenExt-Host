// UsbmuxdClient.cpp
#include "UsbmuxdClient.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace host {

namespace {

constexpr int      kHandshakeTimeoutMs = 2500;
constexpr uint32_t kMaxPacket          = 1u << 20;

// ---- socket helpers -------------------------------------------------------------------------

SOCKET OpenMux()
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    DWORD to = kHandshakeTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(UsbmuxdClient::kMuxPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool SendAll(SOCKET s, const uint8_t* p, size_t n)
{
    while (n > 0) {
        const int r = send(s, reinterpret_cast<const char*>(p), (int)n, 0);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

bool RecvAll(SOCKET s, uint8_t* p, size_t n)
{
    while (n > 0) {
        const int r = recv(s, reinterpret_cast<char*>(p), (int)n, 0);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

// ---- protocol -------------------------------------------------------------------------------

std::string RequestPlist(const char* messageType, const std::string& extraKeys)
{
    std::string p;
    p += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<plist version=\"1.0\">\n<dict>\n"
         "<key>BundleID</key><string>com.screenext.host</string>\n"
         "<key>ClientVersionString</key><string>ScreenExt 1.0</string>\n"
         "<key>MessageType</key><string>";
    p += messageType;
    p += "</string>\n"
         "<key>ProgName</key><string>ScreenExt</string>\n"
         "<key>kLibUSBMuxVersion</key><integer>3</integer>\n";
    p += extraKeys;
    p += "</dict>\n</plist>\n";
    return p;
}

// Sends one request packet and reads exactly one reply packet (header + plist body).
bool Exchange(SOCKET s, uint32_t tag, const std::string& plist, std::string& reply)
{
    // Header fields are little-endian on the wire; Windows on x86/ARM64 is little-endian too.
    std::string pkt(16 + plist.size(), '\0');
    const uint32_t hdr[4] = { (uint32_t)pkt.size(), 1u /*plist*/, 8u /*request*/, tag };
    std::memcpy(&pkt[0], hdr, 16);
    std::memcpy(&pkt[16], plist.data(), plist.size());
    if (!SendAll(s, reinterpret_cast<const uint8_t*>(pkt.data()), pkt.size())) return false;

    uint32_t rh[4] = {};
    if (!RecvAll(s, reinterpret_cast<uint8_t*>(rh), 16)) return false;
    if (rh[0] < 16 || rh[0] > kMaxPacket) return false;
    reply.assign(rh[0] - 16, '\0');
    if (!reply.empty() && !RecvAll(s, reinterpret_cast<uint8_t*>(&reply[0]), reply.size())) return false;
    return true;
}

// ---- tiny plist scanner (the muxer's XML plists are flat and regular) -----------------------

// Finds <key>KEY</key> in [begin, end) followed by <TAG>value</TAG> and returns the value text.
bool FindKeyValue(const std::string& s, size_t begin, size_t end, const std::string& key, const char* tag, std::string& out)
{
    const std::string k = "<key>" + key + "</key>";
    size_t p = s.find(k, begin);
    if (p == std::string::npos || p >= end) return false;
    p += k.size();
    while (p < end && std::isspace((unsigned char)s[p])) ++p;

    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    if (s.compare(p, open.size(), open) != 0) return false;
    p += open.size();
    const size_t q = s.find(close, p);
    if (q == std::string::npos) return false;
    out = s.substr(p, q - p);
    return true;
}

bool FindKeyValue(const std::string& s, const std::string& key, const char* tag, std::string& out)
{
    return FindKeyValue(s, 0, s.size(), key, tag, out);
}

// Result code of a {MessageType: Result, Number: n} reply; -1 if the reply is not a Result.
int ParseResult(const std::string& reply)
{
    std::string type, number;
    if (!FindKeyValue(reply, "MessageType", "string", type) || type != "Result") return -1;
    if (!FindKeyValue(reply, "Number", "integer", number)) return -1;
    return (int)std::strtol(number.c_str(), nullptr, 10);
}

// Splits the DeviceList array into the text of each device <dict> (nested Properties dict included).
std::vector<std::string> DeviceDicts(const std::string& s)
{
    std::vector<std::string> out;
    size_t pos = s.find("<key>DeviceList</key>");
    if (pos == std::string::npos) return out;
    pos = s.find("<array>", pos);
    if (pos == std::string::npos) return out;       // "<array/>" (empty list) lands here
    pos += 7;

    for (;;) {
        const size_t arrEnd = s.find("</array>", pos);
        const size_t d = s.find("<dict>", pos);
        if (d == std::string::npos || (arrEnd != std::string::npos && arrEnd < d)) break;

        int depth = 0;
        size_t i = d;
        while (i < s.size()) {
            const size_t o = s.find("<dict>", i), c = s.find("</dict>", i);
            if (c == std::string::npos) { i = std::string::npos; break; }
            if (o != std::string::npos && o < c) { ++depth; i = o + 6; }
            else { --depth; i = c + 7; if (depth == 0) break; }
        }
        if (i == std::string::npos || depth != 0) break;
        out.push_back(s.substr(d, i - d));
        pos = i;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------

bool UsbmuxdClient::ListDevices(std::vector<UsbDevice>& out)
{
    out.clear();
    SOCKET s = OpenMux();
    if (s == INVALID_SOCKET) return false;

    std::string reply;
    const bool ok = Exchange(s, 1, RequestPlist("ListDevices", ""), reply);
    closesocket(s);
    if (!ok) return false;
    if (ParseResult(reply) > 0) return false;        // muxer answered with an error code

    for (const std::string& dev : DeviceDicts(reply)) {
        std::string id, type, serial;
        if (!FindKeyValue(dev, "DeviceID", "integer", id)) continue;
        FindKeyValue(dev, "ConnectionType", "string", type);
        FindKeyValue(dev, "SerialNumber", "string", serial);
        if (type != "USB") continue;                  // skip Wi-Fi-sync entries
        UsbDevice d;
        d.deviceId = (uint32_t)std::strtoul(id.c_str(), nullptr, 10);
        d.serial = serial;
        out.push_back(d);
    }
    return true;
}

SOCKET UsbmuxdClient::ConnectToDevice(uint32_t deviceId, uint16_t devicePort, int* result)
{
    if (result) *result = -1;
    SOCKET s = OpenMux();
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    // PortNumber is the port in network byte order, written as a plain integer (5000 -> 34835).
    const unsigned swapped = (unsigned)(((devicePort & 0xFF) << 8) | (devicePort >> 8));
    const std::string extra = "<key>DeviceID</key><integer>" + std::to_string(deviceId) + "</integer>\n"
                              "<key>PortNumber</key><integer>" + std::to_string(swapped) + "</integer>\n";

    std::string reply;
    if (!Exchange(s, 2, RequestPlist("Connect", extra), reply)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    const int code = ParseResult(reply);
    if (result) *result = code;
    if (code != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    // From here on this socket IS the tunnel: clear the handshake timeouts (0 = block forever).
    DWORD none = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&none), sizeof(none));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&none), sizeof(none));
    return s;
}

} // namespace host
