@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\prepare-hd-wizard.ps1" %*
if errorlevel 1 (
  echo Media preparation failed. The existing disc data and saves were retained.
  pause
  exit /b 1
)
pause
