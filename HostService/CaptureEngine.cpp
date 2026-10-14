// CaptureEngine.cpp
#include "CaptureEngine.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace host {

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

namespace {
std::mutex   g_logMutex;
FILE*        g_logFile = nullptr;
std::wstring g_logPath;
bool         g_logConsole = true;
long         g_logBytes = 0;
constexpr long kMaxLogBytes = 8 * 1024 * 1024;

void OpenLogLocked()
{
    g_logFile = nullptr;
    if (g_logPath.empty()) return;
    _wfopen_s(&g_logFile, g_logPath.c_str(), L"a, ccs=UTF-8");
    g_logBytes = 0;
    if (g_logFile) {
        fseek(g_logFile, 0, SEEK_END);
        g_logBytes = ftell(g_logFile);
    }
}
} // namespace

void SetLogFile(const wchar_t* path, bool alsoConsole)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
    g_logConsole = alsoConsole;
    g_logPath = (path && *path) ? path : L"";
    OpenLogLocked();
}

void Logf(const wchar_t* fmt, ...)
{
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (g_logConsole) {
            std::vfwprintf(stdout, fmt, args);
            std::fflush(stdout);
        }
        if (g_logFile) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            int w = std::fwprintf(g_logFile, L"%02u:%02u:%02u ", st.wHour, st.wMinute, st.wSecond);
            int w2 = std::vfwprintf(g_logFile, fmt, args2);
            std::fflush(g_logFile);
            g_logBytes += (w > 0 ? w : 0) + (w2 > 0 ? w2 : 0);
            if (g_logBytes > kMaxLogBytes) {            // simple rotation: keep one old file
                fclose(g_logFile);
                g_logFile = nullptr;
                MoveFileExW(g_logPath.c_str(), (g_logPath + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
                OpenLogLocked();
            }
        }
    }
    va_end(args2);
    va_end(args);
}

const wchar_t* ToString(FrameStatus s)
{
    switch (s) {
    case FrameStatus::Ok:         return L"Ok";
    case FrameStatus::NoNewFrame: return L"NoNewFrame";
    case FrameStatus::Timeout:    return L"Timeout";
    case FrameStatus::AccessLost: return L"AccessLost";
    case FrameStatus::DeviceLost: return L"DeviceLost";
    case FrameStatus::Error:      return L"Error";
    }
    return L"?";
}

static const wchar_t* HrName(HRESULT hr)
{
    switch (hr) {
    case E_ACCESSDENIED:                       return L"E_ACCESSDENIED (secure desktop/UAC, or not allowed to duplicate)";
    case DXGI_ERROR_UNSUPPORTED:               return L"DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:   return L"DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (too many duplications active)";
    case DXGI_ERROR_SESSION_DISCONNECTED:      return L"DXGI_ERROR_SESSION_DISCONNECTED";
    case DXGI_ERROR_ACCESS_LOST:               return L"DXGI_ERROR_ACCESS_LOST";
    case DXGI_ERROR_WAIT_TIMEOUT:              return L"DXGI_ERROR_WAIT_TIMEOUT";
    case DXGI_ERROR_DEVICE_REMOVED:            return L"DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET:              return L"DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_INVALID_CALL:              return L"DXGI_ERROR_INVALID_CALL";
    case E_INVALIDARG:                         return L"E_INVALIDARG";
    case E_OUTOFMEMORY:                        return L"E_OUTOFMEMORY";
    default:                                   return L"(unmapped HRESULT)";
    }
}

static std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

static bool SameLuid(const LUID& a, const LUID& b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

// ---------------------------------------------------------------------------------------------
// Output discovery
// ---------------------------------------------------------------------------------------------

struct CaptureEngine::Candidate {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1>  output1;
    LUID                  luid{};
    UINT                  width = 0, height = 0;   // desktop size of this output right now
    std::wstring          adapterName;
    std::wstring          outputName;    // \\.\DISPLAYn
    std::wstring          monitorString; // EnumDisplayDevices DeviceString
    std::wstring          monitorId;     // EnumDisplayDevices DeviceID (MONITOR\XXXYYYY\{guid}\nnnn)
};

bool CaptureEngine::FindTargetOutput(Candidate& result)
{
    // A fresh factory every call: DXGI caches the adapter/output list on a factory, so a
    // factory created before a monitor was (re)plugged would return stale results.
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Logf(L"[capture] CreateDXGIFactory1 failed: 0x%08X %ls\n", (unsigned)hr, HrName(hr));
        return false;
    }

    const std::wstring hint = ToLower(cfg_.nameHint);
    std::vector<Candidate> matches;

    if (autoSize_)
        Logf(L"[capture] Enumerating DXGI adapters/outputs (AUTO size, hint=%ls)\n", cfg_.nameHint.c_str());
    else
        Logf(L"[capture] Enumerating DXGI adapters/outputs (looking for %ux%u%ls%ls)\n",
             cfg_.width, cfg_.height,
             hint.empty() ? L"" : L", hint=", hint.empty() ? L"" : cfg_.nameHint.c_str());

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        Logf(L"[capture]  adapter %u: \"%ls\" LUID=%08lX:%08lX flags=0x%X vendor=0x%04X\n",
             a, ad.Description, (unsigned long)ad.AdapterLuid.HighPart, (unsigned long)ad.AdapterLuid.LowPart,
             ad.Flags, ad.VendorId);

        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            if (FAILED(output->GetDesc(&od)))
                continue;

            const LONG w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            const LONG h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;

            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            std::wstring monStr, monId;
            if (EnumDisplayDevicesW(od.DeviceName, 0, &dd, 0)) {
                monStr = dd.DeviceString;
                monId  = dd.DeviceID;
            }

            Logf(L"[capture]    output %u: %ls  %ldx%ld @(%ld,%ld) attached=%d rot=%d monitor=\"%ls\" id=%ls\n",
                 o, od.DeviceName, w, h, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                 (int)od.AttachedToDesktop, (int)od.Rotation, monStr.c_str(), monId.c_str());

            if (!od.AttachedToDesktop)
                continue;
            if (w <= 0 || h <= 0)
                continue;
            if (!autoSize_ && ((UINT)w != cfg_.width || (UINT)h != cfg_.height))
                continue;

            if (!hint.empty()) {
                const std::wstring hay = ToLower(std::wstring(ad.Description) + L" " + monStr + L" " + monId);
                if (hay.find(hint) == std::wstring::npos)
                    continue;
            }

            Candidate c;
            c.adapter = adapter;
            if (FAILED(output.As(&c.output1)))
                continue;   // needs IDXGIOutput1 (Windows 8+)
            c.luid          = ad.AdapterLuid;
            c.width         = (UINT)w;
            c.height        = (UINT)h;
            c.adapterName   = ad.Description;
            c.outputName    = od.DeviceName;
            c.monitorString = monStr;
            c.monitorId     = monId;
            matches.push_back(std::move(c));
        }
    }

    if (matches.empty()) {
        if (autoSize_)
            Logf(L"[capture] No attached output matched hint \"%ls\". Is the virtual monitor enabled/extended in Display Settings?\n",
                 cfg_.nameHint.c_str());
        else
            Logf(L"[capture] No attached output matched %ux%u%ls. Is the virtual monitor enabled/extended "
                 L"in Display Settings at that resolution? (use --width 0 --height 0 to follow its current resolution)\n",
                 cfg_.width, cfg_.height, hint.empty() ? L"" : L" + hint");
        return false;
    }
    if (matches.size() > 1)
        Logf(L"[capture] %zu outputs matched; using the first. Pass a name hint to disambiguate.\n", matches.size());

    result = std::move(matches.front());
    Logf(L"[capture] Selected %ls on adapter \"%ls\" (monitor id %ls)\n",
         result.outputName.c_str(), result.adapterName.c_str(), result.monitorId.c_str());
    return true;
}

// ---------------------------------------------------------------------------------------------
// Device / texture / duplication
// ---------------------------------------------------------------------------------------------

bool CaptureEngine::CreateDevice(IDXGIAdapter1* adapter)
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};

    // VIDEO_SUPPORT is required by ID3D11VideoProcessor (BGRA->NV12) and by Media Foundation
    // hardware MFTs that share this device through IMFDXGIDeviceManager.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = E_FAIL;

#ifdef _DEBUG
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags | D3D11_CREATE_DEVICE_DEBUG,
                           levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                           device_.ReleaseAndGetAddressOf(), &got, ctx_.ReleaseAndGetAddressOf());
    // Falls through to the non-debug path if the SDK layers aren't installed.
#endif
    if (FAILED(hr)) {
        // The device MUST be created on the adapter that owns the duplicated output.
        // (D3D_DRIVER_TYPE_UNKNOWN is required when passing an explicit adapter.)
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               device_.ReleaseAndGetAddressOf(), &got, ctx_.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        Logf(L"[capture] D3D11CreateDevice failed: 0x%08X %ls\n", (unsigned)hr, HrName(hr));
        return false;
    }

    // The encoder will touch this device from another thread; make the immediate context safe.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx_.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    Logf(L"[capture] D3D11 device created (feature level 0x%X)\n", (unsigned)got);
    return true;
}

bool CaptureEngine::CreatePaddedTexture()
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width            = paddedW_;
    d.Height           = paddedH_;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET is needed for the one-time clear below; SHADER_RESOURCE keeps options open
    // (e.g. an RGB->NV12 pass) if we don't hand the ARGB surface to NVENC directly.
    // We'll confirm the exact bind flags NVENC wants when we register the resource.
    d.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&d, nullptr, padded_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        Logf(L"[capture] CreateTexture2D(%ux%u) failed: 0x%08X %ls\n", d.Width, d.Height, (unsigned)hr, HrName(hr));
        return false;
    }

    // Clear once to opaque black. Row [height] is never written by the per-frame copy
    // (unless replicateLastRow is set), so it stays black.
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device_->CreateRenderTargetView(padded_.Get(), nullptr, &rtv);
    if (FAILED(hr)) {
        Logf(L"[capture] CreateRenderTargetView failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    const float black[4] = { 0.f, 0.f, 0.f, 1.f };
    ctx_->ClearRenderTargetView(rtv.Get(), black);

    Logf(L"[capture] Padded texture %ux%u B8G8R8A8_UNORM allocated\n", d.Width, d.Height);
    return true;
}

bool CaptureEngine::CreateDuplication()
{
    dupl_.Reset();

    HRESULT hr = E_FAIL;

    // Prefer DuplicateOutput1 (Win10 1803+): lets us restrict the returned format to
    // B8G8R8A8_UNORM so an HDR/FP16 desktop is converted for us rather than failing later.
    ComPtr<IDXGIOutput5> out5;
    if (SUCCEEDED(output_.As(&out5))) {
        const DXGI_FORMAT fmts[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
        hr = out5->DuplicateOutput1(device_.Get(), 0, ARRAYSIZE(fmts), fmts, dupl_.ReleaseAndGetAddressOf());
        if (FAILED(hr))
            Logf(L"[capture] DuplicateOutput1 failed: 0x%08X %ls - falling back to DuplicateOutput\n",
                 (unsigned)hr, HrName(hr));
    }
    if (FAILED(hr)) {
        hr = output_->DuplicateOutput(device_.Get(), dupl_.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        Logf(L"[capture] DuplicateOutput failed: 0x%08X %ls\n", (unsigned)hr, HrName(hr));
        dupl_.Reset();
        return false;
    }

    DXGI_OUTDUPL_DESC dd{};
    dupl_->GetDesc(&dd);
    Logf(L"[capture] Duplication ready: %ux%u fmt=%d rot=%d inSystemMemory=%d\n",
         dd.ModeDesc.Width, dd.ModeDesc.Height, (int)dd.ModeDesc.Format, (int)dd.Rotation,
         (int)dd.DesktopImageInSystemMemory);

    if (dd.ModeDesc.Width != cfg_.width || dd.ModeDesc.Height != cfg_.height) {
        Logf(L"[capture] Output mode is %ux%u, expected %ux%u\n",
             dd.ModeDesc.Width, dd.ModeDesc.Height, cfg_.width, cfg_.height);
        dupl_.Reset();
        return false;
    }
    if (dd.DesktopImageInSystemMemory) {
        // In that case the image is accessed via MapDesktopSurface, not as a GPU texture.
        Logf(L"[capture] Desktop image is in system memory; the GPU-texture path doesn't support that.\n");
        dupl_.Reset();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

bool CaptureEngine::Initialize(const CaptureConfig& cfg)
{
    Shutdown();
    cfg_ = cfg;

    if ((cfg_.width == 0) != (cfg_.height == 0)) {
        Logf(L"[capture] Invalid config: width and height must both be 0 (auto) or both non-zero\n");
        return false;
    }
    autoSize_ = (cfg_.width == 0);
    if (autoSize_ && cfg_.nameHint.empty()) {
        Logf(L"[capture] Auto size needs a name hint to identify the virtual monitor (e.g. DELD0E6)\n");
        return false;
    }
    if (!autoSize_)
        ApplyGeometry(cfg_.width, cfg_.height);   // fixed size: derive padded size now
    return Recover();
}

void CaptureEngine::Shutdown()
{
    dupl_.Reset();
    output_.Reset();
    padded_.Reset();
    if (ctx_)
        ctx_->ClearState();
    ctx_.Reset();
    device_.Reset();
}

void CaptureEngine::ApplyGeometry(UINT w, UINT h)
{
    cfg_.width  = w;
    cfg_.height = h;
    paddedW_ = (w + 1) & ~1u;   // 4:2:0 encoders need even dimensions
    paddedH_ = (h + 1) & ~1u;
    ++geometryGeneration_;
    Logf(L"[capture] Geometry: visible %ux%u, padded %ux%u\n", w, h, paddedW_, paddedH_);
}

bool CaptureEngine::Recover()
{
    dupl_.Reset();

    Candidate c;
    if (!FindTargetOutput(c))
        return false;

    // The user may have switched resolution in Windows Display Settings: adopt the new size.
    const bool geometryChanged = (c.width != cfg_.width || c.height != cfg_.height);
    if (geometryChanged)
        ApplyGeometry(c.width, c.height);

    const bool reuseDevice =
        device_ && SameLuid(c.luid, adapterLuid_) && device_->GetDeviceRemovedReason() == S_OK;

    if (reuseDevice && geometryChanged) {
        padded_.Reset();                       // new size -> new padded texture on the same device
        if (!CreatePaddedTexture()) { Shutdown(); return false; }
    }

    if (!reuseDevice) {
        padded_.Reset();
        ctx_.Reset();
        device_.Reset();
        if (!CreateDevice(c.adapter.Get()) || !CreatePaddedTexture()) {
            Shutdown();
            return false;
        }
        adapterLuid_ = c.luid;
        ++deviceGeneration_;
        Logf(L"[capture] Device generation is now %u (re-register any encoder resources)\n", deviceGeneration_);
    }

    output_ = c.output1;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        outputName_ = c.outputName;
    }
    return CreateDuplication();
}

// ---------------------------------------------------------------------------------------------
// Frame acquisition
// ---------------------------------------------------------------------------------------------

FrameStatus CaptureEngine::AcquireNextFrame(UINT timeoutMs, CapturedFrame& out)
{
    if (!dupl_ || !device_)
        return FrameStatus::AccessLost;   // nothing to acquire from; caller should Recover()

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, resource.ReleaseAndGetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return FrameStatus::Timeout;   // normal when the desktop is static; nothing to release

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        Logf(L"[capture] AcquireNextFrame: access lost (mode change / secure desktop / session switch)\n");
        dupl_.Reset();
        return FrameStatus::AccessLost;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Logf(L"[capture] AcquireNextFrame: device lost 0x%08X (reason 0x%08X)\n",
             (unsigned)hr, (unsigned)device_->GetDeviceRemovedReason());
        dupl_.Reset();
        return FrameStatus::DeviceLost;
    }
    if (FAILED(hr)) {
        Logf(L"[capture] AcquireNextFrame failed: 0x%08X %ls\n", (unsigned)hr, HrName(hr));
        return FrameStatus::Error;
    }

    // From here a frame is held and MUST be released exactly once on every path.
    FrameStatus status = FrameStatus::Ok;

    if (info.LastPresentTime.QuadPart == 0) {
        // Pointer-only update: the desktop image didn't change.
        status = FrameStatus::NoNewFrame;
    } else {
        ComPtr<ID3D11Texture2D> src;
        hr = resource.As(&src);
        if (FAILED(hr)) {
            Logf(L"[capture] Desktop resource is not an ID3D11Texture2D: 0x%08X\n", (unsigned)hr);
            status = FrameStatus::Error;
        } else {
            D3D11_TEXTURE2D_DESC sd{};
            src->GetDesc(&sd);

            if (sd.Width != cfg_.width || sd.Height != cfg_.height) {
                Logf(L"[capture] Frame is %ux%u, expected %ux%u (mode changed) - recover required\n",
                     sd.Width, sd.Height, cfg_.width, cfg_.height);
                status = FrameStatus::AccessLost;
            } else if (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                Logf(L"[capture] Unexpected desktop format %d (wanted B8G8R8A8_UNORM)\n", (int)sd.Format);
                status = FrameStatus::Error;
            } else {
                // Copy the 2556x1179 desktop into the top of the 2556x1180 texture.
                D3D11_BOX box{};
                box.left = 0;  box.top = 0;  box.front = 0;
                box.right = cfg_.width;  box.bottom = cfg_.height;  box.back = 1;
                ctx_->CopySubresourceRegion(padded_.Get(), 0, 0, 0, 0, src.Get(), 0, &box);

                if (cfg_.replicateLastRow && paddedH_ > cfg_.height) {
                    D3D11_BOX last{};
                    last.left = 0;  last.right = cfg_.width;
                    last.top = cfg_.height - 1;  last.bottom = cfg_.height;
                    last.front = 0;  last.back = 1;
                    ctx_->CopySubresourceRegion(padded_.Get(), 0, 0, cfg_.height, 0, src.Get(), 0, &last);
                }

                out.texture           = padded_.Get();
                out.frameIndex        = ++frameIndex_;
                out.presentTime       = info.LastPresentTime;
                out.accumulatedFrames = info.AccumulatedFrames;
            }
        }
    }

    // Always release the frame; the copy above has already been queued on the immediate context.
    resource.Reset();
    const HRESULT rhr = dupl_->ReleaseFrame();
    if (rhr == DXGI_ERROR_ACCESS_LOST) {
        // The copy we issued is still valid; just make sure the next call asks for recovery.
        dupl_.Reset();
    } else if (FAILED(rhr)) {
        Logf(L"[capture] ReleaseFrame failed: 0x%08X %ls\n", (unsigned)rhr, HrName(rhr));
    }

    if (status == FrameStatus::AccessLost)
        dupl_.Reset();
    return status;
}

} // namespace host
