"""
RigExec evaluation worker for the Maya client.

Runs under the Python that owns the rig's USD build (OpenUSD 26.08 with
OpenExec) and never inside Maya: Maya's own USD has the same library names
and an older OpenExec, so the two cannot share a process. The client
starts this script with the RigExec environment, then talks to it over
stdin/stdout in the framing protocol.py defines.

    python rigexec_maya_worker.py <stage.usda> [<rig prim path>]

The worker opens the stage, compiles its RigExecRoot, and answers describe
and evaluate requests. Evaluation authors nothing: the client's channel
values reach the evaluator as interactive overrides.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "scripts", "rigexec_maya"))
import protocol  # noqa: E402

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade  # noqa: E402
import rigexec  # noqa: E402

# Control avars that configure the control rather than pose it.
_NON_CHANNELS = {"avars:unitScaleFactor"}
_CHANNEL_TYPES = {Sdf.ValueTypeNames.Double, Sdf.ValueTypeNames.Float,
                  Sdf.ValueTypeNames.Int, Sdf.ValueTypeNames.Bool}


def _Matrix(m):
    return [m[i][j] for i in range(4) for j in range(4)]


def _MatrixFromList(values):
    return Gf.Matrix4d(*[values[i * 4:i * 4 + 4] for i in range(4)])


class Worker:
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
        # Animation, not rigging: the baked program answers exactly what the
        # dynamic path does, about three times faster on the biped, and the
        # per-point weight fields only feed a weight-painting overlay.
        self.rig.evaluation_mode = "baked"
        self.rig.publish_weight_fields = False
        self.time = Usd.TimeCode.Default()
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
        rest = self.rig.evaluate(-1)
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
            self.controls.append({
                "path": str(path),
                "name": prim.GetName(),
                "parent": parentIndex,
                "rotationOrder": str(prim.GetAttribute(
                    "avars:rotationOrder").Get() or "XYZ"),
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
            self.meshes.append(entry)
            entry["_provider"] = provider
            entry["_local"] = local
            entry["_world"] = cache.GetLocalToWorldTransform(prim)
        self._channelAttrs = [
            self.stage.GetPrimAtPath(self.controls[c["control"]]["path"])
            .GetAttribute(c["attr"]) for c in self.channels]
        self._authored = {}
        self._varying = [i for i, attr in enumerate(self._channelAttrs)
                         if attr.ValueMightBeTimeVarying()]
        self._static = []
        for attr in self._channelAttrs:
            value = attr.Get(Usd.TimeCode.Default())
            self._static.append(float(value) if value is not None else 0.0)
        self._controlPaths = [c["path"] for c in self.controls]
        self.layout = protocol.FrameLayout(self.Describe())

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
            "stage": self.stage.GetRootLayer().identifier,
            "rigPath": str(self.rigPath),
            "upAxis": str(UsdGeom.GetStageUpAxis(self.stage)),
            "metersPerUnit": UsdGeom.GetStageMetersPerUnit(self.stage),
            "timeCodesPerSecond": self.stage.GetTimeCodesPerSecond(),
            # Control matrices are asset-space; this places the asset.
            "assetToWorld": _Matrix(self.assetToWorld),
            "controls": self.controls,
            "channels": self.channels,
            "meshes": [{k: v for k, v in m.items() if not k.startswith("_")}
                       for m in self.meshes],
        }

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

    # -- evaluation ----------------------------------------------------

    def _AuthoredAt(self, time):
        """Every channel's stage value at a time. Most channels hold one
        value for all time, read once; only the ones that might vary are
        read again, and those answers are cached per time (the stage is
        never edited, so an answer stands)."""
        cached = self._authored.get(time)
        if cached is None:
            cached = list(self._static)
            code = Usd.TimeCode(time)
            for index in self._varying:
                value = self._channelAttrs[index].Get(code)
                cached[index] = float(value) if value is not None else 0.0
            if len(self._authored) > 256:
                self._authored.clear()
            self._authored[time] = cached
        return cached

    # -- switching without moving ---------------------------------------

    def Match(self, time, values, attr, value):
        """{attribute path: value} that sets the switch `attr` to `value`
        and keeps the pose still, solved by usdRig's own IK/FK and space
        matching. `values` is Maya's pose (every channel), which stands
        over the stage's own values for the solve. A switch that is neither
        an IK/FK limb nor a space switch just takes its value."""
        import ikfkMatch
        import spaceMatch

        if len(values) != len(self.channels):
            raise ValueError("expected %d channel values, got %d"
                             % (len(self.channels), len(values)))
        attrPath = Sdf.Path(str(attr))
        authored = self._AuthoredAt(time)
        current = {}
        for channelAttr, mine, stageValue in zip(self._channelAttrs, values,
                                                 authored):
            if mine != stageValue:
                current[channelAttr.GetPath()] = float(mine)
        code = Usd.TimeCode(time)
        if not hasattr(self, "_written"):
            self._written = ikfkMatch.RigWritten(self.stage)
            self._limbs = ikfkMatch.FindLimbs(self.stage)
            self._rest = {}

        def evaluate(overrides, when):
            merged = dict(current)
            merged.update(overrides)
            self.rig.set_interactive_overrides(
                [(str(p.GetPrimPath()), p.name, float(v))
                 for p, v in merged.items()])
            try:
                return self.rig.evaluate(
                    time if when.IsDefault() else when.GetValue())
            finally:
                self.rig.clear_interactive_overrides()

        limb = next((l for l in self._limbs if l.switchPath == attrPath),
                    None)
        if limb is not None:
            rest = self._rest.get(limb.blend)
            if rest is None:
                rest = ikfkMatch.MeasureRest(limb, self.stage, evaluate, code)
                self._rest[limb.blend] = rest
            toIk = abs(value - limb.ikValue) < abs(value - limb.fkValue)
            plan, _ = ikfkMatch.Plan(limb, self.stage, evaluate, code,
                                     toIk=toIk, rest=rest,
                                     written=self._written, current=current)
        else:
            control = spaceMatch.SwitchTarget(self.stage, attrPath)
            if control is None:
                plan = {attrPath: float(value)}
            else:
                plan = spaceMatch.Plan(self.stage, control, attrPath, value,
                                       code, evaluate, self._written,
                                       current)
        return {str(path): float(v) for path, v in plan.items()}

    def Evaluate(self, time, values):
        if len(values) != len(self.channels):
            raise ValueError("expected %d channel values, got %d"
                             % (len(self.channels), len(values)))
        # Only the channels whose value differs from what the stage already
        # answers at this time: an override per channel costs the evaluator
        # far more than the handful a pose actually moves.
        authored = self._AuthoredAt(time)
        overrides = []
        for channel, value, stageValue in zip(self.channels, values, authored):
            if value != stageValue:
                control = self.controls[channel["control"]]
                overrides.append((control["path"], channel["attr"], value))
        if overrides:
            self.rig.set_interactive_overrides(overrides)
        else:
            self.rig.clear_interactive_overrides()
        pose = self.rig.evaluate(time)
        out = bytearray(self.layout.size)
        for mesh, offset in zip(self.meshes, self.layout.pointOffsets):
            if mesh["moved"]:
                try:
                    data = pose.moved_points_bytes(mesh["path"] + ".points")
                except KeyError:
                    continue
                out[offset:offset + len(data)] = data
            else:
                rest = struct.pack("<%df" % len(mesh["points"]),
                                   *mesh["points"])
                out[offset:offset + len(rest)] = rest
        offset = self.layout.controlMatricesOffset
        frames = pose.control_matrices_bytes(self._controlPaths)
        out[offset:offset + len(frames)] = frames
        offset += len(frames)
        providerPaths = set(pose.provider_paths())
        for mesh in self.meshes:
            world = mesh["_world"]
            if mesh["_provider"] in providerPaths:
                world = mesh["_local"] * _MatrixFromList(
                    pose.provider_xform(mesh["_provider"])) * self.assetToWorld
            struct.pack_into("<16d", out, offset, *_Matrix(world))
            offset += 128
        return bytes(out)


def Serve(worker, stdin, stdout):
    while True:
        message = protocol.ReadMessage(stdin)
        if message is None or message[:1] == protocol.OP_QUIT:
            return
        try:
            op = message[:1]
            if op == protocol.OP_DESCRIBE:
                description = worker.Describe()
                description["pickers"] = worker.Pickers()
                reply = protocol.PackJson(description)
            elif op == protocol.OP_EVALUATE:
                time, values = protocol.UnpackEvaluate(message)
                reply = worker.Evaluate(time, values)
            elif op == protocol.OP_MATCH:
                request = protocol.UnpackJson(message[1:])
                reply = protocol.PackJson(worker.Match(
                    request["time"], request["values"], request["attr"],
                    request["value"]))
            else:
                raise ValueError("unknown request %r" % op)
        except Exception as error:
            reply = protocol.ERROR + str(error).encode("utf-8")
        protocol.WriteMessage(stdout, reply)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    # The reply stream gets a private copy of fd 1, and fd 1 itself is
    # pointed at stderr: anything printed from Python or from C++ lands in
    # the log, never inside a reply.
    stdin = sys.stdin.buffer
    stdout = os.fdopen(os.dup(1), "wb")
    sys.stdout.flush()
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    try:
        worker = Worker(argv[1], argv[2] if len(argv) > 2 else None)
    except Exception as error:
        protocol.WriteMessage(stdout, protocol.ERROR +
                              str(error).encode("utf-8"))
        return 1
    protocol.WriteMessage(stdout, b"ready")
    Serve(worker, stdin, stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
