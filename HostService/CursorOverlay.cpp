// CursorOverlay.cpp
#include "CursorOverlay.h"
#include "CaptureEngine.h"   // host::Logf

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

namespace host {

namespace {

// One quad generated from SV_VertexID (triangle strip), positioned in output pixels.
const char* kHlsl = R"(
cbuffer CB : register(b0)
{
    float2 originPx;   // top-left of the cursor quad, in target pixels
    float2 sizePx;     // quad size in pixels
    float2 targetPx;   // target texture size in pixels
    float2 pad;
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VS(uint vid : SV_VertexID)
{
    float2 c = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
    float2 px = originPx + c * sizePx;
    VSOut o;
    o.pos = float4(px.x / targetPx.x * 2.0 - 1.0, 1.0 - px.y / targetPx.y * 2.0, 0.0, 1.0);
    o.uv = c;
    return o;
}

Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 PS(VSOut i) : SV_Target
{
    return tex.Sample(samp, i.uv);
}
)";

} // namespace

void CursorOverlay::Reset()
{
    alphaSrv_.Reset(); invertSrv_.Reset();
    vs_.Reset(); ps_.Reset(); cb_.Reset(); sampler_.Reset();
    blendAlpha_.Reset(); blendInvert_.Reset(); raster_.Reset();
    ctx_.Reset(); device_.Reset();
    ready_ = visible_ = shapeValid_ = originValid_ = false;
    handle_ = nullptr;
    lastOriginMs_ = 0;
}

bool CursorOverlay::Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    Reset();
    device_ = device;
    ctx_ = ctx;

    ComPtr<ID3DBlob> vsb, psb, err;
    HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), "cursor.hlsl", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) {
        Logf(L"[cursor] vertex shader compile failed: %hs\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    err.Reset();
    hr = D3DCompile(kHlsl, std::strlen(kHlsl), "cursor.hlsl", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        Logf(L"[cursor] pixel shader compile failed: %hs\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) {
        Logf(L"[cursor] shader creation failed\n");
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;       // 1:1 pixels, no smoothing
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) return false;

    // Keep the destination alpha untouched in both blend modes.
    D3D11_BLEND_DESC bl{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    rt.SrcBlend = D3D11_BLEND_ONE;                    // premultiplied-alpha "over"
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    if (FAILED(device_->CreateBlendState(&bl, &blendAlpha_))) return false;

    rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;         // src*(1-dst) + dst*(1-src): src=1 inverts, src=0 keeps
    rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    if (FAILED(device_->CreateBlendState(&bl, &blendInvert_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) return false;

    ready_ = true;
    return true;
}

// Renders the cursor twice (on black and on white) to recover alpha and "invert" pixels, then uploads the result.
bool CursorOverlay::BuildShape(HCURSOR cursor)
{
    alphaSrv_.Reset();
    invertSrv_.Reset();
    shapeValid_ = false;

    ICONINFO ii{};
    if (!GetIconInfo(cursor, &ii)) return false;
    BITMAP bm{};
    HBITMAP ref = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    const bool gotBitmap = ref && GetObjectW(ref, sizeof(bm), &bm) > 0;
    const int w = bm.bmWidth;
    const int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;     // monochrome: AND mask stacked on XOR mask
    hotX_ = (int)ii.xHotspot;
    hotY_ = (int)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    if (!gotBitmap || w <= 0 || h <= 0 || w > 512 || h > 512) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    void* bitsBlack = nullptr;
    void* bitsWhite = nullptr;
    HBITMAP dibBlack = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bitsBlack, nullptr, 0);
    HBITMAP dibWhite = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bitsWhite, nullptr, 0);

    const size_t n = (size_t)w * (size_t)h;
    std::vector<uint32_t> alphaPx(n, 0), invertPx(n, 0);
    bool anyAlpha = false, anyInvert = false;
    bool ok = dibBlack && dibWhite && bitsBlack && bitsWhite;

    if (ok) {
        std::memset(bitsBlack, 0, n * 4);
        uint32_t* wp = static_cast<uint32_t*>(bitsWhite);
        for (size_t i = 0; i < n; ++i) wp[i] = 0x00FFFFFFu;

        HGDIOBJ old = SelectObject(dc, dibBlack);
        DrawIconEx(dc, 0, 0, cursor, w, h, 0, nullptr, DI_NORMAL);
        GdiFlush();
        SelectObject(dc, dibWhite);
        DrawIconEx(dc, 0, 0, cursor, w, h, 0, nullptr, DI_NORMAL);
        GdiFlush();
        SelectObject(dc, old);

        const uint32_t* b = static_cast<const uint32_t*>(bitsBlack);
        const uint32_t* wv = static_cast<const uint32_t*>(bitsWhite);
        for (size_t i = 0; i < n; ++i) {
            int bB = b[i] & 0xFF, bG = (b[i] >> 8) & 0xFF, bR = (b[i] >> 16) & 0xFF;
            const int wB = wv[i] & 0xFF, wG = (wv[i] >> 8) & 0xFF, wR = (wv[i] >> 16) & 0xFF;
            // Opaque pixel: same on both backgrounds (diff 0). Fully transparent: black on black, white on
            // white (diff 255). Semi-transparent: in between. Inverting pixel: the result flips (diff < 0).
            const int diff = ((wB - bB) + (wG - bG) + (wR - bR)) / 3;
            if (diff < -96) {
                invertPx[i] = 0xFFFFFFFFu;
                anyInvert = true;
            } else {
                const int a = std::min(255, std::max(0, 255 - diff));
                if (a >= 4) {
                    bB = std::min(bB, a); bG = std::min(bG, a); bR = std::min(bR, a);      // already premultiplied
                    alphaPx[i] = ((uint32_t)a << 24) | ((uint32_t)bR << 16) | ((uint32_t)bG << 8) | (uint32_t)bB;
                    anyAlpha = true;
                }
            }
        }
    }
    if (dibBlack) DeleteObject(dibBlack);
    if (dibWhite) DeleteObject(dibWhite);
    DeleteDC(dc);
    if (!ok || (!anyAlpha && !anyInvert)) return false;

    auto upload = [&](const std::vector<uint32_t>& px, ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = (UINT)w; d.Height = (UINT)h;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ px.data(), (UINT)w * 4, 0 };
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device_->CreateTexture2D(&d, &init, &tex))) return false;
        return SUCCEEDED(device_->CreateShaderResourceView(tex.Get(), nullptr, &srv));
    };
    if (anyAlpha && !upload(alphaPx, alphaSrv_)) return false;
    if (anyInvert && !upload(invertPx, invertSrv_)) { alphaSrv_.Reset(); return false; }

    w_ = w; h_ = h;
    shapeValid_ = true;
    if (shapeLogCount_++ < 3)
        Logf(L"[cursor] shape %dx%d hotspot (%d,%d) alpha=%d invert=%d\n", w, h, hotX_, hotY_, (int)anyAlpha, (int)anyInvert);
    return true;
}

bool CursorOverlay::Poll(const std::wstring& outputName, UINT visibleWidth, UINT visibleHeight)
{
    if (!ready_) return false;

    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return false;

    // Where is our output on the virtual desktop? (refreshed a few times per second, so moving the monitor in
    // Display Settings keeps the pointer aligned)
    const ULONGLONG now = GetTickCount64();
    if (now - lastOriginMs_ > 250 || outputName != outputName_) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!outputName.empty() && EnumDisplaySettingsExW(outputName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) {
            originX_ = dm.dmPosition.x;
            originY_ = dm.dmPosition.y;
            originValid_ = true;
        }
        outputName_ = outputName;
        lastOriginMs_ = now;
    }
    if (!originValid_) return false;

    const LONG rx = ci.ptScreenPos.x - originX_;
    const LONG ry = ci.ptScreenPos.y - originY_;
    const bool onOutput = (ci.flags & CURSOR_SHOWING) && ci.hCursor &&
                          rx >= 0 && ry >= 0 && rx < (LONG)visibleWidth && ry < (LONG)visibleHeight;

    const bool wasVisible = visible_;
    const int oldX = x_, oldY = y_;
    bool changed = false;

    if (onOutput) {
        if (ci.hCursor != handle_) {
            handle_ = ci.hCursor;
            BuildShape(handle_);                      // on failure shapeValid_ stays false: nothing is drawn
            changed = true;
        }
        x_ = (int)rx;
        y_ = (int)ry;
    }
    visible_ = onOutput && shapeValid_;

    if (visible_ != wasVisible) changed = true;
    if (visible_ && (x_ != oldX || y_ != oldY)) changed = true;
    return changed;
}

void CursorOverlay::Draw(ID3D11RenderTargetView* target, UINT targetW, UINT targetH)
{
    if (!ready_ || !visible_ || !shapeValid_ || !target) return;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    const float c[8] = { (float)(x_ - hotX_), (float)(y_ - hotY_), (float)w_, (float)h_,
                         (float)targetW, (float)targetH, 0.f, 0.f };
    std::memcpy(m.pData, c, sizeof(c));
    ctx_->Unmap(cb_.Get(), 0);

    const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)targetW, (float)targetH, 0.f, 1.f };
    ID3D11RenderTargetView* rtv = target;
    ID3D11Buffer* cb = cb_.Get();
    ID3D11SamplerState* smp = sampler_.Get();
    const float blendFactor[4] = { 0, 0, 0, 0 };

    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, &cb);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &smp);

    if (alphaSrv_) {
        ID3D11ShaderResourceView* srv = alphaSrv_.Get();
        ctx_->OMSetBlendState(blendAlpha_.Get(), blendFactor, 0xFFFFFFFFu);
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->Draw(4, 0);
    }
    if (invertSrv_) {
        ID3D11ShaderResourceView* srv = invertSrv_.Get();
        ctx_->OMSetBlendState(blendInvert_.Get(), blendFactor, 0xFFFFFFFFu);
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->Draw(4, 0);
    }

    // Leave the context clean for whoever uses it next (video processor / MFT).
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
    ctx_->OMSetRenderTargets(1, &nullRtv, nullptr);
    ctx_->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFFu);
}

} // namespace host
