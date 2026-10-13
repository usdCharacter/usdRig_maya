"""
Matched switches: change a limb between IK and FK, or a control's space,
without anything moving.

A port of usdRig's own matching (plugin/rigExecUsdview/ikfkMatch.py and
spaceMatch.py) onto the rig running in Maya. The rig's frames come from the
rigExecRig node (the rigExecFrames command), Maya's channels are the pose,
and the limbs' rest offsets were measured by the export. Conventions are
usdRig's: row vectors (a child's world frame is local * parent), frames in
asset space; Maya's matrices use the same convention.

    plan = Plan(rig, "/Biped/Rig/.../L_Arm.avars:ikfk", 1.0)
    # {attribute path: value}: the matched channels and the switch itself
"""

import math

import maya.api.OpenMaya as om
import maya.mel as mel

_TRANSLATE = ("tx", "ty", "tz")
_ROTATE = ("rx", "ry", "rz")
_IDENTITY = {"tx": 0.0, "ty": 0.0, "tz": 0.0, "rx": 0.0, "ry": 0.0,
             "rz": 0.0, "rspin": 0.0, "sx": 1.0, "sy": 1.0, "sz": 1.0}
_ORDERS = {"XYZ": om.MEulerRotation.kXYZ, "YZX": om.MEulerRotation.kYZX,
           "ZXY": om.MEulerRotation.kZXY, "XZY": om.MEulerRotation.kXZY,
           "YXZ": om.MEulerRotation.kYXZ, "ZYX": om.MEulerRotation.kZYX}
# A limb straighter than this (the middle joint's distance from the line
# through the ends, as a fraction of its length) has no bend plane.
_STRAIGHT = 1e-4
# A space match settles within this, in a few passes.
_SETTLED = 1e-6
_PASSES = 3


def Plan(rig, attrPath, value):
    """{attribute path: value} that sets the switch `attrPath` to `value`
    and keeps the pose still. `rig` is a picker.Rig. A switch that is
    neither an IK/FK limb's nor a space switch's just takes its value."""
    limb = next((l for l in rig.limbs if l["switch"] == attrPath), None)
    if limb is not None:
        toIk = abs(value - limb["ikValue"]) < abs(value - (1.0 - limb["ikValue"]))
        plan = _MatchLimb(rig, limb, toIk)
        plan[attrPath] = limb["ikValue"] if toIk else 1.0 - limb["ikValue"]
        return plan
    control = rig.spaces.get(attrPath)
    if control is not None:
        return _MatchSpace(rig, control, attrPath, value)
    return {attrPath: float(value)}


# -- evaluating ---------------------------------------------------------------

def _Frames(rig, overrides, paths):
    """{path: asset-space frame} with Maya's pose and `overrides` on top."""
    command = ['rigExecFrames "%s"' % rig.node]
    for path, value in overrides.items():
        command.append('-set "%s" %.17g' % (path, value))
    for path in paths:
        command.append('-frame "%s"' % path)
    flat = mel.eval(" ".join(command))
    return {path: om.MMatrix(flat[16 * i:16 * i + 16])
            for i, path in enumerate(paths)}


def _Translation(m):
    return om.MVector(m[12], m[13], m[14])


def _AxisRotation(axis, degrees):
    angles = [0.0, 0.0, 0.0]
    angles["XYZ".index(axis)] = math.radians(degrees)
    return om.MEulerRotation(*angles).asMatrix()


def _NormalizeScale(value):
    """The rig's scale floor: tiny magnitudes become a signed 1e-4."""
    if not math.isfinite(value):
        return 1.0
    if abs(value) < 1e-4:
        return math.copysign(1e-4, value)
    return value


def _Scale(sx, sy, sz):
    return om.MMatrix([_NormalizeScale(sx), 0, 0, 0,
                       0, _NormalizeScale(sy), 0, 0,
                       0, 0, _NormalizeScale(sz), 0,
                       0, 0, 0, 1])


class _Channels(object):
    """One control's channel values as Maya has them, and its avar
    matrix."""

    def __init__(self, rig, path):
        info = rig.controlInfo[path]
        self.path = path
        self.order = info["rotationOrder"] if info["rotationOrder"] in \
            _ORDERS else "XYZ"
        self.sign = tuple(info["rotationSign"])
        self.values = {}
        for name, fallback in _IDENTITY.items():
            plug = rig.Plug(path, "avars:" + name)
            self.values[name] = rig.Get(plug) if plug else fallback

    def Effective(self):
        """Rotation angles as the rig applies them (value * sign)."""
        return tuple(self.values[r] * s for r, s in zip(_ROTATE, self.sign))

    def Matrix(self, values=None):
        """The rig's avar matrix: S * R(order) * Rspin * T."""
        v = dict(self.values, **(values or {}))
        angles = [v[r] * s for r, s in zip(_ROTATE, self.sign)]
        m = _Scale(v["sx"], v["sy"], v["sz"])
        for axis in self.order:
            angle = angles["XYZ".index(axis)]
            if angle:
                m = m * _AxisRotation(axis, angle)
        if v["rspin"]:
            m = m * _AxisRotation("X", v["rspin"] * self.sign[0])
        t = om.MMatrix()
        t.setElement(3, 0, v["tx"])
        t.setElement(3, 1, v["ty"])
        t.setElement(3, 2, v["tz"])
        return m * t

    def Solve(self, target, parent):
        """Translate and rotate values that put this control's posed frame
        on `target`, with `parent` the frame its avars compose onto."""
        local = target * parent.inverse()
        translate = _Translation(local)
        linear = om.MMatrix(local)
        for c in range(3):
            linear.setElement(3, c, 0.0)
        scale = _Scale(self.values["sx"], self.values["sy"],
                       self.values["sz"])
        spin = _AxisRotation("X", self.values["rspin"] * self.sign[0]) \
            if self.values["rspin"] else om.MMatrix()
        rotation = scale.inverse() * linear * spin.inverse()
        euler = om.MTransformationMatrix(rotation).rotation()
        euler = euler.reorder(_ORDERS[self.order])
        hint = om.MEulerRotation(
            *[math.radians(a) for a in self.Effective()],
            order=_ORDERS[self.order])
        euler = euler.closestSolution(hint)
        solved = {t: float(translate[i]) for i, t in enumerate(_TRANSLATE)}
        for r, angle, sign in zip(_ROTATE, (euler.x, euler.y, euler.z),
                                  self.sign):
            solved[r] = math.degrees(angle) * sign
        return solved

    def SolveTranslate(self, point, parent):
        """Translate values that put this control's origin on `point`."""
        local = om.MPoint(point) * parent.inverse()
        return {t: float(local[i]) for i, t in enumerate(_TRANSLATE)}


def _Writable(rig, path, names):
    """The channels of `names` Maya animates on this control."""
    return [n for n in names if rig.Plug(path, "avars:" + n)]


# -- IK/FK ----------------------------------------------------------------

def _PolePosition(points, distance, fallback):
    """Where a pole goes for three joint positions: on their bend plane,
    on the side the middle joint bends toward, `distance` from it."""
    root, middle, end = points
    line = end - root
    length = line.length()
    if length < 1e-9:
        return fallback
    axis = line / length
    offset = middle - root
    bend = offset - axis * (offset * axis)
    if bend.length() <= _STRAIGHT * length:
        bend = fallback - middle
        bend = bend - axis * (bend * axis)
        if bend.length() < 1e-9:
            return fallback
    return middle + bend.normal() * distance


def _MatchLimb(rig, limb, toIk):
    """{attribute path: value} posing the half named by `toIk` onto the
    limb's current joints."""
    joints = limb["joints"]
    paths = list(joints) + list(limb["fkControls"]) + [limb["ikControl"],
                                                       limb["effector"]]
    if limb["pole"]:
        paths.append(limb["pole"])
    pose = _Frames(rig, {}, paths)
    jointFrames = [pose[j] for j in joints]
    values = {}
    if toIk:
        ik = limb["ikControl"]
        control = _Channels(rig, ik)
        controlFrame = pose[ik]
        parent = control.Matrix().inverse() * controlFrame
        # The effector rides the IK control (through a foot's roll stack,
        # whose current values are kept).
        carry = pose[limb["effector"]] * controlFrame.inverse()
        effectorTarget = om.MMatrix(limb["effectorOffset"]).inverse() * \
            jointFrames[2]
        target = carry.inverse() * effectorTarget
        solved = control.Solve(target, parent)
        for name in _Writable(rig, ik, _TRANSLATE + _ROTATE):
            values["%s.avars:%s" % (ik, name)] = solved[name]
        pole = limb["pole"]
        if pole and limb["poleDistance"]:
            # The pole may follow the IK control (a leg's pole follows the
            # foot): read its parent with the IK control already placed.
            moved = _Frames(rig, values, [pole])[pole]
            channels = _Channels(rig, pole)
            poleParent = channels.Matrix().inverse() * moved
            where = _PolePosition([_Translation(m) for m in jointFrames],
                                  limb["poleDistance"],
                                  _Translation(pose[pole]))
            solvedPole = channels.SolveTranslate(where, poleParent)
            for name in _Writable(rig, pole, _TRANSLATE):
                values["%s.avars:%s" % (pole, name)] = solvedPole[name]
    else:
        # Root to end: each control's parent moves with the control above
        # it, so carry the change down rather than re-evaluating.
        previousOld = previousNew = None
        for control, joint, offset in zip(limb["fkControls"], jointFrames,
                                          limb["fkOffsets"]):
            channels = _Channels(rig, control)
            old = pose[control]
            parent = channels.Matrix().inverse() * old
            if previousOld is not None:
                parent = parent * previousOld.inverse() * previousNew
            solved = channels.Solve(om.MMatrix(offset) * joint, parent)
            for name in _Writable(rig, control, _TRANSLATE + _ROTATE):
                values["%s.avars:%s" % (control, name)] = solved[name]
            previousOld = old
            previousNew = channels.Matrix(solved) * parent
    return values


# -- spaces -----------------------------------------------------------------

def _Error(a, b):
    return max(abs(a[i] - b[i]) for i in range(16))


def _MatchSpace(rig, control, spaceAttr, value):
    """{attribute path: value} that sets `spaceAttr` to `value` and keeps
    `control` where it is."""
    target = _Frames(rig, {}, [control])[control]
    channels = _Channels(rig, control)
    names = _Writable(rig, control, _TRANSLATE + _ROTATE)
    values = {}
    plan = {spaceAttr: float(value)}
    for _ in range(_PASSES):
        posed = _Frames(rig, plan, [control])[control]
        if _Error(posed, target) < _SETTLED:
            break
        parent = channels.Matrix(values).inverse() * posed
        solved = channels.Solve(target, parent)
        values.update({n: solved[n] for n in names})
        for name in names:
            plan["%s.avars:%s" % (control, name)] = values[name]
    return plan
