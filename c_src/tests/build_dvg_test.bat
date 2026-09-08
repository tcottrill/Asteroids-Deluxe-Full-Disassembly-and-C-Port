@echo off
rem Build and run dvg_test.exe: a standalone regression/differential test
rem for dvg.c (the DVG state-machine model), linking ..\dvg.c and
rem ..\astdelux_rom.c against dvg_ref.c (the pre-state-machine float
rem walker, kept only as the differential test's reference) and this
rem test's own stubs (dvg_test.c) - no other core module needed.
rem Run from anywhere; output lands here in tests\.
rem
rem Needs MSVC; adjust the path below if your install differs.
setlocal
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)
cd /d "%~dp0"
if not exist obj mkdir obj

if not exist astdelux_test.exe call build_test.bat || exit /b 1

rem Regenerate the four dumps every run (--dumpall records one 2304-byte
rem zero-page+vector-RAM sample per DVG kick; see host_stub.c).
.\astdelux_test.exe 600 --dumpall attract.dump || exit /b 1
.\astdelux_test.exe 1500 --start --fire --thrust --rotl --shield --dumpall play.dump || exit /b 1
.\astdelux_test.exe 1500 --start --fire --rotl --dumpall play_noshield.dump || exit /b 1
.\astdelux_test.exe 300 --test --dumpall stest.dump || exit /b 1

cl /nologo /W4 /std:c11 /O2 /I.. /Fo:obj\ /Fe:dvg_test.exe ^
   ..\dvg.c ..\astdelux_rom.c dvg_ref.c dvg_test.c || exit /b 1
.\dvg_test.exe
exit /b %errorlevel%
