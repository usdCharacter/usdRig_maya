# Commands and nodes

## Nodes

### `rigExecRig`

Poses the rig with every control channel and drives the scene. Built by
[loading a rig](loading.md); one per rig.

| Attribute | |
| --- | --- |
| `time` | Maya's time, connected from `time1` |
| `inLinear[]`, `inAngle[]` | Every control channel, by the rig's channel index |
| `inMesh[]` | Each mesh's rest geometry |
| `rigFile` | The rig's bake (`.rigexec`) |
| `stagePath`, `rigPath` | The USD stage and rig the bake came from |
| `outMesh[]` | Each mesh, deformed |
| `outMeshMatrix[]` | Each mesh's world matrix |
| `outControlMatrix[]` | Each control's `offsetParentMatrix` |

It also carries the tables the scene was built from (channel and control
paths, parents, rotate orders, defaults and mesh placement) and, as
`rigData`, the picker, limbs and space switches.

### `rigExecHoverPicker`

A locator that draws the [hover picker](hover-picker.md) over the viewport.
Showing the hover picker adds it to the scene.

## Commands

### `rigExecCache`

```
rigExecCache [-wait] <rigExecRig node>
```

How many frames the [playback cache](playback.md) holds and how many are
still pending, as two integers. With `-wait`, it first waits until the
background threads are done.

### `rigExecFrames`

```
rigExecFrames <rigExecRig node> [-set <attribute path> <value>]... -frame <path>...
```

The asset-space frames (16 numbers each, row-major) of the rig's controls or
joints at the given paths, with the rig posed as Maya has it now and each
`-set` value on top. Nothing in the scene changes. Matched switches are built
on this.

```python
import maya.mel as mel
frame = mel.eval('rigExecFrames "Biped_stack_rigExec" '
                 '-frame "/Biped/Rig/Main/Shot/Aux/Controls/L_Arm"')
```

### `rigExecHoverImage`

Hands the hover picker's image to the viewport overlay. Used by the hover
picker; you won't need it directly.

### `RigExecToggleHoverPicker`

A runtime command that shows or hides the [hover picker](hover-picker.md),
under Custom Scripts > RigExec in the Hotkey Editor.
