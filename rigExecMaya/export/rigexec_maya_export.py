"""
Exports a RigExec rig for Maya: the baked rig the plug-in evaluates, and a
description of everything Maya builds around it.

Runs once per load, under the Python that owns the rig's USD build (OpenUSD
26.08 with OpenExec), never inside Maya:

    python rigexec_maya_export.py <stage.usda> <out.rigexec> <out.json>
                                  [<rig prim path>]

The .rigexec is usdRig's bake of the rig (rigExecBake): the plug-in poses
it with Maya's channel values, in Maya's own process and without USD. The
JSON describes the controls, channels, meshes, materials, control picker,
IK/FK limbs, space switches and TouchPose regions.
"""

import json
import os
import subprocess
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade
import rigexec

# Control avars that configure the control rather than pose it.
_NON_CHANNELS = {"avars:unitScaleFactor"}
_CHANNEL_TYPES = {Sdf.ValueTypeNames.Double, Sdf.ValueTypeNames.Float,
                  Sdf.ValueTypeNames.Int, Sdf.ValueTypeNames.Bool}
# The one frame the rig is baked at. Maya supplies every pose, so the
# stage's own animation, if any, is not used.
BAKE_FRAME = 1.0


def _Matrix(m):
    return [m[i][j] for i in range(4) for j in range(4)]


class Exporter:
    def __init__(self, stagePath, rigPath=None):
        self.stage = Usd.Stage.Open(stagePath)
        if not self.stage:
            raise RuntimeError("could not open %s" % stagePath)
        if rigPath is None:
            roots = [p.GetPath() for p in self.stage.Traverse()
                     if p.GetTypeName() == "RigExecRoot"]
            if not roots:
                raise RuntimeError("no RigExecRoot in %s" % stagePath)
            rigPath = roots[0]
        self.rigPath = Sdf.Path(str(rigPath))
        self.rig = rigexec.Rig(self.stage, str(self.rigPath))
        self.rig.compile()
        # The per-point weight fields only feed a weight-painting overlay.
        self.rig.publish_weight_fields = False
        self.time = Usd.TimeCode(BAKE_FRAME)
        assetRoot = self.stage.GetPrimAtPath(self.rigPath.GetParentPath())
        self.assetToWorld = UsdGeom.Xformable(assetRoot) \
            .ComputeLocalToWorldTransform(self.time) \
            if assetRoot.IsA(UsdGeom.Xformable) else Gf.Matrix4d(1.0)
        self._Collect()

    # -- description ---------------------------------------------------

    def _Collect(self):
        written = set()
        for prim in self.stage.Traverse():
            rel = prim.GetRelationship("rigExec:moves")
            if rel:
                for target in rel.GetTargets():
                    written.add(target)
        rest = self.rig.evaluate(BAKE_FRAME)
        framed = set(rest.control_paths())
        providers = set(rest.provider_paths())

        self.controls = []
        self.channels = []
        for prim in Usd.PrimRange(self.stage.GetPrimAtPath(self.rigPath)):
            if prim.GetTypeName() != "RigExecControl":
                continue
            path = prim.GetPath()
            if str(path) not in framed:
                continue
            index = len(self.controls)
            parent = path.GetParentPath()
            parentIndex = next((i for i in range(index - 1, -1, -1)
                                if self.controls[i]["path"] == str(parent)),
                               -1)
            sign = prim.GetAttribute("avars:rotationSign")
            sign = sign.Get() if sign else None
            self.controls.append({
                "path": str(path),
                "name": prim.GetName(),
                "parent": parentIndex,
                "rotationOrder": str(prim.GetAttribute(
                    "avars:rotationOrder").Get() or "XYZ"),
                # +1/-1 per axis: a mirrored limb's avar turns its side the
                # same way (the effective angle is the value times this).
                "rotationSign": [-1.0 if float(v) < 0.0 else 1.0
                                 for v in (sign or (1.0, 1.0, 1.0))],
                "guide": self._Guide(prim),
            })
            for attr in prim.GetAttributes():
                name = attr.GetName()
                if (not name.startswith("avars:") or name in _NON_CHANNELS
                        or attr.GetTypeName() not in _CHANNEL_TYPES
                        or attr.GetPath() in written):
                    continue
                value = attr.Get(self.time)
                self.channels.append({
                    "control": index,
                    "attr": name,
                    "type": str(attr.GetTypeName()),
                    "default": float(value) if value is not None else 0.0,
                })

        # The eyes: each surface projector publishes its eye's frame, and its
        # dials, as matrix primvars on the mesh it moves.
        projectors = {}
        for prim in self.stage.Traverse():
            if prim.GetTypeName() != "RigExecSurfaceProjector":
                continue
            frame = prim.GetAttribute("rigExec:shaderPrimvar")
            frame = frame.Get() if frame else None
            if not frame:
                continue
            dials = prim.GetAttribute("rigExec:shaderDialPrimvar")
            dials = dials.Get() if dials else None
            for target in prim.GetRelationship("rigExec:moves").GetTargets():
                mesh = str(target.GetPrimPath())
                projectors[mesh] = {
                    "projector": "%s.primvars:%s" % (mesh, frame),
                    "dials": "%s.primvars:%s" % (mesh, dials) if dials
                    else None,
                }

        self.meshes = []
        cache = UsdGeom.XformCache(self.time)
        for prim in self.stage.Traverse():
            if prim.GetTypeName() != "Mesh":
                continue
            mesh = UsdGeom.Mesh(prim)
            if UsdGeom.Imageable(prim).ComputeVisibility(self.time) == \
                    UsdGeom.Tokens.invisible:
                continue
            if UsdGeom.Imageable(prim).ComputePurpose() not in (
                    UsdGeom.Tokens.default_, UsdGeom.Tokens.render):
                continue
            points = mesh.GetPointsAttr().Get(self.time) or []
            provider, local = self._ProviderOf(prim, providers, cache)
            entry = {
                "path": str(prim.GetPath()),
                "name": prim.GetName(),
                "pointCount": len(points),
                "counts": list(mesh.GetFaceVertexCountsAttr().Get() or []),
                "indices": list(mesh.GetFaceVertexIndicesAttr().Get() or []),
                "points": [c for p in points for c in p],
                "leftHanded": mesh.GetOrientationAttr().Get() ==
                UsdGeom.Tokens.leftHanded,
                "moved": prim.GetAttribute("points").GetPath() in written,
            }
            entry.update(self._Uvs(prim))
            entry["materials"] = self._Materials(prim)
            # Placement: the rig moves a mesh whose ancestor a constraint
            # moves (world = local * provider * assetToWorld); any other
            # mesh keeps its world matrix.
            entry["pointsProperty"] = str(prim.GetPath()) + ".points"
            entry["provider"] = provider or ""
            entry["local"] = _Matrix(local) if local is not None else None
            entry["world"] = _Matrix(cache.GetLocalToWorldTransform(prim))
            if str(prim.GetPath()) in projectors:
                entry["eyeProjector"] = projectors[str(prim.GetPath())]
            self.meshes.append(entry)
        self.written = written

    def _ProviderOf(self, prim, providers, cache):
        """The nearest ancestor-or-self a constraint moves, and the prim's
        rest transform relative to it."""
        walk = prim
        while walk and walk.GetPath() != Sdf.Path.absoluteRootPath:
            if str(walk.GetPath()) in providers:
                local = cache.GetLocalToWorldTransform(prim) * \
                    cache.GetLocalToWorldTransform(walk).GetInverse()
                return str(walk.GetPath()), local
            walk = walk.GetParent()
        return None, None

    def _Uvs(self, prim):
        primvar = UsdGeom.PrimvarsAPI(prim).GetPrimvar("st")
        if not primvar or not primvar.HasValue():
            return {}
        values = primvar.Get(self.time)
        if values is None:
            return {}
        interpolation = primvar.GetInterpolation()
        if interpolation not in (UsdGeom.Tokens.faceVarying,
                                 UsdGeom.Tokens.vertex):
            return {}
        indices = primvar.GetIndices(self.time) if primvar.IsIndexed() \
            else None
        return {
            "uvInterpolation": str(interpolation),
            "uvs": [c for uv in values for c in uv],
            "uvIndices": list(indices) if indices is not None else None,
        }

    def _Materials(self, prim):
        """The mesh's bound materials: the whole-mesh one first, then one
        per material subset with the faces it covers."""
        found = []
        whole = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()[0]
        if whole:
            found.append(self._Material(whole, None))
        binding = UsdShade.MaterialBindingAPI(prim)
        for subset in binding.GetMaterialBindSubsets():
            bound = UsdShade.MaterialBindingAPI(subset.GetPrim()) \
                .ComputeBoundMaterial()[0]
            if bound:
                faces = subset.GetIndicesAttr().Get() or []
                found.append(self._Material(bound, [int(f) for f in faces]))
        return found

    def _Material(self, material, faces):
        """A material's UsdPreviewSurface inputs, or no surface when it uses
        another shader (Maya then shows its default)."""
        surface = None
        source = material.ComputeSurfaceSource()[0]
        if source and source.GetIdAttr().Get() == "UsdPreviewSurface":
            def get(name, fallback):
                shaderInput = source.GetInput(name)
                value = shaderInput.Get() if shaderInput else None
                return fallback if value is None else value

            diffuse = get("diffuseColor", Gf.Vec3f(0.18))
            emissive = get("emissiveColor", Gf.Vec3f(0.0))
            surface = {
                "diffuseColor": [float(c) for c in diffuse],
                "emissiveColor": [float(c) for c in emissive],
                "metallic": float(get("metallic", 0.0)),
                "roughness": float(get("roughness", 0.5)),
                "opacity": float(get("opacity", 1.0)),
                "ior": float(get("ior", 1.5)),
            }
        elif source and source.GetImplementationSource() == "sourceAsset":
            # A glslfx shader Maya can't run. The procedural eye is the one
            # the biped uses; its inputs are kept so Maya builds the same
            # iris and pupil from them (scene._EyeShadingGroup).
            asset = source.GetSourceAsset("glslfx")
            assetPath = asset.path if asset else ""
            if os.path.basename(assetPath).startswith("sbe_eye"):
                inputs = {i.GetBaseName(): i.Get()
                          for i in source.GetInputs()}
                surface = {"eye": {k: float(v) for k, v in inputs.items()
                                   if isinstance(v, (int, float))
                                   and not isinstance(v, bool)},
                           "eyeColors": {}, "eyeTextures": {}}
                for name, value in inputs.items():
                    if isinstance(value, (Gf.Vec3f, Gf.Vec3d)):
                        surface["eyeColors"][name] = [float(c) for c in value]
                    elif isinstance(value, Sdf.AssetPath):
                        resolved = value.resolvedPath or value.path
                        if resolved:
                            surface["eyeTextures"][name] = resolved
        return {"name": material.GetPrim().GetName(), "surface": surface,
                "faces": faces}

    def _Guide(self, prim):
        # Only animator controls author a shape. Constraint, aim and helper
        # transforms leave guide:shape at the schema's "circle" fallback and
        # stay plain transforms in Maya.
        shapeAttr = prim.GetAttribute("guide:shape")
        if not shapeAttr or not shapeAttr.HasAuthoredValue():
            return None

        def get(name, fallback):
            attr = prim.GetAttribute(name)
            value = attr.Get(self.time) if attr else None
            return fallback if value is None else value

        offset = get("guide:offset", Gf.Vec3d(0.0))
        orient = Gf.Matrix3d(Gf.Rotation(Gf.Quatd(
            get("guide:orient", Gf.Quatf(1.0)))))
        color = get("guide:displayColor", Gf.Vec3f(1.0, 1.0, 0.0))
        return {
            "shape": str(get("guide:shape", "circle")),
            "planeNormal": str(get("guide:planeNormal", "Y")),
            "scale": [float(get("guide:scaleX", 1.0)),
                      float(get("guide:scaleY", 1.0)),
                      float(get("guide:scaleZ", 1.0))],
            "offset": [float(offset[0]), float(offset[1]), float(offset[2])],
            # Row-vector rows: a sized point p goes to p * orient.
            "orient": [[float(orient[r][c]) for c in range(3)]
                       for r in range(3)],
            "color": [float(color[0]), float(color[1]), float(color[2])],
        }

    def Describe(self):
        return {
            "bakeFrame": BAKE_FRAME,
            "stage": self.stage.GetRootLayer().identifier,
            "rigPath": str(self.rigPath),
            "upAxis": str(UsdGeom.GetStageUpAxis(self.stage)),
            "metersPerUnit": UsdGeom.GetStageMetersPerUnit(self.stage),
            "timeCodesPerSecond": self.stage.GetTimeCodesPerSecond(),
            # Control matrices are asset-space; this places the asset.
            "assetToWorld": _Matrix(self.assetToWorld),
            "controls": self.controls,
            "channels": self.channels,
            "meshes": self.meshes,
            "pickers": self.Pickers(),
            "limbs": self.Limbs(),
            "spaces": self.Spaces(),
            "touch": self.TouchRegions(),
        }

    # -- TouchPose ---------------------------------------------------------

    def TouchRegions(self):
        """The rig's TouchPose layers: per layer, the mesh it paints (by
        index into `meshes`), its regions (faces and the control each one
        selects, by index into `controls`) and the colours the three
        highlight states take. The rules are usdRig's touchPoseModel:
        typed properties first, the older custom touchpose:* ones as the
        fallback, and a region that selects no exported control dropped."""
        meshIndex = {m["path"]: i for i, m in enumerate(self.meshes)}
        controlIndex = {c["path"]: i for i, c in enumerate(self.controls)}

        def value(prim, typed, custom):
            for name in (typed, custom):
                if not name:
                    continue
                attr = prim.GetAttribute(name)
                got = attr.Get() if attr and attr.IsValid() else None
                if got is not None and (not hasattr(got, "__len__")
                                        or len(got)):
                    return got
            return None

        def targets(prim, typed, custom):
            for name in (typed, custom):
                if not name:
                    continue
                rel = prim.GetRelationship(name)
                found = rel.GetTargets() if rel else []
                if found:
                    return [str(t) for t in found]
            return []

        def color(c, fallback):
            return [float(x) for x in c] if c is not None else list(fallback)

        layers = []
        for scope in self.stage.Traverse():
            if scope.GetTypeName() != "RigExecTouchRegions":
                continue
            meshPath = (targets(scope, "rigExec:touch:mesh", "") or
                        [str(value(scope, "", "touchpose:mesh") or "")])[0]
            if meshPath not in meshIndex:
                continue
            regions = []
            for child in scope.GetChildren():
                if child.GetTypeName() != "RigExecTouchRegion" and \
                        not child.HasAttribute("touchpose:faces"):
                    continue
                control = targets(child, "rigExec:touch:control",
                                  "touchpose:control")
                if not control or control[0] not in controlIndex:
                    continue
                faces = value(child, "rigExec:touch:faces", "touchpose:faces")
                hilight = child.GetAttribute("touchpose:hilight")
                hilight = hilight.Get() if hilight and hilight.IsValid() \
                    else 0
                regions.append({
                    "name": child.GetName(),
                    "control": controlIndex[control[0]],
                    "faces": [int(f) for f in (faces or [])],
                    "color": color(child.GetAttribute("touchpose:color").Get()
                                   if child.HasAttribute("touchpose:color")
                                   else None, (1.0, 0.6, 0.0)),
                    "hilight": int(hilight or 0),
                })
            if not regions:
                continue
            order = scope.GetAttribute("rigExec:touch:layerOrder")
            order = order.Get() if order and order.IsValid() else 0
            name = value(scope, "rigExec:touch:layerName", "")
            palette = value(scope, "rigExec:touch:palette",
                            "touchpose:palette")
            alpha = value(scope, "rigExec:touch:alpha", "touchpose:alpha")
            layers.append({
                "name": str(name) if name else scope.GetName(),
                "order": int(order or 0),
                "mesh": meshIndex[meshPath],
                "regions": regions,
                "palette": [color(c, (1, 1, 1)) for c in (palette or [])],
                "alpha": float(alpha) if alpha is not None else 1.0,
                "leadColor": color(value(scope, "", "touchpose:leadColor"),
                                   (0.054, 0.420, 0.187)),
                "selectedColor": color(
                    value(scope, "", "touchpose:selectedColor"),
                    (0.277, 0.277, 0.277)),
            })
        layers.sort(key=lambda l: (l["order"], l["name"]))
        return layers

    def Pickers(self):
        """The rig's control pickers, as the records usdRig's picker model
        reads: panels and buttons, read by usdRig's own picker reader.
        Empty when the rig has no picker or usdRig has no reader."""
        try:
            # Imported only now: the reader looks its schema types up on
            # import, and they exist once the stage has loaded them.
            import pickerScene
        except ImportError:
            return []
        pickers = []
        for prim in pickerScene.find(self.stage):
            rig = pickerScene.rig_for(prim)
            if rig is not None and rig.GetPath() != self.rigPath:
                continue
            panels, items = [], []
            for panel in sorted((c for c in prim.GetChildren()
                                 if c.IsA(pickerScene.PANEL_TYPE)),
                                key=pickerScene._order):
                panelId = str(panel.GetPath())
                size = pickerScene._v(panel, "ui:size", (400.0, 600.0))
                panels.append({
                    "id": panelId,
                    "label": (str(pickerScene._v(panel, "ui:label", ""))
                              or panel.GetName()),
                    "w": float(size[0]), "h": float(size[1]),
                    "fill": pickerScene._rgba(
                        pickerScene._v(panel, "ui:background"),
                        (41, 41, 41, 255)),
                })
                items.extend(pickerScene._button_record(button, panelId)
                             for button in panel.GetChildren()
                             if button.IsA(pickerScene.BUTTON_TYPE))
            pickers.append({
                "name": (str(pickerScene._v(prim, "ui:label", ""))
                         or prim.GetName()),
                "panels": panels,
                "items": items,
            })
        return pickers

    # -- switching without moving ---------------------------------------

    def Limbs(self):
        """Every IK/FK limb, found by usdRig's own matching code. The rig
        publishes the values a matched switch writes (its match
        constraint's outputs:ikValues and outputs:fkValues); this names
        the channels they land on, and which of them a switch may write."""
        try:
            import ikfkMatch
        except ImportError:
            return []

        def path(p):
            return str(p) if p is not None else ""

        limbs = []
        for limb in ikfkMatch.FindLimbs(self.stage):
            if limb.rigRoot != self.rigPath:
                continue
            limbs.append({
                "match": str(limb.match),
                "switch": str(limb.switchPath),
                "ikValue": float(limb.ikValue),
                "fkControls": [str(p) for p in limb.fkControls],
                "ikControl": str(limb.ikControl),
                "pole": path(limb.pole),
                "upperScale": path(limb.upperScale),
                "lowerScale": path(limb.lowerScale),
                "pin": path(limb.pin),
                "twist": path(limb.twist),
                "writable": sorted(str(p) for p in (limb.writable or ())),
            })
        return limbs

    def _SpaceLabels(self):
        """{space attribute path: its spaces' labels}, found in one walk."""
        if getattr(self, "_spaceLabels", None) is None:
            self._spaceLabels = {}
            for prim in self.stage.Traverse():
                if prim.GetTypeName() != "RigExecSpaceSwitch":
                    continue
                rel = prim.GetRelationship("rigExec:activeSpaceAttribute")
                attr = prim.GetAttribute("rigExec:spaceLabels")
                labels = [str(l) for l in (attr.Get() or [])] if attr else []
                for target in (rel.GetTargets() if rel else []):
                    self._spaceLabels[str(target)] = labels
        return self._spaceLabels

    def Spaces(self):
        """{space attribute path: {"control": the control it places,
        "writable": the channels of it a matched switch may write}}. The
        rig publishes each control's match row per space
        (spaces:matchValues)."""
        try:
            import spaceMatch
        except ImportError:
            return {}
        spaces = {}
        for active, switch in spaceMatch.Switches(self.stage).items():
            if not switch.control.HasPrefix(self.rigPath):
                continue
            labels = self._SpaceLabels().get(str(active), [])
            spaces[str(active)] = {"control": str(switch.control),
                                   "writable": list(switch.writable),
                                   "labels": labels}
        return spaces


def Bake(stagePath, rigPath, output):
    """The poseable bake, by usdRig's own baking tool."""
    root = os.environ.get("RIG") or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir,
        os.pardir, "usdRig")
    tool = os.path.join(root, "build", "rigExecBake")
    if sys.platform == "win32":
        tool += ".exe"
    if not os.path.exists(tool):
        raise RuntimeError("usdRig's rigExecBake was not found at %s; build "
                           "usdRig first" % tool)
    result = subprocess.run(
        [tool, stagePath, "--rig", str(rigPath), "--time", repr(BAKE_FRAME),
         "-o", output],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0 or not os.path.exists(output):
        raise RuntimeError("baking the rig failed:\n" +
                           result.stdout.decode("utf-8", "replace")[-2000:])


def main(argv):
    if len(argv) < 4:
        sys.stderr.write(__doc__)
        return 2
    stagePath, rigFile, jsonFile = argv[1:4]
    exporter = Exporter(stagePath, argv[4] if len(argv) > 4 else None)
    Bake(stagePath, exporter.rigPath, rigFile)
    description = exporter.Describe()
    description["rigFile"] = os.path.abspath(rigFile)
    with open(jsonFile, "w") as out:
        json.dump(description, out, separators=(",", ":"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
