# RigExec for Maya

Animate a RigExec rig in Maya 2027. The rig stays a USD layer, and nothing is
authored back to it. Maya holds the controls and their animation. RigExec
evaluates the rig, and Maya shows the deformed meshes.

## How it works

The rig runs inside Maya. Loading a rig exports it once: usdRig bakes it to a
poseable `.rigexec` file and describes what Maya needs to build around it.
From then on the `rigExecRig` node plays that bake with usdRig's runtime, in
Maya's own process. The runtime links no USD, so it doesn't clash with Maya's
own USD (25.11 with MayaUSD 0.37, which has the same library names as
usdRig's 26.08 build).

- `plug-ins/rigExecMaya.mll` contains the `rigExecRig` node. It poses the
  bake with every control channel and writes back three outputs:
  - each mesh's deformed points;
  - each mesh's world matrix;
  - each control's `offsetParentMatrix`. With the control's own
    translate/rotate/scale on top, this matrix lands the control exactly on
    the frame the rig computed.
- `export/rigexec_maya_export.py` runs once per load, under the usdRig
  checkout's own Python and USD (Maya's Python and USD settings are kept
  away from it). It bakes the rig with usdRig's `rigExecBake --poseable`, and
  writes a description of the controls, channels, meshes, materials, control
  picker, IK/FK limbs and space switches.
- `scripts/rigexec_maya` runs the export (`export.py`) and builds the Maya
  scene (`scene.Load`).

Exported rigs are kept in `cache/` in this module, or in `RIGEXEC_MAYA_CACHE`
when that is set. A saved scene points at its rig's bake there.

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

Saved scenes reopen normally, as long as the exported rig is still in the
cache. If it's gone, load the rig again.

## Playback cache

Every frame the node evaluates is kept, keyed by every channel value. A
lookup compares the whole key, so an edit to any channel misses and nothing
is ever invalidated or served stale.

Four times a second, the node checks whether a recent evaluation missed the
cache. If one did, it works out the frames of the playback range:
- it reads the animated channels at each frame;
- channels with no animation hold their current value.

A pool of background threads then evaluates the missing frames, nearest
the current frame first, each with its own copy of the rig. The node's own
copy stays free for interactive edits.

- `RIGEXEC_MAYA_FILL_WORKERS`: pool size (default: an eighth of the
  hardware threads, at most 8; `0` turns the pool off).
- `RIGEXEC_MAYA_CACHE_FRAMES`: frames kept per node (default 2000, about
  1 GB for the biped); the least recently used go first.
- `rigExecCache [-wait] <node>` returns `{cached, pending}`. With `-wait` it
  first blocks until the pool is done.

## Requirements

- Maya 2027 and its devkit (`devkitBase`). On Windows the Maya install
  itself carries the `include/` and `lib/` the build needs.
- A built usdRig checkout. The plug-in compiles in its runtime sources
  (`libs/rigExecRuntime`, `libs/rigExecBinary`), and loading a rig runs its
  `bin/_env.bat` (or `_env.sh`), its `rigexec` module and its `rigExecBake`
  tool. By default this is `../usdRig` beside this sidecar; set
  `RIGEXEC_ROOT` to use another.

## Build

From the sidecar root, `build_rigexecmaya.bat` runs this configure, with
`MAYA_DEVKIT_LOCATION` defaulting to the Maya 2027 install and `RIGEXEC_ROOT`
to `../usdRig`:

```bat
cmake -S rigExecMaya -B build-rigexecmaya -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DMAYA_DEVKIT_LOCATION=<path to devkitBase> -DRIGEXEC_ROOT=<usdRig>
cmake --build build-rigexecmaya
```

Rebuild the plug-in after updating usdRig, so its runtime reads the bakes
usdRig writes.

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

## Picker

**RigExec > Picker** opens the rig's control picker in a dockable window,
with one tab per panel (the biped has Face and Body). The picker comes from
the rig itself: its `RigExecPicker` prims are read when the rig loads and
kept on the `rigExecRig` node, so saved scenes keep it.

- Click a button to select its controls, or drag a box to select several.
  Shift toggles, Ctrl removes, and Alt adds the mirrored side.
- Selecting in the viewport highlights the matching buttons.
- The IK/FK switches flip a limb, and the space switches open a menu of
  spaces. Switches are matched, so nothing moves: switching to IK puts the
  IK control and pole where the FK arm or leg already is, switching to FK
  poses the FK controls onto the IK limb, and a new space keeps the control
  where it was. Each switch is one undo step and, with auto key on, is
  keyed on every channel it sets. The picker shows only the controls of the
  half of each limb that is active.
- **Zero Ctrls** returns the selected controls, or all of them, to the
  rig's default pose. **Ctrl Vis** shows or hides the controls.
- Ctrl+A selects every control on the panel, and F frames the selection.

The picker uses usdRig's picker rules and its IK/FK and space matching from
the usdRig checkout, so it behaves the same as in usdview.

## Limits

- Only TRS controls get Maya manipulators. The other avars are plain keyable
  attributes.
- Avars that a mover writes are not exposed, because overriding them would
  freeze the rig.
- Control shapes come from `guide:shape`, not from authored curves.
- An IK limb bends only at its hinge, so an FK pose that bends the elbow or
  knee off its hinge axis can't be matched exactly in IK.
- Buttons for parts Maya doesn't animate yet, such as the fingers, don't
  appear.
- Only UsdPreviewSurface materials come across, as Maya standard surfaces
  with their colors, roughness, metalness and opacity. Other shaders (such
  as the eye shader) show the default material. Textures are not carried
  over. Meshes get their `st` UVs.
- The rig is baked at one frame, and Maya supplies every pose: animation
  stored in the USD file itself isn't played.
- A channel the bake doesn't take as an input can be animated in Maya but
  doesn't move the rig; loading the rig warns how many there are.
