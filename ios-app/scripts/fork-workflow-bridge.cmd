@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
if /I "%SCRIPT_DIR:~0,4%"=="\\?\" set "SCRIPT_DIR=%SCRIPT_DIR:~4%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%fork-workflow-bridge.ps1" %*
