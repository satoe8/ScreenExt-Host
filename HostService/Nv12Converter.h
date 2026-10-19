// Nv12Converter.h
// BGRA -> NV12 (BT.709, limited range) with two small pixel-shader passes, instead of the D3D11 Video Processor.
//
// NV12 is two planes in one texture: Y (full resolution, R8) and interleaved CbCr (half resolution, R8G8). On
// Direct3D 11.1 devices each plane can be rendered to through its own render-target view (R8_UNORM selects plane 0,
// R8G8_UNORM selects plane 1), so a fullscreen triangle per plane does the whole conversion on the GPU. This works
// the same on Intel, AMD and NVIDIA. If the driver refuses the plane views, Convert() returns false and the caller
// falls back to the video processor.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <map>
#include <utility>

namespace host {

class Nv12Converter {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Reset();
    bool Ready() const { return ready_; }

    // src: B8G8R8A8 texture with SHADER_RESOURCE, srcW x srcH (both even).
    // dst: NV12 texture with RENDER_TARGET; dstSlice is the array slice (0 for non-array textures).
    // The caller holds the device lock. Returns false if the plane views can't be created or drawn to.
    bool Convert(ID3D11Texture2D* src, UINT srcW, UINT srcH, ID3D11Texture2D* dst, UINT dstSlice);

private:
    struct PlaneViews {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> y, uv;
    };

    Microsoft::WRL::ComPtr<ID3D11Device>          device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>   ctx_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>    vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>     psY_, psUV_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>          cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>    sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;

    ID3D11Texture2D*                                 srcKey_ = nullptr;   // key only
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srcSrv_;
    std::map<std::pair<ID3D11Texture2D*, UINT>, PlaneViews> views_;

    bool ready_ = false;
};

} // namespace host
