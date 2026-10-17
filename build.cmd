@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio Build Tools / vswhere.exe was not found.
  exit /b 1
)

set "VSROOT="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
  echo ERROR: Visual C++ x86 build tools were not found.
  exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1

if not exist build mkdir build
if exist build\dinput8.dll del /Q build\dinput8.dll
if exist build\CCD_TTS.dll del /Q build\CCD_TTS.dll

echo [1/3] Building dinput8 proxy...
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /DWIN32 /D_WINDOWS src\dinput8_proxy.cpp /link ole32.lib /DEF:src\dinput8_proxy.def /OUT:build\dinput8.dll
if errorlevel 1 exit /b 1
if not exist build\dinput8.dll (
  echo ERROR: dinput8 proxy was not produced.
  exit /b 1
)

echo [2/3] Building CCD_TTS...
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /DWIN32 /D_WINDOWS src\tts.cpp /link ole32.lib sapi.lib winhttp.lib winmm.lib comdlg32.lib comctl32.lib shell32.lib user32.lib gdi32.lib /OUT:build\CCD_TTS.dll
if errorlevel 1 exit /b 1
if not exist build\CCD_TTS.dll (
  echo ERROR: CCD_TTS.dll was not produced.
  exit /b 1
)

copy /Y CCD_TTS.ini build\CCD_TTS.ini >nul

echo [3/3] Creating distribution ZIP...
set "DIST_DIR=build\dist"
set "DIST_WIN32=%DIST_DIR%\bin\win32"
set "DIST_ZIP=build\CCD_TTS_1.1.0_Distribution.zip"

if exist "%DIST_DIR%" rmdir /S /Q "%DIST_DIR%"
if exist "%DIST_ZIP%" del /Q "%DIST_ZIP%"
mkdir "%DIST_WIN32%"

copy /Y build\dinput8.dll "%DIST_WIN32%\dinput8.dll" >nul
copy /Y build\CCD_TTS.dll "%DIST_WIN32%\CCD_TTS.dll" >nul
copy /Y CCD_TTS.ini "%DIST_WIN32%\CCD_TTS.ini" >nul
copy /Y supertonic_install.ps1 "%DIST_WIN32%\supertonic_install.ps1" >nul
copy /Y piper_install.ps1 "%DIST_WIN32%\piper_install.ps1" >nul
copy /Y rvc_install.ps1 "%DIST_WIN32%\rvc_install.ps1" >nul
copy /Y TTS_README.txt "%DIST_DIR%\TTS_README.txt" >nul

for %%F in (dinput8.dll CCD_TTS.dll CCD_TTS.ini supertonic_install.ps1 piper_install.ps1 rvc_install.ps1) do (
  if not exist "%DIST_WIN32%\%%F" (
    echo ERROR: Distribution file %%F was not staged.
    exit /b 1
  )
)
if not exist "%DIST_DIR%\TTS_README.txt" (
  echo ERROR: Distribution file TTS_README.txt was not staged.
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%CD%\%DIST_DIR%\*' -DestinationPath '%CD%\%DIST_ZIP%' -Force"
if errorlevel 1 (
  echo ERROR: Failed to create distribution ZIP.
  exit /b 1
)
if not exist "%DIST_ZIP%" (
  echo ERROR: Distribution ZIP was not produced.
  exit /b 1
)

echo.
echo Build complete: %CD%\build
echo Distribution ZIP: %CD%\%DIST_ZIP%
echo ZIP contents: TTS_README.txt plus bin\win32 runtime files
exit /b 0
