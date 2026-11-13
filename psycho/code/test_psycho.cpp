// test_psycho.cpp - unit tests of psycho_logic.cpp (no game). Built and run by dll/build.bat.
#include "psycho_logic.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace ps;

static Cand enemy(uint32_t id, float x, float z, float hp = 200, int state = 20) {
    Cand c; c.root = id; c.ai = id + 1; c.pos = v3(x, 0, z); c.health = hp; c.damage = 0; c.state = state;
    c.faction = 9; c.hostile = true; c.sentry = true; c.has_gun = true; return c;
}
static World world(double t, float x = 0, float z = 0) { World w; w.t = t; w.ram = v3(x, 0, z); w.ram_ok = true; w.psycho = true; return w; }

static void test_parse() {
    CHECK(parse_moves("tornado, toro") == ((1u << MOVE_TORNADO) | (1u << MOVE_TORO)));
    CHECK(parse_moves("Tornado") == (1u << MOVE_TORNADO));
    CHECK(parse_moves("el_toro") == (1u << MOVE_TORO));
    CHECK(parse_moves("charge spin") == ((1u << MOVE_TORNADO) | (1u << MOVE_TORO)));
    CHECK(parse_moves("pinata, mariachi") == 0);
    CHECK(parse_moves("") == 0);
    float lo, hi;
    CHECK(parse_range("20-40", &lo, &hi) && lo == 20 && hi == 40);
    CHECK(parse_range("20, 40", &lo, &hi) && lo == 20 && hi == 40);
    CHECK(parse_range("30", &lo, &hi) && lo == 30 && hi == 30);
    CHECK(parse_range("40 20", &lo, &hi) && lo == 20 && hi == 40);
    CHECK(parse_range("12.5-17.5", &lo, &hi) && lo == 12.5f && hi == 17.5f);
    CHECK(!parse_range("x", &lo, &hi));
    CHECK(!strcmp(move_name(MOVE_TORO), "toro"));
    CHECK(in_combat(13) && in_combat(17) && in_combat(20) && in_combat(22) && in_combat(25));
    CHECK(!in_combat(2) && !in_combat(4) && !in_combat(6) && !in_combat(15) && !in_combat(23) && !in_combat(28) && !in_combat(31));
    Cfg c; c.cooldown_min = 50; c.cooldown_max = 10; c.telegraph = 0.0f; c.clamp();
    CHECK(c.cooldown_max >= c.cooldown_min); CHECK(c.telegraph >= 0.3f);
}

static void test_eligibility() {
    Cfg c; c.enabled = true; c.chance = 1.0f; c.first_delay = 0.0f; c.bosses = true; c.clamp();
    Registry reg; Rng rng;
    Cand e[4] = {enemy(100, 6, 0, 140), enemy(200, 8, 0, 200), enemy(300, 4, 0, 1100), enemy(400, 3, 0, 225, 4)};
    World w = world(10.0);
    // elite only: the 140-hp grunt never; the passive (state 4) soldier never; nearest elite / boss in a fight wins
    Pick p = pick(c, reg, w, e, 4, rng, false, 0);
    CHECK(p.index == 2);   // the boss at 4 m
    CHECK(p.move == MOVE_TORNADO);   // 4 m: below toro_min 5, tornado only
    e[2].health = 140;     // no boss any more
    p = pick(c, reg, w, e, 4, rng, false, 0);
    CHECK(p.index == 1);
    // not PSYCHO / off / a move running / Ram busy / global gap
    World w2 = w; w2.psycho = false; CHECK(pick(c, reg, w2, e, 4, rng, false, 0).index < 0);
    CHECK(pick(c, reg, w, e, 4, rng, true, 0).index < 0);
    World w3 = w; w3.ram_busy = true; CHECK(pick(c, reg, w3, e, 4, rng, false, 0).index < 0);
    CHECK(pick(c, reg, w, e, 4, rng, false, 11.0).index < 0);
    Cfg off = c; off.enabled = false; CHECK(pick(off, reg, w, e, 4, rng, false, 0).index < 0);
    // elite_only = 0: the grunt qualifies
    Cfg any = c; any.elite_only = false;
    Cand g[1] = {enemy(500, 6, 0, 100)};
    CHECK(pick(any, reg, w, g, 1, rng, false, 0).index == 0);
    CHECK(pick(c, reg, w, g, 1, rng, false, 0).index < 0);
    // dead / buddy / not hostile
    Cand d = enemy(600, 6, 0, 300); d.damage = 300; CHECK(pick(c, reg, w, &d, 1, rng, false, 0).index < 0);
    Cand b = enemy(700, 6, 0, 300); b.buddy = true; CHECK(pick(c, reg, w, &b, 1, rng, false, 0).index < 0);
    Cand f = enemy(800, 6, 0, 300); f.hostile = false; CHECK(pick(c, reg, w, &f, 1, rng, false, 0).index < 0);
    // no gun: no tornado (toro from 5 m)
    Cand ng = enemy(900, 4, 0, 300); ng.has_gun = false; CHECK(pick(c, reg, w, &ng, 1, rng, false, 0).index < 0);
    ng.pos = v3(7, 0, 0); ng.ability = MOVE_TORO; p = pick(c, reg, w, &ng, 1, rng, false, 0); CHECK(p.index == 0 && p.move == MOVE_TORO);
    ng.ability = MOVE_TORNADO; CHECK(pick(c, reg, w, &ng, 1, rng, false, 0).index < 0);   // given the spin, no gun: no move
    // only the toro enabled
    Cfg toro = c; toro.moves = 1u << MOVE_TORO;
    Cand t = enemy(1000, 3, 0, 300); CHECK(pick(toro, reg, w, &t, 1, rng, false, 0).index < 0);
    t.pos = v3(10, 0, 0); p = pick(toro, reg, w, &t, 1, rng, false, 0); CHECK(p.move == MOVE_TORO);
    // too far for both
    Cand far = enemy(1100, 30, 0, 300); CHECK(pick(c, reg, w, &far, 1, rng, false, 0).index < 0);
}

static void test_cooldowns() {
    Cfg c; c.enabled = true; c.chance = 1.0f; c.first_delay = 6.0f; c.clamp();
    Registry reg; Rng rng;
    Cand e = enemy(100, 6, 0, 300);
    World w = world(0.0);
    CHECK(pick(c, reg, w, &e, 1, rng, false, 0).index < 0);   // first seen now: waits first_delay
    w.t = 5.9; CHECK(pick(c, reg, w, &e, 1, rng, false, 0).index < 0);
    w.t = 6.1; CHECK(pick(c, reg, w, &e, 1, rng, false, 0).index == 0);
    Known* k = reg.find(100, 101); CHECK(k != nullptr);
    k->next_ok = w.t + 25.0;   // the glue sets the cooldown when a move starts
    w.t = 20; CHECK(pick(c, reg, w, &e, 1, rng, false, 0).index < 0);
    w.t = 31.2; CHECK(pick(c, reg, w, &e, 1, rng, false, 0).index == 0);
    // a reused pooled shell (same root, new AI) is a new enemy
    Cand r = e; r.ai = 999; w.t = 32; CHECK(pick(c, reg, w, &r, 1, rng, false, 0).index < 0);
    // chance 0: never
    Cfg z = c; z.chance = 0.0f; w.t = 100; CHECK(pick(z, reg, w, &e, 1, rng, false, 0).index < 0);
    // the roll: about chance of the checks pass
    Cfg h = c; h.chance = 0.35f; int pass = 0; Rng r2;
    for (int i = 0; i < 4000; ++i) { Registry rr; World ww = world(100.0); Cand ee = enemy(5, 6, 0, 300); h.first_delay = 0;
        if (pick(h, rr, ww, &ee, 1, r2, false, 0).index == 0) ++pass; }
    CHECK(pass > 1250 && pass < 1550);
    // registry: full -> the oldest goes
    Registry full; for (int i = 0; i < Registry::N + 5; ++i) full.get(1000 + i, 1, (double)i, c);
    CHECK(full.count() == Registry::N);
    CHECK(full.find(1000, 1) == nullptr && full.find(1000 + Registry::N + 4, 1) != nullptr);
}

// Eligibility rules: bosses off by default, VIP / escort / scripted enemies never, a pooled shell's new
// occupant waits first_delay again, Ram's voice never plays on an enemy, the hand-back watchdog's test
static void test_bugfix1() {
    Cfg c; c.enabled = true; c.chance = 1.0f; c.first_delay = 0.0f; c.clamp();
    CHECK(!c.bosses);                                   // the default
    Registry reg; Rng rng; World w = world(10.0);
    Cand boss = enemy(100, 4, 0, 4200, 22);
    CHECK(pick(c, reg, w, &boss, 1, rng, false, 0).index < 0);
    CHECK(!strcmp(reject_reason(c, reg, w, boss), "boss (bosses = 0)"));
    Cfg cb = c; cb.bosses = true; CHECK(pick(cb, reg, w, &boss, 1, rng, false, 0).index == 0);
    Cand elite = enemy(200, 4, 0, 200, 20); CHECK(pick(c, reg, w, &elite, 1, rng, false, 0).index == 0);
    Cand vip = elite; vip.root = 300; vip.ai = 301; vip.special = true;
    CHECK(pick(c, reg, w, &vip, 1, rng, false, 0).index < 0);
    CHECK(!strcmp(reject_reason(c, reg, w, vip), "special (VIP placement / escort / scripted)"));
    // the pool hands the same shell (root, ai) to a new placement: first_delay counts again from its arrival
    Cfg d = c; d.first_delay = 6.0f;
    Registry r2; Cand e = enemy(500, 6, 0, 200, 20); e.ph = 0x1000;
    World w0 = world(0.0); CHECK(pick(d, r2, w0, &e, 1, rng, false, 0).index < 0);
    World w7 = world(7.0); CHECK(pick(d, r2, w7, &e, 1, rng, false, 0).index == 0);
    Cand e2 = e; e2.ph = 0x2000;                        // same shell, another placeholder (the MM032 boss case)
    CHECK(pick(d, r2, w7, &e2, 1, rng, false, 0).index < 0);
    CHECK(!strcmp(reject_reason(d, r2, w7, e2), "just arrived (first_delay)"));
    World w14 = world(13.5); CHECK(pick(d, r2, w14, &e2, 1, rng, false, 0).index == 0);
    Cand e3 = e2; e3.health = 4200; e3.state = 20;      // same placeholder pointer, other max health: a new enemy too
    Cfg db = d; db.bosses = true; World w15 = world(14.0);
    CHECK(pick(db, r2, w15, &e3, 1, rng, false, 0).index < 0);
    Known* k = r2.find(500, 501); CHECK(k && k->moves == 0 && k->health == 4200.0f);
    // the old 4-argument get keeps working (no occupant known: nothing compared)
    Registry r3; Known* a1 = r3.get(1, 2, 1.0, d); Known* a2 = r3.get(1, 2, 5.0, d); CHECK(a1 == a2 && a2->first_seen == 1.0);
    // Ram's voice lines are never a sting (the default is none)
    CHECK(is_ram_voice(106) && is_ram_voice(107) && is_ram_voice(108) && is_ram_voice(109));
    CHECK(!is_ram_voice(105) && !is_ram_voice(110) && !is_ram_voice(95));
    CHECK(sting_sound(107) == -1 && sting_sound(106) == -1 && sting_sound(-1) == -1 && sting_sound(95) == 95);
    // hand-back watchdog: only state 2 (no AI control) asks for it
    CHECK(needs_handback(2));
    CHECK(!needs_handback(20) && !needs_handback(22) && !needs_handback(28) && !needs_handback(-1) && !needs_handback(6));
}

static EnemyNow at(float x, float z, float dmg = 0) { EnemyNow e; e.alive = true; e.damage = dmg; e.pos = v3(x, 0, z); return e; }

static void test_tornado() {
    Cfg c; c.enabled = true; c.clamp();
    Active a; Cand e = enemy(1, 6, 0, 300); World w = world(50.0);
    a.begin(c, MOVE_TORNADO, e, w);
    CHECK(a.phase == PH_TELEGRAPH);
    NEAR(a.telegraph_speed(c), 0.76, 1e-4);   // 19 frames at 25 fps over 1 s
    // telegraph: nothing hits
    for (double t = 50.0; t < 50.99; t += 0.016) { w.t = t; Step s = a.step(c, w, at(6, 0)); CHECK(!s.hit && !s.blank && !s.release); }
    w.t = 51.0; Step s = a.step(c, w, at(6, 0));
    CHECK(s.go_active && a.phase == PH_ACTIVE); NEAR(s.clip_speed, c.tornado_fire_speed, 1e-6);
    // active: blanks and hits, 14 frames / 25 / 0.4 = 1.4 s
    int hits = 0, blanks = 0; double t = 51.0; float dealt = 0;
    while (a.phase == PH_ACTIVE && t < 60) { t += 0.016; w.t = t; s = a.step(c, w, at(6, 0)); hits += s.hit; blanks += s.blank; if (s.hit) dealt += s.hit_damage; }
    NEAR(t - 51.0, 1.4, 0.03);
    CHECK(hits == 4);   // ticks at 0.175, 0.525, 0.875, 1.225
    NEAR(dealt, 4 * c.tornado_damage, 1e-3);
    CHECK(blanks >= 14 && blanks <= 17);
    CHECK(a.phase == PH_RECOVER);
    // recover: the clip's last 28 frames (1.12 s) + recover 0.5, then release with "done"
    double tr = t; bool rel = false;
    while (!rel && t < 70) { t += 0.016; w.t = t; s = a.step(c, w, at(6, 0)); rel = s.release; }
    NEAR(t - tr, 1.12 + c.recover, 0.03); CHECK(s.end == END_DONE); CHECK(!a.running());
}

static void test_tornado_dodge_cover_range() {
    Cfg c; c.enabled = true; c.clamp();
    for (int mode = 0; mode < 4; ++mode) {
        Active a; Cand e = enemy(1, 6, 0, 300); World w = world(0.0);
        a.begin(c, MOVE_TORNADO, e, w);
        w.t = 1.0; a.step(c, w, at(6, 0));
        int hits = 0, misses = 0; const char* why = "";
        for (double t = 1.0; a.phase == PH_ACTIVE && t < 5; t += 0.016) {
            w.t = t; w.ram_dodging = mode == 1;
            w.ram = mode == 3 ? v3(30, 0, 0) : v3(0, 0, 0);
            EnemyNow en = at(6, 0); en.los_ok = mode != 2;
            Step s = a.step(c, w, en); hits += s.hit; misses += s.miss; if (s.miss) why = s.miss_why;
        }
        if (mode == 0) CHECK(hits == 4 && misses == 0);
        if (mode == 1) { CHECK(hits == 0 && misses == 4); CHECK(strstr(why, "dodged") != nullptr); }
        if (mode == 2) { CHECK(hits == 0 && misses == 4); CHECK(strstr(why, "cover") != nullptr); }
        if (mode == 3) { CHECK(hits == 0 && misses == 4); CHECK(strstr(why, "range") != nullptr); }
    }
    // dive_dodges = 0: a dive does not save him
    Cfg nd = c; nd.dive_dodges = false;
    Active a; Cand e = enemy(1, 6, 0, 300); World w = world(0.0); a.begin(nd, MOVE_TORNADO, e, w);
    int hits = 0; for (double t = 0.0; t < 3.0; t += 0.016) { w.t = t; w.ram_dodging = true; hits += a.step(nd, w, at(6, 0)).hit; }
    CHECK(hits == 4);
}

static void test_interrupt_and_aborts() {
    Cfg c; c.enabled = true; c.clamp();
    // shot in the wind-up: cancelled, no hit
    Active a; Cand e = enemy(1, 6, 0, 300); e.damage = 40; World w = world(0.0);
    a.begin(c, MOVE_TORNADO, e, w);
    w.t = 0.3; Step s = a.step(c, w, at(6, 0, 40)); CHECK(!s.release);
    w.t = 0.5; s = a.step(c, w, at(6, 0, 52)); CHECK(s.release && s.end == END_INTERRUPTED && !a.running());
    // a hit while ACTIVE does not cancel (committed)
    a.begin(c, MOVE_TORNADO, e, world(0.0)); w = world(1.0); a.step(c, w, at(6, 0, 40)); CHECK(a.phase == PH_ACTIVE);
    w.t = 1.2; s = a.step(c, w, at(6, 0, 90)); CHECK(!s.release && a.phase == PH_ACTIVE);
    // enemy dead / Ram busy at any phase
    a.begin(c, MOVE_TORO, e, world(0.0)); EnemyNow dead = at(6, 0); dead.alive = false;
    w = world(0.2); s = a.step(c, w, dead); CHECK(s.release && s.end == END_ENEMY_DEAD);
    a.begin(c, MOVE_TORO, e, world(0.0)); w = world(0.2); w.ram_busy = true; s = a.step(c, w, at(6, 0)); CHECK(s.release && s.end == END_RAM_BUSY);
}

static void test_toro() {
    Cfg c; c.enabled = true; c.clamp();
    // enemy at (10, 0), Ram at the origin: the telegraph turns the bull toward Ram, the charge locks Ram's spot then
    Active a; Cand e = enemy(1, 10, 0, 300); World w = world(0.0);
    a.begin(c, MOVE_TORO, e, w);
    NEAR(a.yaw, -1.5708, 1e-3);   // facing -x
    w.ram = v3(0, 0, 3);           // Ram walks during the telegraph
    Step s;
    for (double t = 0.0; t < 0.99; t += 0.016) { w.t = t; s = a.step(c, w, at(10, 0)); CHECK(!s.hit); }
    CHECK(s.face);
    w.t = 1.0; s = a.step(c, w, at(10, 0));
    CHECK(s.go_active && a.phase == PH_ACTIVE);
    float d = sqrtf(100 + 9);
    NEAR(a.limit, d + c.toro_overrun, 1e-3);
    NEAR(a.dir.x, -10 / d, 1e-4); NEAR(a.dir.z, 3 / d, 1e-4);
    // the enemy does not move by itself: the logic places it along dir at toro_speed; Ram stays: hit at ~1.3 m
    Vec3 p = v3(10, 0, 0); bool hit = false; double t = 1.0; float dmg = 0; Vec3 imp = v3(0, 0, 0);
    while (a.phase == PH_ACTIVE && t < 5) {
        t += 0.016; w.t = t; s = a.step(c, w, at(p.x, p.z));
        if (s.place) p = s.place_pos;
        if (s.hit) { hit = true; dmg = s.hit_damage; imp = s.hit_impulse; }
    }
    CHECK(hit); CHECK(a.end == END_HIT); NEAR(dmg, c.toro_damage, 1e-4);
    NEAR(dist_xz(p, w.ram), c.toro_hit_radius, 0.2);
    CHECK(imp.x < 0 && imp.z > 0 && imp.y > 0);
    // Ram side-steps after the lock: no hit, the charge runs to its length (overrun)
    a.begin(c, MOVE_TORO, enemy(1, 10, 0, 300), world(0.0));
    w = world(0.0); for (double tt = 0.0; tt <= 1.0; tt += 0.016) { w.t = tt; a.step(c, w, at(10, 0)); }
    w.t = 1.0; a.step(c, w, at(10, 0));
    if (a.phase == PH_TELEGRAPH) { w.t = 1.01; a.step(c, w, at(10, 0)); }
    CHECK(a.phase == PH_ACTIVE);
    p = v3(10, 0, 0); t = w.t; int hits = 0;
    while (a.phase == PH_ACTIVE && t < 6) { t += 0.016; w.t = t; w.ram = v3(0, 0, 4); s = a.step(c, w, at(p.x, p.z)); if (s.place) p = s.place_pos; hits += s.hit; }
    CHECK(hits == 0); CHECK(a.end == END_DISTANCE); NEAR(a.travelled, 13.0, 0.1);
    // a wall ahead ends it
    a.begin(c, MOVE_TORO, enemy(1, 10, 0, 300), world(0.0));
    w = world(1.0); a.step(c, w, at(10, 0)); CHECK(a.phase == PH_ACTIVE);
    EnemyNow wall = at(9, 0); wall.wall_ahead = true; w.t = 1.2; s = a.step(c, w, wall);
    CHECK(s.recover && a.end == END_WALL);
    // a dive through the charge: missed, the charge goes on
    a.begin(c, MOVE_TORO, enemy(1, 10, 0, 300), world(0.0));
    w = world(1.0); a.step(c, w, at(10, 0));
    w.t = 1.9; w.ram_dodging = true; s = a.step(c, w, at(1.0, 0)); CHECK(!s.hit && s.miss && a.phase == PH_ACTIVE);
    // the enemy's own root motion runs ahead: never pulled back
    a.begin(c, MOVE_TORO, enemy(1, 10, 0, 300), world(0.0));
    w = world(1.0); a.step(c, w, at(10, 0));
    w.t = 1.1; s = a.step(c, w, at(7.0, 0)); CHECK(!s.place); NEAR(a.travelled, 3.0, 1e-3);
    // too long: time
    a.begin(c, MOVE_TORO, enemy(1, 10, 0, 300), world(0.0));
    w = world(1.0); a.step(c, w, at(10, 0));
    Cfg slow = c; slow.toro_speed = 1.0f; slow.toro_max_seconds = 0.5f;
    w.t = 1.6; s = a.step(slow, w, at(10, 0)); CHECK(a.end == END_TIME);
    // recover then release with the active phase's end reason
    bool rel = false; t = w.t; while (!rel && t < 10) { t += 0.016; w.t = t; s = a.step(slow, w, at(10, 0)); rel = s.release; }
    CHECK(rel && s.end == END_TIME);
}

static void test_fixed_ability() {
    Cfg c; c.enabled = true;
    const unsigned both = c.moves;
    // the same placement, the same move, every time; both moves occur over consecutive ids (the placements come in runs)
    int nt = 0, no = 0;
    for (uint32_t id = 0x1000; id < 0x1040; ++id) {
        int m = ability_for(id, both);
        CHECK(m == MOVE_TORNADO || m == MOVE_TORO);
        CHECK(m == ability_for(id, both));
        (m == MOVE_TORNADO ? nt : no)++;
    }
    CHECK(nt >= 16 && no >= 16);
    // one move enabled: that move
    CHECK(ability_for(7, 1u << MOVE_TORNADO) == MOVE_TORNADO);
    CHECK(ability_for(7, 1u << MOVE_TORO) == MOVE_TORO);
    CHECK(ability_for(7, 0) == MOVE_NONE);
    CHECK(goodie_of_move(MOVE_TORNADO) == 10 && goodie_of_move(MOVE_TORO) == 11 && goodie_of_move(MOVE_NONE) == 0);
    // pick uses that move only: an enemy fixed on the charge never spins, at a distance only the spin allows it stands
    Registry reg; Rng rng; Cfg h = c; h.chance = 1.0f; h.first_delay = 0;
    World w = world(50);
    Cand e = enemy(100, 3, 0, 300); e.ability = MOVE_TORO; reg.get(e.root, e.ai, 0, h);
    CHECK(pick(h, reg, w, &e, 1, rng, false, 0).index < 0);
    e.pos = v3(7, 0, 0);                                     // both moves fit: always its own
    for (int i = 0; i < 20; ++i) { Pick p = pick(h, reg, w, &e, 1, rng, false, 0); CHECK(p.index == 0 && p.move == MOVE_TORO); }
    e.ability = MOVE_TORNADO;
    for (int i = 0; i < 20; ++i) { Pick p = pick(h, reg, w, &e, 1, rng, false, 0); CHECK(p.index == 0 && p.move == MOVE_TORNADO); }
    e.pos = v3(14, 0, 0); CHECK(pick(h, reg, w, &e, 1, rng, false, 0).index < 0);   // past the spin's range
    // its move was switched off in the ini: it has none
    Cfg only = h; only.moves = 1u << MOVE_TORO; e.pos = v3(7, 0, 0); CHECK(pick(only, reg, w, &e, 1, rng, false, 0).index < 0);
}

// One placement, one drop, however many shells and lives it gets: a reinforcement's re-arm, a heat car's second trip (Bigger
// Fights hands the same three gunmen another shell), an extra enemy that wakes in any shell. A copy is a placement like any other.
static void test_drop_once_per_placement() {
    Cfg c; c.enabled = true; c.drops = true;
    EliteBook bk;
    Cand trip1 = enemy(10, 5, 0, 225); trip1.ph = 700; trip1.uid = 7;
    CHECK(drop_eligible(c, trip1));
    Elite* x = bk.add(trip1.ph, trip1.uid, MOVE_TORO); CHECK(x != nullptr); x->saw(trip1);
    CHECK(!wants_drop(*x));                                                             // alive: nothing owed
    x->dead = true; x->t_dead = 100; CHECK(wants_drop(*x));                             // the first trip's death
    x->dropped = true; x->t_drop = 101; CHECK(!wants_drop(*x));
    // the second trip: the same placement in another shell, re-armed
    Cand trip2 = enemy(11, 5, 0, 225); trip2.ph = 700; trip2.uid = 7;
    Elite* y = bk.find(trip2.ph); CHECK(y == x && bk.count() == 1); y->saw(trip2);
    CHECK(y->root == 11 && y->ai == trip2.ai && !y->dead && y->dropped && y->ability == MOVE_TORO);
    y->dead = true; y->t_dead = 200; CHECK(!wants_drop(*y));                            // dies again: nothing
    // a rewind past the second death: alive again, the first life's drop is not undone, so killed again it still has none
    CHECK(!undo_death(*y, 199.0) && !y->dead && y->dropped);
    y->dead = true; y->t_dead = 199.5; CHECK(!wants_drop(*y));
    // the first life made no drop (a cut-scene): the later life makes the one drop
    Cand other = enemy(12, 5, 0, 225); other.ph = 701; other.uid = 8;
    Elite* z = bk.add(other.ph, other.uid, MOVE_TORNADO); z->saw(other);
    z->dead = true; z->t_dead = 100; z->no_drop = true; CHECK(!wants_drop(*z));
    z->saw(other); z->dead = true; z->t_dead = 150; CHECK(wants_drop(*z));
    // a shell handed to another placement starts that placement's own entry
    Cand shell = trip1; shell.ph = 702; shell.uid = 9; CHECK(bk.find(shell.ph) == nullptr);
    // a full book never forgets a placement that dropped; one that did not is the one to go
    EliteBook full; Elite* first = nullptr;
    for (uint32_t i = 0; i < (uint32_t)EliteBook::N; ++i) {
        Elite* q = full.add(1000 + i, i, MOVE_TORO); q->dead = true; q->t_dead = 1.0 + i; q->dropped = true; q->t_drop = 2.0 + i;
        if (!i) first = q;
    }
    CHECK(full.add(5000, 1, MOVE_TORO) == nullptr && full.find(1000) == first);         // all dropped: nothing is thrown out
    full.find(1050)->dropped = false;
    CHECK(full.add(5000, 1, MOVE_TORO) != nullptr && full.find(1050) == nullptr && full.find(1000) == first && full.find(5000));
}

static void test_drop_rules() {
    Cfg c; c.enabled = true; c.drops = true;
    Cand e = enemy(1, 5, 0, 200);
    CHECK(drop_eligible(c, e));
    Cand g = enemy(2, 5, 0, 150); CHECK(!drop_eligible(c, g));                       // a grunt
    Cand d = e; d.damage = 200; CHECK(!drop_eligible(c, d));                         // dead
    Cand f = e; f.hostile = false; CHECK(!drop_eligible(c, f));                      // a friend
    Cand b = e; b.buddy = true; CHECK(!drop_eligible(c, b));
    Cand s = e; s.special = true; CHECK(!drop_eligible(c, s));                       // VIP, escort, scripted
    Cand bo = enemy(3, 5, 0, 1000); CHECK(!drop_eligible(c, bo));                    // a boss ...
    Cfg cb = c; cb.bosses = true; CHECK(drop_eligible(cb, bo));                      // ... unless bosses use moves
    // the shell: only a shell seen alive can die; a reused or empty shell says nothing
    CHECK(life_of(true, 200, 0, 20) == LIFE_ALIVE);
    CHECK(life_of(true, 200, 200, 20) == LIFE_DEAD);
    CHECK(life_of(true, 200, 10, 28) == LIFE_DEAD);
    CHECK(life_of(false, 200, 200, 28) == LIFE_GONE);
    CHECK(life_of(true, 200, 0, -1) == LIFE_GONE);
    // the body: at rest for a quarter second, or the time is up
    Elite x; x.t_dead = 100; x.rest_ok = true; x.t_rest = 100.1;
    CHECK(!body_settled(c, x, 100.2)); CHECK(body_settled(c, x, 100.4));
    x.t_rest = 101.4; CHECK(!body_settled(c, x, 101.45)); CHECK(body_settled(c, x, 101.6));
    x.rest_ok = false; CHECK(!body_settled(c, x, 101.0)); CHECK(body_settled(c, x, 101.5));
    // the place: on the body, unless it fell away from where the enemy stood or a wall is between them
    bool on = false; Vec3 st = v3(0, 0, 0);
    Vec3 p = drop_spot(c, st, v3(1, 0.1f, 1), true, true, &on); CHECK(on && p.x == 1);
    p = drop_spot(c, st, v3(1, 0.1f, 1), false, true, &on); CHECK(!on && p.x == 0);    // no ground under the body
    p = drop_spot(c, st, v3(1, 0.1f, 1), true, false, &on); CHECK(!on && p.x == 0);    // through a wall
    p = drop_spot(c, st, v3(5, 0, 0), true, true, &on); CHECK(!on && p.x == 0);        // thrown far
    p = drop_spot(c, st, v3(1, -3, 0), true, true, &on); CHECK(!on && p.x == 0);       // fell off the ledge
    // a rewind before the death: alive, the drop goes and it may drop again; a collected one stays collected
    Elite r; r.dead = true; r.t_dead = 50; r.dropped = true; r.t_drop = 51;
    CHECK(!undo_death(r, 50.5) && r.dead);                                             // still dead at that time
    CHECK(undo_death(r, 49.0) && !r.dead && !r.dropped);
    Elite k; k.dead = true; k.t_dead = 50; k.dropped = true; k.t_drop = 51; k.collected = true;
    CHECK(!undo_death(k, 49.0) && !k.dead && k.dropped);
    Elite w; w.dead = true; w.t_dead = 50;                                             // died, no drop yet
    CHECK(!undo_death(w, 49.0) && !w.dead && !w.dropped);
    CHECK(!clock_jump_is_load(8.0)); CHECK(clock_jump_is_load(30.0));
    // the book: one entry per placement, full = a finished one goes, never one that lives
    EliteBook bk;
    CHECK(bk.find(5) == nullptr); CHECK(bk.add(0, 1, 1) == nullptr);
    Elite* a = bk.add(5, 50, MOVE_TORO); CHECK(a && bk.find(5) == a && bk.count() == 1 && a->ability == MOVE_TORO);
    for (uint32_t i = 6; i < 6 + EliteBook::N - 1; ++i) CHECK(bk.add(i, i, MOVE_TORO) != nullptr);
    CHECK(bk.add(1000, 1, MOVE_TORO) == nullptr);                                      // all alive: nobody is thrown out
    bk.find(7)->dead = true; bk.find(7)->t_dead = 10;
    CHECK(bk.add(1000, 1, MOVE_TORNADO) != nullptr && bk.find(7) == nullptr && bk.find(1000) != nullptr);
    bk.clear(); CHECK(bk.count() == 0 && bk.find(5) == nullptr);
    test_drop_once_per_placement();
    Cfg z; z.drop_seconds = 0.1f; z.drop_max = 9; z.clamp(); CHECK(z.drop_seconds == 3.0f && z.drop_max == 5);
}

int main() {
    test_parse();
    test_eligibility();
    test_cooldowns();
    test_tornado();
    test_tornado_dodge_cover_range();
    test_interrupt_and_aborts();
    test_toro();
    test_bugfix1();
    test_fixed_ability();
    test_drop_rules();
    printf("test_psycho: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
