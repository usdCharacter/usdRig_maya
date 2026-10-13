# Troubleshooting

### "no rigExecMaya.mll" from `launch_maya.bat`

Build the plug-in first: run `build_rigexecmaya.bat`. See
[Install and build](install.md).

### "exporting ... failed" when loading a rig

The error shows usdRig's own output. Usually usdRig isn't built, or
`..\usdRig` isn't where the plug-in expects it (set
[`RIGEXEC_ROOT`](environment.md)). To see the same output outside Maya, run
the export by hand:

```bat
rigExecMaya\export\launch_export.bat ..\usdRig ..\usdRig\examples\biped\Biped_stack.usda biped.rigexec biped.json
```

### "cannot read the rig file" on a saved scene

Its baked rig is no longer in the cache. Load the rig again.

### Some channels "are not inputs of the baked rig"

Those channels can be animated in Maya but don't move the rig. If you just
updated usdRig, rebuild the plug-in and load the rig again.

### Warnings about `purpose 'guide'` nested under `'default'`

These come from usdRig's rig compiler while it loads the rig, and are
harmless.

### The hover picker doesn't show

- It shows in the viewport that had focus when you turned it on. Click in
  the viewport you want, then choose **RigExec > Hover Picker** again.
- It's drawn through a locator, so check that locators are shown in the
  viewport's **Show** menu.

### After changing the plug-in's Python

Reload it inside Maya instead of restarting:

```python
import importlib, maya.cmds as cmds, rigexec_maya.scene as s
importlib.reload(s)
cmds.delete("Biped_stack", "Biped_stack_rigExec")
s.Load(r"<dev>/usdRig/examples/biped/Biped_stack.usda")
```
