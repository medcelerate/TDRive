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
};

// Factory function defined by the platform-specific .mm/.cpp that gets
// compiled in. The build system picks exactly one.
std::unique_ptr<IBackend> CreateBackend();

} // namespace tdrive
