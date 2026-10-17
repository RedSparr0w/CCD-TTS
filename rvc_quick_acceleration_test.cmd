@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rvc_quick_acceleration_test.ps1"
exit /b %ERRORLEVEL%
