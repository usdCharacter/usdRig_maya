"""
The control picker: the rig's own picker panels, drawn in a dockable Maya
window, selecting and switching the rig's controls.

    import rigexec_maya.picker
    rigexec_maya.picker.Show()

A rig carries its picker as USD data (RigExecPicker prims). Loading the rig
keeps that picker on its rigExecRig node, and this window draws it. The
rules -- which buttons are live, what a click means, which half of an IK/FK
limb shows -- come from usdRig's picker model, so the picker behaves as it
does in usdview.

Click a button to select its controls; drag a marquee to select several.
Shift toggles, Ctrl removes and Alt brings the mirrored side along. Switch
buttons set a limb's IK/FK or a control's space; Zero Ctrls resets the
selected controls (or all of them), and Ctrl Vis shows or hides the
controls.
"""

import importlib.util
import json
import os

import maya.api.OpenMaya as om
import maya.cmds as cmds
from maya.app.general.mayaMixin import MayaQWidgetDockableMixin

try:
    from PySide6 import QtCore, QtGui, QtWidgets
except ImportError:
    from PySide2 import QtCore, QtGui, QtWidgets

from . import export as _export
from . import matching as _matching
from . import scene as _scene

_WINDOW = "rigExecPicker"
_DIM = 0.22

_pickerModel = None


def _PickerModel():
    """usdRig's picker model (pure Python, no USD or Qt), loaded from the
    usdRig checkout."""
    global _pickerModel
    if _pickerModel is None:
        path = os.path.join(_export.RigExecRoot(), "plugin",
                            "rigExecUsdview", "pickerModel.py")
        if not os.path.exists(path):
            raise RuntimeError("usdRig's picker model was not found at %s"
                               % path)
        spec = importlib.util.spec_from_file_location(
            "rigexec_maya_pickerModel", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _pickerModel = module
    return _pickerModel


_window = None


def Show():
    """Opens the picker for the rigs in the scene, as a dockable window."""
    global _window
    if _window is not None:
        _window._RemoveCallbacks()
        _window = None
    if cmds.workspaceControl(_WINDOW + "WorkspaceControl", exists=True):
        cmds.deleteUI(_WINDOW + "WorkspaceControl")
    _window = PickerWindow()
    _window.show(dockable=True, floating=True, width=460, height=720)
    return _window


# -- the rig in Maya ----------------------------------------------------------

class Rig(object):
    """One rigExecRig node's picker, IK/FK limbs and space switches,
    resolved to Maya nodes."""

    def __init__(self, node):
        self.node = node
        self.pickers = []
        self.controls = {}          # rig prim path -> Maya control
        self.controlInfo = {}       # rig prim path -> rotation order, sign
        self.limbs = []
        self.spaces = {}
        if not cmds.attributeQuery("rigData", node=node, exists=True):
            return
        data = json.loads(cmds.getAttr(node + ".rigData") or "{}")
        self.limbs = data.get("limbs") or []
        self.spaces = data.get("spaces") or {}
        for index, info in enumerate(data.get("controls") or []):
            path = info["path"]
            self.controlInfo[path] = info
            nodes = cmds.listConnections(
                "%s.outControlMatrix[%d]" % (node, index),
                source=False, destination=True) or []
            if nodes:
                self.controls[path] = cmds.ls(nodes[0], long=True)[0]
        model = _PickerModel()
        live = set(self.controls)
        for document in data.get("pickers") or []:
            picker = model.Picker(document, live)
            picker.name = document.get("name") or "Picker"
            # A switch is live only when Maya animates its attribute.
            for button in picker.buttons:
                if button.attr_target:
                    button.live = self.Plug(button.attr_target["path"],
                                            button.attr_target["attr"]) \
                        is not None
            self.pickers.append(picker)

    def PlugForPath(self, attrPath):
        """The Maya plug for `<prim path>.<attribute>`, or None."""
        primPath, _, attr = attrPath.rpartition(".")
        return self.Plug(primPath, attr) if primPath else None

    def Plug(self, primPath, attr):
        """The Maya plug for a rig attribute, or None when Maya does not
        animate it."""
        control = self.controls.get(primPath)
        if control is None or not attr.startswith("avars:"):
            return None
        plug = "%s.%s" % (control, _scene.ChannelPlug(attr))
        return plug if cmds.objExists(plug) else None

    @staticmethod
    def Get(plug):
        return cmds.getAttr(plug)

    def DialPlug(self, dial):
        """The plug for a dial, given as `<prim path>.<attribute>`."""
        return self.PlugForPath(dial)

    def Nodes(self, buttons):
        out = []
        for button in buttons:
            for target in button.targets:
                node = self.controls.get(target)
                if node is not None and node not in out:
                    out.append(node)
        return out

    def ControlsGroup(self):
        """The group holding the rig's controls."""
        for node in self.controls.values():
            parent = cmds.listRelatives(node, parent=True, fullPath=True)
            while parent:
                if parent[0].rsplit("|", 1)[-1] == "controls":
                    return parent[0]
                parent = cmds.listRelatives(parent[0], parent=True,
                                            fullPath=True)
        return None

    def ChannelSources(self):
        """(control plug, channel index) for every channel the rig reads."""
        out = []
        for array in ("inLinear", "inAngle"):
            pairs = cmds.listConnections(
                "%s.%s" % (self.node, array), connections=True, plugs=True,
                source=True, destination=False) or []
            for destination, source in zip(pairs[0::2], pairs[1::2]):
                index = int(destination.rsplit("[", 1)[1].rstrip("]"))
                out.append((source, index))
        return out

    # -- the picker's state and actions, shared by every picker view -------

    def Buttons(self):
        return [b for picker in self.pickers for b in picker.buttons]

    def SelectedPaths(self):
        """The rig paths of the selected controls."""
        selected = set(cmds.ls(selection=True, long=True) or [])
        return set(path for path, node in self.controls.items()
                   if node in selected)

    def Modes(self):
        """{dial: "ik" or "fk"} for each limb in one mode, and every
        switch's value read back from Maya. True as the second value when
        a switch's value changed."""
        modes = {}
        changed = False
        for button in self.Buttons():
            if button.dial and button.dial not in modes:
                plug = self.DialPlug(button.dial)
                value = cmds.getAttr(plug) if plug else None
                if value is not None:
                    if value >= 0.999:
                        modes[button.dial] = "ik"
                    elif value <= 0.001:
                        modes[button.dial] = "fk"
            target = button.attr_target
            if target and button.live:
                plug = self.Plug(target["path"], target["attr"])
                label = button.label_for(cmds.getAttr(plug))
                if label and label != button.value:
                    button.value = label
                    changed = True
        return modes, changed

    def SyncToggles(self):
        group = self.ControlsGroup()
        shown = bool(group) and bool(cmds.getAttr(group + ".visibility"))
        for button in self.Buttons():
            if button.command == "ctrl_vis":
                button.checked = shown

    def Pick(self, buttons, mode, status=None, parent=None):
        """What a click or a box does: selects the buttons' controls
        ("replace", "toggle" or "remove"), or, on a plain click of one
        command or switch button, runs it."""
        status = status or (lambda text: None)
        # A command or switch acts only on a plain click of one button, so
        # a marquee across a limb never flips it or zeroes the rig.
        if len(buttons) == 1 and mode == "replace":
            button = buttons[0]
            if button.command == "zero_ctrls":
                self.ZeroControls(status)
                return
            if button.command == "ctrl_vis":
                self.ToggleControls()
                return
            if button.attr_target:
                self.Switch(button, status, parent)
                return
        nodes = self.Nodes(buttons)
        if mode == "replace":
            if nodes:
                cmds.select(nodes, replace=True)
            else:
                cmds.select(clear=True)
        elif mode == "remove":
            if nodes:
                cmds.select(nodes, deselect=True)
        elif nodes:
            cmds.select(nodes, toggle=True)

    def Switch(self, button, status=None, parent=None):
        """Sets a switch: IK/FK toggles, a space switch offers a menu."""
        status = status or (lambda text: None)
        target = button.attr_target
        plug = self.Plug(target["path"], target["attr"])
        if plug is None:
            return
        labels = target.get("enum") or []
        if len(labels) > 2:
            menu = QtWidgets.QMenu(parent)
            for name in labels:
                action = menu.addAction(str(name))
                action.setCheckable(True)
                action.setChecked(name == button.value)
            picked = menu.exec(QtGui.QCursor.pos())
            if picked is None:
                return
            label = picked.text()
            value = float(labels.index(label))
            if target.get("invert"):
                value = float(len(labels) - 1) - value
        else:
            value, label = button.next_value(cmds.getAttr(plug))
            if value is None:
                return
        self.ApplySwitch(button, value, label, status)

    def ApplySwitch(self, button, value, label, status=None):
        """Sets a switch to `value` as one undoable step, matched so
        nothing moves."""
        status = status or (lambda text: None)
        target = button.attr_target
        plug = self.Plug(target["path"], target["attr"])
        # Matched: the limb or control keeps its place, and only what
        # drives it changes. If matching fails the switch still happens.
        plan, note = {plug: value}, ""
        try:
            solved = _matching.Plan(
                self, target["path"] + "." + target["attr"], value)
            plan = {}
            for attrPath, solvedValue in solved.items():
                solvedPlug = self.PlugForPath(attrPath)
                if solvedPlug is not None:
                    plan[solvedPlug] = solvedValue
            plan[plug] = value
            note = ", matched"
        except Exception as error:
            om.MGlobal.displayWarning(
                "RigExec: switched without matching (%s)" % error)
            note = ", not matched"
        cmds.undoInfo(openChunk=True, chunkName="Switch " + label)
        try:
            autoKey = cmds.autoKeyframe(query=True, state=True)
            for matchedPlug, matchedValue in plan.items():
                if cmds.getAttr(matchedPlug, lock=True):
                    continue
                cmds.setAttr(matchedPlug, matchedValue)
                if autoKey:
                    cmds.setKeyframe(matchedPlug)
        finally:
            cmds.undoInfo(closeChunk=True)
        button.value = label
        self.Modes()
        status("%s: %s%s" % (plug.rsplit("|", 1)[-1], label,
                                            note))

    def ZeroControls(self, status=None):
        """Returns the selected controls (or all of them) to the rig's
        default pose."""
        status = status or (lambda text: None)
        selected = set(cmds.ls(selection=True, long=True) or [])
        controls = set(self.controls.values())
        chosen = (selected & controls) or controls
        defaults = cmds.getAttr(self.node + ".channelDefault") or []
        cmds.undoInfo(openChunk=True, chunkName="Zero controls")
        reset = 0
        try:
            for plug, index in self.ChannelSources():
                node = cmds.ls(plug.split(".", 1)[0], long=True)
                if not node or node[0] not in chosen \
                        or index >= len(defaults):
                    continue
                if cmds.getAttr(plug, lock=True):
                    continue
                cmds.setAttr(plug, defaults[index])
                reset += 1
        finally:
            cmds.undoInfo(closeChunk=True)
        self.Modes()
        status("Zeroed %d channels on %s." % (
            reset, "the selected controls" if selected & controls
            else "every control"))

    def ToggleControls(self):
        group = self.ControlsGroup()
        if group is None:
            return
        cmds.setAttr(group + ".visibility",
                     not cmds.getAttr(group + ".visibility"))
        self.SyncToggles()


def _Rigs():
    return [Rig(node) for node in cmds.ls(type="rigExecRig") or []]


# -- drawing ------------------------------------------------------------------

def _PathFor(button):
    """A button's outline in its own local box."""
    path = QtGui.QPainterPath()
    w, h = button.w, button.h
    shape = button.shape
    if shape in ("circle", "ellipse"):
        path.addEllipse(QtCore.QRectF(0, 0, w, h))
    elif shape == "polygon" and button.polygon:
        _Polygon(path, button.polygon, button.roundness)
    elif shape == "bezier" and button.bezier:
        # Each knot is (anchor, out tangent, in tangent).
        points = button.bezier
        path.moveTo(*points[0][0])
        for i in range(len(points)):
            a = points[i]
            b = points[(i + 1) % len(points)]
            c1 = a[1] if len(a) > 1 else a[0]
            c2 = b[2] if len(b) > 2 else b[0]
            path.cubicTo(c1[0], c1[1], c2[0], c2[1], b[0][0], b[0][1])
        path.closeSubpath()
    elif shape == "trapezoid":
        # A positive slope insets the top edge, a negative one the bottom.
        ls, rs = button.left_slope, button.right_slope
        if button.direction in ("top", "bottom"):
            pts = [(max(ls, 0.0), 0.0), (w - max(rs, 0.0), 0.0),
                   (w - max(-rs, 0.0), h), (max(-ls, 0.0), h)]
        else:
            pts = [(0.0, max(ls, 0.0)), (w, max(rs, 0.0)),
                   (w, h - max(-rs, 0.0)), (0.0, h - max(-ls, 0.0))]
        _Polygon(path, pts, max(button.roundness, 0.0))
    elif shape == "hexagon":
        q = w * 0.25
        path.moveTo(q, 0)
        path.lineTo(w - q, 0)
        path.lineTo(w, h * 0.5)
        path.lineTo(w - q, h)
        path.lineTo(q, h)
        path.lineTo(0, h * 0.5)
        path.closeSubpath()
    elif shape == "triangle":
        if button.direction == "right":
            path.moveTo(0, 0)
            path.lineTo(w, h * 0.5)
            path.lineTo(0, h)
        elif button.direction == "left":
            path.moveTo(w, 0)
            path.lineTo(0, h * 0.5)
            path.lineTo(w, h)
        else:
            path.moveTo(w * 0.5, 0)
            path.lineTo(w, h)
            path.lineTo(0, h)
        path.closeSubpath()
    else:
        r = max(button.roundness, 0.0)
        if shape == "roundedRectangle":
            r = max(r, min(w, h) * 0.35)
        if r > 0:
            path.addRoundedRect(QtCore.QRectF(0, 0, w, h), r, r)
        else:
            path.addRect(QtCore.QRectF(0, 0, w, h))
    return path


def _Polygon(path, points, roundness):
    n = len(points)
    if roundness <= 0 or n < 3:
        path.moveTo(*points[0])
        for p in points[1:]:
            path.lineTo(*p)
        path.closeSubpath()
        return

    def trim(a, b, r):
        dx, dy = b[0] - a[0], b[1] - a[1]
        length = (dx * dx + dy * dy) ** 0.5 or 1.0
        t = min(r, length * 0.5) / length
        return (a[0] + dx * t, a[1] + dy * t)

    path.moveTo(*trim(points[0], points[1], roundness))
    for i in range(n):
        cur = points[(i + 1) % n]
        nxt = points[(i + 2) % n]
        path.lineTo(*trim(cur, points[i], roundness))
        after = trim(cur, nxt, roundness)
        path.quadTo(cur[0], cur[1], after[0], after[1])
    path.closeSubpath()


def DrawButton(painter, button, hover=None, selected=()):
    """One picker button, in panel coordinates: lit when `hover` is
    its id, outlined when `selected` holds all its targets."""
    painter.save()
    painter.translate(button.x, button.y)
    if button.rotation:
        painter.translate(button.w * 0.5, button.h * 0.5)
        painter.rotate(button.rotation)
        painter.translate(-button.w * 0.5, -button.h * 0.5)

    fill = QtGui.QColor(*button.fill)
    if not button.decoration and not button.live:
        fill.setAlpha(int(fill.alpha() * _DIM))
    if button.id == hover and button.live:
        fill = fill.lighter(125)
    painter.setBrush(QtGui.QBrush(fill))
    if button.targets and set(selected).issuperset(button.targets):
        pen = QtGui.QPen(QtGui.QColor(255, 255, 255))
        pen.setWidthF(2.0)
    else:
        pen = QtGui.QPen(QtGui.QColor(*button.stroke))
        pen.setWidthF(max(button.stroke_width, 0.5))
    painter.setPen(pen)
    painter.drawPath(_PathFor(button))

    if button.checkbox:
        side = min(button.h - 4.0, 11.0)
        top = (button.h - side) * 0.5
        left = 3.0 if button.h_align != "right" else \
            button.w - side - 3.0
        painter.setBrush(QtGui.QBrush(QtGui.QColor(58, 58, 58)))
        painter.setPen(QtGui.QPen(QtGui.QColor(18, 18, 18)))
        painter.drawRect(QtCore.QRectF(left, top, side, side))
        if button.checked:
            tick = QtGui.QPen(QtGui.QColor(120, 200, 255))
            tick.setWidthF(1.8)
            painter.setPen(tick)
            painter.drawPolyline([
                QtCore.QPointF(left + side * 0.20, top + side * 0.52),
                QtCore.QPointF(left + side * 0.44, top + side * 0.76),
                QtCore.QPointF(left + side * 0.82, top + side * 0.24)])

    if button.text or button.value:
        _DrawText(painter, button)
    painter.restore()

def _DrawText(painter, button):
    """The label, and a switch's current value with a menu caret."""
    font = QtGui.QFont()
    font.setPixelSize(max(int(round(button.font_size)), 5))
    font.setBold(button.bold)
    box = QtCore.QRectF(2, 0, button.w - 4, button.h)
    both = " ".join(x for x in (button.text, button.value) if x)
    for _ in range(6):
        if (QtGui.QFontMetricsF(font).horizontalAdvance(both)
                <= box.width() - (14.0 if button.value else 0.0)
                or font.pixelSize() <= 5):
            break
        font.setPixelSize(font.pixelSize() - 1)
    painter.setFont(font)
    align = {"left": QtCore.Qt.AlignLeft,
             "right": QtCore.Qt.AlignRight}.get(
                 button.h_align, QtCore.Qt.AlignHCenter)
    if button.checkbox:
        box.setLeft(box.left() + min(button.h - 4.0, 11.0) + 5.0)
        align = QtCore.Qt.AlignLeft

    labelBox, valueBox, valueAlign, caretX = box, None, None, 0.0
    if button.value:
        metrics = QtGui.QFontMetricsF(font)
        want = min(metrics.horizontalAdvance(button.value) + 14.0,
                   box.width())
        if not button.text:
            want = box.width()
            labelBox = None
        if align == QtCore.Qt.AlignRight:
            valueBox = QtCore.QRectF(box.left() + 12, box.top(),
                                     want - 12, box.height())
            valueAlign = QtCore.Qt.AlignLeft
            caretX = box.left() + 5
            if labelBox is not None:
                labelBox = QtCore.QRectF(box)
                labelBox.setLeft(box.left() + want)
        else:
            valueBox = QtCore.QRectF(box.right() - want, box.top(),
                                     want - 12, box.height())
            valueAlign = QtCore.Qt.AlignRight
            caretX = box.right() - 7
            if labelBox is not None:
                labelBox = QtCore.QRectF(box)
                labelBox.setRight(box.right() - want)

    if button.text and labelBox is not None:
        painter.setPen(QtGui.QColor(*button.text_color))
        painter.drawText(labelBox, align | QtCore.Qt.AlignVCenter,
                         button.text)
    if button.value:
        painter.setPen(QtGui.QColor(*button.value_color))
        painter.drawText(valueBox, valueAlign | QtCore.Qt.AlignVCenter,
                         button.value)
        mid = box.center().y()
        caret = QtGui.QPainterPath()
        caret.moveTo(caretX - 3.2, mid - 1.6)
        caret.lineTo(caretX + 3.2, mid - 1.6)
        caret.lineTo(caretX, mid + 2.2)
        caret.closeSubpath()
        painter.setBrush(QtGui.QBrush(QtGui.QColor(*button.value_color)))
        painter.setPen(QtCore.Qt.NoPen)
        painter.drawPath(caret)


class PanelView(QtWidgets.QWidget):
    """One picker panel, drawn and clickable."""

    # (buttons, mode): mode is "replace", "toggle" or "remove".
    picked = QtCore.Signal(object, str)

    def __init__(self, picker, panel, parent=None):
        super(PanelView, self).__init__(parent)
        self._picker = picker
        self._panel = panel
        self._modes = {}
        self._selected = set()
        self._hover = None
        self._press = None
        self._band = None
        self._mode = "replace"
        self._mirror = False
        self.setMouseTracking(True)
        (self._ox, self._oy), (self._cw, self._ch) = \
            _PickerModel().content_box(picker, panel)
        self.setMinimumSize(int(self._cw * 0.5), int(self._ch * 0.5))

    def sizeHint(self):
        return QtCore.QSize(int(self._cw), int(self._ch))

    def _Scale(self):
        return min(self.width() / self._cw, self.height() / self._ch) or 1.0

    def _PanelPos(self, event):
        scale = self._Scale()
        pos = event.position() if hasattr(event, "position") else event.pos()
        return pos.x() / scale + self._ox, pos.y() / scale + self._oy

    # -- painting --------------------------------------------------------

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
        painter.fillRect(self.rect(), QtGui.QColor(42, 42, 42))
        scale = self._Scale()
        painter.scale(scale, scale)
        painter.translate(-self._ox, -self._oy)
        painter.setBrush(QtGui.QBrush(QtGui.QColor(*self._panel.fill)))
        painter.setPen(QtGui.QPen(QtGui.QColor(0, 0, 0, 200)))
        painter.drawRect(QtCore.QRectF(self._ox, self._oy, self._cw,
                                       self._ch))
        for button in self._picker.visible(self._panel.id,
                                           modes=self._modes):
            DrawButton(painter, button, self._hover, self._selected)
        if self._band is not None:
            x0, y0, x1, y1 = self._band
            painter.setBrush(QtGui.QBrush(QtGui.QColor(120, 200, 255, 40)))
            painter.setPen(QtGui.QPen(QtGui.QColor(150, 215, 255)))
            painter.drawRect(QtCore.QRectF(min(x0, x1), min(y0, y1),
                                           abs(x1 - x0), abs(y1 - y0)))
        painter.end()

    # -- mouse -----------------------------------------------------------

    def _WithMirrors(self, buttons):
        """The buttons plus their mirrored partners, when Alt is held."""
        if not self._mirror:
            return buttons
        byName = dict((b.id, b) for b in self._picker.visible(self._panel.id))
        out = []
        for button in buttons:
            partner = byName.get((button.mirror or "").rsplit("/", 1)[-1])
            for candidate in (button, partner):
                if candidate is not None and candidate not in out:
                    out.append(candidate)
        return out

    def mousePressEvent(self, event):
        if event.button() != QtCore.Qt.LeftButton:
            return
        modifiers = event.modifiers()
        self._press = self._PanelPos(event)
        self._band = None
        if modifiers & QtCore.Qt.ControlModifier:
            self._mode = "remove"
        elif modifiers & QtCore.Qt.ShiftModifier:
            self._mode = "toggle"
        else:
            self._mode = "replace"
        self._mirror = bool(modifiers & QtCore.Qt.AltModifier)

    def mouseReleaseEvent(self, event):
        if event.button() != QtCore.Qt.LeftButton or self._press is None:
            return
        start, self._press = self._press, None
        band, self._band = self._band, None
        self.update()
        if band is not None:
            hits = self._picker.within(self._panel.id, *band,
                                       modes=self._modes)
            if hits:
                self.picked.emit(self._WithMirrors(hits), self._mode)
            return
        hits = self._picker.hits(self._panel.id, start[0], start[1],
                                 self._modes)
        self.picked.emit(self._WithMirrors(hits[:1]), self._mode)

    def mouseMoveEvent(self, event):
        if self._press is not None:
            x, y = self._PanelPos(event)
            if abs(x - self._press[0]) > 3.0 or abs(y - self._press[1]) > 3.0:
                self._band = (self._press[0], self._press[1], x, y)
                self.update()
                return
        x, y = self._PanelPos(event)
        hits = self._picker.hits(self._panel.id, x, y, self._modes)
        hover = hits[0].id if hits else None
        if hover != self._hover:
            self._hover = hover
            if hits:
                self.setToolTip("\n".join(t.rsplit("/", 1)[-1]
                                          for t in hits[0].targets)
                                or hits[0].text)
                self.setCursor(QtCore.Qt.PointingHandCursor)
            else:
                self.setToolTip("")
                self.unsetCursor()
            self.update()

    def leaveEvent(self, event):
        self._hover = None
        self.unsetCursor()
        self.update()

    def SetModes(self, modes):
        if modes != self._modes:
            self._modes = dict(modes)
            self.update()

    def SetSelected(self, paths):
        paths = set(paths)
        if paths != self._selected:
            self._selected = paths
            self.update()


# -- the window ---------------------------------------------------------------

class PickerWindow(MayaQWidgetDockableMixin, QtWidgets.QWidget):
    """Every rig's picker in the scene, one tab per panel."""

    def __init__(self, parent=None):
        super(PickerWindow, self).__init__(parent=parent)
        self.setObjectName(_WINDOW)
        self.setWindowTitle("RigExec Picker")
        self._rigs = []
        self._rig = None
        self._views = []
        self._callbacks = []
        self._modes = {}

        layout = QtWidgets.QVBoxLayout(self)
        top = QtWidgets.QHBoxLayout()
        self._rigBox = QtWidgets.QComboBox()
        self._rigBox.currentIndexChanged.connect(self._OnRigChanged)
        top.addWidget(self._rigBox, 1)
        refresh = QtWidgets.QPushButton("Refresh")
        refresh.clicked.connect(self.Reload)
        top.addWidget(refresh)
        layout.addLayout(top)
        self._tabs = QtWidgets.QTabWidget()
        layout.addWidget(self._tabs, 1)
        self._status = QtWidgets.QLabel("")
        self._status.setWordWrap(True)
        layout.addWidget(self._status)

        for keys, slot in (("Ctrl+A", self._SelectAll),
                           ("F", self._Frame)):
            action = QtGui.QAction(self) if hasattr(QtGui, "QAction") \
                else QtWidgets.QAction(self)
            action.setShortcut(QtGui.QKeySequence(keys))
            action.setShortcutContext(QtCore.Qt.WidgetWithChildrenShortcut)
            action.triggered.connect(slot)
            self.addAction(action)

        # Follow the scene: selection highlights its buttons, and a dial or
        # switch keyed over time updates which half of each limb shows.
        self._callbacks.append(om.MEventMessage.addEventCallback(
            "SelectionChanged", lambda *_: self._SyncSelection()))
        for message in (om.MSceneMessage.kAfterOpen,
                        om.MSceneMessage.kAfterNew):
            self._callbacks.append(om.MSceneMessage.addCallback(
                message, lambda *_: self.Reload()))
        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self._SyncModes)
        self._timer.start(250)

        self.Reload()

    # -- loading ---------------------------------------------------------

    def Reload(self):
        current = self._rig.node if self._rig else None
        try:
            self._rigs = [r for r in _Rigs() if r.pickers]
        except Exception as error:
            self._rigs = []
            self._status.setText("Could not read the picker: %s" % error)
        self._rigBox.blockSignals(True)
        self._rigBox.clear()
        for rig in self._rigs:
            self._rigBox.addItem(rig.node)
        index = next((i for i, r in enumerate(self._rigs)
                      if r.node == current), 0)
        self._rigBox.setCurrentIndex(index)
        self._rigBox.blockSignals(False)
        self._rigBox.setVisible(len(self._rigs) > 1)
        self._OnRigChanged(index)

    def _OnRigChanged(self, index):
        self._tabs.clear()
        self._views = []
        self._modes = {}
        self._rig = self._rigs[index] if 0 <= index < len(self._rigs) \
            else None
        if self._rig is None:
            self._status.setText(
                "No rig with a picker in this scene. Load one from "
                "RigExec > Load Rig..., then press Refresh.")
            return
        for picker in self._rig.pickers:
            for panel in picker.panels:
                if not any(b.parent == panel.id and b.live
                           and not b.decoration for b in picker.buttons):
                    continue
                view = PanelView(picker, panel)
                view.picked.connect(self._OnPicked)
                label = panel.label or panel.id.rsplit("/", 1)[-1]
                if len(self._rig.pickers) > 1:
                    label = "%s: %s" % (picker.name, label)
                self._tabs.addTab(view, label)
                self._views.append(view)
        self._status.setText("")
        self._SyncModes()
        self._SyncToggles()
        self._SyncSelection()

    # -- following the scene ---------------------------------------------

    def _Buttons(self):
        return self._rig.Buttons() if self._rig else []

    def _SyncSelection(self):
        if self._rig is None:
            return
        paths = self._rig.SelectedPaths()
        for view in self._views:
            view.SetSelected(paths)

    def _SyncModes(self):
        """Shows each limb's live half, and each switch's current value."""
        if self._rig is None or not self.isVisible():
            return
        modes, changed = self._rig.Modes()
        for view in self._views:
            view.SetModes(modes)
            if changed:
                view.update()

    def _SyncToggles(self):
        if self._rig is not None:
            self._rig.SyncToggles()
        for view in self._views:
            view.update()

    def _OnPicked(self, buttons, mode):
        if self._rig is None:
            return
        self._rig.Pick(buttons, mode, self._status.setText, self)
        self._SyncModes()
        self._SyncToggles()

    def _SelectAll(self):
        buttons = []
        for view in self._views:
            buttons.extend(b for b in view._picker.visible(
                view._panel.id, modes=view._modes) if b.live and b.targets)
        nodes = self._rig.Nodes(buttons) if self._rig else []
        if nodes:
            cmds.select(nodes, replace=True)

    def _Frame(self):
        if cmds.ls(selection=True):
            cmds.viewFit(animate=False)

    # -- closing ---------------------------------------------------------

    def _RemoveCallbacks(self):
        for callback in self._callbacks:
            try:
                om.MMessage.removeCallback(callback)
            except Exception:
                pass
        self._callbacks = []
        self._timer.stop()

    def dockCloseEventTriggered(self):
        self._RemoveCallbacks()

    def closeEvent(self, event):
        self._RemoveCallbacks()
        super(PickerWindow, self).closeEvent(event)
