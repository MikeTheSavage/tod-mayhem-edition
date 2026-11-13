// test_smart.cpp - unit tests of the Smart AI logic (smart_ai/code/smart_logic.cpp). Built and run by dll/build.bat.
// No game, no game memory. Uses the shipped behaviour rows of TUNING.md (CharacterDict) as inputs. Exit code 0 = passed.
#include "smart_logic.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <initializer_list>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace sm;

// rows from the game's behaviour table, as listed in the author's tuning notes (Retreat, strafe, crouch, wait, CS, Pro*, ShootDodge_Min_dist)
static Sentry row(float retreat, float sf, float st, float cf, float ct, float wf, float wt, float hl, float cfor,
                  const float pro[8], float sdmin) {
    Sentry s;
    s.v[F_RETREAT] = retreat; s.v[F_STRAFE_FROM] = sf; s.v[F_STRAFE_TO] = st; s.v[F_CROUCH_FROM] = cf; s.v[F_CROUCH_TO] = ct;
    s.v[F_WAIT_FROM] = wf; s.v[F_WAIT_TO] = wt; s.v[F_CS_HEALTHLEFT] = hl; s.v[F_CS_CHANGE] = cfor;
    for (int i = 0; i < 8; ++i) s.v[PRO_FIRST + i] = pro[i];
    s.v[F_SD_MIN] = sdmin; s.v[F_STOPDIST] = 10.0f; s.health = 100.0f; s.faction = 9;
    return s;
}
static const float P_RANDOM[8] = {30, 30, 7, 0, 0, 0, 0, 0};
static const float P_CROUCH[8] = {100, 0, 0, 0, 0, 0, 0, 0};
static const float P_STAND[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static const float P_CMLONG[8] = {4, 44, 16, 0, 5, 5, 5, 0};
static const float P_VBSMALL[8] = {0, 48, 0, 3, 0, 0, 0, 0};

static void test_rng() {
    Rng a(123), b(123), c(124);
    bool same = true, diff = false;
    for (int i = 0; i < 100; ++i) { uint32_t x = a.u(), y = b.u(), z = c.u(); same = same && x == y; diff = diff || x != z; }
    CHECK(same); CHECK(diff);
    Rng r(7); float lo = 1, hi = 0;
    for (int i = 0; i < 10000; ++i) { float f = r.f(); if (f < lo) lo = f; if (f > hi) hi = f; CHECK(f >= 0.0f && f < 1.0f); if (g_fail) break; }
    CHECK(lo < 0.01f && hi > 0.99f);
    Rng z(0); CHECK(z.u() != 0);
    CHECK(mix(1, 2) != mix(2, 1) && mix(0, 0) != 0);
    float x = Rng(9).range(2.0f, 3.0f); CHECK(x >= 2.0f && x < 3.0f);
}

static void test_eligible() {
    Sentry s = row(3, .3f, .8f, 1, 3, 1, 3, 0, 0, P_RANDOM, 15);
    CHECK(eligible(s, 1000));
    Sentry b = s; b.is_buddy = 1; CHECK(!eligible(b, 1000));
    for (int st : {4, 7, 8, 9}) { Sentry t = s; t.subtype = st; CHECK(!eligible(t, 1000)); }
    for (int st : {7, 8, 9}) { Sentry t = s; t.root_subtype = st; CHECK(!eligible(t, 1000)); }
    Sentry t = s; t.root_subtype = 4; CHECK(eligible(t, 1000));
    for (int f : {6, 13}) { Sentry u = s; u.faction = f; CHECK(!eligible(u, 1000)); }
    Sentry boss = s; boss.health = 4200; CHECK(!eligible(boss, 1000)); CHECK(eligible(boss, 5000));
    Sentry dea = s; dea.health = 1000; CHECK(!eligible(dea, 1000)); dea.health = 999; CHECK(eligible(dea, 1000));   // 1000 hp = special
    Sentry f10 = s; f10.faction = 10; CHECK(!eligible(f10, 1000)); CHECK(eligible(f10, 1000, 0));                   // DEA = Ram's side
    Sentry f29 = s; f29.faction = 29; CHECK(eligible(f29, 1000)); CHECK(!eligible(f29, 1000, parse_factions("29", 0)));
    CHECK(parse_factions("6, 10, 13", 0) == DEFAULT_SKIP_FACTIONS && parse_factions(nullptr, 5) == 5 && parse_factions("", 5) == 5);
    CHECK(parse_factions("-", 5) == 0 && parse_factions("9 ; comment 7", 0) == (1ull << 9) && parse_factions("70,2", 0) == 4);
    Sentry dead = s; dead.health = 0; CHECK(!eligible(dead, 1000));
    Sentry nan = s; nan.v[F_PRO_STRAFE] = sqrtf(-1.0f); CHECK(!eligible(nan, 1000));
}

static bool pair_ok(const float* v, int a, int b) { return v[a] <= v[b] + 1e-5f; }

static void test_persona() {
    PersonaCfg c; c.enabled = true;
    Sentry s = row(3, .3f, .8f, 1, 3, 1, 3, 0, 0, P_RANDOM, 15);
    // disabled / ineligible: nothing changes
    { PersonaCfg off; Rng r(1); Persona p = make_persona(off, s, r, true); CHECK(!p.eligible && p.changed == 0 && p.role == ROLE_NONE); }
    { Sentry b = s; b.is_buddy = 1; Rng r(1); Persona p = make_persona(c, b, r, true); CHECK(!p.eligible && p.changed == 0); }
    // many enemies of each shipped row: invariants + role shares
    const float* rows[5] = {P_RANDOM, P_CROUCH, P_STAND, P_CMLONG, P_VBSMALL};
    const float sdmin[5] = {15, 15, 0, 14, 12}, retreat[5] = {3, 1.5f, 0, 3, 4};
    int roles[4] = {0, 0, 0, 0}, react = 0, stops_differ = 0;
    float minstop = 99, maxstop = 0;
    for (int k = 0; k < 5; ++k) {
        Sentry in = row(retreat[k], .3f, .8f, 1, 3, 1, 3, 50, 10, rows[k], sdmin[k]);
        float lastStop = -1;
        for (int seed = 1; seed <= 400; ++seed) {
            Rng r(mix(seed, k));
            Persona p = make_persona(c, in, r, true);
            CHECK(p.eligible);
            roles[p.role]++;
            for (int f = 0; f < F_COUNT; ++f) { CHECK(p.v[f] == p.v[f] && p.v[f] >= 0.0f); if (g_fail) return; }
            CHECK(pair_ok(p.v, F_STRAFE_FROM, F_STRAFE_TO) && pair_ok(p.v, F_CROUCH_FROM, F_CROUCH_TO) && pair_ok(p.v, F_WAIT_FROM, F_WAIT_TO));
            CHECK(pro_sum(p.v) <= 95.0f + 1e-3f || pro_sum(p.v) <= pro_sum(in.v) + 1e-3f);
            CHECK(p.v[F_STOPDIST] >= 3.0f && p.v[F_STOPDIST] <= 25.0f);
            CHECK(p.v[F_RETREAT] <= p.v[F_STOPDIST] * 0.6f + 1e-4f);
            CHECK(p.v[F_CS_HEALTHLEFT] == in.v[F_CS_HEALTHLEFT] && p.v[F_CS_CHANGE] == in.v[F_CS_CHANGE]);   // morale's job
            CHECK(!(p.changed & ((1u << F_CS_HEALTHLEFT) | (1u << F_CS_CHANGE))));
            if (p.role == ROLE_ASSAULT) { NEAR(p.v[F_STOPDIST], 7.0f, 1e-4); CHECK(p.v[F_RETREAT] <= in.v[F_RETREAT] + 1e-5f); }
            if (p.role == ROLE_CAUTIOUS) { NEAR(p.v[F_STOPDIST], 13.5f, 1e-4); CHECK((p.v[F_RETREAT] > 0.0f) == (in.v[F_RETREAT] > 0.0f)); }
            CHECK(p.v[F_SD_MIN] == in.v[F_SD_MIN]);                                      // never invented, never moved
            if (in.v[F_RETREAT] == 0.0f) CHECK(p.v[F_RETREAT] == 0.0f);                  // no back-off where the row has none
            if (!row_dodges(in.v)) for (int i = 0; i < 8; ++i) CHECK(p.v[PRO_FIRST + i] <= in.v[PRO_FIRST + i] * 1.25f * 1.3f + 1e-4f);
            if (p.role == ROLE_RIFLEMAN) CHECK(p.v[F_STOPDIST] >= 8.5f - 1e-4f && p.v[F_STOPDIST] <= 11.5f + 1e-4f);
            if (p.v[F_STOPDIST] < minstop) minstop = p.v[F_STOPDIST];
            if (p.v[F_STOPDIST] > maxstop) maxstop = p.v[F_STOPDIST];
            if (lastStop >= 0 && fabsf(lastStop - p.v[F_STOPDIST]) > 0.01f) ++stops_differ;
            lastStop = p.v[F_STOPDIST];
            float w = p.v[F_WAIT_AGGR];
            CHECK(w >= 0.05f && w <= c.react_max + 1e-4f); if (w > 0) ++react;
            // the changed mask is exactly the fields that differ
            for (int f = 0; f < F_COUNT; ++f) CHECK(((p.changed >> f) & 1u) == (fabsf(p.v[f] - in.v[f]) > 1e-6f ? 1u : 0u));
            // a weight that was 0 stays 0 (shoot-dodge included)
            for (int i = 0; i < 8; ++i) if (in.v[PRO_FIRST + i] == 0.0f) CHECK(p.v[PRO_FIRST + i] == 0.0f);
        }
    }
    int total = roles[1] + roles[2] + roles[3];
    printf("  roles over %d enemies: assault %d, rifleman %d, cautious %d; StopDistance %.1f..%.1f\n", total, roles[1], roles[2], roles[3], minstop, maxstop);
    CHECK(total == 2000 && roles[0] == 0);
    CHECK(roles[1] > total * 0.25 && roles[1] < total * 0.35);
    CHECK(roles[3] > total * 0.20 && roles[3] < total * 0.30);
    CHECK(react == total && stops_differ > 1000);
    // CrouchDefender (100 % crouch) keeps >= 5 % plain waits
    { Sentry in = row(1.5f, 0, 0, 9, 14, 0, 0, 0, 0, P_CROUCH, 15); Rng r(3); Persona p = make_persona(c, in, r, true); CHECK(pro_sum(p.v) <= 95.0f + 1e-3f); }
    // StandDefender (no moves, no dodge distance) and the melee set (StopDistance 1.5, no moves): no role adds a move,
    // a dodge or a back-off (an earlier build crashed after giving such enemies "dodge 0.0->8.0 pro 0->8")
    for (int seed = 1; seed < 200; ++seed) {
        Sentry in = row(0, 0, 0, 0, 0, 1, 3, 0, 0, P_STAND, 0); Rng r(seed); Persona p = make_persona(c, in, r, true);
        CHECK(p.v[F_SD_MIN] == 0.0f && pro_sum(p.v) == 0.0f && p.v[F_RETREAT] == 0.0f);
        Sentry m = row(0, 0, 0, 0, 0, 1, 3, 0, 0, P_STAND, 0); m.v[F_STOPDIST] = 1.5f; Rng r2(seed);
        Persona q = make_persona(c, m, r2, true);
        CHECK(q.v[F_SD_MIN] == 0.0f && pro_sum(q.v) == 0.0f && q.v[F_RETREAT] == 0.0f && q.v[F_STOPDIST] == 1.5f);
        CHECK(!(q.changed & ~((1u << F_WAIT_FROM) | (1u << F_WAIT_TO) | (1u << F_WAIT_AGGR))));   // only times + reaction
        // a dodge distance without any dodge weight is no dodge either
        Sentry d = row(0, 0, 0, 0, 0, 1, 3, 0, 0, P_STAND, 15); Rng r3(seed);
        Persona t = make_persona(c, d, r3, true); CHECK(pro_sum(t.v) == 0.0f);
    }
    // CMLong (ProShootDodge 0, three 5 % dodge combos): the bonus goes onto the combos only, ProShootDodge stays 0
    { int seen = 0;
      for (int seed = 1; seed < 100; ++seed) {
          Sentry in = row(3, .3f, .8f, 1, 3, 1, 3, 50, 8, P_CMLONG, 14); Rng r(seed); Persona p = make_persona(c, in, r, true);
          CHECK(p.v[F_PRO_SD] == 0.0f && p.v[F_PRO_STRAFE_SD_STRAFE] == 0.0f);
          float sd = p.v[F_PRO_SD_CROUCH] + p.v[F_PRO_SD_STRAFE_CROUCH] + p.v[F_PRO_CROUCH_SD_STRAFE];
          if (p.role == ROLE_ASSAULT) { CHECK(sd > 15.0f * 0.75f * 0.95f + 7.0f); ++seen; }   // jitter >= 0.75, +8 points (x95 % cap)
      }
      CHECK(seen > 10); }
    CHECK(row_dodges(row(0, 0, 0, 0, 0, 1, 3, 0, 0, P_CMLONG, 14).v) && !row_dodges(row(0, 0, 0, 0, 0, 1, 3, 0, 0, P_CMLONG, 0).v));
    CHECK(!row_dodges(row(0, 0, 0, 0, 0, 1, 3, 0, 0, P_RANDOM, 15).v));
    // a custom placeholder range (not 10 m) is kept
    { Sentry in = s; in.v[F_STOPDIST] = 25.0f; for (int seed = 1; seed < 50; ++seed) { Rng r(seed); Persona p = make_persona(c, in, r, true); NEAR(p.v[F_STOPDIST], 25.0f, 1e-6); } }
    // reaction: not without the tick, not over an authored delay
    { Rng r(5); Persona p = make_persona(c, s, r, false); CHECK(p.v[F_WAIT_AGGR] == 0.0f && !(p.changed & 1u)); }
    { Sentry in = s; in.v[F_WAIT_AGGR] = 3.0f; Rng r(5); Persona p = make_persona(c, in, r, true); NEAR(p.v[F_WAIT_AGGR], 3.0f, 1e-6); }
    { PersonaCfg nr = c; nr.reaction = false; Rng r(5); Persona p = make_persona(nr, s, r, true); CHECK(p.v[F_WAIT_AGGR] == 0.0f); }
    // same seed, same persona
    { Rng a(77), b(77); Persona p = make_persona(c, s, a, true), q = make_persona(c, s, b, true); CHECK(!memcmp(p.v, q.v, sizeof p.v) && p.role == q.role); }
    // jitter 0, shares 0: everyone a rifleman, only the range / retreat / dodge / reaction change
    { PersonaCfg z = c; z.jitter = 0; z.assault_share = 0; z.cautious_share = 0; z.dodge_bonus = 0;
      Rng r(11); Persona p = make_persona(z, s, r, true);
      CHECK(p.role == ROLE_RIFLEMAN);
      for (int f = F_STRAFE_FROM; f <= F_PRO_STRAFE_SD_STRAFE; ++f) NEAR(p.v[f], s.v[f], 1e-5);
      CHECK(p.v[F_RETREAT] == s.v[F_RETREAT]); }
    CHECK(!strcmp(role_name(ROLE_CAUTIOUS), "cautious") && !strcmp(field_name(F_STOPDIST), "StopDistance") && !strcmp(field_name(99), "?"));
}

static void test_reaction_states() {
    for (int st : {12, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 26, 27, 29}) CHECK(reaction_used(st));
    for (int st : {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 24, 28, 30, 31}) CHECK(!reaction_used(st));
    for (int st : {13, 17, 18, 19, 20, 22}) CHECK(squad_engaged_state(st));
    for (int st : {4, 14, 21, 23, 28, 30}) CHECK(!squad_engaged_state(st));
}

static SquadIn mk(float dist, int state = 20, uint32_t target = 0x1000, bool ranged = true, bool managed = true, bool engaged = true) {
    SquadIn s; s.managed = managed; s.engaged = engaged; s.ranged = ranged; s.state = state; s.target = target; s.target_sqr = dist * dist;
    return s;
}
static int count(const bool* h, int n) { int c = 0; for (int i = 0; i < n; ++i) c += h[i] ? 1 : 0; return c; }

static void test_squad() {
    SquadCfg c; c.enabled = true;
    // five on one target, two advance: the two closest get the tokens, three hold
    {
        SquadIn in[5] = {mk(20), mk(8), mk(15), mk(12), mk(18)}; SquadMem m[5]; bool h[5];
        int n = squad_assign(c, in, m, h, 5, 1000);
        CHECK(n == 3 && count(h, 5) == 3);
        CHECK(!h[1] && !h[3] && h[0] && h[2] && h[4]);
        CHECK(m[1].token && m[3].token && !m[0].token);
        // stable on the next tick
        n = squad_assign(c, in, m, h, 5, 1100); CHECK(n == 3 && !h[1] && !h[3]);
        // the token holders fight (20) for 8 s: tokens pass to the longest holders, the old holders cool down
        // (0, 2, 4 held equally long: the closest two, 2 at 15 m and 4 at 18 m, advance next)
        n = squad_assign(c, in, m, h, 5, 1000 + 8000);
        CHECK(n == 3 && h[1] && h[3] && h[0] && !h[2] && !h[4]);
        CHECK(m[1].cooldown_until == 9000 + 4000 && m[2].token && m[4].token);
        // a holder that never gets a shot (19) for 5 s is released to hunt
        in[0].state = 19; squad_assign(c, in, m, h, 5, 9100);
        n = squad_assign(c, in, m, h, 5, 9100 + 4900); CHECK(h[0]);
        n = squad_assign(c, in, m, h, 5, 9100 + 5000);
        CHECK(!h[0] && !m[0].token && n == 2);
        n = squad_assign(c, in, m, h, 5, 14200); CHECK(!h[0]);              // keeps hunting (no flip-flop) ...
        in[0].state = 17; n = squad_assign(c, in, m, h, 5, 14100 + 7900); CHECK(!h[0]);
        n = squad_assign(c, in, m, h, 5, 14100 + 8000); CHECK(h[0] && n == 3 && m[1].token && m[3].token);   // ... for one token time
    }
    // the far limit has 2 m of hysteresis
    {
        SquadIn in[4] = {mk(23), mk(10), mk(11), mk(12)}; SquadMem m[4]; bool h[4];
        squad_assign(c, in, m, h, 4, 0); CHECK(!h[0] && m[0].beyond);
        in[0].target_sqr = 21.0f * 21.0f; squad_assign(c, in, m, h, 4, 100); CHECK(!h[0] && m[0].beyond);
        in[0].target_sqr = 19.9f * 19.9f; squad_assign(c, in, m, h, 4, 200); CHECK(h[0] && !m[0].beyond);
        in[0].target_sqr = 21.0f * 21.0f; squad_assign(c, in, m, h, 4, 300); CHECK(h[0] && !m[0].beyond);
    }
    // fewer than min_squad: vanilla (everyone hunts)
    { SquadIn in[2] = {mk(10), mk(12)}; SquadMem m[2]; bool h[2]; CHECK(squad_assign(c, in, m, h, 2, 0) == 0 && !h[0] && !h[1]); }
    // melee, unmanaged, not engaged, no target: never hold, and they do not count for the squad
    {
        SquadIn in[6] = {mk(10), mk(11), mk(12, 21, 0x1000, false), mk(13, 20, 0x1000, true, false), mk(14, 4, 0x1000, true, true, false), mk(9, 20, 0)};
        SquadMem m[6]; bool h[6];
        int n = squad_assign(c, in, m, h, 6, 0);
        CHECK(n == 0 && !h[2] && !h[3] && !h[4] && !h[5]);   // only 2 real members < min 3
    }
    // too far to fight: hunts without a token
    {
        SquadIn in[4] = {mk(30), mk(10), mk(11), mk(12)}; SquadMem m[4]; bool h[4];
        int n = squad_assign(c, in, m, h, 4, 0);
        CHECK(!h[0] && !m[0].token && n == 1 && h[3]);
    }
    // two targets: two groups
    {
        SquadIn in[6] = {mk(10, 20, 1), mk(11, 20, 1), mk(12, 20, 1), mk(10, 20, 2), mk(11, 20, 2), mk(12, 20, 2)};
        SquadMem m[6]; bool h[6];
        int n = squad_assign(c, in, m, h, 6, 0);
        CHECK(n == 2 && h[2] && h[5]);
    }
    // lower limit: tokens dropped (newest first)
    {
        SquadIn in[5] = {mk(10), mk(11), mk(12), mk(13), mk(14)}; SquadMem m[5]; bool h[5];
        SquadCfg c3 = c; c3.max_advancing = 3; squad_assign(c3, in, m, h, 5, 0);
        CHECK(count(h, 5) == 2);
        CHECK(squad_assign(c, in, m, h, 5, 100) == 3);
    }
    // disabled: nobody holds, tokens cleared
    { SquadIn in[4] = {mk(10), mk(11), mk(12), mk(13)}; SquadMem m[4]; bool h[4]; squad_assign(c, in, m, h, 4, 0);
      SquadCfg off = c; off.enabled = false; CHECK(squad_assign(off, in, m, h, 4, 50) == 0 && !m[0].token && !m[0].holding); }
    // everybody on cooldown: nobody gets a token until it ends
    {
        SquadIn in[3] = {mk(10), mk(11), mk(12)}; SquadMem m[3]; bool h[3];
        for (int i = 0; i < 3; ++i) m[i].cooldown_until = 5000;
        CHECK(squad_assign(c, in, m, h, 3, 1000) == 3);
        CHECK(squad_assign(c, in, m, h, 3, 5000) == 1);
    }
}

static void test_morale() {
    MoraleCfg c; c.enabled = true; c.health_bonus = 100.0f; c.max_chance = 30.0f;   // round numbers for the checks
    Fear f; fear_add(f, 0.4f); NEAR(f.f, 0.4, 1e-6); fear_add(f, 0.9f); NEAR(f.f, 1.0, 1e-6); fear_add(f, -2); NEAR(f.f, 0.0, 1e-6);
    f.f = 0.4f; fear_decay(f, c, 5.0f); NEAR(f.f, 0.2, 1e-6); fear_decay(f, c, 100.0f); NEAR(f.f, 0.0, 1e-6); fear_decay(f, c, -1.0f); NEAR(f.f, 0.0, 1e-6);
    float hl, cf;
    morale_values(c, 50, 10, 0.0f, &hl, &cf); CHECK(hl == 50 && cf == 10);
    morale_values(c, 0, 0, 0.4f, &hl, &cf); NEAR(hl, 40, 1e-4); NEAR(cf, 12, 1e-4);
    morale_values(c, 70, 10, 1.0f, &hl, &cf); NEAR(hl, 170, 1e-4); NEAR(cf, 40, 1e-4);
    morale_values(c, 150, 90, 1.0f, &hl, &cf); NEAR(hl, 200, 1e-4); NEAR(cf, 100, 1e-4);
    morale_values(c, 250, 120, 1.0f, &hl, &cf); CHECK(hl == 250 && cf == 120);   // never below the authored row
    // what the game's check does with it (0x5C98C0): flee when hl*0.01 > 1 - damage/health and a roll < cf*0.01
    morale_values(c, 0, 0, 0.4f, &hl, &cf);
    CHECK(!(hl * 0.01f > 1.0f - 0.0f / 100.0f));     // unhurt: one death alone does not break it
    CHECK(hl * 0.01f > 1.0f - 70.0f / 100.0f);       // 30 % health left: it may (12 % per check)
    morale_values(c, 0, 0, 1.0f, &hl, &cf);
    CHECK(hl * 0.01f > 1.0f - 1.0f / 100.0f);        // full fear: even (almost) unhurt
    float a[3] = {0, 0, 0}, b[3] = {3, 4, 12}; NEAR(dist2(a, b), 169, 1e-4);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_rng(); test_eligible(); test_persona(); test_reaction_states(); test_squad(); test_morale();
    printf("test_smart: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
