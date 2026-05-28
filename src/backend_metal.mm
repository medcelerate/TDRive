// backend_metal.mm
//
// macOS Rive renderer backend. Owns a Metal device + command queue, an
// offscreen BGRA8Unorm render target, and a shared MTLBuffer used for
// CPU readback. All Rive API surface in here is the cross-platform set;
// the Metal-specific calls are wrapped behind IBackend.

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>

#include "IBackend.h"

#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/metal/render_context_metal_impl.h"

namespace tdrive {

class MetalBackend : public IBackend {
public:
    ~MetalBackend() override
    {
        @autoreleasepool {
            mRenderTarget.reset();
            if (mRenderContext) {
                mRenderContext->releaseResources();
                mRenderContext.reset();
            }
            mReadBuf = nil;
            mTexture = nil;
            mQueue   = nil;
            mDevice  = nil;
        }
    }

    bool init(std::string& err) override
    {
        @autoreleasepool {
            mDevice = MTLCreateSystemDefaultDevice();
            if (!mDevice) { err = "No Metal device available."; return false; }
            mQueue = [mDevice newCommandQueue];

            rive::gpu::RenderContextMetalImpl::ContextOptions opts;
            mRenderContext = rive::gpu::RenderContextMetalImpl::MakeContext(mDevice, opts);
            if (!mRenderContext) {
                err = "Failed to create Rive Metal render context.";
                mQueue = nil; mDevice = nil;
                return false;
            }
            return true;
        }
    }

    rive::Factory*            factory()       override { return mRenderContext.get(); }
    rive::gpu::RenderContext* renderContext() override { return mRenderContext.get(); }

    bool ensureRenderTarget(uint32_t w, uint32_t h, std::string& err) override
    {
        if (w == 0 || h == 0) { err = "Render target has zero size."; return false; }
        if (mTexture && mW == w && mH == h && mRenderTarget) return true;

        @autoreleasepool {
            MTLTextureDescriptor* desc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                             width:w
                                            height:h
                                         mipmapped:NO];
            desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            desc.storageMode = MTLStorageModePrivate;
            mTexture = [mDevice newTextureWithDescriptor:desc];
            if (!mTexture) { err = "Failed to allocate offscreen MTLTexture."; return false; }

            auto* impl = mRenderContext->static_impl_cast<rive::gpu::RenderContextMetalImpl>();
            mRenderTarget = impl->makeRenderTarget(MTLPixelFormatBGRA8Unorm, w, h);

            NSUInteger bytes = (NSUInteger)w * (NSUInteger)h * 4;
            mReadBuf = [mDevice newBufferWithLength:bytes
                                            options:MTLResourceStorageModeShared];
            mW = w;
            mH = h;
        }
        return true;
    }

    bool renderAndReadback(const rive::gpu::RenderContext::FrameDescriptor& fd,
                           const std::function<void(rive::Renderer*)>&      draw,
                           void*                                            dst,
                           std::string&                                     err) override
    {
        if (!mRenderContext || !mRenderTarget || !mTexture || !mReadBuf) {
            err = "Metal backend not initialized.";
            return false;
        }
        @autoreleasepool {
            mRenderTarget->setTargetTexture(mTexture);
            mRenderContext->beginFrame(fd);

            rive::RiveRenderer renderer(mRenderContext.get());
            draw(&renderer);

            id<MTLCommandBuffer> riveCb = [mQueue commandBuffer];
            rive::gpu::RenderContext::FlushResources flush;
            flush.renderTarget = mRenderTarget.get();
            flush.externalCommandBuffer = (__bridge void*)riveCb;
            mRenderContext->flush(flush);
            [riveCb commit];

            id<MTLCommandBuffer> blitCb = [mQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [blitCb blitCommandEncoder];
            [blit copyFromTexture:mTexture
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(mW, mH, 1)
                         toBuffer:mReadBuf
                destinationOffset:0
           destinationBytesPerRow:mW * 4
         destinationBytesPerImage:mW * mH * 4];
            [blit endEncoding];
            [blitCb commit];
            [blitCb waitUntilCompleted];

            std::memcpy(dst, [mReadBuf contents], (size_t)mW * mH * 4);
            mRenderTarget->setTargetTexture(nil);
        }
        return true;
    }

private:
    id<MTLDevice>       mDevice  = nil;
    id<MTLCommandQueue> mQueue   = nil;
    id<MTLTexture>      mTexture = nil;
    id<MTLBuffer>       mReadBuf = nil;
    uint32_t            mW = 0, mH = 0;

    std::unique_ptr<rive::gpu::RenderContext>  mRenderContext;
    rive::rcp<rive::gpu::RenderTargetMetal>    mRenderTarget;
};

std::unique_ptr<IBackend> CreateBackend()
{
    return std::make_unique<MetalBackend>();
}

} // namespace tdrive
