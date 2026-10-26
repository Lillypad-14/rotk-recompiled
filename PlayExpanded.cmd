@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Play.ps1" -Target expanded
if errorlevel 1 pause
