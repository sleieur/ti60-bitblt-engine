@echo off
REM ===================================================================
REM  goto_scene.bat  <key>  [seconds]
REM
REM  Sends one key to the board and prints its log for N seconds.
REM  No window focus needed - handy for grabbing measurements.
REM
REM  Keys are the on-screen numbering:
REM      1 INTRO  2 FILL  3 COPY  4 IMAGES  5 ALPHA
REM      6 KEY    7 GAME  8 LIMIT 9 SUMMARY
REM
REM      goto_scene.bat 7        jump to the game, watch 8 s
REM      goto_scene.bat 4 12     jump to the picture strip, watch 12 s
REM      goto_scene.bat 8 15     jump to the sprite-limit stress test
REM ===================================================================
setlocal enabledelayedexpansion

if "%~1"=="" (
    echo usage: goto_scene.bat ^<key^> [seconds]
    echo    goto_scene.bat 7
    echo    goto_scene.bat 4 12
    pause
    exit /b 1
)

set "KEY=%~1"
set "SECS=%~2"
if "%SECS%"=="" set "SECS=8"

REM pushd tolerates a trailing backslash in %~dp0, cd /d does not
pushd "%~dp0"
if errorlevel 1 (
    echo cannot enter "%~dp0"
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "serial_log.ps1" -Send "%KEY%" -Seconds %SECS%

popd
echo.
pause
