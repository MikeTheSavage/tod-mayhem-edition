// rd_logic.cpp - pure logic of the [ragdoll] feature (see rd_logic.h). No game memory, no Box3D.
#include "rd_logic.h"
#include <math.h>
#include <string.h>
#include <initializer_list>

namespace rd {
using namespace ph;

static const float PI = 3.14159265358979f;

Q4 qid() { Q4 q = {0, 0, 0, 1}; return q; }
Xf xid() { Xf x; x.p = v3(0, 0, 0); x.q = qid(); return x; }
Xf xmul(Xf a, Xf b) { Xf r; r.q = qnorm(qmul(a.q, b.q)); r.p = add(a.p, qrot(a.q, b.p)); return r; }
Xf xinv(Xf a) { Xf r; r.q = qconj(a.q); r.p = qrot(r.q, mul(a.p, -1.0f)); return r; }
V3 xpoint(Xf a, V3 v) { return add(a.p, qrot(a.q, v)); }
Q4 qaxis(V3 axis, float rad) {
    V3 n = norm(axis); float s = sinf(rad * 0.5f);
    Q4 q = {n.x * s, n.y * s, n.z * s, cosf(rad * 0.5f)}; return q;
}
Q4 qarc(V3 from, V3 to) {
    from = norm(from); to = norm(to);
    float d = dot(from, to);
    if (d > 0.999999f) return qid();
    if (d < -0.999999f) {   // half turn about any axis orthogonal to from
        V3 ax = cross(v3(1, 0, 0), from);
        if (len(ax) < 1e-3f) ax = cross(v3(0, 1, 0), from);
        return qaxis(ax, PI);
    }
    V3 c = cross(from, to);
    Q4 q = {c.x, c.y, c.z, 1.0f + d};
    return qnorm(q);
}
float qangle(Q4 q) {
    q = qnorm(q);
    float w = fabsf(q.w); if (w > 1.0f) w = 1.0f;
    return 2.0f * acosf(w);
}
V3 qlog(Q4 q) {
    q = qnorm(q);
    if (q.w < 0.0f) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    V3 u = v3(q.x, q.y, q.z);
    float s = len(u);
    if (s < 1e-7f) return mul(u, 2.0f);
    float ang = 2.0f * atan2f(s, q.w);
    return mul(u, ang / s);
}
float hinge_angle(Q4 q) {
    float a = 2.0f * atan2f(q.z, q.w);
    while (a > PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}
Xf xf_from_tod(const float m[16], bool* ok) {
    Rigid r = rigid_from_tod(m);
    Xf x; x.p = r.p; x.q = r.q;
    if (ok) *ok = r.ok && fabsf(r.scale.x - 1.0f) < 0.05f && fabsf(r.scale.y - 1.0f) < 0.05f && fabsf(r.scale.z - 1.0f) < 0.05f;
    return x;
}

// name, bone, capsule end, extra, radius, mass, parent, joint, cone, twist, lo, hi, friction
const BodyDef BODIES[NBODY] = {
    {"pelvis",     "Pelvis",     "",          0.00f, 0.130f, 11.0f, -1,      J_NONE,   0.00f, 0.00f, 0.00f, 0.00f, 0.0f},
    {"spine",      "Spine",      "Spine2",    0.00f, 0.120f, 10.0f, PELVIS,  J_SPHERE, 0.45f, 0.35f, 0.00f, 0.00f, 10.0f},
    {"chest",      "Spine2",     "Neck",      0.00f, 0.150f, 15.0f, SPINE,   J_SPHERE, 0.40f, 0.30f, 0.00f, 0.00f, 10.0f},
    {"head",       "Head",       "",          0.00f, 0.100f,  6.0f, CHEST,   J_SPHERE, 0.75f, 0.90f, 0.00f, 0.00f, 2.0f},
    {"l_upperarm", "L UpperArm", "L Forearm", 0.00f, 0.055f,  2.5f, CHEST,   J_SPHERE, 1.60f, 1.20f, 0.00f, 0.00f, 2.0f},
    {"l_forearm",  "L Forearm",  "L Hand",    0.10f, 0.045f,  2.0f, L_UPPER, J_HINGE,  0.00f, 0.00f, -0.05f, 2.60f, 1.0f},
    {"r_upperarm", "R UpperArm", "R Forearm", 0.00f, 0.055f,  2.5f, CHEST,   J_SPHERE, 1.60f, 1.20f, 0.00f, 0.00f, 2.0f},
    {"r_forearm",  "R Forearm",  "R Hand",    0.10f, 0.045f,  2.0f, R_UPPER, J_HINGE,  0.00f, 0.00f, -0.05f, 2.60f, 1.0f},
    {"l_thigh",    "L Thigh",    "L Calf",    0.00f, 0.080f,  9.0f, PELVIS,  J_SPHERE, 1.10f, 0.50f, 0.00f, 0.00f, 6.0f},
    {"l_calf",     "L Calf",     "L Foot",    0.06f, 0.060f,  5.5f, L_THIGH, J_HINGE,  0.00f, 0.00f, -0.05f, 2.50f, 3.0f},
    {"r_thigh",    "R Thigh",    "R Calf",    0.00f, 0.080f,  9.0f, PELVIS,  J_SPHERE, 1.10f, 0.50f, 0.00f, 0.00f, 6.0f},
    {"r_calf",     "R Calf",     "R Foot",    0.06f, 0.060f,  5.5f, R_THIGH, J_HINGE,  0.00f, 0.00f, -0.05f, 2.50f, 3.0f},
};
const char* body_name(int b) { return b >= 0 && b < NBODY ? BODIES[b].name : "?"; }

int find_bone(const Skeleton& s, const char* name) {
    for (int i = 0; i < s.n; ++i) if (!_stricmp(s.name[i], name)) return i;
    return -1;
}
static bool is_ancestor(const Skeleton& s, int anc, int i) {
    for (int g = 0; i >= 0 && g < MAXB; ++g) { if (i == anc) return true; i = s.parent[i]; }
    return false;
}
bool build_map(const Skeleton& s, Map* m) {
    memset(m, 0, sizeof *m);
    m->ok = false;
    if (s.n <= 0 || s.n > MAXB) { strcpy(m->err, "bone count"); return false; }
    for (int i = 0; i < s.n; ++i) if (s.parent[i] < -1 || s.parent[i] >= s.n || s.parent[i] == i) { strcpy(m->err, "parent index"); return false; }
    for (int b = 0; b < NBODY; ++b) {
        m->body_bone[b] = find_bone(s, BODIES[b].bone);
        m->end_bone[b] = BODIES[b].end[0] ? find_bone(s, BODIES[b].end) : -1;
        if (m->body_bone[b] < 0 || (BODIES[b].end[0] && m->end_bone[b] < 0)) {
            strcpy(m->err, "missing bone "); strncat(m->err, m->body_bone[b] < 0 ? BODIES[b].bone : BODIES[b].end, 60);
            return false;
        }
    }
    m->bip = find_bone(s, "Bip"); m->l_thigh = find_bone(s, "L Thigh"); m->r_thigh = find_bone(s, "R Thigh");
    m->neck = find_bone(s, "Neck"); m->head = find_bone(s, "Head");
    if (m->bip < 0 || m->neck < 0 || m->l_thigh < 0 || m->r_thigh < 0) { strcpy(m->err, "missing Bip / Neck / Thigh"); return false; }
    if (s.parent[m->bip] != -1 || s.parent[m->body_bone[PELVIS]] != m->bip) { strcpy(m->err, "Bip / Pelvis hierarchy"); return false; }
    // every body bone must hang (through other bones) under its parent body's bone
    for (int b = 1; b < NBODY; ++b)
        if (!is_ancestor(s, m->body_bone[BODIES[b].parent], s.parent[m->body_bone[b]])) { strcpy(m->err, "body hierarchy at "); strcat(m->err, BODIES[b].name); return false; }
    // drivers: body bones drive themselves, the rest inherits from the nearest ancestor; ForeTwist bones follow the forearm
    int depth[MAXB];
    for (int i = 0; i < s.n; ++i) { int d = 0; for (int j = s.parent[i]; j >= 0 && d < MAXB; j = s.parent[j]) ++d; depth[i] = d; }
    int k = 0;
    for (int d = 0; d < MAXB && k < s.n; ++d) for (int i = 0; i < s.n; ++i) if (depth[i] == d) m->order[k++] = i;
    m->norder = k;
    for (int i = 0; i < s.n; ++i) m->driver[i] = -1;
    for (int b = 0; b < NBODY; ++b) m->driver[m->body_bone[b]] = b;
    m->driver[m->bip] = PELVIS;
    for (int t = 0; t < m->norder; ++t) {
        int i = m->order[t];
        if (m->driver[i] >= 0) continue;
        if (strstr(s.name[i], "ForeTwist")) { m->driver[i] = (s.name[i][0] == 'R' || s.name[i][0] == 'r') ? R_FORE : L_FORE; continue; }
        int p = s.parent[i];
        m->driver[i] = p >= 0 ? m->driver[p] : -1;   // GamePivot, Dummy (children of the Character node): never written
    }
    m->ok = true;
    return true;
}
void fk(const Skeleton& s, const Xf* locals, Xf root, Xf* world) {
    // parents first: depth order computed on the fly (the bone array is not sorted)
    bool done[MAXB] = {false};
    for (int pass = 0, left = s.n; pass < MAXB && left > 0; ++pass)
        for (int i = 0; i < s.n; ++i) {
            if (done[i]) continue;
            int p = s.parent[i];
            if (p >= 0 && !done[p]) continue;
            world[i] = xmul(p >= 0 ? world[p] : root, locals[i]);
            done[i] = true; --left;
        }
}

static void capsule(Layout* L, int b, V3 a, V3 c, float r) {
    int k = L->ncap[b]; if (k >= 2) return;
    L->c1[b][k] = a; L->c2[b][k] = c; L->rad[b][k] = r; L->ncap[b] = k + 1;
}
// a limb capsule along v (body space) from 0.3 r to |v| + extra
static void limb(Layout* L, int b, V3 v, float extra, float r) {
    float l = len(v);
    V3 d = l > 1e-4f ? mul(v, 1.0f / l) : v3(-1, 0, 0);
    float a = 0.3f * r, e = l + extra - r;
    if (e < a) e = a;
    capsule(L, b, mul(d, a), mul(d, e), r);
}
bool build_layout(const Skeleton& s, const Map& m, const Xf* world, const Xf* rest_world, float scale, Layout* L) {
    memset(L, 0, sizeof *L);
    if (!m.ok) return false;
    if (!(scale > 0.2f && scale < 5.0f)) scale = 1.0f;
    for (int b = 0; b < NBODY; ++b) { L->body0[b] = world[m.body_bone[b]]; L->hinge_sign[b] = 1.0f; }
    for (int b = 0; b < NBODY; ++b) {
        const BodyDef& D = BODIES[b];
        Xf inv0 = xinv(L->body0[b]);
        float r = D.radius * scale;
        L->mass[b] = D.mass;
        if (b == PELVIS) {
            V3 a = xpoint(inv0, world[m.l_thigh].p), c = xpoint(inv0, world[m.r_thigh].p);
            capsule(L, b, a, c, r);
        } else if (b == HEAD) {
            // the head's long axis is its local -X (the eyes sit at about x = -0.06 .. -0.07)
            capsule(L, b, v3(-0.07f, 0, 0), v3(-0.15f, 0, 0), r);
        } else {
            limb(L, b, xpoint(inv0, world[m.end_bone[b]].p), D.end_extra, r);
            if (b == CHEST) {   // shoulders: a second capsule between the upper arms
                int lu = m.body_bone[L_UPPER], ru = m.body_bone[R_UPPER];
                V3 a = xpoint(inv0, world[lu].p), c = xpoint(inv0, world[ru].p);
                V3 d = norm(sub(c, a));
                capsule(L, b, add(a, mul(d, 0.04f)), sub(c, mul(d, 0.04f)), 0.075f * scale);
            }
        }
    }
    // joints
    for (int b = 1; b < NBODY; ++b) {
        const BodyDef& D = BODIES[b];
        int a = D.parent;
        L->kind[b] = D.kind; L->cone[b] = D.cone; L->twist_lo[b] = -D.twist; L->twist_hi[b] = D.twist;
        L->lo[b] = D.lo; L->hi[b] = D.hi; L->friction[b] = D.friction;
        V3 pa = xpoint(xinv(L->body0[a]), L->body0[b].p);
        L->frameA[b].p = pa; L->frameB[b].p = v3(0, 0, 0);
        if (D.kind == J_HINGE) {
            // straight limb = identity local rotation of the child bone; flexion = + about the child's local +Y
            Q4 fb = qaxis(v3(1, 0, 0), -PI * 0.5f);   // frame z -> body +Y
            Q4 rel = qmul(qconj(L->body0[a].q), L->body0[b].q);
            float bend = hinge_angle(qmul(qconj(fb), qmul(rel, fb)));
            if (bend < -0.35f) { L->hinge_sign[b] = -1.0f; fb = qaxis(v3(1, 0, 0), PI * 0.5f); }   // a rig that bends the other way
            L->frameA[b].q = fb; L->frameB[b].q = fb;
        } else {
            V3 u = L->ncap[b] ? norm(sub(L->c2[b][0], L->c1[b][0])) : v3(-1, 0, 0);
            Q4 zb = qarc(v3(0, 0, 1), u);
            Q4 qref;
            if (rest_world) qref = qmul(qconj(rest_world[m.body_bone[a]].q), rest_world[m.body_bone[b]].q);
            else qref = qmul(qconj(L->body0[a].q), L->body0[b].q);
            L->frameA[b].q = qnorm(qmul(qref, zb)); L->frameB[b].q = zb;
        }
    }
    // every driven bone's offset to its driving body
    for (int i = 0; i < s.n; ++i) {
        int d = m.driver[i];
        if (d < 0) { L->off[i] = xid(); continue; }
        L->off[i] = xmul(xinv(L->body0[d]), world[i]);
    }
    return true;
}
void body_frames(const Map& m, const Xf* world, Xf* bodies) {
    for (int b = 0; b < NBODY; ++b) bodies[b] = world[m.body_bone[b]];
}
void pose_from_bodies(const Skeleton& s, const Map& m, const Layout& L, const Xf* bodies, Xf root, Xf* out) {
    Xf W[MAXB];
    for (int t = 0; t < m.norder; ++t) {
        int i = m.order[t], p = s.parent[i], d = m.driver[i];
        Xf PW = p >= 0 ? W[p] : root;
        if (d < 0) { W[i] = xmul(PW, s.local[i]); continue; }
        if (i == m.bip) {
            W[i] = xmul(bodies[d], L.off[i]);
            out[i] = xmul(xinv(PW), W[i]);
        } else {
            W[i].q = qnorm(qmul(bodies[d].q, L.off[i].q));
            W[i].p = add(PW.p, qrot(PW.q, s.local[i].p));
            out[i].q = qnorm(qmul(qconj(PW.q), W[i].q));
            out[i].p = s.local[i].p;
        }
    }
}
Q4 joint_target(const Layout& L, int b, const Xf* anim, float* angle) {
    int a = BODIES[b].parent;
    Q4 fa = qmul(anim[a].q, L.frameA[b].q), fb = qmul(anim[b].q, L.frameB[b].q);
    Q4 t = qnorm(qmul(qconj(fa), fb));
    if (angle) *angle = hinge_angle(t);
    return t;
}
void velocity(Xf prev, Xf cur, float dt, float vmax, V3* v, V3* w) {
    *v = v3(0, 0, 0); *w = v3(0, 0, 0);
    if (!(dt > 1e-4f)) return;
    V3 lv = mul(sub(cur.p, prev.p), 1.0f / dt);
    V3 aw = mul(qlog(qmul(cur.q, qconj(prev.q))), 1.0f / dt);
    float l = len(lv); if (l > vmax) lv = mul(lv, vmax / l);
    float a = len(aw); if (a > 30.0f) aw = mul(aw, 30.0f / a);
    *v = lv; *w = aw;
}
static float seg_point(V3 a, V3 b, V3 p, float* t) {
    V3 ab = sub(b, a); float l2 = dot(ab, ab);
    float u = l2 > 1e-12f ? dot(sub(p, a), ab) / l2 : 0.0f;
    if (u < 0) u = 0; if (u > 1) u = 1;
    if (t) *t = u;
    return len(sub(p, add(a, mul(ab, u))));
}
int nearest_body(const Layout& L, const Xf* bodies, V3 p, float maxd, float* dist) {
    int best = -1; float bd = maxd;
    for (int b = 0; b < NBODY; ++b)
        for (int k = 0; k < L.ncap[b]; ++k) {
            float d = seg_point(xpoint(bodies[b], L.c1[b][k]), xpoint(bodies[b], L.c2[b][k]), p, nullptr) - L.rad[b][k];
            if (d < 0) d = 0;
            if (d < bd) { bd = d; best = b; }
        }
    if (dist) *dist = best >= 0 ? bd : maxd;
    return best;
}
// closest approach of segment [from,to] and the capsule axis [a,b]: hit when closer than r; *t = ray parameter there
float ray_capsule(V3 from, V3 to, V3 a, V3 b, float r, float* t) {
    V3 d1 = sub(to, from), d2 = sub(b, a), w = sub(from, a);
    float A = dot(d1, d1), B = dot(d1, d2), C = dot(d2, d2), D = dot(d1, w), E = dot(d2, w);
    float den = A * C - B * B, s, u;
    if (den < 1e-9f) { s = 0; u = C > 1e-9f ? E / C : 0; }
    else { s = (B * E - C * D) / den; u = (A * E - B * D) / den; }
    if (s < 0) s = 0; if (s > 1) s = 1;
    if (u < 0) u = 0; if (u > 1) u = 1;
    // refine s for the clamped u
    if (A > 1e-9f) { s = (dot(sub(add(a, mul(d2, u)), from), d1)) / A; if (s < 0) s = 0; if (s > 1) s = 1; }
    float dist = len(sub(add(from, mul(d1, s)), add(a, mul(d2, u))));
    if (t) *t = s;
    return dist - r;   // < 0: hit
}

bool spawn(const RdApi* A, const Layout& L, const Xf* bodies, const V3* lin, const V3* ang, const Cfg& c,
           float friction_scale, Spawned* out, bool powered) {
    out->r = -1;
    for (int b = 0; b < NBODY; ++b) out->joint[b] = -1;
    if (!A) return false;
    int r = A->create();
    if (r < 0) return false;
    float mk = c.total_mass / 80.0f;
    for (int b = 0; b < NBODY; ++b) {
        RdBodyDesc d; memset(&d, 0, sizeof d);
        d.pos[0] = bodies[b].p.x; d.pos[1] = bodies[b].p.y; d.pos[2] = bodies[b].p.z;
        d.rot[0] = bodies[b].q.x; d.rot[1] = bodies[b].q.y; d.rot[2] = bodies[b].q.z; d.rot[3] = bodies[b].q.w;
        if (lin) { d.lin_vel[0] = lin[b].x; d.lin_vel[1] = lin[b].y; d.lin_vel[2] = lin[b].z; }
        if (ang) { d.ang_vel[0] = ang[b].x; d.ang_vel[1] = ang[b].y; d.ang_vel[2] = ang[b].z; }
        d.ncap = L.ncap[b];
        for (int k = 0; k < L.ncap[b]; ++k) {
            d.c1[k][0] = L.c1[b][k].x; d.c1[k][1] = L.c1[b][k].y; d.c1[k][2] = L.c1[b][k].z;
            d.c2[k][0] = L.c2[b][k].x; d.c2[k][1] = L.c2[b][k].y; d.c2[k][2] = L.c2[b][k].z;
            d.radius[k] = L.rad[b][k];
        }
        d.mass = L.mass[b] * mk;
        d.lin_damping = 0.05f; d.ang_damping = powered ? c.active_ang_damping : c.limp_ang_damping; d.friction = 0.8f; d.restitution = 0.05f;
        if (A->add_body(r, &d) != b) { A->destroy(r); return false; }
    }
    for (int b = 1; b < NBODY; ++b) {
        RdJointDesc j; memset(&j, 0, sizeof j);
        j.body_a = BODIES[b].parent; j.body_b = b; j.kind = L.kind[b] == J_HINGE ? RD_JOINT_HINGE : RD_JOINT_SPHERE;
        const Xf& fa = L.frameA[b]; const Xf& fb = L.frameB[b];
        j.frame_a_pos[0] = fa.p.x; j.frame_a_pos[1] = fa.p.y; j.frame_a_pos[2] = fa.p.z;
        j.frame_a_rot[0] = fa.q.x; j.frame_a_rot[1] = fa.q.y; j.frame_a_rot[2] = fa.q.z; j.frame_a_rot[3] = fa.q.w;
        j.frame_b_pos[0] = fb.p.x; j.frame_b_pos[1] = fb.p.y; j.frame_b_pos[2] = fb.p.z;
        j.frame_b_rot[0] = fb.q.x; j.frame_b_rot[1] = fb.q.y; j.frame_b_rot[2] = fb.q.z; j.frame_b_rot[3] = fb.q.w;
        j.cone = L.cone[b]; j.twist_lo = L.twist_lo[b]; j.twist_hi = L.twist_hi[b]; j.lo = L.lo[b]; j.hi = L.hi[b];
        j.friction = L.friction[b] * friction_scale * mk;
        out->joint[b] = A->add_joint(r, &j);
        if (out->joint[b] < 0) { A->destroy(r); out->joint[0] = -1; return false; }
    }
    out->r = r;
    return true;
}

int core_of(int b) {
    switch (b) {
    case HEAD: case L_UPPER: case L_FORE: case R_UPPER: case R_FORE: return CHEST;
    case L_THIGH: case L_CALF: case R_THIGH: case R_CALF: return PELVIS;
    default: return b;   // pelvis, spine, chest are the core themselves
    }
}
Push share_push(const Layout& L, float mass_k, int b, V3 imp, float max_dv, float core_share) {
    Push p; memset(&p, 0, sizeof p);
    if (b < 0 || b >= NBODY) return p;
    float l = len(imp);
    if (!(l > 1e-6f)) return p;
    if (core_share < 0) core_share = 0; if (core_share > 1) core_share = 1;
    int c = core_of(b);
    float m = L.mass[b] * mass_k, part = l * (c == b ? 1.0f : 1.0f - core_share), cap = m * max_dv;
    if (part > cap) part = cap;
    float rest = l - part;
    V3 dir = mul(imp, 1.0f / l);
    p.body[0] = b; p.imp[0] = mul(dir, part); p.n = 1;
    if (rest > 1e-4f && c != b) {   // the torso takes the rest (also capped: a light body never flies off)
        float cm = L.mass[c] * mass_k * max_dv;
        if (rest > cm) rest = cm;
        p.body[1] = c; p.imp[1] = mul(dir, rest); p.n = 2;
    }
    return p;
}
void active_drive(const Cfg& c, const Strength& s, ActiveDrive* d) {
    d->hz[0] = 0; d->damping[0] = 0;
    for (int b = 1; b < NBODY; ++b) { d->hz[b] = c.joint_stiffness * s.joint(b); d->damping[b] = c.joint_damping; }
    // the balance comes back with the weakest part, so a body knocked far is pulled up gradually, not snapped up
    float m = s.s[PELVIS] < s.min_value() ? s.s[PELVIS] : s.min_value();
    d->balance_hz = c.balance_hertz * (0.25f + 0.75f * m);
    d->balance_damping = c.balance_damping;
}
float tighten_spine(Layout* L, const Xf* bodies, float scale, float margin) {
    float used = 1.0f;
    if (!(scale > 0.0f) || scale >= 1.0f) return 1.0f;
    for (int b : {(int)SPINE, (int)CHEST, (int)HEAD}) {
        int a = BODIES[b].parent;
        Q4 fa = qmul(bodies[a].q, L->frameA[b].q), fb = qmul(bodies[b].q, L->frameB[b].q);
        float cc = dot(qrot(fa, v3(0, 0, 1)), qrot(fb, v3(0, 0, 1))); if (cc > 1) cc = 1; if (cc < -1) cc = -1;
        float swing = acosf(cc);
        Q4 rel = qnorm(qmul(qconj(fa), fb));
        float tw = fabsf(2.0f * atan2f(rel.z, rel.w)); if (tw > PI) tw = 2.0f * PI - tw;
        float k = scale;
        if (L->cone[b] * k < swing + margin) k = (swing + margin) / L->cone[b];
        if (L->twist_hi[b] * k < tw + margin) k = (tw + margin) / L->twist_hi[b];
        if (k > 1.0f) k = 1.0f;
        L->cone[b] *= k; L->twist_lo[b] *= k; L->twist_hi[b] *= k;
        if (k < used) used = k;
    }
    return used;
}

void Strength::reset(float v) { for (int b = 0; b < NBODY; ++b) s[b] = v; }
void Strength::hit(int body, float floor_value, float neighbour_value) {
    if (body < 0 || body >= NBODY) return;
    if (s[body] > floor_value) s[body] = floor_value;
    int p = BODIES[body].parent;
    if (p >= 0 && s[p] > neighbour_value) s[p] = neighbour_value;
    for (int c = 0; c < NBODY; ++c) if (BODIES[c].parent == body && s[c] > neighbour_value) s[c] = neighbour_value;
}
void Strength::recover(float dt, float seconds) {
    float k = seconds > 0.01f ? dt / seconds : 1.0f;
    for (int b = 0; b < NBODY; ++b) { s[b] += k; if (s[b] > 1.0f) s[b] = 1.0f; }
}
float Strength::joint(int b) const {
    int p = BODIES[b].parent;
    return s[b] * (p >= 0 ? 0.5f + 0.5f * s[p] : 1.0f);
}
float Strength::min_value() const { float m = 1.0f; for (int b = 0; b < NBODY; ++b) if (s[b] < m) m = s[b]; return m; }

bool Settle::update(float max_speed, float max_spin, float dt, float speed_tol, float spin_tol, float hold) {
    if (max_speed < speed_tol && max_spin < spin_tol) quiet += dt; else quiet = 0.0f;
    return quiet >= hold;
}

static float clampf(float v, float lo, float hi) { return v != v ? lo : (v < lo ? lo : (v > hi ? hi : v)); }
void Cfg::clamp() {
    if (max_ragdolls < 1) max_ragdolls = 1; if (max_ragdolls > 12) max_ragdolls = 12;
    if (max_frozen < 0) max_frozen = 0; if (max_frozen > 32) max_frozen = 32;
    settle_seconds = clampf(settle_seconds, 0.5f, 60.0f);
    impulse_scale = clampf(impulse_scale, 0.0f, 20.0f);
    joint_stiffness = clampf(joint_stiffness, 0.5f, 60.0f);
    recover_seconds = clampf(recover_seconds, 0.05f, 10.0f);
    scale = clampf(scale, 0.5f, 2.0f);
    total_mass = clampf(total_mass, 20.0f, 300.0f);
    joint_damping = clampf(joint_damping, 0.0f, 5.0f);
    hit_strength = clampf(hit_strength, 0.0f, 1.0f);
    hit_neighbour = clampf(hit_neighbour, hit_strength, 1.0f);
    bullet_impulse = clampf(bullet_impulse, 0.0f, 2000.0f);
    game_hit_scale = clampf(game_hit_scale, 0.0f, 20.0f);
    blast_scale = clampf(blast_scale, 0.0f, 20.0f);
    blast_lift = clampf(blast_lift, 0.0f, 30.0f);
    blast_memory = clampf(blast_memory, 0.0f, 3.0f);
    death_fade = clampf(death_fade, 0.0f, 3.0f);
    limp_friction = clampf(limp_friction, 0.0f, 20.0f);
    active_gravity = clampf(active_gravity, 0.0f, 1.0f);
    balance_hertz = clampf(balance_hertz, 0.0f, 30.0f);
    balance_force = clampf(balance_force, 0.0f, 50000.0f);
    balance_torque = clampf(balance_torque, 0.0f, 20000.0f);
    balance_damping = clampf(balance_damping, 0.0f, 5.0f);
    foot_hertz = clampf(foot_hertz, 0.0f, 40.0f); foot_force = clampf(foot_force, 0.0f, 50000.0f);
    foot_damping = clampf(foot_damping, 0.0f, 5.0f);
    blend_in = clampf(blend_in, 0.0f, 2.0f); blend_out = clampf(blend_out, 0.0f, 3.0f);
    stumble_distance = clampf(stumble_distance, 0.05f, 3.0f);
    stumble_rate = clampf(stumble_rate, 0.0f, 20.0f);
    if (substeps < 1) substeps = 1; if (substeps > 16) substeps = 16;
    max_speed = clampf(max_speed, 1.0f, 200.0f);
    track_radius = clampf(track_radius, 5.0f, 300.0f);
    log_seconds = clampf(log_seconds, 0.5f, 600.0f);
    hit_impulse = clampf(hit_impulse, 0.0f, 2000.0f);
    hit_max_dv = clampf(hit_max_dv, 0.1f, 100.0f);
    hit_core_share = clampf(hit_core_share, 0.0f, 1.0f);
    hit_burst = clampf(hit_burst, 0.5f, 10.0f);
    active_friction = clampf(active_friction, 0.0f, 20.0f);
    active_ang_damping = clampf(active_ang_damping, 0.0f, 50.0f);
    spine_limit_scale = clampf(spine_limit_scale, 0.3f, 1.0f);
    death_max_dv = clampf(death_max_dv, 0.1f, 100.0f);
    death_core_share = clampf(death_core_share, 0.0f, 1.0f);
    limp_ang_damping = clampf(limp_ang_damping, 0.0f, 50.0f);
    blast_strength = clampf(blast_strength, 0.0f, 1.0f);
    stall_seconds = clampf(stall_seconds, 0.1f, 5.0f);
    stall_fall_seconds = clampf(stall_fall_seconds, 0.0f, 10.0f);
    corpse_seconds = clampf(corpse_seconds, 0.5f, 20.0f);
    if (corpse_max < 0) corpse_max = 0; if (corpse_max > 9) corpse_max = 9;
}

bool life_alive(int st, bool lethal) { return st >= 0 && st != 28 && !lethal; }
bool life_dead(int st, bool lethal, bool recent_hit, bool seen_alive) {
    if (!seen_alive) return false;
    return st == 28 || (st >= 0 && lethal && recent_hit);
}

bool StepWatch::stalled(uint32_t game_ms, int limit_ms) {
    if (!have) { stepped(game_ms); return false; }       // the first look starts the clock: a step must come within the limit
    int d = (int)(game_ms - last_ms);
    if (d < 0) { last_ms = game_ms; return false; }      // the clock went back (load, retry, rewind): a new base
    return d > limit_ms;
}
int stall_action(int mode, float t_sim, float fall_s) {
    if (mode == RM_ACTIVE) return SA_RELEASE;
    if (mode == RM_LIMP) return t_sim >= fall_s ? SA_FREEZE : SA_RELEASE;
    return SA_KEEP;
}

}  // namespace rd
