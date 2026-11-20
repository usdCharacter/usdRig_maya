"""
How the rig's marking menu looks in Maya: dark rounded labels with a light
border, lit blue under the cursor, around a centre mark, with a guide from
the centre to the cursor while a stroke is held. The same look usdview's
and Unreal's marking menus have.

Painted in a frameless, transparent window over the screen, so a ring near
the viewport's edge is not clipped, and taking no input: the viewport's
own events drive the gesture (see markingmenu).
"""

import math

try:
    from PySide6 import QtCore, QtGui, QtWidgets
except ImportError:
    from PySide2 import QtCore, QtGui, QtWidgets

_FILL = QtGui.QColor(73, 73, 73, 255)
_BORDER = QtGui.QColor(133, 133, 133, 255)
_TEXT = QtGui.QColor(255, 255, 255)
_LIT_FILL = QtGui.QColor(109, 160, 177, 255)
_LIT_BORDER = QtGui.QColor(172, 218, 255, 255)
_LIT_TEXT = QtGui.QColor(0, 0, 0)
_DISABLED_TEXT = QtGui.QColor(134, 134, 134, 255)
_CHECK = QtGui.QColor(129, 212, 250)
_GUIDE = QtGui.QColor(128, 128, 128, 255)
_RADIUS = 7.0
_BORDER_WIDTH = 2.0


class Painter(object):
    """Where each item of a resolved menu is drawn and what is under a
    point, in the window's pixels around a centre. `model` is usdRig's
    markingMenuModel."""

    def __init__(self, model, font):
        self.mm = model
        self.font = QtGui.QFont(font)
        if self.font.pointSizeF() > 0:
            self.font.setPointSizeF(self.font.pointSizeF() * 1.1)
        metrics = QtGui.QFontMetricsF(self.font)
        self.metrics = metrics
        self.base = metrics.height()
        # An ellipse rather than a circle: wide labels need the room on
        # the left and right more than above and below.
        r = model.RADIUS_PX
        ry = r * 0.7
        dx, dy = r * 0.8, r * 0.45 * 0.7 + self.base * 1.2
        self._anchors = {"N": (0.0, -ry), "S": (0.0, ry),
                         "E": (r, 0.0), "W": (-r, 0.0),
                         "NE": (dx, -dy), "NW": (-dx, -dy),
                         "SE": (dx, dy), "SW": (-dx, dy)}

    def _Size(self, text):
        width = self.metrics.horizontalAdvance(text)
        return (width + self.base * 2.2, self.base * 1.6)

    @staticmethod
    def _Text(resolved):
        text = resolved.label
        if resolved.isSubmenu:
            text += "  ▸"
        return text

    def ItemRects(self, menu, centre, avoid=()):
        """{item id: QRectF} for the ring, each label growing away from the
        centre, and moved aside from the way back to a menu above."""
        out = {}
        cx, cy = centre
        for direction, resolved in menu.slots.items():
            ax, ay = self._anchors[direction]
            w, h = self._Size(self._Text(resolved))
            if resolved.checked:
                w += self.base
            x, y = cx + ax, cy + ay
            if "W" in direction:
                x -= w
                y -= h * 0.5
            elif "E" in direction:
                y -= h * 0.5
            elif direction == "N":
                x -= w * 0.5
                y -= h
            else:
                x -= w * 0.5
            out[resolved.id] = self._Clear(QtCore.QRectF(x, y, w, h),
                                           direction, avoid)
        return out

    def _Clear(self, rect, direction, avoid):
        if not avoid:
            return rect
        reach = self.mm.BACK_RADIUS_PX + 6.0
        dx, dy = self.mm.DIRECTION_VECTORS[direction]
        if dy == 0:
            px, py = 0.0, -1.0
        elif dx == 0:
            px, py = 1.0, 0.0
        else:
            px, py = (dy, -dx) if dy * dx > 0 else (-dy, dx)
            if py * dy < 0:
                px, py = -px, -py
            length = math.hypot(px, py)
            px, py = px / length, py / length
        moved = QtCore.QRectF(rect)
        for _ in range(40):
            if not self._OnACircle(moved, avoid):
                break
            moved.translate(px * 4.0, py * 4.0)
        return moved

    def CoveredDirections(self, menu, centre, circles):
        """The directions whose labels would cover a way-back circle, so a
        submenu leaves them empty."""
        rects = self.ItemRects(menu, centre)
        return [resolved.direction for resolved in menu.slots.values()
                if self._OnACircle(rects[resolved.id], circles)]

    def _OnACircle(self, rect, avoid):
        reach = self.mm.BACK_RADIUS_PX + 6.0
        for ax, ay in avoid:
            if rect.intersects(QtCore.QRectF(ax - reach, ay - reach,
                                             2 * reach, 2 * reach)):
                return True
        return False

    def ColumnRects(self, menu, centre, avoid=()):
        """{item id: QRectF} for the column of extra rows, and the column's
        box (None when there is none). Below the ring, unless a way-back
        circle is there."""
        if not menu.overflow:
            return {}, None
        r = self.mm.RADIUS_PX
        width = max(self._Size(self._Text(item))[0] + self.base
                    for item in menu.overflow)
        h = self.base * 1.5
        height = h * len(menu.overflow)
        gap = r * 0.7 + self.base * 3.0
        cx, cy = centre
        places = [(cx - width * 0.5, cy + gap),
                  (cx - width * 0.5, cy - gap - height),
                  (cx + r * 1.6, cy - height * 0.5),
                  (cx - r * 1.6 - width, cy - height * 0.5)]
        x, y = places[0]
        for px, py in places:
            if not self._OnACircle(QtCore.QRectF(px, py, width, height),
                                   avoid):
                x, y = px, py
                break
        rows = {}
        for i, resolved in enumerate(menu.overflow):
            rows[resolved.id] = QtCore.QRectF(x, y + i * h, width, h)
        box = QtCore.QRectF(x - _BORDER_WIDTH, y - _BORDER_WIDTH,
                            width + 2 * _BORDER_WIDTH,
                            height + 2 * _BORDER_WIDTH)
        return rows, box

    def ItemAt(self, menu, centre, point, avoid=()):
        """The item whose label is under `point`, or None."""
        p = QtCore.QPointF(point[0], point[1])
        rows, _ = self.ColumnRects(menu, centre, avoid)
        for itemId, rect in list(self.ItemRects(menu, centre,
                                                avoid).items()) + \
                list(rows.items()):
            if rect.contains(p):
                return menu.Find(itemId)
        return None

    def Paint(self, painter, menu, centre, lit, cursor, parents=(),
              litBack=None):
        painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
        painter.setRenderHint(QtGui.QPainter.TextAntialiasing, True)
        painter.setFont(self.font)
        cx, cy = centre
        back = self.mm.BACK_RADIUS_PX
        # The way back: a faint trail through every menu above this one,
        # and a circle where each was centred.
        if parents:
            trail = list(parents) + [centre]
            painter.setPen(QtGui.QPen(QtGui.QColor(128, 128, 128, 140), 2.0,
                                      QtCore.Qt.DashLine))
            for a, b in zip(trail, trail[1:]):
                painter.drawLine(QtCore.QPointF(*a), QtCore.QPointF(*b))
            for index, (px, py) in enumerate(parents):
                hot = index == litBack
                painter.setBrush(QtGui.QBrush(_LIT_FILL if hot else _FILL))
                painter.setPen(QtGui.QPen(_LIT_BORDER if hot else _BORDER,
                                          _BORDER_WIDTH))
                painter.drawEllipse(QtCore.QPointF(px, py), back, back)
                painter.setPen(QtGui.QPen(_LIT_TEXT if hot else _TEXT, 2.0))
                s = back * 0.4
                painter.drawPolyline([QtCore.QPointF(px + s * 0.4, py - s),
                                      QtCore.QPointF(px - s * 0.6, py),
                                      QtCore.QPointF(px + s * 0.4, py + s)])
        # The guide from the centre to the cursor, under everything.
        if cursor is not None:
            pen = QtGui.QPen(_GUIDE, 3.0)
            pen.setCapStyle(QtCore.Qt.RoundCap)
            painter.setPen(pen)
            painter.drawLine(QtCore.QPointF(cx, cy),
                             QtCore.QPointF(cursor[0], cursor[1]))
        # The centre mark: a grey ring inside a black one.
        radius = max(6.0, self.base * 0.5)
        painter.setBrush(QtCore.Qt.NoBrush)
        painter.setPen(QtGui.QPen(_GUIDE, 5.0))
        painter.drawEllipse(QtCore.QPointF(cx, cy), radius * 0.5,
                            radius * 0.5)
        painter.setPen(QtGui.QPen(QtGui.QColor(0, 0, 0), 2.0))
        painter.drawEllipse(QtCore.QPointF(cx, cy), radius * 0.5 + 2.0,
                            radius * 0.5 + 2.0)

        rects = self.ItemRects(menu, centre, parents)
        for resolved in menu.slots.values():
            self._PaintPill(painter, resolved, rects[resolved.id],
                            resolved.id == lit)
        rows, box = self.ColumnRects(menu, centre, parents)
        if box is not None:
            painter.setBrush(QtGui.QBrush(_FILL))
            painter.setPen(QtGui.QPen(_BORDER, _BORDER_WIDTH))
            painter.drawRoundedRect(box, _RADIUS, _RADIUS)
            for resolved in menu.overflow:
                rect = rows[resolved.id]
                if resolved.id == lit and resolved.enabled:
                    painter.setBrush(QtGui.QBrush(_LIT_FILL))
                    painter.setPen(QtCore.Qt.NoPen)
                    painter.drawRect(rect)
                self._PaintLabel(painter, resolved, rect,
                                 resolved.id == lit, QtCore.Qt.AlignLeft,
                                 roomForCheck=True)

    def _PaintPill(self, painter, resolved, rect, lit):
        lit = lit and resolved.enabled
        if not resolved.enabled:
            painter.setBrush(QtCore.Qt.NoBrush)
            painter.setPen(QtCore.Qt.NoPen)
        else:
            painter.setBrush(QtGui.QBrush(_LIT_FILL if lit else _FILL))
            painter.setPen(QtGui.QPen(_LIT_BORDER if lit else _BORDER,
                                      _BORDER_WIDTH))
        painter.drawRoundedRect(rect, _RADIUS, _RADIUS)
        self._PaintLabel(painter, resolved, rect, lit,
                         QtCore.Qt.AlignHCenter)

    def _PaintLabel(self, painter, resolved, rect, lit, align,
                    roomForCheck=False):
        box = QtCore.QRectF(rect).adjusted(self.base * 0.6, 0,
                                           -self.base * 0.6, 0)
        if resolved.checked:
            # A check mark for a toggle or choice that is on.
            mark = box.left() + self.base * 0.45
            painter.setPen(QtGui.QPen(_LIT_TEXT if lit else _CHECK, 2.0))
            mid = box.center().y()
            s = self.base * 0.3
            painter.drawPolyline([
                QtCore.QPointF(mark - s, mid),
                QtCore.QPointF(mark - s * 0.3, mid + s * 0.7),
                QtCore.QPointF(mark + s, mid - s * 0.8)])
        if resolved.checked or roomForCheck:
            box.setLeft(box.left() + self.base)
        painter.setPen(_DISABLED_TEXT if not resolved.enabled
                       else _LIT_TEXT if lit else _TEXT)
        painter.drawText(box, align | QtCore.Qt.AlignVCenter,
                         self._Text(resolved))


class Window(QtWidgets.QWidget):
    """The transparent window the menu is painted in. Takes no input."""

    def __init__(self, model, parent=None):
        flags = (QtCore.Qt.Tool | QtCore.Qt.FramelessWindowHint
                 | QtCore.Qt.WindowStaysOnTopHint
                 | QtCore.Qt.NoDropShadowWindowHint
                 | QtCore.Qt.WindowDoesNotAcceptFocus)
        super(Window, self).__init__(parent, flags)
        self.setAttribute(QtCore.Qt.WA_TranslucentBackground, True)
        self.setAttribute(QtCore.Qt.WA_TransparentForMouseEvents, True)
        self.setAttribute(QtCore.Qt.WA_ShowWithoutActivating, True)
        self.setFocusPolicy(QtCore.Qt.NoFocus)
        self.painter = Painter(model, QtWidgets.QApplication.font())
        self.menu = None
        self.centre = None      # window pixels
        self.lit = None         # item id under the cursor
        self.cursor = None
        self.parents = []       # centres of the menus above
        self.litBack = None     # the way-back circle under the cursor

    def Cover(self, view):
        """Lays the window over the screen the viewport is on."""
        screen = view.screen() if hasattr(view, "screen") else None
        if screen is None:
            screen = QtGui.QGuiApplication.primaryScreen()
        self.setGeometry(screen.geometry())

    def ToLocal(self, view, point):
        local = self.mapFromGlobal(view.mapToGlobal(
            QtCore.QPoint(int(round(point[0])), int(round(point[1])))))
        return (local.x() + (point[0] - round(point[0])),
                local.y() + (point[1] - round(point[1])))

    def paintEvent(self, event):
        if self.menu is None or self.centre is None:
            return
        painter = QtGui.QPainter(self)
        try:
            self.painter.Paint(painter, self.menu, self.centre, self.lit,
                               self.cursor, self.parents, self.litBack)
        finally:
            painter.end()
