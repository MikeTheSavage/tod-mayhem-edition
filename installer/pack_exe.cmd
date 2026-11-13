@echo off
rem Packs installer\app.py into one file, "TOD Mayhem Installer.exe" (PyInstaller --onefile --noconsole).
rem
rem   run from the repo root (or from a clean export of it: git archive HEAD | tar -x -C <dir>)
rem   set PACK_PY=<python with: pip install soundfile numpy pyinstaller>   (a venv OUTSIDE the repo)
rem   set PACK_OUT=<folder for dist\, build\ and the .spec>               (default: .\pack_out)
rem
rem What goes in (everything else of the repo stays out): the installer page and code, the data builder (data\), the
rem DLLs and ini of dist\, the Suave clips, the HD HUD art, the HD effects payload + the TOD HD d3d9.dll, the minimap
rem bake, the seamless tools (merge_ini, prefetch table, the vendored transitions.py) and the Suave song FINGERPRINT
rem (suave\audio\fingerprint.json: a coarse loudness envelope, not audio). NOT in it: suave\audio\build (the song
rem segments), any mp3 / wav / flac, hdfx\tests, the hdfx dev tools, hdfx\release\tod_hd_fx.ini, any game
rem archive. soundfile brings its own libsndfile DLL (LGPL), numpy its OpenBLAS.
setlocal
if "%PACK_PY%"=="" set PACK_PY=python
if "%PACK_OUT%"=="" set PACK_OUT=%CD%\pack_out
set R=%CD%
"%PACK_PY%" -m PyInstaller --noconfirm --clean --onefile --noconsole --name "TOD Mayhem Installer" ^
  --distpath "%PACK_OUT%\dist" --workpath "%PACK_OUT%\build" --specpath "%PACK_OUT%" ^
  --paths "%R%\data" --paths "%R%\data\lib" --paths "%R%\seamless\tools" --paths "%R%\suave\audio" ^
  --paths "%R%\hdfx\tools" --paths "%R%\hud\tools" --paths "%R%\hud\tools\lib" --paths "%R%\installer" ^
  --add-data "%R%\installer\web;installer\web" ^
  --add-data "%R%\dist\TODGameplay.dll;dist" --add-data "%R%\dist\dsound.dll;dist" --add-data "%R%\dist\TODPhysics.dll;dist" ^
  --add-data "%R%\dist\TODGameplay.ini;dist" --add-data "%R%\dist\TODMinimapBake.exe;dist" ^
  --add-data "%R%\dist\TODGameplay.map;dist" --add-data "%R%\dist\TODPhysics.map;dist" ^
  --add-data "%R%\dist\TODGameplay_mayhem.dds;dist" --add-data "%R%\dist\TODGameplay_mayhem_fx.dds;dist" ^
  --add-data "%R%\dist\TODGameplay_mayhem_fire.dds;dist" ^
  --add-data "%R%\hud_textures\art;hud_textures\art" --add-data "%R%\hud\tools\bake_minimap.py;hud\tools" ^
  --add-data "%R%\hud\tools\minimap_geo.py;hud\tools" --add-data "%R%\hud\tools\lib;hud\tools\lib" ^
  --add-data "%R%\hdfx\release\d3d9.dll;hdfx\release" --add-data "%R%\hdfx\payload;hdfx\payload" ^
  --add-data "%R%\hdfx\tools\hdfx_stage.py;hdfx\tools" --add-data "%R%\hdfx\tools\hdfx_regen.py;hdfx\tools" ^
  --add-data "%R%\hdfx\tools\hdfx_gamedata.py;hdfx\tools" ^
  --add-data "%R%\data;data" --add-data "%R%\seamless\tools;seamless\tools" ^
  --add-data "%R%\suave\audio\fingerprint.json;suave\audio" --add-data "%R%\suave\anim\clips\game;suave\anim\clips\game" ^
  --hidden-import gameplay.builder --hidden-import gameplay.suave_data --hidden-import gameplay.suave_anims ^
  --hidden-import gameplay.hud_textures --hidden-import gameplay.seamless --hidden-import gameplay.menus ^
  --hidden-import gameplay.bigger_fights --hidden-import gameplay.options --hidden-import gameplay.gamefs ^
  --hidden-import gameplay.savecheck --hidden-import install_gameplay_mod --hidden-import merge_ini ^
  --hidden-import make_prefetch_table --hidden-import catalog_rerun --hidden-import transitions ^
  --hidden-import hdfx_stage --hidden-import hdfx_regen --hidden-import hdfx_gamedata --hidden-import bake_minimap ^
  --hidden-import minimap_geo --hidden-import todcoll --hidden-import song_import --hidden-import ogginfo ^
  --hidden-import soundfile --hidden-import numpy ^
  --exclude-module tkinter --exclude-module PIL --exclude-module matplotlib --exclude-module scipy ^
  --exclude-module pytest --exclude-module IPython ^
  installer\app.py
endlocal
