@echo off
rem One-click graphical build for Windows. Double-click this file.
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "UTILS=%ROOT%\utils"

if not exist "%UTILS%\tools\actraiser-builder.exe" (
    echo ERROR: This package looks incomplete.
    echo Re-extract the downloaded archive and run this again.
    goto :error
)

if not exist "%UTILS%\tools\sdl3\lib\SDL3.dll" (
    echo ERROR: The bundled SDL SDK is incomplete. Re-extract the archive.
    goto :error
)
if not exist "%UTILS%\tools\sdl3\lib\SDL3_ttf.dll" (
    echo ERROR: The bundled SDL font library is missing. Re-extract the archive.
    goto :error
)

echo Opening the local ActRaiser Recomp builder...
echo If the browser does not open, use the private URL shown below.
echo.

"%UTILS%\tools\actraiser-builder.exe" gui --root "%UTILS%" --output-dir "%ROOT%" --snesbuild "%UTILS%\tools\snesbuild.exe" --allow-stubs
if errorlevel 1 goto :error
exit /b 0

:error
echo.
echo The builder stopped unexpectedly. Share the messages above when asking for help.
echo.
pause
exit /b 1
