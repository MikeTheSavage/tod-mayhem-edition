// gp_hud_draw.h - the 2D drawing helpers of [minimap] (gp_hud.cpp): one UI texture with the marker shapes and the digit
// glyphs (GDI+, built once per device), quads in pixels through the game's device.
// Call them between gp_ov_save / gp_ov_set2d and gp_ov_restore.
#pragma once
#include <windows.h>
#include <d3d9.h>
namespace hudd {
enum Cell { C_DIAMOND_O = 0, C_DIAMOND_F, C_ARROW_O, C_ARROW_F, C_GLOW };
bool ensure_ui(IDirect3DDevice9* dev);                 // the UI texture on this device (false: do not draw)
void cell_quad(IDirect3DDevice9* d, int cell, float x, float y, float size, float ang, DWORD col);   // UI texture set by the caller
float text_width(const char* s, float h);
void text(IDirect3DDevice9* d, const char* s, float x, float y, float h, DWORD col);
DWORD argb_mul(DWORD c, float a);
}
