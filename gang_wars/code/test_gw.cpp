// test_gw.cpp - unit tests of the gang wars logic (gang_wars/code/gw_logic.cpp). Built and run by dll/build.bat.
// No game, no game memory. The vanilla matrix is the shipped 19 x 19 FactionRelations of CentralTargetSystem.fragment
// (from the author's reverse-engineering notes, not included), copied into rows 19..37 / columns
// 19..37 the way the game's builder does for the modified copies. Exit code 0 = passed.
#include "gw_logic.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace gw;

static const int SHIPPED[19][19] = {
    {8, 2, 4, 2, 2, 4, 2, 4, 2, 4, 1, 2, 2, 1, 2, 4, 2, 2, 1},   //  0 Player
    {2, 8, 1, 2, 4, 2, 2, 2, 2, 4, 1, 2, 2, 4, 2, 2, 2, 2, 4},   //  1 Morales
    {2, 2, 8, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4},   //  2 ExplosiveObject
    {2, 2, 2, 8, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},   //  3 Unknown
    {2, 4, 2, 2, 8, 2, 2, 2, 2, 4, 1, 2, 2, 4, 2, 2, 2, 2, 1},   //  4 Virgilio
    {2, 2, 2, 2, 2, 8, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},   //  5 MissionTarget
    {2, 2, 2, 2, 2, 2, 8, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},   //  6 Civilian
    {2, 2, 2, 8, 2, 2, 2, 8, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2},   //  7 Animal
    {4, 2, 2, 2, 2, 2, 2, 2, 8, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2},   //  8 Taskgiver
    {4, 4, 4, 4, 4, 4, 2, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 2, 4},   //  9 Enemy
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 1, 1, 1, 2, 1, 1, 1, 1, 2},   // 10 Friend
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},   // 11 Neutral
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 2, 8, 4, 2, 2, 2, 2, 4},   // 12 JohnsonTarget
    {1, 4, 4, 2, 4, 4, 4, 4, 2, 4, 2, 4, 4, 8, 4, 4, 4, 2, 1},   // 13 Wrestler
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 1, 2, 2, 2, 8, 4, 2, 2, 2},   // 14 Police
    {2, 2, 4, 2, 2, 2, 2, 2, 2, 4, 2, 2, 2, 2, 2, 4, 2, 2, 2},   // 15 IdiotDriver
    {2, 4, 4, 2, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 0, 0, 8, 0, 4},   // 16 Gang01
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 8, 2},   // 17 Unused02
    {1, 4, 4, 2, 4, 4, 4, 4, 2, 4, 2, 4, 4, 1, 4, 4, 4, 2, 8},   // 18 MexicanBuddy
};
enum { N = 38 };
static int g_vanilla[N][N];
static int vanilla(void*, int r, int c) { return g_vanilla[r][c]; }
static void build_vanilla() { for (int r = 0; r < N; ++r) for (int c = 0; c < N; ++c) g_vanilla[r][c] = SHIPPED[r % 19][c % 19]; }

static Cfg cfg_default() {
    Cfg c; c.enabled = true; default_groups(c);
    char bad[64];
    parse_pairs(c, "cartel-virgilio, cartel-elvez, army-mexpolice, dea-outlaw, dea-border, outlaw-border, gangster-cartel, gangster-virgilio, gangster-elvez", bad, sizeof bad);
    return c;
}

static void test_parse() {
    int v[8];
    CHECK(parse_ints("0, 1 ,6", v, 8) == 3 && v[0] == 0 && v[1] == 1 && v[2] == 6);
    CHECK(parse_ints("", v, 8) == 0 && parse_ints(nullptr, v, 8) == 0);
    CHECK(parse_ints("4 16 17 3 ; comment", v, 8) == 4 && v[3] == 3);
    CHECK(parse_ints("1,2,3,4,5,6,7,8,9,10", v, 8) == 8);
    Cfg c; default_groups(c);
    CHECK(c.ngroups == 9);
    CHECK(group_of_model(c, 0) == find_group(c, "cartel") && group_of_model(c, 1) == find_group(c, "cartel"));
    CHECK(group_of_model(c, 3) == find_group(c, "virgilio") && group_of_model(c, 22) == find_group(c, "elvez"));
    CHECK(group_of_model(c, 30) == find_group(c, "outlaw") && group_of_model(c, 9) == find_group(c, "dea"));
    CHECK(group_of_model(c, 6) == -1 && group_of_model(c, 23) == -1 && group_of_model(c, 20) == -1);   // Cezar, Civil, Ram
    CHECK(find_group(c, "CARTEL") == find_group(c, "cartel") && find_group(c, "nobody") == -1);
    CHECK(add_group(c, "cartel", "0, 1, 6") == find_group(c, "cartel") && c.ngroups == 9 && group_of_model(c, 6) == find_group(c, "cartel"));
    CHECK(add_group(c, "x", "") == -1 && add_group(c, "", "1") == -1 && add_group(c, "averyveryverylongname", "1") == -1);
    char bad[64];
    int n = parse_pairs(c, "cartel-virgilio, army - mexpolice ; nobody-cartel, cartel, cartel-cartel", bad, sizeof bad);
    CHECK(n == 2);
    int ca = find_group(c, "cartel"), vi = find_group(c, "virgilio"), ar = find_group(c, "army"), mx = find_group(c, "mexpolice");
    CHECK(c.hostile[ca][vi] && c.hostile[vi][ca] && c.hostile[ar][mx] && c.hostile[mx][ar] && !c.hostile[ca][ar] && !c.hostile[ca][ca]);
    CHECK(strstr(bad, "nobody-cartel") && strstr(bad, "cartel-cartel"));
    CHECK(any_rival(c, ca) && !any_rival(c, find_group(c, "dea")));
}

static void test_alloc() {
    Cfg c = cfg_default(); Design d;
    CHECK(alloc_spare(c, d, 0) == 4 && alloc_spare(c, d, 0) == 4);
    CHECK(alloc_spare(c, d, 1) == 16 && alloc_spare(c, d, 2) == 17 && alloc_spare(c, d, 3) == 3);
    CHECK(alloc_spare(c, d, 4) == -1);                              // four spares by default
    release_spare(d, 1); CHECK(alloc_spare(c, d, 4) == 16);
    Cfg bad = c; bad.nspares = 3; bad.spares[0] = 9; bad.spares[1] = 0; bad.spares[2] = 25;   // never 9, 0, or a modified id
    Design e; CHECK(alloc_spare(bad, e, 0) == -1);
}

static void test_matrix() {
    build_vanilla();
    Cfg c = cfg_default(); Design d; d.base = 19;
    int ca = find_group(c, "cartel"), vi = find_group(c, "virgilio"), el = find_group(c, "elvez");
    d.alloc[ca] = 4; d.alloc[el] = 16;
    // not ours: every cell without a spare row / column
    CHECK(desired_cell(c, d, 9, 14, vanilla, 0) == -1 && desired_cell(c, d, 0, 9, vanilla, 0) == -1 && desired_cell(c, d, 14, 9, vanilla, 0) == -1);
    // gang vs gang
    CHECK(desired_cell(c, d, 4, 16, vanilla, 0) == REL_HOSTILE && desired_cell(c, d, 16, 4, vanilla, 0) == REL_HOSTILE);
    CHECK(desired_cell(c, d, 4 + 19, 16, vanilla, 0) == REL_HOSTILE && desired_cell(c, d, 16, 4 + 19, vanilla, 0) == REL_HOSTILE);
    CHECK(desired_cell(c, d, 4, 4, vanilla, 0) == REL_SELF && desired_cell(c, d, 4, 23, vanilla, 0) == REL_SELF);
    // Ram: neutral towards Ram while at war (provoke), hostile once provoked; Ram always sees them as enemies
    CHECK(desired_cell(c, d, 4, 0, vanilla, 0) == REL_NEUTRAL && desired_cell(c, d, 23, 0, vanilla, 0) == REL_HOSTILE);
    CHECK(desired_cell(c, d, 0, 4, vanilla, 0) == REL_HOSTILE && desired_cell(c, d, 0, 23, vanilla, 0) == REL_HOSTILE);
    Design np = d; np.provoke = false;
    CHECK(desired_cell(c, np, 4, 0, vanilla, 0) == REL_HOSTILE);
    // faction 9 and the gangs are friends (excluded enemies are never a rival's target)
    CHECK(desired_cell(c, d, 4, 9, vanilla, 0) == REL_FRIEND && desired_cell(c, d, 9, 4, vanilla, 0) == REL_FRIEND);
    CHECK(desired_cell(c, d, 4, 28, vanilla, 0) == REL_FRIEND && desired_cell(c, d, 28, 16, vanilla, 0) == REL_FRIEND);
    // everyone else as faction 9: police and DEA friends hostile both ways, civilians neutral
    CHECK(desired_cell(c, d, 4, 14, vanilla, 0) == 4 && desired_cell(c, d, 14, 4, vanilla, 0) == 4);
    CHECK(desired_cell(c, d, 4, 10, vanilla, 0) == 4 && desired_cell(c, d, 10, 4, vanilla, 0) == 4);
    CHECK(desired_cell(c, d, 4, 6, vanilla, 0) == 2 && desired_cell(c, d, 6, 4, vanilla, 0) == 2);
    CHECK(desired_cell(c, d, 4, 33, vanilla, 0) == g_vanilla[9][33] && desired_cell(c, d, 33, 16, vanilla, 0) == g_vanilla[33][9]);
    // unallocated spares go back to the shipped values; an unused spare is friend to the gangs
    CHECK(desired_cell(c, d, 17, 14, vanilla, 0) == g_vanilla[17][14] && desired_cell(c, d, 3, 0, vanilla, 0) == g_vanilla[3][0]);
    CHECK(desired_cell(c, d, 4, 17, vanilla, 0) == REL_FRIEND && desired_cell(c, d, 17, 4, vanilla, 0) == REL_FRIEND);
    // non-rival pair: cartel vs army (not configured) -> friends
    int ar = find_group(c, "army"); d.alloc[ar] = 17;
    CHECK(desired_cell(c, d, 4, 17, vanilla, 0) == REL_FRIEND);
    (void)vi;
    // the kill-count rule: row 0 holds 4 for every faction a gang member can hold
    for (int g = 0; g < MAX_GROUPS; ++g) if (d.alloc[g] >= 0) { CHECK(desired_cell(c, d, 0, d.alloc[g], vanilla, 0) == 4); CHECK(desired_cell(c, d, 0, d.alloc[g] + 19, vanilla, 0) == 4); }
    // the vision query hostile bit: cartel (4) sees elvez (16) and Ram only when provoked; faction 9 sees nobody of them
    CHECK((desired_cell(c, d, 4, 16, vanilla, 0) & 4) && !(desired_cell(c, d, 4, 0, vanilla, 0) & 4) && !(desired_cell(c, d, 9, 16, vanilla, 0) & 4));
}

static Member mk(int id, int group, float x, float z) {
    Member m; m.id = id; m.group = group; m.eligible = true; m.alive = true; m.has_pos = true;
    m.pos[0] = x; m.pos[1] = 0; m.pos[2] = z; m.faction = 9; m.orig = 9; m.state = ST_CALM;
    m.target_rival = true;   // fighting by default (test_step_idle covers the idle rule)
    return m;
}
static void apply(Member* m, int n, const Out* o) { for (int i = 0; i < n; ++i) if (o[i].want_faction >= 0) m[i].faction = o[i].want_faction; }

static void test_step_proximity() {
    Cfg c = cfg_default(); Design d; Spark sp; Out o[8];
    int ca = find_group(c, "cartel"), el = find_group(c, "elvez"), ar = find_group(c, "army");
    Member m[6] = {mk(1, ca, 0, 0), mk(2, ca, 3, 0), mk(3, el, 60, 0), mk(4, el, 62, 0), mk(5, ar, 1, 1), mk(6, -1, 0, 1)};
    step(c, d, m, 6, sp, 0.25f, o);
    for (int i = 0; i < 6; ++i) CHECK(m[i].state == ST_CALM && o[i].want_faction == -1);   // 60 m apart: nothing
    CHECK(d.alloc[ca] == -1 && d.alloc[el] == -1);
    m[2].pos[0] = 20; m[3].pos[0] = 40;                              // one elvez guard walks within 25 m of the cartel
    step(c, d, m, 6, sp, 0.25f, o);
    CHECK(m[0].state == ST_WAR && m[1].state == ST_WAR && m[2].state == ST_WAR && m[3].state == ST_CALM);
    CHECK(m[4].state == ST_CALM && m[5].state == ST_CALM);           // army: no rival pair; no gang: never
    CHECK(o[0].want_faction == d.alloc[ca] && o[2].want_faction == d.alloc[el] && d.alloc[ca] != d.alloc[el]);
    CHECK(o[0].rival == 2 && fabsf(o[0].rival_d - 20.0f) < 1e-3f && o[3].want_faction == -1);
    apply(m, 6, o);
    step(c, d, m, 6, sp, 0.25f, o);                                   // steady: nothing more to write
    for (int i = 0; i < 6; ++i) CHECK(o[i].want_faction == -1);
    // hysteresis: 30 m (inside 25 + 8) keeps the fight
    m[2].pos[0] = 33; step(c, d, m, 6, sp, 0.25f, o);
    CHECK(m[0].state == ST_WAR && m[2].state == ST_WAR && m[0].calm_t == 0.0f);
    // apart: calm after calm_s
    m[2].pos[0] = 80; m[3].pos[0] = 90;
    float t = 0; int steps = 0;
    while (m[0].state == ST_WAR && steps < 100) { step(c, d, m, 6, sp, 0.25f, o); apply(m, 6, o); t += 0.25f; ++steps; }
    CHECK(m[0].state == ST_CALM && fabsf(t - c.calm_s) < 0.3f && m[0].faction == 9 && m[2].faction == 9);
    step(c, d, m, 6, sp, 0.25f, o);
    CHECK(d.alloc[ca] == -1 && d.alloc[el] == -1);                    // spares released when nobody holds them
}

static void test_step_game_changes() {
    Cfg c = cfg_default(); Design d; Spark sp; Out o[4];
    int ca = find_group(c, "cartel"), vi = find_group(c, "virgilio");
    Member m[3] = {mk(1, ca, 0, 0), mk(2, vi, 5, 0), mk(3, vi, 7, 0)};
    step(c, d, m, 3, sp, 0.25f, o); apply(m, 3, o);
    CHECK(m[0].faction == d.alloc[ca] && m[1].faction == d.alloc[vi]);
    // Ram shoots member 1: the game's neutral rule moves it (and its friends) to the modified copy
    m[1].faction = d.alloc[vi] + 19; m[2].faction = d.alloc[vi] + 19;
    step(c, d, m, 3, sp, 0.25f, o);
    CHECK(m[1].state == ST_PROVOKED && m[2].state == ST_PROVOKED && o[1].changed && o[1].want_faction == -1);
    // provoked members still fight the rival and go back to the game's faction when calm
    m[0].pos[0] = 200;
    for (int k = 0; k < 40; ++k) { step(c, d, m, 3, sp, 0.25f, o); apply(m, 3, o); }
    CHECK(m[1].state == ST_CALM && m[1].faction == 9 && m[0].faction == 9);
    // the game takes a member (a script sets another faction): released, never written again
    m[0].pos[0] = 0;
    step(c, d, m, 3, sp, 0.25f, o); apply(m, 3, o);
    CHECK(m[0].state == ST_WAR);
    m[0].faction = 14;
    step(c, d, m, 3, sp, 0.25f, o);
    CHECK(m[0].state == ST_RELEASED && o[0].want_faction == -1);
    for (int k = 0; k < 5; ++k) { step(c, d, m, 3, sp, 0.25f, o); CHECK(o[0].want_faction == -1); }
    // a calm member whose faction the game changed: released at once
    Member n[2] = {mk(1, ca, 0, 0), mk(2, vi, 100, 0)}; n[0].faction = 28; Design d2; Spark s2;
    step(c, d2, n, 2, s2, 0.25f, o);
    CHECK(n[0].state == ST_RELEASED);
}

static void test_step_eligibility() {
    Cfg c = cfg_default(); Design d; Spark sp; Out o[4];
    int ca = find_group(c, "cartel"), vi = find_group(c, "virgilio");
    Member m[3] = {mk(1, ca, 0, 0), mk(2, vi, 5, 0), mk(3, vi, 6, 0)};
    m[2].eligible = false;                                             // e.g. a mounted gunner: not moved, not a rival's reason
    step(c, d, m, 3, sp, 0.25f, o); apply(m, 3, o);
    CHECK(m[0].state == ST_WAR && m[1].state == ST_WAR && m[2].state == ST_CALM && m[2].faction == 9);
    m[1].eligible = false;                                             // member 1 is taken by a script: back to 9 at once
    step(c, d, m, 3, sp, 0.25f, o);
    CHECK(m[1].state == ST_CALM && o[1].want_faction == 9);
    apply(m, 3, o);
    // nobody eligible near member 0 any more: it calms after calm_s
    for (int k = 0; k < 40; ++k) { step(c, d, m, 3, sp, 0.25f, o); apply(m, 3, o); }
    CHECK(m[0].state == ST_CALM && m[0].faction == 9);
    // the dead keep their faction (the death already counted with it)
    Member q[2] = {mk(1, ca, 0, 0), mk(2, vi, 5, 0)}; Design d3; Spark s3;
    step(c, d3, q, 2, s3, 0.25f, o); apply(q, 2, o);
    int fq = q[0].faction; q[0].alive = false;
    step(c, d3, q, 2, s3, 0.25f, o);
    CHECK(o[0].want_faction == -1 && q[0].faction == fq);
    // no spare left: stays calm, says why
    Cfg one = c; one.nspares = 1; Design d4; Spark s4;
    Member r[2] = {mk(1, ca, 0, 0), mk(2, vi, 5, 0)};
    step(one, d4, r, 2, s4, 0.25f, o);
    CHECK((r[0].state == ST_WAR) != (r[1].state == ST_WAR) || (r[0].state == ST_CALM && r[1].state == ST_CALM));
}

static void test_step_idle() {
    Cfg c = cfg_default(); Design d; Spark sp; Out o[4];
    int ca = find_group(c, "cartel"), el = find_group(c, "elvez");
    Member m[2] = {mk(1, ca, 0, 0), mk(2, el, 15, 0)};
    step(c, d, m, 2, sp, 0.25f, o); apply(m, 2, o);
    CHECK(m[0].state == ST_WAR && m[1].state == ST_WAR);
    // member 0 fights (its target is the rival), member 1 never gets it in sight (a wall)
    float t = 0;
    for (int k = 0; k < 40; ++k) { m[0].target_rival = true; m[1].target_rival = false; step(c, d, m, 2, sp, 0.25f, o); apply(m, 2, o); t += 0.25f; if (m[1].state == ST_CALM) break; }
    CHECK(m[0].state == ST_WAR && m[1].state == ST_CALM && m[1].faction == 9 && fabsf(t - c.idle_s) < 0.3f && m[1].cooldown_t > 0.0f);
    // cooldown: still near, no new war until retry_s passed
    int k2 = 0;
    while (m[1].state == ST_CALM && k2 < 200) { m[0].target_rival = false; step(c, d, m, 2, sp, 0.25f, o); apply(m, 2, o); ++k2; }
    CHECK(k2 * 0.25f >= c.retry_s - 0.3f && k2 * 0.25f <= c.retry_s + c.idle_s + 0.6f);
    // provoked members are not sent back by the idle rule
    Member q[2] = {mk(1, ca, 0, 0), mk(2, el, 15, 0)}; Design d2; Spark s2;
    step(c, d2, q, 2, s2, 0.25f, o); apply(q, 2, o);
    q[0].faction = d2.alloc[ca] + 19;
    for (int k = 0; k < 40; ++k) { step(c, d2, q, 2, s2, 0.25f, o); apply(q, 2, o); }
    CHECK(q[0].state == ST_PROVOKED);
}

static void test_step_spark() {
    Cfg c = cfg_default(); c.spark = true; Design d; Spark sp; Out o[4];
    int ca = find_group(c, "cartel"), vi = find_group(c, "virgilio");
    Member m[4] = {mk(1, ca, 0, 0), mk(2, ca, 4, 0), mk(3, vi, 10, 0), mk(4, vi, 100, 0)};
    for (int k = 0; k < 4; ++k) { step(c, d, m, 4, sp, 0.25f, o); for (int i = 0; i < 4; ++i) CHECK(m[i].state == ST_CALM); }   // near, no spark
    m[0].ram_hit = true;
    step(c, d, m, 4, sp, 0.25f, o); apply(m, 4, o); m[0].ram_hit = false;
    CHECK(sp.active && m[0].state == ST_PROVOKED && m[0].faction == d.alloc[ca] + 19);   // the one Ram shot goes after Ram
    CHECK(m[1].state == ST_WAR && m[2].state == ST_WAR && m[3].state == ST_CALM);           // the rest fight each other
    // a hit on a member with no rival near: no spark
    Cfg c2 = c; Design d2; Spark s2;
    Member n[2] = {mk(1, ca, 0, 0), mk(2, vi, 100, 0)}; n[0].ram_hit = true;
    step(c2, d2, n, 2, s2, 0.25f, o);
    CHECK(!s2.active && n[0].state == ST_CALM);
    // spark without provoke: the shot one fights the rivals too
    Cfg c3 = c; c3.provoke = false; Design d3; Spark s3;
    Member q[2] = {mk(1, ca, 0, 0), mk(2, vi, 8, 0)}; q[0].ram_hit = true;
    step(c3, d3, q, 2, s3, 0.25f, o);
    CHECK(q[0].state == ST_WAR && q[1].state == ST_WAR);
    // the spark ends when its area is quiet
    m[2].alive = false; m[1].alive = false; m[0].pos[0] = 300;
    for (int k = 0; k < 80; ++k) { step(c, d, m, 4, sp, 0.25f, o); apply(m, 4, o); }
    CHECK(!sp.active);
}

int main() {
    test_parse();
    test_alloc();
    test_matrix();
    test_step_proximity();
    test_step_game_changes();
    test_step_eligibility();
    test_step_spark();
    test_step_idle();
    printf("gang wars logic: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
