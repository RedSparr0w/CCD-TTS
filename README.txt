CCD_TTS 1.1.0 - Source / Build Package
======================================

This package contains the source code and build files for CCD_TTS.

BUILD
-----
Run:
  build.cmd

The build script uses the installed Visual Studio C++ x86 build tools and creates:
  build\CCD_TTS_1.1.0_Distribution.zip

The distribution ZIP contains the runtime files under bin\win32 together with TTS_README.txt.
Development helpers are kept in the source package and are not included in the public distribution.

PROJECT LAYOUT
--------------
src\
  C++ source for the TTS runtime and dinput8 loader.

CCD_TTS.ini
  Default TTS settings.

supertonic_install.ps1
piper_install.ps1
rvc_install.ps1
  Optional engine installers used by the in-game settings window.

TTS_README.txt
  End-user documentation included with the distribution.

STORAGE
-------
Runtime data is stored relative to the game installation:
  City Car Driving\bin\win32\CCD_TTS\

The active configuration file is:
  City Car Driving\bin\win32\CCD_TTS.ini
