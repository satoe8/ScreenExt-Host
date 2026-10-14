// CaptureEngine.h
// Phase 2 / Step 1: locate the IddSampleDriver virtual monitor, duplicate it with
// DXGI Desktop Duplication, and expose each frame in a pre-allocated, even-height
// (2556 x 1180) BGRA texture that is ready to be registered with NVENC.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace host {

// Wide-char printf-style logger (stdout). Defined in CaptureEngine.cpp.
void Logf(const wchar_t* fmt, ...);

// Also write log lines (with a time stamp) to a file; rotates at 8 MB. alsoConsole=false is for
// headless runs. Pass nullptr/empty to stop file logging.
void SetLogFile(const wchar_t* path, bool alsoConsole);

struct CaptureConfig {
    // width = height = 0  -> AUTO: match the output by `nameHint` (required) and adopt whatever
    //                        resolution it is running at right now, and follow later changes.
    // both non-zero       -> only accept an output of exactly that size.
    // The padded (even) size used for encoding is always derived: (w+1)&~1 x (h+1)&~1,
    // so 2556x1179 -> 2556x1180 while 2732x2048 stays 2732x2048.
    UINT width  = 0;
    UINT height = 0;

    // Optional case-insensitive substring. If non-empty, only outputs whose
    // (adapter description + monitor DeviceString + monitor DeviceID) contain it are accepted.
    // Example: L"DELD0E6" (PnP ID derived from the Dell EDID bytes in Driver.cpp monitor #0).
    std::wstring nameHint;

    // false: the extra row (index height) stays opaque black.
    // true : the last real row is copied into the extra row (avoids a black line / chroma
    //        bleed on the bottom edge of the encoded frame).
    bool replicateLastRow = false;
};

enum class FrameStatus {
    Ok,           // a new desktop image was copied into the padded texture
    NoNewFrame,   // only the mouse pointer / metadata changed; texture NOT updated
    Timeout,      // DXGI_ERROR_WAIT_TIMEOUT: desktop didn't change within the timeout
    AccessLost,   // duplication was invalidated (mode change, UAC/secure desktop, ...): call Recover()
    DeviceLost,   // D3D device removed/reset: call Recover() (device & texture will be rebuilt)
    Error         // unexpected failure (details are logged)
};

const wchar_t* ToString(FrameStatus s);

struct CapturedFrame {
    ID3D11Texture2D* texture = nullptr;   // padded BGRA texture, owned by CaptureEngine (do not Release)
    uint64_t         frameIndex = 0;      // counts frames with FrameStatus::Ok
    LARGE_INTEGER    presentTime{};       // DXGI_OUTDUPL_FRAME_INFO::LastPresentTime (QPC)
    UINT             accumulatedFrames = 0;
};

class CaptureEngine {
public:
    CaptureEngine() = default;
    ~CaptureEngine() { Shutdown(); }
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    // Finds the output, creates device/context/padded texture/duplication.
    bool Initialize(const CaptureConfig& cfg);
    void Shutdown();

    // Waits up to timeoutMs for a new desktop image and copies it (2556x1179) into the
    // top of the padded texture. The DXGI frame is always released before returning.
    FrameStatus AcquireNextFrame(UINT timeoutMs, CapturedFrame& out);

    // Call after AccessLost / DeviceLost (retry with a short sleep until it returns true).
    // Re-resolves the output; reuses the D3D device if it is still valid, otherwise rebuilds
    // device + padded texture and increments DeviceGeneration().
    bool Recover();

    ID3D11Device*        Device()  const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return ctx_.Get(); }
    ID3D11Texture2D*     PaddedTexture() const { return padded_.Get(); }
    LUID                 AdapterLuid() const { return adapterLuid_; }

    // GDI name of the duplicated output (e.g. L"\\\\.\\DISPLAY4"); thread-safe, updated by Recover().
    std::wstring OutputDeviceName() const { std::lock_guard<std::mutex> lk(nameMutex_); return outputName_; }

    // Bumps whenever the device (and therefore the padded texture) was recreated.
    // Anything registered against the texture (e.g. NVENC input resources) must be redone.
    uint32_t DeviceGeneration() const { return deviceGeneration_; }

    UINT Width() const { return cfg_.width; }          // visible (cropped) size of the duplicated output
    UINT Height() const { return cfg_.height; }
    UINT PaddedWidth() const { return paddedW_; }      // even-sized texture handed to the encoder
    UINT PaddedHeight() const { return paddedH_; }
    bool AutoSize() const { return autoSize_; }

    // Bumps whenever the visible/padded geometry changed (user switched resolution in Windows
    // Display Settings). The padded texture was recreated; the encoder must be restarted.
    uint32_t GeometryGeneration() const { return geometryGeneration_; }

private:
    struct Candidate;   // defined in the .cpp

    bool FindTargetOutput(Candidate& out);
    bool CreateDevice(IDXGIAdapter1* adapter);
    bool CreatePaddedTexture();
    bool CreateDuplication();
    void ApplyGeometry(UINT w, UINT h);

    CaptureConfig cfg_{};

    Microsoft::WRL::ComPtr<ID3D11Device>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    ctx_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        padded_;
    Microsoft::WRL::ComPtr<IDXGIOutput1>           output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl_;

    mutable std::mutex nameMutex_;
    std::wstring       outputName_;
    LUID     adapterLuid_{};
    uint32_t deviceGeneration_ = 0;
    uint32_t geometryGeneration_ = 0;
    UINT     paddedW_ = 0, paddedH_ = 0;
    bool     autoSize_ = false;
    uint64_t frameIndex_ = 0;
};

} // namespace host
