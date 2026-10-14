// HardwareEncoder.cpp
#include "HardwareEncoder.h"
#include "CaptureEngine.h"   // host::Logf

#include <chrono>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")   // CODECAPI_* GUIDs
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

namespace host {

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

namespace {

int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double QpcToMs(int64_t delta)
{
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    return (double)delta * 1000.0 / freq;
}

UINT Align16(UINT v) { return (v + 15u) & ~15u; }

bool ContainsIdrNal(const std::vector<uint8_t>& d)
{
    const size_t n = d.size();
    for (size_t i = 0; i + 4 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            const size_t nalPos = (d[i + 2] == 1) ? i + 3 : i + 4;
            if (nalPos < n && (d[nalPos] & 0x1F) == 5)
                return true;
        }
    }
    return false;
}

HRESULT SetCodecU32(ICodecAPI* api, const GUID& key, ULONG value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    return api->SetValue(&key, &v);
}

HRESULT SetCodecBool(ICodecAPI* api, const GUID& key, bool value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return api->SetValue(&key, &v);
}

void LogCodec(const wchar_t* name, HRESULT hr)
{
    if (FAILED(hr))
        Logf(L"[enc]   codec setting %ls not accepted (0x%08X) - continuing\n", name, (unsigned)hr);
}

} // namespace

const wchar_t* ToString(SubmitResult r)
{
    switch (r) {
    case SubmitResult::Submitted:             return L"Submitted";
    case SubmitResult::DroppedNoInputRequest: return L"DroppedNoInputRequest";
    case SubmitResult::DroppedPoolEmpty:      return L"DroppedPoolEmpty";
    case SubmitResult::Stopped:               return L"Stopped";
    case SubmitResult::Error:                 return L"Error";
    }
    return L"?";
}

// ---------------------------------------------------------------------------------------------
// IMFAsyncCallback: MF calls Invoke() on a work-queue thread for each MFT event.
// ---------------------------------------------------------------------------------------------

class EventCallback final : public IMFAsyncCallback {
public:
    explicit EventCallback(HardwareEncoder* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = (ULONG)InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }   // default queue
    STDMETHODIMP Invoke(IMFAsyncResult* result) override
    {
        std::lock_guard<std::mutex> lk(mtx_);   // Detach() waits for an in-flight Invoke
        if (owner_)
            owner_->OnEvent(result);
        return S_OK;
    }
    void Detach()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        owner_ = nullptr;
    }

private:
    ~EventCallback() = default;
    LONG             ref_ = 1;
    std::mutex       mtx_;
    HardwareEncoder* owner_;
};

// ---------------------------------------------------------------------------------------------
// MFT discovery
// ---------------------------------------------------------------------------------------------

bool HardwareEncoder::CreateMft(LUID luid)
{
    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = E_FAIL;

#if defined(_MSC_VER)
    // Ask for hardware encoders on the capture device's adapter (Windows 10 SDK: MFTEnum2).
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) &&
        SUCCEEDED(attrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&luid), sizeof(LUID)))) {
        hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, attrs.Get(), &activates, &count);
    }
    if (FAILED(hr) || count == 0) {
        if (SUCCEEDED(hr) && activates) CoTaskMemFree(activates);
        activates = nullptr; count = 0;
        Logf(L"[enc] MFTEnum2 (adapter-specific) found nothing (0x%08X); falling back to MFTEnumEx\n", (unsigned)hr);
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &activates, &count);
    }
#else
    (void)luid;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &activates, &count);
#endif

    if (FAILED(hr) || count == 0) {
        Logf(L"[enc] No hardware H.264 encoder MFT found (hr=0x%08X, count=%u)\n", (unsigned)hr, count);
        if (SUCCEEDED(hr) && activates) CoTaskMemFree(activates);
        return false;
    }

    bool ok = false;
    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i] && !ok) {
            WCHAR* name = nullptr;
            UINT32 len = 0;
            std::wstring friendly;
            if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) && name) {
                friendly = name;
                CoTaskMemFree(name);
            }
            ComPtr<IMFTransform> mft;
            const HRESULT ahr = activates[i]->ActivateObject(IID_PPV_ARGS(&mft));
            Logf(L"[enc] MFT candidate %u/%u: \"%ls\" activate=0x%08X\n", i + 1, count, friendly.c_str(), (unsigned)ahr);
            if (SUCCEEDED(ahr)) {
                mft_ = mft;
                mftName_ = friendly;
                ok = true;
            }
        }
        if (activates[i]) activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Codec API + media types
// ---------------------------------------------------------------------------------------------

bool HardwareEncoder::ConfigureCodecApi()
{
    if (FAILED(mft_.As(&codec_))) {
        Logf(L"[enc] MFT has no ICodecAPI; using defaults\n");
        return true;   // not fatal
    }
    ICodecAPI* c = codec_.Get();

    // Set BEFORE the media types, as the usual hardware-MFT samples do.
    LogCodec(L"AVLowLatencyMode",        SetCodecBool(c, CODECAPI_AVLowLatencyMode, true));
    LogCodec(L"AVEncCommonRealTime",     SetCodecBool(c, CODECAPI_AVEncCommonRealTime, true));
    LogCodec(L"AVEncMPVDefaultBPictureCount", SetCodecU32(c, CODECAPI_AVEncMPVDefaultBPictureCount, 0));
    LogCodec(L"AVEncMPVGOPSize",         SetCodecU32(c, CODECAPI_AVEncMPVGOPSize, cfg_.gopSize));
    LogCodec(L"AVEncCommonQualityVsSpeed", SetCodecU32(c, CODECAPI_AVEncCommonQualityVsSpeed, cfg_.qualityVsSpeed));
    LogCodec(L"AVEncCommonLowLatency",   SetCodecBool(c, CODECAPI_AVEncCommonLowLatency, true));
    if (cfg_.minQp) LogCodec(L"AVEncVideoMinQP", SetCodecU32(c, CODECAPI_AVEncVideoMinQP, cfg_.minQp));
    if (cfg_.maxQp) LogCodec(L"AVEncVideoMaxQP", SetCodecU32(c, CODECAPI_AVEncVideoMaxQP, cfg_.maxQp));
    if (cfg_.vbvMs) {
        // H.264 HRD size is specified in BYTES: kbps * ms / 8.
        const ULONG bytes = (ULONG)((uint64_t)cfg_.bitrateKbps * cfg_.vbvMs / 8);
        Logf(L"[enc]   VBV/HRD buffer: %u ms = %lu bytes\n", cfg_.vbvMs, bytes);
        LogCodec(L"AVEncCommonBufferSize", SetCodecU32(c, CODECAPI_AVEncCommonBufferSize, bytes));
    }

    HRESULT hr = SetCodecU32(c, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_LowDelayVBR);
    if (FAILED(hr)) {
        Logf(L"[enc]   LowDelayVBR not accepted (0x%08X); trying CBR\n", (unsigned)hr);
        LogCodec(L"RateControlMode(CBR)", SetCodecU32(c, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR));
    }
    LogCodec(L"AVEncCommonMeanBitRate",  SetCodecU32(c, CODECAPI_AVEncCommonMeanBitRate, cfg_.bitrateKbps * 1000u));
    return true;
}

static HRESULT MakeVideoType(const GUID& subtype, UINT w, UINT h, UINT fps, bool color,
                             UINT bitrateBps, bool isOutput, IMFMediaType** out)
{
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;

    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    if (isOutput) {
        t->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
        t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        // Deliberately NOT setting the level: Microsoft recommends letting the encoder choose.
    }
    if (color) {
        // Matches what the video processor produces (BT.709, limited range).
        t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    }
    *out = t.Detach();
    return S_OK;
}

bool HardwareEncoder::ConfigureTypes(UINT w, UINT h)
{
    // Encoders require the OUTPUT type first, then the INPUT type.
    HRESULT hr = E_FAIL;
    for (int pass = 0; pass < 2; ++pass) {
        const bool color = (pass == 0);
        ComPtr<IMFMediaType> t;
        MakeVideoType(MFVideoFormat_H264, w, h, cfg_.fps, color, cfg_.bitrateKbps * 1000u, true, &t);
        hr = mft_->SetOutputType(outId_, t.Get(), 0);
        if (SUCCEEDED(hr)) break;
        Logf(L"[enc]   SetOutputType(%ux%u, colorInfo=%d) failed: 0x%08X\n", w, h, (int)color, (unsigned)hr);
    }
    if (FAILED(hr)) return false;

    for (int pass = 0; pass < 2; ++pass) {
        const bool color = (pass == 0);
        ComPtr<IMFMediaType> t;
        MakeVideoType(MFVideoFormat_NV12, w, h, cfg_.fps, color, 0, false, &t);
        hr = mft_->SetInputType(inId_, t.Get(), 0);
        if (SUCCEEDED(hr)) break;
        Logf(L"[enc]   SetInputType(NV12 %ux%u, colorInfo=%d) failed: 0x%08X\n", w, h, (int)color, (unsigned)hr);
    }
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------------------------
// Video processor: BGRA (full-range sRGB-ish) -> NV12 (BT.709 limited)
// ---------------------------------------------------------------------------------------------

bool HardwareEncoder::CreateVideoProcessor(UINT encW, UINT encH)
{
    if (FAILED(device_.As(&vdev_)) || FAILED(ctx_.As(&vctx_))) {
        Logf(L"[enc] Device has no video support. Create it with D3D11_CREATE_DEVICE_VIDEO_SUPPORT.\n");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat  = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate    = { cfg_.fps, 1 };
    cd.InputWidth        = cfg_.width;
    cd.InputHeight       = cfg_.height;
    cd.OutputFrameRate   = { cfg_.fps, 1 };
    cd.OutputWidth       = encW;
    cd.OutputHeight      = encH;
    cd.Usage             = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    HRESULT hr = vdev_->CreateVideoProcessorEnumerator(&cd, &venum_);
    if (FAILED(hr)) { Logf(L"[enc] CreateVideoProcessorEnumerator failed: 0x%08X\n", (unsigned)hr); return false; }

    UINT inSupport = 0, outSupport = 0;
    venum_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &inSupport);
    venum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outSupport);
    Logf(L"[enc] VideoProcessor format support: BGRA input=%d, NV12 output=%d\n",
         (inSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ? 1 : 0,
         (outSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) ? 1 : 0);
    if (!(inSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !(outSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        Logf(L"[enc] Video processor can't do BGRA -> NV12 on this GPU.\n");
        return false;
    }

    hr = vdev_->CreateVideoProcessor(venum_.Get(), 0, &vproc_);
    if (FAILED(hr)) { Logf(L"[enc] CreateVideoProcessor failed: 0x%08X\n", (unsigned)hr); return false; }

    // 1:1 copy: no scaling, picture at the top-left of the (possibly larger) NV12 surface.
    RECT r = { 0, 0, (LONG)cfg_.width, (LONG)cfg_.height };
    vctx_->VideoProcessorSetStreamSourceRect(vproc_.Get(), 0, TRUE, &r);
    vctx_->VideoProcessorSetStreamDestRect(vproc_.Get(), 0, TRUE, &r);
    vctx_->VideoProcessorSetOutputTargetRect(vproc_.Get(), TRUE, &r);
    vctx_->VideoProcessorSetStreamFrameFormat(vproc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vproc_.Get(), 0, FALSE);   // no driver "enhancements"

    // Colour spaces: RGB full range (BT.709 primaries) -> YCbCr studio range (BT.709).
    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(vctx_.As(&vctx1))) {
        vctx1->VideoProcessorSetStreamColorSpace1(vproc_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vctx1->VideoProcessorSetOutputColorSpace1(vproc_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        in.Usage = 0; in.RGB_Range = 0; in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.Usage = 0; out.YCbCr_Matrix = 1 /* BT.709 */; out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx_->VideoProcessorSetStreamColorSpace(vproc_.Get(), 0, &in);
        vctx_->VideoProcessorSetOutputColorSpace(vproc_.Get(), &out);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Input sample allocator (the MFT's own pool of NV12 D3D11 textures)
// ---------------------------------------------------------------------------------------------

bool HardwareEncoder::CreateAllocator()
{
    ComPtr<IMFAttributes> inAttrs;
    if (SUCCEEDED(mft_->GetInputStreamAttributes(inId_, &inAttrs)) && inAttrs) {
        UINT32 aware = 0;
        inAttrs->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        Logf(L"[enc] MFT input stream D3D11-aware: %u\n", aware);
    }

    HRESULT hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_));
    if (FAILED(hr)) { Logf(L"[enc] MFCreateVideoSampleAllocatorEx failed: 0x%08X\n", (unsigned)hr); return false; }

    hr = allocator_->SetDirectXManager(devMgr_.Get());
    if (FAILED(hr)) { Logf(L"[enc] SetDirectXManager failed: 0x%08X\n", (unsigned)hr); return false; }

    ComPtr<IMFMediaType> inType;
    hr = mft_->GetInputCurrentType(inId_, &inType);
    if (FAILED(hr)) { Logf(L"[enc] GetInputCurrentType failed: 0x%08X\n", (unsigned)hr); return false; }

    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    // RENDER_TARGET: the video processor writes into these. VIDEO_ENCODER: the MFT reads them.
    a->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER);

    hr = allocator_->InitializeSampleAllocatorEx(cfg_.poolInitial, cfg_.poolMax, a.Get(), inType.Get());
    if (FAILED(hr)) { Logf(L"[enc] InitializeSampleAllocatorEx failed: 0x%08X\n", (unsigned)hr); return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------------------------

bool HardwareEncoder::Initialize(ID3D11Device* device, LUID adapterLuid, const EncoderConfig& cfg, FrameCallback cb)
{
    Stop();
    cfg_ = cfg;
    cb_  = std::move(cb);
    luid_ = adapterLuid;
    device_ = device;
    device_->GetImmediateContext(&ctx_);

    if (cfg_.width & 1 || cfg_.height & 1) {
        Logf(L"[enc] Input size must be even (got %ux%u)\n", cfg_.width, cfg_.height);
        return false;
    }

    // Both we and the MFT touch the immediate context from different threads.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx_.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { Logf(L"[enc] MFStartup failed: 0x%08X\n", (unsigned)hr); return false; }
    mfStarted_ = true;

    hr = MFCreateDXGIDeviceManager(&resetToken_, &devMgr_);
    if (SUCCEEDED(hr))
        hr = devMgr_->ResetDevice(device_.Get(), resetToken_);
    if (FAILED(hr)) { Logf(L"[enc] DXGI device manager setup failed: 0x%08X\n", (unsigned)hr); TearDown(); return false; }

    // Try the exact size first, then the 16-aligned size.
    const UINT alignedW = Align16(cfg_.width), alignedH = Align16(cfg_.height);
    bool ok = TryStart(cfg_.width, cfg_.height);
    if (!ok && cfg_.allowAlignedFallback && (alignedW != cfg_.width || alignedH != cfg_.height)) {
        Logf(L"[enc] Exact size %ux%u failed; retrying 16-aligned %ux%u\n", cfg_.width, cfg_.height, alignedW, alignedH);
        ok = TryStart(alignedW, alignedH);
    }
    if (!ok) { TearDown(); return false; }

    running_ = true;
    lastKeyQpc_ = Qpc();   // the first frame of a new encoder is an IDR already
    Logf(L"[enc] Encoder running: \"%ls\" %ux%u @%u fps, %u kbps, GOP %u\n",
         mftName_.c_str(), encW_, encH_, cfg_.fps, cfg_.bitrateKbps, cfg_.gopSize);
    return true;
}

bool HardwareEncoder::TryStart(UINT encW, UINT encH)
{
    // Fresh MFT / processor / allocator for every attempt.
    if (callbackRaw_) { callbackRaw_->Detach(); callbackRaw_ = nullptr; }
    callback_.Reset();
    allocator_.Reset(); vproc_.Reset(); venum_.Reset(); vctx_.Reset(); vdev_.Reset();
    inView_.Reset(); inViewTex_ = nullptr; outViews_.clear();
    codec_.Reset(); eventGen_.Reset(); mft_.Reset();
    needInput_ = 0;
    drainComplete_ = false;

    if (!CreateMft(luid_))
        return false;

    // Hardware MFTs are asynchronous: unlock, and ask for low latency.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
        UINT32 isAsync = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        if (!isAsync) {
            Logf(L"[enc] \"%ls\" is not an async MFT; only hardware (async) MFTs are supported here\n", mftName_.c_str());
            return false;
        }
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    // Give the MFT our D3D11 device (must happen before types are negotiated).
    HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(devMgr_.Get()));
    if (FAILED(hr)) {
        Logf(L"[enc] MFT_MESSAGE_SET_D3D_MANAGER failed: 0x%08X (MFT is not on this adapter?)\n", (unsigned)hr);
        return false;
    }

    // Stream IDs: E_NOTIMPL means "0 and 0".
    DWORD in = 0, out = 0;
    if (SUCCEEDED(mft_->GetStreamIDs(1, &in, 1, &out))) { inId_ = in; outId_ = out; }
    else { inId_ = 0; outId_ = 0; }

    ConfigureCodecApi();

    if (!ConfigureTypes(encW, encH))
        return false;
    encW_ = encW; encH_ = encH;

    if (!CreateVideoProcessor(encW, encH)) return false;
    if (!CreateAllocator()) return false;

    // Event pump
    if (FAILED(mft_.As(&eventGen_))) { Logf(L"[enc] MFT has no IMFMediaEventGenerator\n"); return false; }
    callbackRaw_ = new EventCallback(this);
    callback_.Attach(callbackRaw_);   // callback_ owns the initial reference
    hr = eventGen_->BeginGetEvent(callback_.Get(), nullptr);
    if (FAILED(hr)) { Logf(L"[enc] BeginGetEvent failed: 0x%08X\n", (unsigned)hr); return false; }

    hr = mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (SUCCEEDED(hr)) hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) { Logf(L"[enc] Start-streaming messages failed: 0x%08X\n", (unsigned)hr); return false; }

    return true;
}

void HardwareEncoder::Stop()
{
    if (running_ && mft_) {
        draining_ = true;
        inCv_.notify_all();

        {
            std::lock_guard<std::mutex> lk(mftMutex_);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        }
        std::unique_lock<std::mutex> lk(drainMutex_);
        if (!drainCv_.wait_for(lk, std::chrono::seconds(2), [&] { return drainComplete_; }))
            Logf(L"[enc] Drain did not complete within 2 s; shutting down anyway\n");
    }
    running_ = false;
    TearDown();
}

void HardwareEncoder::TearDown()
{
    if (callbackRaw_)
        callbackRaw_->Detach();    // waits for an in-flight Invoke(), then no more calls into us
    callbackRaw_ = nullptr;

    if (mft_) {
        std::lock_guard<std::mutex> lk(mftMutex_);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        inflight_.clear();
    }

    outViews_.clear();
    inView_.Reset(); inViewTex_ = nullptr;
    vproc_.Reset(); venum_.Reset(); vctx_.Reset(); vdev_.Reset();
    allocator_.Reset();
    eventGen_.Reset();
    callback_.Reset();
    codec_.Reset();
    mft_.Reset();
    devMgr_.Reset();
    ctx_.Reset();
    device_.Reset();

    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
    draining_ = false;
}

// ---------------------------------------------------------------------------------------------
// Submit: GPU colour conversion + ProcessInput
// ---------------------------------------------------------------------------------------------

bool HardwareEncoder::ConvertInto(ID3D11Texture2D* src, IMFSample* dstSample)
{
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(dstSample->GetBufferByIndex(0, &buf))) return false;

    ComPtr<IMFDXGIBuffer> dxgiBuf;
    if (FAILED(buf.As(&dxgiBuf))) { Logf(L"[enc] allocator sample is not DXGI-backed\n"); return false; }

    ComPtr<ID3D11Texture2D> dst;
    UINT slice = 0;
    if (FAILED(dxgiBuf->GetResource(IID_PPV_ARGS(&dst))) || FAILED(dxgiBuf->GetSubresourceIndex(&slice)))
        return false;

    // Hardware MFTs want a valid current length on DXGI surface buffers.
    ComPtr<IMF2DBuffer> buf2d;
    if (SUCCEEDED(buf.As(&buf2d))) {
        DWORD len = 0;
        if (SUCCEEDED(buf2d->GetContiguousLength(&len)))
            buf->SetCurrentLength(len);
    }

    // Input view (cached per source texture).
    if (src != inViewTex_) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        iv.Texture2D.MipSlice = 0;
        iv.Texture2D.ArraySlice = 0;
        inView_.Reset();
        HRESULT hr = vdev_->CreateVideoProcessorInputView(src, venum_.Get(), &iv, &inView_);
        if (FAILED(hr)) { Logf(L"[enc] CreateVideoProcessorInputView failed: 0x%08X\n", (unsigned)hr); return false; }
        inViewTex_ = src;
    }

    // Output view (cached per destination texture+slice; the allocator recycles a small set).
    const auto key = std::make_pair(dst.Get(), slice);
    auto it = outViews_.find(key);
    if (it == outViews_.end()) {
        D3D11_TEXTURE2D_DESC td{};
        dst->GetDesc(&td);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        if (td.ArraySize > 1) {
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            ov.Texture2DArray.MipSlice = 0;
            ov.Texture2DArray.FirstArraySlice = slice;
            ov.Texture2DArray.ArraySize = 1;
        } else {
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ov.Texture2D.MipSlice = 0;
        }
        ComPtr<ID3D11VideoProcessorOutputView> view;
        HRESULT hr = vdev_->CreateVideoProcessorOutputView(dst.Get(), venum_.Get(), &ov, &view);
        if (FAILED(hr)) { Logf(L"[enc] CreateVideoProcessorOutputView failed: 0x%08X\n", (unsigned)hr); return false; }
        it = outViews_.emplace(key, view).first;
    }

    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = inView_.Get();
    HRESULT hr = vctx_->VideoProcessorBlt(vproc_.Get(), it->second.Get(), 0, 1, &st);
    if (FAILED(hr)) { Logf(L"[enc] VideoProcessorBlt failed: 0x%08X\n", (unsigned)hr); return false; }

    ctx_->Flush();   // make the conversion visible to the MFT's queue promptly
    return true;
}

void HardwareEncoder::RequestKeyFrame(bool force)
{
    if (!force) {
        const int64_t last = lastKeyQpc_.load();
        if (last && QpcToMs(Qpc() - last) < 500.0)
            return;
    }
    forceKey_ = true;
}

bool HardwareEncoder::HasInputCredit()
{
    std::lock_guard<std::mutex> lk(inMutex_);
    return running_ && !draining_ && needInput_ > 0;
}

SubmitResult HardwareEncoder::Submit(ID3D11Texture2D* bgra)
{
    if (!running_ || draining_)
        return SubmitResult::Stopped;

    const int64_t t0 = Qpc();

    // 1. Wait (briefly) for the MFT to ask for input. If it doesn't, drop: queueing would only add latency.
    {
        std::unique_lock<std::mutex> lk(inMutex_);
        if (!inCv_.wait_for(lk, std::chrono::milliseconds(cfg_.inputWaitMs),
                            [&] { return needInput_ > 0 || draining_ || !running_; })) {
            ++droppedNoInput_;
            return SubmitResult::DroppedNoInputRequest;
        }
        if (draining_ || !running_)
            return SubmitResult::Stopped;
    }

    // 2. An NV12 sample from the MFT's pool.
    ComPtr<IMFSample> sample;
    HRESULT hr = allocator_->AllocateSample(&sample);
    if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) {
        ++droppedPool_;
        return SubmitResult::DroppedPoolEmpty;
    }
    if (FAILED(hr)) {
        Logf(L"[enc] AllocateSample failed: 0x%08X\n", (unsigned)hr);
        return SubmitResult::Error;
    }

    // 3. BGRA -> NV12 on the GPU.
    if (!ConvertInto(bgra, sample.Get()))
        return SubmitResult::Error;

    // 4. Timing + optional forced IDR.
    const LONGLONG sampleTime = (LONGLONG)((frameCounter_++) * 10000000ull / cfg_.fps);
    sample->SetSampleTime(sampleTime);
    sample->SetSampleDuration(10000000LL / cfg_.fps);

    if (forceKey_.exchange(false) && codec_) {
        lastKeyQpc_ = Qpc();
        std::lock_guard<std::mutex> lk(mftMutex_);
        LogCodec(L"AVEncVideoForceKeyFrame", SetCodecU32(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1));
    }

    // 5. Hand it over.
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        inflight_[sampleTime] = { t0, Qpc() };
    }
    {
        std::lock_guard<std::mutex> lk(mftMutex_);
        hr = mft_->ProcessInput(inId_, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        Logf(L"[enc] ProcessInput failed: 0x%08X\n", (unsigned)hr);
        std::lock_guard<std::mutex> lk(statsMutex_);
        inflight_.erase(sampleTime);
        return SubmitResult::Error;
    }
    {
        std::lock_guard<std::mutex> lk(inMutex_);
        if (needInput_ > 0) --needInput_;
    }
    ++submitted_;
    return SubmitResult::Submitted;
}

// ---------------------------------------------------------------------------------------------
// Event handling (MF worker thread)
// ---------------------------------------------------------------------------------------------

void HardwareEncoder::OnEvent(IMFAsyncResult* result)
{
    ComPtr<IMFMediaEvent> ev;
    HRESULT hr = eventGen_ ? eventGen_->EndGetEvent(result, &ev) : E_FAIL;
    if (FAILED(hr) || !ev) {
        if (running_ || draining_)
            Logf(L"[enc] EndGetEvent failed: 0x%08X\n", (unsigned)hr);
        return;   // MFT is going away; don't re-arm
    }

    MediaEventType type = MEUnknown;
    ev->GetType(&type);
    HRESULT evStatus = S_OK;
    ev->GetStatus(&evStatus);

    bool rearm = true;
    switch (type) {
    case METransformNeedInput:
        {
            std::lock_guard<std::mutex> lk(inMutex_);
            ++needInput_;
        }
        inCv_.notify_one();
        break;

    case METransformHaveOutput:
        ProcessOneOutput();
        break;

    case METransformDrainComplete:
        {
            std::lock_guard<std::mutex> lk(drainMutex_);
            drainComplete_ = true;
        }
        drainCv_.notify_all();
        rearm = false;
        break;

    case MEError:
        Logf(L"[enc] MFT reported MEError: 0x%08X\n", (unsigned)evStatus);
        break;

    default:
        break;   // METransformMarker etc.
    }

    if (rearm && eventGen_)
        eventGen_->BeginGetEvent(callback_.Get(), nullptr);
}

void HardwareEncoder::ProcessOneOutput()
{
    ComPtr<IMFSample> outSample;
    HRESULT hr = S_OK;

    {
        std::lock_guard<std::mutex> lk(mftMutex_);
        if (!mft_) return;

        MFT_OUTPUT_STREAM_INFO si{};
        mft_->GetOutputStreamInfo(outId_, &si);
        const bool mftProvides =
            (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        ComPtr<IMFSample> mine;
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = outId_;
        if (!mftProvides) {
            MFCreateSample(&mine);
            ComPtr<IMFMediaBuffer> mb;
            MFCreateMemoryBuffer(si.cbSize ? si.cbSize : (1u << 20), &mb);
            mine->AddBuffer(mb.Get());
            ob.pSample = mine.Get();
        }

        DWORD status = 0;
        hr = mft_->ProcessOutput(0, 1, &ob, &status);

        if (ob.pEvents) { ob.pEvents->Release(); ob.pEvents = nullptr; }

        if (SUCCEEDED(hr)) {
            if (mftProvides) outSample.Attach(ob.pSample);   // we own the MFT-allocated sample
            else             outSample = mine;
        } else if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (mftProvides && ob.pSample) ob.pSample->Release();
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft_->GetOutputAvailableType(outId_, 0, &t)))
                mft_->SetOutputType(outId_, t.Get(), 0);
            Logf(L"[enc] Output type changed; renegotiated\n");
            return;
        } else {
            if (mftProvides && ob.pSample) ob.pSample->Release();
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT)
                Logf(L"[enc] ProcessOutput failed: 0x%08X\n", (unsigned)hr);
            return;
        }
    }
    if (!outSample) return;

    EncodedFrame f;
    outSample->GetSampleTime(&f.sampleTime);

    int64_t submitQpc = 0, inputQpc = 0;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        auto it = inflight_.find(f.sampleTime);
        if (it != inflight_.end()) { submitQpc = it->second.submitQpc; inputQpc = it->second.inputQpc; inflight_.erase(it); }
    }
    const int64_t nowQpc = Qpc();
    if (submitQpc) f.latencyMs = QpcToMs(nowQpc - submitQpc);
    if (inputQpc)  f.encodeMs  = QpcToMs(nowQpc - inputQpc);

    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(outSample->ConvertToContiguousBuffer(&mb))) return;
    BYTE* p = nullptr; DWORD len = 0;
    if (FAILED(mb->Lock(&p, nullptr, &len))) return;
    f.data.assign(p, p + len);
    mb->Unlock();

    UINT32 clean = 0;
    outSample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
    f.keyFrame = (clean != 0) || ContainsIdrNal(f.data);
    f.outputIndex = ++produced_;

    if (cb_) cb_(f);
}

} // namespace host
