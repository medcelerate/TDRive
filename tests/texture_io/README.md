# Texture I/O tests

`texture_io.riv` exercises the **Textures** page: video goes in through the
view-model image properties, and the rendered frame comes out as the TOP's
output. Every artboard binds the same view model, `TextureIO`, which has four
image properties, `videoIn1` … `videoIn4`. Point **Image N Property** at those
names.

Each property holds an embedded test-pattern placeholder, so the file renders
something recognisable before any TOP is connected. Once a TOP is wired in,
its pixels replace the placeholder.

| Artboard | What it checks | Slots used |
|---|---|---|
| `tex_passthrough` | One 1920×1080 image at native size, nothing drawn over it. The output should equal the input. | `videoIn1` |
| `tex_quad` | 2×2 grid of 960×540 cells, each image `Fit = contain`. Checks slot→property routing and non-16:9 inputs (letterboxed against a tinted cell background). | all four |
| `tex_transform` | `videoIn1` inside a circular clip spinning 360° every 4 s; `videoIn2` in a rounded panel that scales and rocks. Checks sampling and clipping under animation. | 1, 2 |
| `tex_alpha` | `videoIn1` over white / black / 50% grey / magenta bands. Checks premultiplied compositing. | `videoIn1` |

## Headless test (macOS)

`texture_io_test.mm` runs the plugin's own Metal backend. It pushes pixels
through `updateImageSlot()`, binds them with `propertyImage()->value()`, renders,
and reads back, with no TouchDesigner involved:

```bash
cmake --build build --target texture_io_test
./build/texture_io_test tests/texture_io/texture_io.riv
```

`ctest --test-dir build` runs the same test. It checks:

- a bit-exact passthrough round trip
- per-frame updates into one slot with a stable `RenderImage`, which is what makes a video input work
- four-slot routing and letterboxing
- exact premultiplied source-over values on all four bands
- the circle clip and a rotation that is visible after 1 s

It does not cover TouchDesigner's own download (the CPU path's frame of latency)
or the Windows CUDA path. The in-TD checks below cover those.

## In TouchDesigner

Create a Rive TOP, set **File** to `tests/texture_io/texture_io.riv`,
**Resolution** to 1920×1080 and **Fit** to Contain. Then, per artboard:

**`tex_passthrough`: bit-exact round trip and latency**
1. Use a Movie File In TOP (1920×1080) with **Image 1 TOP** set to it and
   **Image 1 Property** = `videoIn1`.
2. Connect a Composite TOP in *Difference* mode between the Movie File In and
   the Rive TOP, followed by an Analyze TOP (Maximum).
   - Expected: max = 0 on a paused movie. While it plays, the CPU path lags one
     frame, so the difference is non-zero on motion. Put a Cache TOP (1 frame)
     on the reference side and it should drop back to 0. In CUDA mode it should
     be 0 with no Cache TOP.
3. The Info CHOP's `readback_total_ms` / `cuda_inject_ms` show the cost.

**`tex_quad`: routing**
Feed four different sources into Image 1–4 (for example Constant TOPs in red,
green, blue and yellow, or four movies at different resolutions). Assign
`videoIn1` … `videoIn4` in order. Each should land in its own quadrant:
1 top-left, 2 top-right, 3 bottom-left, 4 bottom-right. A 4:3 source shows a
tinted bar on each side.

**`tex_transform`: live video under animation**
Use a Movie File In on `videoIn1` and a Noise TOP (animated) on `videoIn2`.
Both should keep updating while the circle spins and the panel pulses, with
no tearing and no stall at the loop point.

**`tex_alpha`: premultiplication**
Use a Constant TOP at red with alpha 0.5 on `videoIn1`. The expected output
colours, from left to right, are:

| Band | Expected RGB |
|---|---|
| white | (255,127,127) |
| black | (128,0,0) |
| grey | (192,64,64) |
| magenta | (255,0,127) |

- **CPU path:** this should match exactly.
- **CUDA mode:** the plugin does not premultiply, so feed the TOP through a
  Pre-Multiply RGB by Alpha step first (e.g. a Level TOP). If the black band
  comes out (255,0,0), the input is going in straight-alpha.

## Editing the .riv

The source is a cloud Rive file, and a `.rev` backup needs a paid Rive plan, so
the exported `.riv` is what lives here. If you rebuild it, keep these in mind:

- The editor's runtime export drops every artboard except the first unless the
  artboard is marked **Component**. All four are marked for that reason.
- Image assets must be **embedded**. TDRive loads files without a
  `FileAssetLoader`, so referenced or hosted assets never resolve.
- After re-exporting, run `texture_io_test`. Its pixel coordinates assume the
  layout above (`spinClip` at left 190 / top 270, 540×540; `slidePanel` at
  900, 300, 800×450).
