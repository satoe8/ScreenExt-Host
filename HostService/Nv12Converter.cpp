// Nv12Converter.cpp
#include "Nv12Converter.h"
#include "CaptureEngine.h"   // host::Logf

#include <d3dcompiler.h>

#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace host {

namespace {

const char* kHlsl = R"(
cbuffer CB : register(b0) { float2 srcSize; float2 pad; };
Texture2D src : register(t0);
SamplerState lin : register(s0);

struct VSOut { float4 pos : SV_POSITION; };

// One oversized triangle covers the viewport: vertices (0,0) (2,0) (0,2) in 0..1 space.
VSOut VS(uint vid : SV_VertexID)
{
    float2 p = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);   // BT.709

// Y plane (full resolution): sample exactly at the texel centre.
float4 PS_Y(VSOut i) : SV_Target
{
    float2 uv = (floor(i.pos.xy) + 0.5) / srcSize;
    float3 c = src.SampleLevel(lin, uv, 0).rgb;
    float y = dot(c, kLuma);
    return float4((16.0 + 219.0 * y) / 255.0, 0.0, 0.0, 1.0);
}

// CbCr plane (half resolution): one chroma sample per 2x2 block. A bilinear tap at the block centre
// (2x+1, 2y+1 in texel-edge coordinates) averages the four pixels.
float4 PS_UV(VSOut i) : SV_Target
{
    float2 uv = (floor(i.pos.xy) * 2.0 + 1.0) / srcSize;
    float3 c = src.SampleLevel(lin, uv, 0).rgb;
    float y = dot(c, kLuma);
    float cb = (c.b - y) / 1.8556;
    float cr = (c.r - y) / 1.5748;
    return float4((128.0 + 224.0 * cb) / 255.0, (128.0 + 224.0 * cr) / 255.0, 0.0, 1.0);
}
)";

bool Compile(const char* entry, const char* profile, ComPtr<ID3DBlob>& out)
{
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), "nv12.hlsl", nullptr, nullptr, entry, profile, 0, 0, &out, &err);
    if (FAILED(hr)) {
        Logf(L"[nv12] %hs compile failed: %hs\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    return true;
}

} // namespace

void Nv12Converter::Reset()
{
    views_.clear();
    srcSrv_.Reset(); srcKey_ = nullptr;
    vs_.Reset(); psY_.Reset(); psUV_.Reset(); cb_.Reset(); sampler_.Reset(); raster_.Reset();
    ctx_.Reset(); device_.Reset();
    ready_ = false;
}

bool Nv12Converter::Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    Reset();
    device_ = device;
    ctx_ = ctx;

    ComPtr<ID3DBlob> vsb, pyb, puvb;
    if (!Compile("VS", "vs_4_0", vsb) || !Compile("PS_Y", "ps_4_0", pyb) || !Compile("PS_UV", "ps_4_0", puvb))
        return false;
    if (FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(device_->CreatePixelShader(pyb->GetBufferPointer(), pyb->GetBufferSize(), nullptr, &psY_)) ||
        FAILED(device_->CreatePixelShader(puvb->GetBufferPointer(), puvb->GetBufferSize(), nullptr, &psUV_))) {
        Logf(L"[nv12] shader creation failed\n");
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) return false;

    ready_ = true;
    return true;
}

bool Nv12Converter::Convert(ID3D11Texture2D* src, UINT srcW, UINT srcH, ID3D11Texture2D* dst, UINT dstSlice)
{
    if (!ready_ || !src || !dst) return false;

    // Source SRV (cached: the capture texture never changes between device rebuilds).
    if (src != srcKey_) {
        srcSrv_.Reset();
        if (FAILED(device_->CreateShaderResourceView(src, nullptr, &srcSrv_))) {
            Logf(L"[nv12] CreateShaderResourceView(source) failed\n");
            return false;
        }
        srcKey_ = src;
    }

    // Plane views of the destination NV12 texture (cached per pooled texture + slice).
    const auto key = std::make_pair(dst, dstSlice);
    auto it = views_.find(key);
    if (it == views_.end()) {
        D3D11_TEXTURE2D_DESC td{};
        dst->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        if (td.ArraySize > 1) {
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray.MipSlice = 0;
            rv.Texture2DArray.FirstArraySlice = dstSlice;
            rv.Texture2DArray.ArraySize = 1;
        } else {
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rv.Texture2D.MipSlice = 0;
        }
        PlaneViews pv;
        rv.Format = DXGI_FORMAT_R8_UNORM;          // selects plane 0 (Y)
        HRESULT hr = device_->CreateRenderTargetView(dst, &rv, &pv.y);
        if (SUCCEEDED(hr)) {
            rv.Format = DXGI_FORMAT_R8G8_UNORM;    // selects plane 1 (CbCr)
            hr = device_->CreateRenderTargetView(dst, &rv, &pv.uv);
        }
        if (FAILED(hr)) {
            Logf(L"[nv12] NV12 plane render-target views not supported (0x%08X)\n", (unsigned)hr);
            return false;
        }
        it = views_.emplace(key, std::move(pv)).first;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    const float c[4] = { (float)srcW, (float)srcH, 0.f, 0.f };
    std::memcpy(m.pData, c, sizeof(c));
    ctx_->Unmap(cb_.Get(), 0);

    ID3D11Buffer* cb = cb_.Get();
    ID3D11SamplerState* smp = sampler_.Get();
    ID3D11ShaderResourceView* srv = srcSrv_.Get();
    const float blendFactor[4] = { 0, 0, 0, 0 };

    ctx_->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFFu);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cb);
    ctx_->PSSetSamplers(0, 1, &smp);
    ctx_->PSSetShaderResources(0, 1, &srv);

    // Pass 1: luma at full size.
    ID3D11RenderTargetView* rtv = it->second.y.Get();
    D3D11_VIEWPORT vp = { 0.f, 0.f, (float)srcW, (float)srcH, 0.f, 1.f };
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->PSSetShader(psY_.Get(), nullptr, 0);
    ctx_->Draw(3, 0);

    // Pass 2: chroma at half size.
    rtv = it->second.uv.Get();
    vp.Width = (float)(srcW / 2);
    vp.Height = (float)(srcH / 2);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->PSSetShader(psUV_.Get(), nullptr, 0);
    ctx_->Draw(3, 0);

    // Leave the context clean for the video engine / MFT.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
    ctx_->OMSetRenderTargets(1, &nullRtv, nullptr);
    return true;
}

} // namespace host
