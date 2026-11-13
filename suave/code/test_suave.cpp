// test_suave.cpp - unit tests of the Suave logic (suave/code/suave_logic.cpp). Built and run by dll/build.bat.
// No game, no game memory: a fake game (positions, a dive that takes off when the forward-dive keys come) runs the
// whole move, every abort path and the selection / ring / camera geometry. Exit code 0 = all passed.
#include "suave_logic.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

using namespace sv;

static void test_pairs() {
    bool m = false;
    CHECK(pair_from_model("/characters/military/soldier.model", &m) == PAIR_SOLDIER && m);
    CHECK(pair_from_model("/Characters/DEA/DEA_TaskForce.model", &m) == PAIR_DEA && m);
    CHECK(pair_from_model("/characters/civil/civil.model", &m) == PAIR_CIVIL && m);
    CHECK(pair_from_model("/characters/boss/zombie.model", &m) == PAIR_SOLDIER && !m);   // clip fallback, "other" music
    CHECK(pair_from_model("", &m) == PAIR_SOLDIER && !m);
    CHECK(pair_from_model(nullptr, &m) == PAIR_SOLDIER && !m);
    CHECK(pair_by_limbs(0.479f, 0.894f) == PAIR_SOLDIER && pair_by_limbs(0.532f, 0.921f) == PAIR_CIVIL && pair_by_limbs(0.524f, 0.973f) == PAIR_DEA);
    CHECK(pair_by_limbs(0.40f, 0.80f) == PAIR_SOLDIER && pair_by_limbs(0.56f, 1.02f) == PAIR_DEA && pair_by_limbs(0.55f, 0.90f) == PAIR_CIVIL);
    CHECK(!strcmp(pair_name(PAIR_DEA), "dea") && !strcmp(pair_name(PAIR_CIVIL), "civil") && !strcmp(pair_name(PAIR_SOLDIER), "soldier"));
    Cfg c;
    CHECK(c.segment_for(PAIR_SOLDIER, true) == 1 && c.segment_for(PAIR_CIVIL, true) == 2 && c.segment_for(PAIR_DEA, true) == 3);
    CHECK(c.segment_for(PAIR_SOLDIER, false) == 4);
    c.music_dea = 0; CHECK(c.segment_for(PAIR_DEA, true) == 0);          // 0 = random (start picks)
    c.music_civil = 7; CHECK(c.segment_for(PAIR_CIVIL, true) == 0);      // out of range -> random
    c.music = false; CHECK(c.segment_for(PAIR_SOLDIER, true) == 0);
}

static void test_cfg() {
    Cfg c;
    NEAR(c.beat(), 0.48321, 2e-5);
    NEAR(c.intro_end(), 6 * 0.48321, 1e-3);
    NEAR(c.gather_start(), 4 * 0.48321, 1e-3);
    NEAR(c.release_t(), 25 * 0.48321, 1e-3);                 // intro 6 + release beat 19
    NEAR(c.dive_t(), 25 * 0.48321 + 1.4, 1e-3);
    CHECK(c.ring_radius == 3.0f && c.pull_radius == 15.0f && c.ring_max == 8);
    CHECK(c.god_seconds() > c.freeze_seconds + c.dive_t() + c.after_max);
    Cfg d; d.bpm = 0; d.ring_max = 50; d.ring_radius = -3; d.freeze_scale = 0; d.dance_cycles = 9; d.boom_real = -1;
    d.release_beat = 99; d.attach_seconds = 30; d.music_soldier = 12; d.grab_radius = NAN;
    d.clamp();
    CHECK(d.bpm >= 40 && d.ring_max == MAX_RING && d.ring_radius >= 1 && d.freeze_scale >= 0.01f && d.dance_cycles == 4);
    CHECK(d.boom_real >= d.lookdown_real + d.panic_max + d.blast_after && d.release_beat <= d.dance_beats && d.attach_seconds <= d.dive_at_seconds);
    CHECK(d.music_soldier == 9 && d.grab_radius == 0.5f);
    Cfg e; e.dance_cycles = 2; NEAR(e.release_t(), (6 + 24 + 19) * e.beat(), 1e-3);
}

static void test_choose() {
    Cfg c; Vec3 ram = v3(0, 0, 0);
    Cand k[8];
    for (int i = 0; i < 8; ++i) { k[i].hostile = k[i].alive = k[i].has_ai = true; k[i].killable = true; }
    k[0].pos = v3(4, 0, 0); k[1].pos = v3(2, 0, 1);            // 1 is the nearest within 5 m
    k[2].pos = v3(10, 0, 0); k[3].pos = v3(16, 0, 0);          // 3 beyond the pull radius
    k[4].pos = v3(1, 0, 0); k[4].hostile = false;              // a civilian next to Ram
    k[5].pos = v3(1.5f, 0, 0); k[5].killable = false;          // immune (Sub_Type 8)
    k[6].pos = v3(3, 5, 0);                                    // other floor
    k[7].pos = v3(6, 0, 0); k[7].alive = false;
    Choice ch = choose(c, ram, k, 8);
    CHECK(ch.partner == 1);
    CHECK(ch.n == 2 && ch.members[0] == 0 && ch.members[1] == 2);
    c.ring_max = 1; ch = choose(c, ram, k, 8); CHECK(ch.n == 1 && ch.members[0] == 0);
    c.grab_radius = 1.0f; ch = choose(c, ram, k, 8); CHECK(ch.partner < 0 && ch.n == 0);
    Cand many[20]; for (int i = 0; i < 20; ++i) { many[i].hostile = many[i].alive = many[i].has_ai = true; many[i].pos = v3(1.0f + i * 0.5f, 0, 0); }
    Cfg d; ch = choose(d, ram, many, 20); CHECK(ch.partner == 0 && ch.n == 8 && ch.members[7] == 8);
}

static void test_ring() {
    Cfg c; Vec3 centre = v3(10, 1, 10);
    Vec3 p[4] = {v3(10, 1, 20), v3(20, 1, 10), v3(10, 1, 0), v3(0, 1, 10)};   // north, east, south, west
    Vec3 s[4]; float y[4];
    ring_slots(c, centre, p, 4, s, y);
    for (int i = 0; i < 4; ++i) {
        NEAR(dist_xz(s[i], centre), 3.0, 1e-4);
        CHECK(dist_xz(s[i], p[i]) < 7.01f + 1e-3f);          // each member keeps its own side (no crossing)
        NEAR(s[i].y, 1.0, 1e-6);
        NEAR(yaw_to(s[i], centre), y[i], 1e-5);
    }
    for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 4; ++j) CHECK(dist_xz(s[i], s[j]) > 4.0f);   // evenly spread
    Vec3 one = v3(14, 0, 10), s1; float y1; ring_slots(c, centre, &one, 1, &s1, &y1);
    NEAR(s1.x, 13, 1e-4); NEAR(s1.z, 10, 1e-4);              // a single member stays on its own side
    Vec3 same[3] = {centre, centre, centre}; Vec3 s3[3]; float y3[3]; ring_slots(c, centre, same, 3, s3, y3);
    CHECK(dist_xz(s3[0], s3[1]) > 4.0f && dist_xz(s3[1], s3[2]) > 4.0f);
}

// ------------------------------------------------------------------------------------------------ a fake game
struct Fake {
    Snap s; Cfg c; Suave m;
    float time_mult = 1.0f; bool dive_injected = false; float dive_timer = -1; bool takeoff = true;
    int counts[64] = {}; int faces = 0; int cams = 0; int shots_seen[SH_COUNT] = {};
    float boom_air = -1, boom_clock = -1; int lookdown_clips = 0; bool booms_before_lookdown = false;
    int turn_frames = 0, turning_left = 0; Vec3 known_world, known_cam;   // the glue's turn before the dive keys
    float now = 0.0f, t_lookdown = -1.0f, t_back = -1.0f, t_boom = -1.0f, ram_speed = 1.0f, height_at_back = -1.0f;
    float lift_after = 0.1f;   // game s of the dive before Ram leaves the floor
    void init(int nmembers) {
        s = Snap(); s.ok = true; s.ram = v3(0, 0, 0); s.ram_fwd = v3(0, 0, 1); s.partner_alive = true; s.partner = v3(2.0f, 0, 1.5f);
        s.partner_body = v3(0.49f, 0, 0.59f);
        for (int j = 0; j < nmembers; ++j) { s.member_alive[j] = true; s.member[j] = v3(8.0f * sinf(j * 1.3f), 0, 8.0f * cosf(j * 1.3f)); }
    }
    void apply(const Acts& a) {
        for (int i = 0; i < a.n; ++i) {
            const Act& x = a.a[i];
            ++counts[x.kind];
            if (x.kind == A_FACE) ++faces;
            if (x.kind == A_CAM && x.f[3] >= 0) { ++cams; if (x.who > 0 && x.who < SH_COUNT) ++shots_seen[x.who];
                if (x.who == SH_LOOKDOWN) t_lookdown = now;
                if (x.who == SH_FRONT_DIVE && t_back < 0) { t_back = now; height_at_back = s.ram_height; } }
            if (x.kind == A_RAM_SPEED) ram_speed = x.f[0] > 0.0f ? x.f[0] : 1.0f;
            if (x.kind == A_PARTNER_PLACE) { s.partner = s.ram; s.partner_yaw_err = 0; }
            if (x.kind == A_FREEZE_ON) time_mult = x.f[0];
            if (x.kind == A_FREEZE_OFF) time_mult = 1.0f;
            if (x.kind == A_SLOWMO) time_mult = x.f[0];
            if (x.kind == A_AUTO_DIVE) {
                if (turn_frames > 0) { turning_left = turn_frames; s.dive_turning = true; }
                else { dive_injected = true; dive_timer = 0; s.dive_known = true; s.dive_world = s.dive_cam = v3(x.f[0], 0, x.f[2]); }
            }
            if (x.kind == A_PLAY && x.who == 0 && x.i == CLIP_LOOKDOWN) ++lookdown_clips;
            if (x.kind == A_BOOM) { t_boom = now; boom_air = s.ram_diving ? 1.0f : 0.0f; boom_clock = m.c; if (!m.lookdown_done) booms_before_lookdown = true; for (int b = 0; b <= MAX_RING; ++b) if (x.i & (1 << b)) { if (b == 0) s.partner_alive = false; else s.member_alive[b - 1] = false; } }
            if (x.kind == A_MEMBER_ARRIVE && x.i == 1) s.member[x.who - 1] = v3(x.f[0], x.f[1], x.f[2]);
        }
    }
    void walk(float dt) {   // ring members walk 1.4 m/s toward their slots once told to
        if (turning_left > 0 && --turning_left == 0) {
            s.dive_turning = false; s.dive_known = true; s.dive_world = known_world; s.dive_cam = known_cam;
            dive_injected = true; dive_timer = 0;
        }
        for (int j = 0; j < m.n; ++j) if (m.mstate[j] == M_WALK && s.member_alive[j]) {
            Vec3 d = sub(m.slot[j], s.member[j]); float l = sqrtf(d.x * d.x + d.z * d.z);
            if (l > 1e-3f) { float st = 1.4f * dt * time_mult; if (st > l) st = l; s.member[j] = add(s.member[j], mul(d, st / l)); }
        }
        now += dt;
        if (dive_timer >= 0) {   // the game's dive: takeoff 0.1 s (game) after the keys, flight 0.9 s game
            dive_timer += dt * time_mult * ram_speed;
            s.ram_diving = takeoff && dive_timer > 0.1f && dive_timer < 1.0f;
            s.ram_height = takeoff && dive_timer > lift_after && dive_timer < 1.0f ? 1.0f : 0.0f;
            if (s.ram_diving) time_mult = 0.11f; else if (dive_timer >= 1.0f && takeoff) time_mult = 1.0f;
        }
    }
    int run(float seconds, float dt = 1.0f / 60.0f) {
        int steps = (int)(seconds / dt), i = 0;
        for (; i < steps && m.running(); ++i) { s.time_mult = time_mult; walk(dt); apply(m.tick(c, s, dt)); }
        return i;
    }
};

static void test_full_move() {
    Fake f; f.init(5);
    Vec3 mp[5]; for (int j = 0; j < 5; ++j) mp[j] = f.s.member[j];
    Acts a = f.m.start(f.c, f.s, PAIR_DEA, true, mp, 5, 42);
    CHECK(f.m.phase == P_FREEZE && f.m.segment == 3);              // DEA -> segment 3
    CHECK(a.find(A_GOD_ON) && a.find(A_GOD_ON)->f[0] == f.c.god_seconds());
    CHECK(a.find(A_RAM_TAKE) && a.find(A_PARTNER_TAKE) && a.count(A_MEMBER_TAKE) == 5);
    CHECK(a.find(A_PLAY, -1) && a.find(A_PLAY, -1)->i == CLIP_INTRO && a.find(A_PLAY, 0)->i == CLIP_GRABBED);
    CHECK(a.find(A_FREEZE_ON) && a.find(A_INPUT_LOCK) && a.find(A_CAM_TAKE) && a.find(A_CAM) && a.find(A_CAM)->who == SH_PUSHIN);
    CHECK(a.count(A_FACE) == 2 && a.count(A_MUSIC_START) == 0);
    {   // intro placement: the node is 0.36 m from Ram, Ram's intro Bip (0.23, -0.28) lands back on his spot
        const Act* r = a.find(A_RAM_TAKE);
        Vec3 node = v3(r->f[0], r->f[1], r->f[2]); float y = r->f[3];
        NEAR(dist(node, f.s.ram), sqrtf(0.23f * 0.23f + 0.28f * 0.28f), 1e-4);
        Vec3 fy = v3(sinf(y), 0, cosf(y)), ry = v3(fy.z, 0, -fy.x);
        Vec3 bip = add(add(node, mul(ry, 0.23f)), mul(fy, -0.28f));
        NEAR(dist(bip, f.s.ram), 0.0, 1e-4);
        NEAR(f.m.centre.x, node.x, 1e-6); NEAR(f.m.centre.z, node.z, 1e-6);
        // the partner's intro start (1.08, 0.58) - Ram's (0.23, -0.28) points at where the partner stands
        Vec3 pstart = add(add(node, mul(ry, 1.08f)), mul(fy, 0.58f));
        NEAR(yaw_to(f.s.ram, pstart), yaw_to(f.s.ram, f.s.partner), 2e-3);
        CHECK(f.c.ring_phase_beats == 1.0f);
    }
    f.apply(a);
    // freeze -> intro: music on the downbeat at the freeze end
    f.run(f.c.freeze_seconds - 0.02f);
    CHECK(f.m.phase == P_FREEZE && f.counts[A_MUSIC_START] == 0);
    f.run(0.05f);
    CHECK(f.m.phase == P_INTRO && f.counts[A_MUSIC_START] == 1 && f.counts[A_FREEZE_OFF] == 1);
    // intro: OTS and two-shot cuts, partner faces, ring starts walking 2 beats before the end
    f.run(4.2f * f.c.beat());
    CHECK(f.shots_seen[SH_OTS_PARTNER] == 1 && f.shots_seen[SH_TWO] == 1);
    CHECK(f.counts[A_MEMBER_WALK] == 5 && f.m.ring_started);
    f.run(2.0f * f.c.beat());
    CHECK(f.m.phase == P_GATHER && f.m.dance_started && f.shots_seen[SH_ORBIT] == 1);
    // gather: everybody arrives or is placed by the deadline, then claps with a phase offset
    f.run(f.c.gather_end() - f.m.c + 0.05f);
    int clap = 0; for (int j = 0; j < 5; ++j) clap += f.m.mstate[j] == M_CLAP;
    CHECK(clap == 5 && f.counts[A_MEMBER_ARRIVE] == 5);
    // dance: bar-line cuts until the release; Ram shows the dynamite (in his hand) dynamite_show s before it
    f.run(f.c.release_t() - f.c.dynamite_show - f.m.c - 0.05f);
    CHECK(f.counts[A_DYNAMITE_ARM] == 0);
    f.run(0.1f);
    CHECK(f.counts[A_DYNAMITE_ARM] == 1 && f.m.phase == P_DANCE);
    f.run(f.c.release_t() - f.m.c - 0.02f);
    CHECK(f.m.phase == P_DANCE && f.shots_seen[SH_HERO_LOW] >= 1 && f.shots_seen[SH_OTS_RAM] >= 1);
    CHECK(f.shots_seen[SH_MEMBER_POV] >= 1 || f.shots_seen[SH_WIDE] >= 1);
    f.run(0.05f);
    CHECK(f.m.phase == P_PLANT && f.counts[A_DYNAMITE_ARM] == 1 && f.shots_seen[SH_PLANT_CLOSE] == 1 && f.counts[A_DYNAMITE_ATTACH] == 0);
    f.run(f.c.attach_seconds + 0.02f);
    CHECK(f.counts[A_DYNAMITE_ATTACH] == 1);
    for (int i = 0; i < 2000 && !f.shots_seen[SH_FRONT_LOW]; ++i) f.run(1.0f / 60.0f);   // the plant runs in slow motion
    CHECK(f.shots_seen[SH_FRONT_LOW] == 1 && f.counts[A_SLOWMO] == 2);   // the plant's, then the ending's
    CHECK(f.lookdown_clips == 0);                                  // not before the stumble's last frame
    // the dive: released, injected, takeoff, lookdown, back to the front, BOOM in the air
    f.run(12.0f);
    CHECK(f.counts[A_RAM_RELEASE] == 1 && f.dive_injected);
    NEAR(f.m.dive_dir.x, 0.0, 1e-5); NEAR(f.m.dive_dir.z, -1.0, 1e-5);   // behind Ram = away from the partner
    CHECK(f.lookdown_clips == 1 && f.shots_seen[SH_LOOKDOWN] == 1 && f.shots_seen[SH_FRONT_DIVE] == 1);
    CHECK(f.counts[A_BOOM] == 1 && f.boom_air == 1 && !f.booms_before_lookdown);
    CHECK(f.t_back - f.t_lookdown >= f.c.panic_min - 0.02f && f.t_back - f.t_lookdown <= f.c.panic_max + 0.02f);   // the panic shot reads
    CHECK(f.height_at_back >= f.c.panic_lift && f.t_boom - f.t_back >= f.c.blast_after - 0.02f);
    CHECK(f.counts[A_RAM_SPEED] == 2 && f.ram_speed == 1.0f);   // slowed in the panic cut, normal again at the cut back
    CHECK(!f.m.running() && f.counts[A_END] == 1 && f.counts[A_CAM_RELEASE] == 1);
    CHECK(f.counts[A_GOD_CUT] == 1 && f.counts[A_HUD_END] == 1 && f.counts[A_MUSIC_STOP] == 0);   // the music plays out
    CHECK(f.counts[A_MEMBER_RELEASE] == 0);                        // everybody died in the blast
    CHECK(f.faces >= 12 && f.m.uses == 1 && f.m.aborts == 0);
    CHECK(f.counts[A_INPUT_LOCK] == 2);                            // lock at the start, unlock at the takeoff
    CHECK(f.counts[A_PARTNER_FREEZE] == 1);
}

static void test_player_dive_and_no_takeoff() {
    // the player dives himself: the camera is handed back at the dive window, the boom still waits for the takeoff
    Fake f; f.init(2); f.c.auto_dive = false;
    Vec3 mp[2] = {f.s.member[0], f.s.member[1]};
    f.apply(f.m.start(f.c, f.s, PAIR_SOLDIER, true, mp, 2, 1));
    for (int i = 0; i < 4000 && f.m.phase != P_DIVE; ++i) f.run(1.0f / 60.0f);
    CHECK(f.m.phase == P_DIVE && f.counts[A_CAM_RELEASE] == 1 && !f.dive_injected);
    CHECK(f.counts[A_INPUT_LOCK] == 2);                            // keys back for the player's dive
    f.dive_timer = 0;                                              // the player presses dive now
    f.run(12.0f);
    CHECK(f.counts[A_BOOM] == 1 && f.boom_air == 1 && !f.m.running());
    CHECK(f.shots_seen[SH_LOOKDOWN] == 0 && f.lookdown_clips == 1); // no cut, but the partner still looks down
    // no takeoff at all: the explosion comes after dive_wait anyway
    Fake g; g.init(1); g.takeoff = false; g.c.takeoff_fallback = 10.0f;   // the dive_wait path
    Vec3 m1 = g.s.member[0];
    g.apply(g.m.start(g.c, g.s, PAIR_CIVIL, true, &m1, 1, 2));
    CHECK(g.m.segment == 2);
    for (int i = 0; i < 4000 && g.m.phase != P_DIVE; ++i) g.run(1.0f / 60.0f);
    CHECK(g.m.phase == P_DIVE && g.dive_injected);
    g.run(g.c.dive_wait / g.c.ending_scale * 0.5f);
    CHECK(g.counts[A_BOOM] == 0);
    g.run(12.0f);
    CHECK(g.counts[A_BOOM] == 1 && g.boom_air == 0 && !g.m.running() && g.counts[A_INPUT_LOCK] == 2);
    // detection fails but the fallback runs the whole in-air sequence: look-down cut, back to the front, boom
    Fake h; h.init(1); h.takeoff = false;
    Vec3 m2 = h.s.member[0];
    h.apply(h.m.start(h.c, h.s, PAIR_SOLDIER, true, &m2, 1, 3));
    h.run(30.0f);
    CHECK(h.shots_seen[SH_LOOKDOWN] == 1 && h.shots_seen[SH_FRONT_DIVE] == 1 && h.counts[A_BOOM] == 1 && h.lookdown_clips == 1 && !h.m.running());
    CHECK(h.counts[A_SLOWMO] >= 3);                              // plant, ending, takeoff (+ the end's back to normal)
}

static void test_aborts() {
    struct Case { const char* name; int when_phase; int what; bool hard; };
    // what: 0 partner dies, 1 cut-scene, 2 vehicle, 3 level change, 4 rewind, 5 not ok
    const Case cases[] = {{"partner dies in the dance", P_DANCE, 0, false}, {"cut-scene in the intro", P_INTRO, 1, false},
                          {"vehicle in the freeze", P_FREEZE, 2, false}, {"level change in the gather", P_GATHER, 3, true},
                          {"rewind in the plant", P_PLANT, 4, true}, {"lost control in the dive", P_DIVE, 5, false}};
    for (const Case& k : cases) {
        Fake f; f.init(3);
        Vec3 mp[3] = {f.s.member[0], f.s.member[1], f.s.member[2]};
        f.apply(f.m.start(f.c, f.s, PAIR_SOLDIER, true, mp, 3, 7));
        int guard = 0;
        while (f.m.phase != k.when_phase && f.m.running() && guard++ < 6000) f.run(1.0f / 60.0f);
        CHECK(f.m.phase == k.when_phase);
        if (k.what == 0) f.s.partner_alive = false;
        if (k.what == 1) f.s.cutscene = true;
        if (k.what == 2) f.s.vehicle = true;
        if (k.what == 3) f.s.level_changed = true;
        if (k.what == 4) f.s.rewind = true;
        if (k.what == 5) f.s.ok = false;
        int before_release = f.counts[A_MEMBER_RELEASE];
        f.run(0.05f);
        bool died_after_plant = k.what == 0 && k.when_phase >= P_PLANT;
        CHECK(!f.m.running() || died_after_plant);
        if (died_after_plant) continue;
        CHECK(f.counts[A_END] == 1 && f.m.aborts == 1);
        CHECK(f.counts[A_HUD_END] == 1 && f.counts[A_GOD_CUT] == 1);
        CHECK(f.counts[A_INPUT_LOCK] >= 2 || k.when_phase >= P_DIVE);
        if (f.m.cam_taken) CHECK(false);
        CHECK(f.counts[A_CAM_RELEASE] >= 1);
        if (k.when_phase == P_FREEZE) CHECK(f.counts[A_FREEZE_OFF] == 1);
        if (k.hard) CHECK(f.counts[A_MEMBER_RELEASE] == before_release && f.counts[A_PARTNER_RELEASE] == 0);
        else CHECK(f.counts[A_MEMBER_RELEASE] - before_release == 3 && (f.counts[A_PARTNER_RELEASE] == 1 || k.what == 0));
        if (k.when_phase >= P_PLANT && !k.hard) CHECK(f.counts[A_DYNAMITE_DROP] == 1);
        if (k.when_phase < P_DIVE && !k.hard) CHECK(f.counts[A_RAM_RELEASE] == 1);
        CHECK(f.counts[A_MUSIC_STOP] == 1);
    }
    // pause holds the clock; a partner death after the plant does not abort
    Fake p; p.init(0);
    p.apply(p.m.start(p.c, p.s, PAIR_SOLDIER, true, nullptr, 0, 3));
    p.run(p.c.freeze_seconds + 1.0f);
    float c0 = p.m.c; p.s.paused = true; p.run(2.0f); CHECK(p.m.c == c0 && p.m.running());
    p.s.paused = false; p.run(p.c.release_t() - p.m.c + 0.1f); CHECK(p.m.phase == P_PLANT);
    p.s.partner_alive = false; p.run(0.2f); CHECK(p.m.running());
}

static void test_segments_and_camera() {
    Suave s; int last = 0, rep = 0, hits[5] = {};
    for (int i = 0; i < 400; ++i) { int r = s.pick_segment(4); CHECK(r >= 1 && r <= 4); if (r == last) ++rep; last = r; ++hits[r]; }
    CHECK(rep == 0 && hits[1] > 40 && hits[4] > 40 && s.pick_segment(0) == 0 && s.pick_segment(1) == 1);
    // the music falls back to random when the mapping is 0
    Fake f; f.init(0); f.c.music_soldier = 0;
    f.apply(f.m.start(f.c, f.s, PAIR_SOLDIER, true, nullptr, 0, 9));
    CHECK(f.m.segment >= 1 && f.m.segment <= 4);
    // camera geometry: push-in in front of Ram's face, moving closer; front-low in the dive direction
    Suave m; Cfg c; Snap sn; sn.ram = v3(5, 0, 5); sn.ram_fwd = v3(1, 0, 0); sn.partner_body = v3(5.6f, 0, 4.5f);
    Vec3 e; int t; float fov;
    m.shot_t = 0; CHECK(m.shot_eye(c, SH_PUSHIN, sn, &e, &t, &fov)); NEAR(e.x, 8.2, 1e-4); NEAR(e.y, 1.55, 1e-4); CHECK(t == TGT_RAM_HEAD);
    m.shot_t = 10; m.shot_eye(c, SH_PUSHIN, sn, &e, &t, &fov); NEAR(e.x, 6.7, 1e-4);
    CHECK(m.shot_eye(c, SH_OTS_PARTNER, sn, &e, &t, &fov) && e.x < 5.0f && t == TGT_PARTNER_HEAD);
    m.centre = sn.ram; m.shot_t = 0; m.orbit_a0 = 0; m.shot_eye(c, SH_ORBIT, sn, &e, &t, &fov); NEAR(dist_xz(e, m.centre), 7.2, 1e-3); NEAR(e.y, 5.5, 1e-4);
    m.n = 0; m.pov_member = -1; CHECK(!m.shot_eye(c, SH_MEMBER_POV, sn, &e, &t, &fov));
    m.dive_dir = v3(0, 0, 1); m.shot_eye(c, SH_FRONT_DIVE, sn, &e, &t, &fov); NEAR(e.z, 5.0 + c.dive_cam_distance, 1e-4); NEAR(e.y, c.front_height, 1e-4);
    CHECK(c.front_distance >= 4.0f && c.front_distance <= 6.0f && c.front_height >= 1.0f && c.front_height <= 1.5f);
    CHECK(c.camera && c.faces && c.auto_dive && c.lock_input);
    // camera off: no shot actions at all, the move still runs
    Fake n; n.init(1); n.c.camera = false; Vec3 m1 = n.s.member[0];
    n.apply(n.m.start(n.c, n.s, PAIR_SOLDIER, true, &m1, 1, 5));
    n.run(30.0f);
    CHECK(n.counts[A_CAM] == 0 && n.counts[A_CAM_TAKE] == 0 && n.counts[A_CAM_RELEASE] == 0 && n.counts[A_BOOM] == 1);
    // faces off: no face actions
    Fake q; q.init(1); q.c.faces = false; q.apply(q.m.start(q.c, q.s, PAIR_SOLDIER, true, &m1, 1, 5)); q.run(30.0f);
    CHECK(q.faces == 0 && q.counts[A_BOOM] == 1);
}

static void test_dive_turn() {
    // the key for the wanted direction, relative to Ram's measured facing
    Vec3 w, e; const Vec3 fz = v3(0, 0, 1);
    CHECK(dive_key_rel(fz, v3(0.1f, 0, 1), 1.0f, 45.0f, &w, &e) == DIVE_W); NEAR(w.z, 1.0, 1e-5); NEAR(e.z, 1.0, 1e-5);
    CHECK(dive_key_rel(fz, v3(0.2f, 0, -1), 1.0f, 45.0f, &w, &e) == DIVE_S); NEAR(w.z, -1.0, 1e-5); NEAR(e.x, 1.0, 1e-5);   // profile
    CHECK(dive_key_rel(fz, v3(1, 0, 0.1f), 1.0f, 45.0f, &w, &e) == DIVE_D); NEAR(w.x, 1.0, 1e-5); NEAR(e.z, 1.0, 1e-5);     // right_of(+z) = +x
    CHECK(dive_key_rel(fz, v3(-1, 0, 0), 1.0f, 45.0f, &w, &e) == DIVE_A); NEAR(w.x, -1.0, 1e-5);
    CHECK(dive_key_rel(fz, v3(1, 0, 0), -1.0f, 45.0f, &w, &e) == DIVE_A); NEAR(w.x, 1.0, 1e-5);                          // D mirrored
    NEAR(signed_yaw(v3(0, 0, 1), v3(1, 0, 0)), 3.14159265 / 2, 1e-5); NEAR(fabs(signed_yaw(v3(0, 0, 1), v3(0, 0, -1))), 3.14159265, 1e-5);
    // the turn controller against a fake physics: the request (set, not added) is consumed one frame later, either all
    // at once or spread over three steps (test 5), with either yaw sign
    for (int spread = 0; spread < 2; ++spread) for (int sgn = -1; sgn <= 1; sgn += 2) for (int start = 0; start < 4; ++start) {
        TurnCtl t; float yaw = start == 0 ? 3.0f : (start == 1 ? -2.0f : (start == 2 ? 0.9f : 0.53f)), pend = 0.0f;
        t.begin(v3(0, 0, 1), 0.26f, 1.0f);
        int r = 0, frames = 0;
        for (; frames < 200; ++frames) {
            float take = spread ? pend * 0.5f : pend; if (fabsf(pend) < 0.02f) take = pend;
            yaw += take; pend -= take;
            float d = 0.0f; r = t.step(v3(sinf(yaw), 0, cosf(yaw)), 1.0f / 60.0f, &d);
            if (r) break;
            if (d != 0.0f) pend = sgn * d * 3.14159265f / 180.0f;   // the accumulator is set
        }
        CHECK(r == 1 && frames < 40);
        NEAR(fabs(atan2f(sinf(yaw), cosf(yaw))), 0.0, 0.26);
    }
    {   // the physics never turns him: out of time (the glue then picks the key by his actual facing)
        TurnCtl t; t.begin(v3(0, 0, 1), 0.26f, 0.6f);
        int r = 0; float d; for (int i = 0; i < 100 && !(r = t.step(v3(0, 0, -1), 1.0f / 60.0f, &d)); ++i) {}
        CHECK(r == 2 && t.kicks >= 2 && !t.on);
    }
    // the logic: the dive clock waits for the turn, then the shots follow the dive the glue chose
    Fake f; f.init(1); f.turn_frames = 90; f.known_world = v3(1, 0, 0); f.known_cam = v3(0, 0, 1);
    Vec3 m1 = f.s.member[0];
    f.apply(f.m.start(f.c, f.s, PAIR_SOLDIER, true, &m1, 1, 11));
    for (int i = 0; i < 4000 && f.m.phase != P_DIVE; ++i) f.run(1.0f / 60.0f);
    CHECK(f.m.phase == P_DIVE && f.s.dive_turning && f.shots_seen[SH_FRONT_LOW] == 0);   // the front camera waits for the dive
    f.run(1.0f);   // longer than takeoff_fallback: nothing may start while he turns
    CHECK(f.s.dive_turning && f.m.dive_real == 0.0f && f.m.air_real < 0.0f && f.counts[A_BOOM] == 0);
    f.run(0.6f);
    CHECK(!f.s.dive_turning && f.m.dive_final);
    NEAR(f.m.dive_dir.x, 1.0, 1e-5); NEAR(f.m.cam_dir.z, 1.0, 1e-5);
    CHECK(f.shots_seen[SH_FRONT_LOW] == 1);   // cut once the dive is final, on Ram's actual side
    NEAR(f.m.front_eye.z - f.s.ram.z, f.c.front_distance, 1e-4); NEAR(f.m.front_eye.y - f.s.ram.y, f.c.front_height, 1e-4);
    f.run(12.0f);
    CHECK(f.counts[A_BOOM] == 1 && f.boom_air == 1 && !f.m.running());
    Suave m; Cfg c; Snap sn; sn.ram = v3(0, 0, 0); m.dive_dir = v3(1, 0, 0); m.cam_dir = v3(0, 0, 1);
    int tg; float fov; m.shot_eye(c, SH_FRONT_DIVE, sn, &e, &tg, &fov); NEAR(e.z, c.dive_cam_distance, 1e-4); NEAR(e.x, 0.0, 1e-4);
    // the chest close-up frames the partner's chest, clear of Ram
    Snap ps; ps.ram = v3(0, 0, 0); ps.ram_fwd = v3(0, 0, 1); ps.partner_body = v3(0, 1.0f, 0.6f); ps.partner_chest = v3(0, 1.35f, 0.6f);
    m.shot_eye(c, SH_PLANT_CLOSE, ps, &e, &tg, &fov);
    CHECK(tg == TGT_PARTNER_CHEST); NEAR(e.y, 1.50, 1e-3); CHECK(dist(e, ps.partner_chest) > 1.5f && dist_xz(e, ps.ram) > 0.8f);
    // a tight space: the ring pulls in, at most four members on a small ring
    Fake g; g.init(6); g.s.free_min = 2.5f; Vec3 mp[6]; for (int j = 0; j < 6; ++j) mp[j] = g.s.member[j];
    g.apply(g.m.start(g.c, g.s, PAIR_SOLDIER, true, mp, 6, 4));
    NEAR(g.m.ring_r, 1.8, 1e-4); CHECK(g.m.n == 4);
    NEAR(dist_xz(g.m.slot[0], g.m.centre), 1.8, 1e-3);
    Fake o; o.init(3); o.apply(o.m.start(o.c, o.s, PAIR_SOLDIER, true, mp, 3, 4)); NEAR(o.m.ring_r, 3.0, 1e-6); CHECK(o.m.n == 3);
}

int main() {
    test_pairs();
    test_cfg();
    test_choose();
    test_ring();
    test_full_move();
    test_player_dive_and_no_takeoff();
    test_aborts();
    test_segments_and_camera();
    test_dive_turn();
    printf("test_suave: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
