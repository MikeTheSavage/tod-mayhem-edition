@echo off
rem build_test_rd.bat - quick build of the ragdoll unit tests alone (dll\build.bat runs them too).
setlocal
set ROOT=%~dp0..\..
for %%I in ("%ROOT%") do set ROOT=%%~fI
set B=%ROOT%\build
set B3=%ROOT%\third_party\box3d
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul
if not exist "%B%\obj" mkdir "%B%\obj"
if not exist "%B%\box3d_x86.lib" (
  if not exist "%B%\obj\box3d" mkdir "%B%\obj\box3d"
  cl /nologo /c /std:c17 /O2 /MT /W3 /DNDEBUG /DBOX3D_DISABLE_AVX2 /I"%B3%\include" /I"%B3%\src" /Fo"%B%\obj\box3d\\" "%B3%\src\*.c" > "%B%\obj\box3d\cl.log" 2>&1 || ( type "%B%\obj\box3d\cl.log" & exit /b 1 )
  lib /nologo /out:"%B%\box3d_x86.lib" "%B%\obj\box3d\*.obj" || exit /b 1
)
cl /nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"%B3%\include" /Fo"%B%\obj\\" "%ROOT%\ragdoll\code\test_rd.cpp" "%ROOT%\ragdoll\code\rd_logic.cpp" "%ROOT%\ragdoll\code\rd_sim.cpp" "%ROOT%\physics\code\phys_logic.cpp" "%ROOT%\physics\code\phys_sim.cpp" "%B%\box3d_x86.lib" /Fe"%B%\test_rd.exe" || exit /b 1
"%B%\test_rd.exe"
