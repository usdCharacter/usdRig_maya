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


def ShowPicker():
    from . import picker
    return picker.Show()


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
