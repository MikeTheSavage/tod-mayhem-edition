// minimap_bake.cpp - TODMinimapBake.exe: draws the [minimap] tiles of one TOD map as ink on the radar's parchment.
//
// hud/tools/bake_minimap.py reads the player's own game files (Python, stdlib) and hands this tool the solid collision
// of a map as a triangle soup plus the tree / bush positions. This tool does the pixel work, which Python cannot do at
// this resolution in a reasonable time. No game data is shipped; nothing is read but the input file.
//
//   TODMinimapBake <input.mmb> <out.mmp> <out.occ> [--threads N] [--preview L x z half out.png]
//
// Input (little endian): "MMB1", u32 triangles, u32 marks; triangles: 9 floats + material as float; marks: x, z, r, kind.
//
// Output <map>.mmp, a tile pack (dll/src/gp_hud_logic.h MapPack reads it):
//   "MMP1", u32 version, f32 x0, f32 z1, f32 texel metres of level 0, u32 tile px, u32 border px, u32 levels,
//   per level: u32 cols, u32 rows, u32 offset of the table; table: cols * rows of (u32 offset, u32 size), 0 = empty.
//   Each tile is a PNG: tile px square, of which `border` px on every side repeat the neighbours (so filtering and the
//   mip chain never show a seam). A tile of level L covers (tile px - 2 border) * texel * 2^L metres.
//
// Picture: every tile is rendered from the triangles at S x S sub-samples per texel, so every edge is the triangle's own
// straight edge, anti-aliased. Strokes come from distance fields, so a line width is a number of metres chosen per level
// (level 0 is seen at about 3 pixels per metre, level 1 at about 2). Classes per sub-sample: void (no collision), road
// (asphalt), water, grass, dirt, steep ground, building (a flat top well above the opened local ground), plain ground.
// Straight-alpha ink; plain ground stays transparent so the radar's parchment shows.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

static const int TILE_PX = 512, BORDER_PX = 4, LEVELS = 2;
static const float TEXEL0 = 0.25f;           // metres per texel of level 0
static const float NONE = -1e8f;
enum { M_DIRT = 4, M_BUSH = 7, M_GRASS = 9, M_ASPHALT = 10, M_WATER = 11 };
enum { C_VOID = 0, C_GROUND, C_ROAD, C_GRASS, C_DIRT, C_WATER, C_ROCK, C_BLD };
enum { F_STEEP = 1, F_WALL = 2 };
enum { K_TREE = 1, K_PALM = 2, K_BUSH = 3 };

struct Tri { float a[3], b[3], c[3]; float m; };
struct Mark { float x, z, r, kind; };
static std::vector<Tri> g_tri;
static std::vector<Mark> g_mark;
static std::vector<int> g_flat, g_vert;        // indices: surfaces / vertical faces

// ---------------------------------------------------------------- world grid
static float g_x0, g_z1, g_xw, g_zh;           // map rectangle: north-west corner, size in metres
static int g_gw, g_gh;                         // opened ground on 4 m blocks
static std::vector<float> g_ground;            // -big = no ground known
static const float GROUND_CELL = 4.0f, BIG = 1e9f;
static const float BIN = 64.0f;
static int g_bw, g_bh;
static std::vector<std::vector<int>> g_bin_flat, g_bin_vert, g_bin_mark;

struct Target { float* top; u8* mat; u8* flag; int W, H; float s, ox, oz; };   // column = (x - ox) / s, row = (oz - z) / s

static void raster_flat(const Target& t, const Tri& q) {
    float ax = (q.a[0] - t.ox) / t.s, az = (t.oz - q.a[2]) / t.s;
    float bx = (q.b[0] - t.ox) / t.s, bz = (t.oz - q.b[2]) / t.s;
    float cx = (q.c[0] - t.ox) / t.s, cz = (t.oz - q.c[2]) / t.s;
    float e1x = q.b[0] - q.a[0], e1y = q.b[1] - q.a[1], e1z = q.b[2] - q.a[2];
    float e2x = q.c[0] - q.a[0], e2y = q.c[1] - q.a[1], e2z = q.c[2] - q.a[2];
    float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
    float ln = sqrtf(nx * nx + ny * ny + nz * nz);
    if (ln < 1e-9f) return;
    float flat = fabsf(ny) / ln;
    u8 fl = flat < 0.6f ? (u8)F_STEEP : (u8)0;
    u8 mat = (u8)q.m;
    float d = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
    if (fabsf(d) < 1e-7f) return;
    // height as a plane over (column, row)
    float gx = ((q.b[1] - q.a[1]) * (cz - az) - (q.c[1] - q.a[1]) * (bz - az)) / d;
    float gz = ((q.c[1] - q.a[1]) * (bx - ax) - (q.b[1] - q.a[1]) * (cx - ax)) / d;
    float y0 = q.a[1] - gx * ax - gz * az;
    int r0 = std::max(0, (int)floorf(std::min({az, bz, cz}) - 0.5f));
    int r1 = std::min(t.H - 1, (int)ceilf(std::max({az, bz, cz}) - 0.5f));
    float vx[3] = {ax, bx, cx}, vz[3] = {az, bz, cz};
    for (int r = r0; r <= r1; ++r) {
        float pz = r + 0.5f, l = 1e30f, h = -1e30f;
        for (int i = 0; i < 3; ++i) {
            int j = (i + 1) % 3;
            float z0 = vz[i], z1 = vz[j];
            if ((z0 <= pz && pz < z1) || (z1 <= pz && pz < z0)) {
                float x = vx[i] + (pz - z0) / (z1 - z0) * (vx[j] - vx[i]);
                l = std::min(l, x); h = std::max(h, x);
            }
        }
        if (l > h) continue;
        int c0 = std::max(0, (int)ceilf(l - 0.5f)), c1 = std::min(t.W - 1, (int)floorf(h - 0.5f));
        int base = r * t.W;
        for (int c = c0; c <= c1; ++c) {
            float y = y0 + gx * (c + 0.5f) + gz * pz;
            if (y > t.top[base + c]) { t.top[base + c] = y; t.mat[base + c] = mat; t.flag[base + c] = (u8)((t.flag[base + c] & ~F_STEEP) | fl); }
        }
    }
}
// a vertical face that rises above the surface next to it is a thin wall: its footprint becomes a line
static void raster_wall(const Target& t, const Tri& q) {
    float ymax = std::max({q.a[1], q.b[1], q.c[1]}), ymin = std::min({q.a[1], q.b[1], q.c[1]});
    if (ymax - ymin < 1.0f) return;
    float p[3][2] = {{(q.a[0] - t.ox) / t.s, (t.oz - q.a[2]) / t.s}, {(q.b[0] - t.ox) / t.s, (t.oz - q.b[2]) / t.s}, {(q.c[0] - t.ox) / t.s, (t.oz - q.c[2]) / t.s}};
    int bi = 0, bj = 1; float bl = -1.0f;
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        float l = (p[j][0] - p[i][0]) * (p[j][0] - p[i][0]) + (p[j][1] - p[i][1]) * (p[j][1] - p[i][1]);
        if (l > bl) { bl = l; bi = i; bj = j; }
    }
    int n = (int)(sqrtf(bl) * 1.5f) + 1;
    for (int k = 0; k <= n; ++k) {
        float u = (float)k / n;
        int c = (int)(p[bi][0] + (p[bj][0] - p[bi][0]) * u), r = (int)(p[bi][1] + (p[bj][1] - p[bi][1]) * u);
        if (c < 0 || r < 0 || c >= t.W || r >= t.H) continue;
        int i = r * t.W + c;
        if (t.top[i] < ymax - 0.8f) t.flag[i] |= F_WALL;
    }
}

static void load_input(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    u32 h[3];
    if (fread(h, 4, 3, f) != 3 || memcmp(h, "MMB1", 4)) { fprintf(stderr, "%s: not a bake input\n", path); exit(2); }
    g_tri.resize(h[1]); g_mark.resize(h[2]);
    if (h[1] && fread(g_tri.data(), sizeof(Tri), h[1], f) != h[1]) { fprintf(stderr, "short input\n"); exit(2); }
    if (h[2] && fread(g_mark.data(), sizeof(Mark), h[2], f) != h[2]) { fprintf(stderr, "short input\n"); exit(2); }
    fclose(f);
    for (int i = 0; i < (int)g_tri.size(); ++i) {
        const Tri& q = g_tri[i];
        float e1x = q.b[0] - q.a[0], e1y = q.b[1] - q.a[1], e1z = q.b[2] - q.a[2];
        float e2x = q.c[0] - q.a[0], e2y = q.c[1] - q.a[1], e2z = q.c[2] - q.a[2];
        float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
        float ln = sqrtf(nx * nx + ny * ny + nz * nz);
        if (ln < 1e-9f) continue;
        if (fabsf(ny) / ln < 0.12f) g_vert.push_back(i); else g_flat.push_back(i);
    }
}

// bounds: the 0.3 .. 99.7 % range of the triangle centres plus a margin (far scenery would stretch the rectangle over
// kilometres of nothing); triangles outside are clipped by the tile grid
static void make_bounds() {
    std::vector<float> xs, zs; xs.reserve(g_tri.size()); zs.reserve(g_tri.size());
    for (const Tri& t : g_tri) { xs.push_back((t.a[0] + t.b[0] + t.c[0]) / 3.0f); zs.push_back((t.a[2] + t.b[2] + t.c[2]) / 3.0f); }
    std::sort(xs.begin(), xs.end()); std::sort(zs.begin(), zs.end());
    size_t lo = (size_t)(xs.size() * 0.003), hi = (size_t)(xs.size() * 0.997); if (hi) --hi;
    g_x0 = xs[lo] - 40.0f; float x1 = xs[hi] + 40.0f; float z0 = zs[lo] - 40.0f; g_z1 = zs[hi] + 40.0f;
    g_xw = x1 - g_x0; g_zh = g_z1 - z0;
}

static void make_bins() {
    g_bw = (int)ceilf(g_xw / BIN) + 2; g_bh = (int)ceilf(g_zh / BIN) + 2;
    g_bin_flat.assign((size_t)g_bw * g_bh, {}); g_bin_vert.assign((size_t)g_bw * g_bh, {}); g_bin_mark.assign((size_t)g_bw * g_bh, {});
    auto add = [&](std::vector<std::vector<int>>& bins, int idx) {
        const Tri& t = g_tri[idx];
        float a = std::min({t.a[0], t.b[0], t.c[0]}), b = std::max({t.a[0], t.b[0], t.c[0]});
        float c = std::min({t.a[2], t.b[2], t.c[2]}), d = std::max({t.a[2], t.b[2], t.c[2]});
        int c0 = std::max(0, (int)floorf((a - g_x0) / BIN)), c1 = std::min(g_bw - 1, (int)floorf((b - g_x0) / BIN));
        int r0 = std::max(0, (int)floorf((g_z1 - d) / BIN)), r1 = std::min(g_bh - 1, (int)floorf((g_z1 - c) / BIN));
        for (int r = r0; r <= r1; ++r) for (int k = c0; k <= c1; ++k) bins[(size_t)r * g_bw + k].push_back(idx);
    };
    for (int i : g_flat) add(g_bin_flat, i);
    for (int i : g_vert) add(g_bin_vert, i);
    for (int i = 0; i < (int)g_mark.size(); ++i) {
        const Mark& m = g_mark[i];
        int c = (int)floorf((m.x - g_x0) / BIN), r = (int)floorf((g_z1 - m.z) / BIN);
        if (c >= 0 && r >= 0 && c < g_bw && r < g_bh) g_bin_mark[(size_t)r * g_bw + c].push_back(i);
    }
}

// ---------------------------------------------------------------- the coarse pass (1 m): local ground, occupancy
static void write_occ(const char* path, const std::vector<float>& top, int W, int H, float res) {
    const float cell = 16.0f;
    int cols = (int)ceilf(W * res / cell), rows = (int)ceilf(H * res / cell);
    std::vector<char> occ((size_t)cols * rows, '0');
    for (int r = 0; r < H; ++r) for (int c = 0; c < W; ++c)
        if (top[(size_t)r * W + c] > NONE) occ[(size_t)((int)(r * res / cell)) * cols + (int)(c * res / cell)] = '1';
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb"); if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fprintf(f, "occ %.1f %d %d %.2f %.2f\n", cell, cols, rows, g_x0, g_z1);
    for (int r = 0; r < rows; ++r) { fwrite(&occ[(size_t)r * cols], 1, cols, f); fputc('\n', f); }
    fclose(f);
    remove(path); rename(tmp.c_str(), path);
}
static void coarse_pass(const char* occ_path) {
    const float res = 1.0f;
    int W = (int)ceilf(g_xw / res), H = (int)ceilf(g_zh / res);
    std::vector<float> top((size_t)W * H, NONE); std::vector<u8> mat((size_t)W * H, 255), flag((size_t)W * H, 0);
    Target t = {top.data(), mat.data(), flag.data(), W, H, res, g_x0, g_z1};
    for (int i : g_flat) raster_flat(t, g_tri[i]);
    if (occ_path) write_occ(occ_path, top, W, H, res);
    // an opening (min then max) of the top surface on 4 m blocks follows hills, not buildings
    int B = (int)(GROUND_CELL / res);
    g_gw = (W + B - 1) / B; g_gh = (H + B - 1) / B;
    std::vector<float> cmin((size_t)g_gw * g_gh, BIG);
    for (int r = 0; r < H; ++r) for (int c = 0; c < W; ++c) {
        float v = top[(size_t)r * W + c];
        size_t k = (size_t)(r / B) * g_gw + c / B;
        if (v > NONE && v < cmin[k]) cmin[k] = v;
    }
    const int R = (int)(44.0f / GROUND_CELL);
    auto filt = [&](const std::vector<float>& in, bool want_min) {
        std::vector<float> a = in, o = in;
        for (int y = 0; y < g_gh; ++y) for (int x = 0; x < g_gw; ++x) {
            float v = want_min ? BIG : -BIG;
            for (int k = std::max(0, x - R); k <= std::min(g_gw - 1, x + R); ++k) { float u = in[(size_t)y * g_gw + k]; v = want_min ? std::min(v, u) : std::max(v, u); }
            a[(size_t)y * g_gw + x] = v;
        }
        for (int x = 0; x < g_gw; ++x) for (int y = 0; y < g_gh; ++y) {
            float v = want_min ? BIG : -BIG;
            for (int k = std::max(0, y - R); k <= std::min(g_gh - 1, y + R); ++k) { float u = a[(size_t)k * g_gw + x]; v = want_min ? std::min(v, u) : std::max(v, u); }
            o[(size_t)y * g_gw + x] = v;
        }
        return o;
    };
    std::vector<float> gmin = filt(cmin, true);
    for (float& v : gmin) if (v >= BIG * 0.5f) v = -BIG;
    g_ground = filt(gmin, false);
}

// ---------------------------------------------------------------- noise
static const int NT = 1024;
static std::vector<float> g_noise;           // smooth tileable noise, -1 .. 1, feature size about 48 texels
static void make_noise() {
    g_noise.assign((size_t)NT * NT, 0.0f);
    const int C = 24;
    std::vector<float> c((size_t)C * C);
    u32 s = 0x2F6E2B1u;
    for (float& v : c) { s = s * 1664525u + 1013904223u; v = ((s >> 16) & 0xFFFF) / 32767.5f - 1.0f; }
    auto sm = [](float t) { return t * t * (3.0f - 2.0f * t); };
    for (int y = 0; y < NT; ++y) for (int x = 0; x < NT; ++x) {
        float fx = (float)x * C / NT, fy = (float)y * C / NT;
        int x0 = (int)fx, y0 = (int)fy; float u = sm(fx - x0), v = sm(fy - y0);
        int x1 = (x0 + 1) % C, y1 = (y0 + 1) % C;
        float a = c[(size_t)y0 * C + x0] * (1 - u) + c[(size_t)y0 * C + x1] * u, b = c[(size_t)y1 * C + x0] * (1 - u) + c[(size_t)y1 * C + x1] * u;
        g_noise[(size_t)y * NT + x] = a * (1 - v) + b * v;
    }
}
static inline float noise_at(float wx, float wz, float wavelength) {   // nearest: the table is much finer than the feature
    int x = (int)floorf(wx / wavelength * (NT / 24.0f)) & (NT - 1), y = (int)floorf(wz / wavelength * (NT / 24.0f)) & (NT - 1);
    return g_noise[(size_t)y * NT + x];
}
static inline u32 hash2(int a, int b) { u32 h = (u32)a * 73856093u ^ (u32)b * 19349663u; h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15; return h; }

// ---------------------------------------------------------------- style
struct Ink { float r, g, b; };
static const Ink I_BLD = {66, 40, 20}, I_BLD_HATCH = {44, 26, 12}, I_OUT = {34, 20, 9}, I_ROAD = {122, 88, 48}, I_KERB = {72, 46, 22};
static const Ink I_GRASS = {92, 104, 40}, I_TICK = {62, 76, 28}, I_DIRT = {134, 98, 54}, I_WATER = {46, 92, 120}, I_WLINE = {30, 66, 92};
static const Ink I_ROCK = {112, 84, 58}, I_VOID = {120, 90, 56}, I_WALL = {44, 27, 12}, I_TREE = {74, 92, 38}, I_TREEL = {38, 50, 20};
static const Ink I_BUSH = {96, 112, 48}, I_SHADOW = {54, 34, 16}, I_CONT = {120, 84, 46};

struct Style {                 // widths in metres
    float outline, jump, wall, kerb, hatch_gap, hatch_w, shadow, water_gap, water_w;
    float tick_cell, tick_len, tick_w, tree_line, contour;
};
static const Style STYLES[LEVELS] = {
    {0.42f, 0.20f, 0.30f, 0.20f, 1.70f, 0.26f, 0.75f, 1.35f, 0.20f, 3.0f, 0.80f, 0.20f, 0.20f, 0.11f},   // level 0: about 3.3 px / m
    {0.66f, 0.32f, 0.46f, 0.32f, 2.60f, 0.48f, 1.00f, 2.10f, 0.34f, 4.2f, 1.30f, 0.34f, 0.32f, 0.18f},   // level 1: about 1.8 px / m
};

// ---------------------------------------------------------------- one tile
static const int SS = 3;
static const float CONTOUR = 4.0f;             // metres between contour lines
struct Work {
    std::vector<float> top; std::vector<u8> mat, flag, cls, tree;
    std::vector<u16> d0, d1, d2, d3, d4, d5, d6, d7;
    std::vector<int> label, stack;
    std::vector<float> lattice;
    std::vector<int> seen; int stamp = 0;
    std::vector<float> acc;
    void size(int N) {
        size_t n = (size_t)N * N;
        if (top.size() != n) {
            top.resize(n); mat.resize(n); flag.resize(n); cls.resize(n); tree.resize(n);
            d0.resize(n); d1.resize(n); d2.resize(n); d3.resize(n); d4.resize(n); d5.resize(n); d6.resize(n); d7.resize(n); label.resize(n);
            acc.resize((size_t)TILE_PX * TILE_PX * 4);
        }
    }
};

// 3-4 chamfer distance to the source samples (unit: 1 / 3 sample), capped
static void chamfer(const u8* cls_or_mask, int N, u16* d, bool (*src)(u8)) {
    const u16 INF = 60000;
    for (size_t i = 0, n = (size_t)N * N; i < n; ++i) d[i] = src(cls_or_mask[i]) ? 0 : INF;
    for (int r = 0; r < N; ++r) {
        u16* p = d + (size_t)r * N;
        for (int c = 0; c < N; ++c) {
            u16 v = p[c];
            if (!v) continue;
            int b = v;
            if (c > 0) b = std::min(b, p[c - 1] + 3);
            if (r > 0) {
                b = std::min(b, p[c - N] + 3);
                if (c > 0) b = std::min(b, p[c - N - 1] + 4);
                if (c + 1 < N) b = std::min(b, p[c - N + 1] + 4);
            }
            p[c] = (u16)std::min(b, (int)INF);
        }
    }
    for (int r = N - 1; r >= 0; --r) {
        u16* p = d + (size_t)r * N;
        for (int c = N - 1; c >= 0; --c) {
            u16 v = p[c];
            if (!v) continue;
            int b = v;
            if (c + 1 < N) b = std::min(b, p[c + 1] + 3);
            if (r + 1 < N) {
                b = std::min(b, p[c + N] + 3);
                if (c + 1 < N) b = std::min(b, p[c + N + 1] + 4);
                if (c > 0) b = std::min(b, p[c + N - 1] + 4);
            }
            p[c] = (u16)std::min(b, (int)INF);
        }
    }
}
static bool src_not_bld(u8 c) { return c != C_BLD; }
static bool src_not_road(u8 c) { return c != C_ROAD; }
static bool src_not_water(u8 c) { return c != C_WATER; }
static bool src_has(u8 c) { return c != C_VOID; }
static bool src_nonzero(u8 c) { return c != 0; }
static bool src_zero(u8 c) { return c == 0; }

static inline void over(float* p, const Ink& k, float a) {   // premultiplied accumulate of one ink (0..255 colours)
    float q = 1.0f - a;
    p[0] = p[0] * q + k.r * a; p[1] = p[1] * q + k.g * a; p[2] = p[2] * q + k.b * a; p[3] = p[3] * q + a;
}
static inline float stroke(u16 d, float width_units) {       // 1 inside the stroke, a one-sub-sample ramp at its edge
    float v = (width_units - d) / 3.0f + 0.5f;
    return v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v);
}

// the coarse opened ground at world (x, z), bilinear over the blocks that have one
static float ground_at(float x, float z) {
    float fx = (x - g_x0) / GROUND_CELL - 0.5f, fy = (g_z1 - z) / GROUND_CELL - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float u = fx - x0, v = fy - y0;
    float s = 0.0f, w = 0.0f;
    for (int k = 0; k < 4; ++k) {
        int xx = x0 + (k & 1), yy = y0 + (k >> 1);
        if (xx < 0 || yy < 0 || xx >= g_gw || yy >= g_gh) continue;
        float g = g_ground[(size_t)yy * g_gw + xx]; if (g < -BIG * 0.5f) continue;
        float wk = ((k & 1) ? u : 1 - u) * ((k >> 1) ? v : 1 - v) + 1e-4f;
        s += g * wk; w += wk;
    }
    return w > 0.0f ? s / w : -BIG;
}

// true when the tile holds ink; rgba = TILE_PX^2 straight-alpha pixels (R G B A)
static bool render_tile(Work& W, int level, int col, int row, u8* rgba) {
    const float t = TEXEL0 * (float)(1 << level), s = t / SS;
    const float tw = (TILE_PX - 2 * BORDER_PX) * t;
    const float wx0 = g_x0 + col * tw - BORDER_PX * t, wz1 = g_z1 - row * tw + BORDER_PX * t;   // the texture's north-west corner
    const int gp = (int)ceilf(7.0f / t);        // guard texels: every stroke / distance sees past the tile
    const int N = (TILE_PX + 2 * gp) * SS;
    W.size(N);
    const float ox = wx0 - gp * t, oz = wz1 + gp * t;
    size_t nn = (size_t)N * N;
    std::fill(W.top.begin(), W.top.end(), NONE); memset(W.mat.data(), 255, nn); memset(W.flag.data(), 0, nn);
    Target tg = {W.top.data(), W.mat.data(), W.flag.data(), N, N, s, ox, oz};
    // the triangles of the bins under this tile
    if (W.seen.size() != g_tri.size()) W.seen.assign(g_tri.size(), 0);
    ++W.stamp;
    int c0 = std::max(0, (int)floorf((ox - g_x0) / BIN)), c1 = std::min(g_bw - 1, (int)floorf((ox + N * s - g_x0) / BIN));
    int r0 = std::max(0, (int)floorf((g_z1 - oz) / BIN)), r1 = std::min(g_bh - 1, (int)floorf((g_z1 - (oz - N * s)) / BIN));
    int nflat = 0;
    for (int r = r0; r <= r1; ++r) for (int c = c0; c <= c1; ++c)
        for (int i : g_bin_flat[(size_t)r * g_bw + c]) if (W.seen[i] != W.stamp) { W.seen[i] = W.stamp; raster_flat(tg, g_tri[i]); ++nflat; }
    if (!nflat) return false;
    ++W.stamp;
    for (int r = r0; r <= r1; ++r) for (int c = c0; c <= c1; ++c)
        for (int i : g_bin_vert[(size_t)r * g_bw + c]) if (W.seen[i] != W.stamp) { W.seen[i] = W.stamp; raster_wall(tg, g_tri[i]); }
    // close the hairline gaps between neighbouring collision hulls
    for (int pass = 0; pass < 2; ++pass)
        for (int r = 1; r < N - 1; ++r) for (int c = 1; c < N - 1; ++c) {
            size_t i = (size_t)r * N + c;
            if (W.top[i] > NONE) continue;
            size_t a = 0, b = 0;
            if (W.top[i - 1] > NONE && W.top[i + 1] > NONE) { a = i - 1; b = i + 1; }
            else if (W.top[i - N] > NONE && W.top[i + N] > NONE) { a = i - N; b = i + N; }
            if (a) { size_t k = W.top[a] >= W.top[b] ? a : b; W.top[i] = W.top[k]; W.mat[i] = W.mat[k]; W.flag[i] = W.flag[k]; }
        }
    // local ground on a 1 m lattice
    const float lat = 1.0f; int ln = (int)(N * s / lat) + 2;
    W.lattice.resize((size_t)ln * ln);
    for (int r = 0; r < ln; ++r) for (int c = 0; c < ln; ++c) W.lattice[(size_t)r * ln + c] = ground_at(ox + c * lat, oz - r * lat);
    // classes
    int present[8] = {0};
    for (int r = 0; r < N; ++r) {
        int lr = (int)(r * s / lat);
        for (int c = 0; c < N; ++c) {
            size_t i = (size_t)r * N + c;
            float v = W.top[i]; u8 k = C_VOID;
            if (v > NONE) {
                u8 m = W.mat[i]; bool steep = (W.flag[i] & F_STEEP) != 0;
                float g = W.lattice[(size_t)lr * ln + (int)(c * s / lat)];
                bool soft = m == M_GRASS || m == M_BUSH || m == M_DIRT || m == M_WATER;
                if (m == M_ASPHALT) k = C_ROAD;
                else if (m == M_WATER) k = C_WATER;
                else if (g > -BIG * 0.5f && v - g > 2.6f && !soft && !steep) k = C_BLD;
                else if (steep) k = C_ROCK;
                else if (m == M_GRASS || m == M_BUSH) k = C_GRASS;
                else if (m == M_DIRT) k = C_DIRT;
                else k = C_GROUND;
            }
            W.cls[i] = k; ++present[k];
        }
    }
    if (present[C_VOID] == (int)nn) return false;
    // building specks: components under 4 m2 are clutter that came in with a big model
    if (present[C_BLD]) {
        const int minarea = (int)(4.0f / (s * s));
        std::fill(W.label.begin(), W.label.end(), 0);
        int next = 0;
        for (size_t i0 = 0; i0 < nn; ++i0) {
            if (W.cls[i0] != C_BLD || W.label[i0]) continue;
            ++next; W.stack.clear(); W.stack.push_back((int)i0); W.label[i0] = next;
            size_t start = 0;
            while (start < W.stack.size()) {
                int i = W.stack[start++]; int r = i / N, c = i - r * N;
                int nb[4] = {c > 0 ? i - 1 : -1, c + 1 < N ? i + 1 : -1, r > 0 ? i - N : -1, r + 1 < N ? i + N : -1};
                for (int q : nb) if (q >= 0 && W.cls[q] == C_BLD && !W.label[q]) { W.label[q] = next; W.stack.push_back(q); }
            }
            // a building rises above what is around it: the concrete strip on a viaduct is raised off the ground but level
            // with the road beside it, so it is not one
            // ... and it falls away at its edges, where a hill runs on into its slope
            int edge = 0, under = 0, drop = 0;
            for (int i : W.stack) {
                int r = i / N, c = i - r * N;
                int nb[4] = {c > 0 ? i - 1 : -1, c + 1 < N ? i + 1 : -1, r > 0 ? i - N : -1, r + 1 < N ? i + N : -1};
                bool e = false, u = false, f = false;
                for (int q : nb) if (q >= 0 && W.cls[q] != C_BLD && W.cls[q] != C_VOID) {
                    e = true;
                    if (W.cls[q] == C_ROAD && W.top[q] > W.top[i] - 0.4f) u = true;
                    if (W.top[q] < W.top[i] - 1.0f) f = true;
                }
                edge += e; under += u; drop += f;
            }
            u8 to = C_BLD;
            if ((int)W.stack.size() < minarea || under * 5 > edge) to = C_GROUND;
            else if (edge > 0 && drop * 20 < edge * 3) to = C_ROCK;
            if (to != C_BLD) for (int i : W.stack) { W.cls[i] = to; --present[C_BLD]; ++present[to]; }
        }
    }
    // height jumps on the upper side (roof steps, ledges, wall tops) as a mask, thin walls as another
    std::vector<u8>& J = W.tree;   // reused below after the distance is taken
    for (int r = 0; r < N; ++r) for (int c = 0; c < N; ++c) {
        size_t i = (size_t)r * N + c; u8 j = 0;
        float v = W.top[i];
        if (v > NONE && W.cls[i] != C_VOID) {
            float lim = v - 0.9f;
            int nb[4] = {c > 0 ? -1 : 0, c + 1 < N ? 1 : 0, r > 0 ? -N : 0, r + 1 < N ? N : 0};
            for (int q : nb) if (q && W.top[i + q] > NONE && W.top[i + q] < lim) j = 1;
        }
        J[i] = j;
    }
    bool have_jump = false; for (size_t i = 0; i < nn && !have_jump; ++i) have_jump = J[i] != 0;
    if (have_jump) chamfer(J.data(), N, W.d3.data(), src_nonzero); else std::fill(W.d3.begin(), W.d3.end(), (u16)60000);
    bool have_wall = false;
    for (size_t i = 0; i < nn; ++i) { J[i] = (W.flag[i] & F_WALL) ? 1 : 0; have_wall = have_wall || J[i]; }
    if (have_wall) chamfer(J.data(), N, W.d4.data(), src_nonzero); else std::fill(W.d4.begin(), W.d4.end(), (u16)60000);
    // contour lines of the land: one every CONTOUR metres, none across a jump (the jump has its own line)
    bool have_cont = false;
    {
        auto land = [](u8 k) { return k == C_ROCK || k == C_GROUND || k == C_GRASS || k == C_DIRT; };
        auto lev = [](float v) { return (int)floorf((v + CONTOUR * 0.5f) / CONTOUR); };
        memset(J.data(), 0, nn);
        for (int r = 0; r + 1 < N; ++r) for (int c = 0; c + 1 < N; ++c) {
            size_t i = (size_t)r * N + c;
            if (!land(W.cls[i])) continue;
            float v = W.top[i]; int l = lev(v);
            size_t q[2] = {i + 1, i + N};
            for (size_t n : q) if (land(W.cls[n]) && fabsf(W.top[n] - v) < 0.9f && lev(W.top[n]) != l) { J[i] = 1; have_cont = true; }
        }
    }
    if (have_cont) chamfer(J.data(), N, W.d7.data(), src_nonzero);
    if (present[C_BLD]) chamfer(W.cls.data(), N, W.d0.data(), src_not_bld);
    if (present[C_ROAD]) chamfer(W.cls.data(), N, W.d1.data(), src_not_road);
    if (present[C_WATER]) chamfer(W.cls.data(), N, W.d2.data(), src_not_water);
    if (present[C_VOID]) chamfer(W.cls.data(), N, W.d5.data(), src_has);
    // tree and bush symbols: 1 = tree canopy, 2 = trunk, 3 = bush, drawn only on the ground (buildings stay on top)
    memset(J.data(), 0, nn);
    int cc0 = std::max(0, c0 - 1), cc1 = std::min(g_bw - 1, c1 + 1), rr0 = std::max(0, r0 - 1), rr1 = std::min(g_bh - 1, r1 + 1);
    bool have_tree = false;
    for (int r = rr0; r <= rr1; ++r) for (int c = cc0; c <= cc1; ++c)
        for (int mi : g_bin_mark[(size_t)r * g_bw + c]) {
            const Mark& m = g_mark[mi];
            int kind = (int)m.kind; float R = m.r;
            float mc = (m.x - ox) / s, mr = (oz - m.z) / s, Rs = R * 1.16f / s;
            int a0 = std::max(0, (int)(mc - Rs)), a1 = std::min(N - 1, (int)(mc + Rs) + 1), b0 = std::max(0, (int)(mr - Rs)), b1 = std::min(N - 1, (int)(mr + Rs) + 1);
            u32 h = hash2((int)(m.x * 8), (int)(m.z * 8));
            float ph = (h & 255) / 255.0f * 6.283f; int lobes = kind == K_PALM ? 7 : (R > 3.5f ? 9 : 6);
            float deep = kind == K_PALM ? 0.28f : 0.13f;
            for (int b = b0; b <= b1; ++b) for (int a = a0; a <= a1; ++a) {
                float dx = a + 0.5f - mc, dy = b + 0.5f - mr, d = sqrtf(dx * dx + dy * dy);
                float th = atan2f(dy, dx);
                float lim = (R / s) * (1.0f - deep + deep * sinf(lobes * th + ph));
                size_t i = (size_t)b * N + a;
                if (W.cls[i] == C_BLD || W.cls[i] == C_WATER) continue;
                if (d < lim) { J[i] = kind == K_BUSH ? (J[i] == 1 ? 1 : 3) : 1; have_tree = true; }
            }
            if (kind != K_BUSH) {
                float tr = std::min(0.55f, R * 0.2f) / s;
                for (int b = std::max(0, (int)(mr - tr)); b <= std::min(N - 1, (int)(mr + tr)); ++b)
                    for (int a = std::max(0, (int)(mc - tr)); a <= std::min(N - 1, (int)(mc + tr)); ++a) {
                        float dx = a + 0.5f - mc, dy = b + 0.5f - mr;
                        size_t i = (size_t)b * N + a;
                        if (dx * dx + dy * dy < tr * tr && J[i]) J[i] = 2;
                    }
            }
        }
    if (have_tree) chamfer(J.data(), N, W.d6.data(), src_zero);
    // compose
    const Style& st = STYLES[level];
    const float U = 3.0f / s;                   // distance units per metre
    const float wob = 0.09f / s;                // the hand: lines wander a little (sub-samples)
    const int gs = gp * SS, shx = (int)(st.shadow / s), shy = (int)(st.shadow / s);
    const float cover = 1.0f / (SS * SS);
    float* acc = W.acc.data();
    memset(acc, 0, (size_t)TILE_PX * TILE_PX * 4 * sizeof(float));
    const float k_hatch = 0.7071f / st.hatch_gap, hatch_w = st.hatch_w / st.hatch_gap;
    for (int r = 0; r < TILE_PX * SS; ++r) {
        const float wz = oz - (r + gs + 0.5f) * s;
        for (int c = 0; c < TILE_PX * SS; ++c) {
            const float wx = ox + (c + gs + 0.5f) * s;
            int jc = c + gs + (int)(noise_at(wx, wz, 9.0f) * wob), jr = r + gs + (int)(noise_at(wz + 37.0f, wx - 19.0f, 9.0f) * wob);
            jc = std::min(std::max(jc, 0), N - 1); jr = std::min(std::max(jr, 0), N - 1);
            const size_t j = (size_t)jr * N + jc;
            const u8 k = W.cls[j];
            float p[4] = {0, 0, 0, 0};
            switch (k) {
            case C_VOID: { float v = W.d5[j] / U; if (v < 8.0f) over(p, I_VOID, 0.30f * expf(-v / 2.2f)); break; }
            case C_ROAD: {
                over(p, I_ROAD, 0.27f);
                float a = stroke(W.d1[j], st.kerb * U); if (a > 0) over(p, I_KERB, 0.72f * a);
                break;
            }
            case C_GRASS: case C_DIRT: {
                bool gr = k == C_GRASS;
                over(p, gr ? I_GRASS : I_DIRT, gr ? 0.24f : 0.15f);
                float cell = gr ? st.tick_cell : st.tick_cell * 1.3f;
                float fx = wx / cell, fz = wz / cell; int ix = (int)floorf(fx), iz = (int)floorf(fz);
                u32 h = hash2(ix, iz);
                float ux = (fx - ix - 0.25f - (h & 255) / 512.0f) * cell, uz = (fz - iz - 0.25f - ((h >> 8) & 255) / 512.0f) * cell;
                float ang = ((h >> 16) & 255) / 255.0f * (gr ? 0.9f : 3.14f) - (gr ? 0.45f : 0.0f), ca = cosf(ang), sa = sinf(ang);
                float along = ux * ca + uz * sa, across = -ux * sa + uz * ca;
                float len = gr ? st.tick_len : st.tick_len * 0.4f;
                if (fabsf(along) < len * 0.5f && fabsf(across) < st.tick_w * 0.5f) over(p, gr ? I_TICK : I_DIRT, gr ? 0.55f : 0.5f);
                break;
            }
            case C_WATER: {
                over(p, I_WATER, 0.42f);
                float v = W.d2[j] / U;
                for (int n = 0; n < 3; ++n) {
                    float e = fabsf(v - (n + 0.6f) * st.water_gap * (1.0f + 0.25f * n));
                    if (e < st.water_w * 0.5f) { over(p, I_WLINE, 0.55f - 0.14f * n); break; }
                }
                break;
            }
            case C_ROCK: {
                over(p, I_ROCK, 0.17f);
                break;
            }
            default: break;
            }
            if (have_cont && (k == C_ROCK || k == C_GROUND || k == C_GRASS || k == C_DIRT)) {
                float a = stroke(W.d7[j], st.contour * U); if (a > 0) over(p, I_CONT, 0.50f * a);
            }
            if (k != C_BLD) {                       // the buildings' shadow to the south east
                int sc = jc - shx, sr = jr - shy;
                if (sc >= 0 && sr >= 0 && W.cls[(size_t)sr * N + sc] == C_BLD) over(p, I_SHADOW, 0.20f);
                u8 tm = have_tree ? J[j] : 0;
                if (tm) {
                    bool bush = tm == 3;
                    over(p, bush ? I_BUSH : I_TREE, bush ? 0.42f : 0.52f);
                    float a = stroke(W.d6[j], st.tree_line * U * (bush ? 0.7f : 1.0f)); if (a > 0) over(p, I_TREEL, 0.8f * a);
                    if (tm == 2) over(p, I_TREEL, 0.9f);
                }
            } else {
                float dm = W.d0[j] / U;
                over(p, I_BLD, 0.36f);
                float u = (wx + wz) * k_hatch, f = u - floorf(u);
                if (f < hatch_w) over(p, I_BLD_HATCH, 0.42f);
                if (dm < 1.5f) over(p, I_OUT, 0.20f * (1.0f - dm / 1.5f));
            }
            {
                float a = stroke(W.d3[j], st.jump * U);
                if (a > 0) over(p, I_OUT, (k == C_BLD ? 0.55f : 0.75f) * a);
                a = stroke(W.d4[j], st.wall * U);
                if (a > 0) over(p, I_WALL, 0.88f * a);
                if (k == C_BLD) { a = stroke(W.d0[j], st.outline * U); if (a > 0) over(p, I_OUT, 0.96f * a); }
            }
            float dens = 0.93f + 0.09f * noise_at(wz * 0.31f, wx * 0.31f, 3.0f);
            float* o = acc + ((size_t)(r / SS) * TILE_PX + c / SS) * 4;
            o[0] += p[0] * dens; o[1] += p[1] * dens; o[2] += p[2] * dens; o[3] += p[3] * dens;
        }
    }
    bool ink = false;
    for (int i = 0, n = TILE_PX * TILE_PX; i < n; ++i) {
        float a = acc[i * 4 + 3] * cover;
        u8* o = rgba + i * 4;
        if (a < 0.008f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
        float q = 1.0f / (a / cover);
        for (int k = 0; k < 3; ++k) { int v = (int)(acc[i * 4 + k] * q + 0.5f); o[k] = (u8)(std::min(255, v) & 0xFC); }
        o[3] = (u8)std::min(255, (int)(a * 255.0f + 0.5f));
        ink = true;
    }
    return ink;
}

// ---------------------------------------------------------------- PNG (deflate with fixed codes, hash chains)
struct Bits {
    std::vector<u8>& o; u32 acc = 0; int n = 0;
    explicit Bits(std::vector<u8>& v) : o(v) {}
    void put(u32 v, int bits) { acc |= v << n; n += bits; while (n >= 8) { o.push_back((u8)acc); acc >>= 8; n -= 8; } }
    void huff(u32 code, int bits) { u32 r = 0; for (int i = 0; i < bits; ++i) r |= ((code >> i) & 1) << (bits - 1 - i); put(r, bits); }
    void lit(int v) {
        if (v < 144) huff(0x30 + v, 8); else if (v < 256) huff(0x190 + v - 144, 9);
        else if (v < 280) huff(v - 256, 7); else huff(0xC0 + v - 280, 8);
    }
    void flush() { if (n) { o.push_back((u8)acc); acc = 0; n = 0; } }
};
static u32 adler(const u8* p, size_t n) {
    u32 a = 1, b = 0;
    while (n) { size_t k = std::min<size_t>(n, 5552); n -= k; while (k--) { a += *p++; b += a; } a %= 65521; b %= 65521; }
    return b << 16 | a;
}
static void zlib_compress(const u8* d, size_t n, std::vector<u8>& out) {
    static const int LB[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int LX[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int DB[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int DX[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    out.push_back(0x78); out.push_back(0xDA);
    Bits b(out); b.put(1, 1); b.put(1, 2);      // one final block, fixed codes
    const int HB = 15, WS = 32768, CHAIN = 32;
    std::vector<int> head((size_t)1 << HB, -1), prev(n ? n : 1, -1);
    auto hsh = [&](size_t i) { return ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & ((1 << HB) - 1); };
    size_t i = 0;
    while (i < n) {
        int best = 0, dist = 0;
        if (i + 3 <= n) {
            int h = hsh(i), cand = head[h], lim = (int)std::min<size_t>(258, n - i), chain = CHAIN;
            while (cand >= 0 && (int)i - cand <= WS && chain--) {
                int l = 0; while (l < lim && d[cand + l] == d[i + l]) ++l;
                if (l > best) { best = l; dist = (int)i - cand; if (l == lim) break; }
                cand = prev[cand];
            }
        }
        int adv = 1;
        if (best >= 4 || (best == 3 && dist < 512)) {
            int s = 28; while (LB[s] > best) --s;
            b.lit(257 + s); if (LX[s]) b.put((u32)(best - LB[s]), LX[s]);
            int q = 29; while (DB[q] > dist) --q;
            u32 code = (u32)q, r = 0; for (int k = 0; k < 5; ++k) r |= ((code >> k) & 1) << (4 - k);
            b.put(r, 5); if (DX[q]) b.put((u32)(dist - DB[q]), DX[q]);
            adv = best;
        } else b.lit(d[i]);
        for (int k = 0; k < adv; ++k, ++i)
            if (i + 3 <= n) { int h = hsh(i); prev[i] = head[h]; head[h] = (int)i; }
    }
    b.lit(256); b.flush();
    u32 a = adler(d, n);
    for (int k = 3; k >= 0; --k) out.push_back((u8)(a >> (k * 8)));
}
static u32 g_crc[256];
static void crc_init() { for (u32 n = 0; n < 256; ++n) { u32 c = n; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc[n] = c; } }
static u32 crc(const u8* p, size_t n, u32 c = 0xFFFFFFFFu) { while (n--) c = g_crc[(c ^ *p++) & 255] ^ (c >> 8); return c; }
static void chunk(std::vector<u8>& o, const char* t, const std::vector<u8>& d) {
    u32 n = (u32)d.size();
    for (int k = 3; k >= 0; --k) o.push_back((u8)(n >> (k * 8)));
    size_t at = o.size();
    o.insert(o.end(), t, t + 4); o.insert(o.end(), d.begin(), d.end());
    u32 c = ~crc(&o[at], n + 4);
    for (int k = 3; k >= 0; --k) o.push_back((u8)(c >> (k * 8)));
}
// RGBA -> PNG, per-row filter by the smallest absolute sum
static void encode_png(const u8* rgba, int w, int h, std::vector<u8>& o) {
    static const u8 SIG[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    o.insert(o.end(), SIG, SIG + 8);
    std::vector<u8> ih(13);
    ih[0] = (u8)(w >> 24); ih[1] = (u8)(w >> 16); ih[2] = (u8)(w >> 8); ih[3] = (u8)w; ih[4] = (u8)(h >> 24); ih[5] = (u8)(h >> 16); ih[6] = (u8)(h >> 8); ih[7] = (u8)h;
    ih[8] = 8; ih[9] = 6;
    chunk(o, "IHDR", ih);
    const int st = w * 4;
    std::vector<u8> raw((size_t)(st + 1) * h), cand[4];
    for (auto& c : cand) c.resize(st);
    for (int y = 0; y < h; ++y) {
        const u8* cur = rgba + (size_t)y * st; const u8* up = y ? cur - st : nullptr;
        long best = -1; int bf = 0;
        for (int f = 1; f <= 4; ++f) {
            long sum = 0;
            for (int x = 0; x < st; ++x) {
                int a = x >= 4 ? cur[x - 4] : 0, b = up ? up[x] : 0, c = (up && x >= 4) ? up[x - 4] : 0, p;
                if (f == 1) p = a; else if (f == 2) p = b; else if (f == 3) p = (a + b) >> 1;
                else { int q = a + b - c, pa = abs(q - a), pb = abs(q - b), pc = abs(q - c); p = pa <= pb && pa <= pc ? a : (pb <= pc ? b : c); }
                u8 v = (u8)(cur[x] - p); cand[f - 1][x] = v; sum += v < 128 ? v : 256 - v;
            }
            if (best < 0 || sum < best) { best = sum; bf = f; }
        }
        u8* row = &raw[(size_t)y * (st + 1)]; row[0] = (u8)bf; memcpy(row + 1, cand[bf - 1].data(), st);
    }
    std::vector<u8> z; z.reserve(raw.size() / 3);
    zlib_compress(raw.data(), raw.size(), z);
    chunk(o, "IDAT", z); chunk(o, "IEND", {});
}
// RGBA -> an 8-bit palette PNG (filter 0): a tile is a few inks over paper, 256 colours chosen per tile lose nothing the eye
// sees and the file is about a sixth. Median cut in premultiplied colour, because what a pixel shows is colour times alpha.
static void quantise(const u8* rgba, int npx, u8* index, u8 pal[256][4], int* colours) {
    const u32* px = (const u32*)rgba;
    std::vector<u32> keys(px, px + npx);
    std::sort(keys.begin(), keys.end());
    struct C { u32 key, n, id; float p[4]; };
    std::vector<C> c;
    for (int i = 0; i < npx; ) {
        int j = i; while (j < npx && keys[j] == keys[i]) ++j;
        u32 k = keys[i]; float a = (float)(k >> 24);
        if (k) {                                  // 0 = empty keeps index 0
            C e; e.key = k; e.n = (u32)(j - i); e.id = (u32)c.size();
            e.p[0] = (k & 255) * a / 255.0f; e.p[1] = ((k >> 8) & 255) * a / 255.0f; e.p[2] = ((k >> 16) & 255) * a / 255.0f; e.p[3] = a;
            c.push_back(e);
        }
        i = j;
    }
    std::vector<u32> sorted(c.size());
    for (size_t i = 0; i < c.size(); ++i) sorted[i] = c[i].key;
    struct Box { int a, b; float score; int dim; };
    auto measure = [&](Box& x) {
        float lo[4] = {1e9f, 1e9f, 1e9f, 1e9f}, hi[4] = {-1e9f, -1e9f, -1e9f, -1e9f}; double n = 0;
        for (int i = x.a; i < x.b; ++i) { n += c[i].n; for (int k = 0; k < 4; ++k) { lo[k] = std::min(lo[k], c[i].p[k]); hi[k] = std::max(hi[k], c[i].p[k]); } }
        x.dim = 0; float w = -1;
        for (int k = 0; k < 4; ++k) if (hi[k] - lo[k] > w) { w = hi[k] - lo[k]; x.dim = k; }
        x.score = x.b - x.a > 1 ? (float)(w * w * sqrt(n)) : -1.0f;
    };
    std::vector<Box> box;
    if (!c.empty()) { Box b = {0, (int)c.size(), 0, 0}; measure(b); box.push_back(b); }
    while (box.size() < 255) {
        int best = -1; float top = 0.0f;
        for (int i = 0; i < (int)box.size(); ++i) if (box[i].score > top) { top = box[i].score; best = i; }
        if (best < 0) break;
        Box b = box[best]; int d = b.dim;
        std::sort(c.begin() + b.a, c.begin() + b.b, [d](const C& x, const C& y) { return x.p[d] < y.p[d]; });
        double total = 0, run = 0; for (int i = b.a; i < b.b; ++i) total += c[i].n;
        int cut = b.a + 1;
        for (int i = b.a; i < b.b - 1; ++i) { run += c[i].n; if (run * 2 >= total) { cut = i + 1; break; } cut = i + 2; }
        cut = std::min(cut, b.b - 1);
        Box l = {b.a, cut, 0, 0}, r = {cut, b.b, 0, 0};
        measure(l); measure(r); box[best] = l; box.push_back(r);
    }
    memset(pal, 0, 256 * 4);
    std::vector<u8> to(sorted.size(), 0);
    for (int i = 0; i < (int)box.size(); ++i) {
        double s[4] = {0, 0, 0, 0}, n = 0;
        for (int j = box[i].a; j < box[i].b; ++j) { for (int k = 0; k < 4; ++k) s[k] += (double)c[j].p[k] * c[j].n; n += c[j].n; to[c[j].id] = (u8)(i + 1); }
        double a = s[3] / n;
        for (int k = 0; k < 3; ++k) pal[i + 1][k] = (u8)std::min(255.0, s[3] > 0 ? s[k] / s[3] * 255.0 + 0.5 : 0.0);
        pal[i + 1][3] = (u8)std::min(255.0, a + 0.5);
    }
    for (int i = 0; i < npx; ++i) {
        if (!px[i]) { index[i] = 0; continue; }
        index[i] = to[(size_t)(std::lower_bound(sorted.begin(), sorted.end(), px[i]) - sorted.begin())];
    }
    *colours = (int)box.size() + 1;
}
static void encode_png8(const u8* rgba, int w, int h, std::vector<u8>& o) {
    static const u8 SIG[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    o.insert(o.end(), SIG, SIG + 8);
    std::vector<u8> ih(13);
    ih[0] = (u8)(w >> 24); ih[1] = (u8)(w >> 16); ih[2] = (u8)(w >> 8); ih[3] = (u8)w; ih[4] = (u8)(h >> 24); ih[5] = (u8)(h >> 16); ih[6] = (u8)(h >> 8); ih[7] = (u8)h;
    ih[8] = 8; ih[9] = 3;
    chunk(o, "IHDR", ih);
    std::vector<u8> index((size_t)w * h), raw((size_t)(w + 1) * h, 0);
    static thread_local u8 pal[256][4]; int colours = 0;
    quantise(rgba, w * h, index.data(), pal, &colours);
    std::vector<u8> p((size_t)colours * 3), t((size_t)colours);
    for (int i = 0; i < colours; ++i) { p[(size_t)i * 3] = pal[i][0]; p[(size_t)i * 3 + 1] = pal[i][1]; p[(size_t)i * 3 + 2] = pal[i][2]; t[i] = pal[i][3]; }
    chunk(o, "PLTE", p); chunk(o, "tRNS", t);
    for (int y = 0; y < h; ++y) memcpy(&raw[(size_t)y * (w + 1) + 1], &index[(size_t)y * w], w);
    std::vector<u8> z; z.reserve(raw.size() / 4);
    zlib_compress(raw.data(), raw.size(), z);
    chunk(o, "IDAT", z); chunk(o, "IEND", {});
}
static bool write_file(const std::string& path, const std::vector<u8>& d) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    bool ok = fwrite(d.data(), 1, d.size(), f) == d.size(); fclose(f); return ok;
}

// ---------------------------------------------------------------- the pack
struct Job { int level, col, row; std::vector<u8> png; };
static int level_cols(int L) { float tw = (TILE_PX - 2 * BORDER_PX) * TEXEL0 * (1 << L); return (int)ceilf(g_xw / tw); }
static int level_rows(int L) { float tw = (TILE_PX - 2 * BORDER_PX) * TEXEL0 * (1 << L); return (int)ceilf(g_zh / tw); }

static void run_jobs(std::vector<Job>& jobs, int threads) {
    std::atomic<int> next(0);
    auto work = [&]() {
        Work W; std::vector<u8> rgba((size_t)TILE_PX * TILE_PX * 4);
        for (;;) {
            int i = next.fetch_add(1); if (i >= (int)jobs.size()) break;
            Job& j = jobs[i];
            if (render_tile(W, j.level, j.col, j.row, rgba.data())) encode_png8(rgba.data(), TILE_PX, TILE_PX, j.png);
        }
    };
    std::vector<std::thread> tt;
    for (int i = 0; i < threads; ++i) tt.emplace_back(work);
    for (auto& t : tt) t.join();
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: TODMinimapBake input.mmb out.mmp out.occ [--threads N] [--preview L x z half out.png]\n"); return 2; }
    const char* in = argv[1]; const char* out = argv[2]; const char* occ = argv[3];
    int threads = (int)std::thread::hardware_concurrency(); if (threads < 1) threads = 1; if (threads > 12) threads = 12;
    int pv_level = -1; float pv_x = 0, pv_z = 0, pv_h = 0; const char* pv_out = nullptr;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--preview") && i + 5 < argc) { pv_level = atoi(argv[i + 1]); pv_x = (float)atof(argv[i + 2]); pv_z = (float)atof(argv[i + 3]); pv_h = (float)atof(argv[i + 4]); pv_out = argv[i + 5]; i += 5; }
    }
    crc_init(); make_noise();
    load_input(in);
    if (g_tri.size() < 100) { fprintf(stderr, "too little collision\n"); return 3; }
    make_bounds(); make_bins(); coarse_pass(pv_level >= 0 ? nullptr : occ);
    if (pv_level >= 0) {
        float tw = (TILE_PX - 2 * BORDER_PX) * TEXEL0 * (1 << pv_level);
        int c0 = std::max(0, (int)floorf((pv_x - pv_h - g_x0) / tw)), c1 = std::min(level_cols(pv_level) - 1, (int)floorf((pv_x + pv_h - g_x0) / tw));
        int r0 = std::max(0, (int)floorf((g_z1 - pv_z - pv_h) / tw)), r1 = std::min(level_rows(pv_level) - 1, (int)floorf((g_z1 - pv_z + pv_h) / tw));
        std::vector<Job> jobs;
        for (int r = r0; r <= r1; ++r) for (int c = c0; c <= c1; ++c) { Job j; j.level = pv_level; j.col = c; j.row = r; jobs.push_back(std::move(j)); }
        const int in_px = TILE_PX - 2 * BORDER_PX, MW = (c1 - c0 + 1) * in_px, MH = (r1 - r0 + 1) * in_px;
        std::vector<u8> mosaic((size_t)MW * MH * 4);
        std::atomic<int> next(0);
        auto work = [&]() {
            Work W; std::vector<u8> rgba((size_t)TILE_PX * TILE_PX * 4);
            for (;;) {
                int i = next.fetch_add(1); if (i >= (int)jobs.size()) break;
                Job& j = jobs[i];
                if (!render_tile(W, j.level, j.col, j.row, rgba.data())) continue;
                for (int y = 0; y < in_px; ++y)
                    memcpy(&mosaic[((size_t)((j.row - r0) * in_px + y) * MW + (size_t)(j.col - c0) * in_px) * 4], &rgba[((size_t)(y + BORDER_PX) * TILE_PX + BORDER_PX) * 4], (size_t)in_px * 4);
            }
        };
        std::vector<std::thread> tt;
        for (int i = 0; i < threads; ++i) tt.emplace_back(work);
        for (auto& t : tt) t.join();
        std::vector<u8> png; encode_png(mosaic.data(), MW, MH, png);
        write_file(pv_out, png);
        printf("preview level %d: %d x %d px, world x %.2f z %.2f (north-west), %.3f m/px\n", pv_level, MW, MH, g_x0 + c0 * tw, g_z1 - r0 * tw, tw / in_px);
        return 0;
    }
    std::vector<Job> jobs;
    for (int L = 0; L < LEVELS; ++L)
        for (int r = 0; r < level_rows(L); ++r) for (int c = 0; c < level_cols(L); ++c) { Job j; j.level = L; j.col = c; j.row = r; jobs.push_back(std::move(j)); }
    run_jobs(jobs, threads);
    // the file
    const u32 hdr = 32 + 12 * LEVELS;
    std::vector<u8> pack(hdr, 0);
    auto wr32 = [&](size_t at, u32 v) { memcpy(&pack[at], &v, 4); };
    auto wrf = [&](size_t at, float v) { memcpy(&pack[at], &v, 4); };
    memcpy(&pack[0], "MMP1", 4); wr32(4, 1); wrf(8, g_x0); wrf(12, g_z1); wrf(16, TEXEL0); wr32(20, TILE_PX); wr32(24, BORDER_PX); wr32(28, LEVELS);
    size_t at = hdr;
    u32 table_at[LEVELS];
    for (int L = 0; L < LEVELS; ++L) {
        table_at[L] = (u32)at; size_t need = (size_t)level_cols(L) * level_rows(L) * 8;
        wr32(32 + L * 12, level_cols(L)); wr32(36 + L * 12, level_rows(L)); wr32(40 + L * 12, table_at[L]);
        at += need;
    }
    pack.resize(at, 0);
    int count[LEVELS] = {0}; size_t bytes[LEVELS] = {0};
    for (Job& j : jobs) {
        if (j.png.empty()) continue;
        size_t e = (size_t)table_at[j.level] + ((size_t)j.row * level_cols(j.level) + j.col) * 8;
        u32 off = (u32)pack.size(), sz = (u32)j.png.size();
        memcpy(&pack[e], &off, 4); memcpy(&pack[e + 4], &sz, 4);
        pack.insert(pack.end(), j.png.begin(), j.png.end());
        ++count[j.level]; bytes[j.level] += sz;
        std::vector<u8>().swap(j.png);
    }
    std::string tmp = std::string(out) + ".tmp";
    if (!write_file(tmp, pack)) { fprintf(stderr, "cannot write %s\n", tmp.c_str()); return 2; }
    remove(out); rename(tmp.c_str(), out);
    printf("%.2f %.2f %.2f %.2f %d %d %d %d %zu\n", g_x0, g_z1 - level_rows(0) * (TILE_PX - 2 * BORDER_PX) * TEXEL0, g_x0 + level_cols(0) * (TILE_PX - 2 * BORDER_PX) * TEXEL0, g_z1,
           level_cols(0) * (TILE_PX - 2 * BORDER_PX), level_rows(0) * (TILE_PX - 2 * BORDER_PX), count[0], count[1], pack.size());
    return 0;
}
