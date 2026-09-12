@echo off
rem The POKEY core's own checks, independent of the game:
rem   build_mod.bat pokey + test_pokey.exe --probe   the conformance probe (MAME-derived tables,
rem                                                  RANDOM, timers, serial, pots, host seam)
rem   this script                                    the mute-phase and AUDCTL-rewrite regressions,
rem                                                  then an A/B hash of c012294.c against a saved
rem                                                  golden copy (obj\c012294_golden.c) plus the
rem                                                  two-chip cost benchmark.
rem Usage: tests\build_pokey.bat [save]     save = refresh the golden copy first.
rem Run after ANY change to c012294.c; the three copies (this tree, Space Duel,
rem the Atari 800 project / AAE) are kept byte-identical.
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -no_logo
)
cd /d "%~dp0.."
if not exist obj mkdir obj
cl /nologo /O2 /W4 /std:c11 /D_CRT_SECURE_NO_WARNINGS /I. /Foobj\ tests\test_pokey_mute_phase.c c012294.c /Fe:tests\test_pokey_mute_phase.exe || exit /b 1
tests\test_pokey_mute_phase.exe || exit /b 1
cl /nologo /O2 /W4 /std:c11 /D_CRT_SECURE_NO_WARNINGS /I. /Foobj\ tests\test_audctl_rewrite.c c012294.c /Fe:tests\test_audctl_rewrite.exe || exit /b 1
tests\test_audctl_rewrite.exe || exit /b 1
if "%1"=="save" copy /y c012294.c obj\c012294_golden.c >nul
if not exist obj\c012294_golden.c copy /y c012294.c obj\c012294_golden.c >nul
cl /nologo /O2 /W4 /std:c11 /D_CRT_SECURE_NO_WARNINGS /I. /Foobj\ tests\probe_c012294_golden.c obj\c012294_golden.c /Fe:tests\golden_a.exe || exit /b 1
cl /nologo /O2 /W4 /std:c11 /D_CRT_SECURE_NO_WARNINGS /I. /Foobj\ tests\probe_c012294_golden.c c012294.c /Fe:tests\golden_b.exe || exit /b 1
tests\golden_a.exe > obj\golden_a.txt || exit /b 1
tests\golden_b.exe > obj\golden_b.txt || exit /b 1
fc obj\golden_a.txt obj\golden_b.txt >nul
if errorlevel 1 (
  echo A/B MISMATCH against obj\c012294_golden.c:
  type obj\golden_b.txt
  exit /b 1
)
echo A/B identical:
type obj\golden_b.txt
cl /nologo /O2 /W4 /std:c11 /D_CRT_SECURE_NO_WARNINGS /I. /Foobj\ tests\probe_c012294_bench.c c012294.c /Fe:tests\bench.exe || exit /b 1
tests\bench.exe
exit /b 0
