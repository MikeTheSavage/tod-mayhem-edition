// test_mayhem.cpp - unit tests of [mayhem_title] (dll/src/gp_mayhem_logic.cpp): placement maths, the title picture's
// place, the region header, the burn constants, the timeline, sparks, the fire flipbook frames. Built and run by
// build.bat. No game, no Direct3D. Exit code 0 = all passed.
#include "../../dll/src/gp_mayhem_logic.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace mt;

static void test_affine() {
    Aff A = {2, 0.5f, -0.25f, 3, 10, -7}, I;
    CHECK(aff_inv(A, &I));
    Aff P = aff_mul(A, I);
    NEAR(P.a, 1, 1e-5); NEAR(P.b, 0, 1e-5); NEAR(P.c, 0, 1e-5); NEAR(P.d, 1, 1e-5); NEAR(P.tx, 0, 1e-4); NEAR(P.ty, 0, 1e-4);
    Aff Z = {1, 2, 2, 4, 0, 0}; CHECK(!aff_inv(Z, &I));
}

// the title picture as 0x45D940 draws it, measured in game: 1920x1080 -> (0,-180)-(1920,1260), 1600x1200 -> full
static void test_title_quad() {
    float P[4][2], UV[4][2]; Aff m, a; float x, y;
    title_quad(1920, 1080, false, 0.625f, 1.0f, P, UV);
    NEAR(P[0][0], 0, 1e-3); NEAR(P[0][1], -180, 1e-3); NEAR(P[2][0], 1920, 1e-3); NEAR(P[2][1], 1260, 1e-3);
    NEAR(UV[2][0], 0.625, 1e-6); NEAR(UV[2][1], 1, 1e-6);
    CHECK(uv_to_screen(P, UV, &m)); a = art_to_screen(m);
    aff_apply(a, 65, 100, &x, &y); NEAR(x, 195, 1e-2); NEAR(y, 101.25, 1e-2);       // the logo's T
    title_quad(1600, 1200, false, 0.625f, 1.0f, P, UV);
    NEAR(P[0][1], 0, 1e-3); NEAR(P[2][1], 1200, 1e-3);
    title_quad(1280, 720, true, 0.625f, 1.0f, P, UV);                              // console widescreen flag: 600 H / 450
    NEAR(P[2][1] - P[0][1], 960, 1e-2); NEAR(P[2][0] - P[0][0], 1280, 1e-3);
    float Q[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}}; CHECK(!uv_to_screen(Q, UV, &m));
}

static void test_region_place() {
    Region r; float res[11] = {1.5866f, 0.06f, 0.0537f, 0.0852f, 0.9463f, 0.9148f, 40.0f, 0.6979f, -0.3445f, 0.3236f, 0};
    CHECK(r.from_header(res));
    NEAR(r.aspect, 1.5866, 1e-5); NEAR(r.band_px, 40, 1e-5); NEAR(r.fire_slope, -0.3445, 1e-5);
    float bad[11] = {0}; Region r2; CHECK(!r2.from_header(bad)); NEAR(r2.aspect, 1.5866, 1e-4);   // unchanged
    Place p = {100, 240, 254};
    Rect e = e_rect(r, p);
    NEAR(e.x0 + r.bx0 * e.w, 100, 1e-3); NEAR(e.y0 + r.by0 * e.h, 240, 1e-3);        // the bbox lands where asked
    NEAR((r.bx1 - r.bx0) * e.w, 254, 1e-3); NEAR(e.w / e.h, r.aspect, 1e-4);
    Place d = place_default();
    CHECK(d.bw > 200 && d.bw < 320 && d.bx > 0 && d.bx + d.bw < 640 && d.by > 150 && d.by < 450);
}

// a synthetic hole: an ellipse in the middle of a 128 x 64 fx map (R < 128 inside)
static void make_fx(std::vector<uint8_t>& R, std::vector<uint8_t>& A, int w, int h) {
    R.assign((size_t)w * h, 255); A.assign((size_t)w * h, 128);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float dx = (x - w * 0.5f) / (w * 0.35f), dy = (y - h * 0.5f) / (h * 0.3f);
            float d = sqrtf(dx * dx + dy * dy);
            R[y * w + x] = (uint8_t)(d < 1.0f ? 40 : 230);
            A[y * w + x] = (uint8_t)((x * 37 + y * 11) & 255);
        }
}

static void test_burn() {
    Region r; int w = 128, h = 64; std::vector<uint8_t> R, A; make_fx(R, A, w, h);
    BurnK k;
    CHECK(burn_setup(R.data(), A.data(), w, h, r, 0.5f, 0.5f, &k));
    NEAR(k.iu, 0.5, 1e-6); NEAR(k.iv, 0.5, 1e-6);                                     // inside: kept
    // the farthest hole texel burns at 0.82 (without noise)
    float far = 0.0f;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (R[y * w + x] < 128) {
        float t = burn_time(k, r, (x + 0.5f) / w, (y + 0.5f) / h, 0.5f); if (t > far) far = t; }
    NEAR(far, 0.82, 1e-3);
    NEAR(burn_time(k, r, 0.5f, 0.5f, 0.5f), 0, 1e-6);
    CHECK(burn_time(k, r, 0.6f, 0.5f, 0.5f) > burn_time(k, r, 0.55f, 0.5f, 0.5f));      // further = later
    CHECK(burn_time(k, r, 0.55f, 0.5f, 1.0f) > burn_time(k, r, 0.55f, 0.5f, 0.0f));     // noise moves the front
    CHECK(k.rim > 0 && k.curl > 0 && k.rim < k.scorch && k.curl < k.scorch && k.ke > 0 && k.inv_ramp > 1);
    // an ignition outside the hole moves to the nearest hole texel
    BurnK k2; CHECK(burn_setup(R.data(), A.data(), w, h, r, 0.02f, 0.5f, &k2));
    CHECK(k2.iu > 0.14f && k2.iu < 0.2f); NEAR(k2.iv, 0.5, 0.03);
    int ix = (int)(k2.iu * w), iy = (int)(k2.iv * h); CHECK(R[iy * w + ix] < 128);
    // no hole at all: refused
    std::vector<uint8_t> none((size_t)w * h, 255); BurnK k3; CHECK(!burn_setup(none.data(), A.data(), w, h, r, 0.5f, 0.5f, &k3));
    CHECK(!burn_setup(nullptr, A.data(), w, h, r, 0.5f, 0.5f, &k3));
}

static void test_timeline() {
    Timeline tl; tl.delay = 0.6f; tl.reveal = 2.0f;
    CHECK(!tl.visible()); NEAR(tl.front(), 0, 1e-6);
    tl.start(false); CHECK(!tl.visible()); NEAR(tl.front(), 0, 1e-6);
    float prev = -1; bool mono = true;
    for (int i = 0; i < 300; ++i) { tl.advance(1.0f / 60.0f); float p = tl.front(); if (p < prev - 1e-6f) mono = false; prev = p; }
    CHECK(mono); CHECK(tl.visible() && tl.done()); NEAR(tl.front(), 1.02, 1e-6);
    NEAR(tl.calm(), (5.0 - 2.6) / 1.2 > 1 ? 1 : (5.0 - 2.6) / 1.2, 1e-3);
    Timeline t2; t2.delay = 0.6f; t2.reveal = 2.0f; t2.start(false); t2.advance(0.6f + 1.0f);
    CHECK(t2.front() > 0.6f && t2.front() < 1.0f);                                    // fast start: past half at half time
    NEAR(t2.calm(), 0, 1e-6);
    Timeline t3; t3.start(true); CHECK(t3.visible() && t3.done()); NEAR(t3.calm(), 1, 1e-6);   // already burnt in
}

static void test_sparks_fire() {
    Sparks s;
    for (int i = 0; i < 200; ++i) s.spawn(0.5f, 0.5f, 1.0f, 1.0f);
    CHECK(s.n == Sparks::MAX);
    bool up = true; for (int i = 0; i < s.n; ++i) if (!(s.p[i].vv < 0)) up = false;
    CHECK(up);
    s.step(0.1f, 1.5f); CHECK(s.n == Sparks::MAX);
    bool rose = true; for (int i = 0; i < s.n; ++i) if (!(s.p[i].v < 0.5f)) rose = false;
    CHECK(rose);
    for (int i = 0; i < 40; ++i) s.step(0.1f, 1.5f);
    CHECK(s.n == 0);                                                                  // all died (life <= 1.4 s)
    float sz; Spark sp = {0, 0, 0, 0, 0.0f, 1.0f, 1.0f, 0}; uint32_t c0 = spark_colour(sp, 1.0f, &sz);
    CHECK((c0 >> 24) == 0 && ((c0 >> 16) & 255) > 150 && sz > 0.9f);
    float c4[4], w;
    fire_frames(0.0f, c4, &w); NEAR(c4[0], 0, 1e-6); NEAR(c4[1], 0, 1e-6); NEAR(c4[2], 0.125, 1e-6); NEAR(w, 0, 1e-6);
    fire_frames(1.0f / 24.0f, c4, &w); NEAR(w, 0.5, 1e-4);
    fire_frames(47.5f / 12.0f, c4, &w); NEAR(c4[0], 7 / 8.0, 1e-6); NEAR(c4[1], 5 / 8.0, 1e-6); NEAR(c4[2], 0, 1e-6); NEAR(c4[3], 0, 1e-6);   // 47 -> 0
    fire_frames(4.0f, c4, &w); NEAR(c4[0], 0, 1e-6); NEAR(c4[1], 0, 1e-6);         // the 4 s loop
    CHECK(premul(1, 2, 3, 4) == 0x04010203u);
    uint8_t nt[64 * 64]; noise_tile(nt, 64, 64, 16, 16, 3, 9);
    int lo = 255, hi = 0; for (int i = 0; i < 64 * 64; ++i) { if (nt[i] < lo) lo = nt[i]; if (nt[i] > hi) hi = nt[i]; }
    CHECK(lo == 0 && hi == 255);
}

int main() {
    test_affine(); test_title_quad(); test_region_place(); test_burn(); test_timeline(); test_sparks_fire();
    printf("test_mayhem: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
