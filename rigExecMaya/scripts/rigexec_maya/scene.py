"""
Builds a RigExec rig in the Maya scene: controls to animate, meshes the rig
deforms, and the rigExecRig node between them.

    import rigexec_maya.scene
    rigexec_maya.scene.Load(r"C:/path/to/Biped_stack.usda")

Controls mirror the rig's control hierarchy. Each one's translate, rotate and
scale are its translation, rotation and scale avars; the rig's other avars
are keyable attributes of the same names. The node places every control
through its offsetParentMatrix, so animating with Maya's own tools moves
the rig exactly as the rig defines.
"""

import json
import math
import os

import maya.api.OpenMaya as om
import maya.cmds as cmds

from . import export as _export

PLUGIN = "rigExecMaya"
_CHANNEL_PLUGS = {
    "avars:tx": "translateX", "avars:ty": "translateY",
    "avars:tz": "translateZ", "avars:rx": "rotateX", "avars:ry": "rotateY",
    "avars:rz": "rotateZ", "avars:sx": "scaleX", "avars:sy": "scaleY",
    "avars:sz": "scaleZ",
}
_TRS = ("avars:tx", "avars:ty", "avars:tz", "avars:rx", "avars:ry",
        "avars:rz", "avars:sx", "avars:sy", "avars:sz")
_ROTATE_ORDER_INDEX = {"XYZ": 0, "YZX": 1, "ZXY": 2, "XZY": 3, "YXZ": 4,
                       "ZYX": 5}


def Load(stagePath, rigPath=None, name=None):
    """Exports the rig (a poseable bake and its description, made once by
    usdRig outside Maya), builds it, and returns its rigExecRig node."""
    description = _export.Export(stagePath, rigPath)
    return Build(description, name)


def Build(description, name=None):
    """Builds an exported rig and returns its rigExecRig node."""
    if not cmds.pluginInfo(PLUGIN, query=True, loaded=True):
        cmds.loadPlugin(PLUGIN)
    stagePath = description["stage"]
    name = name or os.path.splitext(os.path.basename(stagePath))[0]

    top = cmds.createNode("transform", name=name)
    controlsGroup = cmds.createNode("transform", name="controls",
                                    parent=top)
    cmds.xform(controlsGroup, matrix=description["assetToWorld"])
    geoGroup = cmds.createNode("transform", name="geo", parent=top)
    node = cmds.createNode("rigExecRig", name=name + "_rigExec")
    cmds.setAttr(node + ".stagePath", stagePath, type="string")
    cmds.setAttr(node + ".rigPath", description["rigPath"], type="string")
    _SetTables(node, description)
    cmds.connectAttr("time1.outTime", node + ".time")

    controls = _BuildControls(description, controlsGroup)
    _ConnectChannels(description, controls, node)
    for c, control in enumerate(controls):
        cmds.connectAttr("%s.outControlMatrix[%d]" % (node, c),
                         control + ".offsetParentMatrix")
    _BuildMeshes(description, geoGroup, node)
    _StoreRigData(description, node)
    cmds.select(clear=True)
    return node


def ChannelPlug(attr):
    """The Maya attribute a rig avar is animated through: translate, rotate
    and scale for the transform avars, else the avar's own name
    (`avars:space` is `space`)."""
    return _CHANNEL_PLUGS.get(attr) or attr[len("avars:"):].replace(":", "_")


def _StoreRigData(description, node):
    """Keeps what the picker and matched switches need on the rig's node, so
    they work in saved scenes too: the pickers, each control's path and
    rotation, each channel, the IK/FK limbs and the space switches."""
    data = {
        "pickers": description.get("pickers") or [],
        "controls": [{"path": c["path"], "name": c["name"],
                      "rotationOrder": c["rotationOrder"],
                      "rotationSign": c.get("rotationSign") or [1, 1, 1]}
                     for c in description["controls"]],
        "channels": [[c["control"], c["attr"]]
                     for c in description["channels"]],
        "limbs": description.get("limbs") or [],
        "spaces": description.get("spaces") or {},
    }
    cmds.addAttr(node, longName="rigData", dataType="string")
    cmds.setAttr(node + ".rigData", json.dumps(data), type="string")


def _SetTables(node, description):
    """The lookups the node's compute reads, from the description."""
    controls = description["controls"]
    trs = [-1] * (9 * len(controls))
    for index, channel in enumerate(description["channels"]):
        if channel["attr"] in _TRS:
            trs[9 * channel["control"] + _TRS.index(channel["attr"])] = index
    cmds.setAttr(node + ".controlParent", [c["parent"] for c in controls],
                 type="Int32Array")
    cmds.setAttr(node + ".controlRotateOrder",
                 [_ROTATE_ORDER_INDEX.get(c["rotationOrder"], 0)
                  for c in controls], type="Int32Array")
    cmds.setAttr(node + ".controlTrs", trs, type="Int32Array")
    cmds.setAttr(node + ".channelDefault",
                 [c["default"] for c in description["channels"]],
                 type="doubleArray")
    meshes = description["meshes"]
    cmds.setAttr(node + ".meshPointCount", [m["pointCount"] for m in meshes],
                 type="Int32Array")
    # The baked rig and how to read it back.
    cmds.setAttr(node + ".rigFile", description["rigFile"], type="string")
    cmds.setAttr(node + ".bakeFrame", description.get("bakeFrame", 1.0))
    paths = ["%s.%s" % (controls[c["control"]]["path"], c["attr"])
             for c in description["channels"]]
    cmds.setAttr(node + ".channelPath", len(paths), *paths,
                 type="stringArray")
    cmds.setAttr(node + ".controlPath", len(controls),
                 *[c["path"] for c in controls], type="stringArray")
    cmds.setAttr(node + ".meshPoints", len(meshes),
                 *[m["pointsProperty"] for m in meshes], type="stringArray")
    cmds.setAttr(node + ".meshProvider", len(meshes),
                 *[m["provider"] for m in meshes], type="stringArray")
    identity = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
    cmds.setAttr(node + ".meshLocal",
                 [v for m in meshes for v in (m["local"] or identity)],
                 type="doubleArray")
    cmds.setAttr(node + ".meshWorld", [v for m in meshes for v in m["world"]],
                 type="doubleArray")
    cmds.setAttr(node + ".assetToWorld", description["assetToWorld"],
                 type="doubleArray")


# -- controls ---------------------------------------------------------------

def _BuildControls(description, group):
    created = []
    for control in description["controls"]:
        parent = created[control["parent"]] if control["parent"] >= 0 \
            else group
        transform = cmds.createNode("transform", name=control["name"],
                                    parent=parent)
        cmds.setAttr(transform + ".rotateOrder",
                     _ROTATE_ORDER_INDEX.get(control["rotationOrder"], 0))
        if control["guide"] is not None:
            _AddShape(transform, control["guide"])
        created.append(cmds.ls(transform, long=True)[0])
    return created


def _AddShape(transform, guide):
    """Curves for a control's guide shape, in its local frame: the unit
    shape turned to its plane, sized by its guide scale, turned by its
    guide orient, then offset."""
    shape = guide["shape"]
    sx, sy, sz = guide["scale"]
    ox, oy, oz = guide["offset"]
    r = guide.get("orient", [[1, 0, 0], [0, 1, 0], [0, 0, 1]])
    loops = _UnitShape(shape)
    if shape in ("circle", "box"):
        loops = [[_Plane(p, guide["planeNormal"]) for p in loop]
                 for loop in loops]
    for loop in loops:
        sized = [(x * sx, y * sy, z * sz) for x, y, z in loop]
        points = [(x * r[0][0] + y * r[1][0] + z * r[2][0] + ox,
                   x * r[0][1] + y * r[1][1] + z * r[2][1] + oy,
                   x * r[0][2] + y * r[1][2] + z * r[2][2] + oz)
                  for x, y, z in sized]
        curve = cmds.curve(degree=1, point=points + [points[0]])
        curveShape = cmds.listRelatives(curve, shapes=True,
                                        fullPath=True)[0]
        cmds.setAttr(curveShape + ".overrideEnabled", 1)
        cmds.setAttr(curveShape + ".overrideRGBColors", 1)
        cmds.setAttr(curveShape + ".overrideColorRGB", *guide["color"])
        cmds.parent(curveShape, transform, shape=True, relative=True)
        cmds.delete(curve)


def _Plane(point, normal):
    """The planar shapes are built normal +Y; turn them to the named axis
    with the same proper rotations the viewport guides use."""
    x, y, z = point
    if normal == "X":  # -90 about Z
        return (y, -x, z)
    if normal == "Z":  # +90 about X
        return (x, -z, y)
    return point


def _UnitShape(shape):
    if shape in ("circle", "sphere"):
        ring = [(math.cos(a), 0.0, math.sin(a))
                for a in (2.0 * math.pi * i / 24 for i in range(24))]
        if shape == "circle":
            return [ring]
        return [ring, [(x, z, y) for x, y, z in ring],
                [(y, x, z) for x, y, z in ring]]
    if shape == "box":
        return [[(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]]
    if shape == "cube":
        bottom = [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]
        top = [(x, 1, z) for x, _, z in bottom]
        sides = [[bottom[i], top[i]] for i in range(4)]
        return [bottom, top] + sides
    if shape == "diamond":
        return [[(1, 0, 0), (0, 1, 0), (-1, 0, 0), (0, -1, 0)],
                [(1, 0, 0), (0, 0, 1), (-1, 0, 0), (0, 0, -1)],
                [(0, 1, 0), (0, 0, 1), (0, -1, 0), (0, 0, -1)]]
    if shape == "pyramid":
        base = [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]
        return [base] + [[corner, (0, 1, 0)] for corner in base]
    return _UnitShape("circle")


# -- channels -----------------------------------------------------------------

def _ConnectChannels(description, controls, node):
    for index, channel in enumerate(description["channels"]):
        control = controls[channel["control"]]
        attr = channel["attr"]
        plug = ChannelPlug(attr)
        if attr not in _CHANNEL_PLUGS:
            if not cmds.attributeQuery(plug, node=control, exists=True):
                cmds.addAttr(control, longName=plug, attributeType="double",
                             keyable=True)
        source = "%s.%s" % (control, plug)
        cmds.setAttr(source, channel["default"])
        target = "inAngle" if attr in ("avars:rx", "avars:ry", "avars:rz") \
            else "inLinear"
        cmds.connectAttr(source, "%s.%s[%d]" % (node, target, index))


# -- meshes -------------------------------------------------------------------

def _BuildMeshes(description, group, node):
    shadingGroups = {}
    for m, mesh in enumerate(description["meshes"]):
        transform = cmds.createNode("transform", name=mesh["name"],
                                    parent=group)
        orig = _CreateMesh(mesh, transform)
        origPath = cmds.ls(orig, long=True)[0]
        cmds.setAttr(origPath + ".intermediateObject", 1)
        shape = cmds.createNode("mesh", name=mesh["name"] + "Shape",
                                parent=transform)
        cmds.connectAttr(origPath + ".outMesh",
                         "%s.inMesh[%d]" % (node, m))
        cmds.connectAttr("%s.outMesh[%d]" % (node, m), shape + ".inMesh")
        cmds.connectAttr("%s.outMeshMatrix[%d]" % (node, m),
                         transform + ".offsetParentMatrix")
        _AssignMaterials(mesh, shape, shadingGroups)


# -- materials ----------------------------------------------------------------

def _AssignMaterials(mesh, shape, shadingGroups):
    """The rig's materials on the mesh: the whole-mesh one, then each face
    subset's. A material that is not a UsdPreviewSurface keeps Maya's
    default."""
    shapePath = cmds.ls(shape, long=True)[0]
    cmds.sets(shapePath, edit=True, forceElement="initialShadingGroup")
    for material in mesh.get("materials", []):
        shadingGroup = _ShadingGroup(material, shadingGroups)
        if material["faces"] is None:
            cmds.sets(shapePath, edit=True, forceElement=shadingGroup)
        elif material["faces"]:
            cmds.sets(_FaceComponents(shapePath, material["faces"]),
                      edit=True, forceElement=shadingGroup)


def _ShadingGroup(material, shadingGroups):
    """One standardSurface and shading group per rig material, shared by
    every mesh that uses it."""
    name = material["name"]
    surface = material["surface"]
    if surface is None:
        return "initialShadingGroup"
    if name in shadingGroups:
        return shadingGroups[name]
    shader = cmds.shadingNode("standardSurface", asShader=True, name=name)
    cmds.setAttr(shader + ".base", 1.0)
    cmds.setAttr(shader + ".baseColor", *surface["diffuseColor"],
                 type="double3")
    cmds.setAttr(shader + ".metalness", surface["metallic"])
    cmds.setAttr(shader + ".specularRoughness", surface["roughness"])
    cmds.setAttr(shader + ".specularIOR", surface["ior"])
    opacity = surface["opacity"]
    cmds.setAttr(shader + ".opacity", opacity, opacity, opacity,
                 type="double3")
    if any(surface["emissiveColor"]):
        cmds.setAttr(shader + ".emission", 1.0)
        cmds.setAttr(shader + ".emissionColor", *surface["emissiveColor"],
                     type="double3")
    shadingGroup = cmds.sets(renderable=True, noSurfaceShader=True,
                             empty=True, name=name + "SG")
    cmds.connectAttr(shader + ".outColor", shadingGroup + ".surfaceShader")
    shadingGroups[name] = shadingGroup
    return shadingGroup


def _FaceComponents(shapePath, faces):
    """Face components for the given indices, as runs (mesh.f[3:9])."""
    runs = []
    for face in sorted(set(faces)):
        if runs and face == runs[-1][1] + 1:
            runs[-1][1] = face
        else:
            runs.append([face, face])
    return ["%s.f[%d:%d]" % (shapePath, a, b) for a, b in runs]


def _CreateMesh(mesh, transform):
    counts = mesh["counts"]
    indices = list(mesh["indices"])
    if mesh.get("leftHanded"):
        indices = _Reversed(counts, indices)
    flat = mesh["points"]
    points = om.MPointArray([(flat[i], flat[i + 1], flat[i + 2])
                             for i in range(0, len(flat), 3)])
    parent = om.MSelectionList().add(transform).getDependNode(0)
    fn = om.MFnMesh()
    shapeObject = fn.create(points, counts, indices, parent=parent)
    fn = om.MFnMesh(shapeObject)
    if mesh.get("uvs"):
        uvs = mesh["uvs"]
        fn.setUVs(uvs[0::2], uvs[1::2])
        uvIds = _UvIds(mesh, counts, indices)
        if uvIds is not None:
            if mesh.get("leftHanded"):
                uvIds = _Reversed(counts, uvIds)
            fn.assignUVs(counts, uvIds)
    om.MFnDependencyNode(shapeObject).setName(mesh["name"] + "ShapeOrig")
    return om.MFnDagNode(shapeObject).fullPathName()


def _UvIds(mesh, counts, indices):
    """Per face-vertex uv ids for the stage's st primvar, in the stage's
    winding (reversed together with the faces when left-handed)."""
    interpolation = mesh.get("uvInterpolation")
    stored = mesh.get("uvIndices")
    if interpolation == "faceVarying":
        return list(stored) if stored is not None else \
            list(range(len(indices)))
    if interpolation == "vertex":
        source = list(mesh["indices"])
        return [stored[v] for v in source] if stored is not None else source
    return None


def _Reversed(counts, values):
    out = []
    cursor = 0
    for count in counts:
        face = values[cursor:cursor + count]
        out.extend([face[0]] + face[1:][::-1])
        cursor += count
    return out
