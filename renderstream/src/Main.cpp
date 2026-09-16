// RiveRenderStream - a standalone Disguise RenderStream workload that renders
// Rive (.riv) content with bidirectional texture support.
//
//   Output : one artboard per RenderStream scene, rendered on the GPU and
//            sendFrame'd to Disguise as a DX11 texture (no CPU readback).
//   Input  : each artboard view-model image property is exposed as an
//            RS_PARAMETER_IMAGE; the incoming texture is written straight into a
//            Rive GPU Canvas (rive::gpu::RenderCanvas) whose live image is bound
//            to that property. This is the "GPU Canvas" input path.
//
// Modeled on the user's SpoutRenderstream (DX11 branch) src/Main.cpp, with the
// Rive runtime standing in for Spout. See renderstream/README notes and
// RiveRSDevice.hpp / RiveRSScene.hpp for the two halves of the bridge.

#define NOMINMAX

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// RS_LOG is used by renderstream.hpp; route it to stderr.
#define RS_LOG(streamexpr) std::cerr << streamexpr << std::endl

#include "RiveRSDevice.hpp"
#include "RiveRSScene.hpp"
#include "renderstream.hpp"   // pulls in d3renderstream.h + the C++ wrapper

using namespace tdrs;

namespace {

struct Args {
    std::string file;
    int  fit            = 1;   // contain
    int  align          = 4;   // center
    int  adapter        = -1;
    int  timeout        = 5000;
    bool enableInput    = true;
    double bg[4]        = {0, 0, 0, 0};
};

bool parseArgs(int argc, char* argv[], Args& a, std::string& err)
{
    auto next = [&](int& i) -> const char* {
        return (i + 1 < argc) ? argv[++i] : nullptr;
    };
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--file" || s == "-f") {
            const char* v = next(i);
            if (!v) { err = "--file requires a path"; return false; }
            a.file = v;
        } else if (s == "--fit")            { const char* v = next(i); if (v) a.fit = std::atoi(v); }
        else if (s == "--align")            { const char* v = next(i); if (v) a.align = std::atoi(v); }
        else if (s == "--graphics-adapter" || s == "-g") { const char* v = next(i); if (v) a.adapter = std::atoi(v); }
        else if (s == "--timeout-limit")    { const char* v = next(i); if (v) a.timeout = std::atoi(v); }
        else if (s == "--no-input")         { a.enableInput = false; }
        else {
            // Unknown flags are ignored so Disguise-appended args don't abort us.
        }
    }
    if (a.file.empty()) { err = "--file <path-to-.riv> is required"; return false; }
    return true;
}

// Build a schema: one scene per artboard, and (when input is enabled) one
// RS_PARAMETER_IMAGE per view-model image property, keyed by property name.
// Strings are _strdup'd; ScopedSchema frees them. sceneImageKeys is filled in
// artboard order for use at frame time.
void buildSchema(RiveRSScene& scene, ScopedSchema& scoped, bool enableInput,
                 std::vector<std::vector<std::string>>& sceneImageKeys)
{
    scoped.schema.engineName    = _strdup("RiveRS");
    scoped.schema.engineVersion = _strdup(("RS" +
        std::to_string(RENDER_STREAM_VERSION_MAJOR) + "." +
        std::to_string(RENDER_STREAM_VERSION_MINOR)).c_str());
    scoped.schema.pluginVersion = _strdup("1.0");
    scoped.schema.info          = _strdup("");

    const size_t nScenes = scene.artboardCount();
    scoped.schema.scenes.nScenes = (uint32_t)nScenes;
    scoped.schema.scenes.scenes  = (RemoteParameters*)malloc(
        sizeof(RemoteParameters) * (nScenes ? nScenes : 1));
    sceneImageKeys.assign(nScenes, {});

    for (size_t i = 0; i < nScenes; ++i) {
        RemoteParameters rp{};
        std::string name = scene.artboardNameAt(i);
        rp.name = _strdup(name.empty() ? ("Artboard" + std::to_string(i)).c_str()
                                       : name.c_str());
        rp.hash = 0;

        std::vector<std::string> keys;
        if (enableInput) {
            keys = scene.imagePropertyNames(i);
            if (keys.empty()) keys.push_back("rive_input");
        }
        sceneImageKeys[i] = keys;

        rp.nParameters = (uint32_t)keys.size();
        rp.parameters  = keys.empty() ? nullptr
            : (RemoteParameter*)malloc(sizeof(RemoteParameter) * keys.size());
        for (size_t k = 0; k < keys.size(); ++k) {
            RemoteParameter p{};
            p.group       = _strdup("Input");
            p.displayName = _strdup(keys[k].c_str());
            p.key         = _strdup(keys[k].c_str());
            p.type        = RS_PARAMETER_IMAGE;
            p.nOptions    = 0;
            p.options     = nullptr;
            p.dmxOffset   = -1;
            p.dmxType     = RS_DMX_16_BE;
            p.flags       = REMOTEPARAMETER_NO_FLAGS;
            rp.parameters[k] = p;
        }
        scoped.schema.scenes.scenes[i] = rp;
    }
}

} // namespace

#undef main
int main(int argc, char* argv[])
{
    Args args;
    std::string err;
    if (!parseArgs(argc, argv, args, err)) {
        std::cerr << "RiveRenderStream: " << err << std::endl;
        return 1;
    }

    RiveRSDevice dev;
    if (!dev.init(args.adapter, err)) {
        std::cerr << "RiveRenderStream: device init failed: " << err << std::endl;
        return 1;
    }

    RiveRSScene scene;
    if (!scene.load(args.file, dev.factory(), err)) {
        std::cerr << "RiveRenderStream: " << err << std::endl;
        return 1;
    }

    RenderStream rs;
    try {
        rs.initialise();
        rs.initialiseGpGpuWithDX11Device(dev.device());
    } catch (const std::exception& e) {
        std::cerr << "RiveRenderStream: RenderStream init failed: "
                  << e.what() << std::endl;
        return 1;
    }

    // Schema (scenes = artboards; image params = view-model image properties).
    ScopedSchema schema;
    std::vector<std::vector<std::string>> sceneImageKeys;
    buildSchema(scene, schema, args.enableInput, sceneImageKeys);
    try {
        rs.setSchema(&schema.schema);
        rs.saveSchema(argv[0], &schema.schema);
    } catch (const std::exception& e) {
        std::cerr << "RiveRenderStream: failed to set schema: "
                  << e.what() << std::endl;
        return 1;
    }

    // getStreams() returns a pointer into RenderStream's own buffer, valid until
    // the next getStreams() call - RenderStream owns it, so we just alias it.
    const StreamDescriptions* descriptions = nullptr;
    std::unordered_map<StreamHandle, StreamTarget> streamTargets;
    std::unordered_map<std::string, CanvasInput>   canvasInputs;

    std::chrono::steady_clock::time_point lastTick;
    bool hasTick = false;

    while (true) {
        auto awaitResult = rs.awaitFrameData(args.timeout);
        if (std::holds_alternative<RS_ERROR>(awaitResult)) {
            RS_ERROR e = std::get<RS_ERROR>(awaitResult);
            if (e == RS_ERROR_STREAMS_CHANGED) {
                descriptions = rs.getStreams();
                const size_t n = descriptions ? descriptions->nStreams : 0;
                for (size_t i = 0; i < n; ++i) {
                    const StreamDescription& d = descriptions->streams[i];
                    std::string terr;
                    if (!dev.ensureStreamTarget(streamTargets[d.handle],
                                                d.width, d.height,
                                                toDxgiFormat(d.format), terr)) {
                        std::cerr << "RiveRenderStream: " << terr << std::endl;
                    }
                }
                continue;
            }
            if (e == RS_ERROR_TIMEOUT) continue;
            if (e == RS_ERROR_QUIT)    break;
            // Anything else: log and keep going rather than tear down.
            std::cerr << "RiveRenderStream: awaitFrameData error " << e << std::endl;
            continue;
        }

        const FrameData& frameData = std::get<FrameData>(awaitResult);
        if (frameData.scene >= schema.schema.scenes.nScenes) continue;

        std::string serr;
        if (!scene.setActiveArtboard(frameData.scene, serr)) {
            std::cerr << "RiveRenderStream: " << serr << std::endl;
            continue;
        }

        // ---- Input: fill GPU canvases from Disguise, bind to view model ------
        if (args.enableInput) {
            const RemoteParameters& sc = schema.schema.scenes.scenes[frameData.scene];
            const auto& keys = sceneImageKeys[frameData.scene];
            try {
                ParameterValues values = rs.getFrameParameters(sc);
                for (size_t k = 0; k < keys.size(); ++k) {
                    ImageFrameData image = values.get<ImageFrameData>(keys[k]);
                    if (image.width == 0 || image.height == 0) continue;

                    CanvasInput& ci = canvasInputs[keys[k]];
                    std::string cerr;
                    if (!dev.ensureCanvasInput(ci, image.width, image.height,
                                               image.format, cerr)) {
                        std::cerr << "RiveRenderStream: " << cerr << std::endl;
                        continue;
                    }
                    SenderFrame recv{};
                    recv.type = RS_FRAMETYPE_DX11_TEXTURE;
                    recv.dx11.resource = ci.resource();
                    rs.getFrameImage(image.imageId, recv);
                    scene.bindImage((int)k, keys[k], ci.image());
                }
            } catch (const std::exception& e) {
                // Missing/undelivered image for this frame - not fatal.
                std::cerr << "RiveRenderStream: input skipped: "
                          << e.what() << std::endl;
            }
        }

        // ---- Advance the state machine on a wall clock ----------------------
        auto now = std::chrono::steady_clock::now();
        float dt = 0.0f;
        if (hasTick) {
            dt = std::chrono::duration<float>(now - lastTick).count();
            if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
        }
        lastTick = now;
        hasTick = true;
        scene.advance(dt);

        // ---- Output: render each stream and send it -------------------------
        const size_t n = descriptions ? descriptions->nStreams : 0;
        for (size_t i = 0; i < n; ++i) {
            const StreamDescription& d = descriptions->streams[i];

            CameraResponseData cameraData{};
            cameraData.tTracked = frameData.tTracked;
            try {
                cameraData.camera = rs.getFrameCamera(d.handle);
            } catch (const RenderStreamError& e) {
                if (e.error == RS_ERROR_NOTFOUND) continue;  // raced streams change
                throw;
            }

            auto it = streamTargets.find(d.handle);
            if (it == streamTargets.end()) continue;
            StreamTarget& target = it->second;

            rive::gpu::RenderContext::FrameDescriptor fd;
            fd.renderTargetWidth  = d.width;
            fd.renderTargetHeight = d.height;
            fd.loadAction = rive::gpu::LoadAction::clear;
            uint8_t r8 = (uint8_t)(args.bg[0] * 255.0 + 0.5);
            uint8_t g8 = (uint8_t)(args.bg[1] * 255.0 + 0.5);
            uint8_t b8 = (uint8_t)(args.bg[2] * 255.0 + 0.5);
            uint8_t a8 = (uint8_t)(args.bg[3] * 255.0 + 0.5);
            fd.clearColor = ((uint32_t)a8 << 24) | ((uint32_t)r8 << 16)
                          | ((uint32_t)g8 <<  8) |  (uint32_t)b8;

            uint32_t w = d.width, h = d.height;
            dev.renderInto(target, fd, [&](rive::Renderer* r) {
                scene.draw(r, args.fit, args.align, w, h);
            });

            SenderFrame out{};
            out.type = RS_FRAMETYPE_DX11_TEXTURE;
            out.dx11.resource = target.tex.Get();

            FrameResponseData response{};
            response.cameraData = &cameraData;
            try {
                rs.sendFrame(d.handle, out, response);
            } catch (const RenderStreamError& e) {
                std::cerr << "RiveRenderStream: sendFrame failed: "
                          << e.what() << std::endl;
            }
        }
    }

    return 0;
}
