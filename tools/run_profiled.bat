@echo off
rem Run the game with timing diagnostics. Play a few points, then close the game.
rem The log is written to build_assets\match_profile.log.
cd /d "%~dp0.."
set MT_PROF=1
set MT_LOG=%~dp0..\build_assets\match_profile.log
start "" /wait build\MarioTennisRecompiled.exe
echo Log saved to build_assets\match_profile.log
