@echo off
rem Build the headless test host, astdelux_test.exe: the whole game
rem against quiet hardware (host_stub.c), for scripted runs and vector-RAM
rem dumps.  Run from anywhere; output lands here in tests\.
rem
rem Needs MSVC; adjust the path below if your install differs.
setlocal
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /W4 /std:c11 /O2 /I.. /Fo:obj\ /Fe:astdelux_test.exe ^
   ..\mainline.c ..\vgutil.c ..\message.c ..\mathrom.c ..\frame.c ..\objects.c ..\draw.c ^
   ..\player.c ..\enemy.c ..\score.c ..\sound.c ..\nmi.c ..\earom.c ..\stest.c ..\c012294.c ..\er2055.c ^
   stubs.c host_stub.c ..\astdelux_rom.c
endlocal
