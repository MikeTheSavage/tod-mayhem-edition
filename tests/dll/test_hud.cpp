// test_hud.cpp - unit tests of dll/src/gp_hud_logic.cpp ([minimap] pure logic)
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "../../dll/src/gp_hud_logic.h"
#include "../../dll/src/gp_hud_tile.h"
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(float a, float b, float e = 1e-3f) { return fabsf(a - b) <= e; }

// a pack header as the baker writes it: 2 levels over a map 1000 x 2000 m from (-100, 500)
static void make_head(uint8_t* d) {
    memset(d, 0, hud::PACK_HEAD_BYTES);
    uint32_t u[] = {1}; float f[] = {-100.0f, 500.0f, 0.25f}; uint32_t v[] = {512, 4, 2, 8, 16, 1000, 4, 8, 2000};
    memcpy(d, "MMP1", 4); memcpy(d + 4, u, 4); memcpy(d + 8, f, 12); memcpy(d + 20, v, sizeof v);
}
// a PNG around a raw zlib stream (the CRCs are not checked by the decoder)
static size_t put_chunk(uint8_t* o, const char* t, const uint8_t* d, uint32_t n) {
    o[0] = (uint8_t)(n >> 24); o[1] = (uint8_t)(n >> 16); o[2] = (uint8_t)(n >> 8); o[3] = (uint8_t)n;
    memcpy(o + 4, t, 4); if (n) memcpy(o + 8, d, n); memset(o + 8 + n, 0, 4);
    return 12 + (size_t)n;
}
static size_t make_png(uint8_t* o, int w, const uint8_t* z, uint32_t zn) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    memcpy(o, SIG, 8); size_t at = 8;
    uint8_t ih[13] = {0, 0, 0, (uint8_t)w, 0, 0, 0, (uint8_t)w, 8, 3, 0, 0, 0};
    uint8_t pl[9] = {10, 20, 30, 40, 50, 60, 70, 80, 90}, tr[3] = {0, 128, 255};
    at += put_chunk(o + at, "IHDR", ih, 13); at += put_chunk(o + at, "PLTE", pl, 9); at += put_chunk(o + at, "tRNS", tr, 3);
    at += put_chunk(o + at, "IDAT", z, zn); at += put_chunk(o + at, "IEND", nullptr, 0);
    return at;
}

int main() {
    using namespace hud;
    // ---- the radar frame: the game's convention (side = right of the view, forward = up on screen)
    {
        Frame f = frame_from_view(0.0f, 1.0f, 0, 0);            // looking along +z (north)
        V2 a = to_radar(f, 0.0f, 10.0f, 1.0f); CHECK(near(a.x, 0) && near(a.y, -10));   // ahead = up
        V2 b = to_radar(f, 10.0f, 0.0f, 1.0f); CHECK(near(b.x, 10) && near(b.y, 0));    // +x = right (left-handed, y up)
        Frame g = frame_from_view(1.0f, 0.0f, 0, 0);            // looking along +x
        V2 c = to_radar(g, 10.0f, 0.0f, 1.0f); CHECK(near(c.x, 0) && near(c.y, -10));
        V2 d = to_radar(g, 0.0f, -10.0f, 1.0f); CHECK(near(d.x, 10) && near(d.y, 0));   // -z is to the right when facing +x
        // round trip at any heading and scale
        for (int i = 0; i < 16; ++i) {
            float a0 = i * 0.39f; Frame h = frame_from_view(sinf(a0) * 3.0f, cosf(a0) * 3.0f, 5, 7);
            V2 r = to_radar(h, 3.5f, -8.25f, 1.08f); V2 w = from_radar(h, r.x, r.y, 1.08f);
            CHECK(near(w.x, 3.5f) && near(w.y, -8.25f));
        }
        // the game's blip formula (0x6F3AD0): x = sin(acos(dot(dir, view))) * dist, sign from the side; y = -cos * dist
        Frame v = frame_from_view(0.6f, 0.8f, 0, 0);
        float dx = -4.0f, dz = 9.0f, dist = sqrtf(dx * dx + dz * dz);
        float c0 = (dx * 0.6f + dz * 0.8f) / dist; float s0 = sqrtf(1 - c0 * c0) * dist;
        float side = dx * 0.8f - dz * 0.6f; if (side < 0) s0 = -s0;
        V2 e = to_radar(v, dx, dz, 1.0f); CHECK(near(e.x, s0, 1e-3f) && near(e.y, -c0 * dist, 1e-3f));
        // north frame: a blip ahead of a player looking east ends up to the right
        V2 n = view_to_north(g, 0.0f, -10.0f); CHECK(near(n.x, 10) && near(n.y, 0));
        V2 n2 = view_to_north(f, 3.0f, 4.0f); CHECK(near(n2.x, 3) && near(n2.y, 4));   // looking north: unchanged
    }
    // ---- the disc from the radar background quad (128 x 128 texture, centre 64.5 / 68, radius 52.5)
    {
        float P[4][2] = {{100, 600}, {100, 856}, {356, 856}, {356, 600}}, UV[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
        Disc d = disc_from_quad(P, UV);
        CHECK(d.ok && near(d.unit, 2.0f) && near(d.cx, 100 + 129.0f) && near(d.cy, 600 + 136.0f) && near(d.r, 105.0f));
        float Q[4][2] = {{356, 600}, {100, 600}, {100, 856}, {356, 856}}, QV[4][2] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};   // other order
        Disc e = disc_from_quad(Q, QV); CHECK(e.ok && near(e.cx, d.cx) && near(e.cy, d.cy) && near(e.unit, 2.0f));
        float Z[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}}; CHECK(!disc_from_quad(P, Z).ok);
    }
    // ---- baked map index
    {
        MapInfo m[3];
        CHECK(parse_map_line("downtown -800 -900 400 300 2048 2048 downtown.png\r\n", &m[0]));
        CHECK(parse_map_line("  harbour -1500 -2000 -500 -800 1024 1024 harbour.png", &m[1]));
        CHECK(parse_map_line("plaza -300 -100 -100 100 512 512 plaza.png", &m[2]));
        MapInfo bad;
        CHECK(!parse_map_line("# comment", &bad)); CHECK(!parse_map_line("x 1 1 0 0 64 64 a.png", &bad)); CHECK(!parse_map_line("x 0 0 1 1 64 a.png", &bad));
        V2 uv = map_uv(m[0], -800, 300); CHECK(near(uv.x, 0) && near(uv.y, 0));
        uv = map_uv(m[0], 400, -900); CHECK(near(uv.x, 1) && near(uv.y, 1));
        CHECK(pick_map(m, 3, 0, 0) == 0); CHECK(pick_map(m, 3, -1000, -1000) == 1); CHECK(pick_map(m, 3, -200, 0) == 2);   // the nested small one
        CHECK(pick_map(m, 3, 5000, 0) == -1);
        // occupancy: the big harbour rect holds a point where only it has ground -> it wins over the smaller rect
        Occ o[3];
        CHECK(parse_occ("occ 100.0 2 2 -800.00 300.00\n10\n00\n", &o[0]));      // downtown: ground only in the NW cell
        CHECK(parse_occ("occ 100.0 10 12 -1500.00 -800.00\n" "1111111111\n1111111111\n1111111111\n1111111111\n1111111111\n1111111111\n"
                        "1111111111\n1111111111\n1111111111\n1111111111\n1111111111\n1111111111\n", &o[1]));
        CHECK(!parse_occ("occ 100.0 2 2 0 0\n10\n", &o[2]));   // too few cells
        o[2] = Occ();
        CHECK(occ_near(o[0], -750, 250) && occ_near(o[0], -650, 150) && !occ_near(o[0], 300, -800));
        MapInfo m2[2] = {m[0], m[1]}; m2[1].x0 = -1500; m2[1].x1 = 500; m2[1].z0 = -2000; m2[1].z1 = 400;   // overlapping rects
        CHECK(pick_map_occ(m2, o, 2, -750, 250) == 0);                            // both rects; downtown has ground here
        CHECK(pick_map_occ(m2, o, 2, -1400, -900) == 1);
        CHECK(pick_map_occ(m2, nullptr, 2, -750, 250) == pick_map(m2, 2, -750, 250));
        delete[] o[0].bits; delete[] o[1].bits;
    }
    // ---- objectives
    {
        CHECK(is_objective(2) && is_objective(9) && is_objective(1) && is_objective(12));
        CHECK(!is_objective(0) && !is_objective(3) && !is_objective(4) && !is_objective(5) && !is_objective(11) && !is_objective(13));
        CHECK(objective_rank(2) < objective_rank(9) && objective_rank(9) < objective_rank(1));
        char t[32];
        format_distance(85.6f, t, sizeof t); CHECK(!strcmp(t, "86 m"));
        format_distance(999.4f, t, sizeof t); CHECK(!strcmp(t, "999 m"));
        format_distance(1234.0f, t, sizeof t); CHECK(!strcmp(t, "1.2 km"));
        format_distance(23456.0f, t, sizeof t); CHECK(!strcmp(t, "23 km"));
        format_distance(-3.0f, t, sizeof t); CHECK(!strcmp(t, "0 m"));
    }
    // ---- projection through a camera at the origin looking along +z (rows right, up, forward, position)
    {
        Cam c; memset(&c, 0, sizeof c);
        c.m[0] = 1; c.m[5] = 1; c.m[10] = 1; c.m[15] = 1; c.fov_deg = 64; c.ratioxy = 1; c.W = 1920; c.H = 1080;
        float x, y, z;
        CHECK(project(c, V3{0, 0, 10}, &x, &y, &z) && near(x, 960) && near(y, 540) && near(z, 10));
        // the top of the picture: y = tan(vfov / 2) z, vfov = 2 atan(0.75 tan 32)
        float ty = 0.75f * tanf(32.0f * 3.14159265f / 180.0f);
        CHECK(project(c, V3{0, ty * 10, 10}, &x, &y, &z) && near(y, 0.0f, 0.05f));
        float tx = ty * 1920.0f / 1080.0f;   // Hor+: the right edge at the screen's aspect
        CHECK(project(c, V3{tx * 10, 0, 10}, &x, &y, &z) && near(x, 1920.0f, 0.1f));
        CHECK(!project(c, V3{2, 0, -10}, &x, &y, &z) && x > 960 && y > 540);   // behind, to the right: points right / down
        // a camera moved and turned 90 degrees (looking along +x): right = -z
        Cam d = c; d.m[0] = 0; d.m[2] = -1; d.m[8] = 1; d.m[10] = 0; d.m[12] = 5; d.m[13] = 1; d.m[14] = 2;
        CHECK(project(d, V3{15, 1, 2}, &x, &y, &z) && near(x, 960) && near(y, 540) && near(z, 10));
        CHECK(project(d, V3{15, 1, 0}, &x, &y, &z) && x > 960);
        Cam w = c; w.wide_flag = true; w.W = 1440; w.H = 1080;   // the console 16:9 flag fixes the aspect
        CHECK(project(w, V3{tx * 10, 0, 10}, &x, &y, &z) && near(x, 1440.0f, 0.5f));
    }
    // ---- screen edge
    {
        bool e; float a;
        V2 p = clamp_to_screen(500, 400, false, 1920, 1080, 40, &e, &a); CHECK(!e && near(p.x, 500) && near(p.y, 400));
        p = clamp_to_screen(3000, 540, false, 1920, 1080, 40, &e, &a); CHECK(e && near(p.x, 1880) && near(p.y, 540) && near(a, 0));
        p = clamp_to_screen(960, -500, false, 1920, 1080, 40, &e, &a); CHECK(e && near(p.y, 40) && near(p.x, 960));
        p = clamp_to_screen(1000, 600, true, 1920, 1080, 40, &e, &a); CHECK(e && (p.x <= 40.01f || p.x >= 1879.99f || p.y <= 40.01f || p.y >= 1039.99f));
        p = clamp_to_screen(960, 540, true, 1920, 1080, 40, &e, &a); CHECK(e && near(p.y, 1040));   // straight behind: bottom
    }
    // ---- the tile pack header
    {
        uint8_t d[PACK_HEAD_BYTES]; PackHead h; make_head(d);
        CHECK(parse_pack_head(d, sizeof d, &h));
        CHECK(h.levels == 2 && h.lv[0].cols == 8 && h.lv[0].rows == 16 && h.lv[1].rows == 8 && h.lv[1].table == 2000);
        CHECK(near(h.lv[0].tile_m, 126.0f) && near(h.lv[1].tile_m, 252.0f));
        CHECK(!parse_pack_head(d, 20, &h));                        // too short
        uint8_t e[PACK_HEAD_BYTES]; memcpy(e, d, sizeof e); e[0] = 'X'; CHECK(!parse_pack_head(e, sizeof e, &h));
        make_head(e); e[4] = 2; CHECK(!parse_pack_head(e, sizeof e, &h));    // a newer version
        make_head(e); e[20] = 0; e[21] = 1; CHECK(!parse_pack_head(e, sizeof e, &h));  // not 512 px
        make_head(e); e[28] = 9; CHECK(!parse_pack_head(e, sizeof e, &h));   // levels
        make_head(e); e[32] = 0; e[33] = 0; e[34] = 0; e[35] = 0; CHECK(!parse_pack_head(e, sizeof e, &h));  // no columns
        CHECK(parse_pack_head(d, sizeof d, &h));
        // the level for a screen: 1080p at zoom 0.75 is 1.82 px / m, 3024 x 1964 is 3.31 px / m
        CHECK(pick_level(h, 1.0f / 1.82f) == 1);
        CHECK(pick_level(h, 1.0f / 3.31f) == 0);
        CHECK(pick_level(h, 1.0f / 2.43f) == 0);                   // 1080p at zoom 1
        CHECK(pick_level(h, 10.0f) == 1);                          // never past the last level
        CHECK(paper_under(0xFFFFFFFFu) == 0xD8F6E4B0u);
        CHECK(paper_under(0x80FF0000u) == 0x6CF60000u);            // keeps the opacity's and the tint's share
        // tile rectangles: north first, neighbours share their edge exactly
        TileRect a = tile_rect(h, 0, 0, 0), b = tile_rect(h, 0, 1, 0), c = tile_rect(h, 0, 0, 1);
        CHECK(near(a.x0, -100.0f) && near(a.z1, 500.0f) && near(a.x1, 26.0f) && near(a.z0, 374.0f));
        CHECK(a.x1 == b.x0 && a.z0 == c.z1);
        CHECK(near(tile_rect(h, 1, 1, 1).x0, 152.0f));
        // the tiles around a player: nearest first, the one under the player at distance 0
        TileRef r[16];
        int n = tiles_near(h, 0, -37.0f, 437.0f, 20.0f, r, 16);
        CHECK(n == 1 && r[0].col == 0 && r[0].row == 0 && near(r[0].dist, 0.0f));
        n = tiles_near(h, 0, 26.0f, 374.0f, 67.0f, r, 16);         // on a corner: four tiles
        CHECK(n == 4);
        for (int i = 1; i < n; ++i) CHECK(r[i - 1].dist <= r[i].dist);
        n = tiles_near(h, 0, 15.0f, 437.0f, 20.0f, r, 16);         // 11 m from the east edge
        CHECK(n == 2 && r[0].col == 0 && r[1].col == 1 && near(r[1].dist, 11.0f));
        n = tiles_near(h, 0, 89.0f, 311.0f, 97.0f, r, 3);          // the cap keeps the nearest
        CHECK(n == 3 && r[0].col == 1 && r[0].row == 1 && r[2].dist >= r[1].dist);
        CHECK(tiles_near(h, 0, -500.0f, 437.0f, 97.0f, r, 16) == 0);    // off the map: west
        CHECK(tiles_near(h, 0, 5000.0f, -5000.0f, 97.0f, r, 16) == 0);  // off the map: south east
        n = tiles_near(h, 1, 898.0f, -1500.0f, 200.0f, r, 16);     // the last tile of the level, clamped
        CHECK(n >= 1 && r[0].col == 3 && r[0].row == 7);
    }
    // ---- index lines: packs may be larger than one texture, a picture of an older bake may not
    {
        MapInfo m;
        CHECK(parse_map_line("map10 293.76 -2380.58 2309.76 1273.42 8064 14616 map10.mmp\n", &m) && m.pack && m.h == 14616);
        CHECK(parse_map_line("map02 -429 -1016 578 747 1344 2048 map02.png\n", &m) && !m.pack);
        CHECK(!parse_map_line("map10 293 -2380 2309 1273 8064 14616 map10.png\n", &m));
    }
    // ---- one tile: palette + alpha, stored and fixed-code deflate, and what must be refused
    {
        const int W = 4; const size_t RAW = (W + 1) * W;
        // rows: filter 0 + indices 0 1 2 1
        uint8_t raw[RAW]; for (int y = 0; y < W; ++y) { uint8_t* p = raw + y * (W + 1); p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 2; p[4] = 1; }
        uint8_t z[64]; size_t zn = 0;
        z[zn++] = 0x78; z[zn++] = 0x01; z[zn++] = 1; z[zn++] = (uint8_t)RAW; z[zn++] = 0; z[zn++] = (uint8_t)~RAW; z[zn++] = 0xFF;
        memcpy(z + zn, raw, RAW); zn += RAW; z[zn++] = 0; z[zn++] = 0; z[zn++] = 0; z[zn++] = 0;
        uint8_t png[256]; size_t n = make_png(png, W, z, (uint32_t)zn);
        uint32_t px[W * W]; uint8_t scratch[RAW];
        CHECK(decode_tile(png, n, W, px, scratch));
        CHECK(px[0] == 0x000A141E && px[1] == 0x8028323C && px[2] == 0xFF46505A && px[3] == 0x8028323C && px[12] == 0x000A141E);
        CHECK(!decode_tile(png, n, 8, px, scratch));               // another size
        CHECK(!decode_tile(png, n - 40, W, px, scratch));          // cut off
        uint8_t bad[256]; memcpy(bad, png, n); bad[0] = 0; CHECK(!decode_tile(bad, n, W, px, scratch));
        uint8_t zb[64]; memcpy(zb, z, zn); zb[7] = 1;              // a filter other than 0
        size_t m = make_png(bad, W, zb, (uint32_t)zn); CHECK(!decode_tile(bad, m, W, px, scratch));
        // the same bytes as the baker writes them: one fixed-code block, 5 literals, then a copy of 15 at distance 5 that
        // overlaps itself (bits go in LSB first, Huffman codes MSB first)
        struct B { uint8_t* o; uint32_t acc; int n; size_t at; } b = {z, 0, 0, 2};
        auto put = [&](uint32_t v, int c) { b.acc |= v << b.n; b.n += c; while (b.n >= 8) { b.o[b.at++] = (uint8_t)b.acc; b.acc >>= 8; b.n -= 8; } };
        auto huff = [&](uint32_t code, int c) { uint32_t r = 0; for (int i = 0; i < c; ++i) r |= ((code >> i) & 1) << (c - 1 - i); put(r, c); };
        put(1, 1); put(1, 2);
        const int lit[5] = {0, 0, 1, 2, 1};
        for (int i = 0; i < 5; ++i) huff(0x30 + lit[i], 8);
        huff(267 - 256, 7); put(0, 1);                             // length 15: code 267, 1 extra bit
        huff(4, 5); put(0, 1);                                     // distance 5: code 4, 1 extra bit
        huff(0, 7);                                                // end of block
        if (b.n) b.o[b.at++] = (uint8_t)b.acc;
        n = make_png(png, W, z, (uint32_t)b.at);
        memset(px, 0, sizeof px);
        CHECK(decode_tile(png, n, W, px, scratch));
        CHECK(px[0] == 0x000A141E && px[2] == 0xFF46505A && px[15] == 0x8028323C);
    }
    // ---- the mip chain: alpha-weighted, so transparent paper never bleeds its colour into the ink
    {
        CHECK(mip_chain_texels(512) == 349525 && mip_chain_texels(1) == 1 && mip_chain_texels(4) == 21);
        uint32_t c[21]; memset(c, 0, sizeof c);
        c[0] = 0xFF102030; c[1] = 0x00FFFFFF; c[4] = 0x00FFFFFF; c[5] = 0xFF102030;   // two ink texels, two empty with white colour
        for (int i = 0; i < 16; ++i) if (i > 5 || (i > 1 && i < 4)) c[i] = 0;
        build_mips(c, 4);
        CHECK((c[16] >> 24) == 128 && (c[16] & 0xFFFFFF) == 0x102030);   // half alpha, the ink's own colour
        CHECK(c[17] == 0);                                         // nothing stays nothing
        CHECK((c[20] >> 24) == 32 && (c[20] & 0xFFFFFF) == 0x102030);
    }
    printf("test_hud: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
