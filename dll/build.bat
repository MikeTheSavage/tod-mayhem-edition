@echo off
rem build.bat - builds TODGameplay.dll + the dsound.dll loader (x86, MSVC v145 / VS 18), runs the unit tests, the
rem non-game test host and the static site check, then copies the results to dist\.
rem Build output: the build folder in the repo root. Never starts the game. Set PY (Python 3) and TODEXE (the GOG TOD.exe) in the environment to override the defaults.
setlocal
set ROOT=%~dp0..
for %%I in ("%ROOT%") do set ROOT=%%~fI
set B=%ROOT%\build
if "%PY%"=="" set PY=python
if "%TODEXE%"=="" set TODEXE=C:\GOG Games\Total Overdose\TOD.exe
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
if not exist "%B%" mkdir "%B%"
if not exist "%B%\host" mkdir "%B%\host"
if not exist "%B%\obj" mkdir "%B%\obj"
set CF=/nologo /std:c++17 /O2 /MT /EHa /W3 /GS /D_CRT_SECURE_NO_WARNINGS

echo === Box3D (third_party\box3d, pinned, C17, x86, SSE2 path) + TODPhysics.dll ([physics], loaded only when enabled)
set B3=%ROOT%\third_party\box3d
if not exist "%B%\obj\box3d" mkdir "%B%\obj\box3d"
cl /nologo /c /std:c17 /O2 /MT /W3 /DNDEBUG /DBOX3D_DISABLE_AVX2 /I"%B3%\include" /I"%B3%\src" /Fo"%B%\obj\box3d\\" "%B3%\src\*.c" > "%B%\obj\box3d\cl.log" 2>&1
if errorlevel 1 ( type "%B%\obj\box3d\cl.log" & echo BUILD FAILED: Box3D & exit /b 1 )
lib /nologo /out:"%B%\box3d_x86.lib" "%B%\obj\box3d\*.obj"
if errorlevel 1 ( echo BUILD FAILED: box3d_x86.lib & exit /b 1 )
cl %CF% /LD /DTODPHYS_DLL /I"%B3%\include" /Fo"%B%\obj\\" "%ROOT%\physics\code\phys_sim.cpp" "%ROOT%\physics\code\phys_car_sim.cpp" "%ROOT%\ragdoll\code\rd_sim.cpp" "%B%\box3d_x86.lib" /Fe"%B%\TODPhysics.dll" /link /MAP:"%B%\TODPhysics.map"
if errorlevel 1 ( echo BUILD FAILED: TODPhysics.dll & exit /b 1 )

echo === TODMinimapBake.exe (the [minimap] tile baker, run by the installer on the player's machine)
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /fp:fast /D_CRT_SECURE_NO_WARNINGS /Fo"%B%\obj\\" "%ROOT%\hud\bake\minimap_bake.cpp" /Fe"%B%\TODMinimapBake.exe" /link /LARGEADDRESSAWARE
if errorlevel 1 ( echo BUILD FAILED: TODMinimapBake.exe & exit /b 1 )

echo === TODGameplay.dll
fxc /nologo /T ps_3_0 /E main /Fh "%B%\obj\gp_mayhem_ps.h" /Vn g_mayhem_ps "%ROOT%\dll\src\gp_mayhem_ps.hlsl" >nul
if errorlevel 1 ( echo BUILD FAILED: gp_mayhem_ps.hlsl & exit /b 1 )
fxc /nologo /T vs_3_0 /E main /Fh "%B%\obj\gp_mayhem_vs.h" /Vn g_mayhem_vs "%ROOT%\dll\src\gp_mayhem_vs.hlsl" >nul
if errorlevel 1 ( echo BUILD FAILED: gp_mayhem_vs.hlsl & exit /b 1 )
rc /nologo /fo "%B%\obj\gp_heat.res" "%ROOT%\dll\src\gp_heat.rc"
if errorlevel 1 ( echo BUILD FAILED: gp_heat.rc & exit /b 1 )
cl %CF% /LD /FAs /Fa"%B%\obj\\" /Fo"%B%\obj\\" "%ROOT%\dll\src\gp_game.cpp" "%ROOT%\dll\src\gp_logic.cpp" "%ROOT%\suave\code\suave_game.cpp" "%ROOT%\suave\code\suave_logic.cpp" "%ROOT%\smart_ai\code\smart_game.cpp" "%ROOT%\smart_ai\code\smart_logic.cpp" "%ROOT%\seamless\code\seam_game.cpp" "%ROOT%\seamless\code\seam_logic.cpp" "%ROOT%\dll\src\gp_debug.cpp" "%ROOT%\dll\src\gp_debug_logic.cpp" "%ROOT%\dll\src\gp_fix.cpp" "%ROOT%\dll\src\gp_display.cpp" "%ROOT%\dll\src\gp_display_logic.cpp" "%ROOT%\dll\src\gp_pad.cpp" "%ROOT%\dll\src\gp_pad_logic.cpp" "%ROOT%\dll\src\gp_pad_prompts.cpp" "%ROOT%\dll\src\gp_glyphs.cpp" "%ROOT%\physics\code\phys_game.cpp" "%ROOT%\physics\code\phys_logic.cpp" "%ROOT%\dll\src\gp_menu.cpp" "%ROOT%\dll\src\gp_difficulty.cpp" "%ROOT%\dll\src\gp_difficulty_logic.cpp" "%ROOT%\dll\src\gp_mayhem.cpp" "%ROOT%\dll\src\gp_mayhem_logic.cpp" "%ROOT%\dll\src\gp_display_modes.cpp" "%ROOT%\psycho\code\psycho_game.cpp" "%ROOT%\psycho\code\psycho_logic.cpp" "%ROOT%\drops\code\drops_game.cpp" "%ROOT%\drops\code\drops_logic.cpp" "%ROOT%\gang_wars\code\gw_game.cpp" "%ROOT%\gang_wars\code\gw_logic.cpp" "%ROOT%\dll\src\gp_overlay.cpp" "%ROOT%\dll\src\gp_hud.cpp" "%ROOT%\dll\src\gp_hud_logic.cpp" "%ROOT%\dll\src\gp_hud_tile.cpp" "%ROOT%\dll\src\gp_hud_tiles.cpp" "%ROOT%\dll\src\gp_hudstage.cpp" "%ROOT%\dll\src\gp_heat.cpp" "%ROOT%\dll\src\gp_heat_logic.cpp" "%ROOT%\dll\src\gp_bigfights.cpp" "%ROOT%\dll\src\gp_bigfights_logic.cpp" "%ROOT%\mayhem\code\mh_game.cpp" "%ROOT%\mayhem\code\mh_logic.cpp" "%ROOT%\mayhem\code\mh_draw.cpp" "%ROOT%\physics\code\car_game.cpp" "%ROOT%\physics\code\car_logic.cpp" "%ROOT%\ragdoll\code\rd_game.cpp" "%ROOT%\ragdoll\code\rd_logic.cpp" "%ROOT%\dll\src\gp_hang.cpp" "%ROOT%\dll\src\gp_hang_logic.cpp" /Fe"%B%\TODGameplay.dll" /I"%B%\obj" /link "%B%\obj\gp_heat.res" /MAP:"%B%\TODGameplay.map" winmm.lib user32.lib gdiplus.lib hid.lib setupapi.lib shell32.lib ole32.lib
if errorlevel 1 ( echo BUILD FAILED: TODGameplay.dll & exit /b 1 )

echo === dsound.dll (loader)
cl %CF% /LD /Fo"%B%\obj\\" "%ROOT%\dll\loader\dsound_proxy.cpp" /Fe"%B%\dsound.dll" /link /DEF:"%ROOT%\dll\loader\dsound.def"
if errorlevel 1 ( echo BUILD FAILED: dsound.dll & exit /b 1 )

echo === unit tests
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_bigfights.cpp" "%ROOT%\dll\src\gp_bigfights_logic.cpp" /Fe"%B%\test_bigfights.exe"
if errorlevel 1 ( echo BUILD FAILED: test_bigfights & exit /b 1 )
"%B%\test_bigfights.exe"
if errorlevel 1 ( echo BIGGER FIGHTS UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_logic.cpp" "%ROOT%\dll\src\gp_logic.cpp" /Fe"%B%\test_logic.exe"
if errorlevel 1 ( echo BUILD FAILED: test_logic & exit /b 1 )
"%B%\test_logic.exe"
if errorlevel 1 ( echo UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\suave\code\test_suave.cpp" "%ROOT%\suave\code\suave_logic.cpp" /Fe"%B%\test_suave.exe"
if errorlevel 1 ( echo BUILD FAILED: test_suave & exit /b 1 )
"%B%\test_suave.exe"
if errorlevel 1 ( echo SUAVE UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\smart_ai\code\test_smart.cpp" "%ROOT%\smart_ai\code\smart_logic.cpp" /Fe"%B%\test_smart.exe"
if errorlevel 1 ( echo BUILD FAILED: test_smart & exit /b 1 )
"%B%\test_smart.exe"
if errorlevel 1 ( echo SMART AI UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\gang_wars\code\test_gw.cpp" "%ROOT%\gang_wars\code\gw_logic.cpp" /Fe"%B%\test_gw.exe"
if errorlevel 1 ( echo BUILD FAILED: test_gw & exit /b 1 )
"%B%\test_gw.exe"
if errorlevel 1 ( echo GANG WARS UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\seamless\code\test_seam.cpp" "%ROOT%\seamless\code\seam_logic.cpp" /Fe"%B%\test_seam.exe"
if errorlevel 1 ( echo BUILD FAILED: test_seam & exit /b 1 )
"%B%\test_seam.exe"
if errorlevel 1 ( echo SEAMLESS UNIT TESTS FAILED & exit /b 1 )

cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_debug.cpp" "%ROOT%\dll\src\gp_debug_logic.cpp" "%ROOT%\dll\src\gp_logic.cpp" /Fe"%B%\test_debug.exe"
if errorlevel 1 ( echo BUILD FAILED: test_debug & exit /b 1 )
"%B%\test_debug.exe"
if errorlevel 1 ( echo DEBUG UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_display.cpp" "%ROOT%\dll\src\gp_display_logic.cpp" /Fe"%B%\test_display.exe"
if errorlevel 1 ( echo BUILD FAILED: test_display & exit /b 1 )
"%B%\test_display.exe"
if errorlevel 1 ( echo DISPLAY UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_pad.cpp" "%ROOT%\dll\src\gp_pad_logic.cpp" /Fe"%B%\test_pad.exe"
if errorlevel 1 ( echo BUILD FAILED: test_pad & exit /b 1 )
"%B%\test_pad.exe"
if errorlevel 1 ( echo PAD UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_pad_prompts.cpp" "%ROOT%\dll\src\gp_pad_prompts.cpp" "%ROOT%\dll\src\gp_pad_logic.cpp" /Fe"%B%\test_pad_prompts.exe"
if errorlevel 1 ( echo BUILD FAILED: test_pad_prompts & exit /b 1 )
"%B%\test_pad_prompts.exe"
if errorlevel 1 ( echo PAD PROMPT UNIT TESTS FAILED & exit /b 1 )
if not exist "%ROOT%\tests\dll\review\pad" mkdir "%ROOT%\tests\dll\review\pad"
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_glyphs.cpp" "%ROOT%\dll\src\gp_glyphs.cpp" "%ROOT%\dll\src\gp_pad_logic.cpp" /Fe"%B%\test_glyphs.exe" /link gdiplus.lib user32.lib
if errorlevel 1 ( echo BUILD FAILED: test_glyphs & exit /b 1 )
"%B%\test_glyphs.exe" "%ROOT%\tests\dll\review\pad"
if errorlevel 1 ( echo GLYPH TESTS FAILED & exit /b 1 )

cl %CF% /I"%B3%\include" /Fo"%B%\obj\\" "%ROOT%\physics\code\test_phys.cpp" "%ROOT%\physics\code\phys_logic.cpp" "%ROOT%\physics\code\phys_sim.cpp" "%B%\box3d_x86.lib" /Fe"%B%\test_phys.exe"
if errorlevel 1 ( echo BUILD FAILED: test_phys & exit /b 1 )
"%B%\test_phys.exe"
if errorlevel 1 ( echo PHYSICS UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\mayhem\code\test_mh.cpp" "%ROOT%\mayhem\code\mh_logic.cpp" /Fe"%B%\test_mh.exe"
if errorlevel 1 ( echo BUILD FAILED: test_mh & exit /b 1 )
"%B%\test_mh.exe"
if errorlevel 1 ( echo MAYHEM PROPS UNIT TESTS FAILED & exit /b 1 )
cl %CF% /I"%B3%\include" /Fo"%B%\obj\\" "%ROOT%\physics\code\test_car.cpp" "%ROOT%\physics\code\car_logic.cpp" "%ROOT%\physics\code\phys_logic.cpp" "%ROOT%\physics\code\phys_sim.cpp" "%ROOT%\physics\code\phys_car_sim.cpp" "%B%\box3d_x86.lib" /Fe"%B%\test_car.exe"
if errorlevel 1 ( echo BUILD FAILED: test_car & exit /b 1 )
"%B%\test_car.exe"
if errorlevel 1 ( echo CAR PHYSICS UNIT TESTS FAILED & exit /b 1 )
cl %CF% /I"%B3%\include" /Fo"%B%\obj\\" "%ROOT%\ragdoll\code\test_rd.cpp" "%ROOT%\ragdoll\code\rd_logic.cpp" "%ROOT%\ragdoll\code\rd_sim.cpp" "%ROOT%\physics\code\phys_logic.cpp" "%ROOT%\physics\code\phys_sim.cpp" "%B%\box3d_x86.lib" /Fe"%B%\test_rd.exe"
if errorlevel 1 ( echo BUILD FAILED: test_rd & exit /b 1 )
"%B%\test_rd.exe"
if errorlevel 1 ( echo RAGDOLL UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_difficulty.cpp" "%ROOT%\dll\src\gp_difficulty_logic.cpp" /Fe"%B%\test_difficulty.exe"
if errorlevel 1 ( echo BUILD FAILED: test_difficulty & exit /b 1 )
"%B%\test_difficulty.exe"
if errorlevel 1 ( echo DIFFICULTY UNIT TESTS FAILED & exit /b 1 )

cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_mayhem.cpp" "%ROOT%\dll\src\gp_mayhem_logic.cpp" /Fe"%B%\test_mayhem.exe"
if errorlevel 1 ( echo BUILD FAILED: test_mayhem & exit /b 1 )
"%B%\test_mayhem.exe"
if errorlevel 1 ( echo MAYHEM UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\psycho\code\test_psycho.cpp" "%ROOT%\psycho\code\psycho_logic.cpp" /Fe"%B%\test_psycho.exe"
if errorlevel 1 ( echo BUILD FAILED: test_psycho & exit /b 1 )
"%B%\test_psycho.exe"
if errorlevel 1 ( echo PSYCHO UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\drops\code\test_drops.cpp" "%ROOT%\drops\code\drops_logic.cpp" /Fe"%B%\test_drops.exe"
if errorlevel 1 ( echo BUILD FAILED: test_drops & exit /b 1 )
"%B%\test_drops.exe"
if errorlevel 1 ( echo DROPS UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_hud.cpp" "%ROOT%\dll\src\gp_hud_logic.cpp" "%ROOT%\dll\src\gp_hud_tile.cpp" /Fe"%B%\test_hud.exe"
if errorlevel 1 ( echo BUILD FAILED: test_hud & exit /b 1 )
"%B%\test_hud.exe"
if errorlevel 1 ( echo HUD UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_hang.cpp" "%ROOT%\dll\src\gp_hang_logic.cpp" /Fe"%B%\test_hang.exe"
if errorlevel 1 ( echo BUILD FAILED: test_hang & exit /b 1 )
"%B%\test_hang.exe" "%B%\TODGameplay.map"
if errorlevel 1 ( echo HANG WATCHDOG UNIT TESTS FAILED & exit /b 1 )
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\test_heat.cpp" "%ROOT%\dll\src\gp_heat_logic.cpp" /Fe"%B%\test_heat.exe"
if errorlevel 1 ( echo BUILD FAILED: test_heat & exit /b 1 )
"%B%\test_heat.exe"
if errorlevel 1 ( echo HEAT UNIT TESTS FAILED & exit /b 1 )

echo === static site check (exe read only)
"%PY%" -I "%ROOT%\tests\dll\check_sites.py"
if errorlevel 1 ( echo SITE CHECK FAILED & exit /b 1 )
"%PY%" -I "%ROOT%\suave\code\tools\check_suave.py"
if errorlevel 1 ( echo SUAVE STATIC CHECK FAILED & exit /b 1 )
"%PY%" -I "%ROOT%\smart_ai\code\tools\check_smart.py"
if errorlevel 1 ( echo SMART AI STATIC CHECK FAILED & exit /b 1 )
if not exist "%ROOT%\tests\dll\review" mkdir "%ROOT%\tests\dll\review"
"%PY%" -I "%ROOT%\tests\dll\check_sites.py" --listing > "%ROOT%\tests\dll\review\sites_listing.txt"
dumpbin /nologo /disasm:nobytes "%B%\TODGameplay.dll" > "%B%\TODGameplay.disasm.txt"
"%PY%" -I "%ROOT%\tests\dll\stub_listing.py" "%B%\TODGameplay.map" "%B%\TODGameplay.disasm.txt" "%ROOT%\tests\dll\review\stubs_disasm.txt"

echo === non-game test host
cl %CF% /Fo"%B%\obj\\" "%ROOT%\tests\dll\host.cpp" /Fe"%B%\host\host.exe" /link /BASE:0x60000000 /DYNAMICBASE:NO /FIXED
if errorlevel 1 ( echo BUILD FAILED: host & exit /b 1 )
copy /y "%B%\TODGameplay.dll" "%B%\host\" >nul
copy /y "%B%\dsound.dll" "%B%\host\" >nul
copy /y "%B%\TODPhysics.dll" "%B%\host\" >nul
if exist "%B%\host\TODGameplay.log" del "%B%\host\TODGameplay.log"
"%B%\host\host.exe" "%TODEXE%" "%B%\host"
if not "%errorlevel%"=="0" ( echo HOST TEST FAILED & exit /b 1 )

echo === dist
if not exist "%ROOT%\dist" mkdir "%ROOT%\dist"
copy /y "%B%\TODGameplay.dll" "%ROOT%\dist\" >nul
copy /y "%B%\dsound.dll" "%ROOT%\dist\" >nul
copy /y "%ROOT%\dll\TODGameplay.ini" "%ROOT%\dist\" >nul
copy /y "%B%\TODPhysics.dll" "%ROOT%\dist\" >nul
copy /y "%B%\TODMinimapBake.exe" "%ROOT%\dist\" >nul
copy /y "%B%\TODGameplay.map" "%ROOT%\dist\" >nul
copy /y "%B%\TODPhysics.map" "%ROOT%\dist\" >nul
for %%F in (TODGameplay_mayhem.dds TODGameplay_mayhem_fx.dds TODGameplay_mayhem_fire.dds) do copy /y "%ROOT%\mayhem_title\art\build\%%F" "%ROOT%\dist\" >nul
echo ALL OK
exit /b 0
