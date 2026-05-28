// backend_d3d11.cpp
//
// Windows Rive renderer backend. Mirrors backend_metal.mm: owns a D3D11
// device + context, an offscreen BGRA8Unorm/RGBA8Unorm render target,
// and a staging texture used for CPU readback.

#include "IBackend.h"

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>
#include <vector>

#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/d3d11/render_context_d3d_impl.hpp"

using Microsoft::WRL::ComPtr;

namespace tdrive {

// The Rive D3D backend renders into an RGBA8 / BGRA8 UAV-capable texture.
// We use BGRA8 so the bytes match the TouchDesigner BGRA8Fixed output format
// without a swizzle.
static constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

class D3D11Backend : public IBackend {
public:
    ~D3D11Backend() override
    {
        if (mRenderContext) {
            mRenderContext->releaseResources();
            mRenderContext.reset();
        }
        mRenderTarget.reset();
        mTarget.Reset();
        mStaging.Reset();
        mContext.Reset();
        mDevice.Reset();
    }

    bool init(std::string& err) override
    {
        ComPtr<IDXGIFactory2> factory;
        HRESULT hr = CreateDXGIFactory(
            __uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) { err = "CreateDXGIFactory failed."; return false; }

        // Pick the first adapter. (Rive's fiddle context iterates to find an
        // Intel-vs-discrete distinction; for an offscreen Custom Operator we
        // just take the default.)
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDesc{};
        if (factory->EnumAdapters(0, &adapter) != DXGI_ERROR_NOT_FOUND) {
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
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            creationFlags,
            featureLevels,
            (UINT)std::size(featureLevels),
            D3D11_SDK_VERSION,
            mDevice.ReleaseAndGetAddressOf(),
            nullptr,
            mContext.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !mDevice || !mContext) {
            err = "D3D11CreateDevice failed.";
            return false;
        }

        mRenderContext = rive::gpu::RenderContextD3DImpl::MakeContext(
            mDevice, mContext, opts);
        if (!mRenderContext) {
            err = "Failed to create Rive D3D11 render context.";
            mContext.Reset();
            mDevice.Reset();
            return false;
        }
        return true;
    }

    rive::Factory*            factory()       override { return mRenderContext.get(); }
    rive::gpu::RenderContext* renderContext() override { return mRenderContext.get(); }

    bool ensureRenderTarget(uint32_t w, uint32_t h, std::string& err) override
    {
        if (w == 0 || h == 0) { err = "Render target has zero size."; return false; }
        if (mTarget && mW == w && mH == h && mRenderTarget && mStaging) return true;

        // Offscreen BGRA8 texture, render-target + UAV (Rive renders via UAV
        // in atomic mode, RTV in raster-ordered mode; we enable both so it
        // works regardless of the path the Rive runtime picks).
        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = kTargetFormat;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        d.BindFlags        = D3D11_BIND_RENDER_TARGET |
                             D3D11_BIND_SHADER_RESOURCE |
                             D3D11_BIND_UNORDERED_ACCESS;
        d.CPUAccessFlags   = 0;
        d.MiscFlags        = 0;

        mTarget.Reset();
        HRESULT hr = mDevice->CreateTexture2D(&d, nullptr,
                                              mTarget.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !mTarget) {
            err = "Failed to allocate offscreen D3D11 texture.";
            return false;
        }

        // Staging texture: CPU-readable copy destination.
        D3D11_TEXTURE2D_DESC s{};
        s.Width            = w;
        s.Height           = h;
        s.MipLevels        = 1;
        s.ArraySize        = 1;
        s.Format           = kTargetFormat;
        s.SampleDesc.Count = 1;
        s.Usage            = D3D11_USAGE_STAGING;
        s.BindFlags        = 0;
        s.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        s.MiscFlags        = 0;

        mStaging.Reset();
        hr = mDevice->CreateTexture2D(&s, nullptr,
                                      mStaging.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !mStaging) {
            err = "Failed to allocate D3D11 staging texture.";
            return false;
        }

        auto* impl = mRenderContext->static_impl_cast<rive::gpu::RenderContextD3DImpl>();
        mRenderTarget = impl->makeRenderTarget(w, h);
        mW = w;
        mH = h;
        return true;
    }

    bool renderAndReadback(const rive::gpu::RenderContext::FrameDescriptor& fd,
                           const std::function<void(rive::Renderer*)>&      draw,
                           void*                                            dst,
                           std::string&                                     err) override
    {
        if (!mRenderContext || !mRenderTarget || !mTarget || !mStaging) {
            err = "D3D11 backend not initialized.";
            return false;
        }

        // Hand the offscreen texture to the Rive render target.
        mRenderTarget->setTargetTexture(mTarget);

        mRenderContext->beginFrame(fd);
        rive::RiveRenderer renderer(mRenderContext.get());
        draw(&renderer);

        // D3D11 doesn't use an external command buffer - flush directly.
        rive::gpu::RenderContext::FlushResources flush;
        flush.renderTarget = mRenderTarget.get();
        flush.externalCommandBuffer = nullptr;
        mRenderContext->flush(flush);

        // Copy GPU texture -> CPU-readable staging texture.
        mContext->CopyResource(mStaging.Get(), mTarget.Get());

        // Map and memcpy row-by-row (RowPitch may exceed width*4).
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = mContext->Map(mStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) { err = "Map(staging) failed."; return false; }

        const uint8_t* src = (const uint8_t*)mapped.pData;
        uint8_t*       d   = (uint8_t*)dst;
        const size_t   rowBytes = (size_t)mW * 4;
        for (uint32_t y = 0; y < mH; ++y) {
            std::memcpy(d + y * rowBytes,
                        src + (size_t)y * mapped.RowPitch,
                        rowBytes);
        }
        mContext->Unmap(mStaging.Get(), 0);
        return true;
    }

private:
    ComPtr<ID3D11Device>         mDevice;
    ComPtr<ID3D11DeviceContext>  mContext;
    ComPtr<ID3D11Texture2D>      mTarget;
    ComPtr<ID3D11Texture2D>      mStaging;
    uint32_t                     mW = 0, mH = 0;

    std::unique_ptr<rive::gpu::RenderContext> mRenderContext;
    rive::rcp<rive::gpu::RenderTargetD3D>     mRenderTarget;
};

std::unique_ptr<IBackend> CreateBackend()
{
    return std::make_unique<D3D11Backend>();
}

} // namespace tdrive
