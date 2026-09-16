// RiveRSDevice.hpp
//
// D3D11 device + Rive render context glue for the RiveRenderStream standalone.
// This is the Windows/D3D11-only sibling of the plugin's src/backend_d3d11.cpp,
// specialized for RenderStream instead of TouchDesigner:
//
//   * One D3D11 device is created here and handed to *both* Rive
//     (RenderContextD3DImpl::MakeContext) and RenderStream
//     (rs.initialiseGpGpuWithDX11Device). Sharing the device is what lets us
//     sendFrame the rendered texture straight to Disguise and receive input
//     textures straight into a Rive canvas - no cross-device copies.
//
//   * Output: a RenderTargetD3D per stream, sized to the StreamDescription.
//     Rive renders into it; we sendFrame(RS_FRAMETYPE_DX11_TEXTURE) the texture.
//
//   * Input (bidirectional): each image parameter gets a GPU Canvas
//     (rive::gpu::RenderCanvas). RenderStream's getFrameImage2 writes the
//     incoming texture directly into the canvas backing (zero copy), and the
//     canvas's renderImage() - a live, GPU-sampled RiveRenderImage - is bound
//     to the artboard's view-model image property. This is the "GPU Canvas"
//     input path (see ensureCanvasBacking in the Rive runtime, which we mirror
//     with the RenderStream-native pixel format instead of the fixed RGBA8).
//
// RIVE_CANVAS must be defined for makeRenderCanvas / RenderCanvas to be visible;
// the runtime libs are built --with_rive_canvas (see .github/workflows/build.yml),
// and render_context.hpp gates a data member on it, so the define must match the
// libs. Unlike the plugin (which only ever holds RenderContext by pointer) we
// call the canvas API directly, so we opt in here.

#pragma once

#ifndef RIVE_CANVAS
#define RIVE_CANVAS
#endif
#ifndef RIVE_ORE
#define RIVE_ORE
#endif

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <functional>
#include <memory>
#include <string>

#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
// texture.hpp before the D3D impl header: the D3D header instantiates
// rcp<rive::gpu::Texture> through RenderContextImpl and MSVC needs the full
// type. (Same ordering note as src/backend_d3d11.cpp.)
#include "rive/renderer/texture.hpp"
#include "rive/renderer/d3d11/render_context_d3d_impl.hpp"
#include "rive/renderer/rive_render_image.hpp"
#include "rive/renderer/render_canvas.hpp"

#include "d3renderstream.h"

namespace tdrs {

using Microsoft::WRL::ComPtr;

// RenderStream pixel format -> DXGI. Mirrors SpoutRenderstream's toDxgiFormat,
// extended for the RGBA8 members this header's enum exposes.
inline DXGI_FORMAT toDxgiFormat(RSPixelFormat format)
{
    switch (format) {
        case RS_FMT_BGRA8:
        case RS_FMT_BGRX8:   return DXGI_FORMAT_B8G8R8A8_UNORM;
        case RS_FMT_RGBA8:
        case RS_FMT_RGBX8:   return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RS_FMT_RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case RS_FMT_RGBA16:  return DXGI_FORMAT_R16G16B16A16_UNORM;
        default:             return DXGI_FORMAT_UNKNOWN;
    }
}

// A per-stream Rive render target (output: Rive -> Disguise).
struct StreamTarget {
    ComPtr<ID3D11Texture2D>               tex;
    rive::rcp<rive::gpu::RenderTargetD3D> rt;
    uint32_t   w   = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

// A per-image-parameter GPU canvas (input: Disguise -> Rive). recvTex is the
// backing texture RenderStream fills via getFrameImage2; canvas->renderImage()
// samples that same texture.
struct CanvasInput {
    rive::rcp<rive::gpu::RenderCanvas> canvas;
    ComPtr<ID3D11Texture2D>            recvTex;
    uint32_t   w   = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    ID3D11Resource* resource() const { return recvTex.Get(); }
    rive::RenderImage* image() const {
        return canvas ? canvas->renderImage() : nullptr;
    }
};

class RiveRSDevice {
public:
    // adapterOrdinal < 0 => first adapter. Returns false + fills err on failure.
    bool init(int adapterOrdinal, std::string& err)
    {
        ComPtr<IDXGIFactory2> factory;
        HRESULT hr = CreateDXGIFactory(
            __uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) { err = "CreateDXGIFactory failed."; return false; }

        UINT ordinal = adapterOrdinal < 0 ? 0u : (UINT)adapterOrdinal;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDesc{};
        if (factory->EnumAdapters(ordinal, &adapter) != DXGI_ERROR_NOT_FOUND) {
            adapter->GetDesc(&adapterDesc);
        }

        rive::gpu::D3DContextOptions opts;
        opts.isIntel = adapterDesc.VendorId == 0x163C ||
                       adapterDesc.VendorId == 0x8086 ||
                       adapterDesc.VendorId == 0x8087;

        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1 };
        UINT creationFlags = 0;
#ifdef _DEBUG
        creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        hr = D3D11CreateDevice(
            adapter.Get(),
            adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, creationFlags,
            featureLevels, (UINT)std::size(featureLevels),
            D3D11_SDK_VERSION,
            mDevice.ReleaseAndGetAddressOf(), nullptr,
            mContext.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !mDevice || !mContext) {
            err = "D3D11CreateDevice failed."; return false;
        }

        mRenderContext = rive::gpu::RenderContextD3DImpl::MakeContext(
            mDevice, mContext, opts);
        if (!mRenderContext) {
            err = "Failed to create Rive D3D11 render context.";
            mContext.Reset(); mDevice.Reset(); return false;
        }
        mImpl = mRenderContext->static_impl_cast<rive::gpu::RenderContextD3DImpl>();
        return true;
    }

    ~RiveRSDevice()
    {
        if (mRenderContext) {
            mRenderContext->releaseResources();
            mRenderContext.reset();
        }
    }

    ID3D11Device*             device()        { return mDevice.Get(); }
    ID3D11DeviceContext*      context()       { return mContext.Get(); }
    rive::Factory*            factory()       { return mRenderContext.get(); }
    rive::gpu::RenderContext* renderContext() { return mRenderContext.get(); }

    // (Re)allocate a stream's render target when its size/format changes.
    bool ensureStreamTarget(StreamTarget& t, uint32_t w, uint32_t h,
                            DXGI_FORMAT fmt, std::string& err)
    {
        if (w == 0 || h == 0) { err = "Stream target has zero size."; return false; }
        if (t.tex && t.rt && t.w == w && t.h == h && t.fmt == fmt) return true;

        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET |
                      D3D11_BIND_SHADER_RESOURCE |
                      D3D11_BIND_UNORDERED_ACCESS;

        t.tex.Reset();
        HRESULT hr = mDevice->CreateTexture2D(&d, nullptr,
                                              t.tex.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !t.tex) {
            err = "Failed to allocate stream render target texture."; return false;
        }
        t.rt  = mImpl->makeRenderTarget(w, h);
        t.w   = w; t.h = h; t.fmt = fmt;
        return true;
    }

    // Run one Rive frame into a stream target and leave the result in t.tex.
    void renderInto(StreamTarget& t,
                    const rive::gpu::RenderContext::FrameDescriptor& fd,
                    const std::function<void(rive::Renderer*)>& draw)
    {
        t.rt->setTargetTexture(t.tex);
        mRenderContext->beginFrame(fd);
        rive::RiveRenderer renderer(mRenderContext.get());
        draw(&renderer);
        rive::gpu::RenderContext::FlushResources flush;
        flush.renderTarget          = t.rt.get();
        flush.externalCommandBuffer = nullptr;
        mRenderContext->flush(flush);
    }

    // (Re)allocate an image parameter's GPU canvas when its size/format changes.
    // The backing texture is created in the RenderStream-native format so
    // getFrameImage2 can write straight into it; the canvas's renderImage()
    // samples the same texture. (This mirrors the runtime's ensureCanvasBacking,
    // which uses a single RTV+SRV texture for both halves.)
    bool ensureCanvasInput(CanvasInput& c, uint32_t w, uint32_t h,
                           RSPixelFormat rsFmt, std::string& err)
    {
        DXGI_FORMAT fmt = toDxgiFormat(rsFmt);
        if (fmt == DXGI_FORMAT_UNKNOWN) { err = "Unsupported input pixel format."; return false; }
        if (w == 0 || h == 0) { err = "Input image has zero size."; return false; }
        if (c.canvas && c.recvTex && c.w == w && c.h == h && c.fmt == fmt) return true;

        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                      D3D11_BIND_RENDER_TARGET |
                      D3D11_BIND_UNORDERED_ACCESS;

        c.recvTex.Reset();
        HRESULT hr = mDevice->CreateTexture2D(&d, nullptr,
                                              c.recvTex.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !c.recvTex) {
            err = "Failed to allocate canvas input texture."; return false;
        }

        c.canvas = mRenderContext->makeDeferredRenderCanvas(w, h);
        if (!c.canvas) { err = "makeDeferredRenderCanvas failed."; return false; }

        auto rt  = mImpl->makeRenderTarget(w, h);
        rt->setTargetTexture(c.recvTex);
        auto img = mImpl->adoptImageTexture(c.recvTex, w, h);
        if (!img) { err = "adoptImageTexture(canvas) failed."; return false; }
        c.canvas->setBacking(std::move(img), std::move(rt));

        c.w = w; c.h = h; c.fmt = fmt;
        return true;
    }

private:
    ComPtr<ID3D11Device>            mDevice;
    ComPtr<ID3D11DeviceContext>     mContext;
    std::unique_ptr<rive::gpu::RenderContext> mRenderContext;
    rive::gpu::RenderContextD3DImpl* mImpl = nullptr;  // owned by mRenderContext
};

} // namespace tdrs
