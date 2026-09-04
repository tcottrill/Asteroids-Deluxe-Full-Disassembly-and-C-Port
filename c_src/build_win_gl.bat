@echo off
rem Build the Asteroids Deluxe C port with the Windows backend (x64):
rem astdelux_win.exe, a window drawing the display list with OpenGL.
rem The vendored framework files (from the Omega Race port) compile at
rem /W3 verbatim; our code stays /W4 /std:c11.
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -no_logo
)
cd /d "%~dp0"

if not exist obj mkdir obj
cl /nologo /W3 /MD /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /c ^
   platform\windows\sys_gl.c platform\windows\glew.c ^
   platform\windows\log.c platform\windows\vector_draw.c ^
   platform\windows\mat4.c platform\windows\rawinput.c ^
   platform\windows\mixer.c platform\windows\fileio.c ^
   platform\windows\miniz.c platform\windows\ini.c ^
   platform\windows\joystick.c ^
   /Foobj\ || exit /b 1

cl /nologo /W4 /std:c11 /MD /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Foobj\ ^
   app_win.c dvg.c platform\windows\plat_win.c ^
   mainline.c vgutil.c message.c mathrom.c frame.c objects.c draw.c ^
   player.c enemy.c score.c sound.c nmi.c earom.c stest.c pokey.c er2055.c astdelux_rom.c ^
   obj\sys_gl.obj obj\glew.obj obj\log.obj ^
   obj\vector_draw.obj obj\mat4.obj obj\rawinput.obj ^
   obj\mixer.obj obj\fileio.obj obj\miniz.obj ^
   obj\ini.obj obj\joystick.obj ^
   /Fe:astdelux_win.exe ^
   /link user32.lib gdi32.lib winmm.lib || exit /b 1
echo astdelux_win.exe OK
