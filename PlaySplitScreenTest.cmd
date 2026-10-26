@echo off
rem Split-screen co-op test (experimental). Plays like normal on one screen. Pick Multiplayer on the
rem title menu and player 2's console joins: the window opens a second screen. Back on the menu it
rem closes again. Player 1: your normal controls. Player 2: a second controller, or WASD + J/K,
rem Space = Start. Saves are read from your Expanded save and never written.
setlocal
set "ROOT=%~dp0"
set "PLAY=%ROOT%.private\expanded\play"
cd /d "%PLAY%" || goto :fail
"%ROOT%build\host-expanded\RotkSplitScreenSpike.exe" "%ROOT%.private\expanded\rotk.gba" "%ROOT%.private\expanded\gba_bios.bin" play "%PLAY%\rotk.sram"
if errorlevel 1 goto :fail
exit /b 0
:fail
pause
