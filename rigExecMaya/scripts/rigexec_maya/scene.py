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

The control curves are drawn by one rigExecControls node rather than by a
curve shape each, which keeps posing fast; clicking a drawn curve selects
its control all the same. The curve shapes are kept, hidden, and can be
shown instead:

    rigexec_maya.scene.SetDrawControls(False)
"""

import json
import math
import os

import maya.api.OpenMaya as om
import maya.cmds as cmds

from . import export as _export

PLUGIN = "rigExecMaya"
# Whether the control curves are drawn by the rig's rigExecControls node
# (fast) or by their own curve shapes.
_DRAW_OPTION = "rigExecDrawControls"
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
    _BuildMeshes(description, geoGroup, node, controlsGroup)
    _BuildControlsDraw(node, controls, controlsGroup)
    _StoreRigData(description, node)
    cmds.select(clear=True)
    # The features that read the rig's data pick the new rig up.
    from . import automatch, touchpose
    automatch.Forget()
    touchpose.Refresh()
    return node


def IsDrawingControls():
    """Whether the rigs' control curves are drawn by their rigExecControls
    node rather than by their curve shapes."""
    if not cmds.optionVar(exists=_DRAW_OPTION):
        return True
    return bool(cmds.optionVar(query=_DRAW_OPTION))


def SetDrawControls(enabled):
    """Draws every rig's control curves with its rigExecControls node
    (True, fast) or with the curve shapes themselves (False)."""
    cmds.optionVar(intValue=(_DRAW_OPTION, int(bool(enabled))))
    for drawer in cmds.ls(type="rigExecControls", long=True) or []:
        _ShowDrawn(drawer, enabled)


def _BuildControlsDraw(node, controls, group):
    """The rigExecControls node that draws the controls' curves: their
    points and colours, each curve's control, the controls themselves and
    the rig's poses."""
    owners, counts, points, colors = [], [], [], []
    for c, control in enumerate(controls):
        for shape in cmds.listRelatives(control, shapes=True, fullPath=True,
                                        type="nurbsCurve") or []:
            cvs = cmds.getAttr(shape + ".cv[*]")
            if len(cvs) < 2:
                continue
            owners.append(c)
            counts.append(len(cvs))
            points.extend(v for cv in cvs for v in cv)
            colors.extend(cmds.getAttr(shape + ".overrideColorRGB")[0])
    transform = cmds.createNode("transform", name="controlCurves",
                                parent=group, skipSelect=True)
    drawer = cmds.createNode("rigExecControls", name="controlCurvesShape",
                             parent=transform, skipSelect=True)
    drawer = cmds.ls(drawer, long=True)[0]
    cmds.setAttr(drawer + ".curveControl", owners, type="Int32Array")
    cmds.setAttr(drawer + ".curveCounts", counts, type="Int32Array")
    cmds.setAttr(drawer + ".curvePoints", points, type="doubleArray")
    cmds.setAttr(drawer + ".curveColors", colors, type="doubleArray")
    for c, control in enumerate(controls):
        cmds.connectAttr(control + ".message", "%s.control[%d]" % (drawer, c))
    cmds.connectAttr(node + ".outControlFrames", drawer + ".rigFrame")
    # Kept out of the way: it is not something to select or animate.
    cmds.setAttr(transform + ".hiddenInOutliner", True)
    for attr in ("translate", "rotate", "scale"):
        cmds.setAttr("%s.%s" % (transform, attr), lock=True)
    _ShowDrawn(drawer, IsDrawingControls())
    return drawer


def _ShowDrawn(drawer, drawn):
    """Shows `drawer`'s drawing of its controls' curves, hiding their
    curve shapes, or the other way round."""
    cmds.setAttr(drawer + ".visibility", bool(drawn))
    plug = drawer + ".control"
    for index in cmds.getAttr(plug, multiIndices=True) or []:
        control = cmds.listConnections("%s[%d]" % (plug, index),
                                       source=True, destination=False)
        if not control:
            continue
        for shape in cmds.listRelatives(control[0], shapes=True,
                                        fullPath=True,
                                        type="nurbsCurve") or []:
            cmds.setAttr(shape + ".lodVisibility", not drawn)


def ChannelPlug(attr):
    """The Maya attribute a rig avar is animated through: translate, rotate
    and scale for the transform avars, else the avar's own name
    (`avars:space` is `space`)."""
    return _CHANNEL_PLUGS.get(attr) or attr[len("avars:"):].replace(":", "_")


def _StoreRigData(description, node):
    """Keeps what the picker, matched switches and TouchPose need on the
    rig's node, so they work in saved scenes too: the pickers, each
    control's path and rotation, each channel, the IK/FK limbs, the space
    switches and the touch regions."""
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
        "touch": description.get("touch") or [],
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
    cmds.setAttr(node + ".channelSign", _ChannelSigns(description),
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
    shaderMatrices = _ShaderMatrices(description)[0]
    cmds.setAttr(node + ".shaderMatrixPath", len(shaderMatrices),
                 *shaderMatrices, type="stringArray")


def _ChannelSigns(description):
    """-1 for each mirrored rotation channel, else 1. The rig turns a
    mirrored control by minus its avar, so the Maya channel holds the avar
    times -1: Maya's manipulator then turns the control the way it is
    dragged, as usdview's does."""
    axes = ("avars:rx", "avars:ry", "avars:rz")
    signs = []
    for channel in description["channels"]:
        sign = description["controls"][channel["control"]].get(
            "rotationSign") or (1, 1, 1)
        signs.append(-1.0 if channel["attr"] in axes and
                     float(sign[axes.index(channel["attr"])]) < 0.0
                     else 1.0)
    return signs


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
    signs = _ChannelSigns(description)
    for index, channel in enumerate(description["channels"]):
        control = controls[channel["control"]]
        attr = channel["attr"]
        plug = ChannelPlug(attr)
        if attr not in _CHANNEL_PLUGS:
            if not cmds.attributeQuery(plug, node=control, exists=True):
                cmds.addAttr(control, longName=plug, attributeType="double",
                             keyable=True)
        source = "%s.%s" % (control, plug)
        cmds.setAttr(source, channel["default"] * signs[index])
        target = "inAngle" if attr in ("avars:rx", "avars:ry", "avars:rz") \
            else "inLinear"
        cmds.connectAttr(source, "%s.%s[%d]" % (node, target, index))


# -- meshes -------------------------------------------------------------------

def _BuildMeshes(description, group, node, space):
    shadingGroups = {}
    eyes = _ShaderMatrices(description)[1]
    if eyes and not _EyeShaderAvailable():
        eyes = {}
    for m, mesh in enumerate(description["meshes"]):
        transform = cmds.createNode("transform", name=mesh["name"],
                                    parent=group)
        orig = _CreateMesh(mesh, transform)
        origPath = cmds.ls(orig, long=True)[0]
        if any((m.get("surface") or {}).get("eye")
               for m in mesh.get("materials", [])):
            _AddEyeUvs(mesh, origPath)
        cmds.setAttr(origPath + ".intermediateObject", 1)
        shape = cmds.createNode("mesh", name=mesh["name"] + "Shape",
                                parent=transform)
        # Materials first, with the rest mesh feeding the shape: the nodes
        # Maya adds for per-face materials then sit before the rig, and run
        # once, rather than after it, copying the mesh on every pose.
        cmds.connectAttr(origPath + ".outMesh", shape + ".inMesh")
        eyeGroup = None
        if m in eyes:
            surface = next((x["surface"] for x in mesh.get("materials", [])
                            if (x.get("surface") or {}).get("eye")
                            is not None), None)
            if surface is not None:
                eyeGroup = _SquarebitEyeShadingGroup(
                    mesh["name"], surface, node, eyes[m][0], eyes[m][1],
                    space)
        _AssignMaterials(mesh, shape, shadingGroups, eyeGroup)
        source = cmds.listConnections(shape + ".inMesh", source=True,
                                      destination=False, plugs=True)[0]
        # The rig's points come in through a deformer, so Maya updates only
        # the points it draws on every pose.
        deformer = cmds.createNode("rigExecDeform",
                                   name=mesh["name"] + "_rigExecDeform")
        cmds.connectAttr(source, deformer + ".input[0].inputGeometry")
        cmds.connectAttr("%s.outMeshFrame[%d]" % (node, m),
                         deformer + ".rigFrame")
        cmds.connectAttr(deformer + ".outputGeometry[0]", shape + ".inMesh",
                         force=True)
        cmds.connectAttr("%s.outMeshMatrix[%d]" % (node, m),
                         transform + ".offsetParentMatrix")


# -- materials ----------------------------------------------------------------

def _AssignMaterials(mesh, shape, shadingGroups, eyeGroup=None):
    """The rig's materials on the mesh: the whole-mesh one, then each face
    subset's. The biped's procedural eye becomes `eyeGroup` (this eye's
    Squarebit Eye network) or else a ramp (_EyeShadingGroup); any other
    material that is not a UsdPreviewSurface keeps Maya's default."""
    shapePath = cmds.ls(shape, long=True)[0]
    cmds.sets(shapePath, edit=True, forceElement="initialShadingGroup")
    for material in mesh.get("materials", []):
        if eyeGroup and                 (material.get("surface") or {}).get("eye") is not None:
            shadingGroup = eyeGroup
        else:
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
    if surface.get("eye") is not None:
        shadingGroups[name] = _EyeShadingGroup(name, surface["eye"])
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


# -- the eye ------------------------------------------------------------------
#
# The biped's eyes use Squarebit Eye's glslfx shader, which Maya can't run.
# Maya gets the same iris and pupil, sized by the shader's own inputs, from
# a circular ramp. The ramp reads a UV set projected straight along the
# eye's rest gaze (+Z), so its coordinates are the shader's: the unit
# eyeball's x and y. The rig turns an eye by moving its points, and the UVs
# ride on the points, so the iris turns with it. The projection is the eye's
# own UV set in Maya: nothing there reads the eye's authored UVs, and a
# second set would not reach the mesh the rig deforms.
_SCLERA = (0.85, 0.83, 0.80)
_LIMBUS = (0.12, 0.08, 0.06)
_IRIS = (0.44, 0.32, 0.25)
_PUPIL = (0.0, 0.0, 0.0)


def _AddEyeUvs(mesh, shapePath):
    """The eye projection as the mesh's UVs: each point's position on the
    unit eyeball, seen down the gaze, as (0.5 + x/2, 0.5 + y/2). The back
    of the eye maps into the sclera."""
    flat = mesh["points"]
    points = [flat[i:i + 3] for i in range(0, len(flat), 3)]
    if not points:
        return
    centre = [sum(p[k] for p in points) / len(points) for k in range(3)]
    radius = max(math.sqrt(sum((p[k] - centre[k]) ** 2 for k in range(3)))
                 for p in points) or 1.0
    us, vs = [], []
    for p in points:
        x, y, z = [(p[k] - centre[k]) / radius for k in range(3)]
        if z < 0.0:
            # Behind the equator: push out past the iris, into the sclera.
            length = math.hypot(x, y) or 1.0
            x, y = x / length, y / length
        us.append(0.5 + 0.5 * x)
        vs.append(0.5 + 0.5 * y)
    fn = om.MFnMesh(om.MSelectionList().add(shapePath).getDagPath(0))
    uvSet = fn.currentUVSetName()
    fn.clearUVs(uvSet)
    fn.setUVs(us, vs, uvSet)
    counts = mesh["counts"]
    indices = list(mesh["indices"])
    if mesh.get("leftHanded"):
        indices = _Reversed(counts, indices)
    fn.assignUVs(counts, indices, uvSet)


def _EyeShadingGroup(name, eye):
    """A standardSurface whose base colour is a circular ramp: sclera,
    a dark limbal ring at the iris edge, the iris, and the pupil, at the
    radii Squarebit Eye's inputs give (fractions of the eyeball radius)."""
    iris = float(eye.get("irisRadius", 0.4875))
    pupil = float(eye.get("pupilRadius", 0.1667))
    bleed = max(0.0, float(eye.get("pupilBleedAmount", 0.08)))
    shader = cmds.shadingNode("standardSurface", asShader=True, name=name)
    cmds.setAttr(shader + ".base", 1.0)
    cmds.setAttr(shader + ".specular", 1.0)
    cmds.setAttr(shader + ".specularRoughness", 0.3)
    cmds.setAttr(shader + ".coat", 1.0)
    cmds.setAttr(shader + ".coatRoughness", 0.02)
    cmds.setAttr(shader + ".coatIOR", 1.376)
    ramp = cmds.shadingNode("ramp", asTexture=True, name=name + "_iris")
    cmds.setAttr(ramp + ".type", 4)             # circular
    cmds.setAttr(ramp + ".interpolation", 4)    # smooth
    stops = [(0.0, _PUPIL), (pupil, _PUPIL),
             (min(pupil + 0.02 + bleed * 0.1, iris), _IRIS),
             (iris * 0.92, _IRIS), (iris, _LIMBUS),
             (min(iris + 0.04, 1.0), _SCLERA), (1.0, _SCLERA)]
    cmds.removeMultiInstance(ramp + ".colorEntryList[1]", b=True)
    cmds.removeMultiInstance(ramp + ".colorEntryList[2]", b=True)
    for i, (position, color) in enumerate(stops):
        cmds.setAttr("%s.colorEntryList[%d].position" % (ramp, i), position)
        cmds.setAttr("%s.colorEntryList[%d].color" % (ramp, i), *color,
                     type="double3")
    place = cmds.shadingNode("place2dTexture", asUtility=True,
                             name=name + "_place")
    cmds.connectAttr(place + ".outUV", ramp + ".uvCoord")
    cmds.connectAttr(place + ".outUvFilterSize", ramp + ".uvFilterSize")
    cmds.connectAttr(ramp + ".outColor", shader + ".baseColor")
    shadingGroup = cmds.sets(renderable=True, noSurfaceShader=True,
                             empty=True, name=name + "SG")
    cmds.connectAttr(shader + ".outColor", shadingGroup + ".surfaceShader")
    return shadingGroup


# -- the eye with Squarebit Eye ----------------------------------------------
#
# With the Squarebit Eye plug-in installed, each eye is drawn as it is in
# Unreal: its eye shader, placed by the frame the rig's projector publishes
# for that eye (outShaderMatrix), with the eye control's dials (pupil and
# iris size, pupil offset) applied on top of the eye's own look. Without it,
# the eye keeps the ramp above.
_EYE_PLUGIN = "squarebitEye"
_EYE_SHADER = "squarebitEyeShader"
_EYE_PROJECTOR = "squarebitEyeProjector"
# The rig's dial slots (the projector's shaderDialSources order), the eye
# shader input each drives, and whether it scales the eye's own value
# (value * (1 + dial)) or offsets it (value + dial). Zero is neutral.
_EYE_DIALS = (("pupilWidth", True), ("pupilHeight", True),
              ("irisWidth", True), ("irisHeight", True),
              ("pupilOffsetX", False), ("pupilOffsetY", False),
              ("pupilRadius", True), ("limbusRadius", True))
# The look's inputs whose names differ on the eye shader.
_EYE_RENAMED = {"irisRadius": "limbusRadius"}


def _Linear(kind):
    """Maya's add or multiply node for one number, by its current name
    (Maya 2027 renamed addDoubleLinear and multDoubleLinear)."""
    short = kind + "DL"
    if short in (cmds.allNodeTypes() or []):
        return short
    return kind + "DoubleLinear"


def _EyeShaderAvailable():
    if cmds.pluginInfo(_EYE_PLUGIN, query=True, loaded=True):
        return True
    try:
        cmds.loadPlugin(_EYE_PLUGIN, quiet=True)
    except RuntimeError:
        return False
    return bool(cmds.pluginInfo(_EYE_PLUGIN, query=True, loaded=True))


def _ShaderMatrices(description):
    """The shader matrix paths the rig node publishes, and each eye mesh's
    (projector index, dials index or None), by mesh index."""
    paths, eyes = [], {}
    for m, mesh in enumerate(description["meshes"]):
        projector = mesh.get("eyeProjector")
        if not projector:
            continue
        paths.append(projector["projector"])
        frame = len(paths) - 1
        dials = None
        if projector.get("dials"):
            paths.append(projector["dials"])
            dials = len(paths) - 1
        eyes[m] = (frame, dials)
    return paths, eyes


def _SquarebitEyeShadingGroup(name, surface, node, frame, dials, space):
    """Squarebit Eye's network for one eye: the eye shader as the base
    colour of a wet standardSurface, placed by the rig."""
    eye = dict(surface.get("eye") or {})
    shader = cmds.shadingNode(_EYE_SHADER, asUtility=True,
                              name=name + "_eyeShader")

    def has(attr):
        return cmds.attributeQuery(attr, node=shader, exists=True)

    # The eye's own look.
    for key, value in eye.items():
        attr = _EYE_RENAMED.get(key, key)
        if has(attr):
            cmds.setAttr(shader + "." + attr, value)
    for key, value in (surface.get("eyeColors") or {}).items():
        if has(key):
            cmds.setAttr(shader + "." + key, *value, type="double3")
    for key, value in (surface.get("eyeTextures") or {}).items():
        attr = key if key.endswith("Path") else key + "Path"
        if has(attr):
            cmds.setAttr(shader + "." + attr, value, type="string")

    # Where the eye is: the projector's frame, from the rig's asset space
    # into the scene.
    place = cmds.createNode("multMatrix", name=name + "_eyeFrame")
    cmds.connectAttr("%s.outShaderMatrix[%d]" % (node, frame),
                     place + ".matrixIn[0]")
    cmds.connectAttr(space + ".worldMatrix[0]", place + ".matrixIn[1]")
    cmds.connectAttr(place + ".matrixSum", shader + ".projectorWorldMatrix")

    # The eye control's dials, on top of the look.
    if dials is not None:
        for slot, (attr, scales) in enumerate(_EYE_DIALS):
            if not has(attr):
                continue
            base = cmds.getAttr(shader + "." + attr)
            dial = "%s.outShaderValue[%d]" % (node, 16 * dials + slot)
            add = cmds.createNode(_Linear("add"),
                                  name="%s_%sDial" % (name, attr))
            cmds.connectAttr(dial, add + ".input1")
            if scales:
                cmds.setAttr(add + ".input2", 1.0)
                scale = cmds.createNode(_Linear("mult"),
                                        name="%s_%sScale" % (name, attr))
                cmds.connectAttr(add + ".output", scale + ".input1")
                cmds.setAttr(scale + ".input2", base)
                cmds.connectAttr(scale + ".output", shader + "." + attr)
            else:
                cmds.setAttr(add + ".input2", base)
                cmds.connectAttr(add + ".output", shader + "." + attr)

    # Squarebit Eye's own surface settings and, with no iris image, its
    # procedural iris, when its tools are there.
    try:
        from squarebit_eye import tool as eyeTool
    except Exception:
        eyeTool = None
    surfaceNode = cmds.shadingNode("standardSurface", asShader=True,
                                   name=name + "_eyeSurface")
    cmds.connectAttr(shader + ".outColor", surfaceNode + ".baseColor")
    cmds.setAttr(surfaceNode + ".specularRoughness",
                 getattr(eyeTool, "SPEC_SCLERA_ROUGHNESS", 0.3))
    cmds.setAttr(surfaceNode + ".coat",
                 getattr(eyeTool, "SPEC_COAT_WEIGHT", 1.0))
    cmds.setAttr(surfaceNode + ".coatRoughness",
                 getattr(eyeTool, "SPEC_COAT_ROUGHNESS", 0.02))
    cmds.setAttr(surfaceNode + ".coatIOR",
                 getattr(eyeTool, "SPEC_COAT_IOR", 1.376))
    if eyeTool is not None and not (surface.get("eyeTextures") or {}):
        try:
            # The procedural iris's colours live on a projector shape,
            # which is otherwise unused: kept hidden.
            holder = cmds.createNode("transform", name=name + "_eyeLook",
                                     skipSelect=True)
            projector = cmds.createNode(_EYE_PROJECTOR,
                                        name=name + "_eyeLookShape",
                                        parent=holder, skipSelect=True)
            cmds.setAttr(holder + ".visibility", False)
            cmds.setAttr(holder + ".hiddenInOutliner", True)
            cmds.parent(holder, space)
            eyeTool.update_procedural_iris(
                {"shader": shader, "projector": holder,
                 "projectorShape": projector, "surface": surfaceNode})
        except Exception as error:
            om.MGlobal.displayWarning(
                "RigExec: the eye's procedural iris was not made: %s"
                % error)
    if eyeTool is not None and not cmds.getAttr(shader + ".scleraTexturePath"):
        # No sclera image: a plain white one, as Squarebit Eye makes for its
        # own eyes (without one the viewport shows the white as missing).
        try:
            cmds.setAttr(shader + ".scleraTexturePath",
                         eyeTool.iris_procedural.bake_white_sclera_png(
                             eyeTool._procedural_texture_path(
                                 name + "_eyeShader_sclera")),
                         type="string")
        except Exception as error:
            om.MGlobal.displayWarning(
                "RigExec: the eye's white was not made: %s" % error)
    shadingGroup = cmds.sets(renderable=True, noSurfaceShader=True,
                             empty=True, name=name + "_eyeSG")
    cmds.connectAttr(surfaceNode + ".outColor",
                     shadingGroup + ".surfaceShader")
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
