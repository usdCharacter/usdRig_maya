# RigExec for Maya

Animate a RigExec rig in Maya 2027. The rig stays a USD layer, and nothing is
authored back to it. Maya holds the controls and their animation. RigExec
evaluates the rig, and Maya shows the deformed meshes.

## How it works

Maya ships its own USD (25.11 with MayaUSD 0.37), which has the same library
names as RigExec's 26.08 build and an older OpenExec. The two cannot share a
process, so the rig runs in a **worker process** under RigExec's own Python and
USD. Maya talks to the worker over a pipe.

- `plug-ins/rigExecMaya.mll` contains the `rigExecRig` node (C++, devkit only,
  no USD). It reads every control channel and the time, asks the worker for
  the frame, and writes back three outputs:
  - each mesh's deformed points;
  - each mesh's world matrix;
  - each control's `offsetParentMatrix`. With the control's own
    translate/rotate/scale on top, this matrix lands the control exactly on
    the frame the rig computed.
- `worker/rigexec_maya_worker.py` opens the stage, compiles the rig, and
  evaluates it with Maya's channel values as interactive overrides. It sends
  only the channels that differ from the stage. It runs the baked program,
  which gives the same answers as the dynamic path several times faster.
  It also skips the per-point weight fields, which only feed a
  weight-painting overlay.
- `scripts/rigexec_maya` builds the Maya scene (`scene.Load`), defines the
  wire format (`protocol.py`), and starts the worker from Python
  (`client.py`). The worker is started with Maya's `PXR_*`, Python and USD
  environment removed. Otherwise the worker would load Maya's USD libraries.

`scene.Load` builds the following:
- one Maya transform per rig control, mirroring the control hierarchy. Only
  controls that author `guide:shape` get curves, drawn from their `guide:*`
  shape. Constraint, aim and helper transforms leave it unauthored and stay
  plain transforms;
- rotate order set from `avars:rotationOrder`;
- the translation, rotation and scale avars on Maya's own channels, and the
  other avars as keyable attributes of the same names (`avars:space` becomes
  `space`);
- the meshes the rig deforms;
- the `rigExecRig` node between them.

Saved scenes reopen normally, and the node starts its worker on first use.

## Playback cache

Every frame the node evaluates is kept, keyed by the full request: the time
and every channel value. A lookup compares the whole key, so an edit to any
channel misses and nothing is ever invalidated or served stale.

Four times a second, the node checks whether a recent evaluation missed the
cache. If one did, it works out the frames of the playback range:
- it reads the animated channels at each frame;
- channels with no animation hold their current value.

A pool of extra worker processes then evaluates the missing frames in
background threads, nearest the current frame first. The node's own worker
stays free for interactive edits.

On the biped, live playback runs at about 22 fps and playback from the
cache at about 140 fps. A 120-frame range fills in about 3.5 s, and a
cached frame matches a live evaluation exactly.

- `RIGEXEC_MAYA_FILL_WORKERS`: pool size (default: an eighth of the
  hardware threads, at most 8; `0` turns the pool off). Each worker holds
  its own copy of the rig, about 250 MB for the biped.
- `RIGEXEC_MAYA_CACHE_FRAMES`: frames kept per node (default 2000, about
  1 GB for the biped); the least recently used go first.
- `rigExecCache [-wait] <node>` returns `{cached, pending}`. With `-wait` it
  first blocks until the pool is done.

## Requirements

- Maya 2027 and its devkit (`devkitBase`). On Windows the Maya install
  itself carries the `include/` and `lib/` the build needs.
- A built usdRig checkout: the worker runs `bin/_env.bat` (or `_env.sh`)
  from it and imports its `rigexec` module. By default this is `../usdRig`
  beside this sidecar; set `RIGEXEC_ROOT` to use another.

## Build

From the sidecar root, `build_rigexecmaya.bat` runs this configure, with
`MAYA_DEVKIT_LOCATION` defaulting to the Maya 2027 install:

```bat
cmake -S rigExecMaya -B build-rigexecmaya -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DMAYA_DEVKIT_LOCATION=<path to devkitBase>
cmake --build build-rigexecmaya
```

The plug-in is written to `plug-ins/`, or to `RIGEXEC_MAYA_PLUGIN_DIR` when
that is set. `MAYA_DEVKIT_LOCATION` may also be set in the environment
instead.

## Use

For a step-by-step setup of the biped, from building usdRig onward, see
[docs/biped-example.md](../docs/biped-example.md).

From the sidecar root, `launch_maya.bat [<rig.usda>]` starts Maya with this
module registered and, given a rig, loads it. For the usdRig biped:

```bat
launch_maya.bat ..\usdRig\examples\biped\Biped_stack.usda
```

Or add this directory to `MAYA_MODULE_PATH` (it holds `rigExecMaya.mod`) and
start Maya. Then load a rig in either of these ways:
- **RigExec > Load Rig...**;
- from Python:

  ```python
  import rigexec_maya.scene
  rigexec_maya.scene.Load(r"C:/path/to/Biped_stack.usda")
  ```

The worker's log is `rigexec_maya_worker.log` in the temp directory.

## Picker

**RigExec > Picker** opens the rig's control picker in a dockable window,
with one tab per panel (the biped has Face and Body). The picker comes from
the rig itself: its `RigExecPicker` prims are read when the rig loads and
kept on the `rigExecRig` node, so saved scenes keep it.

- Click a button to select its controls, or drag a box to select several.
  Shift toggles, Ctrl removes, and Alt adds the mirrored side.
- Selecting in the viewport highlights the matching buttons.
- The IK/FK switches flip a limb, and the space switches open a menu of
  spaces. The picker shows only the controls of the half of each limb that
  is active. With auto key on, a switch is keyed.
- **Zero Ctrls** returns the selected controls, or all of them, to the
  rig's default pose. **Ctrl Vis** shows or hides the controls.
- Ctrl+A selects every control on the panel, and F frames the selection.

The picker uses usdRig's picker rules from the usdRig checkout, so it
behaves the same as in usdview.

## Limits

- Only TRS controls get Maya manipulators. The other avars are plain keyable
  attributes.
- Avars that a mover writes are not exposed, because overriding them would
  freeze the rig.
- Control shapes come from `guide:shape`, not from authored curves.
- Picker switches set IK/FK and spaces without matching, so the limb or
  control may jump. Buttons for parts Maya doesn't animate yet, such as the
  fingers, don't appear.
- Only UsdPreviewSurface materials come across, as Maya standard surfaces
  with their colors, roughness, metalness and opacity. Other shaders (such
  as the eye shader) show the default material. Textures are not carried
  over. Meshes get their `st` UVs.
- An uncached frame costs one evaluation of the rig: about 27 ms for a drag
  on the biped, 36 ms when the time changes too.
