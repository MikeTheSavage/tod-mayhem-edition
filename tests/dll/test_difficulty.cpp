// test_difficulty.cpp - unit tests of [difficulty] (dll/src/gp_difficulty_logic.cpp). Built and run by
// build.bat. No game, no game memory. Exit code 0 = all passed.
#include "../../dll/src/gp_difficulty_logic.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace df;

static void test_levels() {
    CHECK(level_from_setting(0, 0) == EASY && level_from_setting(0, 1) == EASY);
    CHECK(level_from_setting(1, 0) == NORMAL && level_from_setting(1, 2) == NORMAL);
    CHECK(level_from_setting(2, 0) == HARD);
    CHECK(level_from_setting(2, 1) == BRUTAL);
    CHECK(level_from_setting(2, 2) == PSYCHO);
    CHECK(level_from_setting(2, 9) == LEVEL_MAX && level_from_setting(2, -3) == HARD);   // out of range: clamped
    CHECK(level_from_setting(3, 1) == NORMAL && level_from_setting(-5, 0) == EASY);       // as the game reads them
    CHECK(level_from_name("Brutal") == BRUTAL && level_from_name("hard") == HARD && level_from_name("x") == -1);
    CHECK(level_from_name("PSYCHO") == PSYCHO && level_from_name(nullptr) == -1);
    CHECK(!strcmp(level_name(BRUTAL), "brutal") && !strcmp(level_name(PSYCHO), "psycho") && !strcmp(level_name(17), "?"));
}

static void test_factors() {
    // EASY / NORMAL = vanilla: every factor 1
    for (int t = 0; t < T_COUNT; ++t) { NEAR(tuned(t, 3.5f, EASY), 3.5, 1e-6); NEAR(tuned(t, 3.5f, NORMAL), 3.5, 1e-6); }
    // the data option's factors (options.py DIFFICULTY)
    NEAR(tuned(T_DAMAGE_TAKEN, 1.4f, HARD), 1.96, 1e-5);   NEAR(tuned(T_DAMAGE_TAKEN, 1.4f, BRUTAL), 2.8, 1e-5);
    NEAR(tuned(T_DAMAGE_DEALT, 1.0f, HARD), 0.8, 1e-6);    NEAR(tuned(T_DAMAGE_DEALT, 1.0f, BRUTAL), 0.6, 1e-6);
    NEAR(tuned(T_SPREAD, 1.4f, HARD), 0.84, 1e-5);         NEAR(tuned(T_SPREAD, 1.4f, BRUTAL), 0.35, 1e-5);
    NEAR(tuned(T_PAUSE, 2.0f, HARD), 1.5, 1e-6);           NEAR(tuned(T_PAUSE, 2.0f, BRUTAL), 1.0, 1e-6);
    NEAR(tuned(T_REACT, 3.0f, HARD), 2.1, 1e-5);           NEAR(tuned(T_REACT, 3.0f, BRUTAL), 1.5, 1e-6);
    NEAR(tuned(T_AMMO, 250.0f, HARD), 187.5, 1e-4);        NEAR(tuned(T_AMMO, 250.0f, BRUTAL), 125.0, 1e-4);
    NEAR(tuned(99, 2.0f, BRUTAL), 2.0, 0);
    for (int t = 0; t < T_COUNT; ++t) NEAR(tuned(t, 1.7f, PSYCHO), tuned(t, 1.7f, BRUTAL), 0);   // PSYCHO = BRUTAL's tuning for now
    // round trip: brutal -> normal -> hard from the same base never drifts (the glue keeps the base)
    float base = 0.7f;
    float b = tuned(T_PAUSE, base, BRUTAL), n = tuned(T_PAUSE, base, NORMAL), h = tuned(T_PAUSE, base, HARD);
    NEAR(b, 0.35, 1e-6); CHECK(n == base); NEAR(h, 0.525, 1e-6);
}

static void test_rows() {
    for (int i = 0; i <= 9; ++i) CHECK(weapon_row_tuned(i));
    CHECK(!weapon_row_tuned(10) && !weapon_row_tuned(14) && !weapon_row_tuned(-1) && !weapon_row_tuned(19));
    CHECK(!chartype_row_tuned(7));                          // the player's behaviour row
    CHECK(chartype_row_tuned(0) && chartype_row_tuned(22));
    CHECK(!modelstat_row_tuned(20) && !modelstat_row_tuned(10) && !modelstat_row_tuned(18));   // Ram, Ernesto, Tommy
    CHECK(modelstat_row_tuned(0) && modelstat_row_tuned(31));
}

static void test_static() {
    CHECK(static_level(1.4f, 1.0f) == NORMAL);
    CHECK(static_level(1.4f * 2.0f, 0.6f) == BRUTAL);      // a data zip built with difficulty = brutal
    CHECK(static_level(1.4f * 1.4f, 0.8f) == HARD);
    CHECK(static_level(1.4f * 0.6f, 1.5f) == EASY);
    CHECK(static_level(2.0f, 1.0f) == -1);                 // the exe defaults (no data) or another mod
}

static void test_menu() {
    CHECK(menu_shown_value(2, 1, 4) == 3);                 // Hard + BRUTAL + a fourth value -> BRUTAL shown
    CHECK(menu_shown_value(2, 1, 3) == 2);                 // no fourth value (data option missing): HARD
    CHECK(menu_shown_value(2, 0, 5) == 2);
    CHECK(menu_shown_value(2, 2, 5) == 4);                 // PSYCHO
    CHECK(menu_shown_value(2, 2, 4) == 3);                 // PSYCHO saved, only BRUTAL offered: the highest offered
    CHECK(menu_shown_value(1, 1, 4) == 1 && menu_shown_value(0, 2, 5) == 0);
    SetResult r = menu_chosen(3); CHECK(r.setting == 2 && r.extra == 1);
    r = menu_chosen(4); CHECK(r.setting == 2 && r.extra == 2);
    r = menu_chosen(2); CHECK(r.setting == 2 && r.extra == 0);
    r = menu_chosen(1); CHECK(r.setting == 1 && r.extra == 0);
    r = menu_chosen(0); CHECK(r.setting == 0 && r.extra == 0);
    r = menu_chosen(9); CHECK(r.setting == 2 && r.extra == EXTRA_MAX);   // never a value > 2 into the settings
    r = menu_chosen(-1); CHECK(r.setting == 0 && r.extra == 0);
    // round trip: what the menu stores and what it shows again (5 values)
    for (int i = 0; i < 5; ++i) { SetResult s = menu_chosen(i); CHECK(menu_shown_value(s.setting, s.extra, 5) == i); }
}

static void test_sidecar() {
    char t[256]; int e = -1;
    for (int x = 0; x <= EXTRA_MAX; ++x) { format_sidecar(x, t, sizeof t); e = -1; CHECK(parse_sidecar(t, &e) && e == x); }
    format_sidecar(7, t, sizeof t); CHECK(parse_sidecar(t, &e) && e == EXTRA_MAX);
    CHECK(parse_sidecar("brutal=1", &e) && e == 1);          // the first build's format
    CHECK(parse_sidecar("x\r\nbrutal  =  0\r\n", &e) && e == 0);
    CHECK(parse_sidecar("level = psycho", &e) && e == 2);
    CHECK(parse_sidecar("; c\r\nlevel = hard\r\n", &e) && e == 0);
    e = 1; CHECK(!parse_sidecar("level = sideways", &e) && e == 1);
    e = 1; CHECK(!parse_sidecar("nothing here", &e) && e == 1);
    CHECK(!parse_sidecar(nullptr, &e));
}

static void test_reinf() {
    ReinfCfg c; ReinfState s;
    // 6 enemy deaths at share 0.5 -> 3 reinforcements, one per two deaths
    int got = 0;
    for (int i = 0; i < 6; ++i) { s.on_death(c); while (s.take()) ++got; }
    CHECK(got == 3 && s.rearmed == 3 && s.deaths == 6);
    NEAR(s.acc, 0.0, 1e-6);
    // an odd count leaves half a reinforcement waiting
    s.reset(); for (int i = 0; i < 5; ++i) s.on_death(c);
    got = 0; while (s.take()) ++got; CHECK(got == 2); NEAR(s.acc, 0.5, 1e-6);
    // the backlog is capped (a long quiet stretch never turns into a wave)
    s.reset(); for (int i = 0; i < 40; ++i) s.on_death(c);
    got = 0; while (s.take()) ++got; CHECK(got == 8);
    // share 1.0 = double the enemies
    ReinfCfg d; d.share = 1.0f; s.reset(); s.on_death(d); CHECK(s.take() && !s.take());
    // candidates
    int skip[] = {6, 10, 13, 14};
    Seen e; e.faction = 9; e.health = 100; e.sub_type = 0;
    CHECK(reinforcement_candidate(e, c, skip, 4));
    Seen x = e; x.faction = 10; CHECK(!reinforcement_candidate(x, c, skip, 4));      // DEA
    x = e; x.health = 1000; CHECK(!reinforcement_candidate(x, c, skip, 4));          // 1000-hp special
    x = e; x.buddy = true; CHECK(!reinforcement_candidate(x, c, skip, 4));
    x = e; x.sub_type = 9; CHECK(!reinforcement_candidate(x, c, skip, 4));           // civilian
    x = e; x.faction = -1; CHECK(!reinforcement_candidate(x, c, skip, 4));           // never seen alive
    x = e; x.health = 0; CHECK(!reinforcement_candidate(x, c, skip, 4));
    CHECK(distance_ok(20, c) && !distance_ok(10, c) && !distance_ok(60, c));
}

// reinforcement rules for the "random mission fails" fix
static void test_bugfix1() {
    ReinfCfg c; int skip[] = {6, 10, 13, 14};
    Seen e; e.faction = 9; e.health = 140; e.sub_type = 0;
    // modified copies of skipped factions (the game's provoke rule: a DEA friend hit by Ram is faction 29) are skipped too
    Seen x = e; x.faction = 29; CHECK(!reinforcement_candidate(x, c, skip, 4));
    x = e; x.faction = 33; CHECK(!reinforcement_candidate(x, c, skip, 4));          // police copy
    x = e; x.faction = 28; CHECK(reinforcement_candidate(x, c, skip, 4));           // an enemy provoked to its copy: still an enemy
    x = e; x.faction = 18; CHECK(!reinforcement_candidate(x, c, skip, 4));          // Mexican buddies
    x = e; x.faction = 0; CHECK(!reinforcement_candidate(x, c, skip, 4));           // the player's faction
    x = e; x.sub_type = 4; CHECK(!reinforcement_candidate(x, c, skip, 4));          // escort
    x = e; x.sub_type = 8; CHECK(!reinforcement_candidate(x, c, skip, 4));          // never aggressive
    CHECK(faction_skipped(10, skip, 4) && faction_skipped(29, skip, 4) && !faction_skipped(9, skip, 4) && !faction_skipped(28, skip, 4));
    // the live cap: never the 9th shell, the game's own value in a mission with a buddy / escort / friend
    CHECK(live_cap(9, 6, 9, false, false) == 8);
    CHECK(live_cap(9, 8, 9, false, false) == 8);
    CHECK(live_cap(7, 6, 9, false, false) == 7);
    CHECK(live_cap(4, 6, 9, false, false) == 6);      // never below the game's
    CHECK(live_cap(0, 6, 9, false, false) == 6);      // off
    CHECK(live_cap(9, 6, 9, true, false) == 6);       // MM013: Marco on the farm
    CHECK(live_cap(9, 6, 9, true, true) == 8);        // max_live_with_buddies = 1
    // psycho-tune: PSYCHO's own reinforcement share (25 % fewer than BRUTAL's), BRUTAL unchanged, none below
    { ReinfCfg r; CHECK(fabsf(level_share(PSYCHO, r) - 0.375f) < 1e-6f && fabsf(level_share(BRUTAL, r) - 0.5f) < 1e-6f);
      CHECK(level_share(HARD, r) == 0.0f && level_share(NORMAL, r) == 0.0f && level_share(-1, r) == 0.0f);
      r.share = 1.0f; r.share_psycho = 0.2f; CHECK(level_share(BRUTAL, r) == 1.0f && level_share(PSYCHO, r) == 0.2f);
      ReinfCfg p; ReinfState s; int n = 0; for (int i = 0; i < 40; ++i) { s.on_death(ReinfCfg{0.375f}); n += s.take(); }
      CHECK(n == 15); s.reset(); n = 0; for (int i = 0; i < 40; ++i) { s.on_death(p); n += s.take(); } CHECK(n == 20); }   // per 40 deaths: 15 (was 20)
    // a re-armed placement's stored damage (>= Health) makes a born-dead reinforcement; a hurt one is not "born dead"
    CHECK(born_dead(140, 140) && born_dead(140, 500) && !born_dead(140, 0) && !born_dead(140, 60) && !born_dead(0, 50));
}

int main() {
    test_levels(); test_factors(); test_rows(); test_static(); test_menu(); test_sidecar(); test_reinf(); test_bugfix1();
    printf("difficulty unit tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
