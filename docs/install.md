# Install and build

## What you need

- **Maya 2027.** The build uses Maya's devkit headers and libraries. On
  Windows the Maya install itself carries the `include\` and `lib\` the build
  needs, so the separate devkit download is optional.
- **A built usdRig checkout.** The plug-in compiles in usdRig's runtime, and
  loading a rig bakes it with usdRig's own Python, USD and `rigExecBake`
  tool. See usdRig's README ("Build and run").
- **Visual Studio 2022, CMake 3.26 or later, and Ninja.**

## Lay out the checkouts

The plug-in finds usdRig by convention, with the checkouts side by side:

```
<dev>/
    usd-install/     OpenUSD 26.08 with OpenExec (what usdRig builds against)
    usdRig/          the RigExec checkout
    usdRig_maya/     this repository
```

To use a different usdRig checkout, set [`RIGEXEC_ROOT`](environment.md).

## Build usdRig

```bat
cd ..\usdRig
bin\build_rigexec.bat
```

## Build the plug-in

From this repository:

```bat
build_rigexecmaya.bat
```

This writes `rigExecMaya\plug-ins\rigExecMaya.mll`, where the module file
picks it up. It's the same as this configure, with `MAYA_DEVKIT_LOCATION`
defaulting to the Maya 2027 install and `RIGEXEC_ROOT` to `..\usdRig`:

```bat
cmake -S rigExecMaya -B build-rigexecmaya -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DMAYA_DEVKIT_LOCATION=<path to devkitBase> -DRIGEXEC_ROOT=<usdRig>
cmake --build build-rigexecmaya
```

!!! warning "Rebuild after updating usdRig"
    The plug-in compiles usdRig's runtime in, so rebuild it whenever you
    update usdRig. Otherwise its runtime may not read the bakes the newer
    usdRig writes.

!!! note "Close Maya before rebuilding"
    Maya locks `rigExecMaya.mll` while the plug-in is loaded. To build while
    Maya is open, set `RIGEXEC_MAYA_PLUGIN_DIR` to build somewhere else.

## Start Maya

```bat
launch_maya.bat [<rig.usda>]
```

This registers the module (`rigExecMaya\rigExecMaya.mod`) and starts Maya
2027. Given a rig, it loads it once Maya's UI is up. Without one, Maya starts
with a **RigExec** menu.

To register the module yourself instead, add the `rigExecMaya` directory to
`MAYA_MODULE_PATH` before Maya starts.

Next: [the biped tutorial](biped-example.md).
