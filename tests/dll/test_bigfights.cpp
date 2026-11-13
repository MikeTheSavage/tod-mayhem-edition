// test_bigfights.cpp - unit tests of dll/src/gp_bigfights_logic.cpp
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../dll/src/gp_bigfights_logic.h"

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
    using namespace bf;
    int t = 0, s = 0;
    CHECK(parse_clone_folder("BF1_012", &t, &s) && t == 1 && s == 12);
    CHECK(parse_clone_folder("BF2_001", &t, &s) && t == 2 && s == 1);
    CHECK(parse_clone_folder("BF3_120", &t, &s) && t == 3 && s == 120);
    CHECK(!parse_clone_folder("BF1_012e", &t, &s));
    CHECK(!parse_clone_folder("BF4_012", &t, &s));
    CHECK(!parse_clone_folder("BF1_12", &t, &s));
    CHECK(!parse_clone_folder("BFV0", &t, &s));
    CHECK(!parse_clone_folder("", &t, &s));
    CHECK(!parse_clone_folder(nullptr, &t, &s));

    CHECK(!tier_wakes(1, -1, -1)); CHECK(!tier_wakes(1, NORMAL, -1)); CHECK(!tier_wakes(1, HARD, -1));
    CHECK(tier_wakes(1, BRUTAL, -1)); CHECK(!tier_wakes(2, BRUTAL, -1)); CHECK(tier_wakes(3, BRUTAL, -1));
    CHECK(tier_wakes(1, PSYCHO, -1)); CHECK(tier_wakes(2, PSYCHO, -1)); CHECK(tier_wakes(3, PSYCHO, -1));
    CHECK(!tier_wakes(1, PSYCHO, 0)); CHECK(tier_wakes(1, NORMAL, 1)); CHECK(!tier_wakes(2, NORMAL, 1)); CHECK(tier_wakes(2, NORMAL, 2));

    // psycho-tune: PSYCHO wakes wake_share_psycho of each tier's serials, evenly spread; BRUTAL wakes all of its tiers
    CHECK(share_pick(1, 1.0f) && share_pick(7, 1.5f) && !share_pick(1, 0.0f) && !share_pick(0, 0.6f));
    { int n = 0; for (int k = 1; k <= 100; ++k) n += share_pick(k, 0.6f); CHECK(n == 60); }
    { int n = 0; for (int k = 1; k <= 100; ++k) n += share_pick(k, 0.75f); CHECK(n == 75); }
    { int n = 0; for (int k = 1; k <= 7; ++k) n += share_pick(k, 0.6f); CHECK(n == 5); }   // never more than ceil(k x share)
    CHECK(share_pick(1, 0.6f) && share_pick(2, 0.6f) && !share_pick(3, 0.6f) && share_pick(4, 0.6f) && !share_pick(5, 0.6f));
    { bool ok = true; for (int k = 1; k <= 200; ++k) { int n = 0; for (int j = 1; j <= k; ++j) n += share_pick(j, 0.6f);
          if (n != (k * 600 + 999) / 1000) ok = false; } CHECK(ok); }   // the first k serials hold ceil(0.6 k) picks: small blocks too
    {
        Cfg d;   // the shipped defaults
        CHECK(fabsf(d.wake_share_psycho - 0.6f) < 1e-6f && fabsf(d.cap_psycho - 1.3f) < 1e-6f && fabsf(d.cap_brutal - 1.5f) < 1e-6f);
        int brutal = 0, psycho = 0, psycho_old = 0;
        Cfg o; o.wake_share_psycho = 1.0f;
        for (int tier = 1; tier <= 3; ++tier) for (int k = 1; k <= 40; ++k) {
            brutal += clone_wakes(tier, k, BRUTAL, d); psycho += clone_wakes(tier, k, PSYCHO, d); psycho_old += clone_wakes(tier, k, PSYCHO, o);
        }
        CHECK(brutal == 80);         // BRUTAL: tiers 1 and 3, every serial (unchanged)
        CHECK(psycho_old == 120);    // the old PSYCHO: all three tiers
        CHECK(psycho == 72);         // 60 % of them
        CHECK(!clone_wakes(1, 1, NORMAL, d) && !clone_wakes(2, 1, BRUTAL, d) && !clone_wakes(1, 3, PSYCHO, d));
        Cfg f = d; f.force_tier = 2; CHECK(!clone_wakes(1, 3, NORMAL, f) && clone_wakes(1, 4, NORMAL, f));   // force_tier 2 = PSYCHO's rule
        f.force_tier = 1; CHECK(clone_wakes(1, 3, PSYCHO, f));   // force_tier 1 = BRUTAL's: every serial
    }

    Cfg c;
    CHECK(live_cap(NORMAL, 6, 14, c) == 0); CHECK(live_cap(HARD, 8, 14, c) == 0); CHECK(live_cap(-1, 6, 14, c) == 0);
    // psycho-tune: PSYCHO x1.3 (was x1.85 = 11 / 13), BRUTAL x1.5 unchanged
    CHECK(live_cap(BRUTAL, 6, 14, c) == 9); CHECK(live_cap(PSYCHO, 6, 14, c) == 8);
    CHECK(live_cap(BRUTAL, 8, 14, c) == 12); CHECK(live_cap(PSYCHO, 8, 14, c) == 10);
    { Cfg o; o.cap_psycho = 1.85f; CHECK(live_cap(PSYCHO, 6, 14, o) == 11); CHECK(live_cap(PSYCHO, 8, 14, o) == 13); }   // the old ini
    CHECK(live_cap(PSYCHO, 8, 9, c) == 0);             // no spare shell above the game's own value
    CHECK(live_cap(PSYCHO, 6, 9, c) == 8);             // never the last shell
    CHECK(live_cap(BRUTAL, 0, 14, c) == 0);

    CHECK(!strcmp(slot_format(0, "Slot%02d", true), "BFSlot%02d"));
    CHECK(!strcmp(slot_format(10, "Slot%02d", true), "BFSlot%02d"));
    CHECK(!strcmp(slot_format(11, "Slot%02d", true), "Slot%02d"));
    CHECK(!strcmp(slot_format(3, "Slot%02d", false), "Slot%02d"));

    float x, z;
    for (int i = 0; i < 24; ++i) {
        CHECK(ring_spot(i, 5, &x, &z));
        float r = sqrtf(x * x + z * z);
        CHECK(r > 2.0f && r < 4.8f);
    }
    CHECK(!ring_spot(24, 5, &x, &z)); CHECK(!ring_spot(-1, 5, &x, &z));

    CHECK(squads_for_heat(0, c) == 0); CHECK(squads_for_heat(2, c) == 0); CHECK(squads_for_heat(3, c) == 1);
    CHECK(squads_for_heat(4, c) == 2); CHECK(squads_for_heat(5, c) == 3);
    CHECK(squad_may_start(3, 0, 0, false, false, false, c));
    CHECK(!squad_may_start(3, 1, 0, false, false, false, c));
    CHECK(!squad_may_start(2, 0, 0, false, false, false, c));
    CHECK(!squad_may_start(5, 0, 4, false, false, false, c));       // the game's 4 car slots are taken
    CHECK(!squad_may_start(5, 0, 0, true, false, false, c));        // buddy or escort
    CHECK(!squad_may_start(5, 0, 0, false, true, false, c));        // cut-scene
    c.squads_in_missions = false;
    CHECK(!squad_may_start(5, 0, 0, false, false, true, c));
    CHECK(squad_may_start(5, 0, 0, false, false, false, c));
    CHECK(is_free_roam(2)); CHECK(is_free_roam(19)); CHECK(is_free_roam(25));
    CHECK(!is_free_roam(0)); CHECK(!is_free_roam(20)); CHECK(!is_free_roam(-1));

    printf("bigger fights: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
