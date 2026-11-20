"""
The frame-rate display: how fast Maya really is with the rig, shown in the
viewport's heads-up display.

    Viewport  how many times a second the viewport actually draws, the
              average time between draws, and the slowest one. This is the
              speed you see, while posing or playing back.
    Playback  scene frames shown per second against the scene's frame rate,
              while playing (below it, Maya is dropping or slowing frames).
    Rig       rig evaluations a second and the time each takes, and the
              share of poses that came ready-made from the rig's cache
              (playback is cached in the background once the rig is still).
    Frame     where each drawn frame's time goes, in milliseconds: the rig
              evaluating, the rig writing its outputs (controls, meshes),
              the meshes' deformers taking the rig's points, and Maya (the
              rest: drawing, other nodes, scripts).
    Computes  rig computes and mesh deforms per drawn frame (one compute
              and one deform per mesh is the least a new pose takes).

For the detail behind a slow frame, Profile in the RigExec menu records
Maya's Profiler for a few seconds while you work: the rig's own steps show
there under "RigExec".

Every number is measured over the last second. With several viewports the
busiest one is shown. The display follows Maya's own heads-up display
(Display > Heads Up Display), so that needs to be on too.

    import rigexec_maya.framerate
    rigexec_maya.framerate.SetEnabled(True)

Off by default; the choice is kept in the rigExecFrameRate optionVar.
"""

import bisect
import collections
import time

import maya.api.OpenMaya as om
import maya.api.OpenMayaUI as omui
import maya.cmds as cmds
import maya.mel as mel

_OPTION = "rigExecFrameRate"
# The heads-up display rows: name, label, and the text each shows.
_ROWS = (("rigExecFrameRateView", "Viewport", "view"),
         ("rigExecFrameRatePlayback", "Playback", "playback"),
         ("rigExecFrameRateRig", "Rig", "rig"),
         ("rigExecFrameRateFrame", "Frame", "frame"),
         ("rigExecFrameRateComputes", "Computes", "computes"))
_SECTION = 4                  # top right
_WINDOW = 1.0                 # seconds the numbers are measured over
_UPDATE = 0.25                # seconds between updates of the text
_PANELS_EVERY = 2.0           # seconds between looks for new viewports

_counter = None


def IsEnabled():
    if not cmds.optionVar(exists=_OPTION):
        return False
    return bool(cmds.optionVar(query=_OPTION))


def SetEnabled(enabled):
    cmds.optionVar(intValue=(_OPTION, int(bool(enabled))))
    if enabled:
        Install()
    else:
        Uninstall()


def Toggle():
    SetEnabled(not IsEnabled())


def Install():
    """Shows the display, if it is on."""
    global _counter
    Uninstall()
    if IsEnabled():
        _counter = _Counter()


def Uninstall():
    global _counter
    if _counter is not None:
        _counter.Close()
        _counter = None
    for name, _label, _key in _ROWS:
        if cmds.headsUpDisplay(name, exists=True):
            cmds.headsUpDisplay(name, remove=True)


class _Counter(object):

    def __init__(self):
        self.draws = {}           # panel: times it finished drawing
        self.panelCallbacks = {}  # panel: callback id
        self.times = collections.deque()      # when the scene time changed
        self.stats = collections.deque()      # (when, rigExecStats)
        self.text = dict((key, "-") for _n, _l, key in _ROWS)
        self.updated = 0.0
        self.panelsChecked = 0.0
        self.callbacks = [om.MEventMessage.addEventCallback(
            "timeChanged", lambda *_: self.times.append(time.perf_counter()))]
        self._WatchPanels()
        for name, label, key in _ROWS:
            if cmds.headsUpDisplay(name, exists=True):
                cmds.headsUpDisplay(name, remove=True)
            cmds.headsUpDisplay(
                name, section=_SECTION,
                block=cmds.headsUpDisplay(nextFreeBlock=_SECTION),
                label=label, labelWidth=70, dataWidth=200,
                labelFontSize="large", dataFontSize="large",
                attachToRefresh=True,
                command=lambda key=key: self._Text(key))

    def Close(self):
        for callback in self.callbacks + list(self.panelCallbacks.values()):
            try:
                om.MMessage.removeCallback(callback)
            except Exception:
                pass
        self.callbacks = []
        self.panelCallbacks = {}

    def _WatchPanels(self):
        self.panelsChecked = time.perf_counter()
        for panel in cmds.getPanel(type="modelPanel") or []:
            if panel in self.panelCallbacks:
                continue
            try:
                self.panelCallbacks[panel] = \
                    omui.MUiMessage.add3dViewPostRenderMsgCallback(
                        panel, lambda *_, panel=panel: self._Drawn(panel))
            except Exception:
                continue

    def _Drawn(self, panel):
        self.draws.setdefault(panel, collections.deque()).append(
            time.perf_counter())

    # -- the numbers ------------------------------------------------------

    def _Text(self, key):
        now = time.perf_counter()
        if now - self.updated >= _UPDATE:
            self.updated = now
            try:
                self._Update(now)
            except Exception as error:
                self.text = dict((key, "-") for _n, _l, key in _ROWS)
                self.text["view"] = "error: %s" % error
        return self.text[key]

    def _Update(self, now):
        if now - self.panelsChecked >= _PANELS_EVERY:
            self._WatchPanels()
        since = now - _WINDOW

        # The busiest viewport.
        best = None
        for draws in self.draws.values():
            while draws and draws[0] < since:
                draws.popleft()
            if len(draws) >= 2 and (best is None or len(draws) > len(best)):
                best = draws
        drawn = 0.0
        if best is None:
            self.text["view"] = "idle"
        else:
            gaps = [b - a for a, b in zip(list(best), list(best)[1:])]
            drawn = (len(best) - 1) / max(best[-1] - best[0], 1e-6)
            self.text["view"] = "%.1f fps   %.1f ms   slowest %.0f ms" % (
                drawn, 1000.0 * sum(gaps) / len(gaps), 1000.0 * max(gaps))

        # Playback.
        while self.times and self.times[0] < since:
            self.times.popleft()
        target = mel.eval("currentTimeUnitToFPS()")
        if cmds.play(query=True, state=True) and len(self.times) >= 2:
            fps = (len(self.times) - 1) / max(
                self.times[-1] - self.times[0], 1e-6)
            self.text["playback"] = "%.1f of %g fps" % (fps, target)
        else:
            self.text["playback"] = "stopped (%g fps)" % target

        # The rig.
        try:
            stats = cmds.rigExecStats()
        except Exception:
            for key in ("rig", "frame", "computes"):
                self.text[key] = "-"
            self.text["rig"] = "no rig plug-in"
            return
        self.stats.append((now, stats))
        while len(self.stats) > 2 and self.stats[1][0] < since:
            self.stats.popleft()
        (start, first), (end, last) = self.stats[0], self.stats[-1]
        delta = [b - a for a, b in zip(first, last)]
        while len(delta) < 7:
            delta.append(0.0)
        computes, evaluations, computeMs, evaluateMs, writeMs, deforms,             deformMs = delta[:7]
        span = max(end - start, 1e-6)
        if computes <= 0:
            self.text["rig"] = "idle"
        elif evaluations <= 0:
            self.text["rig"] = "%.0f poses/s   all cached" % (
                computes / span)
        else:
            self.text["rig"] = "%.0f evals/s   %.2f ms each   %.0f%% cached"                 % (evaluations / span, evaluateMs / evaluations,
                   100.0 * (1.0 - evaluations / computes))

        # Where each drawn frame's time goes.
        frames = drawn * span
        if frames < 1.0 or best is None:
            self.text["frame"] = "-"
            self.text["computes"] = "-"
            return
        frameMs = 1000.0 / drawn
        evaluate = evaluateMs / frames
        write = writeMs / frames
        other = (computeMs - evaluateMs - writeMs) / frames
        deform = deformMs / frames
        maya = max(0.0, frameMs - evaluate - write - other - deform)
        self.text["frame"] = (
            "rig %.1f + outputs %.1f + deform %.1f + Maya %.1f ms" % (
                evaluate + other, write, deform, maya))
        self.text["computes"] = "%.1f computes   %.1f deforms per frame" % (
            computes / frames, deforms / frames)


# -- profiling ----------------------------------------------------------------
#
# Profile records Maya's Profiler while you work, then says where each drawn
# frame's time went: by kind of work, by step (the rig's own steps are under
# RigExec), and what Maya's other threads did while its main thread waited
# for the scene to evaluate. The recording stays loaded in Maya's Profiler
# window, which opens on it, for the full detail.

PROFILE_SECONDS = 3.0
_REPORT_WINDOW = "rigExecProfileReport"
_profiling = None


def Profile(seconds=PROFILE_SECONDS):
    """Records the next `seconds` of Maya at work: pose or play back while
    it runs. The report then shows in a window and the Script Editor."""
    global _profiling
    if _profiling is not None:
        return
    try:
        from PySide6 import QtCore
    except ImportError:
        from PySide2 import QtCore
    cmds.profiler(bufferSize=500)
    cmds.profiler(reset=True)
    draws = []
    callbacks = []
    for panel in cmds.getPanel(type="modelPanel") or []:
        try:
            callbacks.append(omui.MUiMessage.add3dViewPostRenderMsgCallback(
                panel, lambda *_, panel=panel: draws.append(panel)))
        except Exception:
            continue
    cmds.profiler(sampling=True)
    om.MGlobal.displayInfo(
        "RigExec: profiling for %g seconds; pose or play back now" % seconds)
    _profiling = (draws, callbacks, seconds)
    QtCore.QTimer.singleShot(int(seconds * 1000), _FinishProfile)


def _FinishProfile():
    global _profiling
    draws, callbacks, seconds = _profiling
    _profiling = None
    cmds.profiler(sampling=False)
    for callback in callbacks:
        try:
            om.MMessage.removeCallback(callback)
        except Exception:
            pass
    # The busiest viewport's draws are the frames.
    frames = max([draws.count(p) for p in set(draws)] or [0])
    path = cmds.internalVar(userTmpDir=True) + "rigExecProfile.txt"
    cmds.profiler(output=path)
    try:
        report = _Report(path, frames, seconds)
    except Exception as error:
        report = "RigExec: the profile could not be read: %s" % error
    print(report)
    _ShowReport(report)
    try:
        mel.eval("ProfilerTool")
    except Exception:
        pass


def _Report(path, frames, seconds):
    with open(path, encoding="utf-8", errors="replace") as handle:
        lines = handle.read().splitlines()
    categories = lines[2].split("\t")
    names, i = {}, 0
    while i < len(lines) and not lines[i].startswith("#Comment mapping"):
        i += 1
    while i < len(lines) and not lines[i].startswith("#Event time"):
        if lines[i].startswith("@"):
            key, _sep, value = lines[i].partition(" = ")
            names[key] = value
        i += 1
    events, seen = [], set()
    for line in lines[i + 1:]:
        if line.startswith("#"):
            break
        if line in seen:
            continue
        seen.add(line)
        f = line.split("\t")
        if len(f) < 7:
            continue
        # [thread, start, duration, name, detail, category, own time]
        events.append([int(f[6]), int(f[0]), int(f[4]), names.get(f[1], f[1]),
                       names.get(f[2], f[2]), int(f[3]), int(f[4])])
    # Each event's own time: its duration less what ran inside it.
    events.sort(key=lambda e: (e[0], e[1], -e[2]))
    stack = []
    for event in events:
        while stack and (stack[-1][0] != event[0] or
                         event[1] >= stack[-1][1] + stack[-1][2]):
            stack.pop()
        if stack:
            stack[-1][6] -= event[2]
        stack.append(event)

    def category(index):
        return categories[index] if 0 <= index < len(categories) else "?"

    per = 1000.0 * max(frames, 1)
    byCategory = collections.Counter()
    bySteps = collections.Counter()
    calls = collections.Counter()
    for _t, _s, _d, name, _detail, cat, own in events:
        byCategory[category(cat)] += max(own, 0)
        bySteps[(category(cat), name)] += max(own, 0)
        calls[(category(cat), name)] += 1
    # What other threads did while the main thread waited for evaluation.
    waits = [(e[1], e[1] + e[2], e[0]) for e in events
             if e[3] == "Vp2WaitForEvaluation"]
    waits.sort()
    during = collections.Counter()
    starts = [w[0] for w in waits]
    for thread, start, _d, name, detail, cat, own in events:
        k = bisect.bisect_right(starts, start) - 1
        if k >= 0 and start < waits[k][1] and thread != waits[k][2]:
            label = name if name != "Compute" else "Compute " + detail
            during[(category(cat), label)] += max(own, 0)
    waited = sum(b - a for a, b, _t in waits)

    out = ["", "RigExec profile: %d frames drawn in %g s (%.1f fps)" % (
        frames, seconds, frames / seconds)]
    if frames:
        out.append("  %.1f ms per frame" % (seconds * 1000.0 / frames))
    out.append("")
    out.append("Time per frame by kind of work (ms, each step's own time):")
    for name, value in byCategory.most_common(8):
        out.append("  %-24s %7.2f" % (name, value / per))
    out.append("")
    out.append("Slowest steps (ms per frame, calls per frame):")
    for (cat, name), value in bySteps.most_common(20):
        out.append("  %-20s %-36s %7.2f %7.1f" % (
            cat[:20], name[:36], value / per, calls[(cat, name)] /
            float(max(frames, 1))))
    out.append("")
    out.append("Main thread waiting for the scene to evaluate: %.2f ms per "
               "frame. Meanwhile, on other threads:" % (waited / per))
    for (cat, name), value in during.most_common(12):
        out.append("  %-20s %-36s %7.2f" % (cat[:20], name[:36],
                                             value / per))
    out.append("")
    out.append("Full detail: Windows > General Editors > Profiler.")
    return "\n".join(out)


def _ShowReport(report):
    if cmds.window(_REPORT_WINDOW, exists=True):
        cmds.deleteUI(_REPORT_WINDOW)
    cmds.window(_REPORT_WINDOW, title="RigExec Profile",
                widthHeight=(720, 620))
    cmds.paneLayout()
    cmds.scrollField(text=report.strip(), editable=False, wordWrap=False,
                     font="fixedWidthFont")
    cmds.showWindow(_REPORT_WINDOW)
