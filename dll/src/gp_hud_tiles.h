// gp_hud_tiles.h - [minimap]: the map in the radar from a tile pack (gp_hud_tiles.cpp)
#pragma once
#include <d3d9.h>
#include "gp_hud_logic.h"

// inside gp_ov_set2d: the map's tiles around f, inside the disc; map = its index, path = the pack, colour = tint and
// opacity. Never blocks on the disk. Returns false while the pack is not open (or has failed).
bool tiles_draw(IDirect3DDevice9* dev, const hud::Frame& f, float units_per_m, const hud::Disc& disc, int map, const char* path, DWORD colour);
// inside gp_ov_set2d, BEFORE the radar background: the paper behind the parchment (the world does not read as map)
void tiles_underlay(IDirect3DDevice9* dev, const hud::Disc& disc, DWORD colour);
void tiles_device_changed();                 // the old device is gone, and its textures with it
void tiles_log_perf(double seconds);
