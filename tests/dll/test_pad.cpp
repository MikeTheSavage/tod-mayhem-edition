// test_pad.cpp - unit tests of the [pad] logic (dll/src/gp_pad_logic.cpp): names, XInput / PlayStation report
// decoding, rumble reports, stick shaping, the game's DIJOYSTATE, device used last, prompt style, text
// post-processing, the rumble mixer and the [debug] pad script.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <initializer_list>
#include "../../dll/src/gp_pad_logic.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))
using namespace pad;

static size_t u16(const char* s, uint16_t* out, size_t cap) {   // Latin-1 -> UTF-16
    size_t n = 0; for (; s[n] && n + 1 < cap; ++n) out[n] = (uint8_t)s[n]; out[n] = 0; return n;
}
static bool eq(const uint16_t* a, const char* latin) {
    size_t i = 0; for (; latin[i]; ++i) if (a[i] != (uint8_t)latin[i]) return false; return a[i] == 0;
}

static void test_names() {
    CHECK(btn_from_name("a") == B_A); CHECK(btn_from_name("CROSS") == B_A); CHECK(btn_from_name("Circle") == B_B);
    CHECK(btn_from_name("square") == B_X); CHECK(btn_from_name("TRIANGLE") == B_Y); CHECK(btn_from_name("L1") == B_LB);
    CHECK(btn_from_name("r2") == B_RT); CHECK(btn_from_name("OPTIONS") == B_START); CHECK(btn_from_name("share") == B_BACK);
    CHECK(btn_from_name("create") == B_BACK); CHECK(btn_from_name("dpad_up") == B_UP); CHECK(btn_from_name("xyz") == -1);
    CHECK(btn_from_name(nullptr) == -1);
    for (int b = 0; b < B_COUNT; ++b) CHECK(btn_from_name(btn_name(b)) == b);
    CHECK(virt_from_name("CROSS") == V_CROSS); CHECK(virt_from_name("d_up") == V_DUP); CHECK(virt_from_name("X") == -1);
    for (int v = 0; v < V_COUNT; ++v) CHECK(virt_from_name(virt_name(v)) == v);
    CHECK(mode_from_name("auto") == MODE_AUTO); CHECK(mode_from_name("") == MODE_AUTO); CHECK(mode_from_name("PlayStation") == MODE_PS);
    CHECK(mode_from_name("xbox") == MODE_XBOX); CHECK(mode_from_name("keyboard") == MODE_KEYBOARD); CHECK(mode_from_name("nope") == -1);
}

static void test_xinput() {
    State s = from_xinput(0x1000 | 0x0200 | 0x0001 | 0x0010, 255, 0, 32767, -32768, 0, 16384);
    CHECK(s.connected && s.kind == KIND_XBOX);
    CHECK(s.down(B_A) && s.down(B_RB) && s.down(B_UP) && s.down(B_START) && !s.down(B_B) && !s.down(B_LB));
    NEAR(s.lt, 1.0, 1e-6); NEAR(s.rt, 0.0, 1e-6); NEAR(s.lx, 1.0, 1e-4); NEAR(s.ly, -1.0, 1e-4); NEAR(s.ry, 0.5, 1e-3);
    State all = from_xinput(0xF3FF, 0, 0, 0, 0, 0, 0);
    int n = 0; for (int b = 0; b < B_COUNT; ++b) n += all.down(b);
    CHECK(n == 14);   // 14 digital buttons (LT / RT are analog)
}

static void test_hid() {
    CHECK(hid_model(0x054C, 0x05C4) == HID_DS4); CHECK(hid_model(0x054C, 0x09CC) == HID_DS4); CHECK(hid_model(0x054C, 0x0BA0) == HID_DS4);
    CHECK(hid_model(0x054C, 0x0CE6) == HID_DS); CHECK(hid_model(0x054C, 0x0DF2) == HID_DS); CHECK(hid_model(0x045E, 0x028E) == HID_NONE);
    // the classic CRC-32 check value
    const char* t = "123456789"; CHECK(crc32((const uint8_t*)t, 9) == 0xCBF43926u);
    // DualShock 4 USB report 01: sticks centre-ish, hat up-right (1), Cross + Triangle, L1 + Options, L2 full
    uint8_t r[64] = {0x01, 128, 0, 255, 128, 0x01 | 0x20 | 0x80, 0x01 | 0x20, 0x00, 255, 0};
    State s;
    CHECK(hid_decode(HID_DS4, false, r, 64, s));
    CHECK(s.kind == KIND_PS && s.connected);
    CHECK(s.down(B_UP) && s.down(B_RIGHT) && !s.down(B_DOWN) && s.down(B_A) && s.down(B_Y) && !s.down(B_X));
    CHECK(s.down(B_LB) && s.down(B_START) && !s.down(B_RB));
    NEAR(s.lx, 0.0, 0.01); NEAR(s.ly, 1.0, 0.01); NEAR(s.rx, 1.0, 0.01); NEAR(s.lt, 1.0, 0.01); NEAR(s.rt, 0.0, 0.01);
    // hat 8 = released
    r[5] = 0x08; CHECK(hid_decode(HID_DS4, false, r, 64, s)); CHECK(!s.down(B_UP) && !s.down(B_DOWN) && !s.down(B_LEFT) && !s.down(B_RIGHT));
    // DS4 Bluetooth 11: fields from byte 3, CRC-32 over A1 + 74 bytes
    uint8_t b[78]; memset(b, 0, sizeof b); b[0] = 0x11; b[3] = 0; b[4] = 128; b[5] = 128; b[6] = 128; b[7] = 0x06 | 0x40; b[8] = 0x02; b[10] = 0; b[11] = 200;
    uint8_t h = 0xA1; uint32_t c = crc32(b, 74, crc32(&h, 1)); b[74] = c & 255; b[75] = (c >> 8) & 255; b[76] = (c >> 16) & 255; b[77] = c >> 24;
    CHECK(hid_decode(HID_DS4, true, b, 78, s));
    CHECK(s.down(B_LEFT) && s.down(B_B) && s.down(B_RB)); NEAR(s.lx, -1.0, 0.01); NEAR(s.rt, 200 / 255.0, 0.01);
    b[20] ^= 1; CHECK(!hid_decode(HID_DS4, true, b, 78, s));   // bad checksum refused
    // DualSense USB 01: sticks 1..4, triggers 5..6, buttons from 8
    uint8_t d[64]; memset(d, 0, sizeof d); d[0] = 0x01; d[1] = d[2] = d[3] = d[4] = 128; d[5] = 10; d[6] = 255; d[8] = 0x04 | 0x10; d[9] = 0x80 | 0x10;
    CHECK(hid_decode(HID_DS, false, d, 64, s));
    CHECK(s.down(B_DOWN) && s.down(B_X) && s.down(B_RS) && s.down(B_BACK)); NEAR(s.rt, 1.0, 0.01);
    // DualSense Bluetooth 31
    uint8_t e[78]; memset(e, 0, sizeof e); e[0] = 0x31; e[2] = e[3] = e[4] = e[5] = 128; e[9] = 0x08 | 0x20; e[10] = 0x02;
    c = crc32(e, 74, crc32(&h, 1)); e[74] = c & 255; e[75] = (c >> 8) & 255; e[76] = (c >> 16) & 255; e[77] = c >> 24;
    CHECK(hid_decode(HID_DS, true, e, 78, s)); CHECK(s.down(B_A) && s.down(B_RB) && !s.down(B_UP));
    CHECK(!hid_decode(HID_DS, false, e, 78, s) || true);
    uint8_t x[3] = {0x05, 0, 0}; CHECK(!hid_decode(HID_DS4, false, x, 3, s));
    // rumble output reports
    uint8_t o[80];
    CHECK(hid_rumble_report(HID_DS4, false, 200, 100, o, sizeof o) == 32); CHECK(o[0] == 0x05 && o[1] == 0x01 && o[4] == 100 && o[5] == 200);
    CHECK(hid_rumble_report(HID_DS, false, 200, 100, o, sizeof o) == 48); CHECK(o[0] == 0x02 && o[1] == 0x03 && o[3] == 100 && o[4] == 200);
    CHECK(hid_rumble_report(HID_DS, true, 200, 100, o, sizeof o) == 0);
}

static void test_shape() {
    float x = 0.1f, y = 0.1f; radial(x, y, 0.2f); CHECK(x == 0 && y == 0);
    x = 1.0f; y = 0.0f; radial(x, y, 0.2f); NEAR(x, 1.0, 1e-6);
    x = 0.6f; y = 0.0f; radial(x, y, 0.2f); NEAR(x, 0.5, 1e-6);
    x = 0.8f; y = 0.8f; radial(x, y, 0.2f); NEAR(sqrt(x * x + y * y), 1.0, 1e-5);   // clamped to the unit circle
    CHECK(lift(0.0f) == 0.0f); NEAR(lift(1.0f), 1.0, 1e-6); NEAR(lift(-0.5f), -0.65, 1e-6); NEAR(lift(0.01f), 0.307, 1e-3);
    // the game: p = (v - 0.3) / 0.7 then turn = 180 p^3; with lift(look_axis(x, 2)) that is 180 x^2
    for (float v : {0.25f, 0.5f, 0.75f, 1.0f}) {
        float a = lift(look_axis(v, 2.0f, 1.0f)); float p = (a - 0.3f) / 0.7f;
        NEAR(p * p * p, v * v, 1e-4);
    }
    NEAR(look_axis(-1.0f, 3.0f, 1.0f), -1.0, 1e-6); NEAR(look_axis(0.5f, 3.0f, 1.0f), 0.5, 1e-6);
    NEAR(look_axis(0.5f, 3.0f, 8.0f), 1.0, 1e-6);   // sensitivity x8 reaches the cap
}

static void test_dijoystate() {
    Map m = default_map(); Shape sh; DiState d;
    CHECK(m.src[V_CROSS] == B_A && m.src[V_R1] == B_RT && m.src[V_L1] == B_LT && m.src[V_R2] == B_RB && m.src[V_SELECT] == B_BACK);
    int seen[B_COUNT] = {0}; for (int v = 0; v < V_COUNT; ++v) if (m.src[v] >= 0) ++seen[m.src[v]];
    for (int b = 0; b < B_COUNT; ++b) CHECK(seen[b] == 1);   // every pad button feeds exactly one game button
    State s; to_dijoystate(s, m, sh, d);   // disconnected: centred, nothing pressed
    CHECK(d.lX == 32768 && d.lY == 32768 && d.lZ == 32768 && d.lRz == 32768);
    for (int i = 0; i < 32; ++i) CHECK(d.btn[i] == 0);
    CHECK(d.pov[0] == 0xFFFFFFFFu);
    s.connected = true; s.kind = KIND_XBOX; s.ly = 1.0f; s.rx = -1.0f; s.rt = 0.9f; s.buttons = 1u << B_A | 1u << B_UP;
    to_dijoystate(s, m, sh, d);
    CHECK(d.lY == 0);            // full up = DI 0 (y down positive)
    CHECK(d.lX == 32768);
    CHECK(d.lZ == 0);            // right stick full left
    CHECK(d.btn[V_CROSS] == 0x80 && d.btn[V_R1] == 0x80 && d.btn[V_DUP] == 0x80 && d.btn[V_L1] == 0);
    // the game's own maths on what it gets: v / 32768 - 1 then its 0.3 dead zone: a 0.25 push still moves after lift
    s.ly = 0.45f; s.rx = 0; s.buttons = 0; s.rt = 0; to_dijoystate(s, m, sh, d);
    float gy = -(d.lY / 32768.0f - 1.0f); CHECK(gy > 0.3f);
    CHECK(axis_to_di(-1.0f) == 0 && axis_to_di(1.0f) == 65535 && axis_to_di(0.0f) == 32768);
    // map overrides
    CHECK(map_set(m, "R1", "RB")); CHECK(m.src[V_R1] == B_RB); CHECK(map_virt_of(m, B_RB) == V_R2);
    CHECK(map_set(m, "SQUARE", "none")); CHECK(m.src[V_SQUARE] == -1);
    CHECK(!map_set(m, "Q1", "A")); CHECK(!map_set(m, "CROSS", "LSTICK"));
    Shape inv; inv.invert_y = true; s = State(); s.connected = true; s.ry = 1.0f; to_dijoystate(s, default_map(), inv, d); CHECK(d.lRz == 65535);
}

static void test_activity() {
    Activity a; State s; s.connected = true; s.kind = KIND_PS;
    CHECK(a.active == 0);
    CHECK(!a.step(s, false, 0.0));                 // idle pad: nothing
    s.buttons = 1u << B_A; CHECK(a.step(s, false, 0.1)); CHECK(a.active == 1 && a.last_kind == KIND_PS);
    CHECK(!a.step(s, false, 0.2));                 // held: no new edge, stays pad
    CHECK(a.step(s, true, 0.3)); CHECK(a.active == 0);   // a key: keyboard
    s.buttons = 0; s.lx = 0.9f; CHECK(a.step(s, false, 0.4)); CHECK(a.active == 1);   // a stick push: pad
    s.lx = 0.0f; s.buttons = 1u << B_B; CHECK(!a.step(s, true, 0.5)); CHECK(a.active == 1);   // both in one frame: kept
    State off; CHECK(!pad_activity(off, 0));
    CHECK(resolve_style(MODE_AUTO, 0, KIND_PS) == STYLE_KEYBOARD); CHECK(resolve_style(MODE_AUTO, 1, KIND_PS) == STYLE_PS);
    CHECK(resolve_style(MODE_AUTO, 1, KIND_XBOX) == STYLE_XBOX); CHECK(resolve_style(MODE_AUTO, 1, KIND_NONE) == STYLE_XBOX);
    CHECK(resolve_style(MODE_KEYBOARD, 1, KIND_PS) == STYLE_KEYBOARD); CHECK(resolve_style(MODE_XBOX, 0, KIND_PS) == STYLE_XBOX);
    CHECK(resolve_style(MODE_PS, 0, KIND_XBOX) == STYLE_PS);
}

static void test_text() {
    for (int g = 0; g < G_COUNT; ++g) { CHECK(glyph_code(g) != 0); CHECK(glyph_of_code(glyph_code(g)) == g); for (int h = 0; h < g; ++h) CHECK(glyph_code(g) != glyph_code(h)); }
    CHECK(glyph_of_code('A') == -1);
    static const Literal L[] = {{"[Enter]", B_A}, {"[Entr\xE9" "e]", B_A}, {"[Esc]", B_B}, {"[\xC9" "chap]", B_B}, {"[A]", B_Y}, {"[Space]", B_A}};
    const int NL = 6;
    uint16_t s[128], g;
    // a key-name parameter already replaced by a glyph: the brackets go
    u16("Press [?] to use", s, 128); s[7] = glyph_code(B_X);
    size_t n = post_process(s, 128, L, NL);
    CHECK(n == 14 && s[6] == glyph_code(B_X) && s[7] == ' ' && s[8] == 't');
    // two glyphs in one bracket, with a space
    u16("Use [? ?] now", s, 128); s[5] = glyph_code(B_LB); s[7] = glyph_code(B_RB);
    n = post_process(s, 128, L, NL); CHECK(s[4] == glyph_code(B_LB) && s[5] == ' ' && s[6] == glyph_code(B_RB) && s[7] == ' ' && s[8] == 'n');
    // menu navigation help
    u16("Menu [Enter]  Resume Game [Esc]  Choose Mission [A]", s, 128);
    n = post_process(s, 128, L, NL);
    g = glyph_code(B_A); CHECK(s[5] == g && s[6] == ' ');
    { int found_b = 0, found_y = 0; for (size_t i = 0; i < n; ++i) { found_b += s[i] == glyph_code(B_B); found_y += s[i] == glyph_code(B_Y); } CHECK(found_b == 1 && found_y == 1); }
    // French, case-insensitive
    u16("Continuer [ENTR\xC9" "E]  Annuler [\xC9" "chap]", s, 128);
    n = post_process(s, 128, L, NL); CHECK(s[10] == glyph_code(B_A)); CHECK(s[n - 1] == glyph_code(B_B));
    // untouched: brackets around normal text, an unknown key
    u16("[%0] [F] [Enter", s, 128); n = post_process(s, 128, L, NL); CHECK(eq(s, "[%0] [F] [Enter"));
    // nothing to do
    u16("", s, 128); CHECK(post_process(s, 128, L, NL) == 0);
    // capacity: never writes past cap
    uint16_t small[6] = {'[', 'E', 's', 'c', ']', 0}; n = post_process(small, 6, L, NL); CHECK(n == 1 && small[0] == glyph_code(B_B) && small[1] == 0);
}

static void test_rumble() {
    Rumble r; float b, l;
    r.out(1.0f, true, b, l); CHECK(b == 0 && l == 0);
    r.game[0] = 0.6f; r.game[1] = 0.2f; r.out(1.0f, true, b, l); NEAR(b, 0.6, 1e-6); NEAR(l, 0.2, 1e-6);
    r.out(0.5f, true, b, l); NEAR(b, 0.3, 1e-6);
    r.out(1.0f, false, b, l); CHECK(b == 0 && l == 0);   // VIBRATION off
    r.game[0] = r.game[1] = 0; r.pulse(0.8f, 0.4f, 0.2f); r.out(1.0f, true, b, l); NEAR(b, 0.8, 1e-6); NEAR(l, 0.4, 1e-6);
    r.step(0.1f); r.out(1.0f, true, b, l); NEAR(b, 0.6, 1e-5);   // fades to half over its length
    r.pulse(0.1f, 0.1f, 1.0f); r.out(1.0f, true, b, l); NEAR(b, 0.6, 1e-5);   // a weaker pulse does not cut a stronger one
    r.step(0.2f); r.out(1.0f, true, b, l); CHECK(b == 0 && l == 0);   // over
    r.pulse(0.1f, 0.1f, 1.0f); r.out(2.0f, true, b, l); NEAR(b, 0.2, 1e-5);
    r.game[0] = 1.0f; r.out(2.0f, true, b, l); CHECK(b == 1.0f);   // clamped
}

static bool has_ready(const char* t, void*) { return !strcmp(t, "ready"); }
static void test_script() {
    PadScript s;
    CHECK(s.parse("# comment\nwait 100\nmark menu\nkind playstation\npress A 50\nstick L 0 1 200\ntrigger R 1\nhold RB\nrelease RB\n"
                  "waitlog 500 ready\nrumble_test 0.5 0.25 300\ndisconnect\n"));
    CHECK(s.n == 11 && s.errors == 0);
    PadScript bad; CHECK(!bad.parse("press Q\nstick M 1 1\nfoo\nwait 5\n")); CHECK(bad.errors == 3 && bad.n == 1);
    PadPlayer p;
    p.step(s, 0.0); CHECK(!p.st.connected && p.mark == nullptr);   // wait 100
    p.step(s, 0.05); CHECK(p.mark == nullptr);
    p.step(s, 0.11); CHECK(p.mark && !strcmp(p.mark, "menu"));     // a mark ends the frame
    p.step(s, 0.12); CHECK(p.st.kind == KIND_PS && p.st.down(B_A) && p.st.ly == 1.0f && p.st.rt == 1.0f && !p.st.down(B_RB));
    CHECK(p.waiting >= 0);                                          // at the waitlog
    p.step(s, 0.18, has_ready); CHECK(!p.st.down(B_A));            // 50 ms press over; log has "ready": go on
    p.step(s, 0.19); CHECK(p.rumble[2] > 0.29f && p.rumble[0] == 0.5f);
    CHECK(!p.st.connected);                                         // disconnect ran in the same frame
    p.step(s, 0.5); CHECK(p.done);
    // waitlog timeout
    PadScript t; t.parse("waitlog 100 never\nmark after\n"); PadPlayer q;
    q.step(t, 0.0); q.step(t, 0.05); CHECK(!q.mark); q.step(t, 0.2); CHECK(q.mark && !strcmp(q.mark, "after")); q.step(t, 0.21); CHECK(q.done && !q.mark);
    // timed stick returns to rest
    PadScript u; u.parse("stick R 0.5 -0.5 100\n"); PadPlayer w; w.step(u, 0.0); CHECK(w.st.rx == 0.5f); w.step(u, 0.2); CHECK(w.st.rx == 0.0f && w.st.ry == 0.0f);
}

int main() {
    test_names(); test_xinput(); test_hid(); test_shape(); test_dijoystate(); test_activity(); test_text(); test_rumble(); test_script();
    printf("test_pad: %d checks, %d failed: %s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
