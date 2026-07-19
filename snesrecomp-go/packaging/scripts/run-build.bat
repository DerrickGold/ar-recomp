@echo off
rem One-click build-and-play for Windows. Double-click this file.
rem Put your game ROM (.sfc) in this same folder first. See README.txt.
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "UTILS=%ROOT%\utils"

if not exist "%UTILS%" (
    echo ERROR: This package looks incomplete ^(the utils folder is missing^).
    echo Re-extract the downloaded archive and run this again.
    goto :end
)

cd /d "%ROOT%"

set "ROM="
for %%F in (*.sfc *.smc) do if not defined ROM set "ROM=%%F"
if not defined ROM (
    echo ERROR: No ROM found. Copy your game ROM ^(a .sfc file^) into this folder:
    echo   %ROOT%
    echo then run this again.
    goto :end
)

echo Using ROM: %ROM%
echo Building - the first run takes a few minutes...
echo.

"%UTILS%\tools\snesbuild.exe" all --hermetic --root "%UTILS%" --rom "%ROOT%\%ROM%" --allow-stubs
if errorlevel 1 (
    echo.
    echo ERROR: The build did not complete. The messages above say why;
    echo share them when asking for help.
    goto :end
)

set "BIN_NAME="
for %%F in ("%UTILS%\build\hermetic\*.exe") do if not defined BIN_NAME set "BIN_NAME=%%~nxF"
if not defined BIN_NAME (
    echo ERROR: Build finished but no game program was found.
    goto :end
)

rem Put the finished game (and its media library) in this folder.
copy /y "%UTILS%\build\hermetic\%BIN_NAME%" "%ROOT%\" >nul
if exist "%UTILS%\build\hermetic\SDL2.dll" copy /y "%UTILS%\build\hermetic\SDL2.dll" "%ROOT%\" >nul

rem Create a one-click "play again" script next to the game. In the generated
rem file, %%~dp0 becomes a literal %~dp0 (the folder the script lives in).
set "PLAY=%ROOT%\run-game.bat"
>"%PLAY%" echo @echo off
>>"%PLAY%" echo rem Runs the already-built game. Created by run-build.bat.
>>"%PLAY%" echo cd /d "%%~dp0utils"
>>"%PLAY%" echo "%%~dp0%BIN_NAME%" "%%~dp0%ROM%" --config config.ini

echo.
echo Build complete - this was a one time process. Now you can open `run-game.bat` to start the game
:end
echo.
pause
