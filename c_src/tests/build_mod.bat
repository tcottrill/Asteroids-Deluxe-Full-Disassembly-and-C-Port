@echo off
rem Build the headless host with one module's probe compiled in.
rem
rem     build_mod.bat objects
rem     build_mod.bat pokey
rem
rem For each name given, AD_HAVE_<name> is defined (a hook stubs.c keeps
rem for staging) and probe_<first>.c, if it exists, is compiled in with
rem AD_PROBE defined; host_stub.c then calls its int ad_probe(void) when
rem the exe is run with --probe.  That is how a module checks a contract
rem in isolation.  Output goes to obj_<first>\ and test_<first>.exe, here
rem in tests\, so several probes can be built side by side.
setlocal enabledelayedexpansion
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)
cd /d "%~dp0"
if "%~1"=="" (
  echo usage: build_mod.bat module [module ...]
  exit /b 1
)
set FIRST=%~1
set DEFS=
set SRCS=
:next
if "%~1"=="" goto build
set DEFS=!DEFS! /DAD_HAVE_%~1
shift
goto next
:build
if exist probe_%FIRST%.c (
  set DEFS=!DEFS! /DAD_PROBE
  set SRCS=!SRCS! probe_%FIRST%.c
)
if not exist obj_%FIRST% mkdir obj_%FIRST%
rem Every module is translated, so the build is always the full game; the
rem names given only pick the probe and the output.
cl /nologo /W4 /std:c11 /O2 /I.. /Fo:obj_%FIRST%\ /Fe:test_%FIRST%.exe %DEFS% ^
   ..\mainline.c ..\vgutil.c ..\message.c ..\mathrom.c ..\frame.c ..\objects.c ..\draw.c ^
   ..\player.c ..\enemy.c ..\score.c ..\sound.c ..\nmi.c ..\earom.c ..\stest.c ..\pokey.c ..\er2055.c ^
   stubs.c host_stub.c ..\astdelux_rom.c %SRCS%
endlocal
