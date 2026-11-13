// mh_draw.h - the [mayhem] debris renderer (mayhem/code/mh_draw.cpp): textured boxes and dust puffs drawn with the
// game's device in its world pass (before the frame-end motion blur, under the HUD).
#pragma once
#include <windows.h>

enum { MH_WOOD = 0, MH_METAL = 1, MH_RED = 2, MH_CHAR = 3, MH_MATERIALS = 4 };   // procedural fallbacks (MH_RED: style 0 only)
enum { MH_DRAW_MAX = 512, MH_PUFF_MAX = 48 };

struct MhPiece {
    float pos[3];      // world centre
    float rot[4];      // TRUE rotation (x, y, z, w)
    float half[3];     // half extents (m)
    float scale;       // 1 = full size; shrinks to 0 at the end of the piece's life
    float tint;        // brightness factor of the piece (variety)
    float uv_off;      // style 0: texture offset (variety)
    int   mat;         // MH_WOOD / MH_METAL / MH_RED / MH_CHAR: the procedural texture when tex is null
    void* tex;         // style 1: the broken prop's own IDirect3DTexture9 (held by the debris), else null
    float u0, v0, side;   // style 1: the texture patch the piece shows (UV square), side 0 = the old metre tiling
};
struct MhPuff {        // a dust puff: camera-facing, soft
    float pos[3];
    float radius, alpha;
    float rgb[3];
};

// draws the pieces then the puffs; light = overall brightness (1 = daylight); returns the number of pieces drawn;
// *ms = the draw's cost
bool mh_draw_warm();   // makes the debris textures now (call from the world pass before the first break)
int mh_draw_pieces(const MhPiece* pieces, int n, const MhPuff* puffs, int np, float light, double* ms);

// ---- the broken prop's own texture
struct MhTexInfo {
    void* tex;             // IDirect3DTexture9, AddRef'd (release with mh_tex_release)
    int   w, h;            // level 0
    char  name[96];        // the texture asset's name when readable
    float avg[3];          // mean colour of its used texels (0..1)
    int   ncell;           // used cells of the 16 x 16 map below (alpha >= 0.5 and not empty black)
    unsigned char cell[16 * 16];
    char  why[160];        // why no texture was found (log)
};
// the largest texture of a Model node's ModelAsset (+50h -> TextureSets +20h -> TextureAsset +28h -> Texture +4), read
// back once (smallest mip >= 16 x 16) for its mean colour and used area; false = none found (procedural fallback)
bool mh_prop_texture(DWORD model, MhTexInfo* out);
void mh_tex_addref(void* tex);
void mh_tex_release(void* tex);
