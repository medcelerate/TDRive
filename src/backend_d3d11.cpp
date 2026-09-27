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

#include <chrono>

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
        mStaging[0].Reset();
        mStaging[1].Reset();
        mStagingPending = false;
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

        // Diagnostics only: if any query fails to create, renderGpuMs stays 0.
        D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
        for (auto& t : mGpuTimers) {
            if (FAILED(mDevice->CreateQuery(&dj, &t.disjoint)) ||
                FAILED(mDevice->CreateQuery(&ts, &t.begin)) ||
                FAILED(mDevice->CreateQuery(&ts, &t.end))) {
                t = GpuTimer{};
            }
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
            (mCUDAMode ? mTargetCudaRes != nullptr : mStaging[0] != nullptr))
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

            for (int i = 0; i < 2; ++i) {
                mStaging[i].Reset();
                hr = mDevice->CreateTexture2D(
                    &s, nullptr, mStaging[i].ReleaseAndGetAddressOf());
                if (FAILED(hr) || !mStaging[i]) {
                    err = "Failed to allocate D3D11 staging texture.";
                    return false;
                }
            }
            // Nothing has been copied into either one at the new size yet, so
            // the next readback has to be the synchronous kind.
            mStagingIdx     = 0;
            mStagingPending = false;
        }

        auto* impl = mRenderContext->static_impl_cast<rive::gpu::RenderContextD3DImpl>();
        mRenderTarget = impl->makeRenderTarget(w, h);
        mW = w;
        mH = h;
        return true;
    }

    using Clock = std::chrono::steady_clock;
    tdrive::ReadbackTimings mTimings{};

    bool renderAndReadback(const rive::gpu::RenderContext::FrameDescriptor& fd,
                           const std::function<void(rive::Renderer*)>&      draw,
                           void*                                            dst,
                           std::string&                                     err) override
    {
        if (!mRenderContext || !mRenderTarget || !mTarget || !mStaging[0]) {
            err = "D3D11 backend not initialized.";
            return false;
        }

        const auto t0 = Clock::now();
        renderFrame(fd, draw);
        const auto t1 = Clock::now();

        // Queue the GPU->staging copy for the frame we just drew.
        //
        // The two staging textures are used round-robin so that the Map()
        // below reads the copy queued on the PREVIOUS cook, which the GPU has
        // had a whole frame to retire. Mapping the copy we just queued is what
        // made this expensive: Map(D3D11_MAP_READ) blocks until the GPU
        // catches up, and that stall measured 2.17 ms of a 3.39 ms readback at
        // 3840x2160 - 64% of it, against 0.08 ms of actual Rive rendering.
        //
        // The cost is one frame of latency: the texture handed to
        // TouchDesigner is the frame drawn on the previous cook. The first
        // cook after a resize has no previous copy to read, so it falls back
        // to the synchronous path rather than emitting a blank frame.
        const int cur  = mStagingIdx;
        const int prev = mStagingIdx ^ 1;
        mContext->CopyResource(mStaging[cur].Get(), mTarget.Get());
        // Submit now instead of letting D3D11 batch. Without this the copy sits
        // in the command buffer until something forces a flush - which is the
        // Map() below - so the GPU only starts the copy at the moment we begin
        // waiting for it, and double buffering buys nothing. Flushing here is
        // what actually gives the GPU a whole frame to retire the copy.
        mContext->Flush();
        const auto t2 = Clock::now();

        const int readIdx = mStagingPending ? prev : cur;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = mContext->Map(mStaging[readIdx].Get(), 0,
                                   D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) { err = "Map(staging) failed."; return false; }
        const auto t3 = Clock::now();

        const uint8_t* src = (const uint8_t*)mapped.pData;
        uint8_t*       d   = (uint8_t*)dst;
        const size_t   rowBytes = (size_t)mW * 4;
        if (mapped.RowPitch == rowBytes) {
            // Tightly packed - one memcpy instead of a call per scanline.
            std::memcpy(d, src, rowBytes * mH);
        } else {
            for (uint32_t y = 0; y < mH; ++y) {
                std::memcpy(d + y * rowBytes,
                            src + (size_t)y * mapped.RowPitch,
                            rowBytes);
            }
        }
        mContext->Unmap(mStaging[readIdx].Get(), 0);
        const auto t4 = Clock::now();

        mStagingIdx     = prev;
        mStagingPending = true;

        auto ms = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        mTimings.renderMs = ms(t0, t1);
        mTimings.copyMs   = ms(t1, t2);
        mTimings.mapMs    = ms(t2, t3);
        mTimings.memcpyMs = ms(t3, t4);
        mTimings.totalMs  = ms(t0, t4);
        return true;
    }

    tdrive::ReadbackTimings lastTimings() const override { return mTimings; }

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

        const auto t0 = Clock::now();
        renderFrame(fd, draw);
        // Make sure the D3D work is submitted before CUDA touches the
        // texture. cudaGraphicsMapResources synchronizes with the device,
        // but only against submitted work.
        mContext->Flush();
        const auto t1 = Clock::now();

        cudaError_t ce = api->graphicsMapResources(1, &mTargetCudaRes, nullptr);
        if (ce != cuda::kSuccess) {
            err = std::string("cudaGraphicsMapResources(target) failed: ") +
                  api->getErrorString(ce);
            return false;
        }
        const auto t2 = Clock::now();
        cudaArray* srcArray = nullptr;
        ce = api->graphicsSubResourceGetMappedArray(&srcArray,
                                                    mTargetCudaRes, 0, 0);
        if (ce == cuda::kSuccess && srcArray) {
            ce = api->memcpy2DArrayToArray(
                (cudaArray*)dstCudaArray, 0, 0, srcArray, 0, 0,
                (size_t)mW * 4, (size_t)mH, cuda::kMemcpyDeviceToDevice);
        }
        const auto t3 = Clock::now();
        api->graphicsUnmapResources(1, &mTargetCudaRes, nullptr);
        const auto t4 = Clock::now();

        auto ms = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        mTimings.renderMs = ms(t0, t1);
        mTimings.mapMs    = ms(t1, t2);
        mTimings.copyMs   = ms(t2, t3);
        mTimings.unmapMs  = ms(t3, t4);
        mTimings.memcpyMs = 0.0;
        mTimings.totalMs  = ms(t0, t4);

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
        collectGpuTimers();
        GpuTimer& t = mGpuTimers[mGpuTimerIdx];
        mGpuTimerIdx = (mGpuTimerIdx + 1) % kNumGpuTimers;
        // A set whose result hasn't come back yet is skipped, not waited on.
        const bool timed = t.disjoint.Get() != nullptr && !t.pending;
        if (timed) {
            mContext->Begin(t.disjoint.Get());
            mContext->End(t.begin.Get());
        }

        mRenderTarget->setTargetTexture(mTarget);
        mRenderContext->beginFrame(fd);
        rive::RiveRenderer renderer(mRenderContext.get());
        draw(&renderer);
        rive::gpu::RenderContext::FlushResources flush;
        flush.renderTarget = mRenderTarget.get();
        flush.externalCommandBuffer = nullptr;
        mRenderContext->flush(flush);

        if (timed) {
            mContext->End(t.end.Get());
            mContext->End(t.disjoint.Get());
            t.pending = true;
        }
    }

    void collectGpuTimers()
    {
        for (auto& t : mGpuTimers) {
            if (!t.pending) continue;
            constexpr UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 b = 0, e = 0;
            if (mContext->GetData(t.disjoint.Get(), &dj, sizeof(dj), kNoFlush) != S_OK ||
                mContext->GetData(t.begin.Get(), &b, sizeof(b), kNoFlush) != S_OK ||
                mContext->GetData(t.end.Get(), &e, sizeof(e), kNoFlush) != S_OK)
                continue;
            t.pending = false;
            if (!dj.Disjoint && dj.Frequency && e >= b)
                mTimings.renderGpuMs = (double)(e - b) * 1000.0 / (double)dj.Frequency;
        }
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
    // Two staging textures, used round-robin. See renderAndReadback().
    ComPtr<ID3D11Texture2D>      mStaging[2];
    int                          mStagingIdx     = 0;
    bool                         mStagingPending = false;
    cudaGraphicsResource_t       mTargetCudaRes = nullptr;
    uint32_t                     mW = 0, mH = 0;

    struct GpuTimer {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool                pending = false;
    };
    static constexpr int kNumGpuTimers = 4;
    GpuTimer mGpuTimers[kNumGpuTimers];
    int      mGpuTimerIdx = 0;

    Slot mSlots[kMaxImageSlots];

    std::unique_ptr<rive::gpu::RenderContext> mRenderContext;
    rive::rcp<rive::gpu::RenderTargetD3D>     mRenderTarget;
};

std::unique_ptr<IBackend> CreateBackend(bool cudaMode)
{
    return std::make_unique<D3D11Backend>(cudaMode);
}

} // namespace tdrive
