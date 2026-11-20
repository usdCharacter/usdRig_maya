"""
Matched switches: change a limb between IK and FK, or a control's space,
without anything moving.

The rig does the matching. Each limb's RigExecIkFkMatchConstraint publishes,
every evaluation, the channel values each half needs to hold the joints
where they are (outputs:ikValues, outputs:fkValues), and each space-switched
control publishes its channels per space (spaces:matchValues). A switch
reads them from the rigExecRig node (the rigExecPublished command) with the
rig posed as Maya has it, and writes them with the switch value. Nothing is
solved here. The layouts and rules are usdRig's own
(plugin/rigExecUsdview/ikfkMatch.py and spaceMatch.py):

    outputs:ikValues = [ikControl tx ty tz rx ry rz, pole tx ty tz,
                        upperScale, lowerScale]
    outputs:fkValues = [fkControls[0..2] tx ty tz rx ry rz,
                        upperScale, lowerScale]
    spaces:matchValues = [tx ty tz rx ry rz] per space

A switch to FK also zeroes the pin and the twist, and only the channels the
export found writable (not written or driven by the rig) are written.

    plan = Plan(rig, "/Biped/Rig/.../L_Arm.avars:ikfk", 1.0)
    # {attribute path: value}: the matched channels and the switch itself
"""

import math

import maya.mel as mel

_TRANSLATE = ("tx", "ty", "tz")
_ROTATE = ("rx", "ry", "rz")
_CHANNELS = _TRANSLATE + _ROTATE
# A switch counts as sitting on one half only this close to its value;
# mid-blend both halves drive.
_ON_VALUE = 1e-4


def Published(rig, path):
    """The values the rig publishes at `path` (a property path), as Maya
    poses it now."""
    values = mel.eval('rigExecPublished "%s" "%s"' % (rig.node, path))
    if not values or not all(math.isfinite(v) for v in values):
        raise RuntimeError("the rig publishes no %s" % path)
    return list(values)


def Plan(rig, attrPath, value):
    """{attribute path: value} that sets the switch `attrPath` to `value`
    and keeps the pose still. `rig` is a picker.Rig. A switch that is
    neither an IK/FK limb's nor a space switch's just takes its value."""
    limb = LimbOfSwitch(rig, attrPath)
    if limb is not None:
        toIk = abs(value - limb["ikValue"]) < \
            abs(value - (1.0 - limb["ikValue"]))
        return LimbPlan(rig, limb, toIk)
    space = rig.spaces.get(attrPath)
    if space is not None:
        return SpacePlan(rig, attrPath, space, value)
    return {attrPath: float(value)}


# -- IK/FK ----------------------------------------------------------------

def LimbOfSwitch(rig, attrPath):
    return next((l for l in rig.limbs if l["switch"] == attrPath), None)


def SwitchValue(rig, limb):
    """The limb's switch as Maya has it now, or None."""
    plug = rig.PlugForPath(limb["switch"])
    return float(rig.Get(plug)) if plug else None


def IsIk(rig, limb):
    """Whether the IK half drives the joints now (the switch is nearer the
    IK value)."""
    value = SwitchValue(rig, limb)
    if value is None:
        return False
    return abs(value - limb["ikValue"]) < \
        abs(value - (1.0 - limb["ikValue"]))


def LimbPlan(rig, limb, toIk):
    """{attribute path: value} switching `limb` to the half `toIk` names,
    posed onto its joints from the values the rig published."""
    values = Published(rig, limb["match"] + (".outputs:ikValues" if toIk
                                             else ".outputs:fkValues"))
    if len(values) != (11 if toIk else 20):
        raise RuntimeError("%s published %d values" % (limb["match"],
                                                      len(values)))
    writable = set(limb.get("writable") or ())
    plan = {}

    def put(control, names, numbers):
        for name, number in zip(names, numbers):
            path = "%s.avars:%s" % (control, name)
            if path in writable:
                plan[path] = float(number)

    def channel(path, number):
        if path and path in writable:
            plan[path] = float(number)

    if toIk:
        put(limb["ikControl"], _CHANNELS, values[0:6])
        if limb["pole"]:
            put(limb["pole"], _TRANSLATE, values[6:9])
        channel(limb["upperScale"], values[9])
        channel(limb["lowerScale"], values[10])
    else:
        for i, control in enumerate(limb["fkControls"]):
            put(control, _CHANNELS, values[i * 6:i * 6 + 6])
        channel(limb["upperScale"], values[18])
        channel(limb["lowerScale"], values[19])
        channel(limb["pin"], 0.0)
        channel(limb["twist"], 0.0)
    plan[limb["switch"]] = limb["ikValue"] if toIk else \
        1.0 - limb["ikValue"]
    return plan


def _Under(path, root):
    return bool(root) and (path == root or path.startswith(root + "/"))


def LimbFor(rig, path):
    """The limb a control belongs to (its switch control, an FK or IK
    control, the pole, or anything under them), or None."""
    for limb in rig.limbs:
        switchControl = limb["switch"].rpartition(".")[0]
        if path == switchControl or _Side(limb, path) is not None:
            return limb
    return None


def _Side(limb, path):
    """"ik" or "fk" for a control on one half of `limb`, else None. The IK
    half is the IK control, the pole and every control under them (a
    foot's roll controls); the FK half is the FK controls and everything
    under them."""
    if any(_Under(path, c) for c in limb["fkControls"]):
        return "fk"
    if _Under(path, limb["ikControl"]) or _Under(path, limb["pole"]):
        return "ik"
    return None


def PresentedLimb(rig, path):
    """The limb whose idle half `path` is on: an FK control while the limb
    sits on IK, or an IK control, pole or roll control while it sits on
    FK. None mid-blend, or for a control of the half that drives (as
    usdRig's ikfkMatch.MatchHalf.Presented)."""
    for limb in rig.limbs:
        side = _Side(limb, path)
        if side is None:
            continue
        value = SwitchValue(rig, limb)
        if value is None:
            continue
        onIk = abs(value - limb["ikValue"]) <= _ON_VALUE
        onFk = abs(value - (1.0 - limb["ikValue"])) <= _ON_VALUE
        if (side == "fk" and onIk) or (side == "ik" and onFk):
            return limb
    return None


def LimbLabel(limb):
    """The limb's switch control's name (L_Arm)."""
    return limb["switch"].rpartition(".")[0].rpartition("/")[2]


# -- spaces -----------------------------------------------------------------

def SpaceOf(rig, path):
    """(space attribute path, switch) for the control at `path`, or (None,
    None)."""
    for attrPath, space in rig.spaces.items():
        if space["control"] == path:
            return attrPath, space
    return None, None


def SpacePlan(rig, attrPath, space, value):
    """{attribute path: value} that sets `attrPath` to `value` and keeps the
    switched control where it is."""
    control = space["control"]
    values = Published(rig, control + ".spaces:matchValues")
    index = int(round(float(value)))
    stride = len(_CHANNELS)
    if index < 0 or (index + 1) * stride > len(values):
        raise RuntimeError("%s publishes no match for space %d"
                           % (control.rpartition("/")[2], index))
    row = dict(zip(_CHANNELS, values[index * stride:(index + 1) * stride]))
    plan = {attrPath: float(value)}
    for name in space.get("writable") or ():
        plan["%s.avars:%s" % (control, name)] = row[name]
    return plan
