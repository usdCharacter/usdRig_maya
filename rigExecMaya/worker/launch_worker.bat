@echo off
rem Starts the RigExec worker under the RigExec checkout's own Python and USD.
rem Usage: launch_worker.bat <rig root> <stage> [<rig prim path>]
set "RIGEXEC_WORKER_DIR=%~dp0"
call "%~1\bin\_env.bat"
if errorlevel 1 exit /b 1
"%PY%" "%RIGEXEC_WORKER_DIR%rigexec_maya_worker.py" %2 %3
