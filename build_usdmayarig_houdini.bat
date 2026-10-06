@echo off
rem Configures, builds, installs, and tests the standalone usdMayaRig
rem (Maya ASCII file-format) sidecar against the OpenUSD vendored inside a
rem Houdini install, in build-houdini. The Houdini twin of
rem build_usdmayarig.bat: the sidecar links the RigExec install built by
rem usdRig\bin\build_rigexec_houdini.bat, and the pxr package resolves
rem through that install's recorded USD prefix (the Houdini shim), so no
rem USD path is passed here. The live usdRig tree is untouched.
rem
rem Usage: build_usdmayarig_houdini.bat [--no-test] [--no-install]
rem
rem Known failure: testUsdMayaRigEvaluation fails under the vendored USD
rem 0.26.5 (the moved-pose evaluation returns rest positions) while the
rem identical test passes against stock 26.08, so it is a USD-version
rem behavioral delta in the same family as the main Houdini suite's
rem parity failures, not a sidecar regression.
rem
rem Environment overrides:
rem   HFS / HOUDINI_ROOT  Houdini install (default: Steam Houdini Indie)
rem   RIG                 usdRig repo root (default: ..\usdRig beside this
rem                       sidecar)
rem   RIGINSTALL          Houdini RigExec install prefix (default:
rem                       %RIG%\build-houdini\install)
rem   BUILD               sidecar build dir (default: %MAYA%\build-houdini)
rem   JOBS                parallel compile jobs (default: 8)
setlocal

if not defined MAYA (for %%I in ("%~dp0.") do set "MAYA=%%~fI")
if not defined RIG (for %%I in ("%MAYA%\..\usdRig") do set "RIG=%%~fI")
if not defined BUILD set "BUILD=%MAYA%\build-houdini"
if not defined HOUDINI_ROOT if defined HFS set "HOUDINI_ROOT=%HFS%"
if not defined HOUDINI_ROOT set "HOUDINI_ROOT=D:\SteamLibrary\steamapps\common\Houdini Indie"
if not defined RIGINSTALL set "RIGINSTALL=%RIG%\build-houdini\install"
if not defined JOBS set "JOBS=8"

if exist "%RIG%\bin\_vcvars.bat" (
    call "%RIG%\bin\_vcvars.bat"
) else (
    >&2 echo WARNING: no _vcvars.bat under "%RIG%\bin"; assuming the
    >&2 echo          compiler is already configured.
)
if not exist "%HOUDINI_ROOT%\bin\hython.exe" (
    >&2 echo ERROR: no Houdini install at "%HOUDINI_ROOT%".
    >&2 echo        Set HFS=^<houdini-root^> and retry.
    exit /b 1
)
if not exist "%RIGINSTALL%\lib\cmake\rigExec\rigExecConfig.cmake" (
    >&2 echo ERROR: no Houdini RigExec install at "%RIGINSTALL%".
    >&2 echo        Run usdRig\bin\build_rigexec_houdini.bat first, or set
    >&2 echo        RIGINSTALL=^<houdini-rigexec-install-prefix^>.
    exit /b 1
)

cmake -S "%MAYA%" -B "%BUILD%" -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DHOUDINI_ROOT="%HOUDINI_ROOT%" ^
      -DrigExec_DIR="%RIGINSTALL%\lib\cmake\rigExec" ^
      -DCMAKE_INSTALL_PREFIX="%BUILD%\install"
if errorlevel 1 exit /b 1
cmake --build "%BUILD%" -j %JOBS%
if errorlevel 1 exit /b 1

if "%~1"=="--no-install" exit /b 0
if "%~2"=="--no-install" exit /b 0
cmake --install "%BUILD%"
if errorlevel 1 exit /b 1

if "%~1"=="--no-test" exit /b 0
if "%~2"=="--no-test" exit /b 0
rem The test binaries link the vendored USD and RigExec DLLs, so those
rem directories ride on PATH while they run. The build-tree plugInfo
rem carries an absolute LibraryPath, so no plugin path is needed.
set "PATH=%BUILD%;%RIGINSTALL%\lib;%HOUDINI_ROOT%\bin;%PATH%"
ctest --test-dir "%BUILD%" --output-on-failure
exit /b %errorlevel%
