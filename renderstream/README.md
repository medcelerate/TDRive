# RiveRenderStream

A standalone Windows executable that renders Rive (`.riv`) content as a
[Disguise RenderStream](https://www.disguise.one/en/products/renderstream/)
workload, with **bidirectional texture support**. It is the Rive analogue of the
`SpoutRenderstream` (DX11 branch) tool: RenderStream drives it, it renders on the
GPU, and it can both send and receive textures.

- **Output (Rive → Disguise):** each RenderStream *scene* is one artboard in the
  `.riv`. Rive renders it into a D3D11 texture that is `sendFrame`'d directly to
  Disguise (`RS_FRAMETYPE_DX11_TEXTURE`) - no CPU readback.
- **Input (Disguise → Rive):** every view-model **image** property on an artboard
  is exposed as an `RS_PARAMETER_IMAGE`. The incoming texture is written straight
  into a Rive **GPU Canvas** (`rive::gpu::RenderCanvas`); the canvas's live,
  GPU-sampled image is bound to that view-model property. No CPU round-trip.

## Layout

| File | Role |
|------|------|
| `src/Main.cpp` | RenderStream loop: schema, `awaitFrameData`, receive/send. |
| `src/RiveRSDevice.hpp` | D3D11 device + Rive `RenderContextD3DImpl`; per-stream targets and per-input GPU canvases. |
| `src/RiveRSScene.hpp` | `.riv` load, artboard/state-machine/view-model instances, draw + image binding. |
| `include/` | RenderStream C++ API glue (`d3renderstream.h`, `renderstream.hpp`, `d3helpers.hpp`), vendored from the Disguise SDK. |

The Rive rendering recipe mirrors the plugin's `src/backend_d3d11.cpp` and
`src/TDRiveTOP.cpp`; the single shared D3D11 device is handed to both Rive and
`rs_initialiseGpGpuWithDX11Device`, which is what makes the zero-copy send/receive
possible.

## Building

Windows only. Build the Rive runtime first (canvas + scripting enabled, exactly
as the plugin needs), then build this target:

```bat
scripts\build_rive.bat release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target RiveRenderStream
```

Output: `build\Release\RiveRenderStream.exe`.

The target defines `RIVE_CANVAS` / `RIVE_ORE` so the `makeRenderCanvas` API is
visible and its object layout matches the `--with_rive_canvas` runtime libs.

## Usage

Registered as a RenderStream asset, Disguise appends its own arguments; the
relevant ones you set are:

```
RiveRenderStream.exe --file path\to\scene.riv [options]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--file`, `-f` | *(required)* | `.riv` file to render. |
| `--fit` | `1` (contain) | Rive fit: 0 fill · 1 contain · 2 cover · 3 fitWidth · 4 fitHeight · 5 none · 6 scaleDown · 7 layout. |
| `--align` | `4` (center) | Rive alignment 0-8 (topLeft … bottomRight). |
| `--graphics-adapter`, `-g` | `-1` | DXGI adapter ordinal (`-1` = first). |
| `--timeout-limit` | `5000` | `awaitFrameData` timeout, ms. |
| `--no-input` | *(off)* | Disable the image-input parameters (output only). |

Unknown flags are ignored so Disguise-appended arguments never abort startup.

## Hardware-verification notes

This tool compiles against the canvas-enabled Rive runtime but the RenderStream
round-trip can only be exercised on a Windows machine with the Disguise
RenderStream runtime installed. Items to confirm on hardware:

1. **Input pixel-channel order.** The GPU-canvas backing is created in the
   RenderStream-native format (`toDxgiFormat`). If Disguise delivers inputs as
   BGRA8 while the Rive image sampler expects RGBA, colors will swap; if so, force
   the RenderStream side to RGBA8 or add a swizzle blit into the canvas.
2. **`getFrameImage2` texture requirements.** The receive texture is created
   `DEFAULT` usage with RTV/SRV/UAV binds (no `MISC_SHARED`). SpoutRenderstream's
   input texture used `MISC_SHARED`; confirm RenderStream accepts our texture, and
   add the flag if required.
3. **Schema hashes.** Scene `hash` is left 0 at build time (as SpoutRenderstream
   does); confirm `getFrameParameters` resolves image parameters after
   `setSchema`.
