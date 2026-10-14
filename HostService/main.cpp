// main.cpp - Screen Extension host: a system-tray application (Win32 GUI subsystem, no console window).
//
// Build as a Windows-subsystem app (Linker > System > SubSystem = Windows); wWinMain below is the entry
// point. With no arguments it: follows the virtual monitor (EDID PnP id DELD0E6) at whatever resolution it
// runs, streams over Wi-Fi (TCP 5000 + LAN discovery on UDP 5001) AND over USB (native usbmuxd tunnel to port
// 5000 on a plugged-in iPhone/iPad), and shows a tray icon with "Status: ..." and "Quit".
//
// Options (all optional):
//   --console        attach to the launching console and log there (debugging)
//   --hint <text> | --width W --height H     which output to capture (default: hint DELD0E6, auto size)
//   --listen <port> | --connect host:port | --udp <port> | --usb | --no-usb | --no-net
//   --fps N  --bitrate <Mbps>  --gop N  --vbv-ms N  --min-qp N  --max-qp N  --no-cfr  --no-touch
//   --out <file>     also record the H.264 stream;  --seconds N  stop after N seconds (test runs)
//
// Logs: %LOCALAPPDATA%\ScreenExt\HostService.log.   Must run in the interactive user session.

#include "InputInjector.h"      // first: pulls in Transport.h (winsock2 before windows.h)
#include "Transport.h"
#include "CaptureEngine.h"
#include "HardwareEncoder.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace host;

static volatile LONG g_stop = 0;

// ---- status shared between the host worker thread and the tray UI ----
enum HostStatus { ST_STARTING = 0, ST_WAITING_MONITOR, ST_DISCONNECTED, ST_CONNECTED_WIFI, ST_CONNECTED_USB, ST_STOPPED };
constexpr UINT WM_TRAYICON    = WM_APP + 1;
constexpr UINT WM_HOST_STATUS = WM_APP + 2;
constexpr UINT WM_HOST_EXITED = WM_APP + 3;

static std::atomic<int> g_status{ ST_STARTING };
static HWND             g_trayHwnd = nullptr;
static std::wstring     g_logFilePath;

static void SetStatus(int s)
{
    g_status = s;
    if (g_trayHwnd) PostMessageW(g_trayHwnd, WM_HOST_STATUS, 0, 0);
}
static BOOL WINAPI OnCtrl(DWORD) { InterlockedExchange(&g_stop, 1); return TRUE; }

static double QpcNow()
{
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------------------------
// Bitstream inspection (Annex-B)
// ---------------------------------------------------------------------------------------------

struct Nal { int type; size_t size; const uint8_t* body; };

static std::vector<Nal> SplitNals(const std::vector<uint8_t>& d)
{
    std::vector<size_t> bodyStart, scLen;
    for (size_t i = 0; i + 3 <= d.size();) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)              { bodyStart.push_back(i + 3); scLen.push_back(3); i += 3; }
        else if (i + 4 <= d.size() && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1)
                                                                      { bodyStart.push_back(i + 4); scLen.push_back(4); i += 4; }
        else ++i;
    }
    std::vector<Nal> out;
    for (size_t k = 0; k < bodyStart.size(); ++k) {
        const size_t end = (k + 1 < bodyStart.size()) ? bodyStart[k + 1] - scLen[k + 1] : d.size();
        if (end > bodyStart[k])
            out.push_back({ d[bodyStart[k]] & 0x1F, end - bodyStart[k], &d[bodyStart[k]] });
    }
    return out;
}

static const wchar_t* NalName(int t)
{
    switch (t) {
    case 1: return L"P/B-slice"; case 5: return L"IDR"; case 6: return L"SEI";
    case 7: return L"SPS"; case 8: return L"PPS"; case 9: return L"AUD"; default: return L"other";
    }
}

static const wchar_t* ProfileName(int p)
{
    switch (p) { case 66: return L"Baseline"; case 77: return L"Main"; case 88: return L"Extended";
                 case 100: return L"High"; default: return L"?"; }
}

// ---------------------------------------------------------------------------------------------
// Sink: receives encoded frames on an MF worker thread
// ---------------------------------------------------------------------------------------------

struct Sink {
    FILE* file = nullptr;
    Transport* transport = nullptr;
    std::mutex m;
    std::vector<double> latencies;       // whole run (Submit entry -> frame ready)
    std::vector<double> encodes;         // whole run (ProcessInput -> frame ready)
    double winSum = 0, winMax = 0; unsigned winCount = 0;   // 1-second report window
    double winEncSum = 0; unsigned winEncCount = 0;
    uint64_t frames = 0, keyFrames = 0, bytes = 0, keyBytes = 0;
    bool sawSps = false, sawPps = false;

    void OnFrame(const EncodedFrame& f)
    {
        std::lock_guard<std::mutex> lk(m);
        if (file) fwrite(f.data.data(), 1, f.data.size(), file);
        if (transport) transport->SendFrame(f);   // non-blocking enqueue
        ++frames;
        bytes += f.data.size();
        if (f.keyFrame) { ++keyFrames; keyBytes += f.data.size(); }
        if (f.latencyMs >= 0) {
            latencies.push_back(f.latencyMs);
            if (f.encodeMs >= 0) encodes.push_back(f.encodeMs);
            winSum += f.latencyMs; winMax = std::max(winMax, f.latencyMs); ++winCount;
            if (f.encodeMs >= 0) { winEncSum += f.encodeMs; ++winEncCount; }
        }

        const auto nals = SplitNals(f.data);
        for (const auto& n : nals) {
            if (n.type == 7) {
                if (!sawSps && n.size >= 4) {
                    Logf(L"[main] SPS: profile_idc=%u (%ls) constraint=0x%02X level_idc=%u (Level %.1f)\n",
                         n.body[1], ProfileName(n.body[1]), n.body[2], n.body[3], n.body[3] / 10.0);
                }
                sawSps = true;
            }
            if (n.type == 8) sawPps = true;
        }
        if (f.outputIndex <= 3 || (f.keyFrame && f.outputIndex <= 200)) {
            std::wstring layout;
            for (const auto& n : nals) {
                wchar_t buf[64];
                swprintf(buf, 64, L" %ls(%zu)", NalName(n.type), n.size);
                layout += buf;
            }
            Logf(L"[main] out #%llu %ls t=%.1f ms latency=%.2f ms (encode %.2f ms) size=%zu NALs:%ls\n",
                 (unsigned long long)f.outputIndex, f.keyFrame ? L"KEY" : L"   ",
                 f.sampleTime / 10000.0, f.latencyMs, f.encodeMs, f.data.size(), layout.c_str());
        }
    }

    void TakeWindow(double& avg, double& mx, unsigned& n, double& encAvg)
    {
        std::lock_guard<std::mutex> lk(m);
        n = winCount; avg = winCount ? winSum / winCount : 0; mx = winMax;
        encAvg = winEncCount ? winEncSum / winEncCount : 0;
        winSum = winMax = 0; winCount = 0; winEncSum = 0; winEncCount = 0;
    }
};

static double Percentile(std::vector<double>& sorted, double p)
{
    if (sorted.empty()) return 0;
    const size_t idx = std::min(sorted.size() - 1, (size_t)(p * sorted.size()));
    return sorted[idx];
}

// ---------------------------------------------------------------------------------------------

// Log to %LOCALAPPDATA%\ScreenExt\HostService.log (and to the console when --console is used).
static void SetupLogging(bool alsoConsole)
{
    wchar_t base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(base) + L"\\ScreenExt" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_logFilePath = dir + L"\\HostService.log";
    SetLogFile(g_logFilePath.c_str(), alsoConsole);
}

// The whole capture -> encode -> network pipeline. Runs on a worker thread; returns 0 on a clean stop.
static int HostMain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // no-op if already set
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // Media Foundation needs an MTA

    CaptureConfig capCfg;
    EncoderConfig encCfg;
    double seconds = 10.0;
    bool cfr = true;
    std::wstring outPath;
    TransportConfig netCfg;
    bool useNet = false, secondsGiven = false, noTouch = false;

    // Tray-app defaults (every option can override them): Wi-Fi listener on TCP 5000 plus the native USB
    // tunnel, tuned encoder settings, daemon behaviour (keep retrying until the virtual monitor exists).
    bool headless = true;
    encCfg.vbvMs = 66;
    encCfg.minQp = 24;
    netCfg.mode = TransportMode::TcpListen;
    netCfg.port = 5000;
    netCfg.enableUsb = true;
    useNet = true;

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--hint") && i + 1 < argc)         capCfg.nameHint = argv[++i];
        else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) { seconds = _wtof(argv[++i]); secondsGiven = true; }
        else if (!wcscmp(argv[i], L"--fps") && i + 1 < argc)     encCfg.fps = (UINT)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--bitrate") && i + 1 < argc) encCfg.bitrateKbps = (UINT)(_wtof(argv[++i]) * 1000.0);
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc)     outPath = argv[++i];
        else if (!wcscmp(argv[i], L"--gop") && i + 1 < argc)     encCfg.gopSize = (UINT)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--vbv-ms") && i + 1 < argc)  encCfg.vbvMs = (UINT)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--min-qp") && i + 1 < argc)  encCfg.minQp = (UINT)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--max-qp") && i + 1 < argc)  encCfg.maxQp = (UINT)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--listen") && i + 1 < argc)  { netCfg.mode = TransportMode::TcpListen; netCfg.port = (uint16_t)_wtoi(argv[++i]); useNet = true; }
        else if (!wcscmp(argv[i], L"--udp") && i + 1 < argc)     { netCfg.mode = TransportMode::Udp;       netCfg.port = (uint16_t)_wtoi(argv[++i]); useNet = true; }
        else if (!wcscmp(argv[i], L"--connect") && i + 1 < argc) {
            // host:port (IPv4/hostname). For USB use an iproxy local port, e.g. --connect 127.0.0.1:5000
            std::wstring hp = argv[++i];
            const size_t colon = hp.rfind(L':');
            if (colon == std::wstring::npos) { Logf(L"--connect expects host:port\n"); return 1; }
            netCfg.host.assign(hp.begin(), hp.begin() + colon);
            netCfg.port = (uint16_t)_wtoi(hp.c_str() + colon + 1);
            netCfg.mode = TransportMode::TcpConnect;
            useNet = true;
        }
        else if (!wcscmp(argv[i], L"--width") && i + 1 < argc)   capCfg.width = (UINT)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--height") && i + 1 < argc)  capCfg.height = (UINT)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--headless") || !wcscmp(argv[i], L"--daemon")) headless = true;
        else if (!wcscmp(argv[i], L"--console"))                 headless = false;   // fail fast, chatty logging
        else if (!wcscmp(argv[i], L"--usb"))                     { netCfg.mode = TransportMode::UsbTunnel; useNet = true; }
        else if (!wcscmp(argv[i], L"--no-usb"))                  netCfg.enableUsb = false;
        else if (!wcscmp(argv[i], L"--no-net"))                  useNet = false;
        else if (!wcscmp(argv[i], L"--discovery-port") && i + 1 < argc) netCfg.discoveryPort = (uint16_t)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--no-cfr"))                  cfr = false;
        else if (!wcscmp(argv[i], L"--no-touch"))                noTouch = true;   // log touches only
    }

    // Default: follow the virtual monitor (matched by its EDID PnP id) at whatever resolution it runs.
    if (capCfg.nameHint.empty() && capCfg.width == 0 && capCfg.height == 0)
        capCfg.nameHint = L"DELD0E6";
    if (!useNet && outPath.empty()) outPath = L"test_capture.h264";
    if (useNet && !secondsGiven) seconds = 1e9;   // stream until Ctrl+C

    CaptureEngine engine;
    for (int attempt = 0; !engine.Initialize(capCfg); ++attempt) {
        if (g_stop) return 0;
        if (!headless) { Logf(L"Capture initialization failed.\n"); return 1; }
        // At logon the virtual monitor may not exist yet: keep retrying instead of exiting.
        SetStatus(ST_WAITING_MONITOR);
        Logf(L"[main] Virtual monitor not ready; retrying in %d s...\n", attempt < 3 ? 3 : 15);
        for (int w = 0, total = (attempt < 3 ? 30 : 150); w < total && !g_stop; ++w) Sleep(100);   // quit-aware
    }

    Sink sink;
    if (!outPath.empty() && (_wfopen_s(&sink.file, outPath.c_str(), L"wb") != 0 || !sink.file)) {
        Logf(L"Could not open %ls for writing\n", outPath.c_str());
        return 1;
    }

    Logf(L"[main] Capture geometry: visible %ux%u, padded %ux%u (%ls)\n", engine.Width(), engine.Height(),
         engine.PaddedWidth(), engine.PaddedHeight(), engine.AutoSize() ? L"auto-follow" : L"fixed");
    encCfg.width  = engine.PaddedWidth();
    encCfg.height = engine.PaddedHeight();

    SetStatus(ST_DISCONNECTED);
    encCfg.inputWaitMs = 4;   // never stall the capture loop waiting for the encoder (we check HasInputCredit first)
    HardwareEncoder encoder;
    auto startEncoder = [&]() {
        return encoder.Initialize(engine.Device(), engine.AdapterLuid(), encCfg,
                                  [&sink](const EncodedFrame& f) { sink.OnFrame(f); });
    };
    if (!startEncoder()) {
        Logf(L"Encoder initialization failed.\n");
        return 2;
    }

    // Touch -> Windows touch injection, mapped onto the virtual monitor's current desktop rectangle.
    InputInjector injector;
    bool injectorOk = false;
    if (useNet && !noTouch) {
        injectorOk = injector.Initialize([&engine](DesktopRect& r) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            const std::wstring name = engine.OutputDeviceName();   // e.g. \\.\DISPLAY4, re-read on every event
            if (name.empty() || !EnumDisplaySettingsExW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) return false;
            r.left = dm.dmPosition.x; r.top = dm.dmPosition.y;
            r.width = (LONG)dm.dmPelsWidth; r.height = (LONG)dm.dmPelsHeight;
            return true;
        });
    }

    Transport transport;
    if (useNet) {
        netCfg.codedW = (uint16_t)encoder.EncodeWidth();
        netCfg.codedH = (uint16_t)encoder.EncodeHeight();
        netCfg.cropX = 0; netCfg.cropY = 0;
        netCfg.cropW = (uint16_t)engine.Width();
        netCfg.cropH = (uint16_t)engine.Height();     // 1179: the visible part of the padded picture
        netCfg.fps = (uint16_t)encCfg.fps;

        TransportCallbacks cbs;
        cbs.onClientConnected = [&encoder, &transport] {
            encoder.RequestKeyFrame(true);
            SetStatus(transport.ConnectedViaUsb() ? ST_CONNECTED_USB : ST_CONNECTED_WIFI);
        };   // first frame for a new client = IDR
        cbs.onKeyframeRequest = [&encoder] { encoder.RequestKeyFrame(); };
        cbs.onClientDisconnected = [&injector, &injectorOk] {
            if (injectorOk) injector.ReleaseAll();
            SetStatus(ST_DISCONNECTED);
        };
        cbs.onTouch = [&injector, &injectorOk](const TouchEvent& t) {
            static unsigned moves = 0;
            if (t.phase != TouchPhase::Move || (++moves % 60) == 0)
                Logf(L"[main] touch id=%u phase=%d x=%.3f y=%.3f p=%.2f\n",
                     t.pointerId, (int)t.phase, t.x, t.y, t.pressure);
            if (injectorOk) injector.OnTouch(t);
        };
        if (!transport.Start(netCfg, cbs)) { Logf(L"Transport start failed.\n"); return 3; }
        sink.transport = &transport;
    }

    const double frameInterval = 1.0 / encCfg.fps;
    const UINT acquireTimeoutMs = cfr ? 4 : 100;

    const double start = QpcNow();
    double lastReport = start, lastSubmit = 0;
    bool haveFrame = false;
    bool dirty = false;           // a captured frame hasn't been submitted yet
    // After the desktop stops changing, only a few "settle" repeats are encoded (they let the encoder
    // refine the picture), then a slow heartbeat. 2473 repeats in a 96 s test were pure waste.
    const int    settleRepeats = 8;
    const double heartbeatSec = 0.5;
    int          repeatsSinceCapture = 0;
    uint64_t     noCreditSkips = 0, winNoCredit = 0;
    uint64_t captured = 0, repeats = 0, submitErrors = 0, consecutiveErrors = 0;
    uint64_t winCaptured = 0, winSubmitted = 0, winDropped = 0;
    uint64_t prevSubmitted = 0, prevDropped = 0, prevNetBytes = 0;

    while (!g_stop && (QpcNow() - start) < seconds) {
        CapturedFrame frame;
        const FrameStatus st = engine.AcquireNextFrame(acquireTimeoutMs, frame);
        bool submitNow = false;
        bool isRepeat = false;

        switch (st) {
        case FrameStatus::Ok:
            ++captured; ++winCaptured; haveFrame = true; dirty = true; repeatsSinceCapture = 0;
            if (!cfr) submitNow = true;
            break;

        case FrameStatus::NoNewFrame:
        case FrameStatus::Timeout:
            break;   // pacing / repeats are decided below

        case FrameStatus::AccessLost:
        case FrameStatus::DeviceLost: {
            const uint32_t genBefore = engine.DeviceGeneration();
            const uint32_t geoBefore = engine.GeometryGeneration();
            Logf(L"[main] %ls - recovering capture...\n", ToString(st));
            bool recovered = false;
            for (int attempt = 0; attempt < 50 && !g_stop; ++attempt) {
                if (engine.Recover()) { recovered = true; break; }
                Sleep(100);
            }
            if (!recovered) { Logf(L"[main] Could not recover capture; stopping.\n"); g_stop = 1; break; }
            const bool geometryChanged = engine.GeometryGeneration() != geoBefore;
            if (engine.DeviceGeneration() != genBefore || geometryChanged) {
                if (geometryChanged)
                    Logf(L"[main] Resolution changed: visible %ux%u, padded %ux%u - restarting the encoder\n",
                         engine.Width(), engine.Height(), engine.PaddedWidth(), engine.PaddedHeight());
                else
                    Logf(L"[main] Device rebuilt: restarting the encoder on the new device\n");
                encoder.Stop();                              // drains frames of the old geometry first
                encCfg.width  = engine.PaddedWidth();
                encCfg.height = engine.PaddedHeight();
                if (!startEncoder()) {
                    Logf(L"[main] Encoder restart failed; stopping.\n"); g_stop = 1;
                } else if (useNet) {
                    transport.UpdateGeometry((uint16_t)encoder.EncodeWidth(), (uint16_t)encoder.EncodeHeight(),
                                             (uint16_t)engine.Width(), (uint16_t)engine.Height());
                }
                haveFrame = false;
                dirty = false;
            }
            break;
        }

        case FrameStatus::Error:
            Logf(L"[main] Unrecoverable capture error; stopping.\n");
            g_stop = 1;
            break;
        }

        // Constant-frame-rate pacing: submit a fresh frame once ~one frame interval has passed since
        // the last submission (this also makes --fps 30 work on a 60 Hz virtual display), and repeat the
        // last frame if nothing changed for a full interval.
        if (cfr && haveFrame && !g_stop) {
            const double since = QpcNow() - lastSubmit;
            const double repeatInterval = (repeatsSinceCapture < settleRepeats) ? frameInterval : heartbeatSec;
            const bool due = dirty ? (since >= frameInterval * 0.85) : (since >= repeatInterval);
            if (due) {
                // Never queue behind the encoder: submit only when the MFT has asked for input. A slow encoder
                // (31 fps at 2732x2048) then simply takes the NEWEST frame whenever it is ready, instead of
                // being stuffed at 60 fps and hiding the backlog as latency.
                if (encoder.HasInputCredit()) { submitNow = true; isRepeat = !dirty; }
                else { ++noCreditSkips; ++winNoCredit; }
            }
        }

        // With a network target and nobody connected, don't burn GPU encoding into the void.
        if (submitNow && !g_stop && (!useNet || transport.IsClientConnected() || sink.file)) {
            const SubmitResult r = encoder.Submit(engine.PaddedTexture());
            lastSubmit = QpcNow();
            dirty = false;
            if (isRepeat) { ++repeats; ++repeatsSinceCapture; }
            if (r == SubmitResult::Error) {
                ++submitErrors;
                if (++consecutiveErrors >= 30) { Logf(L"[main] Too many encoder errors; stopping.\n"); g_stop = 1; }
            } else {
                consecutiveErrors = 0;
            }
        }

        const double now = QpcNow();
        if (now - lastReport >= 1.0) {
            double avg, mx, encAvg; unsigned n;
            sink.TakeWindow(avg, mx, n, encAvg);
            const uint64_t sub = encoder.Submitted(), drp = encoder.DroppedNoInput() + encoder.DroppedPoolEmpty();
            winSubmitted = sub - prevSubmitted; winDropped = drp - prevDropped;
            prevSubmitted = sub; prevDropped = drp;
            const bool quiet = headless && !transport.IsClientConnected();   // keep the daemon log small when idle
            if (!quiet)
                Logf(L"[main] %.1fs: captured %llu, submitted %llu, dropped %llu, no-credit %llu, encoded %u | latency avg %.2f ms (encode-only %.2f) max %.2f ms\n",
                     now - start, (unsigned long long)winCaptured, (unsigned long long)winSubmitted,
                     (unsigned long long)winDropped, (unsigned long long)winNoCredit, n, avg, encAvg, mx);
            winNoCredit = 0;
            if (useNet && !quiet) {
                const TransportStats ts = transport.Stats();
                Logf(L"[main]   net: client=%d sent %llu frames (%.1f Mbit/s), dropped %llu, overflows %llu, keyreq %llu, touches %llu\n",
                     (int)transport.IsClientConnected(), (unsigned long long)ts.framesSent,
                     (ts.bytesSent - prevNetBytes) * 8.0 / 1e6, (unsigned long long)ts.framesDropped,
                     (unsigned long long)ts.queueOverflows, (unsigned long long)ts.keyframeRequests,
                     (unsigned long long)ts.touchEvents);
                prevNetBytes = ts.bytesSent;
            }
            winCaptured = 0;
            lastReport = now;
        }
    }

    const double elapsed = QpcNow() - start;
    encoder.Stop();   // drains: remaining frames still go to the sink
    transport.Stop();
    if (injectorOk) injector.ReleaseAll();

    if (sink.file) { fclose(sink.file); sink.file = nullptr; }

    // ---- Report ----
    std::vector<double> lat, enc;
    {
        std::lock_guard<std::mutex> lk(sink.m);
        lat = sink.latencies;
        enc = sink.encodes;
    }
    std::sort(lat.begin(), lat.end());
    std::sort(enc.begin(), enc.end());

    Logf(L"\n===== Summary (%.2f s) =====\n", elapsed);
    Logf(L"Desktop frames captured : %llu (+%llu repeats for constant frame rate)\n",
         (unsigned long long)captured, (unsigned long long)repeats);
    Logf(L"Submitted to encoder    : %llu   dropped(no input request): %llu   dropped(pool empty): %llu   errors: %llu\n",
         (unsigned long long)encoder.Submitted(), (unsigned long long)encoder.DroppedNoInput(),
         (unsigned long long)encoder.DroppedPoolEmpty(), (unsigned long long)submitErrors);
    Logf(L"Skipped (encoder had no input credit): %llu\n", (unsigned long long)noCreditSkips);
    Logf(L"Encoded frames written  : %llu (%llu key frames), %.2f MB, avg %.2f Mbit/s\n",
         (unsigned long long)sink.frames, (unsigned long long)sink.keyFrames,
         sink.bytes / (1024.0 * 1024.0), elapsed > 0 ? sink.bytes * 8.0 / elapsed / 1e6 : 0.0);
    if (sink.keyFrames)
        Logf(L"Average key frame size  : %.1f KB\n", sink.keyBytes / (double)sink.keyFrames / 1024.0);
    Logf(L"SPS/PPS present in stream: %ls / %ls\n", sink.sawSps ? L"yes" : L"NO", sink.sawPps ? L"yes" : L"NO");
    if (!lat.empty()) {
        Logf(L"Encode latency (Submit -> encoded frame ready), %zu samples:\n", lat.size());
        Logf(L"   min %.2f | avg %.2f | p50 %.2f | p95 %.2f | p99 %.2f | max %.2f ms   (first frame: see log above)\n",
             lat.front(),
             [&] { double s = 0; for (double v : lat) s += v; return s / lat.size(); }(),
             Percentile(lat, 0.50), Percentile(lat, 0.95), Percentile(lat, 0.99), lat.back());
    }
    if (!enc.empty()) {
        Logf(L"Encode-only latency (ProcessInput -> frame ready, no wait/convert), %zu samples:\n", enc.size());
        Logf(L"   min %.2f | p50 %.2f | p95 %.2f | p99 %.2f | max %.2f ms\n",
             enc.front(), Percentile(enc, 0.50), Percentile(enc, 0.95), Percentile(enc, 0.99), enc.back());
    }
    if (useNet) {
        const TransportStats ts = transport.Stats();
        Logf(L"Network: %llu frames / %.2f MB sent, %llu dropped, %llu queue overflows, %llu client keyframe requests, %llu overflow-triggered, %llu touch events\n",
             (unsigned long long)ts.framesSent, ts.bytesSent / (1024.0 * 1024.0), (unsigned long long)ts.framesDropped,
             (unsigned long long)ts.queueOverflows, (unsigned long long)ts.keyframeRequests,
             (unsigned long long)ts.hostKeyRequests, (unsigned long long)ts.touchEvents);
    }
    Logf(L"Encoder surface %ux%u (input %ux%u). File: %ls\n",
         encoder.EncodeWidth(), encoder.EncodeHeight(), encCfg.width, encCfg.height,
         outPath.empty() ? L"(none)" : outPath.c_str());

    CoUninitialize();
    SetStatus(ST_STOPPED);
    return (sink.frames > 0 || g_stop) ? 0 : 4;
}


// =============================================================================================
// System tray + entry point
// =============================================================================================

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr UINT ID_TRAY     = 1;
constexpr UINT IDM_STATUS  = 1001;
constexpr UINT IDM_OPENLOG = 1002;
constexpr UINT IDM_QUIT    = 1003;

NOTIFYICONDATAW g_nid{};
HICON           g_icons[4] = {};   // 0 grey, 1 green, 2 orange, 3 red
bool            g_quitRequested = false;

const wchar_t* StatusText(int s)
{
    switch (s) {
    case ST_STARTING:        return L"Starting...";
    case ST_WAITING_MONITOR: return L"Waiting for the virtual display...";
    case ST_DISCONNECTED:    return L"Disconnected - waiting for your iPhone/iPad";
    case ST_CONNECTED_WIFI:  return L"Connected (Wi-Fi)";
    case ST_CONNECTED_USB:   return L"Connected (USB)";
    case ST_STOPPED:         return L"Stopped - see the log";
    }
    return L"";
}

// Draws a 16x16 anti-aliased dot in the given colour.
HICON MakeDotIcon(BYTE r, BYTE g, BYTE b)
{
    const int N = 16;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = N;
    bi.bV5Height = -N;                       // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(N, N, 1, 1, nullptr);
    if (!color || !mask || !bits) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    uint32_t* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float d = std::sqrt((x - 7.5f) * (x - 7.5f) + (y - 7.5f) * (y - 7.5f));
            const float a = std::min(1.0f, std::max(0.0f, 7.0f - d));      // soft edge
            const float shade = (d > 5.2f) ? 0.65f : 1.0f;                  // darker rim
            const uint32_t A = (uint32_t)(a * 255.0f);
            const uint32_t R = (uint32_t)(r * shade * a), G = (uint32_t)(g * shade * a), B = (uint32_t)(b * shade * a);
            px[y * N + x] = (A << 24) | (R << 16) | (G << 8) | B;          // premultiplied alpha
        }
    }
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

int IconIndexFor(int status)
{
    switch (status) {
    case ST_CONNECTED_WIFI:
    case ST_CONNECTED_USB:   return 1;   // green
    case ST_DISCONNECTED:    return 0;   // grey
    case ST_STOPPED:         return 3;   // red
    default:                 return 2;   // orange: starting / waiting for the monitor
    }
}

void UpdateTray(DWORD message)
{
    const int s = g_status.load();
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.hIcon = g_icons[IconIndexFor(s)];
    swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"Screen Extension - %ls", StatusText(s));
    Shell_NotifyIconW(message, &g_nid);
}

void ShowContextMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    const std::wstring label = std::wstring(L"Status: ") + StatusText(g_status.load());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, label.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPENLOG, L"Open log");
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);                    // required, or the menu won't close when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) {                  // Explorer restarted: put the icon back
        UpdateTray(NIM_ADD);
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP || lp == WM_CONTEXTMENU)
            ShowContextMenu(hwnd);
        return 0;

    case WM_HOST_STATUS:
        UpdateTray(NIM_MODIFY);
        return 0;

    case WM_HOST_EXITED: {
        const int rc = (int)wp;
        if (rc == 0 || g_quitRequested) { DestroyWindow(hwnd); return 0; }
        g_status = ST_STOPPED;                    // failed: keep the icon so the user sees it, with a balloon
        UpdateTray(NIM_MODIFY);
        g_nid.uFlags = NIF_INFO;
        g_nid.dwInfoFlags = NIIF_ERROR;
        wcscpy_s(g_nid.szInfoTitle, L"Screen Extension stopped");
        swprintf(g_nid.szInfo, ARRAYSIZE(g_nid.szInfo), L"The host stopped (error %d). Open the log from the tray menu.", rc);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDM_QUIT) {
            g_quitRequested = true;
            InterlockedExchange(&g_stop, 1);      // the worker notices, drains the encoder, closes sockets
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == IDM_OPENLOG && !g_logFilePath.empty()) {
            const std::wstring arg = L"\"" + g_logFilePath + L"\"";
            ShellExecuteW(nullptr, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    bool console = false;
    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--console")) console = true;
    if (console && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    SetupLogging(console);

    // One host per user session (autostart + a manual launch must not fight over the monitor and ports).
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ScreenExtHostService");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        Logf(L"[main] Already running in this session; exiting.\n");
        return 0;
    }

    // Hidden top-level window (not message-only: those don't receive the TaskbarCreated broadcast).
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ScreenExtTrayWindow";
    RegisterClassExW(&wc);
    g_trayHwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Screen Extension", WS_POPUP,
                                 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_trayHwnd) return 1;

    g_icons[0] = MakeDotIcon(150, 150, 150);
    g_icons[1] = MakeDotIcon(46, 184, 92);
    g_icons[2] = MakeDotIcon(240, 170, 30);
    g_icons[3] = MakeDotIcon(210, 60, 60);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_trayHwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uCallbackMessage = WM_TRAYICON;
    UpdateTray(NIM_ADD);

    // The host pipeline runs on a worker; this thread only pumps messages.
    std::thread worker([argc, argv] {
        const int rc = HostMain(argc, argv);
        PostMessageW(g_trayHwnd, WM_HOST_EXITED, (WPARAM)rc, 0);
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Quit: let the worker drain the encoder and close the sockets, but never hang the exit.
    InterlockedExchange(&g_stop, 1);
    if (WaitForSingleObject(reinterpret_cast<HANDLE>(worker.native_handle()), 8000) == WAIT_TIMEOUT) {
        Logf(L"[main] Worker did not stop within 8 s; exiting anyway\n");
        worker.detach();
        ExitProcess(0);
    }
    worker.join();
    return 0;
}
