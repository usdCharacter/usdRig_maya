# Maya ASCII translator

This repository also carries **usdMayaRig**, a separate, read-only OpenUSD
file format for Maya ASCII (`.ma`) files. It's the other direction from the
Maya plug-in: it turns a Maya rig into an executable RigExec rig, with no
Maya needed.

```python
from pxr import Usd

stage = Usd.Stage.Open("character.ma")
stage.GetRootLayer().Export("character.usda")
```

It translates a supported subset of Maya: the transform and joint DAG,
point, orient, scale and parent constraints, rotate-plane IK, skin clusters,
delta mush, NURBS curves and ribbons, polygon meshes, UVs and basic
materials, and Maya ASCII references. Anything it doesn't translate is kept
as source data and reported in the rig's `maya:diagnostics`. It's not a
replacement for Maya's own evaluation, so check the diagnostics before
treating a production rig as complete.

Build it with `build_usdmayarig.bat`. For the full translation contract,
diagnostics and validation, see the repository's README.
