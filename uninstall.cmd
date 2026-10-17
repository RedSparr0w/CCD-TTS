@echo off
setlocal EnableExtensions
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs -WorkingDirectory '%~dp0'"
  exit /b
)

set "GAME_DIR=%CD%"
if not exist "%GAME_DIR%\starter.exe" set "GAME_DIR=C:\Program Files (x86)\Steam\steamapps\common\City Car Driving\bin\win32"

del /Q "%GAME_DIR%\dinput8.dll" 2>nul
del /Q "%GAME_DIR%\CCD_TTS.dll" 2>nul

if /I "%~1"=="/data" (
  echo Removing portable CCD_TTS data...
  rmdir /S /Q "%GAME_DIR%\CCD_TTS" 2>nul
  del /Q "%GAME_DIR%\CCD_TTS.ini" 2>nul
)

rem Safety: if an older package left a ClassLib backup, restore it.
if exist "%GAME_DIR%\ClassLib_original.dll" (
  del /Q "%GAME_DIR%\ClassLib.dll" 2>nul
  move /Y "%GAME_DIR%\ClassLib_original.dll" "%GAME_DIR%\ClassLib.dll" >nul
)

echo CCD_TTS loader removed. Portable voices/models/settings were kept.
echo Run uninstall.cmd /data if you also want to remove CCD_TTS data and settings.
pause
