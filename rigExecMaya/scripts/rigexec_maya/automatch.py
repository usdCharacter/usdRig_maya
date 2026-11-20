"""
IK/FK auto-switch: selecting a control of a limb's idle half switches the
limb to that half, matched, so the control then drives from where it is
drawn.

The idle half is the one the limb's switch does not drive: the FK controls
while the limb sits on IK, the IK control, its pole or a foot roll control
while it sits on FK. The rig draws the idle half on the joints, so it can be
picked. Selecting one switches the limb (one undo step, keyed when auto key
is on), exactly as usdview's gizmo does (_SwitchPresentedLimbs) and the
Unreal plug-in does (RigExecAutoMatch). A control that stays selected never
switches its limb back, mid-blend nothing switches, and the selection an
undo or redo restores is never taken as a new pick.

    import rigexec_maya.automatch
    rigexec_maya.automatch.SetEnabled(False)

On by default; the choice is kept in the rigExecAutoMatch optionVar.
"""

import maya.api.OpenMaya as om
import maya.cmds as cmds

from . import matching as _matching
from . import picker as _picker

_OPTION = "rigExecAutoMatch"

_jobs = []
_seen = set()          # the controls selected as last seen, long names
_skipNext = False      # an undo or redo just restored the selection
_pending = False
_rigs = {}             # rigExecRig node -> picker.Rig


def IsEnabled():
    if not cmds.optionVar(exists=_OPTION):
        return True
    return bool(cmds.optionVar(query=_OPTION))


def SetEnabled(enabled):
    cmds.optionVar(intValue=(_OPTION, int(bool(enabled))))
    if enabled:
        Install()
    else:
        Uninstall()


def Install():
    """Starts watching the selection, if auto-switch is on."""
    global _seen
    Uninstall()
    if not IsEnabled():
        return
    _seen = set(cmds.ls(selection=True, long=True) or [])
    _jobs.append(cmds.scriptJob(event=["SelectionChanged", _OnSelection]))
    _jobs.append(cmds.scriptJob(event=["Undo", _OnUndo]))
    _jobs.append(cmds.scriptJob(event=["Redo", _OnUndo]))
    for event in ("SceneOpened", "NewSceneOpened"):
        _jobs.append(cmds.scriptJob(event=[event, Forget]))


def Uninstall():
    while _jobs:
        job = _jobs.pop()
        if cmds.scriptJob(exists=job):
            cmds.scriptJob(kill=job, force=True)


def Forget():
    """Drops the cached rigs (a scene was opened, or a rig rebuilt)."""
    _rigs.clear()


def _OnUndo():
    global _skipNext
    _skipNext = True


def _OnSelection():
    # Deferred: an undo's Undo event lands after the selection it restores,
    # and the switch must not run inside Maya's selection change.
    global _pending
    if not _pending:
        _pending = True
        cmds.evalDeferred(_Check, lowestPriority=True)


def Rigs():
    """The scene's rigs, kept until one is added, deleted or reloaded."""
    return _Rigs()


def _Rigs():
    nodes = cmds.ls(type="rigExecRig") or []
    for node in list(_rigs):
        if node not in nodes:
            del _rigs[node]
    for node in nodes:
        if node not in _rigs:
            _rigs[node] = _picker.Rig(node)
    return list(_rigs.values())


def _Check():
    global _pending, _skipNext, _seen
    _pending = False
    skip = _skipNext
    _skipNext = False
    selected = cmds.ls(selection=True, long=True) or []
    picked = [n for n in selected if n not in _seen]
    _seen = set(selected)
    if skip or not picked:
        return
    done = []
    for rig in _Rigs():
        byNode = {node: path for path, node in rig.controls.items()}
        limbs = []
        for node in picked:
            path = byNode.get(node)
            limb = _matching.PresentedLimb(rig, path) if path else None
            if limb is not None and limb not in limbs:
                limbs.append(limb)
        for limb in limbs:
            half = rig.SwitchLimb(limb)
            if half:
                done.append("%s to %s" % (_matching.LimbLabel(limb), half))
    if done:
        om.MGlobal.displayInfo("RigExec: switched %s (matched)"
                               % ", ".join(done))
        cmds.inViewMessage(assistMessage="Switched %s (matched)"
                           % ", ".join(done), position="topCenter",
                           fade=True, fadeStayTime=1200)
    # The switch leaves the selection as it was; what is selected now is
    # what was seen.
    _seen = set(cmds.ls(selection=True, long=True) or [])
