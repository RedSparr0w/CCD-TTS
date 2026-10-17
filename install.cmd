@echo off
setlocal EnableExtensions
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs -WorkingDirectory '%~dp0'"
  exit /b
)

set "GAME_DIR=%CD%"
if not exist "%GAME_DIR%\starter.exe" (
  set "GAME_DIR=C:\Program Files (x86)\Steam\steamapps\common\City Car Driving\bin\win32"
)
if not exist "%GAME_DIR%\starter.exe" (
  echo ERROR: starter.exe was not found. Extract this package into CCD\bin\win32 first.
  pause
  exit /b 1
)

if not exist build\dinput8.dll goto :needbuild
if not exist build\CCD_TTS.dll goto :needbuild
goto :built

:needbuild
echo Built files are missing. Building first...
call build.cmd
if errorlevel 1 (
  echo ERROR: Build failed. Nothing was installed.
  pause
  exit /b 1
)

:built
rem Restore the stock ClassLib if an older CCD_TTS version replaced it.
if exist "%GAME_DIR%\ClassLib_original.dll" (
  echo Restoring original ClassLib.dll from the older loader...
  del /Q "%GAME_DIR%\ClassLib.dll" 2>nul
  move /Y "%GAME_DIR%\ClassLib_original.dll" "%GAME_DIR%\ClassLib.dll" >nul
)

rem Upgrade/install in place. Existing dinput8.dll from earlier CCD_TTS versions is overwritten.
rem If another mod also uses dinput8.dll, back it up before installing CCD_TTS.
copy /Y build\dinput8.dll "%GAME_DIR%\dinput8.dll" >nul
if errorlevel 1 (
  echo ERROR: Failed to update dinput8.dll. Make sure the game is closed and retry as administrator.
  pause
  exit /b 1
)
copy /Y build\CCD_TTS.dll "%GAME_DIR%\CCD_TTS.dll" >nul
if errorlevel 1 (
  echo ERROR: Failed to update CCD_TTS.dll. Make sure the game is closed and retry as administrator.
  pause
  exit /b 1
)
copy /Y CCD_TTS.ini "%GAME_DIR%\CCD_TTS.ini" >nul
copy /Y supertonic_install.ps1 "%GAME_DIR%\supertonic_install.ps1" >nul
copy /Y piper_install.ps1 "%GAME_DIR%\piper_install.ps1" >nul
copy /Y rvc_install.ps1 "%GAME_DIR%\rvc_install.ps1" >nul

echo.
echo Installed/updated successfully without replacing any original CCD DLL.
echo Launch City Car Driving normally through Steam.
echo Runtime data: %GAME_DIR%\CCD_TTS
pause
