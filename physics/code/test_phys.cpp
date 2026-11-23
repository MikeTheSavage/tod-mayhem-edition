// test_phys.cpp - unit tests of the [physics] prototype: phys_logic (transform conventions, hull preparation, prop
// names, step clock) and phys_sim (Box3D world through the same function table TODPhysics.dll exports), as 32-bit.
// Built and run by dll/build.bat.
#include <stdio.h>
#include <math.h>
#include <string.h>
#define NOMINMAX
#include <windows.h>
#include "box3d/collision.h"
#include "phys_logic.h"
#include "phys_api.h"
extern "C" const PhApi* __cdecl TODPhysics_GetApi(int version);

using namespace ph;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool nearf(float a, float b, float e) { return fabsf(a - b) <= e; }
static bool nearv(V3 a, V3 b, float e) { return nearf(a.x, b.x, e) && nearf(a.y, b.y, e) && nearf(a.z, b.z, e); }
static bool sameq(Q4 a, Q4 b, float e) {   // q and -q are the same rotation
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return fabsf(fabsf(d) - 1.0f) <= e;
}
static Q4 axis_angle(V3 ax, float ang) { ax = norm(ax); float s = sinf(ang / 2); Q4 q = {ax.x * s, ax.y * s, ax.z * s, cosf(ang / 2)}; return q; }
// the engine's local matrix from a STORED quaternion: the standard column-vector matrix of the stored value, used in
// row-vector form (rows of M = rows of Rc(qs)) - design_renderer_bridge.md 3.2
static void engine_matrix(Q4 qs, V3 p, float m[16]) {
    float x = qs.x, y = qs.y, z = qs.z, w = qs.w;
    float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) m[r * 4 + c] = R[r][c]; m[r * 4 + 3] = 0; }
    m[12] = p.x; m[13] = p.y; m[14] = p.z; m[15] = 1;
}

static void test_math() {
    Q4 q = axis_angle(v3(0.3f, 1, -0.2f), 1.1f);
    V3 p = v3(-242.4f, 1.18f, -43.66f);
    float m[16]; tod_from_rigid(p, q, m);
    Rigid r = rigid_from_tod(m);
    CHECK(r.ok); CHECK(sameq(r.q, q, 1e-5f)); CHECK(nearv(r.p, p, 1e-4f)); CHECK(nearv(r.scale, v3(1, 1, 1), 1e-5f));
    // a local point goes where the true rotation puts it
    V3 l = v3(0.5f, -0.25f, 2.0f);
    CHECK(nearv(tod_point(m, l), add(p, qrot(q, l)), 1e-4f));
    // the engine's matrix from a stored quaternion is the CONJUGATE rotation: SetRotationY(y) stores (0, sin(-y/2), 0, cos(-y/2))
    float y = 0.7f; Q4 qs = {0, sinf(-y / 2), 0, cosf(-y / 2)};
    float em[16]; engine_matrix(qs, v3(1, 2, 3), em);
    Rigid er = rigid_from_tod(em);
    CHECK(er.ok); CHECK(sameq(er.q, true_from_stored(qs), 1e-5f));
    CHECK(sameq(stored_from_true(er.q), qs, 1e-5f));
    CHECK(sameq(er.q, axis_angle(v3(0, 1, 0), y), 1e-5f));   // a positive true yaw
    // local under a rotated, translated, uniformly scaled parent
    Q4 pq = axis_angle(v3(0, 1, 0), -2.0f); float pm[16]; tod_from_rigid(v3(10, 0, -5), pq, pm);
    for (int i = 0; i < 12; ++i) if (i % 4 != 3) pm[i] *= 2.0f;
    Q4 wq = axis_angle(v3(1, 0, 1), 0.4f); V3 wp = v3(12, 1, -4);
    V3 lp; Q4 lq;
    CHECK(local_from_world(pm, wp, wq, &lp, &lq));
    CHECK(nearv(tod_point(pm, lp), wp, 1e-4f));
    CHECK(sameq(qmul(pq, lq), wq, 1e-5f));
    // mirrored matrices are refused
    float mm[16]; memcpy(mm, m, sizeof mm); mm[0] = -mm[0]; mm[1] = -mm[1]; mm[2] = -mm[2];
    CHECK(!rigid_from_tod(mm).ok);
    // slerp ends and midpoint
    Q4 a = axis_angle(v3(0, 1, 0), 0.0f), b = axis_angle(v3(0, 1, 0), 1.0f);
    CHECK(sameq(qslerp(a, b, 0), a, 1e-5f)); CHECK(sameq(qslerp(a, b, 1), b, 1e-5f));
    CHECK(sameq(qslerp(a, b, 0.5f), axis_angle(v3(0, 1, 0), 0.5f), 1e-4f));
}

static void test_hull() {
    V3 cube[8], out[16];
    for (int i = 0; i < 8; ++i) cube[i] = v3((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
    CHECK(prepare_hull(cube, 8, 0.02f, 0.25f, out) == 8);
    V3 nrm; CHECK(nearf(thickness(cube, 8, &nrm), 1.0f, 1e-3f));
    // a floor triangle: slab below, top face unchanged
    V3 tri[3] = {v3(0, 1, 0), v3(2, 1, 0), v3(0, 1, 2)};
    int n = prepare_hull(tri, 3, 0.02f, 0.25f, out);
    CHECK(n == 6);
    bool top = true, below = true;
    for (int i = 0; i < 3; ++i) { top = top && nearv(out[i], tri[i], 1e-5f); below = below && nearf(out[3 + i].y, 0.75f, 1e-4f); }
    CHECK(top); CHECK(below);
    // a tilted floor quad given with a downward winding still gets its slab underneath
    V3 quad[4] = {v3(0, 0, 0), v3(0, 0.2f, 2), v3(2, 0.2f, 2), v3(2, 0, 0)};
    n = prepare_hull(quad, 4, 0.02f, 0.3f, out);
    CHECK(n == 8);
    CHECK(out[4].y < quad[0].y && out[5].y < quad[1].y);
    // a wall: symmetric slab
    V3 wall[4] = {v3(0, 0, 0), v3(0, 3, 0), v3(0, 3, 4), v3(0, 0, 4)};
    n = prepare_hull(wall, 4, 0.02f, 0.2f, out);
    CHECK(n == 8);
    CHECK(nearf(fabsf(out[0].x - out[4].x), 0.2f, 1e-4f) && nearf(out[0].x + out[4].x, 0.0f, 1e-4f));
    // degenerate: a line, a point
    V3 line[3] = {v3(0, 0, 0), v3(1, 0, 0), v3(2, 0, 0)};
    CHECK(prepare_hull(line, 3, 0.02f, 0.2f, out) == 0);
    V3 pt[3] = {v3(1, 1, 1), v3(1, 1, 1), v3(1, 1, 1)};
    CHECK(prepare_hull(pt, 3, 0.02f, 0.2f, out) == 0);
    CHECK(prepare_hull(pt, 2, 0.02f, 0.2f, out) == 0);
}

static void test_names() {
    CHECK(name_match("/Models/Props/Barrel_Red01.model", "crate, barrel"));
    CHECK(name_match("/data/models/x/WOODENCRATE.model", "crate"));
    CHECK(!name_match("/Models/Buildings/House.model", "crate,barrel"));
    CHECK(!name_match("anything", ""));
    CHECK(!name_match(nullptr, "a"));
    CHECK(!name_match("abc", " , ,"));
    PropRule r; CHECK(prop_size_ok(r, 0.5f)); CHECK(!prop_size_ok(r, 5.0f)); CHECK(!prop_size_ok(r, 0.01f));
    float nanv = NAN; CHECK(!prop_size_ok(r, nanv));
}

static void test_clock() {
    StepClock c; c.step = 0.04f;
    CHECK(c.advance(0.016f) == 0); CHECK(c.advance(0.016f) == 0); CHECK(c.advance(0.016f) == 1);
    CHECK(nearf(c.acc, 0.008f, 1e-5f)); CHECK(nearf(c.alpha(), 0.2f, 1e-3f));
    CHECK(c.advance(0.0f) == 0); CHECK(c.advance(-1.0f) == 0);
    CHECK(c.advance(1.0f) == 4);   // backlog dropped
    CHECK(c.acc < c.step);
    Cfg g; g.radius = -3; g.max_bodies = 100000; g.substeps = 0; g.step = 1.0f; g.clamp();
    CHECK(g.radius == 5.0f && g.max_bodies == 512 && g.substeps == 1 && g.step == 0.05f);
    CHECK(nearf(blast_falloff(0, 5), 1, 0) && nearf(blast_falloff(5, 5), 0, 0) && nearf(blast_falloff(2.5f, 5), 0.5f, 1e-6f));
    V3 bi = bullet_impulse(v3(0, 0, 0), v3(0, 0, 10), 20);
    CHECK(nearv(bi, v3(0, 0, 20), 1e-5f));
}

static void add_floor(const PhApi* A, float y) {   // a 40 x 40 m floor made like the game's terrain: two flat triangles
    V3 t1[3] = {v3(-20, y, -20), v3(20, y, -20), v3(-20, y, 20)}, t2[3] = {v3(20, y, -20), v3(20, y, 20), v3(-20, y, 20)};
    V3 out[6];
    int n = prepare_hull(t1, 3, 0.02f, 0.25f, out); CHECK(A->add_static_hull(&out[0].x, n, 0.6f) == 1);
    n = prepare_hull(t2, 3, 0.02f, 0.25f, out); CHECK(A->add_static_hull(&out[0].x, n, 0.6f) == 1);
}
static int add_box(const PhApi* A, V3 p, Q4 q, float h, bool asleep, void* user) {
    PhBodyDesc d; memset(&d, 0, sizeof d);
    d.pos[0] = p.x; d.pos[1] = p.y; d.pos[2] = p.z; d.rot[0] = q.x; d.rot[1] = q.y; d.rot[2] = q.z; d.rot[3] = q.w;
    d.lin_damping = 0.05f; d.ang_damping = 0.15f; d.asleep = asleep ? 1 : 0; d.user = user;
    int b = A->body_create(&d);
    V3 c[8]; for (int i = 0; i < 8; ++i) c[i] = v3((i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h);
    CHECK(A->body_add_hull(b, &c[0].x, 8, 120.0f, 0.6f, 0.1f) == 1);
    CHECK(A->body_finish(b, 3.0f, 120.0f) == 1);
    return b;
}

static void test_sim() {
    const PhApi* A = TODPhysics_GetApi(TODPHYS_API_VERSION);
    CHECK(A != nullptr); if (!A) return;
    CHECK(TODPhysics_GetApi(999) == nullptr);
    CHECK(sizeof(void*) == 4);   // the game is a 32-bit process; so are these tests
    CHECK(A->create(-9.81f, 0.0f) == 1);
    add_floor(A, 1.0f);
    Q4 id = {0, 0, 0, 1};
    int falling = add_box(A, v3(0, 4, 0), axis_angle(v3(1, 0, 1), 0.6f), 0.3f, false, (void*)0x1111);
    int sleeper = add_box(A, v3(5, 1.3f, 0), id, 0.3f, true, (void*)0x2222);
    float m = A->body_mass(falling);
    CHECK(nearf(m, 120.0f * 0.216f, 0.5f));   // 0.6^3 m^3 x 120 kg/m^3 = 25.9 kg
    double ms = 0; int steps = 0;
    for (int i = 0; i < 100; ++i) { ms += A->step(0.04f, 4); ++steps; }   // 4 s
    float p[3], r[4];
    int aw = A->body_get(falling, p, r);
    printf("  box after 4 s: y %.3f (rests at 1.30), awake %d, %.3f ms per 0.04 s step\n", p[1], aw, ms / steps);
    CHECK(nearf(p[1], 1.3f, 0.03f));
    CHECK(aw == 0);   // settled and asleep
    CHECK(A->body_get(sleeper, p, r) == 0); CHECK(nearf(p[0], 5.0f, 1e-4f) && nearf(p[1], 1.3f, 1e-4f));
    PhCounts c; A->counts(&c);
    CHECK(c.bodies == 2 && c.awake == 0 && c.static_shapes == 2 && c.static_failed == 0);
    // a bullet: the ray finds the sleeper, the impulse wakes and moves it
    float from[3] = {5, 1.3f, -10}, to[3] = {5, 1.3f, 10}, hit[3]; void* u = nullptr;
    int hb = A->raycast(from, to, hit, &u);
    CHECK(hb == sleeper); CHECK(u == (void*)0x2222); CHECK(nearf(hit[2], -0.3f, 0.02f));
    float imp[3] = {0, 0, 30};
    A->impulse(hb, imp, hit);
    for (int i = 0; i < 10; ++i) A->step(0.04f, 4);
    CHECK(A->body_get(sleeper, p, r) == 1); CHECK(p[2] > 0.05f);   // 30 N s on 26 kg: 1.2 m/s, friction stops it after ~0.1 m
    // a ray that misses, a ray at the floor only (statics are not reported)
    float f2[3] = {30, 5, 0}, t2[3] = {30, -5, 0};
    CHECK(A->raycast(f2, t2, hit, &u) < 0);
    // an explosion next to the resting box sends it flying
    A->body_get(falling, p, r);
    float y0 = p[1];
    float c0[3] = {p[0] + 1.0f, 1.1f, p[2]};
    int touched = A->explode(c0, 6.0f, 900.0f, 4.0f);
    CHECK(touched >= 1);
    float ymax = y0;
    for (int i = 0; i < 25; ++i) { A->step(0.04f, 4); A->body_get(falling, p, r); if (p[1] > ymax) ymax = p[1]; }
    printf("  explosion: box rose %.2f m, now x %.2f\n", ymax - y0, p[0]);
    CHECK(ymax - y0 > 0.5f); CHECK(p[0] < c0[0] - 1.0f);
    // it comes back down and settles on the floor again
    for (int i = 0; i < 200; ++i) A->step(0.04f, 4);
    A->body_get(falling, p, r); CHECK(p[1] > 1.2f && p[1] < 1.8f);
    // teleport (rewind resync) and removal
    float tp[3] = {-3, 1.3f, -3}; float tr[4] = {0, 0, 0, 1};
    A->body_set(sleeper, tp, tr, 0);
    A->body_get(sleeper, p, r); CHECK(nearf(p[0], -3, 1e-5f));
    A->body_remove(sleeper);
    CHECK(A->body_get(sleeper, p, r) == -1);
    A->counts(&c); CHECK(c.bodies == 1);
    // a body without shapes is refused, an empty hull is refused
    PhBodyDesc d; memset(&d, 0, sizeof d); d.rot[3] = 1;
    int e = A->body_create(&d);
    CHECK(A->body_add_hull(e, nullptr, 0, 100, 0.5f, 0.1f) == 0);
    CHECK(A->body_finish(e, 1, 10) == 0);
    // a pile: 40 boxes dropped on the floor, cost per step
    for (int i = 0; i < 40; ++i) add_box(A, v3(-8.0f + (i % 8) * 0.7f, 2.0f + (i / 8) * 0.8f, -6.0f), axis_angle(v3(0, 1, 0), i * 0.3f), 0.25f, false, nullptr);
    ms = 0; double mx = 0;
    for (int i = 0; i < 50; ++i) { double t = A->step(0.04f, 4); ms += t; if (t > mx) mx = t; }
    A->counts(&c);
    printf("  pile of 40: %.3f ms per step (max %.3f), %d bodies, %d awake\n", ms / 50, mx, c.bodies, c.awake);
    CHECK(c.bodies == 41);
    CHECK(ms / 50 < 10.0);
    // large world coordinates (Downtown is several hundred metres from the origin)
    CHECK(A->create(-9.81f, 0.0f) == 1);
    V3 t1[3] = {v3(-330, 0.3f, -20), v3(-290, 0.3f, -20), v3(-330, 0.3f, 20)}, out[6];
    int n = prepare_hull(t1, 3, 0.02f, 0.25f, out); CHECK(A->add_static_hull(&out[0].x, n, 0.6f) == 1);
    int farbox = add_box(A, v3(-320, 2.0f, -10), id, 0.3f, false, nullptr);
    for (int i = 0; i < 100; ++i) A->step(0.04f, 4);
    A->body_get(farbox, p, r); CHECK(nearf(p[1], 0.6f, 0.03f));
    A->destroy();
    A->counts(&c); CHECK(c.bodies == 0);
    CHECK(A->body_get(farbox, p, r) == -1);
}

// Regression (in-game runs p7 / p12): the Downtown City Hall plaza curb is one 22-point VertexCloud whose top points
// differ by a few mm; as a Box3D hull its near-coplanar faces let a crate fall through. The level is now a triangle mesh
// made from the hull's faces: the crate must rest on it (exact floats from the game, run p12).
static void test_plaza_regression() {
    static const float ring[22][3] = {
        {-222.306213f, 0.0237070024f, -15.6368427f}, {-222.306213f, 0.0237070024f, -57.2658539f}, {-224.106201f, 1.18659794f, -57.266922f},
        {-224.106201f, 1.18659794f, -15.6357765f}, {-226.27124f, 0.0237070024f, -61.2308807f}, {-224.288727f, 0.0237070024f, -60.6996689f},
        {-222.837433f, 0.0237070024f, -59.2483673f}, {-222.837433f, 0.0237070024f, -13.6543293f}, {-224.288727f, 0.0237070024f, -12.2030277f},
        {-226.27124f, 0.0237070024f, -11.671814f}, {-247.520859f, 0.0237070024f, -61.2308807f}, {-224.39679f, 1.18659794f, -58.3492966f},
        {-224.39679f, 1.18659794f, -14.5534f}, {-225.189651f, 1.18659794f, -13.7613297f}, {-226.272308f, 1.18659794f, -13.4718037f},
        {-248.051483f, 1.18659794f, -13.4718037f}, {-250.55397f, 1.18615401f, -14.5177155f}, {-225.189651f, 1.18659794f, -59.1413651f},
        {-226.272308f, 1.18659794f, -59.4308929f}, {-245.384506f, 1.18471897f, -58.4610939f}, {-247.661789f, 1.18659794f, -13.4718037f},
        {-250.022766f, 1.18615401f, -16.5002289f}};
    const PhApi* A = TODPhysics_GetApi(TODPHYS_API_VERSION);
    CHECK(A->create(-9.81f, 0.0f) == 1);
    CHECK(A->add_static_hull(&ring[0][0], 22, 0.6f) == 1);
    Q4 id = {0, 0, 0, 1};
    int b = add_box(A, v3(-234.89f, 2.59f, -43.26f), id, 0.6f, false, nullptr);
    for (int i = 0; i < 100; ++i) A->step(0.04f, 4);
    float p[3], r[4]; A->body_get(b, p, r);
    printf("  plaza curb regression: crate rests at y %.3f (top 1.187)\n", p[1]);
    CHECK(nearf(p[1], 1.787f, 0.03f));
    float f[3] = {-234.89f, 5.0f, -43.26f}, t[3] = {-234.89f, -5.0f, -43.26f}, h[3];
    CHECK(A->raycast_static(f, t, h) == 1 && nearf(h[1], 1.187f, 0.01f));
    A->destroy();
}

static void test_drive() {   // the world steps every frame while Ram is in a vehicle, from the game-ms clock
    DriveGate g;
    CHECK(g.advance(5000) == 0);                         // the first look only sets the base
    CHECK(g.advance(5016) == 16);                        // one 60 Hz frame
    CHECK(g.advance(5016) == 0);                         // paused / loading: the clock stands
    CHECK(g.advance(4000) == 0 && g.advance(4017) == 17);   // went back (rewind / load): a new base, then counts on
    CHECK(g.advance(9000) == 0 && g.advance(9020) == 20);   // a jump of seconds (a load): a new base
    DriveGate w; w.advance(0xFFFFFFF0u); CHECK(w.advance(0x00000010u) == 0x20);   // wraps without sign trouble
    // driving, the clock ran: step
    CHECK(drive_decision(true, false, 16, 0.016f, 1, false, true) == DRV_STEP);
    CHECK(drive_decision(false, false, 16, 0.016f, 1, false, true) == DRV_NONE);    // drive_step = 0
    CHECK(drive_decision(true, true, 16, 0.016f, 1, false, true) == DRV_NONE);     // the on-foot tick ran: it steps itself
    CHECK(drive_decision(true, false, 16, 0.016f, 1, false, false) == DRV_NONE);   // not in a vehicle (seat, script, menu)
    CHECK(drive_decision(true, false, 0, 0.016f, 2, false, true) == DRV_NONE);     // paused
    CHECK(drive_decision(true, false, 16, 0.016f, 2, false, true) == DRV_NONE);    // paused even if the clock moved
    CHECK(drive_decision(true, false, 16, 0.016f, 1, true, true) == DRV_NONE);     // cut-scene
    CHECK(drive_decision(true, false, 0, 0.016f, 3, false, true) == DRV_REWIND);   // rewind: no step, resync after
    CHECK(drive_decision(true, false, 0, 0.016f, 1, false, true) == DRV_NONE);     // the clock stood (loading): no step
    CHECK(drive_decision(true, false, 0, 0.0005f, 1, false, true) == DRV_STEP);    // deep slow motion: < 1 ms a frame
    CHECK(drive_decision(true, false, 0, 0.0f, 1, false, true) == DRV_NONE);
    // the scan point leads the car on the ground plane, capped
    CHECK(nearv(drive_scan_point(v3(1, 2, 3), v3(0, 0, 0), 0.8f, 16.0f), v3(1, 2, 3), 1e-6f));
    CHECK(nearv(drive_scan_point(v3(0, 0, 0), v3(10, 5, 0), 0.8f, 16.0f), v3(8, 0, 0), 1e-5f));
    CHECK(nearv(drive_scan_point(v3(0, 0, 0), v3(0, 0, 40), 0.8f, 16.0f), v3(0, 0, 16), 1e-4f));
    Cfg c; CHECK(c.drive_step && nearf(c.drive_lead, 0.8f, 1e-6f));
    c.drive_lead = 9.0f; c.clamp(); CHECK(nearf(c.drive_lead, 3.0f, 1e-6f));
}

// hang-watch: a NaN / inf box made Box3D's tree descent (b3FindBestSibling) walk into a leaf and loop forever - the
// game thread hung with the frame frozen. Before the dynamic_tree.c patch 17 of 200 of these trees never returned.
// Each run is on a worker thread with a time limit, so a regression fails here instead of hanging the build.
static unsigned g_trng = 1;
static float trnd() { g_trng = g_trng * 1664525u + 1013904223u; return (float)(g_trng >> 8) / 16777216.0f; }
static volatile LONG g_tree_seed = 0, g_tree_done = 0;
static DWORD WINAPI tree_worker(void*) {
    for (int seed = 0; seed < 400; ++seed) {
        g_tree_seed = seed;
        g_trng = (unsigned)seed * 2654435761u + 7;
        b3DynamicTree t = b3DynamicTree_Create(16);
        int n = 2 + (int)(trnd() * 60);
        for (int i = 0; i < n; ++i) {
            float x = trnd() * 50, y = trnd() * 5, z = trnd() * 50, s = 0.2f + trnd() * 4;
            b3AABB a = {{x, y, z}, {x + s, y + s, z + s}};
            b3DynamicTree_CreateProxy(&t, a, 1, (uint64_t)i);
        }
        float bad = (seed & 1) ? NAN : INFINITY;
        b3AABB b = {{-bad, -bad, -bad}, {bad, bad, bad}};
        if (seed & 2) { b.lowerBound.x = 1; b.upperBound.x = bad; }
        b3DynamicTree_CreateProxy(&t, b, 1, 999);
        for (int i = 0; i < 30; ++i) {
            float x = trnd() * 50, z = trnd() * 50;
            b3AABB a = {{x, 1, z}, {x + 1, 2, z + 1}};
            b3DynamicTree_CreateProxy(&t, a, 1, (uint64_t)(100 + i));
            if (i == 10) b3DynamicTree_Rebuild(&t, true);
        }
        b3DynamicTree_Destroy(&t);
    }
    InterlockedExchange(&g_tree_done, 1);
    return 0;
}
static void test_nan_no_hang() {
    HANDLE h = CreateThread(nullptr, 0, tree_worker, nullptr, 0, nullptr);
    DWORD w = WaitForSingleObject(h, 20000);
    CHECK(w == WAIT_OBJECT_0 && g_tree_done == 1);
    if (w != WAIT_OBJECT_0) { printf("  tree insert with a NaN box STUCK at seed %ld\n", (long)g_tree_seed); return; }   // the process exit ends it
    CloseHandle(h);
    // the world never sees a non-finite float: NaN / inf positions, rotations, hull points and rays
    const PhApi* A = TODPhysics_GetApi(TODPHYS_API_VERSION);
    if (!A) return;
    CHECK(A->create(-9.81f, 0.0f) == 1);
    add_floor(A, 0.0f);
    float nanf_ = NAN, inf = INFINITY;
    Q4 id = {0, 0, 0, 1};
    int good = add_box(A, v3(0, 2, 0), id, 0.3f, false, (void*)1);
    PhBodyDesc d; memset(&d, 0, sizeof d);
    d.pos[0] = nanf_; d.pos[1] = inf; d.pos[2] = -inf; d.rot[0] = nanf_; d.rot[3] = inf;
    int bad = A->body_create(&d);
    CHECK(bad >= 0);
    float hull[24]; for (int i = 0; i < 8; ++i) { hull[3 * i] = (i & 1) ? 0.3f : -0.3f; hull[3 * i + 1] = (i & 2) ? 0.3f : -0.3f; hull[3 * i + 2] = (i & 4) ? 0.3f : -0.3f; }
    hull[5] = nanf_;
    A->body_add_hull(bad, hull, 8, 120.0f, 0.6f, 0.1f);
    A->body_finish(bad, 3.0f, 120.0f);
    float shull[12] = {1, 0, 1, 2, 0, 1, 1, 1, inf, 1, nanf_, 2};
    A->add_static_hull(shull, 4, 0.6f);
    float pp[3] = {nanf_, nanf_, nanf_}, rr[4] = {nanf_, 0, 0, 1};
    A->body_set(good, pp, rr, 1);
    float from[3] = {nanf_, 1, 0}, to[3] = {0, inf, 0}, hit[3]; void* u = nullptr;
    A->raycast(from, to, hit, &u); A->raycast_static(from, to, hit);
    float c[3] = {inf, 0, nanf_}; A->explode(c, 6.0f, 900.0f, 4.0f);
    for (int i = 0; i < 20; ++i) A->step(0.04f, 4);
    float p[3], r[4];
    bool finite = true;
    int bodies[2] = {good, bad};
    for (int b : bodies) {
        if (A->body_get(b, p, r) < 0) continue;
        bool f = true; for (int i = 0; i < 3; ++i) f = f && isfinite(p[i]) && isfinite(r[i]);
        if (!f) printf("  body %d after NaN input: pos %g %g %g rot %g %g %g %g\n", b, p[0], p[1], p[2], r[0], r[1], r[2], r[3]);
        finite = finite && f;
    }
    CHECK(finite);
    A->destroy();
}


// hull-hang (MM030 freeze): Box3D's b3HullBuilder_ConnectFaces walked a ring of shared edges with no bound; on a
// degenerate cloud (nearly collinear points, a flat patch far from the origin) the faces share every edge, the walk
// goes round and round, and the game thread never returned (hull_hang patch in third_party/box3d/src/hull.c). The
// three inputs below are real hangs of the ORIGINAL builder found by physics/tools/hull_fuzz.bat (which runs the original
// hull.c next to the patched one and checks that it still hangs there). Here: the patched builder returns promptly,
// make_hull falls back to a box, and random degenerate clouds always terminate.
extern "C" int b3HullBuilderGaveUp;
static const float k_collinear5[] = {
    772.978943f, -45.1926537f, -1793.2699f,
    779.884827f, -50.4212799f, -1797.63013f,
    779.945557f, -50.4672585f, -1797.66846f,
    776.379089f, -47.7669983f, -1795.41675f,
    779.880005f, -50.4250526f, -1797.6333f};
static const float k_collinear_slab10[] = {
    870.20105f, -70.9181061f, 1528.50232f,
    868.540771f, -65.5087128f, 1534.48645f,
    867.06543f, -60.7019806f, 1539.80396f,
    866.771118f, -59.7431412f, 1540.86475f,
    870.20105f, -70.9182281f, 1528.50244f,
    870.059082f, -71.0888214f, 1528.61731f,
    868.398804f, -65.6794281f, 1534.60144f,
    866.923462f, -60.8726921f, 1539.91895f,
    866.62915f, -59.9138527f, 1540.97974f,
    870.059082f, -71.0889435f, 1528.61743f};
static const float k_flat_far_slab8[] = {
    1103.41797f, -24642.7148f, -746.524597f,
    48602.0312f, -24642.6973f, -22013.2402f,
    24642.7207f, -24642.7031f, 0.0149112754f,
    2242.11133f, -24642.7188f, -21281.3711f,
    1103.41797f, -24642.9648f, -746.524597f,
    48602.0312f, -24642.9473f, -22013.2402f,
    24642.7207f, -24642.9531f, 0.014911334f,
    2242.11133f, -24642.9688f, -21281.3711f};

static const float* g_hh_pts = nullptr; static int g_hh_n = 0; static b3HullData* g_hh_h = nullptr; static volatile LONG g_hh_done = 0;
static DWORD WINAPI hh_worker(void*) {
    g_hh_h = b3CreateHull((const b3Vec3*)g_hh_pts, g_hh_n, 32);
    InterlockedExchange(&g_hh_done, 1);
    return 0;
}
static volatile LONG g_fz_done = 0, g_fz_cases = 0, g_fz_hulls = 0, g_fz_null = 0;
static DWORD WINAPI fuzz_worker(void*) {
    for (int seed = 0; seed < 6000; ++seed) {
        g_trng = (unsigned)seed * 2654435761u + 99;
        float ox = 780 + trnd() * 40, oy = -45 + trnd() * 10, oz = -1790 + trnd() * 40;
        b3Vec3 p[96]; int n = 0;
        switch (seed % 5) {
        case 0: {   // nearly collinear: a line plus a sub-millimetre scatter (a pole, a wire, a rail)
            n = 4 + (int)(trnd() * 30); b3Vec3 d = {trnd() - 0.5f, trnd() - 0.5f, trnd() - 0.5f}; float e = (seed % 3) ? 1e-4f : 1e-3f;
            for (int i = 0; i < n; ++i) { float t = trnd() * 20 - 10; p[i] = {ox + d.x * t + (trnd() - 0.5f) * e, oy + d.y * t + (trnd() - 0.5f) * e, oz + d.z * t + (trnd() - 0.5f) * e}; }
            break; }
        case 1: {   // a few distinct points repeated
            int m = 4 + (int)(trnd() * 4); b3Vec3 b[8];
            for (int i = 0; i < m; ++i) b[i] = {ox + trnd() * 3, oy + trnd() * 3, oz + trnd() * 3};
            n = m + (int)(trnd() * 40); if (n > 96) n = 96;
            for (int i = 0; i < n; ++i) p[i] = b[(int)(trnd() * m) % m];
            break; }
        case 2: {   // a flat patch twice (a slab of thickness 0..1 mm), mm noise
            int m = 3 + (int)(trnd() * 12); float th = (seed % 2) ? 0.0f : trnd() * 1e-3f;
            n = 2 * m;
            for (int i = 0; i < m; ++i) { float a = trnd() * 8, b = trnd() * 8, h = (trnd() - 0.5f) * 2e-3f; p[i] = {ox + a, oy + h, oz + b}; p[m + i] = {ox + a, oy + h - th, oz + b}; }
            break; }
        case 3: {   // a small integer lattice
            n = 4 + (int)(trnd() * 40);
            for (int i = 0; i < n; ++i) p[i] = {ox + (float)(int)(trnd() * 3), oy + (float)(int)(trnd() * 3), oz + (float)(int)(trnd() * 3)};
            break; }
        default: {  // far from the origin, tiny extent
            n = 4 + (int)(trnd() * 30); float big = 1000.0f + trnd() * 40000.0f;
            for (int i = 0; i < n; ++i) p[i] = {big + trnd() * 1e-3f, -big + trnd() * 1e-3f, trnd() * ((i % 5) ? 1e-3f : big)};
            break; }
        }
        b3HullData* h = b3CreateHull(p, n, 32);
        InterlockedIncrement(&g_fz_cases);
        if (h) { InterlockedIncrement(&g_fz_hulls); b3DestroyHull(h); } else InterlockedIncrement(&g_fz_null);
    }
    InterlockedExchange(&g_fz_done, 1);
    return 0;
}
static void test_hull_hang() {
    struct { const float* p; int n; const char* name; } fx[] = {{k_collinear5, 5, "collinear5"}, {k_collinear_slab10, 10, "collinear_slab10"}, {k_flat_far_slab8, 8, "flat_far_slab8"}};
    for (auto& f : fx) {
        g_hh_pts = f.p; g_hh_n = f.n; g_hh_h = nullptr; g_hh_done = 0;
        int before = b3HullBuilderGaveUp;
        HANDLE h = CreateThread(nullptr, 0, hh_worker, nullptr, 0, nullptr);
        DWORD w = WaitForSingleObject(h, 5000);
        CHECK(w == WAIT_OBJECT_0 && g_hh_done == 1);
        if (w != WAIT_OBJECT_0) { printf("  hull builder STUCK on %s\n", f.name); return; }   // the process exit ends it
        CloseHandle(h);
        CHECK(g_hh_h == nullptr);                      // it gives up instead of spinning
        CHECK(b3HullBuilderGaveUp == before + 1);      // ... and counts it
        if (g_hh_h) b3DestroyHull(g_hh_h);
    }
    // through the game's function table: the static hull and a body hull both come back as a box (the builder gave up
    // twice per call, on the points and on the 5 mm grid copy)
    const PhApi* A = TODPhysics_GetApi(TODPHYS_API_VERSION);
    if (!A) return;
    CHECK(A->create(-9.81f, 0.0f) == 1);
    PhCounts c0; A->counts(&c0);
    // (the far slab is 48 km wide, far outside any level: no hang is what counts, a refused shape is fine)
    for (int i = 0; i < 3; ++i) {
        int ok = A->add_static_hull(fx[i].p, fx[i].n, 0.6f);
        if (i < 2) CHECK(ok == 1);
        if (i < 2 && ok != 1) printf("  add_static_hull refused %s\n", fx[i].name);
    }
    PhBodyDesc d; memset(&d, 0, sizeof d); d.rot[3] = 1; d.pos[1] = 5;
    int b = A->body_create(&d); CHECK(b >= 0);
    CHECK(A->body_add_hull(b, k_collinear5, 5, 120.0f, 0.6f, 0.1f) == 1);
    CHECK(A->body_finish(b, 3.0f, 120.0f) == 1);
    PhCounts c1; A->counts(&c1);
    CHECK(c1.hull_gave_up - c0.hull_gave_up >= 6);
    CHECK(c1.hull_boxes - c0.hull_boxes >= 1);
    for (int i = 0; i < 10; ++i) A->step(0.04f, 4);
    A->destroy();
    // random degenerate clouds always terminate (6000 builds, limit 60 s; the original hangs on about 2 %)
    HANDLE h = CreateThread(nullptr, 0, fuzz_worker, nullptr, 0, nullptr);
    DWORD w = WaitForSingleObject(h, 60000);
    CHECK(w == WAIT_OBJECT_0 && g_fz_done == 1);
    if (w != WAIT_OBJECT_0) { printf("  degenerate-cloud fuzz STUCK after %ld cases\n", (long)g_fz_cases); return; }
    CloseHandle(h);
    CHECK(g_fz_cases == 6000);
    printf("  hull fuzz: %ld builds, %ld hulls, %ld given up / degenerate\n", (long)g_fz_cases, (long)g_fz_hulls, (long)g_fz_null);
}

int main() {
    test_math(); test_hull(); test_names(); test_clock(); test_sim(); test_plaza_regression(); test_drive(); test_nan_no_hang(); test_hull_hang();
    printf("test_phys: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
