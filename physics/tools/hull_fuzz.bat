@echo off
rem hull_fuzz.bat - builds physics\tools\hull_fuzz.cpp twice as 32-bit x86 (same flags as dll\build.bat):
rem   build\hull_fuzz\hull_fuzz_orig_%U%.exe     with the ORIGINAL Box3D hull.c (git show bc37735:third_party/box3d/src/hull.c)
rem   build\hull_fuzz\hull_fuzz_new_%U%.exe  with the vendored, patched hull.c ("hull_hang")
rem and runs a seed range through both. usage: hull_fuzz.bat [first_seed] [last_seed] [timeout_ms]
rem The original may hang on some inputs (they are reported and saved as build\hull_fuzz\hang\hang_<seed>.pts); the
rem patched build must never hang, and wherever the original returns the patched one must return the same hull.
setlocal
set ROOT=%~dp0..\..
for %%I in ("%ROOT%") do set ROOT=%%~fI
set S=%ROOT%\third_party\box3d
set O=%ROOT%\build\hull_fuzz
set A=%1
set Z=%2
set T=%3
if "%A%"=="" set A=0
if "%Z%"=="" set Z=6000
if "%T%"=="" set T=400
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
if not exist "%O%\common" mkdir "%O%\common"
if not exist "%O%\orig" mkdir "%O%\orig"
if not exist "%O%\patched" mkdir "%O%\patched"
if not exist "%O%\hang" mkdir "%O%\hang"
if not exist "%O%\hang_patched" mkdir "%O%\hang_patched"
rem the original hull.c next to the other sources so its includes resolve
git -C "%ROOT%" show bc37735:third_party/box3d/src/hull.c > "%O%\hull_orig.c"
if errorlevel 1 ( echo cannot read the original hull.c from git & exit /b 1 )
copy /y "%S%\src\*.h" "%O%\" >nul
set CC=/nologo /c /std:c17 /O2 /MT /W3 /DNDEBUG /DBOX3D_DISABLE_AVX2 /I"%S%\include" /I"%S%\src"
rem a fresh exe name per run: Windows reputation checks sometimes refuse to start a rebuilt unsigned exe of a name it has seen
set U=%RANDOM%
del "%O%\hull_fuzz_orig_*.exe" "%O%\hull_fuzz_new_*.exe" 2>nul
if not exist "%O%\common\core.obj" (
  rem every Box3D source once; hull.obj is dropped, the two variants are built below
  cl %CC% /Fo"%O%\common\\" "%S%\src\*.c" > "%O%\cl_common.log" 2>&1
  if errorlevel 1 ( type "%O%\cl_common.log" & echo BUILD FAILED common & exit /b 1 )
  del "%O%\common\hull.obj"
)
cl %CC% /Fo"%O%\orig\hull.obj" "%O%\hull_orig.c" > "%O%\cl_orig.log" 2>&1
if errorlevel 1 ( type "%O%\cl_orig.log" & echo BUILD FAILED orig & exit /b 1 )
cl %CC% /Fo"%O%\patched\hull.obj" "%S%\src\hull.c" > "%O%\cl_patched.log" 2>&1
if errorlevel 1 ( type "%O%\cl_patched.log" & echo BUILD FAILED patched & exit /b 1 )
set CF=/nologo /std:c++17 /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I"%S%\include" /I"%ROOT%\physics\code"
cl %CF% /Fo"%O%\orig\\" "%~dp0hull_fuzz.cpp" "%ROOT%\physics\code\phys_logic.cpp" "%O%\orig\hull.obj" "%O%\common\*.obj" /Fe"%O%\hull_fuzz_orig_%U%.exe" /link /MAP:"%O%\hull_fuzz_orig.map" > "%O%\cl_fuzz_orig.log" 2>&1
if errorlevel 1 ( type "%O%\cl_fuzz_orig.log" & echo BUILD FAILED fuzz orig & exit /b 1 )
cl %CF% /Fo"%O%\patched\\" "%~dp0hull_fuzz.cpp" "%ROOT%\physics\code\phys_logic.cpp" "%O%\patched\hull.obj" "%O%\common\*.obj" /Fe"%O%\hull_fuzz_new_%U%.exe" /link /MAP:"%O%\hull_fuzz_new.map" > "%O%\cl_fuzz_patched.log" 2>&1
if errorlevel 1 ( type "%O%\cl_fuzz_patched.log" & echo BUILD FAILED fuzz patched & exit /b 1 )
echo === original hull.c, seeds %A%..%Z%
"%O%\hull_fuzz_orig_%U%.exe" "%O%\res_orig.txt" %A% %Z% %T% "%O%\hang"
echo === patched hull.c, seeds %A%..%Z%
"%O%\hull_fuzz_new_%U%.exe" "%O%\res_patched.txt" %A% %Z% %T% "%O%\hang_patched"
if errorlevel 1 ( echo PATCHED BUILDER HUNG & exit /b 1 )
echo === compare (rows where the original did not hang)
if "%PY%"=="" set PY=python
"%PY%" "%~dp0hull_fuzz_compare.py" "%O%\res_orig.txt" "%O%\res_patched.txt" || exit /b 1
echo === replay of the saved inputs that hang the original (hull_hang_cases\*.pts)
for %%F in ("%~dp0hull_hang_cases\*.pts") do (
  "%O%\hull_fuzz_orig_%U%.exe" "%O%\replay.txt" --file "%%F" 400 >nul
  if not errorlevel 1 ( echo the ORIGINAL hull.c did not hang on %%~nxF & exit /b 1 )
  "%O%\hull_fuzz_new_%U%.exe" "%O%\replay.txt" --file "%%F" 400 >nul
  if errorlevel 1 ( echo the PATCHED hull.c hung or crashed on %%~nxF & exit /b 1 )
  echo   %%~nxF: original hangs, patched returns
)
echo HULL FUZZ OK
exit /b 0
