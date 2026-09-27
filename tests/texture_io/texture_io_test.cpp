// texture_io_test.cpp
//
// Headless round-trip test of texture injection ("video in") and the rendered
// frame ("video out"), against tests/texture_io/texture_io.riv.
//
// It drives the same pieces the TOP does - the platform backend's
// updateImageSlot() / updateImageSlotCUDA() for the input side, view-model
// propertyImage()->value() for the bind, and renderAndReadback() /
// renderToCUDA() for the output side - so a pass here means the plugin's
// texture paths are sound without launching TouchDesigner.
//
// Modes:
//   cpu   the CPU path (macOS Metal, or Windows D3D11 staging readback).
//   cuda  Windows + NVIDIA only: frames go in and out as cudaArrays, the way
//         TouchDesigner hands them over in CUDA execute mode. cudart is found
//         the same way the plugin finds it (see cuda_interop_win.cpp), so put
//         TouchDesigner's bin directory on PATH if no CUDA toolkit is
//         installed.
//
// Usage: texture_io_test [path/to/texture_io.riv] [cpu|cuda|all]
// Default mode is "all" on Windows (cuda skipped when unavailable), "cpu"
// elsewhere. Exits 0 when every check passes.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "IBackend.h"

#include "rive/artboard.hpp"
#include "rive/file.hpp"
#include "rive/scene.hpp"
#include "rive/layout.hpp"
#include "rive/math/aabb.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_asset_image_runtime.hpp"

#if defined(_WIN32)
#include <windows.h>
#include "cuda_interop_win.h"
#endif

namespace {

constexpr uint32_t W = 1920, H = 1080;

int gFailures = 0;
int gChecks   = 0;

struct RGBA { int r, g, b, a; };

void check(bool ok, const std::string& what)
{
    ++gChecks;
    if (!ok) { ++gFailures; std::printf("  FAIL  %s\n", what.c_str()); }
    else     {              std::printf("  ok    %s\n", what.c_str()); }
}

// Straight RGBA8 -> premultiplied, as TDRiveTOP::applyImageInputsCPU does. In
// CUDA mode the plugin does no premultiply, so the test does it upstream, the
// way the README tells users to.
std::vector<uint8_t> premultiplied(std::vector<uint8_t> px)
{
    for (size_t i = 0; i < px.size(); i += 4) {
        const unsigned a = px[i + 3];
        for (int c = 0; c < 3; ++c) px[i + c] = (uint8_t)((px[i + c] * a + 127) / 255);
    }
    return px;
}

std::vector<uint8_t> solid(uint32_t w, uint32_t h, RGBA c)
{
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = (uint8_t)c.r; px[i + 1] = (uint8_t)c.g;
        px[i + 2] = (uint8_t)c.b; px[i + 3] = (uint8_t)c.a;
    }
    return px;
}

// -----------------------------------------------------------------------------
// Transport: how pixels get into a slot and how the frame gets back out.
// -----------------------------------------------------------------------------

using DrawFn = std::function<void(rive::Renderer*)>;

struct Transport {
    virtual ~Transport() = default;
    virtual const char* name() const = 0;
    // Frames of output latency after the first readback. The Windows CPU path
    // reads back through double-buffered staging (backend_d3d11.cpp), so it
    // returns the previous cook's frame; everything else is same-cook.
    virtual int lag() const = 0;
    // Upload straight-alpha RGBA8 into a slot; returns the slot's RenderImage.
    virtual rive::RenderImage* inject(int slot, uint32_t w, uint32_t h,
                                      const std::vector<uint8_t>& straight) = 0;
    // Render one frame; 'out' receives RGBA8, top row first.
    virtual bool render(const DrawFn& draw, std::vector<uint8_t>& out) = 0;
    virtual rive::Factory* factory() = 0;
};

rive::gpu::RenderContext::FrameDescriptor frameDesc()
{
    rive::gpu::RenderContext::FrameDescriptor fd;
    fd.renderTargetWidth  = W;
    fd.renderTargetHeight = H;
    fd.loadAction = rive::gpu::LoadAction::clear;
    fd.clearColor = 0xff000000;
    return fd;
}

struct CpuTransport : Transport {
    std::unique_ptr<tdrive::IBackend> be;
    std::vector<uint8_t>              bgra;

    bool init()
    {
        be = tdrive::CreateBackend(false);
        std::string err;
        if (!be->init(err) || !be->ensureRenderTarget(W, H, err)) {
            std::printf("cpu backend: %s\n", err.c_str());
            return false;
        }
        bgra.resize((size_t)W * H * 4);
        return true;
    }
    const char* name() const override { return "cpu"; }
#if defined(_WIN32)
    int lag() const override { return 1; }
#else
    int lag() const override { return 0; }
#endif
    rive::Factory* factory() override { return be->factory(); }

    rive::RenderImage* inject(int slot, uint32_t w, uint32_t h,
                              const std::vector<uint8_t>& straight) override
    {
        std::string err;
        auto pm  = premultiplied(straight);
        auto img = be->updateImageSlot(slot, w, h, pm.data(), err);
        if (!img) std::printf("  updateImageSlot: %s\n", err.c_str());
        return img.get();  // the backend's slot keeps it alive
    }

    bool render(const DrawFn& draw, std::vector<uint8_t>& out) override
    {
        std::string err;
        if (!be->renderAndReadback(frameDesc(), draw, bgra.data(), err)) {
            std::printf("  renderAndReadback: %s\n", err.c_str());
            return false;
        }
        out.resize(bgra.size());
        for (size_t i = 0; i < bgra.size(); i += 4) {  // BGRA -> RGBA
            out[i] = bgra[i + 2]; out[i + 1] = bgra[i + 1];
            out[i + 2] = bgra[i]; out[i + 3] = bgra[i + 3];
        }
        return true;
    }
};

#if defined(_WIN32)
// The pieces of cudart the plugin never needs but the test does: allocating
// the arrays TouchDesigner would own, and moving pixels to and from them.
struct CudaTestApi {
    struct ChannelDesc { int x, y, z, w; int f; };
    int (*mallocArray)(cudaArray** a, const ChannelDesc* d, size_t w, size_t h, unsigned flags);
    int (*freeArray)(cudaArray* a);
    int (*memcpy2DToArray)(cudaArray* dst, size_t wOff, size_t hOff, const void* src,
                           size_t spitch, size_t widthBytes, size_t height, int kind);
    int (*memcpy2DFromArray)(void* dst, size_t dpitch, const cudaArray* src, size_t wOff,
                             size_t hOff, size_t widthBytes, size_t height, int kind);
    int (*deviceSynchronize)();

    bool load()
    {
        // tdrive::cuda::Load() has already put a cudart into the process.
        static const char* kNames[] = {
            "cudart64_13.dll", "cudart64_12.dll", "cudart64_110.dll",
            "cudart64_102.dll", "cudart64_101.dll", "cudart64_100.dll",
        };
        HMODULE m = nullptr;
        for (const char* n : kNames) if ((m = GetModuleHandleA(n))) break;
        if (!m) return false;
        auto get = [m](auto& fn, const char* sym) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, sym));
            return fn != nullptr;
        };
        return get(mallocArray, "cudaMallocArray") && get(freeArray, "cudaFreeArray")
            && get(memcpy2DToArray, "cudaMemcpy2DToArray")
            && get(memcpy2DFromArray, "cudaMemcpy2DFromArray")
            && get(deviceSynchronize, "cudaDeviceSynchronize");
    }
};

constexpr int kHostToDevice = 1, kDeviceToHost = 2;

struct CudaTransport : Transport {
    std::unique_ptr<tdrive::IBackend> be;
    CudaTestApi                       api{};
    cudaArray*                        outArr = nullptr;
    struct In { cudaArray* arr = nullptr; uint32_t w = 0, h = 0; } in[tdrive::kMaxImageSlots];

    ~CudaTransport() override
    {
        be.reset();
        if (api.freeArray) {
            for (auto& s : in) if (s.arr) api.freeArray(s.arr);
            if (outArr) api.freeArray(outArr);
        }
    }

    bool init()
    {
        if (!tdrive::cuda::AvailableForD3D11()) {
            std::printf("cuda: no cudart / no CUDA-capable D3D11 adapter\n");
            return false;
        }
        if (!api.load()) { std::printf("cuda: cudart missing test entry points\n"); return false; }
        be = tdrive::CreateBackend(true);
        std::string err;
        if (!be->init(err) || !be->ensureRenderTarget(W, H, err)) {
            std::printf("cuda backend: %s\n", err.c_str());
            return false;
        }
        be->ensureCudaStream();
        const CudaTestApi::ChannelDesc rgba8{ 8, 8, 8, 8, 1 /*unsigned*/ };
        if (api.mallocArray(&outArr, &rgba8, W, H, 0) != 0) {
            std::printf("cuda: cudaMallocArray(out) failed\n");
            return false;
        }
        return true;
    }
    const char* name() const override { return "cuda"; }
    int lag() const override { return 0; }
    rive::Factory* factory() override { return be->factory(); }

    rive::RenderImage* inject(int slot, uint32_t w, uint32_t h,
                              const std::vector<uint8_t>& straight) override
    {
        In& s = in[slot];
        if (!s.arr || s.w != w || s.h != h) {
            if (s.arr) api.freeArray(s.arr);
            const CudaTestApi::ChannelDesc rgba8{ 8, 8, 8, 8, 1 };
            if (api.mallocArray(&s.arr, &rgba8, w, h, 0) != 0) {
                std::printf("  cudaMallocArray(in) failed\n");
                s.arr = nullptr;
                return nullptr;
            }
            s.w = w; s.h = h;
        }
        auto pm = premultiplied(straight);
        if (api.memcpy2DToArray(s.arr, 0, 0, pm.data(), (size_t)w * 4, (size_t)w * 4, h,
                                kHostToDevice) != 0) {
            std::printf("  cudaMemcpy2DToArray failed\n");
            return nullptr;
        }
        std::string err;
        auto img = be->updateImageSlotCUDA(slot, w, h, s.arr, err);
        if (!img) std::printf("  updateImageSlotCUDA: %s\n", err.c_str());
        return img.get();
    }

    bool render(const DrawFn& draw, std::vector<uint8_t>& out) override
    {
        std::string err;
        if (!be->renderToCUDA(frameDesc(), draw, outArr, err)) {
            std::printf("  renderToCUDA: %s\n", err.c_str());
            return false;
        }
        api.deviceSynchronize();
        out.resize((size_t)W * H * 4);  // CUDA-mode target is RGBA8 already
        if (api.memcpy2DFromArray(out.data(), (size_t)W * 4, outArr, 0, 0, (size_t)W * 4, H,
                                  kDeviceToHost) != 0) {
            std::printf("  cudaMemcpy2DFromArray failed\n");
            return false;
        }
        return true;
    }
};
#endif

// -----------------------------------------------------------------------------
// Scene fixture
// -----------------------------------------------------------------------------

// An artboard, its state machine and its bound view model, set up the way
// TDRiveTOP::loadArtboard / bindArtboardViewModel / selectSceneIfNeeded do it.
struct Scene {
    Transport*                                  t = nullptr;
    std::unique_ptr<rive::ArtboardInstance>     artboard;
    std::unique_ptr<rive::Scene>                scene;
    rive::rcp<rive::ViewModelInstanceRuntime>   vm;
    std::vector<uint8_t>                        out;  // RGBA8, W*H

    bool open(Transport& tr, rive::File* file, const char* name)
    {
        t = &tr;
        artboard = file->artboardNamed(name);
        if (!artboard) return false;
        auto* vmr = file->defaultArtboardViewModel(artboard.get());
        if (!vmr) return false;
        vm = vmr->createDefaultInstance();
        if (!vm) vm = vmr->createInstance();
        if (!vm) return false;
        artboard->bindViewModelInstance(vm->instance());
        scene = artboard->defaultStateMachine();
        if (!scene) scene = artboard->defaultScene();
        if (scene) scene->bindViewModelInstance(vm->instance());
        return scene != nullptr;
    }

    rive::RenderImage* inject(int slot, const char* prop, uint32_t w, uint32_t h,
                              const std::vector<uint8_t>& straight)
    {
        rive::RenderImage* img = t->inject(slot, w, h, straight);
        if (!img) return nullptr;
        auto* ip = vm->propertyImage(prop);
        if (!ip) { std::printf("  no image property '%s'\n", prop); return nullptr; }
        ip->value(img);
        return img;
    }

    // One cook: advance by dt, render, read back.
    bool cook(float dt)
    {
        scene->advanceAndApply(dt);
        return t->render([this](rive::Renderer* r) {
            r->save();
            r->align(rive::Fit::contain, rive::Alignment::center,
                     rive::AABB(0, 0, (float)W, (float)H),
                     rive::AABB(0, 0, (float)W, (float)H));
            artboard->draw(r);
            r->restore();
        }, out);
    }

    // Cook, then cook again without advancing until the readback has caught
    // up, so 'out' holds this frame on every transport.
    bool settle(float dt)
    {
        if (!cook(dt)) return false;
        for (int i = 0; i < t->lag(); ++i)
            if (!cook(0.0f)) return false;
        return true;
    }

    RGBA at(int x, int y) const
    {
        const uint8_t* p = &out[((size_t)y * W + x) * 4];
        return { p[0], p[1], p[2], p[3] };
    }
};

bool near(RGBA a, RGBA b, int tol)
{
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol
        && std::abs(a.b - b.b) <= tol;
}

std::string fmt(RGBA c)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%d,%d,%d)", c.r, c.g, c.b);
    return buf;
}

void expectPixel(const Scene& s, int x, int y, RGBA want, int tol, const std::string& what)
{
    RGBA got = s.at(x, y);
    check(near(got, want, tol),
          what + " @" + std::to_string(x) + "," + std::to_string(y)
          + " want " + fmt(want) + " got " + fmt(got));
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

// tex_passthrough: a 1920x1080 input must come back out pixel for pixel.
void testPassthrough(Transport& t, rive::File* file)
{
    std::printf("tex_passthrough\n");
    Scene s;
    if (!s.open(t, file, "tex_passthrough")) { check(false, "open artboard"); return; }

    // Every pixel distinct in its neighbourhood, so an off-by-one shift, a
    // flip or a filtering blur all show up.
    std::vector<uint8_t> in((size_t)W * H * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &in[((size_t)y * W + x) * 4];
            p[0] = (uint8_t)(x & 255); p[1] = (uint8_t)(y & 255);
            p[2] = (uint8_t)((x * 7 + y * 13) & 255); p[3] = 255;
        }
    if (!s.inject(0, "videoIn1", W, H, in) || !s.settle(0.0f)) {
        check(false, "inject + render"); return;
    }

    size_t bad = 0;
    int worst = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t* i = &in[((size_t)y * W + x) * 4];
            RGBA o = s.at((int)x, (int)y);
            int d = std::max({ std::abs(o.r - i[0]), std::abs(o.g - i[1]),
                               std::abs(o.b - i[2]) });
            worst = std::max(worst, d);
            if (d > 1) ++bad;
        }
    check(bad == 0, "round trip exact within 1 LSB (" + std::to_string(bad)
          + " pixels off, worst " + std::to_string(worst) + ")");
}

// Video: a new frame into the same slot each cook must reach the output
// within the transport's documented latency, and the slot must reuse its
// RenderImage so no re-bind is needed.
void testVideoFrames(Transport& t, rive::File* file)
{
    std::printf("video frames (tex_passthrough, expected lag %d)\n", t.lag());
    Scene s;
    if (!s.open(t, file, "tex_passthrough")) { check(false, "open artboard"); return; }

    const RGBA frames[] = { {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
                            {255, 255, 255, 255}, {17, 99, 201, 255}, {90, 30, 160, 255} };
    const int n = (int)(sizeof(frames) / sizeof(frames[0]));

    // Prime with frame 0 so the lagged transports have a previous frame.
    rive::RenderImage* first = s.inject(0, "videoIn1", W, H, solid(W, H, frames[0]));
    if (!first || !s.settle(0.0f)) { check(false, "prime"); return; }

    for (int f = 1; f < n; ++f) {
        rive::RenderImage* img = s.inject(0, "videoIn1", W, H, solid(W, H, frames[f]));
        if (!img || !s.cook(1.0f / 60.0f)) {
            check(false, "inject + render frame " + std::to_string(f)); return;
        }
        check(img == first, "frame " + std::to_string(f) + " reuses slot image");
        const int shown = f - t.lag();
        expectPixel(s, 960, 540, frames[shown], 1,
                    "cook " + std::to_string(f) + " shows frame " + std::to_string(shown));
    }
}

// tex_quad: four slots route to four properties; non-16:9 input is letterboxed
// (Fit = contain) inside its cell rather than overflowing it.
void testQuad(Transport& t, rive::File* file)
{
    std::printf("tex_quad\n");
    Scene s;
    if (!s.open(t, file, "tex_quad")) { check(false, "open artboard"); return; }

    const uint32_t w = 640, h = 480;  // 4:3 into 960x540 cells -> 720x540, pillarboxed
    const RGBA c[4] = { {255, 0, 0, 255}, {0, 255, 0, 255},
                        {0, 0, 255, 255}, {255, 255, 0, 255} };
    const char* props[4] = { "videoIn1", "videoIn2", "videoIn3", "videoIn4" };
    for (int i = 0; i < 4; ++i)
        if (!s.inject(i, props[i], w, h, solid(w, h, c[i]))) {
            check(false, std::string("inject ") + props[i]); return;
        }
    if (!s.settle(0.0f)) { check(false, "render"); return; }

    const int cx[4] = { 480, 1440, 480, 1440 }, cy[4] = { 270, 270, 810, 810 };
    for (int i = 0; i < 4; ++i)
        expectPixel(s, cx[i], cy[i], c[i], 1, std::string(props[i]) + " in its cell");

    // Pillarbox strip of cell 1 shows the cell's own background (#300000).
    expectPixel(s, 60, 270, { 0x30, 0, 0, 255 }, 1, "videoIn1 letterboxed (contain)");
}

// tex_alpha: a 50% red over white / black / grey / magenta bands must composite
// as premultiplied source-over. Straight-alpha mistakes show up as dark or
// bright fringes here.
void testAlpha(Transport& t, rive::File* file)
{
    std::printf("tex_alpha\n");
    Scene s;
    if (!s.open(t, file, "tex_alpha")) { check(false, "open artboard"); return; }
    if (!s.inject(0, "videoIn1", W, H, solid(W, H, { 255, 0, 0, 128 })) || !s.settle(0.0f)) {
        check(false, "inject + render"); return;
    }
    // out = src_premul + dst * (1 - 128/255)
    auto over = [](RGBA dst) {
        const double k = 1.0 - 128.0 / 255.0;
        return RGBA{ (int)std::lround(128 + dst.r * k), (int)std::lround(dst.g * k),
                     (int)std::lround(dst.b * k), 255 };
    };
    expectPixel(s, 240,  540, over({ 255, 255, 255, 255 }), 3, "over white");
    expectPixel(s, 720,  540, over({ 0, 0, 0, 255 }),       3, "over black");
    expectPixel(s, 1200, 540, over({ 128, 128, 128, 255 }), 3, "over grey");
    expectPixel(s, 1680, 540, over({ 255, 0, 255, 255 }),   3, "over magenta");
}

// tex_transform: injected video inside a rotating circular clip and a
// scaling/rotating panel, advancing through the looping animation.
void testTransform(Transport& t, rive::File* file)
{
    std::printf("tex_transform\n");
    Scene s;
    if (!s.open(t, file, "tex_transform")) { check(false, "open artboard"); return; }

    // Quadrant-coloured input so rotation is observable.
    const RGBA TL{ 255, 0, 0, 255 }, TR{ 0, 255, 0, 255 },
               BL{ 0, 0, 255, 255 }, BR{ 255, 255, 255, 255 };
    std::vector<uint8_t> in((size_t)W * H * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            RGBA c = y < H / 2 ? (x < W / 2 ? TL : TR) : (x < W / 2 ? BL : BR);
            uint8_t* p = &in[((size_t)y * W + x) * 4];
            p[0] = (uint8_t)c.r; p[1] = (uint8_t)c.g; p[2] = (uint8_t)c.b; p[3] = 255;
        }
    if (!s.inject(0, "videoIn1", W, H, in)
        || !s.inject(1, "videoIn2", W, H, solid(W, H, { 0, 200, 255, 255 }))
        || !s.settle(0.0f)) {
        check(false, "inject + render"); return;
    }

    // spinClip: 540x540 circle at left 190, top 270 -> centre (460, 540).
    const int px = 460 + 130, py = 540 - 130;  // upper-right of centre, inside circle
    RGBA t0 = s.at(px, py);
    check(near(t0, TR, 2), "spin t=0 upper-right shows TR quadrant, got " + fmt(t0));
    expectPixel(s, 190 + 20, 270 + 20, { 0x18, 0x18, 0x30, 255 }, 2,
                "circle clip hides image corner");
    expectPixel(s, 1300, 525, { 0, 200, 255, 255 }, 2, "videoIn2 in slide panel");

    // spinLoop is 360 degrees over 4 s: after 1 s the quadrant has moved on.
    if (!s.settle(1.0f)) { check(false, "render t=1s"); return; }
    RGBA t1 = s.at(px, py);
    check(!near(t1, TR, 8) && (near(t1, TL, 8) || near(t1, BR, 8)),
          "spin t=1s upper-right rotated to a neighbouring quadrant, got " + fmt(t1));
}

bool runAll(Transport& t, const std::vector<uint8_t>& bytes)
{
    std::printf("\n=== transport: %s ===\n", t.name());
    rive::ImportResult ir;
    auto file = rive::File::import(rive::Span<const uint8_t>(bytes.data(), bytes.size()),
                                   t.factory(), &ir);
    if (!file || ir != rive::ImportResult::success) {
        check(false, std::string(t.name()) + ": parse .riv");
        return false;
    }
    testPassthrough(t, file.get());
    testVideoFrames(t, file.get());
    testQuad(t, file.get());
    testAlpha(t, file.get());
    testTransform(t, file.get());
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "tests/texture_io/texture_io.riv";
#if defined(_WIN32)
    std::string mode = argc > 2 ? argv[2] : "all";
#else
    std::string mode = argc > 2 ? argv[2] : "cpu";
#endif

    std::ifstream f(path, std::ios::binary);
    if (!f.good()) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());

    if (mode == "cpu" || mode == "all") {
        CpuTransport t;
        if (!t.init()) return 2;
        runAll(t, bytes);
    }
    if (mode == "cuda" || mode == "all") {
#if defined(_WIN32)
        CudaTransport t;
        if (t.init()) runAll(t, bytes);
        else if (mode == "cuda") return 2;   // asked for it explicitly
        else std::printf("\n=== transport: cuda === skipped\n");
#else
        std::printf("\n=== transport: cuda === not available on this platform\n");
        if (mode == "cuda") return 2;
#endif
    }

    std::printf("\n%d/%d checks passed\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
