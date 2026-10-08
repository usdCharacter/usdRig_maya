# Biped example: the RigExec biped in Maya

This walks through animating usdRig's biped character in Maya 2027 with the
`rigExecMaya` plug-in. The rig stays a USD layer. Maya holds the controls and
their animation, the rig runs inside Maya from a baked copy, and Maya shows
the deformed meshes. See [rigExecMaya/README.md](../rigExecMaya/README.md) for
how the plug-in works.

## 1. Lay out the checkouts

The plug-in finds RigExec by convention, with three siblings in one folder:

```
<dev>/
    usd-install/     OpenUSD 26.08 with OpenExec (what usdRig builds against)
    usdRig/          the RigExec checkout; holds the biped
    usdRig_maya/     this repository
```

To use a different usdRig checkout, set `RIGEXEC_ROOT` to its path before you
start Maya.

## 2. Build usdRig

The plug-in compiles in usdRig's runtime, and loading a rig bakes it with
usdRig's own Python, USD and `rigExecBake` tool, so usdRig must be built
first. Follow usdRig's README
("Build and run"). On Windows:

```bat
cd ..\usdRig
bin\build_rigexec.bat
```

To check it, run `bin\launch_usdview.bat examples\biped\Biped_stack.usda`.
It should open the biped in usdview.

## 3. Build the Maya plug-in

From this repository:

```bat
build_rigexecmaya.bat
```

This writes `rigExecMaya\plug-ins\rigExecMaya.mll`. It needs Maya 2027 and
Visual Studio 2022 (found through usdRig's `bin\_vcvars.bat`). The node
links the Maya devkit and usdRig's runtime, which it compiles from
`..\usdRig` (no USD). The separate devkit download is optional: by default
the script builds against `C:\Program Files\Autodesk\Maya2027`, which ships
the `include\` and `lib\` the build needs. Set `MAYA_DEVKIT_LOCATION` to use
a devkitBase instead.

Maya locks the `.mll` while the plug-in is loaded, so close Maya before you
rebuild. To build to another folder, set `RIGEXEC_MAYA_PLUGIN_DIR`.
Rebuild the plug-in whenever you update usdRig.

## 4. Launch Maya with the biped

```bat
launch_maya.bat ..\usdRig\examples\biped\Biped_stack.usda
```

This puts `rigExecMaya\` on `MAYA_MODULE_PATH` and starts Maya 2027. Once the
UI is up, it loads the biped, which takes a few seconds while usdRig bakes
the rig (into `rigExecMaya\cache\`). Without an argument, Maya starts with a **RigExec** menu,
and **RigExec > Load Rig...** opens any RigExec stage. Set `MAYA_VERSION` or
`MAYA_LOCATION` to use a different Maya.

## 5. What you get

Loading builds this scene:

```
Biped_stack            top group
    controls           the rig's control hierarchy, placed in asset space
    geo                the deformed meshes, with the rig's materials
Biped_stack_rigExec    the rigExecRig node that drives both
```

- **Controls** (261 on the biped) are the prims that author `guide:shape`.
  They show their curves, and you animate them with Maya's own tools: the
  translate, rotate and scale channels, plus other avars as keyable
  attributes (`avars:space` becomes `space`).
- **Constraint, aim and helper transforms** (`*_cst`, `*_aim`, `*_follow`,
  `*_auto`) are plain transforms with no shape. The rig positions them, so
  leave them alone.
- **Picker**: **RigExec > Picker** opens the biped's Face and Body picker
  panels. Click a button to select its controls, and use the switches to
  flip a limb between IK and FK or to change a control's space, without
  the limb or control moving. **RigExec > Hover Picker** puts the same
  buttons right in the viewport. See "Picker" in the plug-in README.
- **Playback**: frames are cached as they play. Background threads fill
  in the rest of the playback range. See "Playback cache" in
  the plug-in README.

Saved Maya scenes reopen normally while the baked rig is still in the
cache.

## Troubleshooting

- **"no rigExecMaya.mll"** from `launch_maya.bat`: run
  `build_rigexecmaya.bat` first.
- **"exporting ... failed"** when loading: the error shows usdRig's own
  output. Usually usdRig isn't built, or `../usdRig` isn't where step 1
  expects it. You can run the export outside Maya to see the same thing:

  ```bat
  rigExecMaya\export\launch_export.bat ..\usdRig ..\usdRig\examples\biped\Biped_stack.usda biped.rigexec biped.json
  ```
- **"cannot read the rig file"** on a saved scene: its baked rig is no longer
  in the cache. Load the rig again.
- **Warnings about `purpose 'guide'` nested under `'default'`** come from
  usdRig's rig compiler and are harmless.
- **After changing the plug-in's Python**, reload it inside Maya instead of
  restarting:

  ```python
  import importlib, maya.cmds as cmds, rigexec_maya.scene as s
  importlib.reload(s)
  cmds.delete("Biped_stack", "Biped_stack_rigExec")
  s.Load(r"<dev>/usdRig/examples/biped/Biped_stack.usda")
  ```
