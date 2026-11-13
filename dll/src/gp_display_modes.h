// gp_display_modes.h - [display] mode_menu (gp_display_modes.cpp): windowed / borderless / fullscreen at any size,
// chosen in the game's own SETTINGS > SCREEN RESOLUTION row and applied live. Shared between gp_display.cpp (config,
// install order, the fullscreen mode lookup) and gp_display_modes.cpp. Not part of gp_glue.h: only [display] uses it.
#pragma once
#include <windows.h>
#include "gp_logic.h"

struct DmCfg {
    bool on = false;                 // [display] mode_menu (default 1 with [display] enabled = 1)
    bool write_configpc = true;      // keep configpc.txt / the registry sane for the game without the DLL
    int label_chars = 0;             // value labels longer than this are drawn condensed (0 = what the value box holds)
    bool relayout_hud = true;        // re-run the HUD's own screen-dimension / reposition commands after a change
    // the first start without the side file: configpc.txt and the old [display] borderless key
    bool cfg_fs = false; int cfg_w = 0, cfg_h = 0; bool borderless_ini = false;
};
void  dm_load_config(const gp::Ini& ini, const DmCfg& base);
bool  dm_enabled();
int   dm_install();                                  // number of patched sites (0 = off)
void* dm_test_stub(const char* name);                // test host only

// provided by gp_display.cpp: the fullscreen mode lookup with the any-size fallback (FsLookupC); returns the renderer's
// mode entry {w, h, usable, refresh +0Ch, format +10h} or 0
DWORD display_fs_lookup(DWORD renderer, int w, int h);
