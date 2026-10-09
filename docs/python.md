# Python

The plug-in's Python package is `rigexec_maya`, on Maya's path once the
module is registered.

## Loading rigs

```python
import rigexec_maya.scene

node = rigexec_maya.scene.Load(r"C:/path/to/Biped_stack.usda")
```

`scene.Load(stagePath, rigPath=None, name=None)`
:   Exports the rig at `stagePath` and builds it. Returns its `rigExecRig`
    node. `rigPath` picks a `RigExecRoot` when the stage has more than one;
    `name` names the top group (by default, the stage's file name).

`scene.Build(description, name=None)`
:   Builds a rig that's already exported, from its description.

`export.Export(stagePath, rigPath=None)`
:   Bakes and describes a rig, and returns the description. Its `rigFile`
    names the bake.

## Pickers

```python
import rigexec_maya.picker
import rigexec_maya.hover

rigexec_maya.picker.Show()      # the docked picker
rigexec_maya.hover.Toggle()     # the hover picker, in the focused viewport
```

`picker.Show()`
:   Opens the docked [picker](picker.md) for the rigs in the scene.

`hover.Show(panel=None)`, `hover.Hide()`, `hover.Toggle(panel=None)`
:   Shows or hides the [hover picker](hover-picker.md). `panel` names a
    model panel; by default it's the viewport that has focus.

## Matching

```python
import rigexec_maya.matching
import rigexec_maya.picker

rig = rigexec_maya.picker.Rig("Biped_stack_rigExec")
plan = rigexec_maya.matching.Plan(
    rig, "/Biped/Rig/Main/Shot/Aux/Controls/L_Arm.avars:ikfk", 1.0)
# {attribute path: value}: the matched channels and the switch itself
```

`matching.Plan(rig, attrPath, value)`
:   The channel values that set the switch at `attrPath` to `value` without
    moving anything. Nothing is set; the picker applies a plan as one undo
    step.
