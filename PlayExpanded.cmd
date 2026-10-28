@echo off
rem One launcher for both games: its "Expanded" checkbox (front page, game card) picks the mod.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Play.ps1"
if errorlevel 1 pause
