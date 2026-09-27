
# TDRive
<img width="1022" height="877" alt="Screenshot 2026-05-28 at 12 21 32 PM" src="https://github.com/user-attachments/assets/b0fea42f-d3de-4ad7-893c-2b9308697ee4" />

A TouchDesigner Custom TOP that renders [Rive](https://rive.app) animations
(`.riv` files) using Rive's official C++ runtime + GPU renderer.

- **macOS** (arm64) — Metal backend, ships as `TDRiveTOP.plugin`
- **Windows** (x64) — D3D11 backend, ships as `TDRiveTOP.dll`

## Download Prebuilt Binaries

[Latest Versions](https://github.com/medcelerate/TDRive/releases/latest)

## Demo
https://github.com/user-attachments/assets/1ff6806b-c91e-4f86-8652-2938958cb0e0


## Prerequisites

**macOS**
- macOS 13.0+, Xcode command-line tools (`xcode-select --install`)
- `premake5` — `brew install premake`
- CMake 3.20+ — `brew install cmake`

**Windows**
- Visual Studio 2022 with the C++ workload (or VS Build Tools)
- `premake5.exe` on `PATH` — Chocolatey doesn't ship it; download the
  Windows zip from <https://github.com/premake/premake-core/releases>,
  extract `premake5.exe`, and put it somewhere on `PATH`.
- CMake 3.20+ — `choco install cmake`
- A "Developer Command Prompt for VS 2022" (or any shell with vcvars set)

## Build

Step 1 pulls in `rive-runtime` and builds its static libraries. This takes
5–10 minutes the first time and is only needed once per Rive commit.

**macOS**
```sh
./scripts/build_rive.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# -> build/TDRiveTOP.plugin
```

**Windows**
```bat
scripts\build_rive.bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
:: -> build\Release\TDRiveTOP.dll
```

### The CPython schema endpoint

On Windows the plugin exposes its property schema to Python (see
[Generating controls automatically](#generating-controls-automatically-rivecontroltox)),
which needs Python 3.11 headers and `python3.lib` at build time. CMake finds
them automatically from your newest TouchDesigner install, so a normal dev
build needs no extra flags. Two roots are accepted if you need to point it
elsewhere with `-DTD_PYTHON_ROOT=<path>`:

| Layout | Headers | Import library |
|---|---|---|
| TouchDesigner's bundled SDK | `Include/Python.h` (+ `Include/PC/`) | `lib/x64/python3.lib` |
| A stock CPython 3.11 install | `include/Python.h` | `libs/python3.lib` |

The second is what `actions/setup-python` produces, which is how CI builds it
— GitHub runners have no TouchDesigner to borrow the SDK from.

**If CMake cannot resolve a root, configuring fails.** That is deliberate. The
endpoint is compiled behind `#if defined(TDRIVE_PYTHON)`, so a build without
it produces a plugin that loads and renders perfectly but has no Python
attributes at all — indistinguishable from a broken install until someone
touches `propertySchema`. If you want that build, ask for it explicitly with
`-DTDRIVE_PYTHON=OFF`. To check any DLL you have been handed:

```sh
python scripts/verify_python_endpoint.py build/Release/TDRiveTOP.dll
python3 scripts/verify_python_endpoint.py build/TDRiveTOP.plugin/Contents/MacOS/TDRiveTOP
```

On macOS, CMake finds the headers in
`/Applications/TouchDesigner*.app/Contents/Frameworks/Python.framework/Versions/3.11`
(or any CPython 3.11 root passed as `TD_PYTHON_ROOT`). The plugin links no
libpython: its Python symbols bind at load time to the interpreter
TouchDesigner already has loaded, so one build runs on every TD 2023+ install.

## Install in TouchDesigner

Drop the build output into TouchDesigner's plugin search path:

- **macOS**: `~/Library/Application Support/Derivative/TouchDesigner099/Plugins/TDRiveTOP.plugin`
- **Windows**: `%USERPROFILE%\Documents\Derivative\TouchDesigner099\Plugins\TDRiveTOP.dll`

…then restart TouchDesigner. The operator shows up as `Rive` in the Custom
operators palette.

## Parameters

| Parameter      | Description                                                      |
| -------------- | ---------------------------------------------------------------- |
| Riv File       | File picker for the `.riv` file.                                 |
| Reload         | Pulse — reloads the file from disk.                              |
| Artboard       | Dynamic menu of artboards found in the file. Empty = default.    |
| State Machine  | Dynamic menu of state machines on the selected artboard.         |
| Inputs CHOP    | CHOP whose channels drive the state machine inputs (see below).  |
| Strings DAT    | Table DAT whose rows drive view-model properties / text runs.    |
| Fit            | Contain / Cover / Fill / Fit Width / Fit Height / None / Scale Down. |
| Alignment      | 3×3 anchor.                                                      |
| Speed          | Playback speed multiplier.                                       |
| Background Color | RGBA clear color. Set alpha = 0 for transparent output.        |

Output size comes from the TOP's built-in **Common** page (Output
Resolution / Resolution). "Use Input" means the artboard's own authored size.

> **Upgrading from v1.3.0 or earlier:** the custom **Resolution** parameter on
> the Rive page is gone. Projects that set it will come up at the artboard's
> size; set the size on the Common page instead.

## Driving state machine inputs

Custom-OP parameter lists in TouchDesigner are fixed at create-time, so we
can't generate one TD parameter per Rive input. Instead, inputs are driven
through a CHOP — channel names map onto state-machine inputs by name:

- **Number inputs** — channel value is written each cook.
- **Bool inputs** — channel value > 0 ⇒ `true`, else `false`.
- **Trigger inputs** — fires on a rising edge from ≤ 0 to > 0.

The TOP's **Info DAT** (middle-click → Info, or `op('rive1').opInfo`) lists
the active state machine's inputs by `index`, `name`, `type`, and current
`value`. Use that as the reference when naming the CHOP channels.

Example: if the Info DAT shows an input named `Speed` of type `number`,
make a Constant CHOP with one channel called `Speed`, set its value, and
reference that CHOP in **Inputs CHOP**.

## Driving strings (view models / text runs)

Newer Rive files use **data binding** through a view model attached to the
artboard — text on screen reads from view-model `string` / `number` /
`bool` / `trigger` properties rather than from named text runs. The TOP
auto-binds the artboard's default view model when one exists.

The **Strings DAT** parameter points at a Table DAT with two columns. Each
row is `name` followed by `value`. An optional header row is skipped if the
first cell of row 0 is exactly `name` / `Name` / `key` / `Key` /
`label` / `Label`. For each row:

- If a view-model property with that name exists, the value is coerced to
  the property's type (`string`, `number`, `bool`) and written. Triggers
  fire on a rising edge — when the cell content changes AND parses to a
  truthy value (`1`, `true`, `fire`, `on`, `yes`, or a positive number).
- Otherwise, if the selected state machine declares an input with that
  name, the value is applied to it. The **Inputs CHOP** stays the better
  path for *animated* numerics — no float→string→float round trip per
  frame — but this lets one DAT drive an entire artboard.
- Otherwise, the TOP falls back to `artboard->getTextRun(name, "")` so
  older files (named text runs, no view model) keep working.

The Info DAT lists `vm:string` / `vm:number` / `vm:bool` / `vm:trigger`
rows for each view-model property, alongside the SMI inputs. Use it as the
reference when populating your Strings DAT.

## Generating controls automatically (RiveControl.tox)

Filling a Strings DAT by hand gets old fast — `sanabrandv008.riv` exposes
46 properties. The TOP therefore publishes its schema to Python, and
`RiveControl.tox` in this repo turns that into parameters with one pulse.

Three read-only attributes on the node:

| Attribute | Returns |
|---|---|
| `schemaVersion` | Format version of the two below, so a consumer can detect drift. |
| `propertySchema` | Every addressable property: `index`, `source` (`smi`/`vm`), `path`, `type`, `value`, `options`, `container`. |
| `tdJSONPars` | The drivable subset, as TDJSON parameter dicts ready for `TDJSON.addParametersFromJSONList`. |

```python
for e in op('rive1').propertySchema:
    print(e['path'], e['type'], e['value'])
```

> **If those attributes raise `AttributeError`,** the node is fine — your DLL
> was built without the schema endpoint, so TouchDesigner never built a Python
> class for it. Confirm with
> `python scripts/verify_python_endpoint.py <your>.dll` and see
> [The CPython schema endpoint](#the-cpython-schema-endpoint).

The trick that makes `tdJSONPars` work without a lookup table: each entry
carries the **Rive property path in its `label`**, not its name. A path
like `payoffCard/barGraph1Label` is not a legal TouchDesigner parameter
name, but it is a perfectly legal label — so a Parameter DAT set to emit
labels (`name=False, label=True, header=False`) produces exactly the
two-column table the Strings DAT parameter already consumes.

**Using the component:** drop `RiveControl.tox` into your project, set its
**Rive TOP** parameter, and pulse **Build**. It generates one parameter per
addressable property (grouped onto a page per nested view model) and points
that TOP's Strings DAT at its own output. Build is get-or-create, so
re-running it after changing artboard or file adds and updates parameters
without disturbing values you have already set. **Clear** removes the
generated parameters — separate from Build precisely because it discards
their values, expressions and exports.

Properties with no write path (`vm:viewModel` containers, `vm:list`,
`vm:color`, `vm:image`, `vm:font`) are deliberately skipped rather than
generated as parameters that would do nothing; the Status parameter reports
how many.

## Injecting textures (view-model image properties)

Rive view models can expose **image** properties (`vm:image` in the Info
DAT). The **Textures** parameter page has four slots, each pairing an
**Image N TOP** (any TOP in your network) with an **Image N Property** (the
view-model image property it drives). Every cook, the TOP's pixels are
pushed into the Rive image, so video, Render TOPs, NDI — anything — can
feed artwork inside the .riv.

Transport:

- **Default (all platforms)**: a CPU download path (one frame of latency on
  injected textures, imperceptible in most setups). On Windows the rendered
  frame is read back through double-buffered staging, which also adds one
  frame of output latency in exchange for not stalling on the GPU.
- **Windows + NVIDIA, opt-in**: set the environment variable
  `TDRIVE_CUDA=1` before launching TouchDesigner to register the plugin in
  CUDA execute mode, where textures move GPU→GPU in both directions with
  **zero CPU copies**. Input TOPs must be RGBA 8-bit. The mode applies to
  every Rive TOP in the process. The Info CHOP's `cuda_mode` channel shows
  which mode loaded.

  In the Performance Monitor a node in CUDA mode shows ~5 ms of GPU cook time
  at 60 fps. That is TouchDesigner's GPU timer spanning an interop wait, not
  work: a bare CUDA-mode TOP that writes nothing shows ~4.2 ms too, and
  measured uncapped on an RTX 2070 SUPER with 1–8 nodes, CUDA mode
  matched or beat the default mode's frame rate (475 vs 390 fps with one
  node, equal at 4 and 8), used 25–30% less CPU cook time, had steadier frame
  times, and has no extra frame of latency.

The node's Info CHOP breaks the cost down per cook (milliseconds):
`render_ms`, `copy_ms`, `map_ms`, `memcpy_ms`, `readback_total_ms`, plus in
CUDA mode `unmap_ms`, `cuda_begin_ms`, `cuda_inject_ms` and `cuda_end_ms`.
`render_gpu_ms` is Rive's own GPU render time (Windows), and `out_w` /
`out_h` the size actually produced.

Note: Rive samples images as **premultiplied alpha**. The CPU path
premultiplies for you; in CUDA mode, premultiply upstream (e.g. a
Composite/Reorder TOP) if your input has transparency.

## How it works

- Cooks every frame.
- Creates a Rive Metal `RenderContext` (PLS renderer) on the system default
  Metal device.
- Allocates a private `MTLTexture` (`BGRA8Unorm`, render-target usage) at the
  configured resolution, plus a shared `MTLBuffer` for CPU readback.
- Each cook: parses the `.riv` if it changed, advances the active scene by
  `dt × speed`, draws into the offscreen texture, blits the texture into the
  readback buffer, then hands the bytes to TouchDesigner through
  `TOP_ExecuteMode::CPUMem`.

## Troubleshooting

- **Plugin doesn't show up in TD** — make sure you copied the `.plugin`
  bundle (folder), not just the inner binary. Also check Window → Errors
  for load-time messages.
- **Black output** — alpha = 0 in Background Color produces a transparent
  output. View it through a Composite TOP over a solid background to
  confirm the alpha is what you expect.
- **Garbled output above 1280 on a Non-Commercial license** — TouchDesigner
  Non-Commercial caps every TOP at 1280×1280. In CUDA mode the plugin detects
  the smaller buffer, renders at that size and shows a warning. In the
  default mode the plugin cannot see the cap (the C++ API doesn't expose it),
  so it renders at the requested size and TD reads the frame with the wrong
  row width. Keep both sides at 1280 or below on Non-Commercial.
