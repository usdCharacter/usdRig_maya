# Environment variables

## Running

| Variable | Default | |
| --- | --- | --- |
| `RIGEXEC_ROOT` | `..\usdRig` beside this repository | The usdRig checkout that bakes rigs and whose picker rules the pickers use |
| `RIGEXEC_MAYA_CACHE` | `rigExecMaya\cache` | Where exported rigs are kept |
| `RIGEXEC_MAYA_FILL_WORKERS` | An eighth of the hardware threads, at most 8 | Background threads filling the [playback cache](playback.md); `0` turns them off |
| `RIGEXEC_MAYA_CACHE_FRAMES` | 2000 | Frames the playback cache keeps per rig |

## `launch_maya.bat`

| Variable | Default | |
| --- | --- | --- |
| `MAYA_VERSION` | `2027` | The Maya release to start |
| `MAYA_LOCATION` | The install for `MAYA_VERSION` | The Maya install to start |

## `build_rigexecmaya.bat`

| Variable | Default | |
| --- | --- | --- |
| `RIG` | `..\usdRig` beside this repository | The usdRig checkout: its compiler setup and runtime sources |
| `MAYA_VERSION` | `2027` | The Maya release to build for |
| `MAYA_DEVKIT_LOCATION` | The install for `MAYA_VERSION` | A devkitBase, or a Maya install carrying `include\` and `lib\` |
| `BUILD` | `build-rigexecmaya` | The build directory |
| `RIGEXEC_MAYA_PLUGIN_DIR` | `rigExecMaya\plug-ins` | Where the plug-in is written; point it elsewhere to build while Maya has the plug-in loaded |
