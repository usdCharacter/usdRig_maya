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
    _InstallRuntimeCommand()


def ShowPicker():
    from . import picker
    return picker.Show()


def ToggleHoverPicker():
    from . import hover
    return hover.Toggle()


def _InstallRuntimeCommand():
    """RigExecToggleHoverPicker, for a hotkey of your choosing (Maya's own
    P is Parent, so the hover picker doesn't take it)."""
    name = "RigExecToggleHoverPicker"
    if not cmds.runTimeCommand(name, exists=True):
        cmds.runTimeCommand(
            name, category="Custom Scripts.RigExec",
            annotation="Show or hide the RigExec hover picker",
            commandLanguage="python",
            command="import rigexec_maya.menu; "
                    "rigexec_maya.menu.ToggleHoverPicker()")


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
