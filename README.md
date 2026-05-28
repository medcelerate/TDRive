# TDRive

A TouchDesigner Custom TOP that renders [Rive](https://rive.app) animations
(`.riv` files) using Rive's official C++ runtime + GPU renderer on Metal.

Only macOS is supported at the moment (the existing plugin examples in the
parent folder are macOS-only). Builds a `.plugin` bundle you drop into
TouchDesigner's plugin path.

## Layout

```
TDRive/
├── TDRive/                       # plugin source
│   ├── TDRiveTOP.mm              # the plugin
│   ├── TOP_CPlusPlusBase.hpp     # TD SDK header (copied from BGRemoverTOP)
│   ├── CPlusPlus_Common.hpp      # TD SDK header
│   └── Info.plist                # bundle plist
├── scripts/
│   └── build_rive.sh             # one-time Rive runtime build helper
├── third_party/
│   └── rive-runtime/             # cloned by build_rive.sh
└── Makefile                      # plugin build
```

## Prerequisites

- macOS 13.0+, Xcode command-line tools (`xcode-select --install`)
- `premake5` (`brew install premake`) – Rive's build system needs it
- A `.riv` file to test with

## Build

The first step pulls in `rive-runtime` and builds its static libraries. This
takes 5–10 minutes the first time and is only needed once per Rive version.

```sh
./scripts/build_rive.sh        # release build (default)
make                           # builds TDRiveTOP.plugin
```

The plugin lands at `build/release/TDRiveTOP.plugin`.

## Install in TouchDesigner

Drop `TDRiveTOP.plugin` into one of TouchDesigner's plugin search paths,
typically:

```
~/Library/Application Support/Derivative/TouchDesigner099/Plugins/
```

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

The readback buffer is reused across cooks, so the per-frame cost at typical
sizes (1080p–1440p) is a Rive draw + one blit + one `memcpy`.

## Why CPU readback instead of direct GPU sharing

The public TouchDesigner C++ TOP SDK exposes either a CPU buffer upload path
or CUDA. There is no public Metal interop, so direct GPU sharing isn't
available. The CPUMem upload is what the existing example plugins
(`BGRemoverTOP`) also use. If higher throughput is needed later, the path
would be Vulkan-mode TOPs sharing memory with a Vulkan Rive context — that's
a much larger lift.

## Troubleshooting

- **"No static libs found under …/out/release"** — run `scripts/build_rive.sh`
  first. If the Rive build itself fails, check that `premake5` is on `PATH`
  and that you have a recent enough clang (Xcode 14+).
- **Plugin doesn't show up in TD** — make sure you copied the `.plugin`
  bundle (folder), not just the inner binary. Also check Window → Errors
  for load-time messages.
- **Black output** — alpha = 0 in Background Color produces a transparent
  output. View it through a Composite TOP over a solid background to
  confirm the alpha is what you expect.

## License

The TouchDesigner SDK headers (`TOP_CPlusPlusBase.hpp`, `CPlusPlus_Common.hpp`)
are licensed by Derivative and copied verbatim from the SDK samples. Rive's
runtime is MIT-licensed. Plugin code in this repo is yours.
