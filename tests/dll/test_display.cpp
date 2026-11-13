// test_display.cpp - unit tests of the [display] math (dll/src/gp_display_logic.cpp). Built and run by
// build.bat. No game, no game memory. Exit code 0 = all passed.
#include "../../dll/src/gp_display_logic.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace gpd;

static void test_fit() {
    Fit f = fit(1920, 1080);                    // 16:9: 1440 x 1080 in the middle, 240 px each side
    NEAR(f.s, 1.8, 1e-6); NEAR(f.ox, 240, 1e-3); NEAR(f.oy, 0, 1e-3);
    f = fit(800, 600);  NEAR(f.s, 1, 1e-6); NEAR(f.ox, 0, 1e-6); NEAR(f.oy, 0, 1e-6);
    f = fit(1600, 1200); NEAR(f.s, 2, 1e-6); NEAR(f.ox, 0, 1e-4); NEAR(f.oy, 0, 1e-4);
    f = fit(1280, 1024);                        // 5:4: letterbox
    NEAR(f.s, 1.6, 1e-6); NEAR(f.ox, 0, 1e-3); NEAR(f.oy, 32, 1e-3);
    f = fit(2560, 1080);                        // 21:9
    NEAR(f.s, 1.8, 1e-6); NEAR(f.ox, 560, 1e-3);
    f = fit(0, 0); NEAR(f.s, 1, 0); NEAR(f.ox, 0, 0);
}

static void test_classify() {
    // menu frame bars (HUD.fragment): TopBlackBg 800x75 at (0,0), HeaderLineTop 800x8 at (0,90)
    CHECK(classify(400, 37.5f, 400, 37.5f) == CLS_BAND);
    CHECK(classify(400, 94, 400, 4) == CLS_BAND);
    CHECK(classify(400, 300, 400, 300) == CLS_FULL);      // YellowBG / FadeToColorBG 800x600
    CHECK(classify(400, 300, 401, 301) == CLS_FULL);
    CHECK(classify(400, 326.5f, 353.5f, 170.5f) == CLS_BOX);  // EffectBG 707x341 at (47,156)
    CHECK(classify(400, 542, 309, 7) == CLS_BOX);         // load bar frame 618x14 at (91,535)
    CHECK(classify(400, 300, -400, -300) == CLS_FULL);    // negative half sizes (mirrored sprites)
    CHECK(classify(100, 300, 100, 300) == CLS_BOX);       // full height only: fitted
    CHECK(mode_from_name("Cover", 1) == MODE_COVER && mode_from_name("fit", 1) == MODE_FIT &&
          mode_from_name("stretch", 0) == MODE_STRETCH && mode_from_name("x", 2) == 2 && mode_from_name(nullptr, 1) == 1);
}

static void map_pt(const Map& m, float x, float y, float* px, float* py) { *px = m.tx + m.ax * x; *py = m.ty + m.ay * y; }

static void test_sprite_map() {
    float sx = 1920 * 0.00125f, sy = 1080 / 600.0f;
    float x, y;
    Map b = sprite_map(sx, sy, CLS_BOX, MODE_STRETCH, MODE_STRETCH);
    map_pt(b, 0, 0, &x, &y); NEAR(x, 240, 0.01); NEAR(y, 0, 0.01);
    map_pt(b, 800, 600, &x, &y); NEAR(x, 1680, 0.01); NEAR(y, 1080, 0.01);
    NEAR(b.ax, b.ay, 0);                         // square pixels: no stretch
    Map band = sprite_map(sx, sy, CLS_BAND, MODE_STRETCH, MODE_STRETCH);
    map_pt(band, 0, 525, &x, &y); NEAR(x, 0, 0.01); NEAR(y, 945, 0.01);
    map_pt(band, 800, 600, &x, &y); NEAR(x, 1920, 0.01); NEAR(y, 1080, 0.01);
    Map bandfit = sprite_map(sx, sy, CLS_BAND, MODE_FIT, MODE_STRETCH);
    NEAR(bandfit.tx, 240, 0.01); NEAR(bandfit.ax, 1.8, 1e-6);
    Map full = sprite_map(sx, sy, CLS_FULL, MODE_STRETCH, MODE_STRETCH);
    NEAR(full.ax, sx, 1e-6); NEAR(full.ay, sy, 1e-6); NEAR(full.tx, 0, 0); NEAR(full.ty, 0, 0);
    Map cover = sprite_map(sx, sy, CLS_FULL, MODE_STRETCH, MODE_COVER);   // 2.4 uniform, 1440 high: 180 cropped each side
    NEAR(cover.ax, 2.4, 1e-6); NEAR(cover.ay, 2.4, 1e-6); NEAR(cover.tx, 0, 0.01); NEAR(cover.ty, -180, 0.01);
    Map fullfit = sprite_map(sx, sy, CLS_FULL, MODE_STRETCH, MODE_FIT);
    NEAR(fullfit.tx, 240, 0.01);
    // 5:4 letterbox: a band keeps the full width, y follows the fit
    float sx2 = 1280 * 0.00125f, sy2 = 1024 / 600.0f;
    Map band2 = sprite_map(sx2, sy2, CLS_BAND, MODE_STRETCH, MODE_STRETCH);
    map_pt(band2, 0, 0, &x, &y); NEAR(x, 0, 0.01); NEAR(y, 32, 0.01);
    // at 4:3 every mode equals vanilla
    for (int c = 0; c < 3; ++c) for (int m = 0; m < 3; ++m) {
        Map v = sprite_map(2.0f, 2.0f, (Cls)c, m, m);
        NEAR(v.ax, 2, 1e-6); NEAR(v.ay, 2, 1e-6); NEAR(v.tx, 0, 1e-3); NEAR(v.ty, 0, 1e-3);
    }
    float mtx[16];
    for (int i = 0; i < 16; ++i) mtx[i] = 99.0f;
    write_matrix(mtx, b, 1.0f);
    CHECK(mtx[0] == b.ax && mtx[5] == b.ay && mtx[10] == 1.0f && mtx[15] == 1.0f && mtx[12] == b.tx && mtx[13] == b.ty);
    int zeros = 0; for (int i = 0; i < 16; ++i) if (i != 0 && i != 5 && i != 10 && i != 12 && i != 13 && i != 15 && mtx[i] == 0.0f) ++zeros;
    CHECK(zeros == 10);
}

static void test_tag() {
    float s = 1.8f, t = tag_scale(s);
    NEAR(t, s, 1e-6);                            // invisible
    uint32_t tb; memcpy(&tb, &t, 4);
    CHECK(tb & 1u);
    CHECK(is_tagged(tb, tb, t));
    float one = 1.0f; uint32_t ob; memcpy(&ob, &one, 4);
    CHECK(!is_tagged(ob, ob, tag_scale(1.0f)));  // vanilla 1.0 (the reset value) never matches
    CHECK(is_tagged(0x3F800001u, 0x3F800001u, tag_scale(1.0f)));
    CHECK(!is_tagged(tb, ob, t));
    float z = 0.0f; CHECK(!is_tagged(0, 0, z));  // nothing tagged yet
}

static void test_mouse_loadbar() {
    Fit f = fit(1920, 1080);
    float u, v;
    mouse_to_canvas(f, 240, 0, &u, &v); NEAR(u, 0, 1e-3); NEAR(v, 0, 1e-3);
    mouse_to_canvas(f, 960, 540, &u, &v); NEAR(u, 400, 1e-3); NEAR(v, 300, 1e-3);
    mouse_to_canvas(f, 1680, 1080, &u, &v); NEAR(u, 800, 1e-3); NEAR(v, 600, 1e-3);
    // a canvas point -> fitted pixels -> back through the mouse mapping
    Map b = sprite_map(2.4f, 1.8f, CLS_BOX, MODE_STRETCH, MODE_STRETCH);
    float px, py; map_pt(b, 87, 220, &px, &py);
    mouse_to_canvas(f, px, py, &u, &v); NEAR(u, 87, 1e-3); NEAR(v, 220, 1e-3);
    // load bar: vanilla pixel x = 91 * 2.4 -> fitted 240 + 91 * 1.8
    float x = 91 * 2.4f, y = 535 * 1.8f;
    loadbar_point(2.4f, 1.8f, f, &x, &y); NEAR(x, 240 + 91 * 1.8, 1e-2); NEAR(y, 535 * 1.8, 1e-2);
    // the fitted load bar lines up with its frame sprite (a fitted box)
    float fx, fy; map_pt(b, 91, 535, &fx, &fy); NEAR(x, fx, 1e-2); NEAR(y, fy, 1e-2);
}

static void test_res() {
    Res r[5];
    CHECK(res_table("auto", 1920, 1080, r));
    CHECK(r[0].w == 1280 && r[0].h == 720 && r[1].w == 1600 && r[1].h == 900 && r[2].w == 1920 && r[2].h == 1080 &&
          r[3].w == 2560 && r[3].h == 1440 && r[4].w == 1920 && r[4].h == 1080);
    CHECK(res_table("auto", 2560, 1080, r) && r[4].w == 2560 && r[4].h == 1080);
    CHECK(res_table("AUTO", 0, 0, r) && r[4].w == 1920 && r[4].h == 1080);   // no configured size
    CHECK(res_table(" 1280x720, 1366 x 768,1600x900 , 1920x1080,3840x2160", 0, 0, r));
    CHECK(r[1].w == 1366 && r[1].h == 768 && r[4].w == 3840 && r[4].h == 2160);
    Res keep[5] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}};
    CHECK(!res_table("1280x720,1600x900", 0, 0, keep) && keep[0].w == 1);  // 4 entries
    CHECK(!res_table("1280x720,1600x900,1920x1080,2560x1440,3840x2160,640x480", 0, 0, keep) && keep[0].w == 1);
    CHECK(!res_table("1280x720,1600x900,1920x1080,2560x1440,10x10", 0, 0, keep) && keep[0].w == 1);
    CHECK(!res_table("off", 0, 0, keep) && !res_table(nullptr, 0, 0, keep));
    int w = 0, h = 0;
    CHECK(parse_screensize("fullscreen = true\r\nscreensize = 1920,1080\r\nfilesystem = a.zip\r\n", &w, &h) && w == 1920 && h == 1080);
    CHECK(parse_screensize("screensize=1600, 1200\n# screensize = 1,1\nscreensize = 2560 , 1440\n", &w, &h) && w == 2560 && h == 1440);
    w = 7; CHECK(!parse_screensize("fullscreen = true\n", &w, &h) && w == 7);
    CHECK(!parse_screensize("screensize = 10,10\n", &w, &h));
    CHECK(!parse_screensize("screensizex = 1920,1080\n", &w, &h));
}

static void test_hud() {
    CHECK(hud_height(1.0f) == 480 && hud_height(0.8f) == 600 && hud_height(2.0f) == 240 && hud_height(0.1f) == 1920 && hud_height(9.0f) == 120);
    Hud h = hud(1920, 1080, 480);
    CHECK(h.vw == 853 && h.vh == 480);
    NEAR(h.dot_x, 436.5, 1e-3); NEAR(h.dot_y, 176, 1e-3);
    h = hud(640, 480, 480);                     // 4:3 = vanilla
    CHECK(h.vw == 640); NEAR(h.dot_x, 330, 1e-3);
    h = hud(1920, 1080, 600);                   // hud_scale 0.8
    CHECK(h.vw == 1067 && h.vh == 600); NEAR(h.dot_y, 220, 1e-3);
    h = hud(2560, 1080, 480); CHECK(h.vw == 1138);
    h = hud(0, 0, 0); CHECK(h.vw == 640 && h.vh == 480);
}

// ---- display modes (SETTINGS > SCREEN RESOLUTION, mode_menu)
static const Mon MON_3_2 = {3024, 1964, 3024, 1892, 22, 67};   // a 3:2 screen at 150 % (task bar 72 px)
static const Mon MON_1080 = {1920, 1080, 1920, 1040, 16, 39};  // a 1080p screen at 100 %
static bool has(const DMode* l, int n, int w, int h, int k) { return find_mode(l, n, DMode{w, h, k}) >= 0; }

static void test_modes() {
    Res std_[64]; int ns = standard_sizes(std_, 64);
    CHECK(ns >= 20);
    // the adapter of the 3:2 screen (a subset of what EnumAdapterModes reports there)
    const Res ad[] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 800}, {1280, 1024}, {1600, 900}, {1600, 1200},
                      {1680, 1050}, {1920, 1080}, {1920, 1200}, {1920, 1280}, {2160, 1440}, {2560, 1440}, {2560, 1600}, {3024, 1964},
                      {1366, 768}, {3840, 2160}};
    const int nad = (int)(sizeof ad / sizeof ad[0]);
    DMode l[64];
    DMode cur = {1920, 1080, K_WINDOWED};
    int n = build_modes(MON_3_2, ad, nad, true, &cur, l, 64);
    CHECK(n > 10 && n <= 40);
    CHECK(has(l, n, 1920, 1080, K_WINDOWED) && has(l, n, 1280, 720, K_WINDOWED) && has(l, n, 2560, 1600, K_WINDOWED) && has(l, n, 2160, 1440, K_WINDOWED));
    CHECK(!has(l, n, 3200, 1800, K_WINDOWED));             // 1800 + 67 > 1892
    CHECK(!has(l, n, 3024, 1964, K_WINDOWED));             // the monitor's size never fits a framed window
    CHECK(!has(l, n, 2560, 1080, K_WINDOWED));             // 21:9 only on ultrawide monitors
    CHECK(has(l, n, 3024, 1964, K_BORDERLESS));
    int nb = 0; for (int i = 0; i < n; ++i) nb += l[i].kind == K_BORDERLESS; CHECK(nb == 1);
    CHECK(has(l, n, 3024, 1964, K_FULLSCREEN) && has(l, n, 1920, 1080, K_FULLSCREEN) && has(l, n, 1600, 1200, K_FULLSCREEN));
    CHECK(!has(l, n, 1366, 768, K_FULLSCREEN) && !has(l, n, 800, 600, K_FULLSCREEN) && !has(l, n, 1280, 1024, K_FULLSCREEN));
    CHECK(!has(l, n, 3840, 2160, K_FULLSCREEN));           // above the monitor's size
    for (int i = 1; i < n; ++i) {                          // sorted: kind, then area; no duplicates
        CHECK(l[i - 1].kind < l[i].kind || (l[i - 1].kind == l[i].kind && (long long)l[i - 1].w * l[i - 1].h <= (long long)l[i].w * l[i].h));
        CHECK(!same_mode(l[i - 1], l[i]));
    }
    // fullscreen not possible (window on a secondary monitor): no fullscreen values
    int n2 = build_modes(MON_3_2, ad, nad, false, &cur, l, 64);
    for (int i = 0; i < n2; ++i) CHECK(l[i].kind != K_FULLSCREEN);
    // an odd current size is offered too (so the row can show it)
    DMode odd = {1366, 700, K_WINDOWED};
    n2 = build_modes(MON_3_2, ad, nad, true, &odd, l, 64);
    CHECK(has(l, n2, 1366, 700, K_WINDOWED));
    // a 1080p screen: windowed only below 1080 high
    n2 = build_modes(MON_1080, ad, nad, true, &cur, l, 64);
    CHECK(has(l, n2, 1920, 1080, K_WINDOWED));             // the current mode is always offered
    CHECK(has(l, n2, 1600, 900, K_WINDOWED) && !has(l, n2, 1680, 1050, K_WINDOWED) && has(l, n2, 1920, 1080, K_BORDERLESS));
    // the cap
    CHECK(build_modes(MON_3_2, ad, nad, true, &cur, l, 5) == 5);
    // ultrawide monitor offers 21:9 windowed
    Mon uw = {3440, 1440, 3440, 1400, 16, 39};
    n2 = build_modes(uw, ad, nad, true, nullptr, l, 64);
    CHECK(has(l, n2, 2560, 1080, K_WINDOWED) && has(l, n2, 3440, 1440, K_BORDERLESS));

    char b[64];
    label(DMode{1920, 1080, K_WINDOWED}, 3024, 1964, b, sizeof b); CHECK(!strcmp(b, "1920X1080 WINDOW"));
    label(DMode{3024, 1964, K_BORDERLESS}, 3024, 1964, b, sizeof b); CHECK(!strcmp(b, "3024X1964 NATIVE"));
    label(DMode{3024, 1964, K_FULLSCREEN}, 3024, 1964, b, sizeof b); CHECK(!strcmp(b, "3024X1964 FULL"));
    label(DMode{1920, 1080, K_FULLSCREEN}, 3024, 1964, b, sizeof b); CHECK(!strcmp(b, "1920X1080 FULL"));
    CHECK(kind_from_name("Borderless") == K_BORDERLESS && kind_from_name(" windowed") == K_WINDOWED && kind_from_name("fullscreen") == K_FULLSCREEN &&
          kind_from_name("x") == -1 && kind_from_name(nullptr) == -1);
    NEAR(label_scale(11, 18), 1.0, 0); NEAR(label_scale(29, 18), 18.0 / 29.0, 1e-6); NEAR(label_scale(60, 18), 0.6, 1e-6);
}

static void test_mode_file() {
    char t[512]; DMode m; int x = 9, y = 9; bool hm = false;
    int n = format_mode_file(DMode{3024, 1964, K_BORDERLESS}, -3024, 0, t, sizeof t);
    CHECK(n > 0 && n == (int)strlen(t));
    CHECK(parse_mode_file(t, &m, &x, &y, &hm) && m.w == 3024 && m.h == 1964 && m.kind == K_BORDERLESS && hm && x == -3024 && y == 0);
    CHECK(parse_mode_file("mode=windowed\nwidth=1280\nheight=720\n", &m, &x, &y, &hm) && m.kind == K_WINDOWED && m.w == 1280 && !hm);
    CHECK(!parse_mode_file("mode = sideways\nwidth = 1280\nheight = 720\n", &m, nullptr, nullptr, nullptr));
    CHECK(!parse_mode_file("mode = windowed\nwidth = 12\nheight = 720\n", &m, nullptr, nullptr, nullptr));
    CHECK(!parse_mode_file("", &m, nullptr, nullptr, nullptr) && !parse_mode_file(nullptr, &m, nullptr, nullptr, nullptr));
    bool fs = false;
    CHECK(parse_fullscreen("fullscreen = true\r\nscreensize = 1920,1080\r\n", &fs) && fs);
    CHECK(parse_fullscreen("fullscreen = false\r\n", &fs) && !fs);
    CHECK(!parse_fullscreen("screensize = 1,1\n# fullscreen = true\n", &fs));
}

static void test_startup_vanilla() {
    const Res ad[] = {{1920, 1080}, {1600, 1200}, {3024, 1964}};
    // first start without a side file
    DMode d = startup_default(false, 1920, 1080, false, MON_3_2, ad, 3, true);
    CHECK(d.kind == K_WINDOWED && d.w == 1920 && d.h == 1080);
    d = startup_default(true, 3024, 1964, true, MON_3_2, ad, 3, true);
    CHECK(d.kind == K_BORDERLESS && d.w == 3024 && d.h == 1964);
    d = startup_default(true, 1920, 1080, false, MON_3_2, ad, 3, true);
    CHECK(d.kind == K_FULLSCREEN && d.w == 1920 && d.h == 1080);
    d = startup_default(true, 2560, 1440, false, MON_3_2, ad, 3, true);   // not offered: borderless at the monitor's size
    CHECK(d.kind == K_BORDERLESS && d.w == 3024);
    d = startup_default(true, 1920, 1080, false, MON_3_2, ad, 3, false);  // exclusive not possible here
    CHECK(d.kind == K_BORDERLESS);
    d = startup_default(false, 0, 0, false, MON_3_2, ad, 3, true);        // no screensize: 1920x1080
    CHECK(d.kind == K_WINDOWED && d.w == 1920 && d.h == 1080);
    // validate: a windowed size that does not fit shrinks (same aspect first)
    d = validate(DMode{3840, 2160, K_WINDOWED}, MON_3_2, ad, 3, true);
    CHECK(d.kind == K_WINDOWED && d.w == 2560 && d.h == 1440);
    d = validate(DMode{1920, 1080, K_WINDOWED}, MON_1080, ad, 3, true);
    CHECK(d.kind == K_WINDOWED && d.w == 1600 && d.h == 900);
    d = validate(DMode{1920, 1080, K_BORDERLESS}, MON_3_2, ad, 3, true);  // borderless follows the monitor
    CHECK(d.w == 3024 && d.h == 1964);
    // the vanilla game (DPI unaware at 150 %: logical work area 2016 x 1261)
    Res v = vanilla_window(DMode{1920, 1080, K_WINDOWED}, MON_3_2, 1.5f);
    CHECK(v.w == 1920 && v.h == 1080);
    v = vanilla_window(DMode{2560, 1440, K_WINDOWED}, MON_3_2, 1.5f);
    CHECK(v.w == 1920 && v.h == 1080);
    v = vanilla_window(DMode{3024, 1964, K_BORDERLESS}, MON_3_2, 1.5f);
    CHECK(v.w == 1920 && v.h == 1080);
    v = vanilla_window(DMode{1280, 720, K_WINDOWED}, MON_3_2, 1.5f);
    CHECK(v.w == 1280 && v.h == 720);
    CHECK(vanilla_index(Res{1920, 1080}, MON_3_2, 1.5f) == 3);   // 1280x1024: not taller than the window
    CHECK(vanilla_index(Res{1280, 720}, MON_3_2, 1.5f) == 1);    // 800x600
    CHECK(vanilla_index(Res{1600, 900}, MON_1080, 1.0f) == 2);   // 1024x768
    v = vanilla_window(DMode{1920, 1080, K_WINDOWED}, MON_1080, 1.0f);   // does not fit with its frame at 1080
    CHECK(v.w == 1600 && v.h == 900);
}

static void test_configpc() {
    char out[1024];
    const char* user = "fullscreen = false\r\nscreensize = 1920,1080\r\n# TOD Gameplay+ data options\r\nfilesystem = todgameplay_data.zip, blocks.naz\r\n";
    int n = configpc_set(user, false, 1920, 1080, out, sizeof out);
    CHECK(n == (int)strlen(user) && !strcmp(out, user));                 // unchanged values: byte-identical
    n = configpc_set(user, true, 3024, 1964, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "fullscreen = true\r\nscreensize = 3024,1964\r\n# TOD Gameplay+ data options\r\nfilesystem = todgameplay_data.zip, blocks.naz\r\n"));
    n = configpc_set("filesystem = blocks.naz\n", false, 1280, 720, out, sizeof out);   // missing keys: added at the top
    CHECK(n > 0 && !strcmp(out, "fullscreen = false\nscreensize = 1280,720\nfilesystem = blocks.naz\n"));
    n = configpc_set("  screensize=800,600\nfullscreen =true", false, 1600, 900, out, sizeof out);   // indent kept, last line without EOL
    CHECK(n > 0 && !strcmp(out, "  screensize = 1600,900\nfullscreen = false"));
    n = configpc_set("# fullscreen = true\nscreensizes = 1\n", false, 1600, 900, out, sizeof out);    // comments / other keys untouched
    CHECK(n > 0 && !strcmp(out, "fullscreen = false\nscreensize = 1600,900\n# fullscreen = true\nscreensizes = 1\n"));
    CHECK(configpc_set(user, false, 1, 1, out, 20) == -1);
}

int main() {
    test_fit(); test_classify(); test_sprite_map(); test_tag(); test_mouse_loadbar(); test_res(); test_hud();
    test_modes(); test_mode_file(); test_startup_vanilla(); test_configpc();
    printf("display unit tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
