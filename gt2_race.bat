@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" -Mode simulation --race %*
if errorlevel 1 pause
