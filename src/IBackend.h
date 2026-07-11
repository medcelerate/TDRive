// IBackend.h
//
// Platform-specific Rive renderer wrapper. The Rive C++ API
// (rive::File / Artboard / StateMachineInstance / ViewModelInstanceRuntime)
// is identical on every platform, so all the file/parameter/CHOP/DAT plumbing
// lives in TDRiveTOP.{h,cpp}. This interface isolates the GPU bits:
//
//   * macOS  -> backend_metal.mm   (Metal, RenderContextMetalImpl)
//   * Win32  -> backend_d3d11.cpp  (D3D11, RenderContextD3DImpl)
//
// The backend owns the rive::gpu::RenderContext (which doubles as the
// rive::Factory used to parse .riv files) and the offscreen render target.
// It must also do the readback - it's the only part that knows which GPU
// API to talk to.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "rive/factory.hpp"
#include "rive/renderer.hpp"
#include "rive/renderer/render_context.hpp"

namespace tdrive {

// Number of injectable texture slots (the Image1..ImageN / Imageprop1..N
// parameter pairs on the node).
constexpr int kMaxImageSlots = 4;

class IBackend {
public:
    virtual ~IBackend() = default;

    // One-time setup. Returns false and fills 'err' on failure.
    virtual bool init(std::string& err) = 0;

    // Both of these stay valid for the backend's lifetime once init() succeeds.
    virtual rive::Factory*               factory()       = 0;
    virtual rive::gpu::RenderContext*    renderContext() = 0;

    // (Re)allocate the offscreen render target + any readback buffer at the
    // given size. May be called every cook with the same size - cheap when
    // unchanged.
    virtual bool ensureRenderTarget(uint32_t width, uint32_t height,
                                    std::string& err) = 0;

    // Run a Rive frame end-to-end:
    //   beginFrame(fd) -> draw(renderer) -> flush -> blit to CPU -> memcpy to dst
    // dst points at width*height*4 bytes of BGRA8 in row-major order, top row
    // first. Returns false on failure.
    virtual bool renderAndReadback(
        const rive::gpu::RenderContext::FrameDescriptor& fd,
        const std::function<void(rive::Renderer*)>&      draw,
        void*                                            dst,
        std::string&                                     err) = 0;

    // -------------------------------------------------------------------------
    // Texture injection (CPU path - works on every platform)
    // -------------------------------------------------------------------------
    // Upload width*height RGBA8 *premultiplied* pixels (tightly packed, top row
    // first) into the given slot's GPU texture and return a rive::RenderImage
    // wrapping it. The backend reuses the slot's texture across calls when the
    // size is unchanged, so the returned RenderImage pointer is stable frame to
    // frame - callers only need to re-bind view-model properties when the
    // pointer changes. Returns nullptr and fills 'err' on failure.
    virtual rive::rcp<rive::RenderImage> updateImageSlot(
        int slot, uint32_t width, uint32_t height,
        const uint8_t* rgbaPremultiplied, std::string& err) = 0;

    // -------------------------------------------------------------------------
    // CUDA interop (Windows / D3D11 + NVIDIA only; default stubs elsewhere)
    // -------------------------------------------------------------------------
    // True when this backend can move textures to/from CUDA arrays without a
    // CPU round-trip.
    virtual bool cudaInterop() const { return false; }

    // Same contract as updateImageSlot, but the source pixels come from a
    // cudaArray* (RGBA8, as handed out by TouchDesigner in CUDA execute mode).
    // Must be called between OP_Context::beginCUDAOperations/end.
    virtual rive::rcp<rive::RenderImage> updateImageSlotCUDA(
        int /*slot*/, uint32_t /*width*/, uint32_t /*height*/,
        void* /*cudaArray*/, std::string& err)
    {
        err = "CUDA interop is not available on this backend.";
        return nullptr;
    }

    // Run a Rive frame and copy the result GPU->GPU into dstCudaArray (RGBA8,
    // width*height, as created by TOP_Output::createCUDAArray). The CUDA copy
    // portion must run between OP_Context::beginCUDAOperations/end; the caller
    // is responsible for that bracketing.
    virtual bool renderToCUDA(
        const rive::gpu::RenderContext::FrameDescriptor& /*fd*/,
        const std::function<void(rive::Renderer*)>&      /*draw*/,
        void* /*dstCudaArray*/, std::string& err)
    {
        err = "CUDA interop is not available on this backend.";
        return false;
    }
};

// Factory function defined by the platform-specific .mm/.cpp that gets
// compiled in. The build system picks exactly one. 'cudaMode' is true when
// the plugin registered with TOP_ExecuteMode::CUDA (Windows + NVIDIA); the
// backend then allocates CUDA-shareable resources. Ignored on macOS.
std::unique_ptr<IBackend> CreateBackend(bool cudaMode);

} // namespace tdrive
