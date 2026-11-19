"""
The right-click marking menu for rig controls in the viewport.

Right-click over a control (its curves, or a TouchPose region, which stands
for its control) and the rig's Selection menu opens: flick toward a
direction and release to run a command, or hold to see the ring and
release over an item. A quick tap leaves the ring up to be clicked; Escape
or a click away from it closes it. A submenu leaves a circle where the menu
above it was, and moving back onto the circle goes back. While controls
are selected, a right-click anywhere in the viewport opens it for them.
Over anything else, with nothing of the rig selected, Maya's own
right-click menu opens as always. Alt+right-click stays the camera's.

The menu looks the way it does in usdview and Unreal (see radialmenu): the
column below the ring holds the RigExec switches, the TouchPose highlight
strength and the picker.

Right-clicking a control that is not selected selects it first, so the menu
always acts on what is highlighted.

The items and where they sit are usdRig's Selection menu
(plugin/rigExecUsdview/markingMenuDefs.py), the same one usdview and the
Unreal plug-in use, so a flick means the same thing in every host:

    N  Frame              S  Zero pose
    W  Key                E  Reset to rest
    NW FK/IK toggle       NE Space (submenu)
    SW Select counterpart

    import rigexec_maya.markingmenu
    rigexec_maya.markingmenu.Install()
"""

import importlib.util
import math
import os
import re
import sys

import maya.api.OpenMaya as om
import maya.api.OpenMayaUI as omui
import maya.cmds as cmds
import maya.mel as mel

try:
    from PySide6 import QtCore, QtGui, QtWidgets
    import shiboken6 as shiboken
except ImportError:
    from PySide2 import QtCore, QtGui, QtWidgets
    import shiboken2 as shiboken

from . import export as _export
from . import matching as _matching
from . import picker as _picker

_MENU = "rigExecMarkingMenu"
_OPTION = "rigExecMarkingMenu"

_controller = None

# The transform channels Reset to rest puts back, and their rest values.
# Everything else a control carries is Zero pose's.
_REST_VALUES = {"tx": 0.0, "ty": 0.0, "tz": 0.0, "rx": 0.0, "ry": 0.0,
                "rz": 0.0, "rspin": 0.0, "sx": 1.0, "sy": 1.0, "sz": 1.0}

_defs = None


def _Defs():
    """usdRig's marking menu declarations (markingMenuDefs, which imports
    markingMenuModel), from the usdRig checkout. Both are free of Qt and
    USD."""
    global _defs
    if _defs is None:
        folder = os.path.join(_export.RigExecRoot(), "plugin",
                              "rigExecUsdview")
        modules = {}
        for name in ("markingMenuModel", "markingMenuDefs"):
            path = os.path.join(folder, name + ".py")
            if not os.path.exists(path):
                raise RuntimeError("usdRig's marking menu was not found at "
                                   "%s" % path)
            spec = importlib.util.spec_from_file_location(name, path)
            module = importlib.util.module_from_spec(spec)
            # markingMenuDefs imports markingMenuModel by name.
            sys.modules.setdefault(name, module)
            spec.loader.exec_module(sys.modules[name])
            modules[name] = sys.modules[name]
        _defs = modules["markingMenuDefs"]
    return _defs


# -- installing ---------------------------------------------------------------

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


def _ObjectMenus():
    """Maya's own right-click marking menu in each viewport (the one it
    builds with buildObjectMenuItemsNow), by full path."""
    menus = []
    for panel in cmds.getPanel(type="modelPanel") or []:
        for menu in cmds.lsUI(type="popupMenu", long=True) or []:
            if menu.endswith("|%sObjectPop" % panel):
                menus.append(menu)
    return menus


def Install():
    """Puts the menu on the viewport's right button, if it is on. Over
    anything that is not the rig's, Maya's own menu opens as always."""
    global _controller
    Uninstall()
    if IsEnabled():
        _controller = _Controller()


def Uninstall():
    global _controller
    if _controller is not None:
        _controller.Close()
        _controller = None
    # What earlier versions put on Maya's own menu.
    if cmds.popupMenu(_MENU, exists=True):
        cmds.deleteUI(_MENU)
    for menu in _ObjectMenus():
        cmds.popupMenu(menu, edit=True,
                       postMenuCommand='buildObjectMenuItemsNow "%s"' % menu)


def IsOpen():
    """Whether the menu is up (TouchPose leaves the clicks to it)."""
    return _controller is not None and _controller.gesture is not None


# -- what the click is on -----------------------------------------------------

def _CursorInView():
    """(view, x, y): the active 3D view and the cursor in its port
    coordinates (origin bottom left), or (None, 0, 0)."""
    try:
        view = omui.M3dView.active3dView()
        widget = shiboken.wrapInstance(int(view.widget()), QtWidgets.QWidget)
    except Exception:
        return None, 0, 0
    local = widget.mapFromGlobal(QtGui.QCursor.pos())
    if not widget.rect().contains(local):
        return None, 0, 0
    scaleX = view.portWidth() / float(max(widget.width(), 1))
    scaleY = view.portHeight() / float(max(widget.height(), 1))
    return view, int(local.x() * scaleX), \
        int((widget.height() - local.y()) * scaleY)


# How near the cursor a control's curve must pass, in pixels.
_PICK_PIXELS = 8.0


def _ControlUnderCursor(rig, view, x, y):
    """The rig path of the control whose curves pass nearest the cursor
    (within _PICK_PIXELS), or None. Measured on screen without picking, so
    the selection never changes for it."""
    toClip = view.modelViewMatrix() * view.projectionMatrix()
    width, height = float(view.portWidth()), float(view.portHeight())

    def screen(point):
        clip = om.MPoint(point) * toClip
        if clip.w <= 1e-9:
            return None
        return ((clip.x / clip.w * 0.5 + 0.5) * width,
                (clip.y / clip.w * 0.5 + 0.5) * height)

    best, bestDistance = None, _PICK_PIXELS
    for path, curves in _Curves(rig):
        for dagPath in curves:
            # The curve's box on screen first: most are nowhere near.
            box = om.MFnDagNode(dagPath).boundingBox
            box.transformUsing(dagPath.inclusiveMatrix())
            corners = [screen((cx, cy, cz))
                       for cx in (box.min.x, box.max.x)
                       for cy in (box.min.y, box.max.y)
                       for cz in (box.min.z, box.max.z)]
            corners = [c for c in corners if c is not None]
            if not corners or                     x < min(c[0] for c in corners) - _PICK_PIXELS or                     x > max(c[0] for c in corners) + _PICK_PIXELS or                     y < min(c[1] for c in corners) - _PICK_PIXELS or                     y > max(c[1] for c in corners) + _PICK_PIXELS:
                continue
            points = om.MFnNurbsCurve(dagPath).cvPositions(om.MSpace.kWorld)
            onScreen = [screen(p) for p in points]
            for a, b in zip(onScreen, onScreen[1:]):
                if a is None or b is None:
                    continue
                distance = _SegmentDistance(x, y, a, b)
                if distance < bestDistance:
                    best, bestDistance = path, distance
    return best


def _SegmentDistance(x, y, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = dx * dx + dy * dy
    t = 0.0 if length <= 1e-12 else         max(0.0, min(1.0, ((x - a[0]) * dx + (y - a[1]) * dy) / length))
    return math.hypot(x - (a[0] + t * dx), y - (a[1] + t * dy))


def _Curves(rig):
    """[(rig path, [curve dag paths])] for every control with a shape, kept
    on the rig."""
    curves = getattr(rig, "_markingMenuCurves", None)
    if curves is None:
        curves = []
        for path, node in rig.controls.items():
            shapes = cmds.listRelatives(node, shapes=True, fullPath=True,
                                        type="nurbsCurve") or []
            if shapes:
                selection = om.MSelectionList()
                for shape in shapes:
                    selection.add(shape)
                curves.append((path, [selection.getDagPath(i)
                                      for i in range(len(shapes))]))
        rig._markingMenuCurves = curves
    return curves


def _Target():
    """(rig, control paths the menu acts on), or (None, [])."""
    from . import automatch, touchpose
    rigs = automatch.Rigs()
    if not rigs:
        return None, []
    view, x, y = _CursorInView()
    hitRig = hitPath = None
    if view is not None:
        # A control's curves in front win over the skin under them.
        for rig in rigs:
            hitPath = _ControlUnderCursor(rig, view, x, y)
            if hitPath:
                hitRig = rig
                break
        if hitPath is None:
            hitRig, hitPath = touchpose.ControlAt(x, y, view)
    if hitPath:
        # A control that is not selected is selected first, so the menu
        # acts on what is highlighted.
        if hitPath not in hitRig.SelectedPaths():
            cmds.select(hitRig.controls[hitPath], replace=True)
        return hitRig, sorted(hitRig.SelectedPaths())
    for rig in rigs:
        selected = rig.SelectedPaths()
        if selected:
            return rig, sorted(selected)
    return None, []


# -- building -----------------------------------------------------------------

class _Context(object):
    """Answers the menu's questions (markingMenuDefs names them) for the
    selected controls."""

    def __init__(self, rig, paths):
        self.rig = rig
        self.paths = paths
        self.limb = next((l for l in (_matching.LimbFor(rig, p)
                                      for p in paths) if l), None)
        self.spaceAttr, self.space = None, None
        for path in paths:
            self.spaceAttr, self.space = _matching.SpaceOf(rig, path)
            if self.space is not None:
                break

    def Test(self, name):
        if not name:
            return True
        if name == "limb":
            return self.limb is not None
        if name == "hasSpaces":
            return self.space is not None and \
                len(self.space.get("labels") or []) > 1
        if name == "control":
            return bool(self.paths)
        return False

    def Read(self, name):
        from . import automatch, touchpose
        if name == "limbIsIk":
            return self.limb is not None and _matching.IsIk(self.rig,
                                                            self.limb)
        if name == "space":
            plug = self.rig.PlugForPath(self.spaceAttr) \
                if self.spaceAttr else None
            return int(round(cmds.getAttr(plug))) if plug else None
        if name == "autoSwitch":
            return automatch.IsEnabled()
        if name == "touchPose":
            return touchpose.IsEnabled()
        if name == "strength":
            strength = touchpose.Strength()
            return next((v for _l, v in touchpose.STRENGTH_PRESETS
                         if abs(v - strength) < 0.01), None)
        return None

    def Label(self, itemId, default):
        if itemId == "selection.fkik" and self.limb is not None:
            return "Switch to %s" % (
                "FK" if _matching.IsIk(self.rig, self.limb) else "IK")
        return default

    def Children(self, name):
        if name == "spaces" and self.space is not None:
            return [dict(id="selection.space.%d" % i, label=str(label),
                         kind="radio", read="space", value=i,
                         write="SetSpace")
                    for i, label in enumerate(self.space.get("labels")
                                              or [])]
        if name == "strengths":
            from . import touchpose
            return [dict(id="maya.strength.%d" % i,
                         label="%s (%d%%)" % (label, round(value * 100)),
                         kind="radio", read="strength", value=value,
                         write="SetStrength")
                    for i, (label, value) in
                    enumerate(touchpose.STRENGTH_PRESETS)] + [
                dict(id="maya.strength.options", label="Options...",
                     write="TouchPoseOptions")]
        return ()


def _Menu():
    """usdRig's Selection menu, its ring as declared, with Maya's own rows
    in the column below it."""
    defs = _Defs()
    model = sys.modules["markingMenuModel"]
    items = [item for item in defs.SELECTION.items if not item.overflow]
    items += [
        dict(id="maya.autoSwitch", label="IK/FK auto-switch",
             kind=model.TOGGLE, overflow=True, read="autoSwitch",
             write="SetAutoSwitch"),
        dict(id="maya.touchPose", label="TouchPose", kind=model.TOGGLE,
             overflow=True, read="touchPose", write="SetTouchPose"),
        dict(id="maya.highlight", label="Highlight", kind=model.SUBMENU,
             overflow=True, children="strengths"),
        dict(id="maya.picker", label="Picker...", overflow=True,
             write="ShowPicker"),
    ]
    return model.Menu("selection", items, label=defs.SELECTION.label)


class _Controller(QtCore.QObject):
    """Watches every viewport's right button and runs the gesture."""

    def __init__(self):
        super(_Controller, self).__init__()
        _Defs()
        self.mm = sys.modules["markingMenuModel"]
        self.gesture = None
        self.view = None            # the viewport widget the menu is on
        self.window = None
        self.widgets = []
        # Each viewport's native window, by the viewport: a right-click
        # reaches it before Maya's own menu sees it.
        self.windows = {}
        self.pressed = False
        self.sectorLit = None
        self.lastPoint = None
        self.eventSinceTick = False
        self.rig = None
        self.context = None
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(16)
        self.timer.timeout.connect(self._Tick)
        # Viewports come and go (a torn-off panel, a new layout).
        self.watchTimer = QtCore.QTimer(self)
        self.watchTimer.setInterval(1000)
        self.watchTimer.timeout.connect(self._Watch)
        self.watchTimer.start()
        self._Watch()

    def _Watch(self):
        current = []
        for panel in cmds.getPanel(type="modelPanel") or []:
            try:
                view = omui.M3dView.getM3dViewFromModelPanel(panel)
                current.append(int(view.widget()))
            except Exception:
                continue
        known = []
        for widget in self.widgets:
            try:
                known.append(shiboken.getCppPointer(widget)[0])
            except Exception:
                continue
        if sorted(current) == sorted(known):
            return
        self._Unwatch()
        for pointer in current:
            widget = shiboken.wrapInstance(pointer, QtWidgets.QWidget)
            widget.installEventFilter(self)
            self.widgets.append(widget)
            handle = widget.windowHandle()
            if handle is not None:
                handle.installEventFilter(self)
                self.windows[shiboken.getCppPointer(handle)[0]] =                     (handle, widget)

    def _Unwatch(self):
        for watched in self.widgets + [h for h, _w in
                                       self.windows.values()]:
            try:
                watched.removeEventFilter(self)
            except RuntimeError:
                pass
        self.widgets = []
        self.windows = {}

    def Close(self):
        if self.gesture is not None:
            self._Finish()
        self.watchTimer.stop()
        self._Unwatch()
        if self.window is not None:
            self.window.deleteLater()
            self.window = None

    # -- the gesture -----------------------------------------------------

    def _Open(self, widget, point, rig, paths):
        from . import radialmenu
        self.view = widget
        self.rig = rig
        self.context = _Context(rig, paths)
        if self.window is None:
            self.window = radialmenu.Window(self.mm, widget.window())
        self.window.Cover(widget)
        self.gesture = self.mm.Gesture(
            _Menu(), self.context,
            blocker=self.window.painter.CoveredDirections)
        self.lastPoint = point
        self.pressed = True
        self._Handle(self.gesture.Press(point[0], point[1]))
        self.timer.start()
        QtWidgets.QApplication.instance().installEventFilter(self)

    def _Finish(self):
        self.gesture = None
        self.pressed = False
        self.timer.stop()
        QtWidgets.QApplication.instance().removeEventFilter(self)
        if self.window is not None:
            self.window.hide()
            self.window.menu = None
        if self.view is not None and \
                QtWidgets.QWidget.mouseGrabber() is self.view:
            self.view.releaseMouse()

    def _Handle(self, emissions):
        window, mm = self.window, self.mm
        for emission in emissions:
            if self.gesture is None:
                return
            kind = emission.kind
            if kind in (mm.SHOW, mm.OPEN):
                window.menu = emission.menu
                window.centre = window.ToLocal(self.view, emission.centre)
                window.lit = window.litBack = self.sectorLit = None
                window.parents = [window.ToLocal(self.view, c) for c in
                                  self.gesture.parentCentres]
                window.show()
                window.raise_()
                window.update()
                if emission.mode == mm.MODE_CLICK and \
                        QtWidgets.QWidget.mouseGrabber() is not self.view:
                    # Up for clicking: every click, even one outside the
                    # viewport, is the menu's to take or dismiss.
                    self.view.grabMouse()
            elif kind == mm.HIGHLIGHT:
                self.sectorLit = emission.item.id if emission.item else None
                window.lit = self.sectorLit
                window.update()
            elif kind == mm.CONFIRM:
                rig, context = self.rig, self.context
                self._Finish()
                # After the event that confirmed it, so Maya has the
                # mouse back first.
                QtCore.QTimer.singleShot(
                    0, lambda item=emission.item: _Run(rig, context, item))
            elif kind == mm.CANCEL:
                self._Finish()

    def _ItemUnder(self, point):
        window = self.window
        if window is None or not window.isVisible() or window.menu is None:
            return None
        return window.painter.ItemAt(window.menu, window.centre,
                                     window.ToLocal(self.view, point),
                                     window.parents)

    def _MovedTo(self, point):
        window = self.window
        if window is None or not window.isVisible():
            return
        flick = self.gesture.mode == self.mm.MODE_FLICK and self.pressed
        window.cursor = window.ToLocal(self.view, point) if flick else None
        if window.menu is not None:
            over = self._ItemUnder(point)
            if over is not None:
                window.lit = over.id
            elif flick:
                window.lit = self.sectorLit
            else:
                window.lit = None
        window.litBack = self.gesture.BackTarget(point[0], point[1])
        window.update()

    def _Tick(self):
        if self.gesture is None:
            return
        self._Handle(self.gesture.Tick())
        if self.gesture is None:
            return
        if not self.eventSinceTick and \
                self.gesture.state == self.mm.STATE_CLICK:
            # Up for clicking, the viewport may report no moves at all:
            # follow the cursor itself.
            local = self.view.mapFromGlobal(QtGui.QCursor.pos())
            point = (local.x(), local.y())
            if point != self.lastPoint:
                self.lastPoint = point
                self._Handle(self.gesture.Move(point[0], point[1]))
        self.eventSinceTick = False
        if self.gesture is not None and self.lastPoint is not None:
            self._MovedTo(self.lastPoint)

    def _ClickAt(self, point):
        if self.gesture.BackTarget(point[0], point[1]) is not None:
            self._Handle(self.gesture.Press(point[0], point[1]))
            return
        over = self._ItemUnder(point)
        if over is not None:
            if over.enabled:
                self._Handle(self.gesture.Choose(over.id))
            return
        self._Handle(self.gesture.Press(point[0], point[1]))

    def _Point(self, event):
        """The event's position in the menu's viewport."""
        try:
            pos = event.globalPosition().toPoint()
        except AttributeError:
            pos = event.globalPos()
        local = self.view.mapFromGlobal(pos)
        return (local.x(), local.y())

    def eventFilter(self, watched, event):
        try:
            return self._Event(watched, event)
        except Exception as error:
            om.MGlobal.displayWarning("RigExec marking menu: %s" % error)
            if self.gesture is not None:
                self._Finish()
            return False

    def _Event(self, watched, event):
        kind = event.type()
        # A viewport's native window stands for the viewport.
        if isinstance(watched, QtGui.QWindow):
            found = self.windows.get(shiboken.getCppPointer(watched)[0])
            if found is None:
                return False
            watched = found[1]
        if self.gesture is not None:
            if kind in (QtCore.QEvent.KeyPress,
                        QtCore.QEvent.ShortcutOverride) and \
                    event.key() == QtCore.Qt.Key_Escape:
                self._Handle(self.gesture.Cancel())
                event.accept()
                return True
            if watched is not self.view or \
                    not isinstance(event, QtGui.QMouseEvent):
                return False
            point = self._Point(event)
            if kind in (QtCore.QEvent.MouseButtonPress,
                        QtCore.QEvent.MouseButtonDblClick):
                if self.gesture.state == self.mm.STATE_CLICK:
                    self._ClickAt(point)
                else:
                    self._Handle(self.gesture.Press(point[0], point[1]))
                return True
            if kind == QtCore.QEvent.MouseMove:
                self.lastPoint = point
                self.eventSinceTick = True
                self._Handle(self.gesture.Move(point[0], point[1]))
                if self.gesture is not None:
                    self._MovedTo(point)
                return True
            if kind == QtCore.QEvent.MouseButtonRelease:
                self.pressed = False
                over = self._ItemUnder(point)
                self._Handle(self.gesture.Release(
                    point[0], point[1],
                    itemId=over.id if over is not None else None))
                return True
            return False
        if kind == QtCore.QEvent.MouseButtonPress and \
                watched in self.widgets and \
                event.button() == QtCore.Qt.RightButton and \
                not event.modifiers() & QtCore.Qt.AltModifier:
            try:
                rig, paths = _Target()
            except Exception as error:
                om.MGlobal.displayWarning("RigExec marking menu: %s" % error)
                return False
            if rig is None:
                return False            # Maya's own menu
            self.view = watched
            self._Open(watched, self._Point(event), rig, paths)
            return True
        return False


def _Run(rig, context, resolved):
    """Runs a confirmed item, by the `write` name it carries, as one undo
    step."""
    from . import automatch, touchpose
    item = resolved.item
    paths = context.paths
    actions = {
        "KeySelection": lambda: _Key(rig, paths),
        "ResetToRest": lambda: _ResetToRest(rig, paths),
        "ZeroPose": lambda: rig.ZeroControls(_Status),
        "FrameSelection": lambda: cmds.viewFit(
            [rig.controls[p] for p in paths]),
        "ToggleFkIk": lambda: _ToggleFkIk(rig, context.limb),
        "SelectCounterpart": lambda: _SelectCounterpart(rig, paths),
        "SetSpace": lambda: _SetSpace(
            rig, context.spaceAttr, item.value,
            (context.space.get("labels") or [])[item.value]),
        "SetAutoSwitch": lambda: automatch.SetEnabled(
            not automatch.IsEnabled()),
        "SetTouchPose": lambda: touchpose.SetEnabled(
            not touchpose.IsEnabled()),
        "SetStrength": lambda: touchpose.SetStrength(item.value),
        "TouchPoseOptions": touchpose.ShowOptions,
        "ShowPicker": _picker.Show,
    }
    run = actions.get(item.write)
    if run is None:
        return
    cmds.undoInfo(openChunk=True, chunkName="RigExec " + resolved.label)
    try:
        run()
    except Exception as error:
        om.MGlobal.displayWarning("RigExec: %s failed: %s"
                                  % (resolved.label, error))
    finally:
        cmds.undoInfo(closeChunk=True)


# -- the commands -------------------------------------------------------------

def _Status(text):
    om.MGlobal.displayInfo("RigExec: " + text)


def _ChannelPlugs(rig, paths):
    """Every plug the rig reads on the controls at `paths`."""
    nodes = set(rig.controls[p] for p in paths if p in rig.controls)
    out = []
    for plug, _index in rig.ChannelSources():
        node = cmds.ls(plug.split(".", 1)[0], long=True)
        if node and node[0] in nodes:
            out.append(plug)
    return out


def _Key(rig, paths):
    plugs = _ChannelPlugs(rig, paths)
    if plugs:
        cmds.setKeyframe(plugs)
    _Status("Keyed %d channels on %d controls" % (len(plugs), len(paths)))


def _ResetToRest(rig, paths):
    plan = {}
    for path in paths:
        for name, rest in _REST_VALUES.items():
            plug = rig.Plug(path, "avars:" + name)
            if plug and abs(cmds.getAttr(plug) - rest) > 1e-12:
                plan["%s.avars:%s" % (path, name)] = rest
    count = rig.Write(plan, "Reset to rest")
    _Status("Reset %d channels to rest" % count)


def _ToggleFkIk(rig, limb):
    if limb is None:
        return
    half = rig.SwitchLimb(limb)
    if half:
        _Status("%s to %s (matched)" % (_matching.LimbLabel(limb), half))


def _SetSpace(rig, attrPath, index, label):
    rig.SwitchSpace(attrPath, float(index), label)
    _Status("%s space: %s (matched)"
            % (attrPath.rpartition(".")[0].rpartition("/")[2], label))


# The side markers a counterpart swaps, as usdRig's markingMenuUI has them.
_SIDE_SWAPS = (
    (re.compile(r"^L_"), "R_"), (re.compile(r"^R_"), "L_"),
    (re.compile(r"^l_"), "r_"), (re.compile(r"^r_"), "l_"),
    (re.compile(r"_L_"), "_R_"), (re.compile(r"_R_"), "_L_"),
    (re.compile(r"_l_"), "_r_"), (re.compile(r"_r_"), "_l_"),
    (re.compile(r"_L$"), "_R"), (re.compile(r"_R$"), "_L"),
    (re.compile(r"_l$"), "_r"), (re.compile(r"_r$"), "_l"),
    (re.compile(r"^Left"), "Right"), (re.compile(r"^Right"), "Left"),
    (re.compile(r"^left"), "right"), (re.compile(r"^right"), "left"),
)


def CounterpartName(name):
    """The control's name on the other side (L_Arm -> R_Arm), or None for a
    name with no side marker."""
    for pattern, replacement in _SIDE_SWAPS:
        if pattern.search(name):
            return pattern.sub(replacement, name, count=1)
    return None


def CounterpartPath(rig, path):
    """The mirrored control's path in `rig`, or None: a sibling first, then
    the whole path mirrored, then any control of that name."""
    parent, _, name = path.rpartition("/")
    other = CounterpartName(name)
    if other is None:
        return None
    for candidate in (parent + "/" + other,
                      "/".join(CounterpartName(e) or e
                               for e in path.split("/"))):
        if candidate in rig.controls:
            return candidate
    return next((p for p in rig.controls
                 if p.rpartition("/")[2] == other), None)


def _SelectCounterpart(rig, paths):
    found = [CounterpartPath(rig, p) for p in paths]
    nodes = [rig.controls[p] for p in found if p]
    if nodes:
        cmds.select(nodes, replace=True)
    _Status("Selected %d counterparts" % len(nodes)
            if nodes else "No counterpart")
