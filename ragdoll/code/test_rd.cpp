// test_rd.cpp - unit tests of the [ragdoll] feature: rd_logic on REAL skeletons from the game (rd_fixture.inc: four
// characters of a renderer-bridge bone probe with the game's own world matrices) and rd_sim (Box3D ragdolls in the
// phys_sim world through the same function tables TODPhysics.dll exports), as 32-bit. Built and run by dll/build.bat.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <windows.h>
#include <initializer_list>
#include <stdlib.h>
#include "rd_logic.h"
#include "../../physics/code/phys_api.h"
extern "C" const PhApi* __cdecl TODPhysics_GetApi(int version);
extern "C" const RdApi* __cdecl TODPhysics_GetRagdollApi(int version);

using namespace rd;
using ph::v3; using ph::add; using ph::sub; using ph::mul; using ph::len; using ph::qrot; using ph::qmul; using ph::qconj;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool nearf(float a, float b, float e) { return fabsf(a - b) <= e; }
static bool nearv(V3 a, V3 b, float e) { return nearf(a.x, b.x, e) && nearf(a.y, b.y, e) && nearf(a.z, b.z, e); }
static float qdiff(Q4 a, Q4 b) { return qangle(qmul(qconj(a), b)); }
static bool finite3(V3 v) { return v.x == v.x && v.y == v.y && v.z == v.z && fabsf(v.x) < 1e6f && fabsf(v.y) < 1e6f && fabsf(v.z) < 1e6f; }

struct FxBone { int idx, parent; const char* name; float lp[3], sq[4], world[16]; };
struct FxChar { int handle; const FxBone* bones; int n; const float* root; };
#include "rd_fixture.inc"
static const int NFX = (int)(sizeof FX_CHARS / sizeof FX_CHARS[0]);

static void load(const FxChar& c, Skeleton* s, Xf* root) {
    memset(s, 0, sizeof *s);
    s->n = c.n;
    for (int i = 0; i < c.n; ++i) {
        const FxBone& b = c.bones[i];
        s->parent[i] = b.parent;
        strncpy(s->name[i], b.name, 23);
        s->local[i].p = v3(b.lp[0], b.lp[1], b.lp[2]);
        Q4 st = {b.sq[0], b.sq[1], b.sq[2], b.sq[3]};
        s->local[i].q = ph::true_from_stored(st);
    }
    bool ok; *root = xf_from_tod(c.root, &ok);
}

static void test_math() {
    Q4 a = qaxis(v3(0.2f, 1, 0.4f), 0.9f);
    Xf x = {v3(1, 2, 3), a};
    Xf y = xmul(x, xinv(x));
    CHECK(nearv(y.p, v3(0, 0, 0), 1e-5f)); CHECK(qangle(y.q) < 1e-3f);
    CHECK(nearv(qrot(qarc(v3(0, 0, 1), v3(1, 0, 0)), v3(0, 0, 1)), v3(1, 0, 0), 1e-5f));
    CHECK(nearv(qrot(qarc(v3(0, 0, 1), v3(0, 0, -1)), v3(0, 0, 1)), v3(0, 0, -1), 1e-5f));
    CHECK(nearf(hinge_angle(qaxis(v3(0, 0, 1), 1.2f)), 1.2f, 1e-4f));
    CHECK(nearf(hinge_angle(qaxis(v3(0, 0, 1), -0.7f)), -0.7f, 1e-4f));
    CHECK(nearv(qlog(qaxis(v3(0, 1, 0), 0.5f)), v3(0, 0.5f, 0), 1e-4f));
    // R_x(-90) takes z to +y (the hinge frame)
    CHECK(nearv(qrot(qaxis(v3(1, 0, 0), -1.5707963f), v3(0, 0, 1)), v3(0, 1, 0), 1e-5f));
    V3 v, w;
    Xf p0 = {v3(0, 0, 0), qid()}, p1 = {v3(0.1f, 0, 0), qaxis(v3(0, 1, 0), 0.05f)};
    velocity(p0, p1, 0.02f, 100.0f, &v, &w);
    CHECK(nearv(v, v3(5, 0, 0), 1e-3f)); CHECK(nearv(w, v3(0, 2.5f, 0), 1e-2f));
    velocity(p0, p1, 0.0001f, 3.0f, &v, &w);   // too small dt: zero
    CHECK(len(v) == 0.0f);
    // ray vs capsule
    float t; CHECK(ray_capsule(v3(-1, 0.05f, 0), v3(1, 0.05f, 0), v3(0, -1, 0), v3(0, 1, 0), 0.1f, &t) < 0 && nearf(t, 0.5f, 0.05f));
    CHECK(ray_capsule(v3(-1, 0, 0.5f), v3(1, 0, 0.5f), v3(0, -1, 0), v3(0, 1, 0), 0.1f, &t) > 0);
}

static void test_strength() {
    Strength s; s.reset(1.0f);
    s.hit(CHEST, 0.1f, 0.5f);
    CHECK(nearf(s.s[CHEST], 0.1f, 1e-6f)); CHECK(nearf(s.s[SPINE], 0.5f, 1e-6f)); CHECK(nearf(s.s[HEAD], 0.5f, 1e-6f));
    CHECK(nearf(s.s[L_UPPER], 0.5f, 1e-6f)); CHECK(nearf(s.s[PELVIS], 1.0f, 1e-6f)); CHECK(nearf(s.s[L_THIGH], 1.0f, 1e-6f));
    CHECK(nearf(s.min_value(), 0.1f, 1e-6f));
    s.recover(0.35f, 0.7f);   // half way
    CHECK(nearf(s.s[CHEST], 0.6f, 1e-5f)); CHECK(nearf(s.s[SPINE], 1.0f, 1e-5f));
    CHECK(s.joint(CHEST) < s.joint(L_THIGH));
    s.recover(10.0f, 0.7f); CHECK(nearf(s.min_value(), 1.0f, 1e-6f));
    Settle st; CHECK(!st.update(0.1f, 0.1f, 0.2f, 0.2f, 0.5f, 0.5f)); CHECK(!st.update(0.1f, 0.1f, 0.2f, 0.2f, 0.5f, 0.5f));
    CHECK(st.update(0.1f, 0.1f, 0.2f, 0.2f, 0.5f, 0.5f)); CHECK(!st.update(3.0f, 0.1f, 0.2f, 0.2f, 0.5f, 0.5f));
    Cfg c; c.max_ragdolls = 99; c.joint_stiffness = -3; c.substeps = 0; c.clamp();
    CHECK(c.max_ragdolls == 12); CHECK(c.joint_stiffness >= 0.5f); CHECK(c.substeps == 1);
}

static void test_skeletons() {
    for (int ci = 0; ci < NFX; ++ci) {
        Skeleton s; Xf root; load(FX_CHARS[ci], &s, &root);
        // forward kinematics of the true locals reproduces the game's own world matrices (bridge read rule)
        Xf W[MAXB]; fk(s, s.local, root, W);
        float worst_p = 0, worst_q = 0;
        for (int i = 0; i < s.n; ++i) {
            bool ok; Xf g = xf_from_tod(FX_CHARS[ci].bones[i].world, &ok);
            if (!ok) continue;
            float dp = len(sub(g.p, W[i].p)), dq = qdiff(g.q, W[i].q);
            if (dp > worst_p) worst_p = dp; if (dq > worst_q) worst_q = dq;
        }
        CHECK(worst_p < 0.002f); CHECK(worst_q < 0.003f);
        if (ci == 0) printf("  fk vs game world: worst %.4f mm / %.4f deg (character %d)\n", worst_p * 1000, worst_q * 57.3f, FX_CHARS[ci].handle);
        Map m; CHECK(build_map(s, &m));
        if (!m.ok) { printf("  map: %s\n", m.err); continue; }
        CHECK(m.driver[find_bone(s, "L ForeTwist")] == L_FORE); CHECK(m.driver[find_bone(s, "R ForeTwist1")] == R_FORE);
        CHECK(m.driver[find_bone(s, "L Finger21")] == L_FORE); CHECK(m.driver[find_bone(s, "EyeL")] == HEAD);
        CHECK(m.driver[find_bone(s, "R Toe0")] == R_CALF); CHECK(m.driver[find_bone(s, "Neck")] == CHEST);
        CHECK(m.driver[find_bone(s, "Spine1")] == SPINE); CHECK(m.driver[find_bone(s, "Bip")] == PELVIS);
        CHECK(m.driver[find_bone(s, "GamePivot")] == -1); CHECK(m.driver[find_bone(s, "Dummy")] == -1);
        CHECK(m.norder == s.n);
        Layout L; CHECK(build_layout(s, m, W, nullptr, 1.0f, &L));
        float mass = 0; for (int b = 0; b < NBODY; ++b) mass += L.mass[b];
        CHECK(nearf(mass, 80.0f, 0.01f));
        // limb lengths from the rig: upper arm 0.285, forearm 0.239 + hand, thigh / calf 0.486
        float ua = len(sub(L.c2[L_UPPER][0], L.c1[L_UPPER][0])) + 0.3f * L.rad[L_UPPER][0] + L.rad[L_UPPER][0];
        float th = len(sub(L.c2[L_THIGH][0], L.c1[L_THIGH][0])) + 0.3f * L.rad[L_THIGH][0] + L.rad[L_THIGH][0];
        if (ci < 2) printf("  character %d: upper arm %.3f thigh %.3f\n", FX_CHARS[ci].handle, ua, th);
        CHECK(ua > 0.2f && ua < 0.35f); CHECK(th > 0.4f && th < 0.6f);   // rigs differ per model (0.259 .. 0.285 / 0.486 .. 0.503)
        CHECK(L.ncap[CHEST] == 2); CHECK(L.ncap[PELVIS] == 1);
        // the head capsule points up out of the neck (eyes are on the -X side of the head bone)
        V3 head_up = sub(xpoint(L.body0[HEAD], L.c2[HEAD][0]), L.body0[HEAD].p);
        V3 eye = sub(W[find_bone(s, "EyeL")].p, W[find_bone(s, "Head")].p);
        CHECK(ph::dot(ph::norm(head_up), ph::norm(eye)) > 0.3f);
        if (ci == 1) printf("  head: eye offset in head space (%.3f %.3f %.3f)\n", qrot(qconj(W[find_bone(s, "Head")].q), eye).x,
                            qrot(qconj(W[find_bone(s, "Head")].q), eye).y, qrot(qconj(W[find_bone(s, "Head")].q), eye).z);
        // every rig bends knees / elbows the + way (hinge_sign +1), and the creation bends are inside the limits
        for (int b : {L_FORE, R_FORE, L_CALF, R_CALF}) {
            CHECK(L.hinge_sign[b] == 1.0f);
            float ang; joint_target(L, b, L.body0, &ang);
            CHECK(ang > L.lo[b] - 0.15f && ang < L.hi[b] + 0.6f);
            if (ci == 1) printf("  %s bend at creation %.1f deg\n", body_name(b), ang * 57.3f);
        }
        // spherical joints centred on the creation pose without a rest pose: target = identity there
        for (int b : {SPINE, CHEST, HEAD, L_UPPER, R_UPPER, L_THIGH, R_THIGH}) CHECK(qangle(joint_target(L, b, L.body0, nullptr)) < 1e-3f);
        // the joint anchors coincide in the creation pose
        for (int b = 1; b < NBODY; ++b) {
            V3 pa = xpoint(L.body0[BODIES[b].parent], L.frameA[b].p), pb = xpoint(L.body0[b], L.frameB[b].p);
            CHECK(len(sub(pa, pb)) < 1e-3f);
        }
        // round trip: bodies at the creation pose give back the original locals of every driven bone
        Xf out[MAXB]; memcpy(out, s.local, sizeof(Xf) * s.n);
        Xf bodies[NBODY]; body_frames(m, W, bodies);
        Xf garbage[MAXB]; for (int i = 0; i < s.n; ++i) { garbage[i].p = v3(9, 9, 9); garbage[i].q = qaxis(v3(1, 0, 0), 2.0f); }
        memcpy(out, garbage, sizeof(Xf) * s.n);
        pose_from_bodies(s, m, L, bodies, root, out);
        float wq = 0, wp = 0; int written = 0, untouched = 0;
        for (int i = 0; i < s.n; ++i) {
            if (m.driver[i] < 0) { if (nearv(out[i].p, v3(9, 9, 9), 0)) ++untouched; continue; }
            ++written;
            float dq = qdiff(out[i].q, s.local[i].q), dp = len(sub(out[i].p, s.local[i].p));
            if (dq > wq) wq = dq; if (dp > wp) wp = dp;
        }
        CHECK(written == s.n - 2); CHECK(untouched == 2); CHECK(wq < 2e-3f); CHECK(wp < 1e-3f);
        // a rotated pelvis body moves the whole body rigidly: every bone world follows
        Xf turn = {v3(0.5f, 0.2f, -0.3f), qaxis(v3(0, 1, 0), 0.8f)};
        Xf moved[NBODY]; for (int b = 0; b < NBODY; ++b) moved[b] = xmul(turn, bodies[b]);
        pose_from_bodies(s, m, L, moved, root, out);
        Xf W2[MAXB]; Skeleton s2 = s; for (int i = 0; i < s.n; ++i) if (m.driver[i] >= 0) s2.local[i] = out[i];
        fk(s2, s2.local, root, W2);
        float worst = 0;
        for (int i = 0; i < s.n; ++i) if (m.driver[i] >= 0) { float d = len(sub(W2[i].p, xpoint(turn, W[i].p))); if (d > worst) worst = d; }
        CHECK(worst < 2e-3f);
        int nb = nearest_body(L, bodies, W[find_bone(s, "Head")].p, 1.0f, nullptr);
        CHECK(nb == HEAD || nb == CHEST);
        CHECK(nearest_body(L, bodies, xpoint(L.body0[L_CALF], v3(-0.25f, 0, 0)), 1.0f, nullptr) == L_CALF);
    }
}

// ---- Box3D: ragdolls in the phys_sim world on a flat floor
static const PhApi* PA; static const RdApi* RA;
static V3 g_fc;   // floor centre (the fixture characters stand around (-480, -139, -2216))
static void floor_world(float y) {
    PA->create(-9.81f, 0.0f);
    float box[8][3];
    for (int i = 0; i < 8; ++i) { box[i][0] = g_fc.x + ((i & 1) ? 80.0f : -80.0f); box[i][1] = (i & 2) ? y : y - 1.0f; box[i][2] = g_fc.z + ((i & 4) ? 80.0f : -80.0f); }
    PA->add_static_hull(&box[0][0], 8, 0.8f);
}
static float body_y(int r, int b) { float p[3], q[4]; RA->get(r, b, p, q); return p[1]; }
static void read_bodies(int r, Xf* out) {
    for (int b = 0; b < NBODY; ++b) { float p[3], q[4]; RA->get(r, b, p, q); out[b].p = v3(p[0], p[1], p[2]); Q4 qq = {q[0], q[1], q[2], q[3]}; out[b].q = qq; }
}
static float max_cone_violation(const Layout& L, const Xf* bodies) {
    float worst = 0;
    for (int b = 1; b < NBODY; ++b) {
        if (L.kind[b] != J_SPHERE) continue;
        Q4 fa = qmul(bodies[BODIES[b].parent].q, L.frameA[b].q), fb = qmul(bodies[b].q, L.frameB[b].q);
        float c = ph::dot(qrot(fa, v3(0, 0, 1)), qrot(fb, v3(0, 0, 1))); if (c > 1) c = 1; if (c < -1) c = -1;
        float v = acosf(c) - L.cone[b]; if (v > worst) worst = v;
    }
    return worst;
}
static void test_sim() {
    PA = TODPhysics_GetApi(TODPHYS_API_VERSION); RA = TODPhysics_GetRagdollApi(TODRAG_API_VERSION);
    CHECK(PA != nullptr); CHECK(RA != nullptr); CHECK(TODPhysics_GetRagdollApi(77) == nullptr);
    if (!PA || !RA) return;
    CHECK(RA->create() < 0);   // no world yet (or a destroyed one)
    Skeleton s; Xf root; load(FX_CHARS[1], &s, &root);   // a standing character
    Map m; build_map(s, &m);
    Xf W[MAXB]; fk(s, s.local, root, W);
    float ground = W[find_bone(s, "L Toe0")].p.y - 0.03f;
    g_fc = W[find_bone(s, "Pelvis")].p;
    // 1. limp: lifted 1.5 m, dropped -> lands on the floor, joints inside their limits, settles
    {
        floor_world(ground);
        Xf lift = {v3(0, 1.5f, 0), qid()};
        Xf W1[MAXB]; Xf r1 = xmul(lift, root); fk(s, s.local, r1, W1);
        Layout L; build_layout(s, m, W1, nullptr, 1.0f, &L);
        Cfg c; Spawned sp; CHECK(spawn(RA, L, L.body0, nullptr, nullptr, c, 1.0f, &sp));
        CHECK(sp.r >= 0); CHECK(RA->valid(sp.r) == 1); CHECK(RA->count() == 1);
        float worst_cone = 0; Settle st; bool settled = false; float t_settle = -1; Xf B[NBODY];
        LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); double ms = 0; int steps = 0;
        for (int i = 0; i < 150; ++i) {
            QueryPerformanceCounter(&a); PA->step(0.04f, 8); QueryPerformanceCounter(&b); ms += (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart; ++steps;
            read_bodies(sp.r, B);
            float v = max_cone_violation(L, B); if (v > worst_cone) worst_cone = v;
            float ms_ = 0, mw = 0;
            for (int k = 0; k < NBODY; ++k) { float lv[3], av[3]; RA->vel(sp.r, k, lv, av); float sp_ = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]), sw = sqrtf(av[0] * av[0] + av[1] * av[1] + av[2] * av[2]); if (sp_ > ms_) ms_ = sp_; if (sw > mw) mw = sw; }
            if (!settled && st.update(ms_, mw, 0.04f, 0.15f, 0.6f, 0.5f)) { settled = true; t_settle = i * 0.04f; }
        }
        bool fin = true; for (int k = 0; k < NBODY; ++k) fin = fin && finite3(B[k].p);
        CHECK(fin);
        float py = B[PELVIS].p.y - ground;
        printf("  limp drop: pelvis %.2f m above the floor after 6 s, worst cone excess %.1f deg, settled %s at %.1f s, step %.3f ms (1 ragdoll, 8 substeps)\n",
               py, worst_cone * 57.3f, settled ? "yes" : "no", t_settle, ms / steps);
        CHECK(py > 0.04f && py < 0.45f);   // lying on the floor
        float rest_cone = max_cone_violation(L, B);
        printf("  cone excess at rest %.1f deg\n", rest_cone * 57.3f);
        CHECK(worst_cone < 0.45f);   // soft joints overshoot a little on impact ...
        CHECK(rest_cone < 0.1f);     // ... and are inside their limits again at rest
        CHECK(settled);
        for (int k = 0; k < NBODY; ++k) CHECK(B[k].p.y > ground - 0.05f);   // nothing through the floor
        // the pose written back from the settled bodies keeps the bones connected (fk of the written locals)
        Xf out[MAXB]; memcpy(out, s.local, sizeof(Xf) * s.n);
        pose_from_bodies(s, m, L, B, r1, out);
        Skeleton s2 = s; for (int i = 0; i < s.n; ++i) if (m.driver[i] >= 0) s2.local[i] = out[i];
        Xf W2[MAXB]; fk(s2, s2.local, r1, W2);
        float gap = 0;   // the skeleton's driving bones vs the simulated bodies (joint drift)
        for (int b = 0; b < NBODY; ++b) { float d = len(sub(W2[m.body_bone[b]].p, B[b].p)); if (d > gap) gap = d; }
        printf("  written skeleton vs bodies: worst %.3f m\n", gap);
        CHECK(gap < 0.08f);
        RA->destroy(sp.r); CHECK(RA->count() == 0); CHECK(RA->valid(sp.r) == 0);
    }
    // 2. powered: springs toward the standing pose + pelvis anchor: stays up; a chest hit knocks it, it recovers
    {
        floor_world(ground);
        Layout L; build_layout(s, m, W, nullptr, 1.0f, &L);
        Cfg c; Spawned sp; spawn(RA, L, L.body0, nullptr, nullptr, c, 0.0f, &sp);
        RA->gravity(sp.r, c.active_gravity);
        Xf anim[NBODY]; body_frames(m, W, anim);
        Strength str; str.reset(1.0f);
        auto drive = [&](float dt) {
            for (int b = 1; b < NBODY; ++b) {
                float ang; Q4 t = joint_target(L, b, anim, &ang);
                float tq[4] = {t.x, t.y, t.z, t.w};
                RA->drive(sp.r, sp.joint[b], c.joint_stiffness * str.joint(b), c.joint_damping, tq, ang);
            }
            float pp[3] = {anim[PELVIS].p.x, anim[PELVIS].p.y, anim[PELVIS].p.z}, pq[4] = {anim[PELVIS].q.x, anim[PELVIS].q.y, anim[PELVIS].q.z, anim[PELVIS].q.w};
            RA->anchor(sp.r, PELVIS, pp, pq, c.balance_hertz, c.balance_hertz, 1.0f, c.balance_force, c.balance_torque, dt);
            str.recover(dt, c.recover_seconds);
        };
        Xf B[NBODY];
        for (int i = 0; i < 50; ++i) { drive(0.04f); PA->step(0.04f, c.substeps); }
        read_bodies(sp.r, B);
        float err = 0; for (int b = 0; b < NBODY; ++b) { float e = qdiff(B[b].q, anim[b].q); if (e > err) err = e; }
        float pel = len(sub(B[PELVIS].p, anim[PELVIS].p));
        printf("  powered hold 2 s: worst body %.1f deg off the pose, pelvis %.3f m off\n", err * 57.3f, pel);
        CHECK(err < 0.35f); CHECK(pel < 0.08f);
        // a 250 N s shove on the chest from behind, the chest goes weak
        V3 chest = B[CHEST].p; float imp[3] = {0, 0, 250.0f}, at[3] = {chest.x, chest.y + 0.1f, chest.z};
        RA->impulse(sp.r, CHEST, imp, at); str.hit(CHEST, c.hit_strength, c.hit_neighbour);
        float peak = 0; float back_at = -1;
        for (int i = 0; i < 75; ++i) {
            drive(0.04f); PA->step(0.04f, c.substeps);
            read_bodies(sp.r, B);
            float off = qdiff(B[CHEST].q, anim[CHEST].q);
            if (off > peak) peak = off;
            if (back_at < 0 && i > 5 && off < 0.15f) back_at = i * 0.04f;
        }
        printf("  shove: chest up to %.0f deg off the pose, back within 9 deg after %.2f s\n", peak * 57.3f, back_at);
        CHECK(peak > 0.2f);          // visibly knocked
        CHECK(back_at > 0 && back_at < 2.0f);   // and back
        RA->destroy(sp.r);
    }
    // 3. explosion throws a ragdoll; 6 ragdolls cost
    {
        floor_world(ground);
        Layout L; build_layout(s, m, W, nullptr, 1.0f, &L);
        Cfg c; Spawned sp[6];
        for (int k = 0; k < 6; ++k) {
            Xf off = {v3(2.0f * k, 0, 0), qid()}; Xf B0[NBODY]; for (int b = 0; b < NBODY; ++b) B0[b] = xmul(off, L.body0[b]);
            CHECK(spawn(RA, L, B0, nullptr, nullptr, c, 1.0f, &sp[k]));
        }
        CHECK(RA->count() == 6);
        V3 p0 = L.body0[PELVIS].p;
        float cc[3] = {p0.x - 1.0f, p0.y - 0.5f, p0.z};
        int n = RA->explode(cc, 7.0f, 450.0f * c.blast_scale, c.blast_lift);
        CHECK(n >= 3);
        LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); double ms = 0, mx = 0;
        float top = 0;
        for (int i = 0; i < 100; ++i) {
            QueryPerformanceCounter(&a); PA->step(0.04f, 8); QueryPerformanceCounter(&b);
            double d = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart; ms += d; if (d > mx) mx = d;
            float y = body_y(sp[0].r, PELVIS) - p0.y; if (y > top) top = y;
        }
        float pp[3], pq[4]; RA->get(sp[0].r, PELVIS, pp, pq);
        float dist = len(sub(v3(pp[0], pp[1], pp[2]), p0));
        printf("  blast 1 m from the first of 6: thrown %.1f m (up to %.1f m high); 6 ragdolls step %.3f ms avg %.3f max (8 substeps)\n", dist, top, ms / 100, mx);
        CHECK(dist > 3.0f); CHECK(top > 0.8f);
        // ray cast hits a ragdoll body
        float from[3] = {pp[0], pp[1] + 3.0f, pp[2]}, to[3] = {pp[0], pp[1] - 3.0f, pp[2]}, h[3]; int bi = -1;
        int rr = RA->raycast(from, to, h, &bi);
        CHECK(rr >= 0 && bi >= 0);
        for (int k = 0; k < 6; ++k) RA->destroy(sp[k].r);
        // a new world kills the old ragdolls' handles
        Spawned again; spawn(RA, L, L.body0, nullptr, nullptr, c, 1.0f, &again);
        floor_world(ground);
        CHECK(RA->valid(again.r) == 0);
        float p[3], q[4]; CHECK(RA->get(again.r, 0, p, q) == -1);
        RA->destroy(again.r);
    }
    PA->destroy();
}

// ---- tuning sweep (RD_SWEEP=1 test_rd.exe): a standing powered ragdoll, one shove on the chest / pelvis
struct Knock { float chest, pelvis_tilt, pelvis_off, back; };
static Knock knock(const Cfg& c, int body, float impulse) {
    Skeleton s; Xf root; load(FX_CHARS[1], &s, &root);
    Map m; build_map(s, &m);
    Xf W[MAXB]; fk(s, s.local, root, W);
    float ground = W[find_bone(s, "L Toe0")].p.y - 0.03f;
    g_fc = W[find_bone(s, "Pelvis")].p;
    floor_world(ground);
    Layout L; build_layout(s, m, W, nullptr, 1.0f, &L);
    Spawned sp; spawn(RA, L, L.body0, nullptr, nullptr, c, 0.0f, &sp);
    RA->gravity(sp.r, c.active_gravity);
    Xf anim[NBODY]; body_frames(m, W, anim);
    Strength str; str.reset(1.0f);
    auto drive = [&](float dt) {
        for (int b = 1; b < NBODY; ++b) {
            float ang; Q4 t = joint_target(L, b, anim, &ang);
            float tq[4] = {t.x, t.y, t.z, t.w};
            RA->drive(sp.r, sp.joint[b], c.joint_stiffness * str.joint(b), c.joint_damping, tq, ang);
        }
        float k = 0.4f + 0.6f * str.s[PELVIS];
        float pp[3] = {anim[PELVIS].p.x, anim[PELVIS].p.y, anim[PELVIS].p.z}, pq[4] = {anim[PELVIS].q.x, anim[PELVIS].q.y, anim[PELVIS].q.z, anim[PELVIS].q.w};
        RA->anchor(sp.r, PELVIS, pp, pq, c.balance_hertz * k, c.balance_hertz * k, 1.0f, c.balance_force, c.balance_torque, dt);
        for (int fb : {(int)L_CALF, (int)R_CALF}) { float fp[3] = {anim[fb].p.x, anim[fb].p.y, anim[fb].p.z}, fq[4] = {anim[fb].q.x, anim[fb].q.y, anim[fb].q.z, anim[fb].q.w};
            RA->anchor(sp.r, fb, fp, fq, c.foot_hertz, c.foot_hertz * 0.5f, 1.0f, c.foot_force, c.foot_force * 0.1f, dt); }
        str.recover(dt, c.recover_seconds);
    };
    Xf B[NBODY];
    for (int i = 0; i < 25; ++i) { drive(0.04f); PA->step(0.04f, c.substeps); }
    read_bodies(sp.r, B);
    V3 at3 = B[body].p; float imp[3] = {0, 0, impulse}, at[3] = {at3.x, at3.y, at3.z};
    RA->impulse(sp.r, body, imp, at); str.hit(body, c.hit_strength, c.hit_neighbour);
    Knock k = {0, 0, 0, -1};
    for (int i = 0; i < 60; ++i) {
        drive(0.04f); PA->step(0.04f, c.substeps); read_bodies(sp.r, B);
        float ch = qdiff(B[CHEST].q, anim[CHEST].q), pt = qdiff(B[PELVIS].q, anim[PELVIS].q), po = len(sub(B[PELVIS].p, anim[PELVIS].p));
        if (ch > k.chest) k.chest = ch; if (pt > k.pelvis_tilt) k.pelvis_tilt = pt; if (po > k.pelvis_off) k.pelvis_off = po;
        if (k.back < 0 && i > 3 && ch < 0.12f && pt < 0.08f) k.back = i * 0.04f;
    }
    RA->destroy(sp.r);
    return k;
}
static void sweep() {
    PA = TODPhysics_GetApi(TODPHYS_API_VERSION); RA = TODPhysics_GetRagdollApi(TODRAG_API_VERSION);
    struct P { const char* name; float stiff, hs, hn, bh, bf, bt, grav, damp; };
    P ps[] = {
        {"defaults", 18, 0.15f, 0.55f, 8, 6000, 4000, 0.5f, 1.1f},
        {"weak pelvis", 18, 0.15f, 0.55f, 5, 3000, 2000, 0.5f, 1.1f},
        {"current", 14, 0.08f, 0.45f, 5, 2600, 1500, 0.6f, 1.0f},
        {"medium", 18, 0.15f, 0.55f, 8, 6000, 4000, 0.5f, 1.1f},
        {"medium soft", 16, 0.12f, 0.5f, 8, 6000, 3000, 0.5f, 1.1f},
        {"stiffer", 20, 0.25f, 0.7f, 6, 4000, 2500, 0.6f, 1.0f},
        {"strong anchor", 16, 0.2f, 0.65f, 8, 6000, 4000, 0.5f, 1.0f},
        {"soft hit", 18, 0.35f, 0.75f, 7, 5000, 3000, 0.5f, 1.2f},
        {"very strong", 22, 0.3f, 0.75f, 10, 9000, 6000, 0.5f, 1.2f},
    };
    for (auto& p : ps)
        for (float imp : {45.0f, 70.0f, 150.0f}) for (int body : {(int)CHEST, (int)PELVIS}) {
            Cfg c; c.joint_stiffness = p.stiff; c.hit_strength = p.hs; c.hit_neighbour = p.hn; c.balance_hertz = p.bh;
            c.balance_force = p.bf; c.balance_torque = p.bt; c.active_gravity = p.grav; c.joint_damping = p.damp; c.clamp();
            Knock k = knock(c, body, imp);
            printf("  %-14s %3.0f N s on %-6s: chest %5.1f deg, pelvis tilt %5.1f deg, pelvis off %.2f m, back %.2f s\n", p.name, imp,
                   body_name(body), k.chest * 57.3f, k.pelvis_tilt * 57.3f, k.pelvis_off, k.back);
        }
    PA->destroy();
}

// ---- hit lab (RD_HITLAB=1 test_rd.exe, ragdoll-hits): the game's live-hit and death logic (share_push, active_drive,
// the anchors and the blend of rd_game.cpp) on a standing character, measured on the pose the player sees:
//   peak  = largest visible angle of a body off the animation (deg)
//   rev   = direction reversals of the head / hand along the push (amplitude > 2 cm): 1 = out and back, more = wobble
//   hand  = top speed of a forearm body (m/s)        back = seconds until the animation has the body again
static bool g_trace = false;
struct LabRun { float peak_chest, peak_head, peak_fore, hand, back, settle, cone, hmax, hmin; int rev_head, rev_hand; };
struct Rev {   // reversal counter of a signed signal with a dead band
    float ext = 0; int dir = 0, n = 0; bool started = false;
    void add(float v, float band) {
        if (!started) { if (fabsf(v) > band) { started = true; ext = v; dir = v > 0 ? 1 : -1; } return; }
        if (dir > 0) { if (v > ext) ext = v; else if (ext - v > band) { ++n; dir = -1; ext = v; } }
        else { if (v < ext) ext = v; else if (v - ext > band) { ++n; dir = 1; ext = v; } }
    }
};
static V3 cap_mid(const Layout& L, const Xf* B, int b) { return xpoint(B[b], mul(add(L.c1[b][0], L.c2[b][0]), 0.5f)); }
static void lab_push(const Layout& L, int r, const Xf* B, int b, V3 imp, float maxdv, float share, float mk) {
    Push p = share_push(L, mk, b, imp, maxdv, share);
    V3 dir = ph::norm(imp);
    for (int k = 0; k < p.n; ++k) {
        V3 at = cap_mid(L, B, p.body[k]);
        if (k == 0) at = sub(at, mul(dir, L.rad[b][0]));   // on the surface the bullet came through
        float i3[3] = {p.imp[k].x, p.imp[k].y, p.imp[k].z}, a3[3] = {at.x, at.y, at.z};
        RA->impulse(r, p.body[k], i3, a3);
    }
}
static LabRun lab_active(const Cfg& c, int body, int hits, float every) {
    Skeleton s; Xf root; load(FX_CHARS[1], &s, &root);
    Map m; build_map(s, &m);
    Xf W[MAXB]; fk(s, s.local, root, W);
    float ground = W[find_bone(s, "L Toe0")].p.y - 0.03f;
    g_fc = W[find_bone(s, "Pelvis")].p;
    floor_world(ground);
    Layout L; build_layout(s, m, W, nullptr, 1.0f, &L);
    Xf anim[NBODY]; body_frames(m, W, anim);
    tighten_spine(&L, anim, c.spine_limit_scale, 0.06f);
    Spawned sp; spawn(RA, L, anim, nullptr, nullptr, c, c.active_friction, &sp, true);
    RA->gravity(sp.r, c.active_gravity);
    float mk = c.total_mass / 80.0f;
    Strength str; str.reset(1.0f);
    float w = c.blend_in > 0 ? 0.0f : 1.0f, t = 0, t_hit = 0, budget = 0, budget_t = -1; bool releasing = false;
    V3 dir = v3(0, 0, 1);   // shot from behind (+z)
    LabRun R; memset(&R, 0, sizeof R); R.back = -1;
    Rev rh, rf; Xf B[NBODY]; read_bodies(sp.r, B);
    int next = 0;
    for (int i = 0; i < 100; ++i, t += 0.04f) {
        while (next < hits && t >= next * every - 1e-4f) {   // a hit: the game's hit_active (budget, share, weak muscles)
            if (t - budget_t > 0.5f) { budget_t = t; budget = 0; }
            float J = c.hit_impulse * c.impulse_scale, cap = c.hit_burst * c.hit_impulse * c.impulse_scale;
            if (budget + J > cap) J = cap - budget > 0 ? cap - budget : 0;
            budget += J;
            if (J > 0) lab_push(L, sp.r, B, body, mul(dir, J), c.hit_max_dv, c.hit_core_share, mk);
            str.hit(body, c.hit_strength, c.hit_neighbour);
            t_hit = t; releasing = false; ++next;
        }
        // ragdoll_before_step, M_ACTIVE
        str.recover(0.04f, c.recover_seconds);
        ActiveDrive ad; active_drive(c, str, &ad);
        for (int b = 1; b < NBODY; ++b) {
            float ang; Q4 q = joint_target(L, b, anim, &ang); float tq[4] = {q.x, q.y, q.z, q.w};
            RA->drive(sp.r, sp.joint[b], ad.hz[b], ad.damping[b], tq, ang);
        }
        float pp[3] = {anim[PELVIS].p.x, anim[PELVIS].p.y, anim[PELVIS].p.z}, pq[4] = {anim[PELVIS].q.x, anim[PELVIS].q.y, anim[PELVIS].q.z, anim[PELVIS].q.w};
        RA->anchor(sp.r, PELVIS, pp, pq, ad.balance_hz, ad.balance_hz, ad.balance_damping, c.balance_force, c.balance_torque, 0.04f);
        for (int fb : {(int)L_CALF, (int)R_CALF}) {
            float fp[3] = {anim[fb].p.x, anim[fb].p.y, anim[fb].p.z}, fq[4] = {anim[fb].q.x, anim[fb].q.y, anim[fb].q.z, anim[fb].q.w};
            RA->anchor(sp.r, fb, fp, fq, c.foot_hertz, c.foot_hertz * 0.5f, c.foot_damping, c.foot_force, c.foot_force * 0.1f, 0.04f);
        }
        float in = c.blend_in > 0 ? 0.04f / c.blend_in : 1.0f, outr = c.blend_out > 0 ? 0.04f / c.blend_out : 1.0f;
        bool recovered = str.min_value() > 0.98f && t - t_hit > c.recover_seconds + 0.15f;
        if (recovered) { releasing = true; w -= outr; } else w += in;
        if (w > 1.0f) w = 1.0f;
        PA->step(0.04f, c.substeps);
        read_bodies(sp.r, B);
        if (releasing && w <= 0.0f) { R.back = t + 0.04f; break; }
        // what the player sees: slerp(anim, sim, w)
        float pc = w * qdiff(B[CHEST].q, anim[CHEST].q), ph_ = w * qdiff(B[HEAD].q, anim[HEAD].q);
        float pf = w * (qdiff(B[L_FORE].q, anim[L_FORE].q) > qdiff(B[R_FORE].q, anim[R_FORE].q) ? qdiff(B[L_FORE].q, anim[L_FORE].q) : qdiff(B[R_FORE].q, anim[R_FORE].q));
        if (pc > R.peak_chest) R.peak_chest = pc; if (ph_ > R.peak_head) R.peak_head = ph_; if (pf > R.peak_fore) R.peak_fore = pf;
        V3 hv = sub(cap_mid(L, B, HEAD), cap_mid(L, anim, HEAD));
        V3 fv = sub(xpoint(B[L_FORE], L.c2[L_FORE][0]), xpoint(anim[L_FORE], L.c2[L_FORE][0]));
        V3 fv2 = sub(xpoint(B[R_FORE], L.c2[R_FORE][0]), xpoint(anim[R_FORE], L.c2[R_FORE][0]));
        if (len(fv2) > len(fv)) fv = fv2;
        rh.add(w * ph::dot(hv, dir), 0.02f);
        { float hs = w * ph::dot(hv, dir); if (hs > R.hmax) R.hmax = hs; if (hs < R.hmin) R.hmin = hs; }
        rf.add(w * ph::dot(fv, dir), 0.02f);
        if (g_trace) printf("    t %.2f w %.2f head %+.3f hand %+.3f chest %5.1f str %.2f | pelvis %4.1f spine %4.1f thigh %4.1f calf %4.1f\n", t, w, w * ph::dot(hv, dir), w * ph::dot(fv, dir), pc * 57.3f, str.min_value(),
                            w * qdiff(B[PELVIS].q, anim[PELVIS].q) * 57.3f, w * qdiff(B[SPINE].q, anim[SPINE].q) * 57.3f, w * qdiff(B[L_THIGH].q, anim[L_THIGH].q) * 57.3f, w * qdiff(B[L_CALF].q, anim[L_CALF].q) * 57.3f);
        for (int fb : {(int)L_FORE, (int)R_FORE}) { float lv[3], av[3]; RA->vel(sp.r, fb, lv, av); float v = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]); if (v > R.hand) R.hand = v; }
        float cv = max_cone_violation(L, B); if (cv > R.cone) R.cone = cv;
    }
    R.rev_head = rh.n; R.rev_hand = rf.n;
    RA->destroy(sp.r);
    return R;
}
static LabRun lab_death(const Cfg& c, int body) {   // killed standing by one bullet from behind: start_death + apply_death_push
    Skeleton s; Xf root; load(FX_CHARS[1], &s, &root);
    Map m; build_map(s, &m);
    Xf W[MAXB]; fk(s, s.local, root, W);
    float ground = W[find_bone(s, "L Toe0")].p.y - 0.03f;
    g_fc = W[find_bone(s, "Pelvis")].p;
    floor_world(ground);
    Layout L; build_layout(s, m, W, nullptr, 1.0f, &L);
    Xf anim[NBODY]; body_frames(m, W, anim);
    tighten_spine(&L, anim, c.spine_limit_scale, 0.06f);
    Spawned sp; spawn(RA, L, anim, nullptr, nullptr, c, c.limp_friction, &sp, false);
    float mk = c.total_mass / 80.0f;
    V3 dir = v3(0, 0, 1);
    Xf B[NBODY]; read_bodies(sp.r, B);
    lab_push(L, sp.r, B, body, mul(dir, c.bullet_impulse * c.impulse_scale), c.death_max_dv, c.death_core_share, mk);
    LabRun R; memset(&R, 0, sizeof R); R.back = -1; R.settle = -1;
    Rev rh, rf; Settle st; float t = 0;
    for (int i = 0; i < 125; ++i, t += 0.04f) {
        float k = c.death_fade > 0 ? 1.0f - t / c.death_fade : 0.0f;
        for (int b = 1; b < NBODY; ++b) {
            if (k > 0) { float ang; Q4 q = joint_target(L, b, anim, &ang); float tq[4] = {q.x, q.y, q.z, q.w}; RA->drive(sp.r, sp.joint[b], c.joint_stiffness * k, c.joint_damping, tq, ang); }
            else RA->drive(sp.r, sp.joint[b], 0, 0, nullptr, 0);
        }
        PA->step(0.04f, c.substeps);
        read_bodies(sp.r, B);
        // head and hands relative to the chest (the body itself falls): whip / wobble of the parts
        Xf ci = xinv(B[CHEST]);
        V3 hv = xpoint(ci, cap_mid(L, B, HEAD));
        V3 fv = xpoint(ci, xpoint(B[L_FORE], L.c2[L_FORE][0]));
        static V3 h0, f0; if (i == 0) { h0 = hv; f0 = fv; }
        rh.add(hv.z - h0.z, 0.02f); rf.add(len(sub(fv, f0)), 0.04f);
        float mv = 0, mw = 0;
        for (int b = 0; b < NBODY; ++b) {
            float lv[3], av[3]; RA->vel(sp.r, b, lv, av);
            float v = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]), wv = sqrtf(av[0] * av[0] + av[1] * av[1] + av[2] * av[2]);
            if (v > mv) mv = v; if (wv > mw) mw = wv;
            if ((b == L_FORE || b == R_FORE) && v > R.hand) R.hand = v;
        }
        float hd = qdiff(qmul(qconj(B[CHEST].q), B[HEAD].q), qmul(qconj(anim[CHEST].q), anim[HEAD].q));
        if (hd > R.peak_head) R.peak_head = hd;
        float cv = max_cone_violation(L, B); if (cv > R.cone) R.cone = cv;
        if (g_trace && t < 1.6f) printf("    t %.2f head-z %+.3f hand %.3f head/chest %5.1f cone+ %5.1f pelvis y %.2f\n", t, hv.z - h0.z, len(sub(fv, f0)), hd * 57.3f, cv * 57.3f, B[PELVIS].p.y - ground);
        if (R.settle < 0 && st.update(mv, mw, 0.04f, 0.12f, 0.6f, 0.6f)) R.settle = t;
    }
    R.rev_head = rh.n; R.rev_hand = rf.n;
    RA->destroy(sp.r);
    return R;
}
static void lab_cfg(Cfg& c, const char* spec) {   // "old" | "new", then ":key=value" overrides
    char buf[1024]; strncpy(buf, spec, 1023); buf[1023] = 0;
    char* p = strtok(buf, ":");
    c = Cfg();
    if (p && !strcmp(p, "old")) {   // the installed tuning (master 1a13961)
        c.hit_impulse = 110; c.hit_max_dv = 48; c.hit_core_share = 0; c.hit_burst = 1.6f; c.active_friction = 0; c.active_ang_damping = 0.6f;
        c.spine_limit_scale = 1; c.death_max_dv = 100; c.death_core_share = 0; c.limp_ang_damping = 0.6f;
        c.joint_stiffness = 18; c.joint_damping = 1.1f; c.hit_strength = 0.15f; c.hit_neighbour = 0.55f; c.recover_seconds = 0.7f;
        c.active_gravity = 0.5f; c.limp_friction = 1.0f; c.death_fade = 0.35f; c.bullet_impulse = 110; c.balance_hertz = 8;
    }
    struct F { const char* k; float* v; } fs[] = {
        {"hit_impulse", &c.hit_impulse}, {"hit_max_dv", &c.hit_max_dv}, {"hit_core_share", &c.hit_core_share}, {"hit_burst", &c.hit_burst},
        {"active_friction", &c.active_friction}, {"active_ang_damping", &c.active_ang_damping}, {"spine_limit_scale", &c.spine_limit_scale},
        {"death_max_dv", &c.death_max_dv}, {"death_core_share", &c.death_core_share}, {"limp_ang_damping", &c.limp_ang_damping},
        {"joint_stiffness", &c.joint_stiffness}, {"joint_damping", &c.joint_damping}, {"hit_strength", &c.hit_strength},
        {"hit_neighbour", &c.hit_neighbour}, {"recover_seconds", &c.recover_seconds}, {"active_gravity", &c.active_gravity},
        {"limp_friction", &c.limp_friction}, {"death_fade", &c.death_fade}, {"bullet_impulse", &c.bullet_impulse},
        {"balance_hertz", &c.balance_hertz}, {"balance_damping", &c.balance_damping}, {"foot_force", &c.foot_force}, {"foot_damping", &c.foot_damping}, {"balance_force", &c.balance_force}, {"blend_out", &c.blend_out}, {"foot_hertz", &c.foot_hertz}};
    while ((p = strtok(nullptr, ":")) != nullptr) {
        char* e = strchr(p, '='); if (!e) continue; *e = 0;
        bool ok = false;
        for (auto& f : fs) if (!strcmp(f.k, p)) { *f.v = (float)atof(e + 1); ok = true; }
        if (!ok) printf("  (unknown key %s)\n", p);
    }
}
static void hitlab() {
    PA = TODPhysics_GetApi(TODPHYS_API_VERSION); RA = TODPhysics_GetRagdollApi(TODRAG_API_VERSION);
    g_trace = getenv("RD_TRACE") != nullptr;
    int only = getenv("RD_TRACE") ? atoi(getenv("RD_TRACE")) : -1;   // RD_TRACE=<body>: that body only, x1 and death
    const char* names[16]; int nn = 0;
    const char* env = getenv("RD_HITLAB");
    static char buf[4096]; strncpy(buf, env && strcmp(env, "1") ? env : "old,new", 4095);
    for (char* p = strtok(buf, ","); p && nn < 16; p = strtok(nullptr, ",")) names[nn++] = p;
    for (int k = 0; k < nn; ++k) {
        Cfg c; lab_cfg(c, names[k]); c.clamp();
        printf("== %s: hit %.0f N s dv %.1f core %.2f burst %.1f | stiff %.0f Hz damp %.2f hs %.2f hn %.2f rec %.2f grav %.2f fric %.1f angd %.1f spine x%.2f | death dv %.1f core %.2f angd %.1f fric %.1f\n",
               names[k], c.hit_impulse, c.hit_max_dv, c.hit_core_share, c.hit_burst, c.joint_stiffness, c.joint_damping, c.hit_strength, c.hit_neighbour,
               c.recover_seconds, c.active_gravity, c.active_friction, c.active_ang_damping, c.spine_limit_scale, c.death_max_dv, c.death_core_share, c.limp_ang_damping, c.limp_friction);
        for (int body : {(int)CHEST, (int)HEAD, (int)L_FORE, (int)L_UPPER, (int)PELVIS, (int)L_THIGH}) for (int hits : {1, 5}) {
            if (only >= 0 && (body != only || hits != 1)) continue;
            LabRun r = lab_active(c, body, hits, 0.1f);
            printf("  live %-10s x%d: peak chest %5.1f head %5.1f forearm %5.1f deg | rev head %d hand %d | hand %5.1f m/s | back %.2f s | head +%.2f %.2f m\n",
                   body_name(body), hits, r.peak_chest * 57.3f, r.peak_head * 57.3f, r.peak_fore * 57.3f, r.rev_head, r.rev_hand, r.hand, r.back, r.hmax, r.hmin);
        }
        for (int body : {(int)CHEST, (int)HEAD, (int)L_FORE}) {
            if (only >= 0 && body != only) continue;
            LabRun r = lab_death(c, body);
            printf("  death %-10s: head vs chest up to %5.1f deg | rev head %d hand %d | hand %5.1f m/s | settled %.2f s | cone+ %.1f\n",
                   body_name(body), r.peak_head * 57.3f, r.rev_head, r.rev_hand, r.hand, r.settle, r.cone * 57.3f);
        }
    }
    PA->destroy();
}

// ragdoll-hits: the shared push, the spine limits, the powered drive, and the hit lab against the tube-man tuning
static void test_hits() {
    Skeleton s; Xf root; load(FX_CHARS[1], &s, &root);
    Map m; build_map(s, &m);
    Xf W[MAXB]; fk(s, s.local, root, W);
    Layout L; build_layout(s, m, W, nullptr, 1.0f, &L);
    CHECK(core_of(L_FORE) == CHEST && core_of(HEAD) == CHEST && core_of(R_CALF) == PELVIS && core_of(SPINE) == SPINE && core_of(PELVIS) == PELVIS);
    // a 60 N s shot in the forearm: the 2 kg forearm gets at most 4 m/s (8 N s), the chest the rest
    Push p = share_push(L, 1.0f, L_FORE, v3(0, 0, 60), 4.0f, 0.5f);
    CHECK(p.n == 2 && p.body[0] == L_FORE && p.body[1] == CHEST);
    CHECK(nearf(len(p.imp[0]), 8.0f, 1e-3f)); CHECK(nearf(len(p.imp[1]), 52.0f, 1e-3f));
    CHECK(p.imp[1].z > 0 && fabsf(p.imp[1].x) < 1e-5f);
    // a torso part takes it all, capped by its own mass (15 kg chest x 4 m/s = 60)
    p = share_push(L, 1.0f, CHEST, v3(0, 0, 100), 4.0f, 0.5f);
    CHECK(p.n == 1 && nearf(len(p.imp[0]), 60.0f, 1e-3f));
    p = share_push(L, 1.0f, HEAD, v3(10, 0, 0), 4.0f, 0.5f);   // 5 to the head (half), 5 to the chest
    CHECK(p.n == 2 && nearf(len(p.imp[0]), 5.0f, 1e-3f) && nearf(len(p.imp[1]), 5.0f, 1e-3f));
    p = share_push(L, 1.0f, HEAD, v3(0, 0, 0), 4.0f, 0.5f); CHECK(p.n == 0);
    // the old tuning (110 N s, 48 m/s): a 2 kg forearm took 96 N s = 48 m/s (the whip)
    p = share_push(L, 1.0f, L_FORE, v3(0, 0, 110), 48.0f, 0.0f);
    CHECK(nearf(len(p.imp[0]), 96.0f, 1e-3f));
    // spine limits: x0.7 on a neutral pose, never tighter than the pose itself (+ margin)
    Layout L2 = L; Xf anim[NBODY]; body_frames(m, W, anim);
    float k = tighten_spine(&L2, anim, 0.7f, 0.06f);
    CHECK(k > 0.69f && k <= 1.0f);
    CHECK(nearf(L2.cone[SPINE], L.cone[SPINE] * 0.7f, 0.02f)); CHECK(nearf(L2.twist_hi[HEAD], L.twist_hi[HEAD] * 0.7f, 0.02f));
    CHECK(L2.cone[L_UPPER] == L.cone[L_UPPER]);   // only spine / chest / head
    Xf bent[NBODY]; memcpy(bent, anim, sizeof bent);   // the chest bent 15 deg off its limit centre
    bent[CHEST].q = ph::qnorm(qmul(qaxis(v3(1, 0, 0), 0.26f), bent[CHEST].q));
    Layout L3 = L; tighten_spine(&L3, bent, 0.5f, 0.06f);
    { Q4 fa = qmul(bent[SPINE].q, L3.frameA[CHEST].q), fb = qmul(bent[CHEST].q, L3.frameB[CHEST].q);
      float c = ph::dot(qrot(fa, v3(0, 0, 1)), qrot(fb, v3(0, 0, 1))); float sw = acosf(c > 1 ? 1 : c);
      CHECK(L3.cone[CHEST] >= sw + 0.059f); CHECK(L3.cone[CHEST] <= L.cone[CHEST] + 1e-5f); }
    CHECK(tighten_spine(&L2, anim, 1.0f, 0.06f) == 1.0f);
    // the powered drive: joint hertz from the strengths, the balance from the weakest part
    Cfg c; Strength st; st.reset(1.0f); ActiveDrive ad; active_drive(c, st, &ad);
    CHECK(nearf(ad.hz[CHEST], c.joint_stiffness, 1e-4f)); CHECK(nearf(ad.balance_hz, c.balance_hertz, 1e-4f)); CHECK(ad.damping[HEAD] == c.joint_damping);
    st.hit(CHEST, c.hit_strength, c.hit_neighbour); active_drive(c, st, &ad);
    CHECK(nearf(ad.hz[CHEST], c.joint_stiffness * st.joint(CHEST), 1e-4f)); CHECK(ad.balance_hz < c.balance_hertz);
    // version 1 of the ragdoll table is still served (an older TODGameplay.dll), with the same functions
    CHECK(TODPhysics_GetRagdollApi(1) == TODPhysics_GetRagdollApi(2) && TODPhysics_GetRagdollApi(2)->damping != nullptr);
    // hit lab: the new defaults flinch and come back, the old tuning folded the man over and swayed (the tube man)
    PA = TODPhysics_GetApi(TODPHYS_API_VERSION); RA = TODPhysics_GetRagdollApi(TODRAG_API_VERSION);
    Cfg old; lab_cfg(old, "old"); old.clamp(); Cfg nw; nw.clamp();
    LabRun o = lab_active(old, CHEST, 1, 0.1f), n = lab_active(nw, CHEST, 1, 0.1f);
    LabRun of = lab_active(old, L_FORE, 1, 0.1f), nf = lab_active(nw, L_FORE, 1, 0.1f);
    LabRun od = lab_death(old, L_FORE), nd = lab_death(nw, L_FORE);
    printf("  hit lab, one bullet in the chest: old %.0f deg / sway back %.2f m, new %.0f deg / %.2f m; forearm hit: hand %.1f -> %.1f m/s; "
           "killed by a forearm shot: hand %.1f -> %.1f m/s\n", o.peak_chest * 57.3f, -o.hmin, n.peak_chest * 57.3f, -n.hmin, of.hand, nf.hand, od.hand, nd.hand);
    CHECK(o.peak_chest > 0.7f && -o.hmin > 0.2f);                 // the report, reproduced
    CHECK(n.peak_chest > 0.12f && n.peak_chest < 0.45f);          // a visible flinch (7 .. 26 deg) ...
    CHECK(-n.hmin < 0.35f * n.hmax);                              // ... that does not sway back past the pose
    CHECK(n.back > 0.5f && n.back < 1.5f);                        // and the animation has him back
    CHECK(nf.hand < 3.0f && of.hand > 8.0f);                      // no forearm whip
    CHECK(nd.hand < 6.0f && nd.settle > 0 && nd.settle < 2.5f);   // a heavy fall that settles
    PA->destroy();
}

static void test_life() {   // stale death state of a reused shell / a born-dead reinforcement is no death
    CHECK(life_alive(20, false) && life_alive(6, false) && !life_alive(28, false) && !life_alive(20, true) && !life_alive(-1, false));
    CHECK(!life_dead(28, false, false, false));          // a reused shell still in state 28, never seen alive: no ragdoll
    CHECK(!life_dead(20, true, false, false));           // a reinforcement born with a lethal DamageTaken
    CHECK(!life_dead(20, true, false, true));            // the same, seen alive before, but no hit on it: stale, not a death
    CHECK(life_dead(28, false, false, true));            // a real death
    CHECK(life_dead(20, true, true, true));              // a lethal hit the AI has not turned into state 28 yet
    CHECK(!life_dead(-1, true, true, true));             // back in the pool
}

static void test_stall() {   // psycho-tune: the world steps only in the on-foot input tick (Ram driving = no step)
    StepWatch w;
    CHECK(!w.stalled(1000, 300));                        // the first look only sets the base
    CHECK(!w.stalled(1300, 300));                        // exactly the limit: not yet
    CHECK(w.stalled(1301, 300));                         // game time ran on with no step
    w.stepped(1301); CHECK(!w.stalled(1500, 300));       // a step resets it
    CHECK(!w.stalled(1500, 300) && !w.stalled(1500, 300));   // paused: the game-ms clock stands, so no stall
    CHECK(!w.stalled(200, 300));                         // the clock went back (load / rewind): rebase, no stall
    CHECK(!w.stalled(500, 300) && w.stalled(501, 300));  // ... and counts from the new base
    StepWatch s; s.stepped(0x7FFFFFF0u); CHECK(!s.stalled(0x80000100u, 300) && s.stalled(0x80000200u, 300));   // no sign trouble
    CHECK(stall_action(RM_NONE, 0.0f, 0.8f) == SA_KEEP);
    CHECK(stall_action(RM_FROZEN, 2.0f, 0.8f) == SA_KEEP);   // a frozen corpse is a pose, it needs no step
    CHECK(stall_action(RM_ACTIVE, 0.5f, 0.8f) == SA_RELEASE);   // a live enemy goes back to its animation
    CHECK(stall_action(RM_LIMP, 0.0f, 0.8f) == SA_RELEASE);  // a corpse that never fell: the game's own death clip (no standing corpse)
    CHECK(stall_action(RM_LIMP, 0.79f, 0.8f) == SA_RELEASE);
    CHECK(stall_action(RM_LIMP, 0.8f, 0.8f) == SA_FREEZE);   // it had fallen: keep the pose it lies in
    Cfg c; c.stall_seconds = 0.0f; c.stall_fall_seconds = 99.0f; c.clamp();
    CHECK(c.stall_seconds >= 0.1f - 1e-6f && c.stall_fall_seconds <= 10.0f + 1e-6f);
}

int main() {
    if (getenv("RD_SWEEP")) { sweep(); return 0; }
    if (getenv("RD_HITLAB")) { hitlab(); return 0; }
    test_stall();
    test_life();
    test_math();
    test_strength();
    test_skeletons();
    test_sim();
    test_hits();
    printf("test_rd: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
