@echo off
rem Build the Asteroids Deluxe C port for the Teensy 4.1 backend:
rem   build_teensy.bat          stage the core, compile (hex under astdelux_teensy\build\)
rem   build_teensy.bat upload   ... and upload to the Teensy on USB
rem Needs arduino-cli with the PJRC Teensy package (README.md).  816 MHz,
rem "Fastest" (-O3), USB type Serial - the options vstcm's firmware uses.
setlocal
cd /d "%~dp0"
set CLI=arduino-cli
where %CLI% >nul 2>nul || set CLI=%LOCALAPPDATA%\Programs\arduino-cli\arduino-cli.exe
if not exist "%CLI%" (
  echo arduino-cli not found on PATH or in %LOCALAPPDATA%\Programs\arduino-cli
  exit /b 1
)
python stage.py || exit /b 1
"%CLI%" compile --fqbn teensy:avr:teensy41:usb=serial,speed=816,opt=o3std ^
   --warnings default --export-binaries astdelux_teensy || exit /b 1
if /i "%~1"=="upload" (
  "%CLI%" upload --fqbn teensy:avr:teensy41:usb=serial,speed=816,opt=o3std astdelux_teensy || exit /b 1
)
echo astdelux_teensy OK
endlocal
