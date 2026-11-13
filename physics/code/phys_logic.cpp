// phys_logic.cpp - pure logic of the [physics] prototype (see phys_logic.h). No game memory, no Box3D.
#include "phys_logic.h"
#include <math.h>
#include <string.h>
#include <ctype.h>

namespace ph {

V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
V3 mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float len(V3 a) { return sqrtf(dot(a, a)); }
V3 norm(V3 a) { float l = len(a); return l > 1e-12f ? mul(a, 1.0f / l) : v3(0, 0, 0); }

Q4 qmul(Q4 a, Q4 b) {
    Q4 r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}
Q4 qconj(Q4 q) { Q4 r = {-q.x, -q.y, -q.z, q.w}; return r; }
Q4 qnorm(Q4 q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 1e-12f)) { Q4 id = {0, 0, 0, 1}; return id; }
    Q4 r = {q.x / l, q.y / l, q.z / l, q.w / l}; return r;
}
V3 qrot(Q4 q, V3 v) {   // v' = q v q*
    V3 u = v3(q.x, q.y, q.z);
    V3 t = mul(cross(u, v), 2.0f);
    return add(add(v, mul(t, q.w)), cross(u, t));
}
Q4 qslerp(Q4 a, Q4 b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1.0f - t) * th) / s; wb = sinf(t * th) / s;
    }
    Q4 r = {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    return qnorm(r);
}

// rotation matrix (R[i][j], column-vector) -> quaternion (Shepperd)
static Q4 quat_from_R(const float R[3][3]) {
    Q4 q; float tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (R[2][1] - R[1][2]) / s; q.y = (R[0][2] - R[2][0]) / s; q.z = (R[1][0] - R[0][1]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        float s = sqrtf(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2.0f;
        q.w = (R[2][1] - R[1][2]) / s; q.x = 0.25f * s; q.y = (R[0][1] + R[1][0]) / s; q.z = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        float s = sqrtf(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2.0f;
        q.w = (R[0][2] - R[2][0]) / s; q.x = (R[0][1] + R[1][0]) / s; q.y = 0.25f * s; q.z = (R[1][2] + R[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2.0f;
        q.w = (R[1][0] - R[0][1]) / s; q.x = (R[0][2] + R[2][0]) / s; q.y = (R[1][2] + R[2][1]) / s; q.z = 0.25f * s;
    }
    return qnorm(q);
}

Rigid rigid_from_tod(const float m[16]) {
    Rigid r; r.ok = false;
    V3 row[3] = {v3(m[0], m[1], m[2]), v3(m[4], m[5], m[6]), v3(m[8], m[9], m[10])};
    r.p = v3(m[12], m[13], m[14]);
    r.scale = v3(len(row[0]), len(row[1]), len(row[2]));
    Q4 id = {0, 0, 0, 1}; r.q = id;
    if (!(r.scale.x > 1e-6f && r.scale.y > 1e-6f && r.scale.z > 1e-6f)) return r;
    if (!(r.p.x == r.p.x && r.p.y == r.p.y && r.p.z == r.p.z)) return r;
    V3 c0 = mul(row[0], 1.0f / r.scale.x), c1 = mul(row[1], 1.0f / r.scale.y), c2 = mul(row[2], 1.0f / r.scale.z);
    if (dot(cross(c0, c1), c2) <= 0.0f) return r;   // mirrored: not a rigid motion
    float R[3][3] = {{c0.x, c1.x, c2.x}, {c0.y, c1.y, c2.y}, {c0.z, c1.z, c2.z}};   // column j = local axis j
    r.q = quat_from_R(R);
    r.ok = true;
    return r;
}
void tod_from_rigid(V3 p, Q4 q, float out[16]) {
    V3 ax = qrot(q, v3(1, 0, 0)), ay = qrot(q, v3(0, 1, 0)), az = qrot(q, v3(0, 0, 1));
    out[0] = ax.x; out[1] = ax.y; out[2] = ax.z; out[3] = 0;
    out[4] = ay.x; out[5] = ay.y; out[6] = ay.z; out[7] = 0;
    out[8] = az.x; out[9] = az.y; out[10] = az.z; out[11] = 0;
    out[12] = p.x; out[13] = p.y; out[14] = p.z; out[15] = 1;
}
V3 tod_point(const float m[16], V3 l) {
    return v3(l.x * m[0] + l.y * m[4] + l.z * m[8] + m[12], l.x * m[1] + l.y * m[5] + l.z * m[9] + m[13],
              l.x * m[2] + l.y * m[6] + l.z * m[10] + m[14]);
}
Q4 stored_from_true(Q4 q) { return qconj(qnorm(q)); }
Q4 true_from_stored(Q4 s) { return qconj(qnorm(s)); }

bool local_from_world(const float parent[16], V3 wp, Q4 wq, V3* lp, Q4* lq) {
    Rigid pr = rigid_from_tod(parent);
    if (!pr.ok) return false;
    V3 d = qrot(qconj(pr.q), sub(wp, pr.p));
    *lp = v3(d.x / pr.scale.x, d.y / pr.scale.y, d.z / pr.scale.z);
    *lq = qnorm(qmul(qconj(pr.q), wq));
    return true;
}

// ---- symmetric 3x3 eigen (Jacobi), smallest-eigenvalue vector
static V3 smallest_axis(const float C[3][3]) {
    float a[3][3], v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    memcpy(a, C, sizeof a);
    for (int sweep = 0; sweep < 12; ++sweep) {
        float off = fabsf(a[0][1]) + fabsf(a[0][2]) + fabsf(a[1][2]);
        if (off < 1e-12f) break;
        for (int p = 0; p < 2; ++p) for (int q = p + 1; q < 3; ++q) {
            if (fabsf(a[p][q]) < 1e-15f) continue;
            float th = (a[q][q] - a[p][p]) / (2.0f * a[p][q]);
            float t = (th >= 0 ? 1.0f : -1.0f) / (fabsf(th) + sqrtf(th * th + 1.0f));
            float c = 1.0f / sqrtf(t * t + 1.0f), s = t * c;
            for (int k = 0; k < 3; ++k) {   // A = J^T A J
                float akp = a[k][p], akq = a[k][q];
                a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq;
            }
            for (int k = 0; k < 3; ++k) {
                float apk = a[p][k], aqk = a[q][k];
                a[p][k] = c * apk - s * aqk; a[q][k] = s * apk + c * aqk;
            }
            for (int k = 0; k < 3; ++k) {
                float vkp = v[k][p], vkq = v[k][q];
                v[k][p] = c * vkp - s * vkq; v[k][q] = s * vkp + c * vkq;
            }
        }
    }
    int m = 0; if (a[1][1] < a[m][m]) m = 1; if (a[2][2] < a[m][m]) m = 2;
    return norm(v3(v[0][m], v[1][m], v[2][m]));
}
float thickness(const V3* pts, int n, V3* normal) {
    if (normal) *normal = v3(0, 1, 0);
    if (n < 3) return 0.0f;
    V3 c = v3(0, 0, 0);
    for (int i = 0; i < n; ++i) c = add(c, pts[i]);
    c = mul(c, 1.0f / n);
    float C[3][3] = {{0}};
    for (int i = 0; i < n; ++i) {
        V3 d = sub(pts[i], c); float e[3] = {d.x, d.y, d.z};
        for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k) C[r][k] += e[r] * e[k];
    }
    V3 ax = smallest_axis(C);
    if (len(ax) < 0.5f) return 0.0f;
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < n; ++i) { float t = dot(sub(pts[i], c), ax); if (t < lo) lo = t; if (t > hi) hi = t; }
    if (normal) *normal = ax;
    return hi - lo;
}
int prepare_hull(const V3* pts, int n, float min_thickness, float slab, V3* out) {
    if (n < 3) return 0;
    // reject a degenerate cloud (all points on a line / one point)
    float span = 0.0f;
    for (int i = 1; i < n; ++i) { float d = len(sub(pts[i], pts[0])); if (d > span) span = d; }
    if (!(span > 1e-3f)) return 0;
    V3 nrm;
    float th = thickness(pts, n, &nrm);
    if (n >= 4 && th >= min_thickness) { for (int i = 0; i < n; ++i) out[i] = pts[i]; return n; }
    // flat: area check (a sliver of collinear points has no plane)
    V3 far = pts[0]; float best = 0.0f;
    for (int i = 1; i < n; ++i) for (int j = i + 1; j < n; ++j) {
        float a = len(cross(sub(pts[i], pts[0]), sub(pts[j], pts[0])));
        if (a > best) { best = a; far = pts[j]; }
        if (j - i > 8) break;   // bounded effort; a real patch shows area early
    }
    (void)far;
    if (!(best > 1e-5f)) return 0;
    if (fabsf(nrm.y) > 0.3f) {   // floor / ramp / ceiling-like: the slab goes below (the walkable side stays exact)
        if (nrm.y < 0.0f) nrm = mul(nrm, -1.0f);
        for (int i = 0; i < n; ++i) { out[i] = pts[i]; out[n + i] = sub(pts[i], mul(nrm, slab)); }
    } else {                     // wall: a slab centred on the patch (both sides collide)
        V3 h = mul(nrm, slab * 0.5f);
        for (int i = 0; i < n; ++i) { out[i] = add(pts[i], h); out[n + i] = sub(pts[i], h); }
    }
    return 2 * n;
}

static bool ieq_sub(const char* hay, const char* needle, size_t nl) {
    if (!nl) return false;
    for (const char* h = hay; *h; ++h) {
        size_t k = 0;
        while (k < nl && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)needle[k])) ++k;
        if (k == nl) return true;
    }
    return false;
}
bool name_match(const char* name, const char* patterns) {
    if (!name || !patterns) return false;
    const char* p = patterns;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        const char* e = p; while (*e && *e != ',') ++e;
        const char* t = e; while (t > p && (t[-1] == ' ' || t[-1] == '\t')) --t;
        if (t > p && ieq_sub(name, p, (size_t)(t - p))) return true;
        p = e;
    }
    return false;
}
bool prop_size_ok(const PropRule& r, float rad) { return rad == rad && rad >= r.min_radius && rad <= r.max_radius; }

V3 bullet_impulse(V3 from, V3 to, float strength) { return mul(norm(sub(to, from)), strength); }
float blast_falloff(float d, float r) {
    if (!(r > 0.0f) || !(d == d)) return 0.0f;
    float k = 1.0f - d / r;
    return k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
}

int StepClock::advance(float dt) {
    if (!(dt > 0.0f) || !(step > 0.0f)) return 0;
    if (dt > 0.25f) dt = 0.25f;
    acc += dt;
    int n = (int)(acc / step);
    if (n > max_steps) { n = max_steps; acc = step * 0.999f; }   // spiral of death: drop the backlog
    else acc -= n * step;
    if (acc < 0.0f) acc = 0.0f;
    return n;
}

uint32_t DriveGate::advance(uint32_t game_ms) {
    if (!have) { last_ms = game_ms; have = true; return 0; }
    uint32_t d = game_ms - last_ms;
    last_ms = game_ms;
    if ((int32_t)d <= 0 || d > 1000u) return 0;   // stood still, went back (rewind / load) or jumped (load): a new base
    return d;
}
int drive_decision(bool enabled, bool foot_ticked, uint32_t ms, float dt, int playmode, bool cutscene, bool in_vehicle) {
    if (!enabled || foot_ticked || !in_vehicle) return DRV_NONE;
    if (playmode == 3) return DRV_REWIND;          // the game restores the nodes; bodies are put back afterwards
    if (playmode == 2 || cutscene) return DRV_NONE;
    if (ms > 0) return DRV_STEP;
    if (dt > 0.0f && dt < 0.001f) return DRV_STEP;   // deep slow motion: the world runs, the truncated ms clock does not
    return DRV_NONE;
}
V3 drive_scan_point(V3 ram, V3 vel, float lead, float max_ahead) {
    V3 a = mul(v3(vel.x, 0.0f, vel.z), lead);
    float l = len(a);
    if (!(l == l) || l < 1e-4f) return ram;
    if (l > max_ahead) a = mul(a, max_ahead / l);
    return add(ram, a);
}

static float clampf(float v, float lo, float hi) { return v != v ? lo : (v < lo ? lo : (v > hi ? hi : v)); }
void Cfg::clamp() {
    drive_lead = clampf(drive_lead, 0.0f, 3.0f);
    radius = clampf(radius, 5.0f, 150.0f);
    rescan_distance = clampf(rescan_distance, 2.0f, radius);
    if (max_bodies < 1) max_bodies = 1; if (max_bodies > 512) max_bodies = 512;
    step = clampf(step, 0.005f, 0.05f);
    if (substeps < 1) substeps = 1; if (substeps > 16) substeps = 16;
    gravity = clampf(gravity, 0.0f, 50.0f);
    density = clampf(density, 1.0f, 8000.0f);
    min_mass = clampf(min_mass, 0.1f, 1000.0f); max_mass = clampf(max_mass, min_mass, 5000.0f);
    friction = clampf(friction, 0.0f, 2.0f); restitution = clampf(restitution, 0.0f, 1.0f);
    linear_damping = clampf(linear_damping, 0.0f, 5.0f); angular_damping = clampf(angular_damping, 0.0f, 5.0f);
    bullet_impulse = clampf(bullet_impulse, 0.0f, 1000.0f);
    shell_radius = clampf(shell_radius, 0.5f, 40.0f); shell_impulse = clampf(shell_impulse, 0.0f, 100000.0f);
    destroy_radius = clampf(destroy_radius, 0.5f, 40.0f); destroy_impulse = clampf(destroy_impulse, 0.0f, 100000.0f);
    blast_lift = clampf(blast_lift, 0.0f, 30.0f);
    slab = clampf(slab, 0.02f, 2.0f); min_thickness = clampf(min_thickness, 0.001f, 0.5f);
    log_seconds = clampf(log_seconds, 0.5f, 600.0f);
}

}  // namespace ph
