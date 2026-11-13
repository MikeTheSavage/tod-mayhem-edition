// test_logic.cpp - unit tests of the pure logic of TODGameplay.dll (dll/src/gp_logic.cpp). Built and run by build.bat.
// No game, no game memory. Exit code 0 = all passed.
#include "../../dll/src/gp_logic.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace gp;

static void test_ini() {
    Ini ini;
    ini.parse("; comment\n[BulletTime]\r\nenabled = 1 ; on\nscale=0.25\nkey = MOUSE3\n[keys]\njump = SPACE\n"
              "[general]\nlog = off\n[bullettime]\nscale = 0.3   # later wins\n");
    CHECK(ini.geti("bullettime", "enabled", 0) == 1);
    NEAR(ini.getf("bullettime", "scale", 0), 0.3, 1e-6);
    CHECK(!strcmp(ini.get("BULLETTIME", "KEY"), "MOUSE3"));
    CHECK(ini.geti("general", "log", 1) == 0);
    CHECK(ini.get("keys", "jump") && !strcmp(ini.get("keys", "jump"), "SPACE"));
    CHECK(ini.get("nope", "x") == nullptr);
    CHECK(ini.geti("bullettime", "missing", 7) == 7);
    Ini empty; empty.parse("");
    CHECK(empty.n == 0);
}

static void test_keys() {
    CHECK(key_from_name("SPACE") == 0x39);
    CHECK(key_from_name("lshift") == 0x2A);
    CHECK(key_from_name("DIK_F10") == 0x44);
    CHECK(key_from_name("MOUSE3") == KEY_MOUSE + 2);
    CHECK(key_from_name("PAD_RS") == KEY_PAD + 7);
    CHECK(key_from_name("0x39") == 0x39);
    CHECK(key_from_name("NONE") == KEY_NONE);
    CHECK(key_from_name("bogus") == -2);
    CHECK(action_default_key("jump") == 0x2A);
    CHECK(action_default_key("shootdodge") == 0x39);
    CHECK(action_default_key("nothing") == -1);
    CHECK(!strcmp(key_name(0x39), "SPACE"));
}

static void test_remap() {
    KeyRemap r; r.clear();
    uint8_t e[256], s[256]; bool md[8] = {}, mp[8] = {};
    // no binding: untouched
    memset(e, 0, 256); memset(s, 0, 256); s[0x39] = 0x80; e[0x39] = 1;
    r.apply(e, s, md, mp);
    CHECK(s[0x39] == 0x80 && e[0x39] == 1);
    // swap jump (LSHIFT) and shootdodge (SPACE)
    CHECK(r.bind(0x2A, 0x39)); CHECK(r.bind(0x39, 0x2A));
    memset(e, 0, 256); memset(s, 0, 256); s[0x39] = 0x80; e[0x39] = 1;   // physical SPACE pressed
    r.apply(e, s, md, mp);
    CHECK(s[0x2A] == 0x80 && e[0x2A] == 1);   // the game sees LSHIFT (jump)
    CHECK(s[0x39] == 0 && e[0x39] == 0);      // and not SPACE
    // one-way move: jump on X; physical X loses its own function, LSHIFT keeps nothing (bound to X)
    r.clear(); CHECK(r.bind(0x2A, 0x2D));
    memset(e, 0, 256); memset(s, 0, 256); s[0x2D] = 0x80; s[0x2A] = 0x80;
    r.apply(e, s, md, mp);
    CHECK(s[0x2A] == 0x80);   // from X
    CHECK(s[0x2D] == 0);      // X itself cleared
    memset(s, 0, 256); s[0x2A] = 0x80;     // only physical LSHIFT: no jump any more
    r.apply(e, s, md, mp);
    CHECK(s[0x2A] == 0);
    // unbind
    r.clear(); CHECK(r.bind(0x0F, KEY_NONE));
    memset(s, 0, 256); s[0x0F] = 0x80; r.apply(e, s, md, mp); CHECK(s[0x0F] == 0);
    // mouse source with edges
    r.clear(); CHECK(r.bind(0x39, KEY_MOUSE + 3));
    memset(e, 0, 256); memset(s, 0, 256); md[3] = true; mp[3] = false;
    r.apply(e, s, md, mp); CHECK(s[0x39] == 0x80 && e[0x39] == 1);
    md[3] = false; mp[3] = true; memset(e, 0, 256);
    r.apply(e, s, md, mp); CHECK(s[0x39] == 0 && e[0x39] == 2);
    CHECK(!r.bind(0x39, KEY_PAD + 1));   // pad buttons cannot drive keyboard keys
    CHECK(!r.bind(300, 0x39));
}

static void test_meter() {
    MeterCfg c; Meter m; m.value = 1.0f;
    for (int i = 0; i < 400; ++i) m.drain(c, 0.01f);     // 4 s of an 8 s meter
    NEAR(m.value, 0.5, 1e-3);
    for (int i = 0; i < 400; ++i) m.drain(c, 0.01f);
    CHECK(m.empty());
    CHECK(!m.can_start(c));
    m.idle(c, 1.0f); CHECK(m.value == 0.0f);              // within the regen delay
    m.idle(c, 1.5f); NEAR(m.value, 0.03, 1e-5);           // since_use 2.5 >= 2: + 0.02 * 1.5
    m.kills(c, 2); NEAR(m.value, 0.27, 1e-5);
    m.points(c, 100); NEAR(m.value, 0.32, 1e-5);
    CHECK(m.can_start(c));
    m.kills(c, 100); CHECK(m.value == 1.0f);
}

static BtGame game(float wt, float tm, float ttu) { BtGame g; g.ok = true; g.world_time = wt; g.time_mult = tm; g.time_to_use = ttu; g.desired = tm; return g; }

static void test_bt() {
    BtCfg c; c.enabled = true; MeterCfg mc; Meter m; m.value = 1.0f; BtLogic b;
    // off -> press -> ramp in: trigger_slowmotion(scale, ramp, blur)
    BtActions a = b.tick(c, mc, m, game(1, 1, 0), true, 0.016f);
    CHECK(b.state == BT_RAMP_IN && a.send_slowmo && !a.set_world_time);
    NEAR(a.slowmo_target, 0.30, 1e-6); NEAR(a.slowmo_seconds, 0.25, 1e-6); CHECK(a.slowmo_no_blur == 0);
    // ramping: no latch yet
    a = b.tick(c, mc, m, game(1, 0.6f, 0.1f), true, 0.016f);
    CHECK(b.state == BT_RAMP_IN && !a.set_world_time && !a.send_slowmo);
    // arrived: latch WorldTime = scale
    a = b.tick(c, mc, m, game(1, 0.3f, 0.0f), true, 0.016f);
    CHECK(b.state == BT_ACTIVE && a.set_world_time); NEAR(a.world_time, 0.30, 1e-6);
    // active, want off -> WorldTime 1 + ramp back
    a = b.tick(c, mc, m, game(0.3f, 0.3f, 0), false, 0.016f);
    CHECK(b.state == BT_OFF && a.set_world_time && a.send_slowmo);
    NEAR(a.world_time, 1.0, 1e-6); NEAR(a.slowmo_target, 1.0, 1e-6);
    // a want-off during the ramp is deferred until the latch (a trigger during the interpolation would be ignored)
    b = BtLogic(); b.tick(c, mc, m, game(1, 1, 0), true, 0.016f);
    a = b.tick(c, mc, m, game(1, 0.7f, 0.2f), false, 0.016f);
    CHECK(b.state == BT_RAMP_IN && !a.send_slowmo);
    // busy controller (dive): one resend after the timeout, then latch after the second
    int resends = 0, ticks = 0;
    for (; ticks < 100 && b.state == BT_RAMP_IN; ++ticks) { a = b.tick(c, mc, m, game(1, 0.11f, 0.0f), true, 0.016f); if (a.send_slowmo) ++resends; }
    CHECK(b.state == BT_ACTIVE && resends == 1 && ticks > 60);
    // still ramping (time_to_use > 0) at the timeout: no resend, latch
    b = BtLogic(); b.tick(c, mc, m, game(1, 1, 0), true, 0.016f);
    resends = 0;
    for (ticks = 0; ticks < 100 && b.state == BT_RAMP_IN; ++ticks) { a = b.tick(c, mc, m, game(1, 0.5f, 0.1f), true, 0.016f); if (a.send_slowmo) ++resends; }
    CHECK(b.state == BT_ACTIVE && resends == 0);
    // quick re-toggle: the first trigger was ignored (time stays 1.0, controller idle): the resend brings it in
    b = BtLogic(); b.tick(c, mc, m, game(1, 1, 0), true, 0.016f);
    resends = 0;
    for (ticks = 0; ticks < 100 && b.state == BT_RAMP_IN; ++ticks) {
        float tm = resends ? 0.3f : 1.0f;
        a = b.tick(c, mc, m, game(1, tm, 0.0f), true, 0.016f); if (a.send_slowmo) ++resends;
    }
    CHECK(b.state == BT_ACTIVE && resends == 1 && a.set_world_time);
    b.state = BT_ACTIVE;
    // the game changes WorldTime (death, vehicle, mission start): give up without touching anything
    a = b.tick(c, mc, m, game(1.0f, 1.0f, 0), true, 0.016f);
    CHECK(b.state == BT_OFF && !a.set_world_time && !a.send_slowmo && b.ended_by_game == 1);
    // refused: exclusive, cut-scene, empty meter, WorldTime not 1, disabled, not in control
    BtGame g = game(1, 1, 0); g.exclusive = true;
    a = b.tick(c, mc, m, g, true, 0.016f); CHECK(b.state == BT_OFF && !a.send_slowmo);
    g = game(1, 1, 0); g.cutscene = true; a = b.tick(c, mc, m, g, true, 0.016f); CHECK(b.state == BT_OFF);
    a = b.tick(c, mc, m, game(0.01f, 0.01f, 0), true, 0.016f); CHECK(b.state == BT_OFF);   // Time Stop
    Meter e; e.value = 0.05f; a = b.tick(c, mc, e, game(1, 1, 0), true, 0.016f); CHECK(b.state == BT_OFF);
    BtCfg off = c; off.enabled = false; a = b.tick(off, mc, m, game(1, 1, 0), true, 0.016f); CHECK(b.state == BT_OFF);
    g = game(1, 1, 0); g.ok = false; a = b.tick(c, mc, m, g, true, 0.016f); CHECK(b.state == BT_OFF);
    // meter runs out while active
    b = BtLogic(); Meter s; s.value = 0.2f; mc.capacity_s = 1.0f;
    b.tick(c, mc, s, game(1, 1, 0), true, 0.016f);
    b.tick(c, mc, s, game(1, 0.3f, 0), true, 0.016f);
    CHECK(b.state == BT_ACTIVE);
    int n = 0;
    while (b.state == BT_ACTIVE && n < 100) { a = b.tick(c, mc, s, game(0.3f, 0.3f, 0), true, 0.016f); ++n; }
    CHECK(b.state == BT_OFF && a.set_world_time && a.send_slowmo && s.empty());
    CHECK(n > 5 && n < 20);
    // no blur option
    BtCfg nb = c; nb.blur = false; b = BtLogic(); m.value = 1;
    a = b.tick(nb, mc, m, game(1, 1, 0), true, 0.016f); CHECK(a.slowmo_no_blur == 1);
    // abort from the main loop
    b = BtLogic(); b.state = BT_ACTIVE;
    a = b.abort_check(game(0.3f, 0.3f, 0), false, false, false); CHECK(b.state == BT_ACTIVE && !a.send_force_normal);
    a = b.abort_check(game(0.3f, 0.3f, 0), false, true, true);  CHECK(b.state == BT_ACTIVE);          // paused
    g = game(0.3f, 0.3f, 0); g.exclusive = true;
    a = b.abort_check(g, false, true, false); CHECK(b.state == BT_ACTIVE);                             // power-up menu
    a = b.abort_check(game(0.3f, 0.3f, 0), true, false, false);
    CHECK(b.state == BT_OFF && a.send_force_normal && a.set_world_time); NEAR(a.world_time, 1.0, 1e-6);
    b.state = BT_RAMP_IN;
    a = b.abort_check(game(1, 0.5f, 0.1f), false, true, false);
    CHECK(b.state == BT_OFF && a.send_force_normal && !a.set_world_time);
}

static void test_yaw() {
    YawAcc y; y.seconds_per_frame = 0.95f / 60.0f;
    // 60 fps, an integrate every 2.5 frames: all motion arrives
    float total_in = 0, total_out = 0; uint32_t t = 1000; int frame = 0; y.last_consume_ms = t;
    for (int i = 0; i < 600; ++i) {
        float turn = (i % 7) * 10.0f;                 // deg/s from this frame's counts
        y.on_write(turn, t, 250); total_in += turn * y.seconds_per_frame;
        if (++frame % 5 == 0 || frame % 5 == 2) total_out += y.on_consume(0.04f, t) * 0.04f;   // 2 integrates / 5 frames
        t += 16;
    }
    total_out += y.on_consume(0.04f, t) * 0.04f;
    NEAR(total_out, total_in, 1e-3);
    // frame-rate independence: same counts (deg) at 240 fps vs 60 fps -> same angle
    YawAcc a, b; a.seconds_per_frame = b.seconds_per_frame = 0.95f / 60.0f; a.last_consume_ms = b.last_consume_ms = 0;
    float out60 = 0, out240 = 0;
    for (int i = 0; i < 60; ++i) { a.on_write(100.0f, 0, 250); if (i % 2) out60 += a.on_consume(0.04f, 0) * 0.04f; }
    for (int i = 0; i < 240; ++i) { b.on_write(25.0f, 0, 250); if (i % 10 == 9) out240 += b.on_consume(0.04f, 0) * 0.04f; }
    NEAR(out60, out240, 1e-3);
    // stale accumulation dropped; clamp at 180 deg
    YawAcc c; c.last_consume_ms = 0; c.on_write(1000.0f, 100, 250); CHECK(c.acc > 0);
    c.on_write(1000.0f, 400, 250); NEAR(c.acc, 1000.0f * c.seconds_per_frame, 1e-3);   // dropped first, then added
    for (int i = 0; i < 2000; ++i) c.on_write(100000.0f, 400, 250);
    CHECK(c.acc <= 180.0f);
    c.on_pad(); CHECK(!c.mouse_mode && c.acc == 0.0f);
    CHECK(c.on_consume(0.0f, 0) == 0.0f);
    // slow motion keeps the accumulation longer
    CHECK(yaw_drop_ms(1.0f) == 250);
    CHECK(yaw_drop_ms(0.3f) > 250 && yaw_drop_ms(0.3f) < 1000);
    CHECK(yaw_drop_ms(0.0f) == 5000);
}

static void test_mouse_y() {
    NEAR(mouse_y_factor(false, 1, false, 0.016f), 1.0, 1e-6);
    NEAR(mouse_y_factor(true, 1, false, 0.016f), -1.0, 1e-6);
    NEAR(mouse_y_factor(true, 0.5f, false, 0.016f), -0.5, 1e-6);
    NEAR(mouse_y_factor(false, 1, true, 1.0f / 60.0f), 1.0, 1e-5);
    NEAR(mouse_y_factor(false, 1, true, 1.0f / 240.0f), 4.0, 1e-4);
    NEAR(mouse_y_factor(false, 1, true, 1e-6f), 10.0, 1e-4);   // clamped
}

static void test_carry() {
    // integer-ms deltas: identical to the game's truncation
    MsCarry c;
    for (int v = -100; v <= 100; ++v) CHECK(c.credit((double)v) == v);
    CHECK(c.frac == 0.0);
    // slow motion 0.3 at 360 fps: frame ~2.78 ms real -> 0.83 game ms: vanilla credits 0 forever, carry keeps time
    MsCarry s; long long sum = 0; long long van = 0; double exact = 0;
    for (int i = 0; i < 3600; ++i) { double v = 0.3 * 2.777777; exact += v; sum += s.credit(v); van += (int)v; }
    CHECK(van == 0);
    CHECK(llabs(sum - (long long)exact) <= 1);
    // rewind (negative) keeps the same total
    MsCarry r; long long rs = 0; double rx = 0;
    for (int i = 0; i < 1000; ++i) { double v = -16.6; rx += v; rs += r.credit(v); }
    CHECK(llabs(rs - (long long)floor(rx)) <= 1);
    // mixed
    MsCarry m; long long ms = 0; double mx = 0;
    for (int i = 0; i < 1000; ++i) { double v = (i % 3 == 0) ? -2.25 : 3.4; mx += v; ms += m.credit(v); }
    CHECK(llabs(ms - (long long)floor(mx)) <= 1);
    CHECK(m.frac >= 0.0 && m.frac < 1.0);
}

static void test_limiter() {
    Limiter l; double period = 1.0 / 60.0;
    CHECK(l.wait_for(10.0, period) == 0.0);                 // first frame
    NEAR(l.wait_for(10.005, period), period - 0.005, 1e-9); // early -> wait
    NEAR(l.next, 10.0 + 2 * period, 1e-9);
    CHECK(l.wait_for(10.0 + 2 * period + 0.001, period) == 0.0);   // slightly late: no wait, cadence kept
    NEAR(l.next, 10.0 + 3 * period, 1e-9);
    CHECK(l.wait_for(20.0, period) == 0.0);                 // very late: resync, no burst
    NEAR(l.next, 20.0 + period, 1e-9);
    CHECK(l.wait_for(1.0, 0.0) == 0.0);                     // off
}

static void test_misc() {
    float v[3] = {1, 0, 1}; diag_clamp(v); NEAR(v[0], 0.70710678, 1e-6); NEAR(v[2], 0.70710678, 1e-6);
    float w[3] = {0, 0, 1}; diag_clamp(w); CHECK(w[2] == 1.0f && w[0] == 0.0f);
    float u[3] = {0.5f, 0, 0.5f}; diag_clamp(u); CHECK(u[0] == 0.5f);
    NEAR(fov_remap(64, 80), 80, 1e-3);
    NEAR(fov_remap(64, 0), 64, 1e-6);
    CHECK(fov_remap(104, 80) > 104 && fov_remap(104, 80) < 130);
    NEAR(fov_remap(69, 64.0f), 69, 1e-3);
    NEAR(steer_return(0.785f, 0.25f), 0.535, 1e-6); NEAR(steer_return(-0.785f, 0.25f), -0.535, 1e-6);
    CHECK(steer_return(0.2f, 0.25f) == 0.0f && steer_return(-0.2f, 0.25f) == 0.0f && steer_return(0.0f, 0.25f) == 0.0f);
    { float w = 0.785f; int n = 0; while (w != 0.0f && n < 10) { w = steer_return(w, 0.25f); ++n; } CHECK(n == 4); }
    CHECK(!restart_allowed(0) && !restart_allowed(1) && !restart_allowed(2) && !restart_allowed(70) && !restart_allowed(80));
    CHECK(restart_allowed(3) && restart_allowed(28) && restart_allowed(30) && restart_allowed(69) && !restart_allowed(-1));
}

static void test_md5() {
    uint8_t d[16]; char h[33];
    md5("", 0, d); md5_hex(d, h); CHECK(!strcmp(h, "d41d8cd98f00b204e9800998ecf8427e"));
    md5("abc", 3, d); md5_hex(d, h); CHECK(!strcmp(h, "900150983cd24fb0d6963f7d28e17f72"));
    const char* q = "The quick brown fox jumps over the lazy dog";
    md5(q, strlen(q), d); md5_hex(d, h); CHECK(!strcmp(h, "9e107d9d372bb6826bd81d3542a419d6"));
    Md5 m; m.init(); for (size_t i = 0; i < strlen(q); ++i) m.update(q + i, 1); m.final(d); md5_hex(d, h);
    CHECK(!strcmp(h, "9e107d9d372bb6826bd81d3542a419d6"));
}

int main() {
    test_ini(); test_keys(); test_remap(); test_meter(); test_bt(); test_yaw(); test_mouse_y(); test_carry();
    test_limiter(); test_misc(); test_md5();
    printf("test_logic: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
