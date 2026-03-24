@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fork-workflow-bridge.ps1" %*
