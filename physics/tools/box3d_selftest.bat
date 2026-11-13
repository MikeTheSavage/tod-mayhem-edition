@echo off
rem box3d_selftest.bat - builds Box3D's own unit tests (third_party\box3d\test) as a 32-bit x86 program with the same
rem flags as dll\build.bat and runs them. Output: <repo>\build\box3d_test. Not part of build.bat (about a minute).
setlocal
set ROOT=%~dp0..\..
for %%I in ("%ROOT%") do set ROOT=%%~fI
set S=%ROOT%\third_party\box3d
set O=%ROOT%\build\box3d_test
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
if not exist "%O%" mkdir "%O%"
cl /nologo /std:c17 /O2 /MT /W3 /DNDEBUG /DBOX3D_DISABLE_AVX2 /I"%S%\include" /I"%S%\src" /I"%S%\shared" /Fo"%O%\\" "%S%\src\*.c" "%S%\test\*.c" "%S%\shared\*.c" /Fe"%O%\box3d_test_x86.exe" > "%O%\cl.log" 2>&1
if errorlevel 1 ( type "%O%\cl.log" & echo BUILD FAILED & exit /b 1 )
pushd "%O%"
"%O%\box3d_test_x86.exe"
set R=%errorlevel%
popd
if not "%R%"=="0" ( echo BOX3D TESTS FAILED & exit /b 1 )
echo BOX3D x86 SELFTEST OK
exit /b 0
