@echo off
rem Configures, builds, installs, and tests the standalone usdMayaRig
rem (Maya ASCII file-format) sidecar against stock OpenUSD 26.08, in build.
rem The sidecar links the RigExec install in usdRig\build\install, which is
rem created from the stock build on demand; the live usdRig tree is
rem untouched (the plugin was removed from the main build and is staged
rem here from release-backup-20260928).
rem
rem Usage: build_usdmayarig.bat [--no-test]
rem
rem Environment overrides:
rem   RIG                 usdRig repo root (default: ..\usdRig beside this
rem                       sidecar)
rem   USD                 stock OpenUSD install (default: ..\usd-install
rem                       beside the repo, the _env.bat convention)
rem   RIGINSTALL          stock RigExec install prefix (default:
rem                       %RIG%\build\install; installed from %RIG%\build
rem                       when missing)
rem   BUILD               sidecar build dir (default: %MAYA%\build)
rem   JOBS                parallel compile jobs (default: 8)
setlocal

if not defined MAYA (for %%I in ("%~dp0.") do set "MAYA=%%~fI")
if not defined RIG (for %%I in ("%MAYA%\..\usdRig") do set "RIG=%%~fI")
if not defined USD (for %%I in ("%RIG%\..\usd-install") do set "USD=%%~fI")
if not defined BUILD set "BUILD=%MAYA%\build"
if not defined RIGINSTALL set "RIGINSTALL=%RIG%\build\install"
if not defined JOBS set "JOBS=8"

if exist "%RIG%\bin\_vcvars.bat" (
    call "%RIG%\bin\_vcvars.bat"
) else (
    >&2 echo WARNING: no _vcvars.bat under "%RIG%\bin"; assuming the
    >&2 echo          compiler is already configured.
)

if not exist "%RIGINSTALL%\lib\cmake\rigExec\rigExecConfig.cmake" (
    if not exist "%RIG%\build\rigExecRigging.dll" (
        >&2 echo ERROR: no stock RigExec build at "%RIG%\build".
        >&2 echo        Run usdRig\bin\build_rigexec.bat first, or set
        >&2 echo        RIGINSTALL=^<stock-rigexec-install-prefix^>.
        exit /b 1
    )
    >&2 echo Installing the stock RigExec package to "%RIGINSTALL%" ...
    cmake --install "%RIG%\build" --prefix "%RIGINSTALL%"
    if errorlevel 1 exit /b 1
)

cmake -S "%MAYA%" -B "%BUILD%" -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DrigExec_DIR="%RIGINSTALL%\lib\cmake\rigExec" ^
      -DCMAKE_INSTALL_PREFIX="%BUILD%\install"
if errorlevel 1 exit /b 1
cmake --build "%BUILD%" -j %JOBS%
if errorlevel 1 exit /b 1
cmake --install "%BUILD%"
if errorlevel 1 exit /b 1

if "%~1"=="--no-test" exit /b 0
rem The test binaries link the stock USD and RigExec DLLs, so those
rem directories ride on PATH while they run. The build-tree plugInfo
rem carries an absolute LibraryPath, so no plugin path is needed.
set "PATH=%BUILD%;%RIGINSTALL%\lib;%USD%\bin;%USD%\lib;%PATH%"
ctest --test-dir "%BUILD%" --output-on-failure
exit /b %errorlevel%
