// test_heat.cpp - unit tests of [mayhem_heat] (dll/src/gp_heat_logic.cpp). Built and run by build.bat. No game,
// no game memory. Exit code 0 = all passed.
#include "../../dll/src/gp_heat_logic.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace ht;

static void test_levels() {
    Cfg c;
    CHECK(level_of(c, 0) == 0 && level_of(c, 29.9f) == 0 && level_of(c, 30) == 1 && level_of(c, 89) == 1);
    CHECK(level_of(c, 90) == 2 && level_of(c, 170) == 3 && level_of(c, 270) == 4 && level_of(c, 390) == 5 && level_of(c, 9999) == 5);
    NEAR(level_start(c, 0), 0, 0); NEAR(level_start(c, 3), 170, 0);
    c.max_level = 3;
    CHECK(level_of(c, 9999) == 3 && level_of(c, 200) == 3);
    State s;
    for (int i = 0; i < 100; ++i) s.event(c, EV_KILL_COP, 1, true);
    NEAR(s.heat, 270, 1e-3);                                     // capped at the start of the unused level 4
    CHECK(s.tick(c, 0.01f, 0) == 3);
    c.max_level = 9; State t; for (int i = 0; i < 100; ++i) t.event(c, EV_KILL_COP, 1, true); NEAR(t.heat, 460, 1e-3);   // clamped to 5
}

static void test_events_and_cooling() {
    Cfg c; State s;
    s.event(c, EV_KILL_ENEMY, 1, true); s.event(c, EV_KILL_ENEMY, 1, true); s.event(c, EV_KILL_ENEMY, 1, true);
    NEAR(s.heat, 30, 1e-4); CHECK(s.tick(c, 0.1f, 3) == 1 && s.ups == 1 && s.last_up_level == 1);
    s.event(c, EV_STYLE, 250, true); NEAR(s.heat, 40, 1e-3);    // 250 action points x 0.04
    s.event(c, EV_DESTROY, 1, false); NEAR(s.heat, 43, 1e-3);   // own_weight 1: unknown culprit counts fully by default
    c.own_weight = 0.4f; s.event(c, EV_BARREL, 1, false); NEAR(s.heat, 46.6, 1e-3);
    s.event(c, EV_KILL_CIVIL, -1, true); NEAR(s.heat, 46.6, 1e-3);   // nonsense amounts ignored
    s.event(c, 99, 1, true); NEAR(s.heat, 46.6, 1e-3);
    // not calm yet: no cooling within calm_seconds
    s.since_event = 0; s.tick(c, 6.9f, 0); NEAR(s.heat, 46.6, 1e-3); CHECK(!s.cooling);
    // an event Ram did not cause does not reset the calm clock
    s.event(c, EV_KILL_NPC, 1, false); CHECK(s.since_event > 6.8f);
    // calm: one level per decay_seconds; heat 47.4 = level 1 at x = 1 + (47.4 - 30) / 60
    s.tick(c, 0.2f, 0); CHECK(s.cooling);
    float x0 = 1.0f + (s.heat - 30.0f) / 60.0f;
    s.tick(c, 1.0f, 0);
    float x1 = 1.0f + (s.heat - 30.0f) / 60.0f;
    NEAR(x0 - x1, 0.1, 1e-3);                                    // 1 / decay_seconds per second
    // enemies close: slower (near_factor 0.3)
    float h = s.heat; s.tick(c, 1.0f, 2); NEAR((h - s.heat) / 60.0f, 0.03, 1e-3);
    // a full cool-down from level 5: about 5 x decay_seconds + the rest of the band, step by step
    State b; for (int i = 0; i < 40; ++i) b.event(c, EV_KILL_COP, 1, true);
    b.tick(c, 0.0f, 0); CHECK(b.level == 5 && b.peak == 5);
    b.since_event = c.calm_seconds;
    int seen[6] = {0, 0, 0, 0, 0, 0}; float tsec = 0;
    while (b.heat > 0.0f && tsec < 200.0f) { b.tick(c, 0.05f, 0); seen[b.level] = 1; tsec += 0.05f; }
    CHECK(seen[4] && seen[3] && seen[2] && seen[1] && seen[0]);   // every step on the way down
    CHECK(tsec > 50.0f && tsec < 61.0f);                          // 5 levels + level 0's band, 10 s each, from the cap
    CHECK(b.peak == 0 && b.level == 0);
}

static void test_look() {
    Rgb w = tint_for(0, 1); NEAR(w.r, 1, 0); NEAR(w.g, 1, 0); NEAR(w.b, 1, 0);
    Rgb o = tint_for(2, 1); NEAR(o.g, 0.74, 1e-6); CHECK(o.b < o.g && o.g < o.r);   // orange
    Rgb r = tint_for(5, 1); CHECK(r.g < 0.2f && r.b < 0.15f && r.r == 1.0f);        // red
    Rgb half = tint_for(5, 0.5f); NEAR(half.g, 0.59, 1e-6);
    Rgb mid = tint_for(2.5f, 1); NEAR(mid.g, 0.67, 1e-6);
    // monotone toward red
    float last = 2; for (float l = 0; l <= 5.0f; l += 0.25f) { Rgb k = tint_for(l, 1); CHECK(k.g <= last + 1e-6f); last = k.g; }
    // follow: about half a second per level step, never overshoots
    float sh = 0; sh = follow(sh, 2, 0.25f); NEAR(sh, 0.5, 1e-6); sh = follow(sh, 2, 10.0f); NEAR(sh, 2, 0);
    sh = follow(2, 0, 0.1f); NEAR(sh, 1.8, 1e-6);
    // the edge pulse only at the top level, alternating red and dark
    Ring n = edge_ring(4.0f, 5, 0.0); CHECK(n.a == 0);
    Ring a = edge_ring(5.0f, 5, 0.4), d = edge_ring(5.0f, 5, 1.2);   // sin peak at 0.4 s, trough at 1.2 s (period 1.6)
    CHECK(a.a > 0.8f && a.r > 0.8f && d.a > 0.7f && d.r < 0.1f);
    Ring top3 = edge_ring(3.0f, 3, 0.4); CHECK(top3.a > 0.8f);         // max_level 3: the pulse is at level 3
    // icons: icon i fills while heat climbs to level i + 1
    Cfg c;
    NEAR(icon_fill(c, 0, 0), 0, 0); NEAR(icon_fill(c, 15, 0), 0.5, 1e-6); NEAR(icon_fill(c, 30, 0), 1, 0);
    NEAR(icon_fill(c, 30, 1), 0, 0); NEAR(icon_fill(c, 60, 1), 0.5, 1e-6); NEAR(icon_fill(c, 400, 4), 1, 0);
    NEAR(icon_fill(c, 400, 5), 0, 0);
    c.max_level = 3; NEAR(icon_fill(c, 400, 3), 0, 0); NEAR(icon_fill(c, 400, 2), 1, 0);
    State s; s.level = 3; s.cooling = true;
    CHECK(icon_glow(s, 2, 0.0) == 1.0f && icon_glow(s, 1, 0.0) == 1.0f && icon_glow(s, 2, 0.55) < 0.6f);
    s.cooling = false; CHECK(icon_glow(s, 2, 0.55) == 1.0f);
}

static void test_geometry() {
    // the Radar_BG quad as the sprite builds it: TL, BL, BR, TR with UVs (0,0) (0,1) (1,1) (1,0)
    float P[4][2] = {{100, 700}, {100, 988}, {388, 988}, {388, 700}}, UV[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    Aff m; CHECK(uv_affine(P, UV, &m));
    float x, y; aff_apply(m, 0.5f, 0.5f, &x, &y); NEAR(x, 244, 1e-3); NEAR(y, 844, 1e-3);
    aff_apply(m, 1, 1, &x, &y); NEAR(x, 388, 1e-3); NEAR(y, 988, 1e-3);
    // a rotated / mirrored quad still maps its corners
    float P2[4][2] = {{0, 0}, {10, 0}, {10, 20}, {0, 20}};
    CHECK(uv_affine(P2, UV, &m)); aff_apply(m, 1, 0, &x, &y); NEAR(x, 0, 1e-4); NEAR(y, 20, 1e-4);
    float Pd[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}}; CHECK(!uv_affine(Pd, UV, &m));
    // icons: outside the disc, on its right side, apart from each other, clear of the top tab
    float pu = 0, pv = 0;
    for (int i = 0; i < 5; ++i) {
        float u, v, s; icon_place(i, 5, 1.0f, &u, &v, &s);
        float d = sqrtf((u - DISC_U) * (u - DISC_U) + (v - DISC_V) * (v - DISC_V));
        CHECK(d - s * 0.5f > DISC_R - 0.02f);                    // the icon does not cover the disc
        CHECK(u > DISC_U + 0.15f);                               // right of the tab
        if (i) CHECK(sqrtf((u - pu) * (u - pu) + (v - pv) * (v - pv)) >= s * 1.0f);   // no overlap
        pu = u; pv = v;
        CHECK(s > 0.15f && s < 0.2f);
    }
}

static void test_escalation() {
    Esc e0 = escalation(0, 1); CHECK(e0.wave == 0 && e0.share_add == 0 && e0.max_rearms == 1 && !e0.prefer_high);
    Esc e2 = escalation(2, 1); CHECK(e2.wave == 2 && e2.interval_s == 45 && !e2.prefer_high);
    Esc e5 = escalation(5, 1); CHECK(e5.wave == 5 && e5.max_rearms == 3 && e5.prefer_high); NEAR(e5.share_add, 1.25, 1e-6);
    // monotone escalation
    for (int l = 1; l <= 5; ++l) { Esc a = escalation(l - 1, 1), b = escalation(l, 1); CHECK(b.wave >= a.wave && b.share_add > a.share_add && b.max_rearms >= a.max_rearms); }
    Esc s = escalation(5, 0.4f); CHECK(s.wave == 2); NEAR(s.share_add, 0.5, 1e-6); NEAR(s.interval_s, 44, 1e-4);   // easy
    Esc z = escalation(5, 0); CHECK(z.wave == 0 && z.share_add == 0);
    Esc tiny = escalation(2, 0.2f); CHECK(tiny.wave == 1);         // a wave never rounds away to nothing
    Esc out = escalation(17, 1); CHECK(out.wave == 5);
    NEAR(difficulty_scale(3), 1, 0); CHECK(difficulty_scale(0) < difficulty_scale(1) && difficulty_scale(1) < difficulty_scale(2) && difficulty_scale(4) > 1);
    NEAR(difficulty_scale(-1), 0.7, 1e-6);
}

static void test_score() {
    // the multiplier table: none and 1 = x1, then 1.25 / 1.5 / 1.75 / 2 at 2..5
    NEAR(score_multiplier(0, 5, 2.0f), 1.0, 0); NEAR(score_multiplier(1, 5, 2.0f), 1.0, 0);
    NEAR(score_multiplier(2, 5, 2.0f), 1.25, 1e-6); NEAR(score_multiplier(3, 5, 2.0f), 1.5, 1e-6);
    NEAR(score_multiplier(4, 5, 2.0f), 1.75, 1e-6); NEAR(score_multiplier(5, 5, 2.0f), 2.0, 1e-6);
    NEAR(score_multiplier(9, 5, 2.0f), 2.0, 1e-6); NEAR(score_multiplier(-3, 5, 2.0f), 1.0, 0);
    NEAR(score_multiplier(5, 5, 1.0f), 1.0, 0); NEAR(score_multiplier(5, 5, 0.5f), 1.0, 0);    // never below x1
    NEAR(score_multiplier(3, 3, 2.0f), 2.0, 1e-6); NEAR(score_multiplier(2, 3, 2.0f), 1.5, 1e-6);   // fewer levels: same span
    NEAR(score_multiplier(1, 1, 2.0f), 1.0, 0);
    NEAR(score_multiplier(5, 5, 3.0f), 3.0, 1e-6); NEAR(score_multiplier(5, 5, 99.0f), 10.0, 1e-6);
    // the game's combo award (CurrentActionPoints x ComboMultiplier, e.g. the HUD's 2x19 = 38) scaled
    CHECK(scale_score(38, 1.0f) == 38); CHECK(scale_score(38, 2.0f) == 76); CHECK(scale_score(38, 1.25f) == 48);   // 47.5 -> 48
    CHECK(scale_score(38, 1.75f) == 67); CHECK(scale_score(0, 2.0f) == 0); CHECK(scale_score(-50, 2.0f) == -50);
    CHECK(scale_score(1500000000, 2.0f) == 2000000000);
    char b[16];
    CHECK(score_label(1.0f, b, 16) == 0 && b[0] == 0);
    CHECK(score_label(1.25f, b, 16) == 5 && !strcmp(b, "x1.25"));
    CHECK(score_label(1.5f, b, 16) == 4 && !strcmp(b, "x1.5"));
    CHECK(score_label(1.75f, b, 16) == 5 && !strcmp(b, "x1.75"));
    CHECK(score_label(2.0f, b, 16) == 2 && !strcmp(b, "x2"));
    CHECK(score_label(2.0f, b, 2) == 0);
}

int main() {
    test_levels(); test_events_and_cooling(); test_look(); test_geometry(); test_escalation(); test_score();
    printf("heat unit tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
