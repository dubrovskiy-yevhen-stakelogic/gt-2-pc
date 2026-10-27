@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\import-gt4-models.ps1" %*
if errorlevel 1 pause
