@echo off
REM ===================================================================
REM  double-click this file to watch the board's UART
REM
REM  It bypasses the PowerShell execution policy, which is why running
REM  .\serial_log.ps1 by hand fails on a default Windows install.
REM
REM  Once the window is open, CLICK ON IT so it has focus, then:
REM      1-9    jump to a scene
REM      SPACE  next scene
REM      ESC    quit
REM ===================================================================
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0serial_log.ps1" %*
echo.
echo ---- closed ----
pause
