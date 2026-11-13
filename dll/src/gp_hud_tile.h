// gp_hud_tile.h - pure logic of the [minimap] tile packs (<map>.mmp, written by hud/bake/minimap_bake.cpp; no Direct3D, no
// game memory, unit tested by tests/dll/test_hud.cpp): the pack header, which level and which tiles a radar needs, the
// decode of one tile (8-bit palette PNG) and its mip chain.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hud {

const int PACK_MAX_LEVELS = 4;
struct PackLevel { int cols, rows; uint32_t table; float tile_m; };   // table: file offset of cols * rows (offset, size)
struct PackHead { float x0, z1, texel; int tile_px, border, levels; PackLevel lv[PACK_MAX_LEVELS]; };
const size_t PACK_HEAD_BYTES = 32 + 12 * PACK_MAX_LEVELS;             // read this much
// false when it is not a pack this DLL can draw: magic, version, 512 px tiles, sizes
bool parse_pack_head(const uint8_t* d, size_t n, PackHead* h);

// level 0 is drawn for about 3 pixels per metre, level 1 for about 2: the finest level that is not minified much
int pick_level(const PackHead& h, float metres_per_pixel);
// the paper drawn behind the radar's background: its parchment lets the world through, and bright walls behind it read as map
uint32_t paper_under(uint32_t colour);

// a tile's inner rectangle in world space (north = max z)
struct TileRect { float x0, z0, x1, z1; };
TileRect tile_rect(const PackHead& h, int level, int col, int row);

// the tiles of a level within `radius` metres of (px, pz), nearest first; at most cap
struct TileRef { int col, row; float dist; };
int tiles_near(const PackHead& h, int level, float px, float pz, float radius, TileRef* out, int cap);

// texels of a full mip chain of a w x w texture (w a power of two)
size_t mip_chain_texels(int w);
// one tile: 8-bit palette PNG, w x w, filter 0, one IDAT -> ARGB (D3DFMT_A8R8G8B8). scratch holds (w + 1) * w bytes.
bool decode_tile(const uint8_t* png, size_t n, int w, uint32_t* argb, uint8_t* scratch);
// fills the levels after the first (they follow each other in `chain`) with an alpha-weighted box filter
void build_mips(uint32_t* chain, int w);

}  // namespace hud
