// test_car.cpp - unit tests of [car_physics]: car_logic (boxes, sweep targets, blast / push / self kicks, velocities)
// and phys_car_sim (kinematic car boxes pushing Box3D props, wreck bodies, touch reports) through the same function
// tables TODPhysics.dll exports, as 32-bit. Built and run by dll/build.bat.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "car_logic.h"
#include "phys_api.h"
#include "car_api.h"
extern "C" const PhApi* __cdecl TODPhysics_GetApi(int version);
extern "C" const PhCarApi* __cdecl TODPhysics_GetCarApi(int version);

using namespace ph;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool nearf(float a, float b, float e) { return fabsf(a - b) <= e; }
static bool nearv(V3 a, V3 b, float e) { return nearf(a.x, b.x, e) && nearf(a.y, b.y, e) && nearf(a.z, b.z, e); }

static void test_cfg() {
    cp::Cfg c;
    CHECK(!c.enabled && c.push_props && c.blast_cars && c.wreck_dynamic && c.impulse_scale == 1.0f);
    c.impulse_scale = -3; c.max_cars = 500; c.margin = 9; c.blast_spin = -100; c.wreck_mass = 5;
    c.clamp();
    CHECK(c.impulse_scale == 0.0f); CHECK(c.max_cars == 64); CHECK(c.margin == 1.0f); CHECK(c.blast_spin == -30.0f); CHECK(c.wreck_mass == 100.0f);
    volatile float zero = 0.0f;
    cp::Cfg n; n.impulse_scale = zero / zero; n.clamp(); CHECK(n.impulse_scale == 0.0f);   // NaN from a bad ini
}

static void test_box() {
    V3 pts[8];
    for (int i = 0; i < 8; ++i) pts[i] = v3((i & 1) ? 0.9f : -0.9f, (i & 2) ? 1.5f : 0.2f, (i & 4) ? 2.4f : -2.2f);
    cp::Box b = cp::box_from_points(pts, 8, 0.1f);
    CHECK(b.ok);
    CHECK(nearv(b.half, v3(1.0f, 0.65f, 2.4f), 1e-5f));
    CHECK(nearv(b.center, v3(0.0f, 0.85f, 0.1f), 1e-5f));
    CHECK(!cp::box_from_points(pts, 3, 0.1f).ok);                    // too few points
    V3 flat[4] = {v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), v3(1, 0, 1)};
    CHECK(!cp::box_from_points(flat, 4, 0.1f).ok);                   // flat: not a car
    V3 huge[4] = {v3(-30, 0, 0), v3(30, 1, 0), v3(0, 0, 1), v3(1, 2, 1)};
    CHECK(!cp::box_from_points(huge, 4, 0.1f).ok);                   // 60 m: not a car
    cp::Box d = cp::default_box(); CHECK(d.ok && d.half.z > d.half.x);
}

static Q4 yawq(float a) { Q4 q = {0.0f, sinf(a * 0.5f), 0.0f, cosf(a * 0.5f)}; return q; }

static void test_sweep() {
    cp::Pose a = {v3(0, 0, 0), yawq(0)}, b = {v3(4, 0, 0), yawq(1.0f)};
    cp::Pose m = cp::sweep_target(a, b, 0.5f, v3(10, 0, 0), 0.04f);
    CHECK(nearv(m.p, v3(2.4f, 0, 0), 1e-5f));                        // half way + 0.4 m lead
    V3 fw = qrot(m.q, v3(0, 0, 1)); CHECK(nearf(atan2f(fw.x, fw.z), 0.5f, 1e-4f));
    CHECK(nearv(cp::sweep_target(a, b, 2.0f, v3(0, 0, 0), 0).p, b.p, 1e-5f));   // t clamped
    CHECK(cp::teleported(v3(0, 0, 0), v3(7, 0, 0), 6.0f)); CHECK(!cp::teleported(v3(0, 0, 0), v3(5, 0, 0), 6.0f));
    CHECK(nearv(cp::lin_vel(v3(1, 2, 3), v3(2, 2, 1), 0.5f), v3(2, 0, -4), 1e-5f));
    V3 w = cp::ang_vel(yawq(0.2f), yawq(0.5f), 0.1f);
    CHECK(nearv(w, v3(0, 3.0f, 0), 1e-3f));
    Q4 rx = {sinf(0.05f), 0, 0, cosf(0.05f)};                         // 0.1 rad about x in 0.05 s
    CHECK(nearv(cp::ang_vel(yawq(0.7f), qmul(rx, yawq(0.7f)), 0.05f), v3(2.0f, 0, 0), 1e-3f));
}

static void test_blast() {
    cp::Cfg g;
    // shell numbers of [physics]: radius 7, 450 per area, lift 4; a 750 kg sedan 2 m east of the blast
    cp::Kick k = cp::blast_kick(v3(2, 0.8f, 0), 750.0f, v3(0, 0.2f, 0), 7.0f, 450.0f, 4.0f, g, 1);
    CHECK(k.hit);
    float f = 1.0f - sqrtf(4.0f + 0.36f) / 10.5f;
    CHECK(nearf(k.k, f, 1e-4f));
    CHECK(k.dv.x > 5.0f && fabsf(k.dv.z) < 1e-4f);                  // away from the blast
    CHECK(nearf(k.dv.x, 2.0f * 450.0f * 8.0f / 750.0f * f, 1e-3f));
    CHECK(k.dv.y > 4.0f);                                            // and up
    // tip over away from it: the west (near) side rises -> rotation about -z
    CHECK(k.dw.z < -1.0f && fabsf(k.dw.x) < 1e-4f);
    V3 west = v3(-1, 0, 0), next = add(west, mul(cross(k.dw, west), 0.05f));
    CHECK(next.y > 0.0f);                                            // the near side moves up
    // out of reach, heavier car, impulse_scale
    CHECK(!cp::blast_kick(v3(11, 0, 0), 750.0f, v3(0, 0, 0), 7.0f, 450.0f, 4.0f, g, 1).hit);
    cp::Kick heavy = cp::blast_kick(v3(2, 0.8f, 0), 3000.0f, v3(0, 0.2f, 0), 7.0f, 450.0f, 4.0f, g, 1);
    CHECK(heavy.dv.x < k.dv.x * 0.3f);
    cp::Cfg g2; g2.impulse_scale = 0.5f;
    cp::Kick half = cp::blast_kick(v3(2, 0.8f, 0), 750.0f, v3(0, 0.2f, 0), 7.0f, 450.0f, 4.0f, g2, 1);
    CHECK(nearf(half.dv.x, k.dv.x * 0.5f, 1e-3f)); CHECK(nearf(half.dw.z, k.dw.z * 0.5f, 1e-3f));
    // right under the car: still a side (from the seed), capped
    cp::Kick under = cp::blast_kick(v3(0, 0.8f, 0), 600.0f, v3(0, 0.3f, 0), 7.0f, 450.0f, 4.0f, g, 7);
    CHECK(under.hit && len(v3(under.dv.x, 0, under.dv.z)) > 1.0f);
    CHECK(len(under.dv) <= g.max_speed + 1e-3f && len(under.dw) <= g.max_spin + 1e-3f);
    // the dramatic default: a sedan 2 m from a grenade leaves at more than 10 m/s, spinning more than 2 rad/s
    CHECK(len(k.dv) > 10.0f && len(k.dw) > 2.0f);
}

static void test_push_self() {
    cp::Cfg g;
    cp::Kick slow = cp::push_kick(v3(1.0f, 0, 0), v3(1, 0, 0), g, 1); CHECK(!slow.hit);
    cp::Kick k = cp::push_kick(v3(15, 0, 0), v3(0.8f, 0.0f, 0.6f), g, 2);
    CHECK(k.hit && nearf(k.k, 1.0f, 1e-5f));
    CHECK(k.dv.x > 15.0f * g.push_gain * 0.99f);                     // forward with the car
    CHECK(k.dv.z > 1.0f);                                            // out to the side the contact faces
    CHECK(nearf(k.dv.y, g.push_lift, 1e-4f));                        // over the bonnet
    CHECK(len(k.dw) > 1.0f);
    cp::Kick k6 = cp::push_kick(v3(0, 0, 6), v3(0, 0, 1), g, 3);
    CHECK(nearf(k6.dv.y, g.push_lift * 0.5f, 1e-4f));                // half lift at half of push_full_speed
    cp::Kick s = cp::self_kick(g, 9);
    CHECK(s.hit && nearf(s.dv.y, g.self_kick, 1e-4f) && len(s.dw) > 1.0f);
    cp::Cfg off; off.self_kick = 0; off.self_spin = 0; CHECK(!cp::self_kick(off, 9).hit);
    cp::Cfg kick7; kick7.self_kick = 7; CHECK(nearf(cp::self_kick(kick7, 9).dv.y, 7.0f, 1e-4f));
}

// Box3D: a crate on the floor, a kinematic car box driven through it at 12 m/s -> the crate flies off ahead; a touch is
// reported; a wreck box falls, lands and sleeps on the floor
static void test_sim() {
    const PhApi* A = TODPhysics_GetApi(TODPHYS_API_VERSION);
    const PhCarApi* C = TODPhysics_GetCarApi(TODPHYS_CAR_API_VERSION);
    CHECK(A != nullptr && C != nullptr);
    if (!A || !C) return;
    CHECK(TODPhysics_GetCarApi(99) == nullptr);
    CHECK(A->create(-9.81f, 0.0f) == 1);
    float floor[8 * 3]; int k = 0;
    for (int i = 0; i < 8; ++i) { floor[k++] = (i & 1) ? 60.0f : -60.0f; floor[k++] = (i & 2) ? 0.0f : -0.5f; floor[k++] = (i & 4) ? 60.0f : -60.0f; }
    CHECK(A->add_static_hull(floor, 8, 0.6f) == 1);
    PhBodyDesc d; memset(&d, 0, sizeof d);
    d.pos[0] = 10; d.pos[1] = 0.31f; d.pos[2] = 0; d.rot[3] = 1; d.lin_damping = 0.05f; d.ang_damping = 0.15f; d.asleep = 1;
    int crate = A->body_create(&d);
    float cube[8 * 3]; k = 0;
    for (int i = 0; i < 8; ++i) { cube[k++] = (i & 1) ? 0.3f : -0.3f; cube[k++] = (i & 2) ? 0.3f : -0.3f; cube[k++] = (i & 4) ? 0.3f : -0.3f; }
    CHECK(A->body_add_hull(crate, cube, 8, 40.0f, 0.6f, 0.15f) == 1);
    CHECK(A->body_finish(crate, 3.0f, 80.0f) == 1);
    float half[3] = {0.95f, 0.7f, 2.3f}, off[3] = {0, 0.8f, 0}, pos[3] = {0, 0, 0}, rot[4] = {0, 0.70710678f, 0, 0.70710678f};   // +z along +x
    int car = C->kin_create(half, off, pos, rot, (void*)0x1234);
    CHECK(car >= 0 && C->kind(car) == 1 && C->kind(crate) == 0 && C->kind(9999) == -1);
    PhCounts pc; A->counts(&pc); CHECK(pc.bodies == 1);               // a kinematic car is not counted (rescans)
    cp::Cfg g;
    int touches = 0, kicks = 0; float x = 0;
    for (int s = 0; s < 40; ++s) {
        x += 12.0f * 0.04f;
        float p[3] = {x, 0, 0};
        C->kin_move(car, p, rot, 0.04f, 0);
        A->step(0.04f, 4);
        PhTouch t[8]; int n = C->touches(t, 8);
        for (int i = 0; i < n; ++i) {
            ++touches;
            CHECK(t[i].car == car && t[i].prop == crate);
            CHECK(t[i].normal[0] > 0.5f);                               // from the car toward the crate
            cp::Kick kk = cp::push_kick(v3(12, 0, 0), v3(t[i].normal[0], t[i].normal[1], t[i].normal[2]), g, 5);
            float m = A->body_mass(crate), imp[3] = {kk.dv.x * m, kk.dv.y * m, kk.dv.z * m};
            A->impulse(crate, imp, t[i].point);
            ++kicks;
        }
    }
    float cp3[3], cq[4]; A->body_get(crate, cp3, cq);
    CHECK(touches >= 1 && kicks >= 1);
    CHECK(cp3[0] > x + 1.0f);                                          // thrown ahead of the car
    float carp[3], carq[4]; A->body_get(car, carp, carq);
    CHECK(nearf(carp[0], x, 0.05f) && nearf(carp[1], 0.0f, 0.01f));     // the kinematic box went exactly where it was sent
    C->kin_move(car, pos, rot, 0.0f, 1);                               // teleport
    A->body_get(car, carp, carq); CHECK(nearf(carp[0], 0.0f, 1e-4f));
    // wreck: a 1400 kg box dropped from 3 m with a spin lands, the crate does not stop it, it sleeps
    float wp[3] = {-10, 3, 0}, wr[4] = {0, 0, 0, 1}, wh[3] = {0.9f, 0.7f, 2.2f}, wo[3] = {0, 0.7f, 0};
    int wreck = C->wreck_create(wh, wo, wp, wr, 1400.0f, 0.7f, 0.1f, 0.05f, 0.25f, nullptr);
    CHECK(wreck >= 0 && C->kind(wreck) == 2);
    CHECK(nearf(A->body_mass(wreck), 1400.0f, 1.0f));
    float lv[3] = {2, 6, 0}, av[3] = {0, 0, 4};
    C->set_vel(wreck, lv, av);
    A->counts(&pc); CHECK(pc.bodies == 2 && pc.awake >= 1);          // a wreck in flight holds rescans
    int slept = -1;
    for (int s = 0; s < 400 && slept < 0; ++s) { A->step(0.04f, 4); float p[3], q[4]; if (A->body_get(wreck, p, q) == 0) slept = s; }
    float p[3], q[4]; A->body_get(wreck, p, q);
    CHECK(slept > 0);
    CHECK(p[1] > 0.0f && p[1] < 2.0f);                                 // on the floor, not through it
    // a bullet ray finds the wreck, not the crate or the car
    float f[3] = {p[0], 6.0f, p[2]}, t[3] = {p[0], -1.0f, p[2]}, h[3];
    CHECK(C->raycast_wreck(f, t, h) == wreck);
    float f2[3] = {cp3[0], 5.0f, cp3[2]}, t2[3] = {cp3[0], -1.0f, cp3[2]};
    CHECK(C->raycast_wreck(f2, t2, h) == -1);
    // a [physics] blast lifts the crate but never kicks the kinematic car or the wreck through the props' lift path
    float c3[3] = {0, 0.2f, 0};
    A->explode(c3, 7.0f, 450.0f, 4.0f);
    A->body_get(car, carp, carq); CHECK(nearf(carp[0], 0.0f, 1e-4f));
    A->destroy();
    CHECK(C->kin_create(half, off, pos, rot, nullptr) == -1);          // no world
}

int main() {
    test_cfg();
    test_box();
    test_sweep();
    test_blast();
    test_push_self();
    test_sim();
    printf("test_car: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
