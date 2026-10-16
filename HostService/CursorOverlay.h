// CursorOverlay.h
// Draws the Windows mouse pointer onto the captured frame.
//
// Desktop Duplication frames do not contain the pointer, so moving the mouse over the virtual display changes
// nothing in the video. This class polls the Win32 pointer (GetCursorInfo: position, visibility, HCURSOR),
// renders the cursor bitmap once per shape (GetIconInfo + DrawIconEx on a black and on a white background to
// recover per-pixel alpha AND detect "invert the screen" pixels such as the I-beam), and draws it with the GPU:
//   * normal pixels: premultiplied alpha blend
//   * inverting pixels: blend  src*(1-dst) + dst*(1-src)  (an XOR-style invert)
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace host {

class CursorOverlay {
public:
    // Creates the shaders/blend states on `device`. Returns false if the overlay can't be used.
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Reset();

    // Samples the pointer. `outputName` is the GDI name of the captured output (e.g. \\.\DISPLAY4), whose
    // desktop position is looked up so we can convert to output coordinates. Returns true if anything the
    // viewer would see changed (position, visibility or shape).
    bool Poll(const std::wstring& outputName, UINT visibleWidth, UINT visibleHeight);

    bool Visible() const { return visible_; }

    // Draws the pointer into `target` (render target of size targetW x targetH). Caller holds the device lock.
    void Draw(ID3D11RenderTargetView* target, UINT targetW, UINT targetH);

private:
    bool BuildShape(HCURSOR cursor);

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>  vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>        cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>  sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>    blendAlpha_, blendInvert_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> alphaSrv_, invertSrv_;

    bool     ready_ = false;
    bool     visible_ = false;
    bool     shapeValid_ = false;
    HCURSOR  handle_ = nullptr;
    int      w_ = 0, h_ = 0, hotX_ = 0, hotY_ = 0;
    int      x_ = 0, y_ = 0;                 // hotspot position in output coordinates

    std::wstring outputName_;
    ULONGLONG    lastOriginMs_ = 0;
    LONG         originX_ = 0, originY_ = 0;
    bool         originValid_ = false;
    int          shapeLogCount_ = 0;
};

} // namespace host
