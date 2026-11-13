// test_drops.cpp - unit tests of drops_logic.cpp (no game). Built and run by dll/build.bat.
#include "drops_logic.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace dr;

static Cfg on() { Cfg c; c.enabled = true; return c; }

static void test_tables() {
    CHECK(slot_of_goodie(G_PINATA) == 10 && slot_of_goodie(G_REWIND) == 1 && slot_of_goodie(G_TORNADO) == 3 && slot_of_goodie(G_TORO) == 4);
    CHECK(slot_of_goodie(G_MEDIUM_HEALTH) == -1 && slot_of_goodie(G_LARGE_HEALTH) == -1);
    CHECK(!strcmp(goodie_name(G_PINATA), "pinata") && !strcmp(goodie_name(G_LARGE_HEALTH), "large health") && !strcmp(goodie_name(99), "?"));
    for (int i = 0; i < N_POWERUPS; ++i) CHECK(POWERUP_SLOT[i] != 11);   // slot 11 is Suave's (Tequila): never one of ours
}

static void test_ammo() {
    Cfg c;                                       // off: the difficulty's own factor
    NEAR(ammo_factor(c, 0.5f), 0.5f, 1e-6);
    c = on();                                    // on: the scale replaces it, every difficulty above vanilla
    NEAR(ammo_factor(c, 0.5f), 1.75f, 1e-6); NEAR(ammo_factor(c, 1.0f), 1.75f, 1e-6); NEAR(ammo_factor(c, 0.75f), 1.75f, 1e-6);
    c.keep_difficulty_ammo = true;               // keep: x the difficulty factor too
    NEAR(ammo_factor(c, 0.5f), 0.875f, 1e-6);
    c.keep_difficulty_ammo = false; c.ammo_scale = 1.0f;   // the "before" setting of the proof run: vanilla on NORMAL
    NEAR(ammo_factor(c, 1.0f), 1.0f, 1e-6);
    c.ammo_scale = 99.0f; c.clamp(); CHECK(c.ammo_scale == 5.0f);
    c.ammo_scale = -1.0f; c.clamp(); CHECK(c.ammo_scale == 0.0f);
}

static void test_health_chance() {
    Cfg c = on();
    NEAR(health_chance(c, 1.0f), 0.20f, 1e-6);   // full health
    NEAR(health_chance(c, 0.0f), 0.50f, 1e-6);   // nearly dead: + the whole bonus
    NEAR(health_chance(c, 0.5f), 0.35f, 1e-6);
    NEAR(health_chance(c, 7.0f), 0.20f, 1e-6);   // out of range reads clamp
    c.health_chance = 0.9f; NEAR(health_chance(c, 0.0f), 1.0f, 1e-6);
}

static void test_powerup_pick() {
    Cfg c = on(); Rng r;
    int counts[12], maxima[12];
    for (int i = 0; i < 12; ++i) { counts[i] = 0; maxima[i] = 3; }
    RollIn in; in.counts = counts; in.maxima = maxima;
    int seen[17] = {0};
    for (int k = 0; k < 20000; ++k) { int g = pick_powerup(c, r, in); CHECK(g >= 0 && g <= 16); if (g >= 0) ++seen[g]; }
    int total = 0; for (int w : c.weight) total += w;
    for (int i = 0; i < N_POWERUPS; ++i) NEAR((double)seen[POWERUP_GOODIE[i]] / 20000.0, (double)c.weight[i] / total, 0.02);
    // full / locked / unknown slots never come
    counts[10] = 3;                              // the Pinata slot full
    maxima[3] = 0;                               // tornado locked
    counts[4] = -1;                              // toro unknown
    memset(seen, 0, sizeof seen);
    for (int k = 0; k < 5000; ++k) { int g = pick_powerup(c, r, in); if (g >= 0) ++seen[g]; }
    CHECK(seen[G_PINATA] == 0 && seen[G_TORNADO] == 0 && seen[G_TORO] == 0 && seen[G_REWIND] > 0);
    // Suave on slot 10: the Pinata never comes
    counts[10] = 0; in.excluded_slot = 10; memset(seen, 0, sizeof seen);
    for (int k = 0; k < 5000; ++k) { int g = pick_powerup(c, r, in); if (g >= 0) ++seen[g]; }
    CHECK(seen[G_PINATA] == 0);
    // everything full: none
    for (int i = 0; i < 12; ++i) counts[i] = maxima[i];
    CHECK(pick_powerup(c, r, in) == -1);
    // all weights 0: none
    Cfg z = on(); for (int& w : z.weight) w = 0; for (int i = 0; i < 12; ++i) counts[i] = 0;
    CHECK(pick_powerup(z, r, in) == -1);
}

static void test_roll() {
    Cfg c = on(); Rng r;
    int counts[12], maxima[12]; for (int i = 0; i < 12; ++i) { counts[i] = 0; maxima[i] = 3; }
    RollIn in; in.counts = counts; in.maxima = maxima; in.ram_health_frac = 1.0f;
    int health = 0, large = 0, pu = 0, none = 0; const int N = 40000;
    for (int k = 0; k < N; ++k) {
        Roll ro = roll(c, r, in);
        if (ro.goodie < 0) ++none; else if (ro.goodie == G_MEDIUM_HEALTH) ++health; else if (ro.goodie == G_LARGE_HEALTH) { ++health; ++large; } else ++pu;
    }
    NEAR((double)pu / N, 0.05, 0.006);
    NEAR((double)health / N, 0.95 * 0.20, 0.008);   // the health roll comes when no power-up came
    NEAR((double)large / health, 0.25, 0.03);
    // hurt Ram: more health
    in.ram_health_frac = 0.2f; health = 0;
    for (int k = 0; k < N; ++k) { Roll ro = roll(c, r, in); if (ro.goodie == G_MEDIUM_HEALTH || ro.goodie == G_LARGE_HEALTH) ++health; }
    NEAR((double)health / N, 0.95 * (0.20 + 0.30 * 0.8), 0.01);
    // a PSYCHO elite that leaves its move: no power-up, health as usual
    in.ram_health_frac = 1.0f; in.elite_drop = true; pu = 0;
    for (int k = 0; k < N; ++k) { Roll ro = roll(c, r, in); if (ro.goodie >= 8) ++pu; CHECK(ro.p_powerup == 0.0f); }
    CHECK(pu == 0);
    c.elite_skip_powerup = false; pu = 0;
    for (int k = 0; k < N; ++k) { Roll ro = roll(c, r, in); if (ro.goodie >= 8) ++pu; }
    CHECK(pu > 0);
    // the "before" setting (chances 0): nothing ever
    Cfg b = on(); b.health_chance = 0; b.health_hurt_bonus = 0; b.powerup_chance = 0; in.elite_drop = false; in.ram_health_frac = 0.0f;
    for (int k = 0; k < 2000; ++k) CHECK(roll(b, r, in).goodie == -1);
    // every power-up slot full: the power-up roll falls through to the health roll
    for (int i = 0; i < 12; ++i) counts[i] = 3;
    Cfg p = on(); p.powerup_chance = 1.0f; p.health_chance = 1.0f; in.ram_health_frac = 1.0f;
    Roll ro = roll(p, r, in); CHECK(ro.goodie == G_MEDIUM_HEALTH || ro.goodie == G_LARGE_HEALTH);
}

static void test_rest_y() {
    float g[5]; bool h[5];
    // flat floor: the floor + lift
    for (int i = 0; i < 5; ++i) { g[i] = -139.0f; h[i] = true; }
    NEAR(rest_y(g, h, 5, 0.0f, 0.35f), -139.0f, 1e-5); NEAR(rest_y(g, h, 5, 0.1f, 0.35f), -138.9f, 1e-5);
    // a slope: the uphill probe, so the pickup never sinks into it
    g[0] = -139.0f; g[1] = -138.9f; g[2] = -139.1f; g[3] = -139.05f; g[4] = -138.95f;
    NEAR(rest_y(g, h, 5, 0.0f, 0.35f), -138.9f, 1e-5);
    // a stair edge: the centre on the lower step, one probe on the next (0.25 m higher): on the step
    for (int i = 0; i < 5; ++i) g[i] = 0.0f; g[3] = 0.25f;
    NEAR(rest_y(g, h, 5, 0.0f, 0.35f), 0.25f, 1e-5);
    // a wall / table next to it (1 m higher): left out
    g[3] = 1.0f; NEAR(rest_y(g, h, 5, 0.0f, 0.35f), 0.0f, 1e-5);
    // no centre hit: the lowest of the others
    h[0] = false; g[1] = 0.1f; g[2] = 0.05f; g[3] = 0.2f; g[4] = 0.6f;
    NEAR(rest_y(g, h, 5, 0.0f, 0.35f), 0.2f, 1e-5);
    // nothing hit
    for (int i = 0; i < 5; ++i) h[i] = false;
    float n = rest_y(g, h, 5, 0.0f, 0.35f); CHECK(!(n == n));
    // one probe only (footprint 0)
    h[0] = true; g[0] = 3.0f; NEAR(rest_y(g, h, 1, 0.05f, 0.35f), 3.05f, 1e-5);
}

static void test_floor_offset() {
    Cfg c = on();
    c.calibrate_max = 1.5f;
    NEAR(floor_offset(c, -139.0f, -140.0f, true), 1.0f, 1e-5);        // the line test 1 m low
    NEAR(floor_offset(c, -139.0f, -139.01f, true), 0.0f, 1e-5);       // reads right (within 3 cm)
    NEAR(floor_offset(c, -139.0f, -138.5f, true), 0.0f, 1e-5);        // reads high: a step under the feet, never lowered
    NEAR(floor_offset(c, -139.0f, -141.0f, true), 0.0f, 1e-5);        // 2 m off: something else
    NEAR(floor_offset(c, -139.0f, -140.0f, false), 0.0f, 1e-5);       // no hit
    c.calibrate_max = 0.0f; NEAR(floor_offset(c, -139.0f, -140.0f, true), 0.0f, 1e-5);   // off
}

static void test_spot() {
    Cfg c = on(); bool ob = false;
    Vec3 stood = v3(0, 0, 0), body = v3(1.5f, -0.2f, 0);
    Vec3 p = drop_spot(c, stood, body, true, true, &ob); CHECK(ob && p.x == 1.5f);
    p = drop_spot(c, stood, v3(5, 0, 0), true, true, &ob); CHECK(!ob && p.x == 0.0f);      // too far: where it stood
    p = drop_spot(c, stood, v3(1, -3, 0), true, true, &ob); CHECK(!ob);                     // fell off a ledge
    p = drop_spot(c, stood, body, true, false, &ob); CHECK(!ob);                            // a wall between
    p = drop_spot(c, stood, body, false, true, &ob); CHECK(!ob);                            // no floor under the body
    Vec3 b = beside(v3(0, 1, 0), v3(10, 5, 0), 0.7f);   // toward Ram, height kept
    NEAR(b.x, 0.7f, 1e-5); NEAR(b.z, 0.0f, 1e-5); NEAR(b.y, 1.0f, 1e-5);
    b = beside(v3(0, 0, 0), v3(1, 0, 0), 0.7f); NEAR(b.x, 0.5f, 1e-5);                    // never past half way to Ram
    b = beside(v3(2, 0, 2), v3(2, 0, 2), 0.7f); NEAR(b.x, 2.0f, 1e-5); NEAR(b.z, 2.0f, 1e-5);   // Ram on it: the body
    b = beside(v3(0, 0, 0), v3(0, 0, -10), 0.0f); NEAR(b.z, 0.0f, 1e-5);
}

static void test_book() {
    Book k; Cfg c = on();
    CHECK(k.count() == 0 && !k.find(5));
    Body* a = k.add(5, 1.0); CHECK(a && a->used && a->key == 5 && a->goodie == -2);
    CHECK(k.find(5) == a && k.count() == 1);
    for (uint32_t i = 0; i < Book::N - 1; ++i) CHECK(k.add(100 + i, 2.0 + i) != nullptr);
    CHECK(k.count() == Book::N);
    // full: the one seen longest ago that is not dead goes
    Body* x = k.add(999, 500.0); CHECK(x && x->key == 999 && !k.find(5));
    // a full book of dead bodies waiting: nothing is evicted
    Book d; for (uint32_t i = 0; i < Book::N; ++i) { Body* y = d.add(i + 1, 1.0); y->dead = true; y->t_dead = 1.0; }
    CHECK(d.add(777, 2.0) == nullptr);
    d.b[3].done = true; d.b[3].t_dead = 0.5; d.b[7].done = true; d.b[7].t_dead = 0.7;
    Body* z = d.add(778, 3.0); CHECK(z == &d.b[3]);     // the oldest finished one
    k.clear(); CHECK(k.count() == 0);
    // settling
    Body s; s.t_dead = 10.0; s.rest_ok = false;
    CHECK(!body_settled(c, s, 10.5) && body_settled(c, s, 11.5));
    s.rest_ok = true; s.t_rest = 10.1;
    CHECK(!body_settled(c, s, 10.2) && body_settled(c, s, 10.4));
    // life
    CHECK(life_of(false, 100, 0, 20) == LIFE_GONE && life_of(true, 100, 100, 20) == LIFE_DEAD && life_of(true, 100, 0, 28) == LIFE_DEAD && life_of(true, 100, 10, 20) == LIFE_ALIVE);
    CHECK(clock_jump_is_load(11.0) && !clock_jump_is_load(4.0));
}

static void test_clamp() {
    Cfg c; c.max_on_ground = 9; c.drop_seconds = 1; c.lift = 9; c.footprint = -1; c.max_step = 0; c.weight[2] = -5; c.health_chance = 2;
    c.clamp();
    CHECK(c.max_on_ground == 5 && c.drop_seconds == 5.0f && c.lift == 1.5f && c.footprint == 0.0f && c.max_step == 0.05f && c.weight[2] == 0 && c.health_chance == 1.0f);
    Cfg d; d.max_on_ground = 0; d.clamp(); CHECK(d.max_on_ground == 1);
    Cfg e; float nan = sqrtf(-1.0f); e.powerup_chance = nan; e.clamp(); CHECK(e.powerup_chance == 0.0f);
}

int main() {
    test_tables();
    test_ammo();
    test_health_chance();
    test_powerup_pick();
    test_roll();
    test_rest_y();
    test_floor_offset();
    test_spot();
    test_book();
    test_clamp();
    printf("test_drops: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
