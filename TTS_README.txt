CCD_TTS 1.1.0
=============

CCD_TTS adds text-to-speech support to City Car Driving.

INSTALLATION
------------
1. Extract the distribution ZIP into the City Car Driving installation folder.
2. Allow the included bin\win32 folder to merge with the existing game folder.
3. Start City Car Driving normally through Steam.
4. Press F10 in-game to open the TTS settings window.

No separate launcher is required.

SETTINGS
--------
Use F10 to:
- Enable or disable TTS.
- Select a speech engine and voice.
- Adjust speech rate, pitch, and volume where supported.
- Install optional speech engines.
- Import compatible voices or RVC models.
- Test the selected voice.
- Open the custom speech replacements file.
- Save your settings.

SPEECH ENGINES
--------------
Windows SAPI
  Uses voices already installed in Windows.

Supertonic
  Optional engine that can be installed from the F10 settings window.

Piper
  Optional engine that can be installed from the F10 settings window.

Piper + RVC
  Uses Piper for base speech and RVC for voice conversion.
  Compatible RVC models can be imported from the F10 settings window.
  Supported NVIDIA systems can use GPU acceleration automatically; CPU fallback is available.

DATA STORAGE
------------
Settings are stored in:
  City Car Driving\bin\win32\CCD_TTS.ini

Downloaded engines, imported voices and models, logs, and generated audio are stored in:
  City Car Driving\bin\win32\CCD_TTS\

This keeps the TTS installation together with the game files.

CUSTOM SPEECH REPLACEMENTS
--------------------------
Click "Edit Replacements" in F10 to create/open:
  City Car Driving\bin\win32\CCD_TTS\replacements.txt

Each line uses: original text = spoken text
For example: Button:2 = B Button
Lines beginning with # or ; are comments. Leave the right side blank to omit a phrase.
Rules match case-insensitively, use longer matches first, and reload automatically
when the file is saved. Rules are applied once before the built-in speech cleanup.
Only spoken text changes; the game display is unaffected.

SPEECH CLEANUP
--------------
CCD_TTS applies generic speech-only text cleanup before sending text to the selected voice.
This can improve how controls, numeric ranges, measurements, speeds, percentages, durations, and similar values are spoken.

Examples:
  Button:5       -> Button 5
  1-2            -> 1 to 2
  100 m          -> 100 meters
  40-60 km/h     -> 40 to 60 kilometers per hour
  20-30%         -> 20 to 30 percent
  1-2 sec.       -> 1 to 2 seconds
  3500 RPM       -> 3500 R P M

These changes affect speech only. The text shown by the game is not modified.

LOGS
----
Runtime logs are stored in:
  City Car Driving\bin\win32\CCD_TTS\

If an optional engine fails to install or start, use the Install Log button in the F10 settings window for more information.

NOTES
-----
- Optional engines are downloaded only when requested.
- Imported voices and models remain stored with the game installation.
- Child speech-engine processes are closed with the game.
- The City Car Driving installation folder must be writable for settings, logs, downloads, and imported models to be saved.
