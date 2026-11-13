// car_logic.cpp - pure logic of [car_physics] (see car_logic.h). No game memory, no Box3D.
#include "car_logic.h"
#include <math.h>

namespace cp {
using namespace ph;

static float clampf(float v, float lo, float hi) { return v != v ? lo : (v < lo ? lo : (v > hi ? hi : v)); }

void Cfg::clamp() {
    impulse_scale = clampf(impulse_scale, 0.0f, 10.0f);
    margin = clampf(margin, 0.0f, 1.0f);
    lead = clampf(lead, 0.0f, 0.2f);
    push_gain = clampf(push_gain, 0.0f, 3.0f);
    push_lift = clampf(push_lift, 0.0f, 20.0f);
    push_full_speed = clampf(push_full_speed, 1.0f, 60.0f);
    push_min_speed = clampf(push_min_speed, 0.0f, 20.0f);
    push_spin = clampf(push_spin, 0.0f, 30.0f);
    blast_gain = clampf(blast_gain, 0.0f, 20.0f);
    blast_area = clampf(blast_area, 0.5f, 40.0f);
    blast_min_mass = clampf(blast_min_mass, 50.0f, 20000.0f);
    blast_max_mass = clampf(blast_max_mass, blast_min_mass, 50000.0f);
    blast_lift_gain = clampf(blast_lift_gain, 0.0f, 10.0f);
    blast_spin = clampf(blast_spin, -30.0f, 30.0f);
    blast_reach = clampf(blast_reach, 0.5f, 4.0f);
    max_speed = clampf(max_speed, 0.0f, 80.0f);
    max_spin = clampf(max_spin, 0.0f, 40.0f);
    self_kick = clampf(self_kick, 0.0f, 40.0f);
    self_spin = clampf(self_spin, 0.0f, 30.0f);
    car_blast_radius = clampf(car_blast_radius, 0.0f, 40.0f);
    car_blast_impulse = clampf(car_blast_impulse, 0.0f, 5000.0f);
    wreck_mass = clampf(wreck_mass, 100.0f, 20000.0f);
    wreck_friction = clampf(wreck_friction, 0.0f, 2.0f);
    wreck_restitution = clampf(wreck_restitution, 0.0f, 1.0f);
    wreck_linear_damping = clampf(wreck_linear_damping, 0.0f, 5.0f);
    wreck_angular_damping = clampf(wreck_angular_damping, 0.0f, 5.0f);
    wreck_bullet_gain = clampf(wreck_bullet_gain, 0.0f, 200.0f);
    teleport_distance = clampf(teleport_distance, 1.0f, 100.0f);
    car_rescan_seconds = clampf(car_rescan_seconds, 0.5f, 60.0f);
    car_radius = clampf(car_radius, 10.0f, 300.0f);
    if (max_cars < 1) max_cars = 1;
    if (max_cars > 64) max_cars = 64;
}

Box box_from_points(const V3* pts, int n, float margin) {
    Box b; b.ok = false; b.half = v3(0, 0, 0); b.center = v3(0, 0, 0);
    if (!pts || n < 4) return b;
    V3 lo = pts[0], hi = pts[0];
    for (int i = 1; i < n; ++i) {
        const V3& p = pts[i];
        if (!(p.x == p.x && p.y == p.y && p.z == p.z)) return b;
        lo.x = fminf(lo.x, p.x); lo.y = fminf(lo.y, p.y); lo.z = fminf(lo.z, p.z);
        hi.x = fmaxf(hi.x, p.x); hi.y = fmaxf(hi.y, p.y); hi.z = fmaxf(hi.z, p.z);
    }
    V3 h = mul(sub(hi, lo), 0.5f);
    // a car: at least 0.5 m wide / tall / long, at most a bus (20 m)
    if (h.x < 0.25f || h.y < 0.25f || h.z < 0.25f || h.x > 10.0f || h.y > 10.0f || h.z > 10.0f) return b;
    b.center = mul(add(lo, hi), 0.5f);
    b.half = v3(h.x + margin, h.y, h.z + margin);
    b.ok = true;
    return b;
}
Box default_box() { Box b; b.half = v3(0.95f, 0.75f, 2.3f); b.center = v3(0.0f, 0.8f, 0.0f); b.ok = true; return b; }

Pose sweep_target(const Pose& prev, const Pose& cur, float t, V3 v, float lead) {
    t = clampf(t, 0.0f, 1.0f);
    Pose r;
    r.p = add(add(prev.p, mul(sub(cur.p, prev.p), t)), mul(v, lead));
    r.q = qslerp(prev.q, cur.q, t);
    return r;
}
bool teleported(V3 a, V3 b, float max_dist) { return len(sub(b, a)) > max_dist; }

unsigned hash(unsigned x) {   // lowbias32
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
static float rnd(unsigned seed, unsigned k) { return (float)(hash(seed * 2654435761U + k) & 0xFFFF) / 65535.0f * 2.0f - 1.0f; }

static V3 cap(V3 v, float m) { float l = len(v); return (l > m && l > 0.0f) ? mul(v, m / l) : v; }

Kick blast_kick(V3 car, float mass, V3 c, float radius, float ipa, float lift, const Cfg& g, unsigned seed) {
    Kick k; k.dv = v3(0, 0, 0); k.dw = v3(0, 0, 0); k.k = 0.0f; k.hit = false;
    float reach = radius * g.blast_reach;
    if (!(reach > 0.0f)) return k;
    V3 d = sub(car, c);
    float dist = len(d);
    if (!(dist < reach)) return k;
    float f = 1.0f - dist / reach;
    V3 dir = norm(v3(d.x, 0.0f, d.z));
    if (len(dir) < 0.5f) {   // right under or over the car: a side from the seed
        float a = rnd(seed, 1) * 3.14159265f;
        dir = v3(cosf(a), 0.0f, sinf(a));
    }
    float m = clampf(mass, g.blast_min_mass, g.blast_max_mass);
    float s = g.impulse_scale;
    float side = s * g.blast_gain * ipa * g.blast_area / m * f;
    float up = s * (lift * g.blast_lift_gain * f) + 0.35f * side;
    k.dv = cap(add(mul(dir, side), v3(0.0f, up, 0.0f)), g.max_speed);
    // tip over away from the blast: the near side rises. Axis = up x dir (blast at -x, car at +x: the car's -x side up)
    V3 axis = cross(v3(0, 1, 0), dir);
    V3 w = mul(axis, s * g.blast_spin * f);
    w = add(w, v3(0.0f, s * g.blast_spin * 0.3f * f * rnd(seed, 2), 0.0f));   // a little yaw
    k.dw = cap(w, g.max_spin);
    k.k = f; k.hit = true;
    return k;
}

Kick self_kick(const Cfg& g, unsigned seed) {
    Kick k; k.k = 1.0f; k.hit = g.self_kick > 0.0f || g.self_spin > 0.0f;
    float s = g.impulse_scale;
    float a = rnd(seed, 3) * 3.14159265f;
    V3 side = v3(cosf(a), 0.0f, sinf(a));
    k.dv = cap(add(v3(0.0f, s * g.self_kick, 0.0f), mul(side, s * g.self_kick * 0.25f)), g.max_speed);
    k.dw = cap(add(mul(cross(v3(0, 1, 0), side), s * g.self_spin), v3(0.0f, s * g.self_spin * 0.4f * rnd(seed, 4), 0.0f)), g.max_spin);
    return k;
}

Kick push_kick(V3 car_vel, V3 normal, const Cfg& g, unsigned seed) {
    Kick k; k.dv = v3(0, 0, 0); k.dw = v3(0, 0, 0); k.k = 0.0f; k.hit = false;
    V3 vh = v3(car_vel.x, 0.0f, car_vel.z);
    float speed = len(vh);
    if (!(speed >= g.push_min_speed) || speed <= 0.0f) return k;
    float f = clampf(speed / g.push_full_speed, 0.0f, 1.0f);
    float s = g.impulse_scale;
    V3 nh = norm(v3(normal.x, 0.0f, normal.z));
    // forward with the car, outward from the bumper (props scatter to the sides), up over the bonnet
    V3 dv = add(mul(vh, s * g.push_gain), mul(nh, s * speed * 0.2f));
    dv = add(dv, v3(0.0f, s * g.push_lift * f, 0.0f));
    k.dv = cap(dv, g.max_speed * 1.5f);
    V3 ax = norm(v3(rnd(seed, 5) + nh.z, 0.3f * rnd(seed, 6), rnd(seed, 7) - nh.x));
    if (len(ax) < 0.5f) ax = v3(1, 0, 0);
    k.dw = mul(ax, s * g.push_spin * f);
    k.k = f; k.hit = true;
    return k;
}

V3 lin_vel(V3 a, V3 b, float dt) { return dt > 1e-5f ? mul(sub(b, a), 1.0f / dt) : v3(0, 0, 0); }
V3 ang_vel(Q4 a, Q4 b, float dt) {
    if (!(dt > 1e-5f)) return v3(0, 0, 0);
    Q4 r = qmul(qnorm(b), qconj(qnorm(a)));   // world-frame delta: b = r a
    if (r.w < 0.0f) { r.x = -r.x; r.y = -r.y; r.z = -r.z; r.w = -r.w; }
    float s = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z);
    if (s < 1e-7f) return v3(0, 0, 0);
    float ang = 2.0f * atan2f(s, r.w);
    return mul(v3(r.x / s, r.y / s, r.z / s), ang / dt);
}
}  // namespace cp
