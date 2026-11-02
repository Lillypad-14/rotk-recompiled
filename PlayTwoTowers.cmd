@echo off
rem The Two Towers on PC (docs/SUPER_GAME.md, phase 1).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Play.ps1" -Target ttt
if errorlevel 1 pause
