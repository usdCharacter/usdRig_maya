"""The RigExec menu in Maya's main menu bar."""
import maya.cmds as cmds
import maya.mel as mel

_MENU = "rigExecMenu"


def Install():
    if cmds.menu(_MENU, exists=True):
        cmds.deleteUI(_MENU)
    window = mel.eval("$rigExecMain = $gMainWindow")
    cmds.menu(_MENU, label="RigExec", parent=window, tearOff=True)
    cmds.menuItem(label="Load Rig...", command=lambda *_: LoadRigDialog())
    cmds.menuItem(label="Picker", command=lambda *_: ShowPicker())
    cmds.menuItem(label="Hover Picker",
                  annotation="Show or hide the picker over the viewport "
                  "(bind RigExecToggleHoverPicker in the Hotkey Editor)",
                  command=lambda *_: ToggleHoverPicker())
    cmds.menuItem(divider=True)
    from . import automatch, framerate, markingmenu, touchpose
    cmds.menuItem(label="TouchPose", checkBox=touchpose.IsEnabled(),
                  annotation="Hover the skin to light a control's region; "
                  "click it to select the control",
                  command=lambda on: touchpose.SetEnabled(bool(on)))
    cmds.menuItem(optionBox=True,
                  annotation="TouchPose Options: how strongly the skin "
                  "is tinted",
                  command=lambda *_: touchpose.ShowOptions())
    cmds.menuItem(label="IK/FK Auto-Switch",
                  checkBox=automatch.IsEnabled(),
                  annotation="Selecting a control of a limb's idle half "
                  "switches the limb to it, matched",
                  command=lambda on: automatch.SetEnabled(bool(on)))
    cmds.menuItem(label="Marking Menu", checkBox=markingmenu.IsEnabled(),
                  annotation="Right-click a control for the rig's marking "
                  "menu",
                  command=lambda on: markingmenu.SetEnabled(bool(on)))
    cmds.menuItem(divider=True)
    from . import scene
    cmds.menuItem(label="Fast Control Drawing",
                  checkBox=scene.IsDrawingControls(),
                  annotation="Draw the control curves from the rig in one "
                  "go (off: Maya draws each curve shape itself, slower)",
                  command=lambda on: scene.SetDrawControls(bool(on)))
    cmds.menuItem(label="Frame Rate Display",
                  checkBox=framerate.IsEnabled(),
                  annotation="Show how fast the viewport, playback and the "
                  "rig really are, in the viewport",
                  command=lambda on: framerate.SetEnabled(bool(on)))
    cmds.menuItem(label="Profile %g Seconds" % framerate.PROFILE_SECONDS,
                  annotation="Record Maya's Profiler while you pose or play "
                  "back, then show where each frame's time went",
                  command=lambda *_: framerate.Profile())
    _InstallRuntimeCommand()
    for feature in (automatch, framerate, markingmenu, touchpose):
        try:
            feature.Install()
        except Exception as error:
            import maya.api.OpenMaya as om
            om.MGlobal.displayWarning("RigExec: %s did not start: %s"
                                      % (feature.__name__, error))


def ShowPicker():
    from . import picker
    return picker.Show()


def ToggleHoverPicker():
    from . import hover
    return hover.Toggle()


def _InstallRuntimeCommand():
    """Commands for hotkeys of your choosing (Hotkey Editor > Custom
    Scripts > RigExec): the hover picker (Maya's own P is Parent, so the
    hover picker doesn't take it), the TouchPose highlight strength and
    the frame-rate display."""
    commands = (
        ("RigExecToggleHoverPicker", "Show or hide the RigExec hover picker",
         "import rigexec_maya.menu; rigexec_maya.menu.ToggleHoverPicker()"),
        ("RigExecTouchPoseStronger", "Make the TouchPose highlight stronger",
         "import rigexec_maya.touchpose as t; t.StepStrength(1)"),
        ("RigExecTouchPoseFainter", "Make the TouchPose highlight fainter",
         "import rigexec_maya.touchpose as t; t.StepStrength(-1)"),
        ("RigExecToggleFrameRate", "Show or hide the RigExec frame rate",
         "import rigexec_maya.framerate as f; f.Toggle()"),
    )
    for name, annotation, command in commands:
        if not cmds.runTimeCommand(name, exists=True):
            cmds.runTimeCommand(
                name, category="Custom Scripts.RigExec",
                annotation=annotation, commandLanguage="python",
                command=command)


def LoadRigDialog():
    picked = cmds.fileDialog2(fileMode=1, caption="Load a RigExec rig",
                              fileFilter="USD (*.usda *.usdc *.usd)")
    if not picked:
        return None
    from . import scene
    cmds.waitCursor(state=True)
    try:
        return scene.Load(picked[0])
    finally:
        cmds.waitCursor(state=False)
