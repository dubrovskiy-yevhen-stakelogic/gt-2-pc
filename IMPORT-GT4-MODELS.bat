@echo off
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0scripts\import-gt4-models.ps1" %*
if errorlevel 1 (
  pause
  exit /b 1
)
exit /b 0
