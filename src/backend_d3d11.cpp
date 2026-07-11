// backend_d3d11.cpp
//
// Windows Rive renderer backend. Mirrors backend_metal.mm: owns a D3D11
// device + context, an offscreen BGRA8Unorm/RGBA8Unorm render target,
// and a staging texture used for CPU readback.
//
// In CUDA execute mode (NVIDIA GPUs) the render target is additionally
// registered with the CUDA runtime so the finished frame can be copied
// GPU->GPU into the cudaArray TouchDesigner hands us - no CPU round-trip.
// Injected textures (Image1..N params) follow the same pattern in reverse.

#include "IBackend.h"

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>
#include <vector>

#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
// texture.hpp must be included before render_context_d3d_impl.hpp - the D3D
// header instantiates rcp<rive::gpu::Texture> through RenderContextImpl, and
// MSVC needs the full type for that. Metal's path includes it transitively;
// MSVC's path doesn't.
#include "rive/renderer/texture.hpp"
#include "rive/renderer/d3d11/render_context_d3d_impl.hpp"
#include "rive/renderer/rive_render_image.hpp"

#include "cuda_interop_win.h"

using Microsoft::WRL::ComPtr;

namespace tdrive {

class D3D11Backend : public IBackend {
public:
    explicit D3D11Backend(bool cudaMode) : mCUDAMode(cudaMode)
    {
        // BGRA8 matches TouchDesigner's BGRA8Fixed CPU upload without a
        // swizzle. In CUDA mode we use RGBA8 instead: it's in CUDA's
        // documented set of interop-safe formats (BGRA8 is not) and is also
        // unconditionally UAV-compatible.
        mTargetFormat = cudaMode ? DXGI_FORMAT_R8G8B8A8_UNORM
                                 : DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    ~D3D11Backend() override
    {
        unregisterTargetCUDA();
        for (auto& s : mSlots) releaseSlot(s);
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

        // Default: first adapter. In CUDA mode we must create the D3D11
        // device on the adapter CUDA can talk to (hybrid-GPU machines can
        // have the default adapter be the non-NVIDIA one).
        UINT ordinal = 0;
        if (mCUDAMode) {
            int cudaOrdinal = cuda::FindCUDAAdapterOrdinal();
            if (cudaOrdinal < 0) {
                err = "CUDA execute mode active but no DXGI adapter maps to "
                      "a CUDA device.";
                return false;
            }
            ordinal = (UINT)cudaOrdinal;
        }

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

    bool cudaInterop() const override { return mCUDAMode; }

    bool ensureRenderTarget(uint32_t w, uint32_t h, std::string& err) override
    {
        if (w == 0 || h == 0) { err = "Render target has zero size."; return false; }
        if (mTarget && mW == w && mH == h && mRenderTarget &&
            (mCUDAMode ? mTargetCudaRes != nullptr : mStaging != nullptr))
            return true;

        unregisterTargetCUDA();

        // Offscreen texture, render-target + UAV (Rive renders via UAV in
        // atomic mode, RTV in raster-ordered mode; we enable both so it
        // works regardless of the path the Rive runtime picks).
        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = mTargetFormat;
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

        if (mCUDAMode) {
            const auto* api = cuda::Get();
            if (!api) { err = "CUDA runtime not loaded."; return false; }
            cudaError_t ce = api->graphicsD3D11RegisterResource(
                &mTargetCudaRes, mTarget.Get(),
                cuda::kGraphicsRegisterFlagsNone);
            if (ce != cuda::kSuccess) {
                err = std::string("cudaGraphicsD3D11RegisterResource(target) "
                                  "failed: ") + api->getErrorString(ce);
                mTargetCudaRes = nullptr;
                return false;
            }
        } else {
            // Staging texture: CPU-readable copy destination.
            D3D11_TEXTURE2D_DESC s{};
            s.Width            = w;
            s.Height           = h;
            s.MipLevels        = 1;
            s.ArraySize        = 1;
            s.Format           = mTargetFormat;
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

        renderFrame(fd, draw);

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

    bool renderToCUDA(const rive::gpu::RenderContext::FrameDescriptor& fd,
                      const std::function<void(rive::Renderer*)>&      draw,
                      void* dstCudaArray, std::string& err) override
    {
        const auto* api = cuda::Get();
        if (!mCUDAMode || !api) { err = "CUDA interop inactive."; return false; }
        if (!mRenderContext || !mRenderTarget || !mTarget || !mTargetCudaRes) {
            err = "D3D11 backend not initialized (CUDA).";
            return false;
        }

        renderFrame(fd, draw);
        // Make sure the D3D work is submitted before CUDA touches the
        // texture. cudaGraphicsMapResources synchronizes with the device,
        // but only against submitted work.
        mContext->Flush();

        cudaError_t ce = api->graphicsMapResources(1, &mTargetCudaRes, nullptr);
        if (ce != cuda::kSuccess) {
            err = std::string("cudaGraphicsMapResources(target) failed: ") +
                  api->getErrorString(ce);
            return false;
        }
        cudaArray* srcArray = nullptr;
        ce = api->graphicsSubResourceGetMappedArray(&srcArray,
                                                    mTargetCudaRes, 0, 0);
        if (ce == cuda::kSuccess && srcArray) {
            ce = api->memcpy2DArrayToArray(
                (cudaArray*)dstCudaArray, 0, 0, srcArray, 0, 0,
                (size_t)mW * 4, (size_t)mH, cuda::kMemcpyDeviceToDevice);
        }
        api->graphicsUnmapResources(1, &mTargetCudaRes, nullptr);

        if (ce != cuda::kSuccess) {
            err = std::string("CUDA target copy failed: ") +
                  api->getErrorString(ce);
            return false;
        }
        return true;
    }

    rive::rcp<rive::RenderImage> updateImageSlot(
        int slot, uint32_t w, uint32_t h,
        const uint8_t* rgba, std::string& err) override
    {
        if (!ensureSlotTexture(slot, w, h, err)) return nullptr;
        Slot& s = mSlots[slot];
        mContext->UpdateSubresource(s.tex.Get(), 0, nullptr,
                                    rgba, w * 4, 0);
        return s.img;
    }

    rive::rcp<rive::RenderImage> updateImageSlotCUDA(
        int slot, uint32_t w, uint32_t h,
        void* srcCudaArray, std::string& err) override
    {
        const auto* api = cuda::Get();
        if (!mCUDAMode || !api) { err = "CUDA interop inactive."; return nullptr; }
        if (!ensureSlotTexture(slot, w, h, err)) return nullptr;
        Slot& s = mSlots[slot];

        if (!s.cudaRes) {
            cudaError_t ce = api->graphicsD3D11RegisterResource(
                &s.cudaRes, s.tex.Get(), cuda::kGraphicsRegisterFlagsNone);
            if (ce != cuda::kSuccess) {
                err = std::string("cudaGraphicsD3D11RegisterResource(image) "
                                  "failed: ") + api->getErrorString(ce);
                s.cudaRes = nullptr;
                return nullptr;
            }
        }

        cudaError_t ce = api->graphicsMapResources(1, &s.cudaRes, nullptr);
        if (ce != cuda::kSuccess) {
            err = std::string("cudaGraphicsMapResources(image) failed: ") +
                  api->getErrorString(ce);
            return nullptr;
        }
        cudaArray* dstArray = nullptr;
        ce = api->graphicsSubResourceGetMappedArray(&dstArray, s.cudaRes, 0, 0);
        if (ce == cuda::kSuccess && dstArray) {
            ce = api->memcpy2DArrayToArray(
                dstArray, 0, 0, (cudaArray*)srcCudaArray, 0, 0,
                (size_t)w * 4, (size_t)h, cuda::kMemcpyDeviceToDevice);
        }
        api->graphicsUnmapResources(1, &s.cudaRes, nullptr);

        if (ce != cuda::kSuccess) {
            err = std::string("CUDA image copy failed: ") +
                  api->getErrorString(ce);
            return nullptr;
        }
        return s.img;
    }

private:
    struct Slot {
        ComPtr<ID3D11Texture2D>       tex;
        rive::rcp<rive::RenderImage>  img;
        uint32_t                      w = 0, h = 0;
        cudaGraphicsResource_t        cudaRes = nullptr;
    };

    void renderFrame(const rive::gpu::RenderContext::FrameDescriptor& fd,
                     const std::function<void(rive::Renderer*)>&      draw)
    {
        mRenderTarget->setTargetTexture(mTarget);
        mRenderContext->beginFrame(fd);
        rive::RiveRenderer renderer(mRenderContext.get());
        draw(&renderer);
        rive::gpu::RenderContext::FlushResources flush;
        flush.renderTarget = mRenderTarget.get();
        flush.externalCommandBuffer = nullptr;
        mRenderContext->flush(flush);
    }

    bool ensureSlotTexture(int slot, uint32_t w, uint32_t h, std::string& err)
    {
        if (slot < 0 || slot >= kMaxImageSlots) { err = "Bad image slot."; return false; }
        if (w == 0 || h == 0) { err = "Image input has zero size."; return false; }
        Slot& s = mSlots[slot];
        if (s.tex && s.w == w && s.h == h && s.img) return true;

        releaseSlot(s);

        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        d.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags   = 0;
        d.MiscFlags        = 0;

        HRESULT hr = mDevice->CreateTexture2D(&d, nullptr,
                                              s.tex.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !s.tex) {
            err = "Failed to allocate D3D11 image texture.";
            return false;
        }

        auto* impl = mRenderContext->static_impl_cast<rive::gpu::RenderContextD3DImpl>();
        auto riveTex = impl->adoptImageTexture(s.tex, w, h);
        if (!riveTex) { err = "adoptImageTexture failed."; return false; }
        s.img = rive::make_rcp<rive::RiveRenderImage>(std::move(riveTex));
        s.w = w;
        s.h = h;
        return true;
    }

    void releaseSlot(Slot& s)
    {
        if (s.cudaRes) {
            if (const auto* api = cuda::Get())
                api->graphicsUnregisterResource(s.cudaRes);
            s.cudaRes = nullptr;
        }
        s.img.reset();
        s.tex.Reset();
        s.w = s.h = 0;
    }

    void unregisterTargetCUDA()
    {
        if (mTargetCudaRes) {
            if (const auto* api = cuda::Get())
                api->graphicsUnregisterResource(mTargetCudaRes);
            mTargetCudaRes = nullptr;
        }
    }

    bool                         mCUDAMode = false;
    DXGI_FORMAT                  mTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<ID3D11Device>         mDevice;
    ComPtr<ID3D11DeviceContext>  mContext;
    ComPtr<ID3D11Texture2D>      mTarget;
    ComPtr<ID3D11Texture2D>      mStaging;
    cudaGraphicsResource_t       mTargetCudaRes = nullptr;
    uint32_t                     mW = 0, mH = 0;

    Slot mSlots[kMaxImageSlots];

    std::unique_ptr<rive::gpu::RenderContext> mRenderContext;
    rive::rcp<rive::gpu::RenderTargetD3D>     mRenderTarget;
};

std::unique_ptr<IBackend> CreateBackend(bool cudaMode)
{
    return std::make_unique<D3D11Backend>(cudaMode);
}

} // namespace tdrive
