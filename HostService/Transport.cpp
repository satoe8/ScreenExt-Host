// Transport.cpp
#include "Transport.h"
#include "CaptureEngine.h"   // host::Logf
#include "UsbmuxdClient.h"

#include <mstcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

#pragma comment(lib, "ws2_32.lib")

namespace host {

namespace {

int64_t QpcNow()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t QpcFreq()
{
    static const int64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return (int64_t)x.QuadPart; }();
    return f;
}

uint64_t HostTimeUs() { return (uint64_t)(QpcNow() * 1000000ll / QpcFreq()); }

// Scans an Annex-B buffer for SPS (7) / PPS (8) NAL units.
void ScanParamSets(const std::vector<uint8_t>& d, bool& sps, bool& pps)
{
    sps = pps = false;
    for (size_t i = 0; i + 4 < d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            const size_t p = (d[i + 2] == 1) ? i + 3 : i + 4;
            if (p < d.size()) {
                const int t = d[p] & 0x1F;
                if (t == 7) sps = true;
                if (t == 8) pps = true;
            }
        }
    }
}

bool RecvAll(SOCKET s, uint8_t* p, size_t n)
{
    while (n > 0) {
        const int r = recv(s, reinterpret_cast<char*>(p), (int)std::min<size_t>(n, 1 << 20), 0);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

void CloseSock(std::atomic<SOCKET>& s)
{
    const SOCKET old = s.exchange(INVALID_SOCKET);
    if (old != INVALID_SOCKET) {
        shutdown(old, SD_BOTH);
        closesocket(old);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

bool Transport::Start(const TransportConfig& cfg, TransportCallbacks cb)
{
    Stop();
    cfg_ = cfg;
    cb_  = std::move(cb);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Logf(L"[net] WSAStartup failed\n");
        return false;
    }
    wsaStarted_ = true;
    stopping_ = false;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        stats_ = {};
    }

    if (cfg_.mode == TransportMode::Udp) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) { Logf(L"[net] UDP socket failed: %d\n", WSAGetLastError()); return false; }
        int buf = 4 << 20;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&buf), sizeof(buf));
        DWORD to = 500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&to), sizeof(to));
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(cfg_.port);
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            Logf(L"[net] UDP bind to port %u failed: %d\n", cfg_.port, WSAGetLastError());
            closesocket(s);
            return false;
        }
        clientSock_ = s;
        Logf(L"[net] UDP listening on port %u (client must send a datagram to connect)\n", cfg_.port);
        ioThread_ = std::thread(&Transport::IoThreadUdp, this);
    } else if (cfg_.mode != TransportMode::UsbTunnel) {
        ioThread_ = std::thread(&Transport::IoThreadTcp, this);
    }
    sendThread_ = std::thread(&Transport::SenderThread, this);
    if (cfg_.mode == TransportMode::UsbTunnel || (cfg_.mode == TransportMode::TcpListen && cfg_.enableUsb))
        usbThread_ = std::thread(&Transport::UsbThread, this);
    if ((cfg_.mode == TransportMode::TcpListen || cfg_.mode == TransportMode::Udp) && cfg_.discoveryPort != 0)
        discoveryThread_ = std::thread(&Transport::DiscoveryThread, this);
    return true;
}

// Answers unicast discovery probes ("who is a ScreenExt host?") with the stream port and PC name.
void Transport::DiscoveryThread()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { Logf(L"[net] Discovery socket failed: %d\n", WSAGetLastError()); return; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes), sizeof(yes));
    DWORD to = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&to), sizeof(to));
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(cfg_.discoveryPort);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        Logf(L"[net] Discovery: cannot bind UDP %u (%d) - auto-discovery disabled\n", cfg_.discoveryPort, WSAGetLastError());
        closesocket(s);
        return;
    }
    discoverySock_ = s;

    // Reply: magic, version, mode, streamPort, nameLen, name
    char name[48] = {};
    wchar_t wname[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD wlen = MAX_COMPUTERNAME_LENGTH + 1;
    int nameLen = 0;
    if (GetComputerNameW(wname, &wlen))
        nameLen = WideCharToMultiByte(CP_UTF8, 0, wname, (int)wlen, name, (int)sizeof(name), nullptr, nullptr);
    std::vector<uint8_t> reply(9 + (size_t)std::max(0, nameLen));
    const uint32_t magic = kReplyMagic;
    std::memcpy(&reply[0], &magic, 4);
    reply[4] = kVersion;
    reply[5] = (cfg_.mode == TransportMode::Udp) ? 1 : 0;
    std::memcpy(&reply[6], &cfg_.port, 2);
    reply[8] = (uint8_t)std::max(0, nameLen);
    if (nameLen > 0) std::memcpy(&reply[9], name, (size_t)nameLen);

    Logf(L"[net] Discovery responder listening on UDP %u (allow it in Windows Firewall)\n", cfg_.discoveryPort);

    std::set<uint32_t> seen;
    uint8_t buf[64];
    while (!stopping_) {
        const SOCKET cur = discoverySock_.load();
        if (cur == INVALID_SOCKET) break;
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(cur, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) {
            const int e = WSAGetLastError();
            if (stopping_ || e == WSAENOTSOCK || e == WSAEINTR) break;
            continue;   // timeout / ICMP reset: keep listening
        }
        uint32_t m = 0;
        if (n < 8) continue;
        std::memcpy(&m, buf, 4);
        if (m != kProbeMagic || buf[4] != kVersion) continue;

        sendto(cur, reinterpret_cast<const char*>(reply.data()), (int)reply.size(), 0,
               reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        if (seen.insert(from.sin_addr.s_addr).second) {
            char ip[32] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            Logf(L"[net] Discovery probe answered for %hs\n", ip);
        }
    }
}

void Transport::Stop()
{
    stopping_ = true;
    CloseSock(listenSock_);
    CloseSock(clientSock_);
    CloseSock(discoverySock_);
    qCv_.notify_all();
    if (ioThread_.joinable())   ioThread_.join();
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (usbThread_.joinable())  usbThread_.join();
    if (sendThread_.joinable()) sendThread_.join();
    connected_ = false;
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        queue_.clear();
    }
    if (wsaStarted_) { WSACleanup(); wsaStarted_ = false; }
}

TransportStats Transport::Stats() const
{
    std::lock_guard<std::mutex> lk(statsMutex_);
    return stats_;
}

// ---------------------------------------------------------------------------------------------
// Message building / queue
// ---------------------------------------------------------------------------------------------

Transport::Message Transport::BuildSimple(MsgType t, const void* payload, size_t n)
{
    auto m = std::make_shared<std::vector<uint8_t>>(sizeof(MsgHeader) + n);
    MsgHeader h{ kMagic, kVersion, (uint8_t)t, 0, (uint32_t)n };
    std::memcpy(m->data(), &h, sizeof(h));
    if (n) std::memcpy(m->data() + sizeof(h), payload, n);
    return m;
}

Transport::Message Transport::BuildHello()
{
    HelloPayload hp;
    {
        std::lock_guard<std::mutex> lk(qMutex_);   // geometry may be updated while streaming
        hp = { 1, cfg_.codedW, cfg_.codedH, cfg_.cropX, cfg_.cropY, cfg_.cropW, cfg_.cropH, cfg_.fps,
               (uint16_t)(viaUsb_ ? 1 : (cfg_.mode == TransportMode::Udp ? 2 : 0)) };
    }
    return BuildSimple(MSG_HELLO, &hp, sizeof(hp));
}

void Transport::UpdateGeometry(uint16_t codedW, uint16_t codedH, uint16_t cropW, uint16_t cropH)
{
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        cfg_.codedW = codedW;  cfg_.codedH = codedH;
        cfg_.cropX = 0;        cfg_.cropY = 0;
        cfg_.cropW = cropW;    cfg_.cropH = cropH;
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const QueueItem& i) { return i.video; }),
                     queue_.end());
        waitingForKey_ = true;   // the restarted encoder starts with an IDR; drop anything else until then
    }
    Logf(L"[net] Stream geometry now coded %ux%u, visible %ux%u\n", codedW, codedH, cropW, cropH);
    if (connected_) {
        Enqueue(BuildHello(), ctlSeq_++, false, true);   // client resets its decoder and crop on HELLO
        if (cb_.onKeyframeRequest) cb_.onKeyframeRequest();
    }
}

void Transport::Enqueue(Message m, uint32_t seq, bool video, bool front)
{
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        if (front) queue_.push_front({ std::move(m), seq, video });
        else       queue_.push_back({ std::move(m), seq, video });
    }
    qCv_.notify_one();
}

void Transport::OnConnected()
{
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        queue_.clear();
        waitingForKey_ = false;
    }
    connected_ = true;

    Enqueue(BuildHello(), ctlSeq_++, false, true);

    Logf(L"[net] Client connected via %ls\n", viaUsb_ ? L"USB" : (cfg_.mode == TransportMode::Udp ? L"UDP" : L"Wi-Fi/LAN"));
    if (cb_.onClientConnected) cb_.onClientConnected();   // -> request an IDR
}

void Transport::OnDisconnected()
{
    if (!connected_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        queue_.clear();
    }
    Logf(L"[net] Client disconnected\n");
    if (cb_.onClientDisconnected) cb_.onClientDisconnected();
}

void Transport::SendFrame(const EncodedFrame& f)
{
    if (!connected_) return;

    bool requestKey = false;
    bool discontinuity = false;
    {
        std::lock_guard<std::mutex> lk(qMutex_);

        size_t pendingVideo = 0;
        for (const auto& it : queue_) pendingVideo += it.video ? 1 : 0;

        if (pendingVideo >= cfg_.maxQueuedFrames) {
            // Client/link can't keep up: flush stale video, resume at the next IDR.
            const size_t before = queue_.size();
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const QueueItem& i) { return i.video; }),
                         queue_.end());
            {
                std::lock_guard<std::mutex> sl(statsMutex_);
                stats_.framesDropped += before - queue_.size();
                ++stats_.queueOverflows;
            }
            waitingForKey_ = true;
            // At most ONE IDR request per second. Previously every overflow requested an IDR, and the IDR
            // itself was often flushed by the next overflow before it could be sent: a stalled client
            // turned 133 overflows into 133 key frames in a 96 s run.
            const ULONGLONG nowMs = GetTickCount64();
            if (nowMs - lastOverflowKeyReqMs_ >= 1000) {
                lastOverflowKeyReqMs_ = nowMs;
                requestKey = true;
                std::lock_guard<std::mutex> sl(statsMutex_);
                ++stats_.hostKeyRequests;
            }
        }

        if (waitingForKey_ && !f.keyFrame) {
            std::lock_guard<std::mutex> sl(statsMutex_);
            ++stats_.framesDropped;
        } else {
            if (waitingForKey_ && f.keyFrame) { waitingForKey_ = false; discontinuity = true; }

            bool sps, pps;
            ScanParamSets(f.data, sps, pps);
            uint16_t flags = 0;
            if (f.keyFrame)  flags |= VF_KEYFRAME;
            if (sps)         flags |= VF_HAS_SPS;
            if (pps)         flags |= VF_HAS_PPS;
            if (discontinuity) flags |= VF_DISCONTINUITY;

            auto m = std::make_shared<std::vector<uint8_t>>(sizeof(MsgHeader) + sizeof(VideoHeader) + f.data.size());
            MsgHeader mh{ kMagic, kVersion, MSG_VIDEO, flags, (uint32_t)(sizeof(VideoHeader) + f.data.size()) };
            VideoHeader vh{ (uint32_t)f.outputIndex, (uint64_t)(f.sampleTime / 10),
                            cfg_.cropX, cfg_.cropY, cfg_.cropW, cfg_.cropH };
            std::memcpy(m->data(), &mh, sizeof(mh));
            std::memcpy(m->data() + sizeof(mh), &vh, sizeof(vh));
            std::memcpy(m->data() + sizeof(mh) + sizeof(vh), f.data.data(), f.data.size());
            queue_.push_back({ std::move(m), vh.frameNumber, true });
        }
    }
    qCv_.notify_one();

    if (requestKey && cb_.onKeyframeRequest) cb_.onKeyframeRequest();
}

// ---------------------------------------------------------------------------------------------
// Sender thread
// ---------------------------------------------------------------------------------------------

bool Transport::SendAllTcp(const uint8_t* p, size_t n)
{
    const SOCKET s = clientSock_.load();
    if (s == INVALID_SOCKET) return false;
    while (n > 0) {
        const int r = send(s, reinterpret_cast<const char*>(p), (int)std::min<size_t>(n, 1 << 20), 0);
        if (r <= 0) {
            const int e = WSAGetLastError();
            if (!stopping_)
                Logf(L"[net] TCP send %ls (WSA %d): dropping the client\n",
                     e == WSAETIMEDOUT ? L"timed out - client stopped reading" : L"failed", e);
            return false;
        }
        p += r; n -= (size_t)r;
    }
    return true;
}

void Transport::SendFragmentedUdp(const QueueItem& item)
{
    sockaddr_in peer;
    {
        std::lock_guard<std::mutex> lk(peerMutex_);
        peer = udpPeer_;
    }
    const SOCKET s = clientSock_.load();
    if (s == INVALID_SOCKET) return;

    const std::vector<uint8_t>& msg = *item.msg;
    const size_t chunk = cfg_.udpPayload;
    const size_t count = (msg.size() + chunk - 1) / chunk;
    if (count == 0 || count > 65535) return;

    std::vector<uint8_t> dgram(sizeof(UdpHeader) + chunk);
    const double secPerByte = 8.0 / (cfg_.udpMaxMbps * 1e6);
    const int64_t t0 = QpcNow();
    double budgetSec = 0;

    for (size_t i = 0; i < count && !stopping_; ++i) {
        const size_t off = i * chunk;
        const size_t len = std::min(chunk, msg.size() - off);
        UdpHeader uh{ kUdpMagic, 0, 0, item.seq, (uint16_t)i, (uint16_t)count };
        std::memcpy(dgram.data(), &uh, sizeof(uh));
        std::memcpy(dgram.data() + sizeof(uh), msg.data() + off, len);

        // Pace: don't dump a 1 MB IDR into the NIC/Wi-Fi in one burst.
        budgetSec += (double)(len + sizeof(uh)) * secPerByte;
        while ((double)(QpcNow() - t0) / (double)QpcFreq() < budgetSec - 0.0005)
            SwitchToThread();

        sendto(s, reinterpret_cast<const char*>(dgram.data()), (int)(sizeof(uh) + len), 0,
               reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
        std::lock_guard<std::mutex> sl(statsMutex_);
        ++stats_.datagramsSent;
    }
}

void Transport::SenderThread()
{
    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lk(qMutex_);
            qCv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!connected_) continue;

        if (cfg_.mode == TransportMode::Udp) {
            SendFragmentedUdp(item);
        } else if (!SendAllTcp(item.msg->data(), item.msg->size())) {
            // Make the receive loop notice and tear the session down.
            const SOCKET s = clientSock_.load();
            if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
            continue;
        }

        if (item.video) {
            std::lock_guard<std::mutex> sl(statsMutex_);
            ++stats_.framesSent;
            stats_.bytesSent += item.msg->size();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Incoming (client -> host)
// ---------------------------------------------------------------------------------------------

void Transport::HandleMessage(const MsgHeader& h, const uint8_t* payload)
{
    switch (h.type) {
    case MSG_KEYFRAME_REQ:
        { std::lock_guard<std::mutex> sl(statsMutex_); ++stats_.keyframeRequests; }
        if (cb_.onKeyframeRequest) cb_.onKeyframeRequest();
        break;

    case MSG_TOUCH:
        if (h.payloadLen == sizeof(TouchPayload)) {
            TouchPayload tp;
            std::memcpy(&tp, payload, sizeof(tp));
            { std::lock_guard<std::mutex> sl(statsMutex_); ++stats_.touchEvents; }
            if (cb_.onTouch && tp.phase <= 3)
                cb_.onTouch({ tp.pointerId, (TouchPhase)tp.phase, tp.x / 65535.0f, tp.y / 65535.0f, tp.pressure / 65535.0f });
        }
        break;

    case MSG_PEN:
        if (h.payloadLen == sizeof(PenPayload)) {
            PenPayload pp;
            std::memcpy(&pp, payload, sizeof(pp));
            { std::lock_guard<std::mutex> sl(statsMutex_); ++stats_.penEvents; }
            if (cb_.onPen && pp.phase <= 3) {
                PenEvent e;
                e.phase    = (TouchPhase)pp.phase;
                e.barrel   = (pp.flags & 1) != 0;
                e.eraser   = (pp.flags & 2) != 0;
                e.x        = pp.x / 65535.0f;
                e.y        = pp.y / 65535.0f;
                e.pressure = pp.pressure / 65535.0f;
                e.tiltX    = std::max(-90, std::min(90, (int)pp.tiltX));
                e.tiltY    = std::max(-90, std::min(90, (int)pp.tiltY));
                e.rotation = (int)(pp.rotation % 360);
                cb_.onPen(e);
            }
        }
        break;

    case MSG_PING:
        if (h.payloadLen == 8) {
            uint64_t out[2];
            std::memcpy(&out[0], payload, 8);
            out[1] = HostTimeUs();
            Enqueue(BuildSimple(MSG_PONG, out, sizeof(out)), ctlSeq_++, false, true);
        }
        break;

    default:
        break;   // unknown types are ignored (forward compatibility)
    }
}

// ---------------------------------------------------------------------------------------------
// IO threads
// ---------------------------------------------------------------------------------------------

void Transport::IoThreadTcp()
{
    if (cfg_.mode == TransportMode::TcpListen) {
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes), sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(cfg_.port);
        if (ls == INVALID_SOCKET || bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(ls, 1) != 0) {
            Logf(L"[net] TCP listen on port %u failed: %d\n", cfg_.port, WSAGetLastError());
            if (ls != INVALID_SOCKET) closesocket(ls);
            return;
        }
        listenSock_ = ls;
        Logf(L"[net] TCP listening on port %u\n", cfg_.port);
    } else {
        Logf(L"[net] TCP connect mode -> %hs:%u (retrying until the client app is listening)\n",
             cfg_.host.c_str(), cfg_.port);
    }

    while (!stopping_) {
        SOCKET c = INVALID_SOCKET;

        if (cfg_.mode == TransportMode::TcpListen) {
            c = accept(listenSock_.load(), nullptr, nullptr);
            if (c == INVALID_SOCKET) { if (stopping_) break; Sleep(50); continue; }
        } else {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
            const std::string port = std::to_string(cfg_.port);
            if (getaddrinfo(cfg_.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) { Sleep(500); continue; }
            c = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            // Remember the socket early so Stop() can abort a pending connect().
            clientSock_ = c;
            const int r = (c == INVALID_SOCKET) ? SOCKET_ERROR : connect(c, res->ai_addr, (int)res->ai_addrlen);
            freeaddrinfo(res);
            if (r != 0) { CloseSock(clientSock_); if (!stopping_) Sleep(500); continue; }
        }

        RunTcpSession(c, false);
    }
}

// One client session over an already-connected TCP-like socket: either an accepted/connected LAN socket, or
// the usbmuxd tunnel. Returns when the client goes away. Only one session can be active at a time.
void Transport::RunTcpSession(SOCKET c, bool viaUsb)
{
    std::unique_lock<std::timed_mutex> session(sessionMutex_, std::defer_lock);
    const bool locked = viaUsb ? session.try_lock_for(std::chrono::seconds(3))   // a USB tunnel REPLACES a Wi-Fi session:
                               : session.try_lock();                              // the app drops its Wi-Fi link when it arrives
    if (!locked) {
        Logf(L"[net] Rejected a second client: a session is already active\n");
        closesocket(c);
        return;
    }

    int one = 1, snd = 1 << 20;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&one), sizeof(one));
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&snd), sizeof(snd));
    // A client that stops reading (app frozen/suspended) must not block the sender for minutes:
    // fail the send after 5 s with no progress, and detect dead peers while idle via keep-alive.
    DWORD sndTimeoutMs = 5000;
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&sndTimeoutMs), sizeof(sndTimeoutMs));
    tcp_keepalive ka{ 1, 5000, 1000 };   // on, first probe after 5 s idle, then every 1 s
    DWORD ret = 0;
    WSAIoctl(c, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &ret, nullptr, nullptr);
    clientSock_ = c;
    viaUsb_ = viaUsb;
    OnConnected();

    // Receive loop: control messages from the client.
    std::vector<uint8_t> payload;
    while (!stopping_) {
        MsgHeader h;
        if (!RecvAll(c, reinterpret_cast<uint8_t*>(&h), sizeof(h))) break;
        if (h.magic != kMagic || h.version != kVersion || h.payloadLen > 65536) {
            Logf(L"[net] Bad message header from client; closing\n");
            break;
        }
        payload.resize(h.payloadLen);
        if (h.payloadLen && !RecvAll(c, payload.data(), h.payloadLen)) break;
        HandleMessage(h, payload.data());
    }

    OnDisconnected();
    CloseSock(clientSock_);
}

// Native replacement for iproxy: whenever no client is connected and an iPhone/iPad is plugged in, open a
// usbmuxd tunnel to the app's listening port and run the normal session over it.
void Transport::UsbThread()
{
    enum State { S_NONE, S_NOMUX, S_NODEVICE, S_NOAPP } last = S_NONE;
    Logf(L"[net] USB tunnel enabled (device port %u)\n", cfg_.usbDevicePort);

    auto nap = [this](int ms) { for (int i = 0; i < ms / 50 && !stopping_; ++i) Sleep(50); };

    while (!stopping_) {
        if (connected_ && viaUsb_) { nap(500); continue; }   // already on USB: nothing to do
        // Not connected, or connected over Wi-Fi: USB is preferred (lower latency), so keep looking for a device.

        std::vector<UsbDevice> devices;
        if (!UsbmuxdClient::ListDevices(devices)) {
            if (last != S_NOMUX) {
                Logf(L"[net] USB: Apple Mobile Device Service not reachable. Install iTunes or the \"Apple Devices\" app from the Microsoft Store.\n");
                last = S_NOMUX;
            }
            nap(3000);
            continue;
        }
        if (devices.empty()) {
            if (last != S_NODEVICE) { Logf(L"[net] USB: no iPhone/iPad connected\n"); last = S_NODEVICE; }
            nap(1000);
            continue;
        }

        bool ranSession = false;
        for (const UsbDevice& d : devices) {
            if (stopping_ || (connected_ && viaUsb_)) break;
            int result = -1;
            SOCKET s = UsbmuxdClient::ConnectToDevice(d.deviceId, cfg_.usbDevicePort, &result);
            if (s == INVALID_SOCKET) {
                if (last != S_NOAPP) {
                    Logf(L"[net] USB: device %hs found, but nothing is listening on its port %u yet (muxer result %d). Open the Screen Extension app on it.\n",
                         d.serial.c_str(), cfg_.usbDevicePort, result);
                    last = S_NOAPP;
                }
                continue;
            }
            Logf(L"[net] USB: tunnel to device %hs established%ls\n", d.serial.c_str(),
                 connected_ ? L" - switching from Wi-Fi to USB" : L"");
            last = S_NONE;
            RunTcpSession(s, true);                       // blocks until the session ends
            ranSession = true;
            break;
        }
        nap(ranSession ? 500 : 1000);
    }
}

void Transport::IoThreadUdp()
{
    std::vector<uint8_t> buf(2048);
    while (!stopping_) {
        const SOCKET s = clientSock_.load();
        if (s == INVALID_SOCKET) break;

        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(s, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);

        if (n < 0) {
            const int e = WSAGetLastError();
            if (e != WSAETIMEDOUT && e != WSAECONNRESET && !stopping_) { Sleep(10); }
            if (connected_ && GetTickCount64() - lastPeerTickMs_ > 5000) OnDisconnected();
            continue;
        }
        if ((size_t)n < sizeof(UdpHeader) + sizeof(MsgHeader)) continue;

        UdpHeader uh;
        std::memcpy(&uh, buf.data(), sizeof(uh));
        if (uh.magic != kUdpMagic || uh.kind != 1 || uh.fragCount != 1) continue;

        MsgHeader mh;
        std::memcpy(&mh, buf.data() + sizeof(uh), sizeof(mh));
        if (mh.magic != kMagic || mh.version != kVersion ||
            sizeof(uh) + sizeof(mh) + mh.payloadLen > (size_t)n) continue;

        lastPeerTickMs_ = GetTickCount64();
        bool newPeer = false;
        {
            std::lock_guard<std::mutex> lk(peerMutex_);
            if (!connected_ || from.sin_addr.s_addr != udpPeer_.sin_addr.s_addr || from.sin_port != udpPeer_.sin_port) {
                udpPeer_ = from;
                newPeer = true;
            }
        }
        if (newPeer) {
            if (connected_) OnDisconnected();
            OnConnected();
        }
        HandleMessage(mh, buf.data() + sizeof(uh) + sizeof(mh));
    }
}

} // namespace host
