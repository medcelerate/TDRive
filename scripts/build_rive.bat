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
REM   * premake5.exe on PATH (choco install premake, or download from premake.github.io).
REM   * Run this from a "Developer Command Prompt for VS 2022" (or one that
REM     has vcvars set), so cl.exe / MSBuild are available.
REM
REM Outputs land at:
REM   third_party\rive-runtime\renderer\out\<config>\*.lib

setlocal
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
    echo Install with: choco install premake
    echo Or download from https://premake.github.io/download
    exit /b 1
)

REM The renderer/ premake5.lua dofiles in the core runtime + decoders +
REM dependencies, so a single invocation produces everything.
pushd "%RIVE_DIR%\renderer"
call "%RIVE_DIR%\build\build_rive.bat" %ARGS%
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
