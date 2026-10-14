// HardwareEncoder.h
// Phase 2 / Step 3: zero-copy GPU H.264 encoding through a Media Foundation hardware MFT
// (Intel QuickSync / AMD / NVIDIA MFT - whichever owns the capture device's adapter).
//
//   padded BGRA texture --(ID3D11VideoProcessor, GPU)--> NV12 texture from the MFT's own
//   IMFVideoSampleAllocatorEx --(IMFTransform::ProcessInput)--> Annex-B H.264 via callback
//
// Threading: Submit() is called from your capture thread. The FrameCallback is invoked on a
// Media Foundation worker thread (one event at a time). Keep the callback short.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>
#include <strmif.h>
#include <codecapi.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace host {

struct EncoderConfig {
    UINT width  = 2556;       // size of the BGRA texture handed to Submit() (the padded one)
    UINT height = 1180;
    UINT fps    = 60;
    UINT bitrateKbps = 20000;
    UINT gopSize     = 36000; // 0 would mean "encoder decides", so use a very long GOP (10 min @60);
                              // IDRs then come only from RequestKeyFrame() (client connect / loss).
    UINT vbvMs       = 66;    // HRD buffer in ms of bitrate (H.264: CODECAPI_AVEncCommonBufferSize is in BYTES).
                              // Smaller = smaller IDRs but softer first frame. 0 = leave to encoder.
    UINT minQp       = 0;     // 0 = unset. Raising it (e.g. 24) caps IDR quality/size (AVEncVideoMinQP).
    UINT maxQp       = 0;     // 0 = unset (AVEncVideoMaxQP).
    UINT qualityVsSpeed = 50; // CODECAPI_AVEncCommonQualityVsSpeed, 0 (fastest) .. 100 (best)
    UINT inputWaitMs = 20;    // how long Submit() waits for the MFT to ask for input before dropping
    UINT poolInitial = 4;     // NV12 sample pool (initial/max) owned by the MFT's allocator
    UINT poolMax     = 8;

    // If the MFT rejects the exact size (e.g. 2556x1180), retry with 16-aligned dimensions
    // (2560x1184). The picture is placed at the top-left; the client must crop either way.
    bool allowAlignedFallback = true;
};

struct EncodedFrame {
    std::vector<uint8_t> data;   // Annex-B access unit (start codes), as emitted by the MFT
    bool      keyFrame    = false;
    LONGLONG  sampleTime  = 0;   // 100 ns units, = frameNumber * 1e7 / fps
    uint64_t  outputIndex = 0;   // 1-based count of frames produced by the encoder
    double    latencyMs   = -1;  // Submit() entry -> encoded frame available (-1 if unknown)
    double    encodeMs    = -1;  // ProcessInput() call -> encoded frame available (MFT + event hop only)
};

using FrameCallback = std::function<void(const EncodedFrame&)>;

enum class SubmitResult {
    Submitted,
    DroppedNoInputRequest,  // MFT didn't ask for input in time (backpressure) - frame dropped
    DroppedPoolEmpty,       // all pooled NV12 samples still in flight - frame dropped
    Stopped,                // encoder not running / draining
    Error                   // logged
};
const wchar_t* ToString(SubmitResult r);

class EventCallback;   // IMFAsyncCallback implementation (HardwareEncoder.cpp)

class HardwareEncoder {
public:
    HardwareEncoder() = default;
    ~HardwareEncoder() { Stop(); }
    HardwareEncoder(const HardwareEncoder&) = delete;
    HardwareEncoder& operator=(const HardwareEncoder&) = delete;

    // `device` must be the D3D11 device that owns the source textures, created with
    // D3D11_CREATE_DEVICE_VIDEO_SUPPORT (the video processor + MFT both need it).
    // `adapterLuid` selects the hardware MFT that lives on the same GPU.
    // Requires COM to be initialized as MTA on the calling thread.
    bool Initialize(ID3D11Device* device, LUID adapterLuid, const EncoderConfig& cfg, FrameCallback cb);

    // Convert + enqueue one frame. `bgra` must be B8G8R8A8_UNORM, cfg.width x cfg.height, with
    // D3D11_BIND_SHADER_RESOURCE. Must not be called concurrently with Stop().
    SubmitResult Submit(ID3D11Texture2D* bgra);

    // Next submitted frame is coded as an IDR. Non-forced requests are coalesced: ignored if an IDR was
    // requested/produced within the last 500 ms (client PLI + overflow recovery tend to arrive together).
    void RequestKeyFrame(bool force = false);

    // True if the MFT has asked for another input frame, i.e. Submit() would not have to wait.
    bool HasInputCredit();

    // Drains the encoder (remaining frames are delivered through the callback), then releases
    // everything. Safe to call more than once.
    void Stop();

    bool     IsRunning() const { return running_; }
    UINT     EncodeWidth() const { return encW_; }
    UINT     EncodeHeight() const { return encH_; }
    const std::wstring& MftName() const { return mftName_; }

    uint64_t Submitted() const { return submitted_; }
    uint64_t DroppedNoInput() const { return droppedNoInput_; }
    uint64_t DroppedPoolEmpty() const { return droppedPool_; }
    uint64_t Produced() const { return produced_; }

private:
    friend class EventCallback;

    bool TryStart(UINT encW, UINT encH);
    bool CreateMft(LUID luid);
    bool ConfigureCodecApi();
    bool ConfigureTypes(UINT encW, UINT encH);
    bool CreateVideoProcessor(UINT encW, UINT encH);
    bool CreateAllocator();
    void TearDown();

    bool ConvertInto(ID3D11Texture2D* src, IMFSample* dstSample);
    void OnEvent(IMFAsyncResult* result);
    void ProcessOneOutput();

    EncoderConfig cfg_{};
    FrameCallback cb_;
    LUID          luid_{};
    UINT          encW_ = 0, encH_ = 0;
    std::wstring  mftName_;

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;

    // Video processor (BGRA -> NV12)
    Microsoft::WRL::ComPtr<ID3D11VideoDevice>                vdev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext>               vctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator>   venum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor>             vproc_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>    inView_;
    ID3D11Texture2D*                                         inViewTex_ = nullptr; // key only, not owned
    std::map<std::pair<ID3D11Texture2D*, UINT>,
             Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>> outViews_;

    // Media Foundation
    bool mfStarted_ = false;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager>     devMgr_;
    UINT                                             resetToken_ = 0;
    Microsoft::WRL::ComPtr<IMFTransform>             mft_;
    Microsoft::WRL::ComPtr<ICodecAPI>                codec_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator>   eventGen_;
    Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    Microsoft::WRL::ComPtr<IMFAsyncCallback>         callback_;
    EventCallback*                                   callbackRaw_ = nullptr;
    DWORD inId_ = 0, outId_ = 0;

    std::mutex mftMutex_;           // serializes ProcessInput / ProcessOutput / ProcessMessage

    std::mutex              inMutex_;   // guards needInput_
    std::condition_variable inCv_;
    int                     needInput_ = 0;

    std::mutex drainMutex_;
    std::condition_variable drainCv_;
    bool drainComplete_ = false;

    struct InFlight { int64_t submitQpc; int64_t inputQpc; };
    std::mutex                            statsMutex_;
    std::unordered_map<LONGLONG, InFlight> inflight_;

    std::atomic<bool>     running_{false};
    std::atomic<bool>     draining_{false};
    std::atomic<bool>     forceKey_{false};
    std::atomic<int64_t>  lastKeyQpc_{0};
    uint64_t              frameCounter_ = 0;      // Submit() thread only
    std::atomic<uint64_t> submitted_{0}, droppedNoInput_{0}, droppedPool_{0}, produced_{0};
};

} // namespace host
