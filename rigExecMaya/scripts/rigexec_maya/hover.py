"""
The hover picker: the rig's picker tabs drawn right in the viewport, as
floating buttons in screen space.

    import rigexec_maya.hover
    rigexec_maya.hover.Toggle()        # in the viewport that has focus

Each tab is a small round handle with its buttons hanging off it. There is
no window around them: anywhere that isn't a button, a handle or the
opacity knob stays the viewport's, so a click there selects, box-selects,
drags a manipulator or moves the camera as usual.

  * handle, drag          move the tab; its buttons follow
  * handle, Shift+drag    scale the tab
  * handle, double-click  collapse the tab to its handle, or expand it
  * button, click         select, with the docked picker's modifiers
                          (Shift toggles, Ctrl removes, Alt mirrors);
                          switches match, as in the docked picker
  * button, drag          box-select within the tab
  * middle-drag a tab     fade it
  * knob (bottom right)   drag to fade every tab; double-click for full

A port of usdRig's hover picker (plugin/rigExecUsdview/pickerHoverUI.py),
whose layout and gesture rules (pickerHoverModel.py) it loads from the
usdRig checkout. The layout is saved per user, in the same place usdview
keeps it, and never in the scene.

Maya's viewport is a native window that a translucent Qt widget can't sit
over, so the picker is painted into an image and drawn by Viewport 2.0
(the rigExecHoverPicker locator and the rigExecHoverImage command).
"""

import ctypes
import importlib.util
import math
import os

import maya.api.OpenMaya as om
import maya.api.OpenMayaUI as omui
import maya.cmds as cmds

try:
    from PySide6 import QtCore, QtGui, QtWidgets
    import shiboken6 as shiboken
except ImportError:
    from PySide2 import QtCore, QtGui, QtWidgets
    import shiboken2 as shiboken

from . import export as _export
from . import picker as _picker

# Alt or Meta on a press is the camera's (tumble, track, dolly), except on
# a picker button, where Alt is the mirror modifier.
_CAMERA_MODIFIERS = QtCore.Qt.AltModifier | QtCore.Qt.MetaModifier
# A press that travels further than this is a drag, not a click.
_DRAG_SLOP = 3.0

_SETTINGS_ORG = "RigExec"
_SETTINGS_APP = "HoverPicker"
_SETTINGS_KEY = "layout"
_LOCATOR = "rigExecHoverPicker"

_HANDLE_FILL = QtGui.QColor(225, 225, 225)
_HANDLE_COLLAPSED_FILL = QtGui.QColor(70, 70, 70)
_HANDLE_HOVER_FILL = QtGui.QColor(169, 211, 255)
_HANDLE_EDGE = QtGui.QColor(20, 20, 20)
_LABEL_COLOR = QtGui.QColor(235, 235, 235)
_LABEL_SHADOW = QtGui.QColor(0, 0, 0, 170)

# The overall-opacity knob sits bottom right, lifted clear of Maya's
# heads-up display there (usdRig places it for usdview's).
_KNOB_HUD_CLEARANCE = 150.0

_hoverModel = None


def _HoverModel():
    """usdRig's hover layout model (pure Python), from the usdRig
    checkout."""
    global _hoverModel
    if _hoverModel is None:
        path = os.path.join(_export.RigExecRoot(), "plugin",
                            "rigExecUsdview", "pickerHoverModel.py")
        if not os.path.exists(path):
            raise RuntimeError("usdRig's hover picker model was not found at "
                               "%s" % path)
        spec = importlib.util.spec_from_file_location(
            "rigexec_maya_pickerHoverModel", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _hoverModel = module
    return _hoverModel


def _LoadLayout():
    settings = QtCore.QSettings(_SETTINGS_ORG, _SETTINGS_APP)
    return _HoverModel().Layout.FromJson(settings.value(_SETTINGS_KEY, ""))


def _SaveLayout(layout):
    settings = QtCore.QSettings(_SETTINGS_ORG, _SETTINGS_APP)
    settings.setValue(_SETTINGS_KEY, layout.ToJson())


def _EventPoint(event):
    point = event.position() if hasattr(event, "position") else event.pos()
    return (point.x(), point.y())


def _Shown(picker, panelId, modes=None):
    """The buttons drawn over the viewport: the live ones the docked picker
    shows, without its backdrops, which would only hide the scene."""
    return [b for b in picker.visible(panelId, modes=modes)
            if not b.decoration]


class _Tab(object):
    """One picker panel as the hover picker shows it."""

    def __init__(self, rig, picker, panel):
        self.rig = rig
        self.picker = picker
        self.panel = panel
        self.key = _HoverModel().TabKey(getattr(picker, "name", ""),
                                        panel.id)
        self.label = panel.label or panel.id.rsplit("/", 1)[-1]
        # The buttons hang off the box around everything the tab can show,
        # in either half of every IK/FK switch.
        shown = _Shown(picker, panel.id)
        if shown:
            self.ox = min(b.x for b in shown)
            self.oy = min(b.y for b in shown)
            self.cw = max(b.x + b.w for b in shown) - self.ox
            self.ch = max(b.y + b.h for b in shown) - self.oy
        else:
            self.ox = self.oy = 0.0
            self.cw = self.ch = 1.0


def _FocusedModelPanel():
    panel = cmds.getPanel(withFocus=True)
    if panel and cmds.getPanel(typeOf=panel) == "modelPanel":
        return panel
    visible = [p for p in cmds.getPanel(visiblePanels=True) or []
               if cmds.getPanel(typeOf=p) == "modelPanel"]
    return visible[0] if visible else None


def _EnsureLocator():
    """The scene's rigExecHoverPicker locator, which draws the image."""
    found = cmds.ls(type=_LOCATOR)
    if found:
        return found[0]
    state = cmds.undoInfo(query=True, stateWithoutFlush=True)
    cmds.undoInfo(stateWithoutFlush=False)
    try:
        shape = cmds.createNode(_LOCATOR, name=_LOCATOR + "Shape",
                                skipSelect=True)
        transform = cmds.listRelatives(shape, parent=True)[0]
        transform = cmds.rename(transform, _LOCATOR)
        for attr in ("translate", "rotate", "scale"):
            cmds.setAttr("%s.%s" % (transform, attr), lock=True)
        cmds.setAttr(transform + ".hiddenInOutliner", True)
    finally:
        cmds.undoInfo(stateWithoutFlush=state)
    return cmds.ls(type=_LOCATOR)[0]


class HoverPicker(QtCore.QObject):
    """Places, paints and routes the hover picker in one model panel."""

    def __init__(self, panel):
        super(HoverPicker, self).__init__()
        self.panel = panel
        self._model = _HoverModel()
        self._layout = _LoadLayout()
        self._view = omui.M3dView.getM3dViewFromModelPanel(panel)
        self._widget = shiboken.wrapInstance(int(self._view.widget()),
                                             QtWidgets.QWidget)
        self._rigs = []
        self._tabs = []
        self._selected = set()
        self._modes = {}
        # (tab key, button id) under the cursor, (tab key, None) for a
        # handle, ("knob", None) for the knob, or None.
        self._hover = None
        self._gesture = None
        self._cursorSet = False
        self._buffer = None
        self._callbacks = []
        self._widget.setAttribute(QtCore.Qt.WA_Hover, True)
        self._widget.installEventFilter(self)
        self._callbacks.append(om.MEventMessage.addEventCallback(
            "SelectionChanged", lambda *_: self._OnSelectionChanged()))
        for message in (om.MSceneMessage.kAfterOpen,
                        om.MSceneMessage.kAfterNew):
            self._callbacks.append(om.MSceneMessage.addCallback(
                message, lambda *_: QtCore.QTimer.singleShot(
                    0, self.Rebuild)))
        # A dial or switch keyed over time changes which half of each limb
        # shows; read it a few times a second.
        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self._OnTick)
        self._timer.start(250)
        self.Rebuild()

    # -- state ------------------------------------------------------------

    def Rebuild(self):
        try:
            self._rigs = [r for r in _picker._Rigs() if r.pickers]
        except Exception as error:
            om.MGlobal.displayWarning("RigExec hover picker: %s" % error)
            self._rigs = []
        tabs = []
        for rig in self._rigs:
            for picker in rig.pickers:
                for panel in picker.panels:
                    if any(b.parent == panel.id and b.live
                           and not b.decoration for b in picker.buttons):
                        tabs.append(_Tab(rig, picker, panel))
        self._tabs = tabs
        height = float(self._widget.height())
        fits = {t.key: self._model.FitScale(t.ch, height) for t in tabs}
        before = len(self._layout.tabs)
        self._layout.Ensure([t.key for t in tabs], fits)
        if len(self._layout.tabs) != before:
            _SaveLayout(self._layout)
        if self._rigs:
            _EnsureLocator()
        for rig in self._rigs:
            rig.SyncToggles()
        self._modes = self._Modes()
        self._OnSelectionChanged()

    def _Modes(self):
        modes = {}
        for rig in self._rigs:
            modes.update(rig.Modes()[0])
        return modes

    def _OnTick(self):
        if not self._rigs or self._gesture is not None:
            return
        try:
            modes = self._Modes()
        except Exception:
            # A rig deleted under us; the next scene event rebuilds.
            return
        if modes != self._modes:
            self._modes = modes
            self.Render()

    def _OnSelectionChanged(self):
        selected = set()
        for rig in self._rigs:
            try:
                selected |= rig.SelectedPaths()
            except Exception:
                pass
        self._selected = selected
        self.Render()

    def Close(self):
        self._timer.stop()
        for callback in self._callbacks:
            try:
                om.MMessage.removeCallback(callback)
            except Exception:
                pass
        self._callbacks = []
        try:
            self._widget.removeEventFilter(self)
            if self._cursorSet:
                self._widget.unsetCursor()
        except RuntimeError:
            pass
        if cmds.pluginInfo("rigExecMaya", query=True, loaded=True):
            cmds.rigExecHoverImage("-panel", self.panel, "-clear")
        self._Refresh()

    # -- hit testing ------------------------------------------------------

    def Hit(self, point):
        """What is under a viewport point: ("knob", None, None),
        ("handle", tab, None), ("button", tab, button), or None. Topmost
        first: the knob, then the tabs in reverse paint order."""
        cx, cy = self._KnobCentre(self._widget.width(),
                                  self._widget.height())
        if math.hypot(point[0] - cx, point[1] - cy) <= (
                self._model.KNOB_RADIUS + self._model.HANDLE_SLOP):
            return ("knob", None, None)
        for tab in reversed(self._tabs):
            layout = self._layout.Tab(tab.key)
            if layout is None:
                continue
            if layout.HitsHandle(point):
                return ("handle", tab, None)
            if layout.collapsed:
                continue
            x, y = layout.ToPanel((tab.ox, tab.oy), point)
            live = [b for b in tab.picker.hits(tab.panel.id, x, y,
                                               self._modes) if b.live]
            if live:
                return ("button", tab, live[0])
        return None

    # -- painting ---------------------------------------------------------

    def Render(self):
        """Paints the picker and hands it to the viewport."""
        if not cmds.pluginInfo("rigExecMaya", query=True, loaded=True):
            return
        width, height = self._view.portWidth(), self._view.portHeight()
        if width <= 0 or height <= 0:
            return
        image = QtGui.QImage(width, height, QtGui.QImage.Format_RGBA8888)
        image.fill(QtCore.Qt.transparent)
        painter = QtGui.QPainter(image)
        painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
        # Viewport pixels can be finer than the widget's (display scaling);
        # everything is laid out in widget coordinates, where the mouse is.
        logical = max(self._widget.width(), 1)
        painter.scale(width / float(logical), width / float(logical))
        try:
            self.Paint(painter, self._widget.width(), self._widget.height())
        finally:
            painter.end()
        self._buffer = (ctypes.c_ubyte * image.sizeInBytes()) \
            .from_buffer_copy(bytes(image.constBits()))
        cmds.rigExecHoverImage("-panel", self.panel, "-size", width, height,
                               "-address",
                               str(ctypes.addressof(self._buffer)))
        self._Refresh()

    def _Refresh(self):
        try:
            self._view.refresh(False, True)
        except Exception:
            pass

    def Paint(self, painter, width, height):
        gesture = self._gesture or {}
        for tab in self._tabs:
            layout = self._layout.Tab(tab.key)
            if layout is None:
                continue
            painter.save()
            painter.setOpacity(self._layout.TabOpacity(tab.key))
            if not layout.collapsed:
                painter.save()
                cx, cy = layout.ButtonsCorner()
                painter.translate(cx, cy)
                painter.scale(layout.scale, layout.scale)
                painter.translate(-tab.ox, -tab.oy)
                hover = (self._hover[1] if self._hover
                         and self._hover[0] == tab.key else None)
                for button in _Shown(tab.picker, tab.panel.id, self._modes):
                    _picker.DrawButton(painter, button, hover,
                                       self._selected)
                painter.restore()
            handleHovered = self._hover == (tab.key, None)
            active = gesture.get("tab") is tab
            self._PaintHandle(painter, layout, handleHovered or active)
            if handleHovered or active:
                self._PaintLabel(painter, layout, tab,
                                 gesture if active else None)
            painter.restore()
        band = gesture.get("band")
        if band is not None:
            x0, y0, x1, y1 = band
            painter.setBrush(QtGui.QBrush(QtGui.QColor(120, 200, 255, 40)))
            pen = QtGui.QPen(QtGui.QColor(150, 215, 255))
            pen.setWidthF(1.0)
            painter.setPen(pen)
            painter.drawRect(QtCore.QRectF(min(x0, x1), min(y0, y1),
                                           abs(x1 - x0), abs(y1 - y0)))
        self._PaintKnob(painter, width, height,
                        self._hover == ("knob", None)
                        or gesture.get("kind") == "knob")

    def _PaintHandle(self, painter, layout, hovered):
        r = self._model.HANDLE_RADIUS
        fill = (_HANDLE_HOVER_FILL if hovered else
                _HANDLE_COLLAPSED_FILL if layout.collapsed else
                _HANDLE_FILL)
        painter.setBrush(QtGui.QBrush(fill))
        pen = QtGui.QPen(_HANDLE_EDGE)
        pen.setWidthF(1.5)
        painter.setPen(pen)
        painter.drawEllipse(QtCore.QPointF(layout.x, layout.y), r, r)
        if layout.collapsed:
            # A collapsed tab reads as a ring with a dot in it.
            painter.setBrush(QtGui.QBrush(_HANDLE_FILL))
            painter.setPen(QtCore.Qt.NoPen)
            painter.drawEllipse(QtCore.QPointF(layout.x, layout.y),
                                r * 0.35, r * 0.35)

    def _PaintLabel(self, painter, layout, tab, gesture):
        text = tab.label
        if len(set(t.picker for t in self._tabs)) > 1:
            text = "%s %s" % (getattr(tab.picker, "name", ""), text)
        kind = (gesture or {}).get("kind")
        if kind == "scale":
            text += "  %d%%" % round(layout.scale * 100.0)
        elif kind == "fade":
            text += "  opacity %d%%" % round(layout.opacity * 100.0)
        self._PaintText(painter, layout.x + self._model.HANDLE_RADIUS + 6.0,
                        layout.y, text)

    def _PaintText(self, painter, x, y, text, alignRight=False):
        font = QtGui.QFont()
        font.setPixelSize(11)
        painter.setFont(font)
        metrics = QtGui.QFontMetricsF(font)
        width = metrics.horizontalAdvance(text)
        left = x - width if alignRight else x
        base = y + metrics.ascent() * 0.5 - 1.0
        painter.setPen(_LABEL_SHADOW)
        painter.drawText(QtCore.QPointF(left + 1.0, base + 1.0), text)
        painter.setPen(_LABEL_COLOR)
        painter.drawText(QtCore.QPointF(left, base), text)

    def _KnobCentre(self, width, height):
        return (width - self._model.KNOB_INSET - self._model.KNOB_RADIUS,
                height - _KNOB_HUD_CLEARANCE - self._model.KNOB_RADIUS)

    def _PaintKnob(self, painter, width, height, hovered):
        cx, cy = self._KnobCentre(width, height)
        r = self._model.KNOB_RADIUS
        centre = QtCore.QPointF(cx, cy)
        painter.setOpacity(1.0)
        painter.setBrush(QtGui.QBrush(QtGui.QColor(40, 40, 40, 200)))
        pen = QtGui.QPen(_HANDLE_HOVER_FILL if hovered else _HANDLE_FILL)
        pen.setWidthF(1.5)
        painter.setPen(pen)
        painter.drawEllipse(centre, r, r)
        # The overall opacity as a filled wedge, full at 100%.
        painter.setBrush(QtGui.QBrush(_HANDLE_HOVER_FILL if hovered
                                      else _HANDLE_FILL))
        painter.setPen(QtCore.Qt.NoPen)
        box = QtCore.QRectF(cx - r + 2.0, cy - r + 2.0,
                            2.0 * r - 4.0, 2.0 * r - 4.0)
        span = int(round(-360.0 * 16.0 * self._layout.opacity))
        painter.drawPie(box, 90 * 16, span)
        if hovered:
            self._PaintText(painter, cx - r - 6.0, cy,
                            "picker opacity %d%%"
                            % round(self._layout.opacity * 100.0),
                            alignRight=True)

    # -- the mouse --------------------------------------------------------

    def eventFilter(self, watched, event):
        try:
            return self._Event(event)
        except Exception as error:
            # An exception escaping a filter on the viewport would break
            # the viewport's own mouse handling.
            om.MGlobal.displayWarning("RigExec hover picker: %s" % error)
            self._gesture = None
            return False

    def _Event(self, event):
        kind = event.type()
        if kind in (QtCore.QEvent.Resize, QtCore.QEvent.Show):
            QtCore.QTimer.singleShot(0, self.Render)
            return False
        if kind in (QtCore.QEvent.MouseButtonPress,
                    QtCore.QEvent.MouseButtonDblClick):
            return self._Press(event,
                               kind == QtCore.QEvent.MouseButtonDblClick)
        if kind == QtCore.QEvent.MouseMove:
            return self._Move(event)
        if kind == QtCore.QEvent.MouseButtonRelease:
            return self._Release(event)
        if kind == QtCore.QEvent.HoverMove:
            if self._gesture is not None:
                return True
            return self._HoverAt(_EventPoint(event))
        if kind in (QtCore.QEvent.Leave, QtCore.QEvent.HoverLeave):
            self._SetHover(None)
        return False

    def _HoverAt(self, point):
        hit = self.Hit(point)
        if hit is None:
            self._SetHover(None)
            return False
        what, tab, button = hit
        self._SetHover(("knob", None) if what == "knob" else
                       (tab.key, None) if what == "handle" else
                       (tab.key, button.id))
        return True

    def _Press(self, event, double):
        if self._gesture is not None:
            return True
        point = _EventPoint(event)
        hit = self.Hit(point)
        if hit is None:
            return False
        what, tab, button = hit
        mouse = event.button()
        if event.modifiers() & _CAMERA_MODIFIERS and what != "button":
            return False
        if event.modifiers() & _CAMERA_MODIFIERS and \
                mouse != QtCore.Qt.LeftButton:
            return False
        if mouse == QtCore.Qt.LeftButton and double:
            if what == "handle":
                layout = self._layout.Tab(tab.key)
                layout.collapsed = not layout.collapsed
                _SaveLayout(self._layout)
                self.Render()
                return True
            if what == "knob":
                self._layout.opacity = 1.0
                _SaveLayout(self._layout)
                self.Render()
                return True
        if mouse == QtCore.Qt.LeftButton:
            if what == "knob":
                self._gesture = {"kind": "knob", "start": point,
                                 "opacity": self._layout.opacity}
            elif what == "handle":
                layout = self._layout.Tab(tab.key)
                if event.modifiers() & QtCore.Qt.ShiftModifier:
                    self._gesture = {"kind": "scale", "tab": tab,
                                     "start": point, "scale": layout.scale}
                else:
                    self._gesture = {"kind": "move", "tab": tab,
                                     "last": point}
            else:
                layout = self._layout.Tab(tab.key)
                modifiers = event.modifiers()
                if modifiers & QtCore.Qt.ControlModifier:
                    mode = "remove"
                elif modifiers & QtCore.Qt.ShiftModifier:
                    mode = "toggle"
                else:
                    mode = "replace"
                self._gesture = {
                    "kind": "pick", "tab": tab, "start": point,
                    "panelStart": layout.ToPanel((tab.ox, tab.oy), point),
                    "mode": mode,
                    "mirror": bool(modifiers & QtCore.Qt.AltModifier),
                    "band": None}
            self.Render()
            return True
        if mouse == QtCore.Qt.MiddleButton:
            if what == "knob":
                self._gesture = {"kind": "knob", "start": point,
                                 "opacity": self._layout.opacity,
                                 "button": QtCore.Qt.MiddleButton}
            else:
                layout = self._layout.Tab(tab.key)
                self._gesture = {"kind": "fade", "tab": tab, "start": point,
                                 "opacity": layout.opacity,
                                 "button": QtCore.Qt.MiddleButton}
            self.Render()
            return True
        # Right-click and anything else stay the viewport's.
        return False

    def _Move(self, event):
        point = _EventPoint(event)
        gesture = self._gesture
        if gesture is None:
            if event.buttons() != QtCore.Qt.NoButton:
                return False
            return self._HoverAt(point)
        kind = gesture["kind"]
        if kind == "knob":
            self._layout.Faded(gesture["opacity"],
                               point[0] - gesture["start"][0])
        elif kind == "move":
            last = gesture["last"]
            self._layout.Tab(gesture["tab"].key).Moved(point[0] - last[0],
                                                       point[1] - last[1])
            gesture["last"] = point
        elif kind == "scale":
            start = gesture["start"]
            self._layout.Tab(gesture["tab"].key).Scaled(
                gesture["scale"], point[0] - start[0], point[1] - start[1])
        elif kind == "fade":
            self._layout.Tab(gesture["tab"].key).Faded(
                gesture["opacity"], point[0] - gesture["start"][0])
        elif kind == "pick":
            start = gesture["start"]
            if gesture["band"] is not None or math.hypot(
                    point[0] - start[0], point[1] - start[1]) > _DRAG_SLOP:
                gesture["band"] = (start[0], start[1], point[0], point[1])
        self.Render()
        return True

    def _Release(self, event):
        gesture = self._gesture
        if gesture is None:
            return False
        if event.button() != gesture.get("button", QtCore.Qt.LeftButton):
            return True
        self._gesture = None
        if gesture["kind"] == "pick":
            self._FinishPick(gesture)
        else:
            _SaveLayout(self._layout)
        self.Render()
        return True

    def _FinishPick(self, gesture):
        tab = gesture["tab"]
        layout = self._layout.Tab(tab.key)
        band = gesture["band"]
        if band is not None:
            x0, y0 = layout.ToPanel((tab.ox, tab.oy), band[:2])
            x1, y1 = layout.ToPanel((tab.ox, tab.oy), band[2:])
            buttons = tab.picker.within(tab.panel.id, x0, y0, x1, y1,
                                        self._modes)
            if not buttons:
                return
        else:
            x, y = gesture["panelStart"]
            buttons = [b for b in tab.picker.hits(tab.panel.id, x, y,
                                                  self._modes)
                       if b.live][:1]
        if gesture["mirror"]:
            buttons = self._WithMirrors(tab, buttons)
        tab.rig.Pick(buttons, gesture["mode"], om.MGlobal.displayInfo,
                     self._widget)
        tab.rig.SyncToggles()
        self._modes = self._Modes()

    def _WithMirrors(self, tab, buttons):
        """The buttons plus their mirrored partners."""
        byName = {b.id: b for b in tab.picker.visible(tab.panel.id)}
        out = []
        for button in buttons:
            partner = byName.get((button.mirror or "").rsplit("/", 1)[-1])
            for candidate in (button, partner):
                if candidate is not None and candidate not in out:
                    out.append(candidate)
        return out

    def _SetHover(self, hover):
        if hover == self._hover:
            return
        self._hover = hover
        if hover is not None and not self._cursorSet:
            self._widget.setCursor(QtCore.Qt.PointingHandCursor)
            self._cursorSet = True
        elif hover is None and self._cursorSet:
            self._widget.unsetCursor()
            self._cursorSet = False
        self.Render()


_instance = None


def IsShown():
    return _instance is not None


def Show(panel=None):
    """Shows the hover picker in `panel` (default: the focused viewport)."""
    global _instance
    if not cmds.pluginInfo("rigExecMaya", query=True, loaded=True):
        cmds.loadPlugin("rigExecMaya")
    panel = panel or _FocusedModelPanel()
    if panel is None:
        raise RuntimeError("no viewport to show the hover picker in")
    if _instance is not None:
        if _instance.panel == panel:
            return _instance
        Hide()
    _instance = HoverPicker(panel)
    layout = _instance._layout
    layout.enabled = True
    _SaveLayout(layout)
    return _instance


def Hide():
    global _instance
    if _instance is not None:
        _instance._layout.enabled = False
        _SaveLayout(_instance._layout)
        _instance.Close()
        _instance = None


def Toggle(panel=None):
    """Shows the hover picker in the focused viewport, or hides it."""
    if _instance is not None and (panel is None or
                                  _instance.panel == panel):
        Hide()
        return None
    return Show(panel)
