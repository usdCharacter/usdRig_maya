@echo off
rem Starts Maya with the rigExecMaya module registered, so the RigExec menu
rem and the rigExecRig node are available. Build the plug-in first with
rem build_rigexecmaya.bat.
rem
rem Usage: launch_maya.bat [<rig.usda>] [maya args...]
rem
rem With a rig, Maya loads it through rigexec_maya.scene.Load once the UI is
rem up; with none, use RigExec > Load Rig... The usdRig biped is at
rem ..\usdRig\examples\biped\Biped_stack.usda.
rem
rem Environment overrides:
rem   MAYA_VERSION   Maya release to start (default: 2027)
rem   MAYA_LOCATION  Maya install (default: the one for MAYA_VERSION)
rem   RIGEXEC_ROOT   usdRig checkout the worker runs under (default:
rem                  ..\usdRig beside this sidecar)
setlocal

if not defined SIDECAR (for %%I in ("%~dp0.") do set "SIDECAR=%%~fI")
if not defined MAYA_VERSION set "MAYA_VERSION=2027"
if not defined MAYA_LOCATION set "MAYA_LOCATION=%ProgramFiles%\Autodesk\Maya%MAYA_VERSION%"

if not exist "%SIDECAR%\rigExecMaya\plug-ins\rigExecMaya.mll" (
    >&2 echo ERROR: no rigExecMaya.mll; run build_rigexecmaya.bat first.
    exit /b 1
)
if not exist "%MAYA_LOCATION%\bin\maya.exe" (
    >&2 echo ERROR: no Maya at "%MAYA_LOCATION%"; set MAYA_LOCATION.
    exit /b 1
)

set "MAYA_MODULE_PATH=%SIDECAR%\rigExecMaya;%MAYA_MODULE_PATH%"

if "%~1"=="" (
    start "" "%MAYA_LOCATION%\bin\maya.exe"
    exit /b 0
)
for %%I in ("%~1") do set "RIG_STAGE=%%~fI"
set "RIG_STAGE=%RIG_STAGE:\=/%"
shift
start "" "%MAYA_LOCATION%\bin\maya.exe" -command "python(\"import maya.utils, rigexec_maya.scene; maya.utils.executeDeferred(lambda: rigexec_maya.scene.Load('%RIG_STAGE%'))\")" %1 %2 %3 %4 %5 %6 %7 %8 %9
exit /b 0
