@echo off
REM Build the Rive runtime static libraries needed by TDRiveTOP on Windows.
REM
REM Usage:
REM   scripts\build_rive.bat              :: release build (default)
REM   scripts\build_rive.bat debug        :: debug build
REM   scripts\build_rive.bat clean release
REM
REM Prereqs:
REM   * Visual Studio 2022 with the C++ workload (or Build Tools).
REM   * premake5.exe on PATH.
REM   * Git for Windows installed - provides sh.exe / bash.exe that Rive's
REM     own build_rive.sh wrapper requires.
REM   * Run this from a "Developer Command Prompt for VS 2022" (or one that
REM     has vcvars set), so cl.exe / MSBuild are available.
REM
REM Outputs land at:
REM   third_party\rive-runtime\renderer\out\<config>\*.lib

setlocal EnableDelayedExpansion
set "ROOT_DIR=%~dp0.."
set "RIVE_DIR=%ROOT_DIR%\third_party\rive-runtime"
set "RIVE_REPO=https://github.com/rive-app/rive-runtime.git"

if "%~1"=="" (
    set "ARGS=release"
) else (
    set "ARGS=%*"
)

if not exist "%RIVE_DIR%\premake5_v2.lua" (
    if not exist "%RIVE_DIR%\.git" (
        echo ^>^> Cloning rive-runtime into %RIVE_DIR%
        if not exist "%ROOT_DIR%\third_party" mkdir "%ROOT_DIR%\third_party"
        git clone --recursive "%RIVE_REPO%" "%RIVE_DIR%"
        if errorlevel 1 (
            echo ERROR: git clone failed.
            exit /b 1
        )
    )
)

where premake5 >nul 2>&1
if errorlevel 1 (
    echo ERROR: premake5 not found on PATH.
    echo Download the Windows zip from https://github.com/premake/premake-core/releases
    echo Extract premake5.exe somewhere on PATH.
    exit /b 1
)

REM Rive's build_rive.bat is a one-liner that just runs "sh build_rive.sh".
REM For that to work, build_rive.sh must be findable - which means the Rive
REM build/ directory has to be on PATH. We also need an sh on PATH, which
REM Git for Windows provides.
set "PATH=%RIVE_DIR%\build;%PATH%"

REM Make sure a POSIX shell is reachable. Git for Windows installs sh at
REM C:\Program Files\Git\usr\bin\sh.exe but doesn't put it on PATH by
REM default. Add it if we can find it.
where sh >nul 2>&1
if errorlevel 1 (
    if exist "C:\Program Files\Git\usr\bin\sh.exe" (
        set "PATH=C:\Program Files\Git\usr\bin;%PATH%"
    ) else (
        echo ERROR: sh not found. Install Git for Windows.
        exit /b 1
    )
)

REM Invoke from renderer/ - its premake5.lua dofiles in core + decoders +
REM dependencies, producing every static lib we need in one shot.
pushd "%RIVE_DIR%\renderer"
sh "%RIVE_DIR%\build\build_rive.sh" %ARGS%
set "_RIVE_RC=%errorlevel%"
popd

if not "%_RIVE_RC%"=="0" (
    echo ERROR: Rive build failed.
    exit /b %_RIVE_RC%
)

echo.
echo ^>^> Done.
echo Static libs are in:
echo   %RIVE_DIR%\renderer\out\^<config^>\
endlocal
