"""Adds the RigExec menu once Maya's UI is up (module scripts run at startup)."""
import maya.cmds as cmds
import maya.utils

if not cmds.about(batch=True):
    maya.utils.executeDeferred("import rigexec_maya.menu; rigexec_maya.menu.Install()")
