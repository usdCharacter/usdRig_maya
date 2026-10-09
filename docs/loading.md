# Loading a rig

Load a RigExec rig from **RigExec > Load Rig...**, or start Maya with one:

```bat
launch_maya.bat ..\usdRig\examples\biped\Biped_stack.usda
```

Or from Python:

```python
import rigexec_maya.scene
rigexec_maya.scene.Load(r"C:/path/to/Biped_stack.usda")
```

## What loading does

Loading takes a few seconds:

1. **Export.** usdRig bakes the rig into a `.rigexec` file and describes
   what Maya needs to build around it: the controls, channels, meshes,
   materials, the control picker, IK/FK limbs and space switches. This runs
   once, outside Maya, under the usdRig checkout's own Python and USD.
2. **Build.** The plug-in builds the Maya scene from that description and
   connects it to the rig.

From then on the rig runs inside Maya, with nothing else running alongside
it. See [How it works](how-it-works.md).

## The scene it builds

For the biped:

```
Biped_stack            top group
    controls           the rig's control hierarchy, placed in asset space
    geo                the deformed meshes, with the rig's materials
Biped_stack_rigExec    the rigExecRig node that drives both
```

- **Controls** mirror the rig's control hierarchy. Controls that author a
  `guide:shape` show their curves; constraint, aim and helper transforms are
  plain transforms with no shape. See [Animating](animating.md).
- **Meshes** carry the rig's `UsdPreviewSurface` materials as Maya Standard
  Surfaces, and their `st` UVs.
- **The `rigExecRig` node** poses the rig with every control channel and
  moves the meshes and controls.

## Where exported rigs are kept

Exported rigs go in `rigExecMaya\cache\`, or in
[`RIGEXEC_MAYA_CACHE`](environment.md) when that is set. A saved Maya scene
points at its rig's bake there.

!!! tip "Saved scenes"
    Saved scenes reopen normally while the exported rig is still in the
    cache. If it's gone, load the rig again.
