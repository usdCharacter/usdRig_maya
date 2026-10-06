# usdMayaRig

> This sidecar also carries **`rigExecMaya/`**, the Maya 2027 plug-in that
> animates a RigExec rig inside Maya (the `rigExecRig` node plus a worker
> process running `../usdRig`). Build it with `build_rigexecmaya.bat` and
> start Maya with `launch_maya.bat [<rig.usda>]`.
>
> **To set up and run the biped example, see
> [docs/biped-example.md](docs/biped-example.md).** How the plug-in works:
> [rigExecMaya/README.md](rigExecMaya/README.md).

A native, read-only OpenUSD `SdfFileFormat` plugin for Maya ASCII (`.ma`).
It depends on this repository's `rigExecRigging` authoring library and schema,
and produces an executable `RigExecRoot` plus native USD geometry and materials.
It does not require Maya, execute MEL/Python, or load Maya node plugins.

This is a **supported-subset translator**, not a replacement for Maya's DG
evaluation. Inspect diagnostics before treating a production rig as complete.

## Build and load

This sidecar is staged beside the usdRig checkout (`../usdMayaRig` next to
`../usdRig`) and builds standalone against a RigExec install; the live
usdRig tree carries no Maya sources. Two flavors, matching the main builds:

```bat
rem Against stock OpenUSD 26.08 (installs the stock RigExec package from
rem ..\usdRig\build on demand):
build_usdmayarig.bat

rem Against the USD vendored in Steam Houdini Indie:
build_usdmayarig_houdini.bat
```

Each script configures, builds, installs (to `build/install` or
`build-houdini/install`), and runs the two native tests (`testUsdMayaRig`,
`testUsdMayaRigEvaluation`). The Houdini flavor's evaluation test is a
known failure: the moved-pose check returns rest positions under vendored
USD 0.26.5 while passing against stock 26.08, a USD-version behavioral
delta rather than a sidecar regression.

The Houdini launchers (`usdRig/bin/launch_houdini_rigexec.bat` and the
combined `launch_houdini_both.bat`) require the Houdini sidecar install
and register it through `HOUDINI_USD_DSO_PATH`; their `check` mode opens
`tests/fixtures/rig.ma` through the format. The shared stock launcher
environments (`usdRig/bin/_env.bat`, `bin/_env.sh`) register the stock
build plugin (and the `usdview/` companion) whenever the sidecar was
built. In a custom host, put these directories on `PXR_PLUGINPATH_NAME`
(semicolon separated on Windows; colon separated on Linux/macOS):

* `<sidecar>/build/usd/usdMayaRig/resources` (or the matching
  `build-houdini` tree, or `lib/usd/usdMayaRig/resources` under an install
  prefix)
* the RigExec schema resources beside it
  (`lib/usd/rigExecSchema/resources` under the RigExec install the sidecar
  was built against)

USD and RigExec shared libraries must be discoverable by the platform loader;
on Windows add their library directories and the sidecar build directory to
`PATH`.

```python
from pxr import Sdf, Usd

stage = Usd.Stage.Open("character.ma")
print(stage.GetDefaultPrim().GetAttribute("maya:diagnostics").Get())
stage.GetRootLayer().Export("character.usda")

# Reject any reported unsupported feature or unresolved asset.
layer = Sdf.Layer.FindOrOpen("character.ma", {"strict": "1"})
stage = Usd.Stage.Open(layer)
```

`.ma` saving/editing is deliberately disabled. Export to `.usd`, `.usda`, or
`.usdc`, or author pose edits in a USD/session overlay. `ExportToString()` emits
USDA for inspection. An import failure leaves an existing layer unchanged.

The same standalone configure by hand, against any installed usdRig
package (the `.bat` scripts above are this plus the install/test
environment):

```sh
cmake -S . -B build-maya -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DrigExec_DIR=/path/to/rig/lib/cmake/rigExec
cmake --build build-maya
cmake --install build-maya --prefix /path/to/maya-plugin
```

The library installs in `lib`; registration installs in
`lib/usd/usdMayaRig/resources`. Register that resources directory alongside
RigExec's schema. The plugin links to `rigExec::rigExecRigging` in standalone
builds; it does not include or duplicate the rig engine's sources.

## Translation contract

| Maya data | USD result |
| --- | --- |
| Transform/joint DAG | Nested `RigExecControl` / `RigExecJoint` providers under `/Rig/Dag`, saved local matrices in `rest:space`; avars edit relative to that saved pose |
| Rotation order, rotate axis, joint orient, pivots, offset-parent matrix | Incorporated into the saved local bind/rest matrix |
| Direct translate/rotate/scale connections with identity local rest frames | Native avar channel connections |
| Point, orient, scale, parent constraints | Native RigExec constraint movers, source weights, channel masks and offsets; dependency-ordered mover stack |
| Polygon vertices, signed edges, faces | `UsdGeomMesh` under `/Rig/Geometry`, points in rig asset space, subdivision disabled |
| Stored mesh vertex tweaks | Sparse `.pt` offsets applied to input vertices before skinning; animated/connected tweaks are not evaluated |
| First UV set | Indexed, face-varying `primvars:st` |
| Cached 3D NURBS curves | `UsdGeomNurbsCurves`, knots, CVs and rational weights |
| Open NURBS ribbon surfaces and uvPin | `UsdGeomNurbsPatch`, native linear skinning, rational surface position/tangent evaluation in `RigExecNurbsSurfacePin` |
| Rotate-plane / single-chain IK | Native `RigExecSingleChainIkConstraint`, pole targets, virtual terminal joints and preserved bone orientation |
| Skin cluster | `RigExecSkinMover`, sparse Maya influence indices remapped to a dense palette, weights, normalization, envelope, linear or DQ skinning |
| Post-skin deltaMush | Native `RigExecDeltaMushMover`, explicit rest points, iterative smoothing, border pinning, distance weights, displacement and envelope |
| `bindPreMatrix` and `geomMatrix` | Bind-frame proxy joints and geometry bind placement; constraints follow the live DAG pose |
| Lambert, Blinn, Phong, standardSurface, aiStandardSurface | Approximate `UsdPreviewSurface` material: base color, opacity, roughness, metallic |
| File node directly driving base color | `UsdUVTexture` plus `UsdPrimvarReader_float2`; authored file assets and basic color space |
| Shading-engine membership | Whole-mesh binding or face subsets from `objectGrpCompList` |
| Implicit `lambert1` / `initialShadingGroup` | Default material and binding, including leading-colon names |
| Maya ASCII references | Recursively parsed, namespaced nodes and dependencies; cycles fail explicitly |
| Units / time unit / `upAxis` | USD stage meters-per-unit, frame rate and up-axis |

Maya references must use compatible length/angle units and up-axis. Different
units, binary references, or missing references are retained as dependencies
with diagnostics. References are flattened into the translated rig, while their
source paths remain discoverable through `GetExternalAssetDependencies()`.
Reference edits that require executing commands are not applied.

Asset identifiers are anchored through `ArResolver` to the file that authored
them, including referenced-file textures, so exported USD retains usable
dependencies. Assets are referenced, not copied or packaged.

## Diagnostics and retained source data

The root's `maya:diagnostics` string array reports untranslated node types,
connections, commands, unavailable geometry and assets, and known semantic
approximations. Layer custom data includes `mayaRig:source`,
`mayaRig:diagnostics`, `mayaRig:dependencies`, and `mayaRig:complete` (meaning
there were no reported diagnostics, **not** a proof of Maya equivalence).

`/Rig/Source/Nodes` retains each source node's type, name, ordered attribute
assignments, data types and values. Connection endpoints and remaining commands
are stored on `/Rig`; asset-bearing source attributes are also typed USD assets.
Names use collision-free escaping, including Maya namespaces. Ambiguous short
DAG names, malformed topology/arrays, non-finite numeric inputs, and cycles fail
the read rather than silently binding to the wrong node.

Known limits:

* No animation curves, expressions, arbitrary utility-node evaluation, spline IK,
  blendshapes, lattice or connected tweak deformation, construction-history evaluation,
  custom Maya nodes, reference edits, DAG instancing, or Maya binary files.
  Their graph data is retained; untranslated nodes/connections are diagnosed.
* Skinning needs one output mesh and stored input geometry. Geometry connections
  are followed through `groupParts` and topology-preserving `deltaMush` history;
  the upstream skin binding, influence palette and geometry bind matrix survive
  that history. Enabled deltaMush nodes become native operators after skinning.
  Iterative Laplacian smoothing is supported; nonzero contour constraints,
  nonidentity scale and painted deltaMush weights are diagnosed as unsupported.
  The native detail-frame convention has not been proven numerically equivalent
  to Maya's implementation.
  Blended linear/DQ skinning
  is diagnosed rather than approximated. Maya-specific DQ scale behavior has
  not been validated against Maya.
* Non-rigid rest matrices are preserved but diagnosed because RigExec
  orthonormalizes rest frames. Parent-scale compensation and
  `inheritsTransform=false` do not have full Maya-equivalent evaluation.
* Shader conversion approximates the listed surface inputs; procedural and
  layered shading, normal maps, placement nodes and additional connections are
  not evaluated. Extra UV/color sets are retained as source data with a
  diagnostic; polygon holes/legacy encodings fail explicitly. Authored split
  normals and edge smoothing are not currently translated.
* Multi-source rotation/scale constraint interpolation is diagnosed as an
  approximation. No production-rig/Maya parity claim is made.

## Validation

Native tests exercise actual plugin discovery without linking the reader,
strict mode, atomic failure, USD serialization, units, constraints, NURBS,
mesh topology/UVs/materials, sparse skin weights, namespaced references,
relative textures, asset dependency discovery, cycle rejection and inert
script preservation. The evaluator test moves a control through a constraint
and bind proxies and checks the resulting weighted mesh vertices.

Downloaded Autodesk example validation is reproducible with
`tests/proofDownloadedExamples.py` and `tests/proofMayaReference.py`. The latter
uses Maya only as an independent numerical reference; the reader itself remains
Maya-independent. See the generated `build/maya-example-proof/REPORT.md` for
source hashes, pose comparisons, and the explicit limits of that evidence.

The larger Autodesk rhino character is pinned in `tests/characterExample.json`.
Run `tests/proofCharacter.py --download-only`, obtain the Maya reference with
`tests/proofCharacterMaya.py`, then run `tests/proofCharacter.py` and
`tests/renderCharacterProof.py`. **This character currently fails parity**:
some utility connections remain unsupported and numerical deformation differences
remain. Native deltaMush, skin binding, ribbon pins and IK now
execute and deform the body. All five full reference comparisons still fail
the original tolerance; the complete deformation is not equivalent. The parity script
returns a nonzero exit status and writes measurements, rather than treating a
successful layer open or evaluator compilation as success. See
`build/maya-character-proof/REPORT.md` for the full measured failure report.

Character testing also exposed fixes for parent-before-child constraint
ordering, chunked face arrays, and stored sparse vertex tweaks. Eyes/lenses and
teeth now pass all five character poses. Control validation checks the original
54 Maya curve shapes and parent matrices; 53 pass the original tolerance and
one scapular shape differs by about 0.000154 cm. The body still fails and its
remaining deformation differences remain explicitly diagnosed.

Run `tests/proofNativeUsd.py` after conversion to check the exported USD alone
in a fresh process. It measures body motion from five native controls, checks
world translation, the foot IK endpoint, material bindings, and resetting the
pose. No Maya runtime or source scene is used. Open the exported USD with
`tests/inspectCharacter.py` to inspect it in usdview.

Native expression schemas declare the frames they read through the
`rigExec:poseInputs` relationship. The evaluator includes those providers in
the pose dependency closure and refreshes connected spaces after constraints.
Surface pins declare both the skin influences and their parent frame here.
IK uses `rigExec:orientationMode = "preserve"`; existing rigs retain the
default `aimX` behavior.

The companion `usdview` directory belongs on both `PYTHONPATH` and
`PXR_PLUGINPATH_NAME` (the repository launchers configure this). It makes
clicking an imported curve select its owning RigExec control. Maya group
transforms have their generic guide rings suppressed so the original control
shapes remain identifiable.
Installed layout: add `lib/python/usdMayaRigUsdview` to those two paths.

Format references: Autodesk's [Maya ASCII organization](https://download.autodesk.com/us/maya/2010help/files/Maya_ASCII_file_format_Organization_of_Maya_ASCII_files.htm)
and [setAttr data encodings](https://help.autodesk.com/cloudhelp/ENU/MayaCRE-Tech-Docs/Commands/setAttr.html).
