@echo off
rem Runs the export under the usdRig checkout's own Python and USD.
rem Usage: launch_export.bat <usdRig root> <stage> <out.rigexec> <out.json> [<rig prim path>]
set "RIGEXEC_EXPORT_DIR=%~dp0"
call "%~1\bin\_env.bat"
if errorlevel 1 exit /b 1
"%PY%" "%RIGEXEC_EXPORT_DIR%rigexec_maya_export.py" %2 %3 %4 %5
