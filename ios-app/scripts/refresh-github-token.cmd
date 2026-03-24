@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0refresh-github-token.ps1" %*
