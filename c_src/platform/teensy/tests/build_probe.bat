@echo off
rem Build and run the PC probe of the Teensy backend's pure modules
rem (vec_beam.c, audio_mix.c) with MSVC, the same way tests\build_mod.bat
rem probes the game modules.  Output lands here in tests\.
setlocal
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /W4 /std:c11 /O2 /I..\astdelux_teensy /Fo:obj\ /Fe:probe_teensy.exe ^
   probe_teensy.c ..\astdelux_teensy\vec_beam.c ..\astdelux_teensy\audio_mix.c || exit /b 1
"%~dp0probe_teensy.exe"
endlocal
