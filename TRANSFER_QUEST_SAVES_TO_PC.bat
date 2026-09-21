@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\transfer-saves.ps1" -Direction QuestToPC %*
if errorlevel 1 (
  echo Save transfer failed. Check the message above. Existing backups are retained.
  pause
  exit /b 1
)
pause
