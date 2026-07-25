@echo off
rem One-click graphical build for Windows. Double-click this file.
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "UTILS=%ROOT%\utils"

if not exist "%UTILS%\tools\snesbuild.exe" (
    echo ERROR: This package looks incomplete.
    echo Re-extract the downloaded archive and run this again.
    goto :error
)

if not exist "%UTILS%\tools\sdl3\lib\SDL3.dll" (
    echo NOTE: bundled SDL3.dll was not found. This platform may require a
    echo system SDL3 development package before it can build.
    echo.
)

echo Opening the local ActRaiser Recomp builder...
echo If the browser does not open, use the private URL shown below.
echo.

"%UTILS%\tools\snesbuild.exe" gui --root "%UTILS%" --output-dir "%ROOT%" --allow-stubs
if errorlevel 1 goto :error
exit /b 0

:error
echo.
echo The builder stopped unexpectedly. Share the messages above when asking for help.
echo.
pause
exit /b 1
