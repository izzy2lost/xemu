@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0set-github-token.ps1" %*
