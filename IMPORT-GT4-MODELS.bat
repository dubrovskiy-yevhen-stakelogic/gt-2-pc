@echo off
setlocal
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0scripts\import-gt4-models.ps1" %*
set "result=%errorlevel%"
if not defined GT2_IMPORT_NO_PAUSE pause
exit /b %result%
