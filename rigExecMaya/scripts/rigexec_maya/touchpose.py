"""
TouchPose in Maya: the character's own skin as the control picker, as
usdview's TouchPose plug-in and the Unreal plug-in do it.

Hovering a touch region lights it; a click selects the control it names
(Shift toggles, Ctrl removes, a plain click replaces); the regions of the
selected controls stay lit, the last one selected in the lead colour. The
regions come from the rig (RigExecTouchRegions: faces of a mesh and the
control each set selects), through the export.

A click selects exactly what is lit: TouchPose takes the click on a lit
region for itself, so a control curve near the cursor never wins it
instead. A drag is still Maya's (a marquee): once the mouse moves, the
press is handed back to the viewport. The marquee also picks the
controls whose regions it covers on the side of the skin facing you, as
if their curves were inside it (Shift toggles, Ctrl removes, Ctrl+Shift
adds, as in Maya). Alt is the camera's, and the hover
picker's buttons and the manipulator's handles sit over the skin and win:
over them nothing new is lit, and the click is Maya's. A mesh with no
touch regions (hair, a prop) hides the skin behind it. While the rig is
being moved (a manipulator drag, playback, a scrub, a channel dragged or
typed, an attribute driving a control) TouchPose is off, and it comes back
once the rig has been still for a moment. While TouchPose is on, the rig's meshes cannot be selected in the
viewport (they are shown as reference), so a marquee picks only controls;
turning TouchPose off gives them back as they were.

    import rigexec_maya.touchpose
    rigexec_maya.touchpose.SetEnabled(True)

On by default; the choice is kept in the rigExecTouchPose optionVar. The
highlight is drawn by a rigExecTouchHighlight locator per touch layer.

How strongly the skin is tinted is your own preference, the same for every
rig and scene (RigExec > TouchPose option box, or Highlight in the marking
menu):

    rigexec_maya.touchpose.SetStrength(0.5)
"""

import math

import maya.api.OpenMaya as om
import maya.api.OpenMayaUI as omui
import maya.cmds as cmds

try:
    from PySide6 import QtCore, QtGui, QtWidgets
    import shiboken6 as shiboken
except ImportError:
    from PySide2 import QtCore, QtGui, QtWidgets
    import shiboken2 as shiboken

from . import picker as _picker

_OPTION = "rigExecTouchPose"
# How strongly hovered and selected regions are tinted, 0 to 1.
_STRENGTH_OPTION = "rigExecTouchPoseStrength"
DEFAULT_STRENGTH = 0.30
# The marking menu's choices.
STRENGTH_PRESETS = (("Faint", 0.15), ("Light", 0.30), ("Medium", 0.50),
                    ("Strong", 0.80))
_OPTIONS_WINDOW = "rigExecTouchPoseOptions"
_LOCATOR = "rigExecTouchHighlight"
# Kept on a mesh TouchPose has made unselectable: how its display override
# was before, so it is given back as it was, even after a save.
_LOCK = "rigExecTouchLock"
# A press that travels further than this is a drag, not a click.
_DRAG_SLOP = 3.0
# What the brightest channel of each state's colour is scaled to (usdRig's
# touchPoseModel): a scale, never a mix toward white, so hues stay as
# authored.
_LEAD_VALUE = 0.95
_SELECTED_VALUE = 0.62
_HOVER_VALUE = 1.0
# How long the rig has to sit still after a drag, playback or a scrub
# before the highlight comes back, in milliseconds.
_QUIET_MS = 250
# Maya's manipulators at a manipulator scale of 1, in pixels: the rotate
# sphere's radius (its outer ring included), how far the move and scale
# arrows reach, and how close to a handle counts as on it.
_MANIP_PIXELS = 72.0
_MANIP_ARROW = 90.0
_MANIP_SLOP = 10.0
# How long a held button may keep TouchPose stood down with nothing
# moving, in milliseconds, should Qt miss the button's release.
_HELD_LIMIT_MS = 5000

_controller = None


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


def Strength():
    """How strongly hovered and selected regions are tinted, 0 to 1."""
    if not cmds.optionVar(exists=_STRENGTH_OPTION):
        return DEFAULT_STRENGTH
    return float(cmds.optionVar(query=_STRENGTH_OPTION))


def SetStrength(strength):
    """Sets the highlight strength (0.05 to 1), shown at once."""
    strength = max(0.05, min(1.0, float(strength)))
    cmds.optionVar(floatValue=(_STRENGTH_OPTION, strength))
    if _controller is not None:
        for layer in _controller.layers:
            layer.SetStrength(strength)
    if cmds.window(_OPTIONS_WINDOW, exists=True):
        slider = _OPTIONS_WINDOW + "|column|strength"
        if cmds.floatSliderGrp(slider, exists=True) and abs(
                cmds.floatSliderGrp(slider, query=True, value=True) -
                strength * 100.0) > 1e-3:
            cmds.floatSliderGrp(slider, edit=True, value=strength * 100.0)
    return strength


def StepStrength(step):
    """Makes the highlight stronger (step > 0) or fainter, by 5%."""
    return SetStrength(round(Strength() * 20.0 + step) / 20.0)


def ShowOptions():
    """The TouchPose Options window: the highlight strength, live."""
    if cmds.window(_OPTIONS_WINDOW, exists=True):
        cmds.deleteUI(_OPTIONS_WINDOW)
    cmds.window(_OPTIONS_WINDOW, title="TouchPose Options",
                widthHeight=(380, 80), sizeable=True)
    cmds.columnLayout("column", adjustableColumn=True, rowSpacing=6,
                      columnOffset=("both", 8))
    tip = ("How strongly hovered and selected areas are tinted. Lower it "
           "to see more of the model underneath.")
    cmds.floatSliderGrp(
        "strength", label="Highlight strength", field=True,
        minValue=5, maxValue=100, fieldMinValue=5, fieldMaxValue=100,
        step=5, fieldStep=5, precision=0, value=Strength() * 100.0,
        annotation=tip, columnWidth3=(110, 50, 200),
        dragCommand=lambda value: SetStrength(value / 100.0),
        changeCommand=lambda value: SetStrength(value / 100.0))
    cmds.button(label="Reset to %d%%" % round(DEFAULT_STRENGTH * 100),
                command=lambda *_: SetStrength(DEFAULT_STRENGTH))
    cmds.showWindow(_OPTIONS_WINDOW)


def Install():
    """Starts TouchPose on every rig in the scene, if it is on."""
    global _controller
    Uninstall()
    if IsEnabled():
        _controller = _Controller()


def Uninstall():
    global _controller
    if _controller is not None:
        _controller.Close()
        _controller = None
    # Meshes left unselectable by a scene saved with TouchPose on.
    _Unlock([m for m in cmds.ls(type="mesh", long=True) or []
             if cmds.attributeQuery(_LOCK, node=m, exists=True)])


def Refresh():
    """Re-reads the scene's rigs (after one is loaded or deleted)."""
    if _controller is not None:
        _controller.Rebuild()


def ControlAt(x, y, view=None):
    """(rig, control path) of the touch region at port coordinates (x, y)
    of the active view (origin bottom left), or (None, None)."""
    if _controller is None:
        return None, None
    hit = _controller.HitAt(view or omui.M3dView.active3dView(), x, y)
    if hit is None:
        return None, None
    layer, region = hit
    return layer.rig, layer.ControlOf(region)


def _ScaleTo(color, value):
    peak = max(color)
    if peak <= 1e-6:
        return (value, value, value)
    return tuple(min(1.0, c * value / peak) for c in color)


class _Layer(object):
    """One touch layer of one rig: its mesh in Maya, its regions and the
    locator that lights them."""

    def __init__(self, rig, data):
        self.rig = rig
        self.data = data
        self.regions = data["regions"]
        # The drawn mesh this output feeds.
        self.shape = _MeshFed(
            "%s.outMeshFrame[%d]" % (rig.node, data["mesh"])) or _MeshFed(
            "%s.outMesh[%d]" % (rig.node, data["mesh"]))
        self.regionOf = {}
        for index, region in enumerate(self.regions):
            for face in region["faces"]:
                self.regionOf[face] = index
        self.byControl = {}
        for index, region in enumerate(self.regions):
            path = rig.controlPaths[region["control"]] \
                if region["control"] < len(rig.controlPaths) else None
            if path:
                self.byControl.setdefault(path, []).append(index)
        self.locator = None
        self._shown = None
        self._faceVertices = None

    def RegionsIn(self, view, left, bottom, right, top):
        """The regions with a face facing the camera whose centre lies in
        the port rectangle (origin bottom left)."""
        if not self.shape or not cmds.objExists(self.shape):
            return set()
        path = om.MSelectionList().add(self.shape).getDagPath(0)
        if not path.isVisible():
            return set()
        fn = om.MFnMesh(path)
        if self._faceVertices is None:
            counts, vertices = fn.getVertices()
            self._faceVertices, start = [], 0
            for count in counts:
                self._faceVertices.append(vertices[start:start + count])
                start += count
        points = fn.getPoints(om.MSpace.kWorld)
        camera = om.MFnCamera(view.getCamera())
        eye = camera.eyePoint(om.MSpace.kWorld)
        ortho = camera.isOrtho()
        forward = camera.viewDirection(om.MSpace.kWorld)
        found = set()
        for index, region in enumerate(self.regions):
            for face in region["faces"]:
                if face >= len(self._faceVertices):
                    continue
                corners = [points[v] for v in self._faceVertices[face]]
                if len(corners) < 3:
                    continue
                centre = om.MPoint()
                for corner in corners:
                    centre += om.MVector(corner)
                centre = centre / len(corners)
                normal = (om.MVector(corners[1] - corners[0]) ^
                          om.MVector(corners[-1] - corners[0]))
                toward = forward if ortho else om.MVector(centre - eye)
                if normal * toward >= 0:
                    continue
                x, y = view.worldToView(centre)[:2]
                if left <= x <= right and bottom <= y <= top:
                    found.add(index)
                    break
        return found

    def ControlOf(self, region):
        index = self.regions[region]["control"]
        return self.rig.controlPaths[index] \
            if index < len(self.rig.controlPaths) else None

    def HoverColor(self, region):
        palette = self.data.get("palette") or []
        color = palette[self.regions[region]["hilight"] % len(palette)] \
            if palette else self.regions[region]["color"]
        return _ScaleTo(color, _HOVER_VALUE)

    def Intersect(self, source, direction):
        """(distance, region) where the ray meets this layer's mesh, or
        None."""
        if not self.shape or not cmds.objExists(self.shape):
            return None
        try:
            path = om.MSelectionList().add(self.shape).getDagPath(0)
            hit = _RayHit(path, self.shape, source, direction, 1e7)
        except Exception:
            return None
        if not hit or hit[2] < 0:
            return None
        region = self.regionOf.get(hit[2])
        return (hit[1], region)

    def EnsureLocator(self):
        if self.locator and cmds.objExists(self.locator):
            return self.locator
        if not self.shape:
            return None
        state = cmds.undoInfo(query=True, stateWithoutFlush=True)
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            # Under the mesh it lights, reading the mesh's own points: it
            # draws in the mesh's space wherever the rig puts it, and goes
            # when the mesh does.
            meshTransform = cmds.listRelatives(self.shape, parent=True,
                                               fullPath=True)[0]
            name = "%s_touch" % self.data["name"].replace(" ", "_")
            transform = cmds.createNode("transform", name=name,
                                        parent=meshTransform, skipSelect=True)
            shape = cmds.createNode(_LOCATOR, name=name + "Shape",
                                    parent=transform, skipSelect=True)
            cmds.setAttr(transform + ".hiddenInOutliner", True)
            shape = cmds.ls(shape, long=True)[0]
            cmds.connectAttr(self.shape + ".outMesh", shape + ".touchMesh")
            cmds.setAttr(shape + ".alpha", Strength())
            cmds.setAttr(shape + ".leadColor",
                         *_ScaleTo(self.data["leadColor"], _LEAD_VALUE))
            cmds.setAttr(shape + ".selectedColor",
                         *_ScaleTo(self.data["selectedColor"],
                                   _SELECTED_VALUE))
            self.locator = shape
        finally:
            cmds.undoInfo(stateWithoutFlush=state)
        return self.locator

    def SetStrength(self, strength):
        if not self.locator or not cmds.objExists(self.locator):
            return
        state = cmds.undoInfo(query=True, stateWithoutFlush=True)
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            cmds.setAttr(self.locator + ".alpha", strength)
        finally:
            cmds.undoInfo(stateWithoutFlush=state)

    def Show(self, hover, lead, selected):
        """Lights `hover`, `lead` and `selected` (region indices)."""
        key = (hover, lead, tuple(selected))
        if key == self._shown:
            return
        self._shown = key
        locator = self.EnsureLocator()
        if locator is None:
            return
        state = cmds.undoInfo(query=True, stateWithoutFlush=True)
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            def faces(regions):
                return [f for r in regions if r is not None and r >= 0
                        for f in self.regions[r]["faces"]]
            if hover is not None and hover >= 0:
                cmds.setAttr(locator + ".hoverColor", *self.HoverColor(hover))
            cmds.setAttr(locator + ".hoverFaces", faces([hover]),
                         type="Int32Array")
            cmds.setAttr(locator + ".leadFaces", faces([lead]),
                         type="Int32Array")
            cmds.setAttr(locator + ".selectedFaces", faces(selected),
                         type="Int32Array")
        finally:
            cmds.undoInfo(stateWithoutFlush=state)

    def Clear(self):
        self.Show(None, None, ())

    def SetVisible(self, visible):
        if not self.locator or not cmds.objExists(self.locator):
            return
        transform = cmds.listRelatives(self.locator, parent=True,
                                       fullPath=True)[0]
        state = cmds.undoInfo(query=True, stateWithoutFlush=True)
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            cmds.setAttr(transform + ".visibility", bool(visible))
        finally:
            cmds.undoInfo(stateWithoutFlush=state)

    def Delete(self):
        if self.locator and cmds.objExists(self.locator):
            state = cmds.undoInfo(query=True, stateWithoutFlush=True)
            cmds.undoInfo(stateWithoutFlush=False)
            try:
                cmds.delete(cmds.listRelatives(self.locator, parent=True,
                                               fullPath=True))
            finally:
                cmds.undoInfo(stateWithoutFlush=state)
        self.locator = None


class _Controller(QtCore.QObject):
    """Watches every model panel's mouse, and the selection."""

    def __init__(self):
        super(_Controller, self).__init__()
        if not cmds.pluginInfo("rigExecMaya", query=True, loaded=True):
            cmds.loadPlugin("rigExecMaya")
        self.layers = []
        self.locked = []               # meshes made unselectable
        self.widgets = []
        self.hover = None              # (layer, region) under the cursor
        self.press = None              # (pos, modifiers, selection, view)
        self.suspended = False         # the rig is moving: nothing is lit
        self.occluders = None          # meshes that hide the skin
        self.manipulating = None       # where a manipulator drag began
        self.forwarding = False        # handing a press back to Maya
        self.handedBack = False        # until the release, the drag's Maya's
        self.box = None                # where a left drag began, for regions
        self.rigCallbacks = []         # told when a rig changes
        self.busySince = QtCore.QElapsedTimer()
        # Viewports come and go (a torn-off panel, a new layout): the
        # watched ones are checked every second.
        self.watchTimer = QtCore.QTimer()
        self.watchTimer.setInterval(1000)
        self.watchTimer.timeout.connect(self._CheckWatched)
        self.watchTimer.start()
        self.quiet = QtCore.QTimer()
        self.quiet.setSingleShot(True)
        self.quiet.setInterval(_QUIET_MS)
        self.quiet.timeout.connect(self._Quiet)
        self.callbacks = [
            om.MEventMessage.addEventCallback(
                "SelectionChanged", lambda *_: self._Relight()),
            om.MEventMessage.addEventCallback(
                "timeChanged", lambda *_: self._Suspend()),
            om.MDGMessage.addNodeAddedCallback(
                lambda *_: self._ForgetOccluders(), "mesh"),
            om.MDGMessage.addNodeRemovedCallback(
                lambda *_: self._ForgetOccluders(), "mesh"),
        ]
        for message in (om.MSceneMessage.kAfterOpen,
                        om.MSceneMessage.kAfterNew):
            self.callbacks.append(om.MSceneMessage.addCallback(
                message, lambda *_: QtCore.QTimer.singleShot(
                    0, self.Rebuild)))
        self.Rebuild()

    def Rebuild(self):
        for layer in self.layers:
            layer.Delete()
        self.layers = []
        for rig in _picker._Rigs():
            for data in rig.touch:
                layer = _Layer(rig, data)
                if layer.shape:
                    self.layers.append(layer)
        _Unlock(self.locked)
        self.locked = _Lock([m for rig in _picker._Rigs()
                             for m in _RigMeshes(rig)])
        # Any change to a rig (a channel dragged or typed, an attribute
        # driving a control, playback) stands TouchPose down until the rig
        # is still again.
        self._Unhook()
        for rig in _picker._Rigs():
            try:
                node = om.MSelectionList().add(rig.node).getDependNode(0)
                self.rigCallbacks.append(
                    om.MNodeMessage.addNodeDirtyPlugCallback(
                        node, lambda *_: self._Suspend()))
            except Exception:
                continue
        # Any leftover highlight from a deleted rig.
        for stale in cmds.ls(type=_LOCATOR) or []:
            if not any(l.locator and cmds.ls(l.locator)[0] ==
                       cmds.ls(stale)[0] for l in self.layers):
                state = cmds.undoInfo(query=True, stateWithoutFlush=True)
                cmds.undoInfo(stateWithoutFlush=False)
                try:
                    cmds.delete(cmds.listRelatives(stale, parent=True))
                finally:
                    cmds.undoInfo(stateWithoutFlush=state)
        self._Watch()
        self.occluders = None
        _framePlugs.clear()
        self.hover = None
        self._Relight()

    def _Unhook(self):
        for callback in self.rigCallbacks:
            try:
                om.MMessage.removeCallback(callback)
            except Exception:
                pass
        self.rigCallbacks = []

    def _Watch(self):
        for widget in self.widgets:
            try:
                widget.removeEventFilter(self)
            except RuntimeError:
                pass
        self.widgets = []
        for panel in cmds.getPanel(type="modelPanel") or []:
            try:
                view = omui.M3dView.getM3dViewFromModelPanel(panel)
                widget = shiboken.wrapInstance(int(view.widget()),
                                               QtWidgets.QWidget)
            except Exception:
                continue
            widget.setMouseTracking(True)
            widget.installEventFilter(self)
            self.widgets.append(widget)

    def _ViewWidgets(self):
        out = []
        for panel in cmds.getPanel(type="modelPanel") or []:
            try:
                view = omui.M3dView.getM3dViewFromModelPanel(panel)
                out.append(int(view.widget()))
            except Exception:
                continue
        return out

    def _CheckWatched(self):
        try:
            watched = set()
            for widget in self.widgets:
                try:
                    watched.add(shiboken.getCppPointer(widget)[0])
                except Exception:
                    continue
            if set(self._ViewWidgets()) != watched:
                self._Watch()
        except Exception:
            pass

    def Close(self):
        self.quiet.stop()
        self.watchTimer.stop()
        for callback in self.callbacks:
            om.MMessage.removeCallback(callback)
        self.callbacks = []
        self._Unhook()
        for widget in self.widgets:
            try:
                widget.removeEventFilter(self)
            except RuntimeError:
                pass
        self.widgets = []
        for layer in self.layers:
            layer.Delete()
        self.layers = []
        _Unlock(self.locked)
        self.locked = []

    # -- picking ----------------------------------------------------------

    def HitAt(self, view, x, y):
        """(layer, region) of the nearest touch region at port coordinates
        (x, y), or None (the skin there is unpainted, or nothing is
        hit)."""
        if not self.layers:
            return None
        source, direction = om.MPoint(), om.MVector()
        view.viewToWorld(int(x), int(y), source, direction)
        best = None
        for layer in self.layers:
            found = layer.Intersect(source, direction)
            if found is not None and (best is None or found[0] < best[0]):
                best = (found[0], layer, found[1])
        if best is None or best[2] is None:
            return None
        if self._Hidden(source, direction, best[0]):
            return None
        return best[1], best[2]

    def _Occluders(self):
        """Every mesh with no touch regions: hair, teeth, props."""
        if self.occluders is None:
            touched = set(cmds.ls([l.shape for l in self.layers],
                                  long=True) or [])
            self.occluders = []
            for shape in cmds.ls(type="mesh", long=True,
                                 noIntermediate=True) or []:
                if shape in touched:
                    continue
                try:
                    self.occluders.append(
                        om.MSelectionList().add(shape).getDagPath(0))
                except Exception:
                    continue
        return self.occluders

    def _ForgetOccluders(self):
        self.occluders = None
        _framePlugs.clear()

    def _Hidden(self, source, direction, distance):
        """True when a shown mesh with no touch regions is in front of the
        skin, `distance` along the ray."""
        for path in self._Occluders():
            try:
                if not path.isValid() or not path.isVisible():
                    continue
                box = om.MFnMesh(path).boundingBox
                box.transformUsing(path.inclusiveMatrix())
                if not _RayMeetsBox(source, direction, box, distance):
                    continue
                hit = _RayHit(path, path.fullPathName(), source, direction,
                              distance)
            except Exception:
                continue
            if hit and hit[2] >= 0 and hit[1] < distance:
                return True
        return False

    def _ViewFor(self, widget):
        for panel in cmds.getPanel(type="modelPanel") or []:
            try:
                view = omui.M3dView.getM3dViewFromModelPanel(panel)
                if shiboken.getCppPointer(widget)[0] == int(view.widget()):
                    return view
            except Exception:
                continue
        return None

    @staticmethod
    def _PortPoint(view, widget, pos):
        scaleX = view.portWidth() / float(max(widget.width(), 1))
        scaleY = view.portHeight() / float(max(widget.height(), 1))
        return pos.x() * scaleX, (widget.height() - pos.y()) * scaleY

    # -- events -----------------------------------------------------------

    def eventFilter(self, watched, event):
        if self.forwarding:
            return False
        from . import markingmenu
        if markingmenu.IsOpen():
            # The marking menu is up: its clicks are its own.
            self._SetHover(None)
            return False
        try:
            return bool(self._Event(watched, event))
        except Exception as error:
            om.MGlobal.displayWarning("RigExec TouchPose: %s" % error)
            self.press = None
            return False

    def _Event(self, widget, event):
        """True when TouchPose takes the event (a click on a lit region)."""
        kind = event.type()
        if kind == QtCore.QEvent.MouseButtonRelease and self.suspended:
            # A manipulator drag just ended: back at once, not after the
            # quiet wait, so the next hover lights straight away.
            QtCore.QTimer.singleShot(0, self._Quiet)
        if kind == QtCore.QEvent.MouseButtonRelease and \
                event.button() == QtCore.Qt.LeftButton and \
                self.box is not None:
            box, self.box = self.box, None
            end = QtCore.QPointF(event.pos())
            if _Same(box[0], widget) and \
                    (end - box[1]).manhattanLength() > _DRAG_SLOP:
                # After Maya's own marquee has picked its curves.
                QtCore.QTimer.singleShot(
                    0, lambda: self._BoxSelect(box, end))
        if kind == QtCore.QEvent.MouseMove and not event.buttons():
            self.manipulating = None
            self.press = None
            self.handedBack = False
            self._Hover(widget, event.pos())
            return False
        if self.handedBack:
            # Maya presses again as the drag it was handed begins.
            if kind == QtCore.QEvent.MouseButtonRelease:
                self.handedBack = False
            return False
        if kind == QtCore.QEvent.MouseMove:
            if self.press is not None:
                if (QtCore.QPointF(event.pos()) -
                        self.press[0]).manhattanLength() > _DRAG_SLOP:
                    # A drag after all: the viewport gets its press now.
                    self._Forward(self.press)
                    self.press = None
                    self.handedBack = True
                    return False
                return True
            # A drag that began on the manipulator moves the rig.
            if self.manipulating is not None and \
                    (QtCore.QPointF(event.pos()) -
                     self.manipulating).manhattanLength() > _DRAG_SLOP:
                self._Suspend()
            return False
        if kind == QtCore.QEvent.Leave:
            self._SetHover(None)
            return False
        if kind in (QtCore.QEvent.MouseButtonPress,
                    QtCore.QEvent.MouseButtonDblClick):
            self.manipulating = None
            self.press = None
            self.box = None
            alt = bool(event.modifiers() & QtCore.Qt.AltModifier)
            if not alt and self._OnManipulator(widget, event):
                self.manipulating = QtCore.QPointF(event.pos())
                return False
            if not alt and event.button() == QtCore.Qt.LeftButton and \
                    kind == QtCore.QEvent.MouseButtonPress and \
                    not _HoverPickerOver():
                self.box = (widget, QtCore.QPointF(event.pos()),
                            event.modifiers(),
                            cmds.ls(selection=True, long=True) or [])
            if alt or event.button() != QtCore.Qt.LeftButton or \
                    self._Busy() or _HoverPickerOver():
                return False
            view = self._ViewFor(widget)
            if view is None:
                return False
            x, y = self._PortPoint(view, widget, event.pos())
            hit = self.HitAt(view, x, y)
            if hit is None:
                return False
            self.press = (QtCore.QPointF(event.pos()), event.modifiers(),
                          hit, widget, event.globalPosition()
                          if hasattr(event, "globalPosition")
                          else QtCore.QPointF(event.globalPos()))
            self._SetHover(hit)
            return True
        if kind == QtCore.QEvent.MouseButtonRelease and \
                event.button() == QtCore.Qt.LeftButton and \
                self.press is not None:
            press, self.press = self.press, None
            self._Select(press[2], press[1])
            return True
        return False

    def _Forward(self, press):
        """Hands a press TouchPose held back to the viewport, where it
        began, so the drag that follows is Maya's."""
        pos, modifiers, _hit, widget, globalPos = press
        event = QtGui.QMouseEvent(
            QtCore.QEvent.MouseButtonPress, pos, globalPos,
            QtCore.Qt.LeftButton, QtCore.Qt.LeftButton, modifiers)
        self.forwarding = True
        try:
            QtWidgets.QApplication.sendEvent(widget, event)
        finally:
            self.forwarding = False

    def _BoxSelect(self, box, end):
        """Adds the controls whose regions a marquee covered to what Maya's
        marquee picked, by Maya's marquee rules."""
        widget, start, modifiers, before = box
        view = self._ViewFor(widget)
        if view is None:
            return
        x0, y0 = self._PortPoint(view, widget, start)
        x1, y1 = self._PortPoint(view, widget, end)
        covered = set()
        for layer in self.layers:
            for region in layer.RegionsIn(view, min(x0, x1), min(y0, y1),
                                          max(x0, x1), max(y0, y1)):
                node = layer.rig.controls.get(layer.ControlOf(region))
                if node:
                    covered.add(node)
        if not covered:
            return
        after = cmds.ls(selection=True, long=True) or []
        shift = bool(modifiers & QtCore.Qt.ShiftModifier)
        ctrl = bool(modifiers & QtCore.Qt.ControlModifier)
        if ctrl and not shift:
            chosen = [n for n in after if n not in covered]
        elif shift and not ctrl:
            # Toggled, except what Maya's marquee has already toggled.
            changed = set(before) ^ set(after)
            chosen = list(after)
            for node in sorted(covered - changed):
                if node in chosen:
                    chosen.remove(node)
                else:
                    chosen.append(node)
        else:
            chosen = after + sorted(covered - set(after))
        if chosen == after:
            return
        if chosen:
            cmds.select(chosen, replace=True)
        else:
            cmds.select(clear=True)

    def _Busy(self):
        return self.suspended or bool(cmds.play(query=True, state=True))

    def _Hover(self, widget, pos):
        if self._Busy() or _HoverPickerOver():
            self._SetHover(None)
            return
        view = self._ViewFor(widget)
        if view is None:
            return
        x, y = self._PortPoint(view, widget, pos)
        if _OverManipulator(view, x, y):
            self._SetHover(None)
            return
        self._SetHover(self.HitAt(view, x, y))

    # -- standing down while the rig moves --------------------------------

    def _OnManipulator(self, widget, event):
        """Whether a press starts a manipulator drag: the left button on
        the manipulator, or the middle button anywhere while it is
        shown."""
        view = self._ViewFor(widget)
        if view is None or _HoverPickerOver():
            return False
        if event.button() == QtCore.Qt.MiddleButton:
            return _ManipulatorShown() is not None
        if event.button() == QtCore.Qt.LeftButton:
            return _OverManipulator(
                view, *self._PortPoint(view, widget, event.pos()))
        return False

    def _Suspend(self):
        """Puts every highlight out until the rig has been still for a
        moment. Called from Maya's callbacks too, so the scene is changed
        only once control is back with Qt."""
        self.quiet.start()
        self.busySince.start()
        if not self.suspended:
            self.suspended = True
            QtCore.QTimer.singleShot(0, self._ShowAll)

    def _Quiet(self):
        held = QtWidgets.QApplication.mouseButtons() != QtCore.Qt.NoButton
        if cmds.play(query=True, state=True) or (
                held and self.busySince.isValid() and
                self.busySince.elapsed() < _HELD_LIMIT_MS):
            self.quiet.start()
            return
        self.quiet.stop()
        self.suspended = False
        self._ShowAll()
        QtCore.QTimer.singleShot(0, self._Warm)

    def _Warm(self):
        """Rebuilds the ray-test grids of meshes the rig has just moved,
        right away, so the first hover after a pose is as quick as any."""
        if self.suspended:
            return
        source, direction = om.MPoint(0, 0, 1e6), om.MVector(0, 0, -1)
        for layer in self.layers:
            layer.Intersect(source, direction)
        for path in self._Occluders():
            try:
                if path.isValid() and path.isVisible():
                    _RayHit(path, path.fullPathName(), source, direction, 1.0)
            except Exception:
                continue

    def _ShowAll(self):
        if self.suspended:
            self.hover = None
        for layer in self.layers:
            layer.SetVisible(not self.suspended)
        if not self.suspended:
            self._Relight()

    def _SetHover(self, hover):
        if hover == self.hover:
            return
        self.hover = hover
        self._Relight()

    def _Select(self, hit, modifiers):
        """Selects the control of `hit`'s region by the picker's rules:
        Ctrl removes, Shift toggles, a plain click replaces."""
        layer, region = hit
        node = layer.rig.controls.get(layer.ControlOf(region))
        if node is None:
            return
        before = cmds.ls(selection=True, long=True) or []
        if modifiers & QtCore.Qt.ControlModifier:
            chosen = [n for n in before if n != node]
        elif modifiers & QtCore.Qt.ShiftModifier:
            chosen = [n for n in before if n != node] if node in before \
                else before + [node]
        else:
            chosen = [node]
        if chosen:
            cmds.select(chosen, replace=True)
        else:
            cmds.select(clear=True)

    def _Relight(self):
        """Lights hover and selection on every layer."""
        selected = cmds.ls(selection=True, long=True) or []
        for layer in self.layers:
            byNode = {node: path for path, node in layer.rig.controls.items()}
            lead, others = None, []
            for node in selected:
                regions = layer.byControl.get(byNode.get(node), [])
                if regions:
                    if lead is not None:
                        others.append(lead)
                    lead = regions[0]
                    others.extend(regions[1:])
            hover = self.hover[1] if self.hover and \
                self.hover[0] is layer else None
            layer.Show(hover, lead, others)


def _Same(a, b):
    """Whether two Qt objects are the same one."""
    try:
        return shiboken.getCppPointer(a)[0] == shiboken.getCppPointer(b)[0]
    except Exception:
        return False


def _RigMeshes(rig):
    """Every shown mesh `rig` drives."""
    meshes = []
    plugs = cmds.listConnections(rig.node, source=False, destination=True,
                                 connections=True, plugs=True) or []
    for plug in plugs[0::2]:
        attribute = plug.split(".", 1)[-1]
        if attribute.startswith(("outMeshFrame[", "outMesh[")):
            shape = _MeshFed(plug)
            if shape and shape not in meshes:
                meshes.append(shape)
    return meshes


def _Lock(shapes):
    """Shows `shapes` as reference, so the viewport cannot select them.
    Returns the ones locked. A mesh whose display is set by a display
    layer is left alone."""
    locked = []
    state = cmds.undoInfo(query=True, stateWithoutFlush=True)
    cmds.undoInfo(stateWithoutFlush=False)
    try:
        for shape in shapes:
            try:
                if not cmds.attributeQuery(_LOCK, node=shape, exists=True):
                    if cmds.listConnections(shape + ".drawOverride",
                                            source=True, destination=False):
                        continue
                    before = "%d %d" % (
                        cmds.getAttr(shape + ".overrideEnabled"),
                        cmds.getAttr(shape + ".overrideDisplayType"))
                    cmds.addAttr(shape, longName=_LOCK, dataType="string")
                    cmds.setAttr(shape + "." + _LOCK, before, type="string")
                cmds.setAttr(shape + ".overrideEnabled", True)
                cmds.setAttr(shape + ".overrideDisplayType", 2)
                locked.append(shape)
            except Exception:
                continue
    finally:
        cmds.undoInfo(stateWithoutFlush=state)
    return locked


def _Unlock(shapes):
    """Gives `shapes` back the display they had before _Lock."""
    state = cmds.undoInfo(query=True, stateWithoutFlush=True)
    cmds.undoInfo(stateWithoutFlush=False)
    try:
        for shape in shapes:
            try:
                if not cmds.objExists(shape) or not cmds.attributeQuery(
                        _LOCK, node=shape, exists=True):
                    continue
                enabled, kind = (int(v) for v in
                                 cmds.getAttr(shape + "." + _LOCK).split())
                cmds.setAttr(shape + ".overrideDisplayType", kind)
                cmds.setAttr(shape + ".overrideEnabled", enabled)
                cmds.deleteAttr(shape + "." + _LOCK)
            except Exception:
                continue
    finally:
        cmds.undoInfo(stateWithoutFlush=state)


def _MeshFed(plug):
    """The shown mesh `plug` feeds, following any nodes in between (a
    scene saved before materials were set up ahead of the rig has a
    groupParts node there), or None."""
    seen = set()
    plugs = [plug]
    while plugs:
        found = cmds.listConnections(plugs.pop(0), source=False,
                                     destination=True, plugs=True,
                                     shapes=True) or []
        for target in found:
            node = target.split(".")[0]
            if node in seen:
                continue
            seen.add(node)
            if cmds.nodeType(node) == "mesh":
                path = cmds.ls(node, long=True, noIntermediate=True)
                if path:
                    return path[0]
                continue
            for output in ("outputGeometry", "outMesh", "output"):
                if cmds.attributeQuery(output, node=node, exists=True):
                    plugs.append(node + "." + output)
    return None


# The ray-test grid's cells along each side: quick to rebuild after a pose
# (Maya's automatic grid takes several times as long on the body).
_GRID_CELLS = 20

# Each mesh's rig frame plug ("" for a mesh the rig does not move), and the
# frame its ray-test grid was built for. Maya keeps a mesh's grid when a
# deformer moves its points, so the grid is rebuilt here whenever the rig
# has moved the mesh since: otherwise TouchPose would hit the skin where it
# was, not where it is drawn.
_framePlugs = {}
_gridFrames = {}


def _FramePlug(shape):
    if shape not in _framePlugs:
        plug = ""
        deformers = cmds.listConnections(shape + ".inMesh", source=True,
                                         destination=False) or []
        if deformers and cmds.nodeType(deformers[0]) == "rigExecDeform":
            # The rig's pose number: the mesh moves only when it changes.
            sources = cmds.listConnections(
                deformers[0] + ".rigFrame", source=True, destination=False)
            plug = sources[0] + ".outFrame" if sources else ""
        _framePlugs[shape] = plug
    return _framePlugs[shape]


def _RayHit(path, shape, source, direction, distance):
    """MFnMesh.closestIntersection on the mesh as it is drawn now."""
    fn = om.MFnMesh(path)
    point = om.MFloatPoint(source)
    ray = om.MFloatVector(direction)
    plug = _FramePlug(shape)
    if not plug:
        # No way to tell when it moves: tested face by face.
        return fn.closestIntersection(point, ray, om.MSpace.kWorld,
                                      distance, False)
    frame = cmds.getAttr(plug)
    if _gridFrames.get(shape) != frame:
        fn.freeCachedIntersectionAccelerator()
        _gridFrames[shape] = frame
    return fn.closestIntersection(
        point, ray, om.MSpace.kWorld, distance, False,
        accelParams=fn.uniformGridParams(_GRID_CELLS, _GRID_CELLS,
                                         _GRID_CELLS))


def _RayMeetsBox(source, direction, box, length):
    """Whether the ray from `source` meets the world box within
    `length`."""
    near, far = 0.0, length
    low, high = box.min, box.max
    for axis in range(3):
        d = direction[axis]
        if abs(d) < 1e-12:
            if source[axis] < low[axis] or source[axis] > high[axis]:
                return False
            continue
        t0 = (low[axis] - source[axis]) / d
        t1 = (high[axis] - source[axis]) / d
        if t0 > t1:
            t0, t1 = t1, t0
        near, far = max(near, t0), min(far, t1)
        if near > far:
            return False
    return True


_MANIP_TOOLS = {
    "manipMove": (cmds.manipMoveContext, "Move"),
    "manipRotate": (cmds.manipRotateContext, "Rotate"),
    "manipScale": (cmds.manipScaleContext, "Scale"),
}


def _ManipulatorShown():
    """Where the move, rotate or scale manipulator is (world space), or
    None when none is shown."""
    if not cmds.ls(selection=True):
        return None
    try:
        tool = _MANIP_TOOLS.get(cmds.contextInfo(cmds.currentCtx(),
                                                 c=True))
        if tool is None:
            return None
        return tool[0](tool[1], query=True, position=True) or None
    except Exception:
        return None


def _OverManipulator(view, x, y):
    """Whether the cursor at port coordinates (x, y) is on a handle of the
    move, rotate or scale manipulator. Maya lights the handle there and
    the click is Maya's, so nothing new is lit on the skin."""
    if not cmds.ls(selection=True) or not _ManipulatorsShown(view):
        return False
    try:
        kind = cmds.contextInfo(cmds.currentCtx(), c=True)
        tool = _MANIP_TOOLS.get(kind)
        if tool is None:
            return False
        position = tool[0](tool[1], query=True, position=True)
        if not position:
            return False
        centre = om.MPoint(*position)
        cx, cy, _clipped = view.worldToView(centre)
        scale = (cmds.manipOptions(query=True, scale=True) or [1.0])[0]
        scale *= cmds.mayaDpiSetting(query=True, realScaleValue=True)
        slop = _MANIP_SLOP * cmds.mayaDpiSetting(query=True,
                                                 realScaleValue=True)
        if kind == "manipRotate":
            # The rotate sphere: its rings, and inside them a free turn.
            reach = _MANIP_PIXELS * scale
            return (x - cx) ** 2 + (y - cy) ** 2 <= reach * reach
        # Move and scale: three arrows, the plane squares between them,
        # and the centre.
        if math.hypot(x - cx, y - cy) <= slop * 1.5:
            return True
        camera = om.MFnCamera(view.getCamera())
        rx, ry, _clipped = view.worldToView(
            centre + camera.rightDirection(om.MSpace.kWorld))
        perUnit = math.hypot(rx - cx, ry - cy)
        if perUnit <= 1e-9:
            return False
        length = _MANIP_ARROW * scale / perUnit
        ends = []
        for axis in _ManipAxes(tool[0](tool[1], query=True, mode=True)):
            ex, ey, _clipped = view.worldToView(centre + axis * length)
            ends.append((ex - cx, ey - cy))
    except Exception:
        return False
    px, py = x - cx, y - cy
    for ex, ey in ends:
        if _SegmentDistance(px, py, ex, ey) <= slop:
            return True
    for i in range(3):
        for j in range(i + 1, 3):
            (ax, ay), (bx, by) = ends[i], ends[j]
            det = ax * by - ay * bx
            if abs(det) < 1e-6:
                continue
            u = (px * by - py * bx) / det
            v = (ax * py - ay * px) / det
            if 0.25 <= abs(u) <= 0.6 and 0.25 <= abs(v) <= 0.6:
                return True
    return False


def _ManipulatorsShown(view):
    """Whether `view`'s panel shows manipulators (Show > Manipulators)."""
    try:
        widget = int(view.widget())
        for panel in cmds.getPanel(type="modelPanel") or []:
            if int(omui.M3dView.getM3dViewFromModelPanel(panel).widget()) \
                    == widget:
                return bool(cmds.modelEditor(panel, query=True,
                                             manipulators=True))
    except Exception:
        pass
    return True


def _ManipAxes(mode):
    """The manipulator's axes (world space) for a move or scale mode:
    world, the selection's parent space, or its own axes."""
    if mode == 2:
        return [om.MVector(1, 0, 0), om.MVector(0, 1, 0),
                om.MVector(0, 0, 1)]
    node = (cmds.ls(selection=True, long=True, transforms=True) or
            [None])[-1]
    if node is None:
        return [om.MVector(1, 0, 0), om.MVector(0, 1, 0),
                om.MVector(0, 0, 1)]
    if mode == 1:
        matrix = om.MMatrix(cmds.getAttr(node + ".offsetParentMatrix")) * \
            om.MMatrix(cmds.getAttr(node + ".parentMatrix"))
    else:
        matrix = om.MMatrix(cmds.getAttr(node + ".worldMatrix"))
    return [om.MVector(matrix[4 * i], matrix[4 * i + 1],
                       matrix[4 * i + 2]).normal() for i in range(3)]


def _SegmentDistance(px, py, ex, ey):
    """Distance from (px, py) to the segment from the origin to (ex, ey)."""
    length = ex * ex + ey * ey
    t = 0.0 if length <= 1e-9 else \
        max(0.0, min(1.0, (px * ex + py * ey) / length))
    return math.hypot(px - t * ex, py - t * ey)


def _HoverPickerOver():
    """Whether the hover picker's buttons are under the cursor; they sit
    over the skin and win."""
    from . import hover
    instance = hover._instance
    return instance is not None and instance._hover is not None
