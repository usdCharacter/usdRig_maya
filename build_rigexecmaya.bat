@echo off
rem Configures and builds the rigExecMaya plug-in (the rigExecRig node) into
rem rigExecMaya\plug-ins, where rigExecMaya.mod picks it up. The node links
rem the Maya devkit and usdRig's USD-free runtime, compiled in from the
rem usdRig checkout (see rigExecMaya\README.md).
rem
rem Usage: build_rigexecmaya.bat
rem
rem Environment overrides:
rem   RIG                   usdRig repo root, for _vcvars.bat and the
rem                         runtime sources (default: ..\usdRig beside this
rem                         sidecar)
rem   MAYA_VERSION          Maya release to build for (default: 2027)
rem   MAYA_DEVKIT_LOCATION  devkitBase, or a Maya install carrying include\
rem                         and lib\ (default: the Maya install for
rem                         MAYA_VERSION)
rem   BUILD                 build dir (default: %SIDECAR%\build-rigexecmaya)
rem   RIGEXEC_MAYA_PLUGIN_DIR  plug-in output dir (default:
rem                         rigExecMaya\plug-ins); point it elsewhere to build
rem                         while a Maya session holds the default copy
setlocal

if not defined SIDECAR (for %%I in ("%~dp0.") do set "SIDECAR=%%~fI")
if not defined RIG (for %%I in ("%SIDECAR%\..\usdRig") do set "RIG=%%~fI")
if not defined MAYA_VERSION set "MAYA_VERSION=2027"
if not defined MAYA_DEVKIT_LOCATION set "MAYA_DEVKIT_LOCATION=%ProgramFiles%\Autodesk\Maya%MAYA_VERSION%"
if not defined BUILD set "BUILD=%SIDECAR%\build-rigexecmaya"

if exist "%RIG%\bin\_vcvars.bat" (
    call "%RIG%\bin\_vcvars.bat"
) else (
    >&2 echo WARNING: no _vcvars.bat under "%RIG%\bin"; assuming the
    >&2 echo          compiler is already configured.
)

set "PLUGIN_DIR_ARG="
if defined RIGEXEC_MAYA_PLUGIN_DIR set "PLUGIN_DIR_ARG=-DRIGEXEC_MAYA_PLUGIN_DIR=%RIGEXEC_MAYA_PLUGIN_DIR%"

cmake -S "%SIDECAR%\rigExecMaya" -B "%BUILD%" -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DMAYA_DEVKIT_LOCATION="%MAYA_DEVKIT_LOCATION%" ^
      -DRIGEXEC_ROOT="%RIG%" %PLUGIN_DIR_ARG%
if errorlevel 1 exit /b 1
cmake --build "%BUILD%"
exit /b %errorlevel%
