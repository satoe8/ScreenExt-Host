// Transport.h
// Wire protocol + transport for streaming encoded frames to the iOS client and receiving
// back-channel messages (keyframe requests, touch, ping).
//
// ALL integers are LITTLE-ENDIAN (x86 host and ARM64 iOS agree). Structs are packed.
//
// ---------------------------------------------------------------------------------------------
// MESSAGE (identical bytes over TCP and, fragmented, over UDP)
//
//   MsgHeader (12 bytes)
//     u32 magic        0x53444449  ("IDDS" in memory order 49 44 44 53)
//     u8  version      1
//     u8  type         MsgType
//     u16 flags        VideoFlags for MSG_VIDEO, otherwise 0
//     u32 payloadLen   number of bytes that follow this header
//
//   MSG_VIDEO payload:  VideoHeader (20 bytes) + Annex-B access unit (payloadLen - 20 bytes)
//     u32 frameNumber  encoder output counter; gaps = frames the host dropped
//     u64 ptsUs        presentation timestamp, microseconds, 0 at stream start
//     u16 cropX, cropY, cropW, cropH   visible rectangle inside the coded picture
//                                      (2556x1179 inside 2556x1180 / 2560x1184)
//
//   MSG_HELLO (host -> client, first message after connect):
//     u16 codec(1 = H.264 Annex-B), u16 codedW, u16 codedH,
//     u16 cropX, u16 cropY, u16 cropW, u16 cropH, u16 fps
//
//   MSG_KEYFRAME_REQ (client -> host): empty payload. Send when a decoder error occurs or a
//                                      UDP frame has missing fragments.
//   MSG_TOUCH (client -> host): TouchPayload (8 bytes)
//     u8 pointerId, u8 phase (0 down, 1 move, 2 up, 3 cancel),
//     u16 x, u16 y   position in the crop rectangle, 0..65535 (0,0 = top-left of the visible area)
//     u16 pressure   0..65535
//   MSG_PEN (client -> host): PenPayload (14 bytes) - Apple Pencil, injected as a real Windows pen
//     u8 phase (0 down, 1 move, 2 up, 3 cancel), u8 flags (bit0 barrel button, bit1 eraser),
//     u16 x, u16 y (0..65535 in the visible area), u16 pressure (0..65535),
//     i16 tiltX, i16 tiltY (degrees, -90..90), u16 rotation (degrees 0..359)
//   MSG_PING (client -> host): u64 clientTimeUs      -> host answers MSG_PONG:
//   MSG_PONG (host -> client): u64 clientTimeUs (echo), u64 hostTimeUs
//
// ---------------------------------------------------------------------------------------------
// DISCOVERY (UDP, UNICAST probe -> unicast reply; default port 5001, separate from the stream port)
//   probe (client -> host): u32 magic 0x51444449 ("IDDQ"), u8 version(1), u8 reserved[3]   (8 bytes)
//   reply (host -> client): u32 magic 0x52444449 ("IDDR"), u8 version(1), u8 mode (0 TCP, 1 UDP),
//                           u16 streamPort, u8 nameLen, name[nameLen] (UTF-8 PC name)
//   iOS cannot send/receive UDP broadcast without Apple's restricted multicast entitlement, so the
//   client sweeps the probe over every address of its own subnet instead (unicast needs no entitlement).
// ---------------------------------------------------------------------------------------------
// UDP DATAGRAM (12 bytes header + up to mtuPayload bytes)
//   u16 magic 0x4455, u8 kind (0 = fragment of a host->client message, 1 = client->host message),
//   u8 reserved, u32 messageSeq (frameNumber for video, running counter otherwise),
//   u16 fragIndex, u16 fragCount
//   kind 0: concatenating fragments 0..fragCount-1 yields one complete MESSAGE (above).
//   kind 1: the body is one complete MESSAGE (fragCount = 1).
//   The client "connects" by sending any kind-1 datagram (e.g. MSG_PING) to the host's port;
//   it should send a PING about once per second as keep-alive (host times out after 5 s).
// ---------------------------------------------------------------------------------------------
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "HardwareEncoder.h"   // EncodedFrame

namespace host {

constexpr uint32_t kMagic   = 0x53444449u;
constexpr uint8_t  kVersion = 1;

enum MsgType : uint8_t {
    MSG_VIDEO = 1, MSG_HELLO = 2, MSG_KEYFRAME_REQ = 3, MSG_TOUCH = 4, MSG_PING = 5, MSG_PONG = 6, MSG_PEN = 7
};

enum VideoFlags : uint16_t {
    VF_KEYFRAME      = 1 << 0,
    VF_HAS_SPS       = 1 << 1,
    VF_HAS_PPS       = 1 << 2,
    VF_DISCONTINUITY = 1 << 3,   // host dropped frames before this one: flush the decoder first
};

#pragma pack(push, 1)
struct MsgHeader   { uint32_t magic; uint8_t version; uint8_t type; uint16_t flags; uint32_t payloadLen; };
struct VideoHeader { uint32_t frameNumber; uint64_t ptsUs; uint16_t cropX, cropY, cropW, cropH; };
struct HelloPayload{ uint16_t codec, codedW, codedH, cropX, cropY, cropW, cropH, fps; };
struct TouchPayload{ uint8_t pointerId; uint8_t phase; uint16_t x, y, pressure; };
struct PenPayload  { uint8_t phase; uint8_t flags; uint16_t x, y, pressure; int16_t tiltX, tiltY; uint16_t rotation; };
struct UdpHeader   { uint16_t magic; uint8_t kind; uint8_t reserved; uint32_t seq; uint16_t fragIndex, fragCount; };
#pragma pack(pop)
static_assert(sizeof(MsgHeader) == 12 && sizeof(VideoHeader) == 20 && sizeof(HelloPayload) == 16 &&
              sizeof(TouchPayload) == 8 && sizeof(PenPayload) == 14 && sizeof(UdpHeader) == 12, "wire struct size changed");

constexpr uint16_t kUdpMagic = 0x4455;
constexpr uint16_t kDiscoveryPort = 5001;
constexpr uint32_t kProbeMagic = 0x51444449u;   // "IDDQ"
constexpr uint32_t kReplyMagic = 0x52444449u;   // "IDDR"

enum class TransportMode { TcpListen, TcpConnect, Udp, UsbTunnel };

struct TransportConfig {
    TransportMode mode = TransportMode::TcpListen;
    std::string   host = "127.0.0.1";   // TcpConnect target (e.g. iproxy local port)
    uint16_t      port = 5000;

    // Native USB tunnel (replaces iproxy): when set with TcpListen, the host ALSO connects through Apple's
    // usbmuxd to `usbDevicePort` on any plugged-in iPhone/iPad whose app is listening there.
    // TransportMode::UsbTunnel does only that (no LAN listener).
    bool          enableUsb = false;
    uint16_t      usbDevicePort = 5000;

    // Stream description (goes into HELLO and every video header).
    uint16_t codedW = 2556, codedH = 1180;
    uint16_t cropX = 0, cropY = 0, cropW = 2556, cropH = 1179;
    uint16_t fps = 60;

    uint16_t discoveryPort   = kDiscoveryPort;   // 0 = no discovery responder
    size_t   maxQueuedFrames = 4;     // beyond this the client is considered too slow
    size_t   udpPayload      = 1200;  // bytes per datagram body (fits typical 1500 MTU incl. headers)
    double   udpMaxMbps      = 300.0; // simple pacing for UDP bursts (IDR frames)
};

enum class TouchPhase : uint8_t { Down = 0, Move = 1, Up = 2, Cancel = 3 };
struct TouchEvent { uint8_t pointerId; TouchPhase phase; float x, y; float pressure; };  // x,y,pressure in 0..1
struct PenEvent   { TouchPhase phase; bool barrel; bool eraser; float x, y, pressure; int tiltX, tiltY, rotation; };

struct TransportCallbacks {
    std::function<void()>                 onClientConnected;     // request a keyframe here
    std::function<void()>                 onClientDisconnected;
    std::function<void()>                 onKeyframeRequest;     // client PLI or host-side overload
    std::function<void(const TouchEvent&)> onTouch;
    std::function<void(const PenEvent&)>   onPen;
};

struct TransportStats {
    uint64_t framesSent = 0, bytesSent = 0, framesDropped = 0, queueOverflows = 0;
    uint64_t keyframeRequests = 0, touchEvents = 0, datagramsSent = 0;
    uint64_t penEvents = 0;
    uint64_t hostKeyRequests = 0;   // IDRs requested by the host itself after a queue overflow
};

class Transport {
public:
    Transport() = default;
    ~Transport() { Stop(); }
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool Start(const TransportConfig& cfg, TransportCallbacks cb);
    void Stop();

    bool IsClientConnected() const { return connected_; }
    bool ConnectedViaUsb() const { return viaUsb_; }   // valid while connected

    // Non-blocking (safe from the encoder's callback thread): serializes and enqueues the frame.
    // Drops it if no client is connected or the client is too slow (see maxQueuedFrames).
    void SendFrame(const EncodedFrame& f);

    TransportStats Stats() const;

    // The stream geometry changed (e.g. the PC resolution was switched): flush stale video, send a
    // fresh HELLO to the connected client, and drop frames until the next IDR. Call it after the
    // encoder has been restarted (so no old-geometry frames can still arrive).
    void UpdateGeometry(uint16_t codedW, uint16_t codedH, uint16_t cropW, uint16_t cropH);

private:
    using Message = std::shared_ptr<std::vector<uint8_t>>;
    struct QueueItem { Message msg; uint32_t seq; bool video; };

    void IoThreadTcp();
    void IoThreadUdp();
    void SenderThread();
    void DiscoveryThread();
    void UsbThread();
    void RunTcpSession(SOCKET c, bool viaUsb);   // one client session; shared by LAN accept and USB tunnel

    bool SendAllTcp(const uint8_t* p, size_t n);
    void SendFragmentedUdp(const QueueItem& item);
    void HandleMessage(const MsgHeader& h, const uint8_t* payload);
    void Enqueue(Message m, uint32_t seq, bool video, bool front);
    Message BuildSimple(MsgType t, const void* payload, size_t n);
    Message BuildHello();
    void OnConnected();
    void OnDisconnected();

    TransportConfig    cfg_{};
    TransportCallbacks cb_;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    bool wsaStarted_ = false;

    std::atomic<SOCKET> listenSock_{INVALID_SOCKET};
    std::atomic<SOCKET> clientSock_{INVALID_SOCKET};   // TCP session, or the UDP socket
    sockaddr_in         udpPeer_{};
    std::mutex          peerMutex_;
    std::atomic<ULONGLONG> lastPeerTickMs_{0};

    std::thread ioThread_, sendThread_, discoveryThread_, usbThread_;
    std::mutex sessionMutex_;               // only one client session at a time
    std::atomic<bool> viaUsb_{false};
    std::atomic<SOCKET> discoverySock_{INVALID_SOCKET};
    std::atomic<ULONGLONG> lastOverflowKeyReqMs_{0};

    std::mutex              qMutex_;
    std::condition_variable qCv_;
    std::deque<QueueItem>   queue_;
    bool                    waitingForKey_ = false;
    uint32_t                ctlSeq_ = 0;

    mutable std::mutex statsMutex_;
    TransportStats     stats_;
};

} // namespace host
