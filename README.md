
# TDRive
<img width="1022" height="877" alt="Screenshot 2026-05-28 at 12 21 32 PM" src="https://github.com/user-attachments/assets/b0fea42f-d3de-4ad7-893c-2b9308697ee4" />

A TouchDesigner Custom TOP that renders [Rive](https://rive.app) animations
(`.riv` files) using Rive's official C++ runtime + GPU renderer.

- **macOS** (arm64) — Metal backend, ships as `TDRiveTOP.plugin`
- **Windows** (x64) — D3D11 backend, ships as `TDRiveTOP.dll`

## Layout

```
TDRive/
├── src/
│   ├── TDRiveTOP.{h,cpp}         # cross-platform shell
│   ├── IBackend.h                # backend interface
│   ├── backend_metal.mm          # macOS Metal backend
│   ├── backend_d3d11.cpp         # Windows D3D11 backend
│   └── Info.plist                # macOS bundle plist
├── td_sdk/
│   ├── TOP_CPlusPlusBase.hpp     # TD Custom Operator SDK headers
│   └── CPlusPlus_Common.hpp
├── scripts/
│   ├── build_rive.sh             # macOS Rive runtime build
│   ├── build_rive.bat            # Windows Rive runtime build
│   └── inspect_riv.mm            # diagnostic dumper (macOS only)
├── third_party/
│   └── rive-runtime/             # cloned by build_rive.{sh,bat}
└── CMakeLists.txt                # cross-platform plugin build
```

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
| Resolution     | Width × height of the output texture.                            |

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
row is `name` followed by `value`. An optional `name value` header row is
skipped if the first cell of row 0 is exactly `name` / `Name` / `key` /
`Key`. For each row:

- If a view-model property with that name exists, the value is coerced to
  the property's type (`string`, `number`, `bool`) and written. Triggers
  fire on a rising edge — when the cell content changes AND parses to a
  truthy value (`1`, `true`, `fire`, `on`, `yes`, or a positive number).
- Otherwise, the TOP falls back to `artboard->getTextRun(name, "")` so
  older files (named text runs, no view model) keep working.

The Info DAT lists `vm:string` / `vm:number` / `vm:bool` / `vm:trigger`
rows for each view-model property, alongside the SMI inputs. Use it as the
reference when populating your Strings DAT.

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
