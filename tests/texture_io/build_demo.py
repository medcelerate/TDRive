# build_demo.py - builds a TouchDesigner network that runs texture_io.riv live.
#
# Run it inside TouchDesigner (Textport, or any DAT's run()):
#
#     RIV_PATH = '/path/to/TDRive/tests/texture_io/texture_io.riv'
#     exec(open('/path/to/TDRive/tests/texture_io/build_demo.py').read())
#
# It (re)creates /project1/texture_io_demo containing four video sources, one
# Rive TOP per test artboard wired to them through the Textures page, a
# passthrough difference check, and an "overview" Layout TOP of all four
# outputs. The Rive TOP plugin must already be loadable (Plugins folder next
# to the .toe, or TouchDesigner's user Plugins folder).
#
# Optional globals:
#   RIV_PATH  path to texture_io.riv (default: next to this script's usual
#             location under project.folder)
#   SNAP_DIR  if set, ~3 s after building, saves every Rive TOP and source as
#             PNG plus a report.json of node errors and sampled pixels there.

import json
import os

W, H = 1920, 1080

RIV_PATH = globals().get('RIV_PATH') or os.path.join(
    project.folder, 'tests', 'texture_io', 'texture_io.riv')
SNAP_DIR = globals().get('SNAP_DIR')


def setp(o, name, value, expr=False):
    """Set a parameter by name, tolerating the casing C++ plugins expose."""
    for n in (name, name.capitalize(), name.lower()):
        p = getattr(o.par, n, None)
        if p is not None:
            if expr:
                p.expr = value
            else:
                p.val = value
            return p
    raise AttributeError('%s has no parameter %s' % (o.path, name))


def res(o):
    o.par.outputresolution = 'custom'
    o.par.resolutionw = W
    o.par.resolutionh = H


def place(o, col, row):
    o.nodeX = col * 220
    o.nodeY = -row * 180


def create_rive(parent, name):
    last = None
    for t in ('riveTOP', 'RiveTOP', 'rive', 'Rive'):
        try:
            return parent.create(t, name)
        except Exception as e:  # noqa: BLE001 - try the next spelling
            last = e
    raise RuntimeError('Could not create a Rive TOP - is the TDRive plugin '
                       'loaded? (%s)' % last)


def label(parent, name, text_expr, under):
    t = parent.create(textTOP, name + '_label')
    res(t)
    setp(t, 'text', text_expr, expr=True)
    t.par.fontsizex = 110
    o = parent.create(overTOP, name)
    res(o)
    o.inputConnectors[0].connect(t)
    o.inputConnectors[1].connect(under)
    return t, o


root = op('/project1')
old = root.op('texture_io_demo')
if old is not None:
    old.destroy()
demo = root.create(containerCOMP, 'texture_io_demo')
demo.par.w = 1280
demo.par.h = 720

# --- video in ---------------------------------------------------------------
movie = demo.create(moviefileinTOP, 'movie')
movie.par.file = app.samplesFolder + '/Map/Count.mov'
res(movie)
l1, v1 = label(demo, 'v1', "'videoIn1  frame ' + str(absTime.frame)", movie)

noise = demo.create(noiseTOP, 'noise')
res(noise)
noise.par.mono = False
noise.par.tz.expr = 'absTime.seconds * 0.3'
l2, v2 = label(demo, 'v2', "'videoIn2'", noise)

ramp = demo.create(rampTOP, 'ramp')
res(ramp)
ramp.par.type = 'radial'
ramp.par.phase.expr = 'absTime.seconds * 0.5'
l3, v3 = label(demo, 'v3', "'videoIn3'", ramp)

const = demo.create(constantTOP, 'const')
res(const)
const.par.colorr.expr = 'abs(math.sin(absTime.seconds))'
const.par.colorg.expr = 'abs(math.sin(absTime.seconds * 0.7))'
const.par.colorb.expr = 'abs(math.cos(absTime.seconds * 0.5))'
l4, v4 = label(demo, 'v4', "'videoIn4'", const)

# Half-transparent red, for tex_alpha: over black it must read (128,0,0).
alpha_src = demo.create(constantTOP, 'alpha_src')
res(alpha_src)
alpha_src.par.colorr = 1
alpha_src.par.colorg = 0
alpha_src.par.colorb = 0
alpha_src.par.alpha = 0.5

sources = [(movie, l1, v1), (noise, l2, v2), (ramp, l3, v3), (const, l4, v4)]
for row, (base, lab, out) in enumerate(sources):
    place(lab, 0, row * 1.2)
    place(base, 0, row * 1.2 + 0.6)
    place(out, 1, row * 1.2 + 0.3)
place(alpha_src, 1, 5)

# --- Rive ------------------------------------------------------------------
wiring = {
    'tex_passthrough': [v1],
    'tex_quad':        [v1, v2, v3, v4],
    'tex_transform':   [v1, v2],
    'tex_alpha':       [alpha_src],
}
rives = {}
for row, (artboard, inputs) in enumerate(wiring.items()):
    r = create_rive(demo, 'rive_' + artboard[4:])
    setp(r, 'File', RIV_PATH)
    setp(r, 'Artboard', artboard)
    setp(r, 'Fit', 'contain')
    res(r)
    for i, src in enumerate(inputs, start=1):
        setp(r, 'Image%d' % i, src.path)
        setp(r, 'Imageprop%d' % i, 'videoIn%d' % i)
    place(r, 3, row * 1.4)
    r.viewer = True
    rives[artboard] = r

# Passthrough check: |output - input|. Non-zero on moving video on the CPU
# path, which injects the previous cook's download (one frame of latency).
diff = demo.create(compositeTOP, 'passthrough_diff')
diff.inputConnectors[0].connect(rives['tex_passthrough'])
diff.inputConnectors[1].connect(v1)
diff.par.operand = 'difference'
place(diff, 4, 0)
diff_max = demo.create(analyzeTOP, 'passthrough_diff_max')
diff_max.inputConnectors[0].connect(diff)
diff_max.par.op = 'maximum'
place(diff_max, 5, 0)

overview = demo.create(layoutTOP, 'overview')
res(overview)
for r in rives.values():
    overview.inputConnectors[len(overview.inputs)].connect(r)
place(overview, 5, 2)
overview.viewer = True
overview.display = True

out = demo.create(outTOP, 'out1')
out.inputConnectors[0].connect(overview)
place(out, 6, 2)

# Show the demo network in the first network editor pane.
try:
    for pane in ui.panes:
        if pane.type == PaneType.NETWORKEDITOR:
            pane.owner = demo
            pane.home()
            break
except Exception:  # noqa: BLE001 - UI is optional (e.g. no panes yet)
    pass

print('texture_io demo built in', demo.path, 'using', RIV_PATH)


# --- optional self-check ---------------------------------------------------
def _snapshot(demo_path, snap_dir):
    d = op(demo_path)
    os.makedirs(snap_dir, exist_ok=True)
    report = {'nodes': {}, 'samples': {}}
    for o in d.children:
        if o.family != 'TOP':
            continue
        entry = {'type': o.type, 'errors': o.errors(), 'warnings': o.warnings(),
                 'res': [o.width, o.height]}
        if o.name.startswith('rive_') or o.name in ('overview', 'alpha_src',
                                                     'v1', 'v2', 'v3', 'v4'):
            path = os.path.join(snap_dir, o.name + '.png')
            try:
                o.save(path)
                entry['png'] = path
            except Exception as e:  # noqa: BLE001
                entry['save_error'] = str(e)
        report['nodes'][o.name] = entry

    # TOP.sample() takes pixel coords from the bottom-left corner.
    def px(top, x, y):
        return [round(c * 255) for c in top.sample(x=x, y=H - 1 - y)]
    a = d.op('rive_alpha')
    if a is not None:
        report['samples']['alpha'] = {k: px(a, x, 540) for k, x in
                                      (('white', 240), ('black', 720),
                                       ('grey', 1200), ('magenta', 1680))}
    q = d.op('rive_quad')
    if q is not None:
        report['samples']['quad_centers'] = [px(q, 480, 270), px(q, 1440, 270),
                                             px(q, 480, 810), px(q, 1440, 810)]
    m = d.op('passthrough_diff_max')
    if m is not None:
        report['samples']['passthrough_diff_max'] = px(m, 0, H - 1)
    with open(os.path.join(snap_dir, 'report.json'), 'w') as f:
        json.dump(report, f, indent=2)
    print('texture_io demo snapshot written to', snap_dir)


if SNAP_DIR:
    run('args[0](args[1], args[2])', _snapshot, demo.path, SNAP_DIR,
        delayFrames=180)
