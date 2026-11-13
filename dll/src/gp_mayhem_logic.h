// gp_mayhem_logic.h - the pure part of [mayhem_title]: placement maths, the burn-through timeline and its shader
// constants, the ember / spark particles. No game memory, no Windows or Direct3D calls: unit tested by
// tests/dll/test_mayhem.cpp. The glue that draws it is gp_mayhem.cpp; the per-pixel look is the pixel shader
// gp_mayhem_ps.hlsl (its numpy twin mayhem_title/tools/burn_model.py was used to tune it offline).
//
// Spaces: "art" = pixels of the original title-screen texture (start_loadscreen, 1024 x 512, art in 640 x 512).
// "E" = the effect region: the lettering's bounding box plus margins (mayhem_title/art/build_mayhem_textures.py);
// the lettering texture and the fx texture both cover E exactly, uv 0..1. Distances in E heights.
#pragma once
#include <stdint.h>

namespace mt {

// ---------------------------------------------------------------- 2D affine maps: x' = a x + b y + tx, y' = c x + d y + ty
struct Aff { float a, b, c, d, tx, ty; };
Aff  aff_id();
Aff  aff_mul(const Aff& A, const Aff& B);                     // A after B
bool aff_inv(const Aff& A, Aff* out);
void aff_apply(const Aff& A, float x, float y, float* ox, float* oy);
// a quad: corners p[0] top-left, p[1] bottom-left, p[2] bottom-right, p[3] top-right and their UVs -> UV -> screen
bool uv_to_screen(const float p[4][2], const float uv[4][2], Aff* out);
static const float ART_W = 1024.0f, ART_H = 512.0f;          // UV -> art pixels
Aff  art_to_screen(const Aff& uv_to_scr);                    // art pixels -> screen
// the title picture as the engine's full-screen path 0x45D940 draws it: full back-buffer width, 600/800 of it high
// (600 H / 450 with the console widescreen flag), centred; UV (0,0)-(um, vm)
void title_quad(float W, float H, bool wide_flag, float um, float vm, float P[4][2], float UV[4][2]);

// ---------------------------------------------------------------- the effect region (from the fx texture's header)
struct Region {
    float aspect = 1.5866f;                                  // E width / height
    float bx0 = 0.0537f, by0 = 0.0852f, bx1 = 0.9463f, by1 = 0.9148f;   // the lettering's bbox inside E (fractions)
    float band_px = 40.0f;                                   // the hole field's +-0.5 range in fx pixels
    float fire_base0 = 0.70f, fire_slope = -0.34f, fire_band = 0.32f;   // fire layer: v = base0 + slope u, band height
    bool from_header(const float res[11]);                   // the DDS reserved words written by the build script
};
struct Place { float bx, by, bw; };                          // the lettering's bbox: left, top, width (art px)
Place place_default();                                       // under OVERDOSE, over the shotgun barrels
struct Rect { float x0, y0, w, h; };
Rect e_rect(const Region& r, const Place& p);                // E in art px

// ---------------------------------------------------------------- burn constants (the shader's c1 / c2 / c3)
struct BurnK {
    float iu = 0.5f, iv = 0.5f;                              // ignition in E uv (moved into the hole if outside)
    float kT = 1.0f;                                         // T per E height (the farthest hole pixel burns at 0.82)
    float ke = 0.1f;                                         // hole field (0.5 - R) -> T units
    float rim = 0.01f, curl = 0.02f, scorch = 0.05f;         // T units
    float noise = 0.1f;                                      // ragged front amplitude (T units, x2 in the shader)
    float inv_ramp = 6.25f;                                  // letters: dark -> glowing -> final over 1 / inv_ramp of T
};
// fx: the fx texture's R (hole field) and A (burn noise) channels, fw x fh bytes each; ignition in E uv
bool burn_setup(const uint8_t* R, const uint8_t* A, int fw, int fh, const Region& reg, float ign_u, float ign_v, BurnK* k);
// the burn time of one fx pixel (what the shader computes): distance from the ignition + noise
float burn_time(const BurnK& k, const Region& reg, float u, float v, float noise01);

// ---------------------------------------------------------------- the timeline of one show
struct Timeline {
    float delay = 0.6f, reveal = 2.0f;                       // s: vanilla, then ignition -> full reveal
    float t = -1.0f;                                         // s since the show (negative: not shown)
    void start(bool already_burnt);                          // already_burnt: straight to the end state
    void advance(float dt) { if (t >= 0.0f) t += dt; }
    bool visible() const { return t >= delay; }
    float front() const;                                     // the shader's p: 0.02 .. 1 (fast ignition, slowing down)
    float calm() const;                                      // 0 while burning .. 1 settled (1.2 s after the reveal)
    bool  done() const { return t >= delay + reveal; }
};

// ---------------------------------------------------------------- embers and sparks (E uv; velocities in E heights / s)
struct Spark { float u, v, vu, vv, age, life, size, phase; };
struct Sparks {
    enum { MAX = 96 };
    Spark p[MAX];
    int n = 0;
    uint32_t rng = 7;
    float acc = 0.0f;
    void spawn(float u, float v, float speed, float life);
    void step(float dt, float aspect);                       // buoyancy, drag, wobble, ageing
};
uint32_t spark_colour(const Spark& s, float intensity, float* size_out);   // premultiplied, additive (alpha 0)

// ---------------------------------------------------------------- the fire flipbook (8 x 8 atlas, 48 frames, 12 fps)
void fire_frames(float t, float out_c4[4], float* blend);    // atlas origins of the two frames and their blend

// ---------------------------------------------------------------- helpers
uint32_t xorshift(uint32_t* s);
float    frand(uint32_t* s);                                 // 0..1
float    smoothstep(float a, float b, float x);
uint32_t premul(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void     noise_tile(uint8_t* out, int w, int h, int cell_x, int cell_y, int octaves, uint32_t seed);   // tileable 0..255

}  // namespace mt
