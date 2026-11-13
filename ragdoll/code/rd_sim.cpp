// rd_sim.cpp - the ragdoll part of TODPhysics.dll (rd_api.h): capsule bodies, spherical / revolute joints with cone,
// twist and hinge limits, joint springs toward a target pose (Box3D's own joint springs), joint friction (a motor at
// zero speed), a pelvis anchor (a kinematic body + motor joint springs with a force cap), ray casts and explosions.
// Lives in the world of phys_sim.cpp (phsim_world): ragdolls land on the scanned level mesh and push props.
// Single-threaded like phys_sim.cpp; no game knowledge. Also linked into ragdoll/code/test_rd.cpp.
#include <windows.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "box3d/box3d.h"
#include "box3d/math_functions.h"
#include "rd_api.h"

#include "../../physics/code/phys_sim_internal.h"   // the one world accessor phsim_world() / phsim_build_mesh() (phys_sim.cpp)

namespace {
enum : uint64_t { CAT_STATIC = PHSIM_CAT_STATIC, CAT_PROP = PHSIM_CAT_PROP, CAT_RAGDOLL = PHSIM_CAT_RAGDOLL };   // phys_sim_internal.h
struct Joint { b3JointId id; int kind; };
struct Rag {
    bool used; b3WorldId world; int group;
    b3BodyId body[TODRAG_MAX_BODIES]; int nbody;
    std::vector<Joint> joints;
    b3BodyId anchor[TODRAG_MAX_BODIES]; b3JointId anchor_joint[TODRAG_MAX_BODIES];   // per body (pelvis, feet ...)
};
std::vector<Rag> g_rags;
int g_group_seq = 0;

bool world_ok(const Rag& r) {
    b3WorldId w = phsim_world();
    return r.used && b3World_IsValid(w) && w.index1 == r.world.index1 && w.generation == r.world.generation;
}
Rag* rag(int r) {
    if (r < 0 || r >= (int)g_rags.size() || !world_ok(g_rags[r])) return nullptr;
    return &g_rags[r];
}
b3BodyId body(int r, int b) {
    Rag* p = rag(r);
    if (!p || b < 0 || b >= p->nbody || !b3Body_IsValid(p->body[b])) return b3_nullBodyId;
    return p->body[b];
}
b3Vec3 V(const float* p) { return phsim_v(p); }   // finite, |x| <= 1e5 (phys_sim_internal.h)
b3Quat Q(const float* q) { return phsim_q(q); }   // non-finite -> identity
b3Transform T(const float* p, const float* q) { b3Transform t; t.p = V(p); t.q = Q(q); return t; }
bool okb(b3BodyId b) { return b3Body_IsValid(b); }

int rd_create(void) {
    b3WorldId w = phsim_world();
    if (!b3World_IsValid(w)) return -1;
    int idx = -1;
    for (int i = 0; i < (int)g_rags.size(); ++i) if (!g_rags[i].used) { idx = i; break; }
    if (idx < 0) { idx = (int)g_rags.size(); g_rags.push_back(Rag()); }
    Rag& r = g_rags[idx];
    r.used = true; r.world = w; r.group = -(1 + (g_group_seq++ % 30000)); r.nbody = 0; r.joints.clear();
    for (int i = 0; i < TODRAG_MAX_BODIES; ++i) { r.anchor[i] = b3_nullBodyId; r.anchor_joint[i] = b3_nullJointId; }
    return idx;
}
void rd_destroy(int ri) {
    if (ri < 0 || ri >= (int)g_rags.size() || !g_rags[ri].used) return;
    Rag& r = g_rags[ri];
    if (world_ok(r)) {   // joints go with their bodies
        for (int i = 0; i < r.nbody; ++i) if (okb(r.body[i])) b3DestroyBody(r.body[i]);
        for (int i = 0; i < TODRAG_MAX_BODIES; ++i) if (okb(r.anchor[i])) b3DestroyBody(r.anchor[i]);
    }
    r.used = false; r.nbody = 0; r.joints.clear();
    for (int i = 0; i < TODRAG_MAX_BODIES; ++i) { r.anchor[i] = b3_nullBodyId; r.anchor_joint[i] = b3_nullJointId; }
}
int rd_valid(int r) { return rag(r) ? 1 : 0; }
int rd_add_body(int ri, const RdBodyDesc* d) {
    Rag* r = rag(ri);
    if (!r || !d || r->nbody >= TODRAG_MAX_BODIES || d->ncap < 1) return -1;
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position = V(d->pos); bd.rotation = Q(d->rot);
    bd.linearVelocity = V(d->lin_vel); bd.angularVelocity = V(d->ang_vel);
    bd.linearDamping = d->lin_damping; bd.angularDamping = d->ang_damping;
    bd.isAwake = true;
    bd.userData = (void*)(intptr_t)(((ri + 1) << 8) | (r->nbody + 1));
    b3BodyId id = b3CreateBody(r->world, &bd);
    if (!okb(id)) return -1;
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.filter.categoryBits = CAT_RAGDOLL; sd.filter.maskBits = CAT_STATIC | CAT_PROP | CAT_RAGDOLL;
    sd.filter.groupIndex = r->group;   // never against its own bodies
    sd.density = 1000.0f; sd.baseMaterial.friction = d->friction; sd.baseMaterial.restitution = d->restitution;
    sd.updateBodyMass = false;
    int shapes = 0;
    for (int k = 0; k < d->ncap && k < 2; ++k) {
        b3Capsule c; c.center1 = V(d->c1[k]); c.center2 = V(d->c2[k]); c.radius = d->radius[k];
        if (!(c.radius > 0.005f)) continue;
        if (b3Shape_IsValid(b3CreateCapsuleShape(id, &sd, &c))) ++shapes;
    }
    if (!shapes) { b3DestroyBody(id); return -1; }
    b3Body_ApplyMassFromShapes(id);
    b3MassData md = b3Body_GetMassData(id);
    if (md.mass > 0.0f && d->mass > 0.0f) {
        float k = d->mass / md.mass;
        md.mass = d->mass;
        md.inertia.cx = b3MulSV(k, md.inertia.cx); md.inertia.cy = b3MulSV(k, md.inertia.cy); md.inertia.cz = b3MulSV(k, md.inertia.cz);
        b3Body_SetMassData(id, md);
    }
    r->body[r->nbody] = id;
    return r->nbody++;
}
int rd_add_joint(int ri, const RdJointDesc* d) {
    Rag* r = rag(ri);
    if (!r || !d) return -1;
    b3BodyId a = body(ri, d->body_a), b = body(ri, d->body_b);
    if (!okb(a) || !okb(b)) return -1;
    b3JointId id = b3_nullJointId;
    if (d->kind == RD_JOINT_HINGE) {
        b3RevoluteJointDef jd = b3DefaultRevoluteJointDef();
        jd.base.bodyIdA = a; jd.base.bodyIdB = b;
        jd.base.localFrameA = T(d->frame_a_pos, d->frame_a_rot); jd.base.localFrameB = T(d->frame_b_pos, d->frame_b_rot);
        jd.base.collideConnected = false;
        jd.enableLimit = true; jd.lowerAngle = d->lo; jd.upperAngle = d->hi;
        jd.enableMotor = d->friction > 0.0f; jd.maxMotorTorque = d->friction; jd.motorSpeed = 0.0f;
        jd.enableSpring = false;
        id = b3CreateRevoluteJoint(r->world, &jd);
    } else {
        b3SphericalJointDef jd = b3DefaultSphericalJointDef();
        jd.base.bodyIdA = a; jd.base.bodyIdB = b;
        jd.base.localFrameA = T(d->frame_a_pos, d->frame_a_rot); jd.base.localFrameB = T(d->frame_b_pos, d->frame_b_rot);
        jd.base.collideConnected = false;
        jd.enableConeLimit = d->cone > 0.0f; jd.coneAngle = d->cone;
        jd.enableTwistLimit = d->twist_hi > d->twist_lo; jd.lowerTwistAngle = d->twist_lo; jd.upperTwistAngle = d->twist_hi;
        jd.enableMotor = d->friction > 0.0f; jd.maxMotorTorque = d->friction;
        jd.motorVelocity.x = jd.motorVelocity.y = jd.motorVelocity.z = 0.0f;
        jd.enableSpring = false;
        id = b3CreateSphericalJoint(r->world, &jd);
    }
    if (!b3Joint_IsValid(id)) return -1;
    Joint j; j.id = id; j.kind = d->kind;
    r->joints.push_back(j);
    return (int)r->joints.size() - 1;
}
Joint* joint(int ri, int j) {
    Rag* r = rag(ri);
    if (!r || j < 0 || j >= (int)r->joints.size() || !b3Joint_IsValid(r->joints[j].id)) return nullptr;
    return &r->joints[j];
}
int rd_get(int r, int b, float pos[3], float rot[4]) {
    b3BodyId id = body(r, b); if (!okb(id)) return -1;
    b3WorldTransform t = b3Body_GetTransform(id);
    pos[0] = (float)t.p.x; pos[1] = (float)t.p.y; pos[2] = (float)t.p.z;
    rot[0] = t.q.v.x; rot[1] = t.q.v.y; rot[2] = t.q.v.z; rot[3] = t.q.s;
    if (!phsim_finite(pos, 3) || !phsim_finite(rot, 4)) return -1;   // a blown-up body: never handed to the game
    return b3Body_IsAwake(id) ? 1 : 0;
}
void rd_vel(int r, int b, float lin[3], float ang[3]) {
    b3BodyId id = body(r, b);
    b3Vec3 v = {0, 0, 0}, w = {0, 0, 0};
    if (okb(id)) { v = b3Body_GetLinearVelocity(id); w = b3Body_GetAngularVelocity(id); }
    lin[0] = v.x; lin[1] = v.y; lin[2] = v.z; ang[0] = w.x; ang[1] = w.y; ang[2] = w.z;
    phsim_zero_if_bad(lin, 3); phsim_zero_if_bad(ang, 3);
}
void rd_impulse(int r, int b, const float imp[3], const float point[3]) {
    b3BodyId id = body(r, b); if (!okb(id)) return;
    b3Body_ApplyLinearImpulse(id, V(imp), V(point), true);
}
void rd_set_vel(int r, int b, const float lin[3], const float ang[3]) {
    b3BodyId id = body(r, b); if (!okb(id)) return;
    b3Body_SetLinearVelocity(id, V(lin)); b3Body_SetAngularVelocity(id, V(ang));
}
void rd_drive(int ri, int j, float hertz, float damping, const float target[4], float angle) {
    Joint* p = joint(ri, j); if (!p) return;
    bool on = hertz > 0.0f;
    if (p->kind == RD_JOINT_HINGE) {
        b3RevoluteJoint_EnableSpring(p->id, on);
        if (on) { b3RevoluteJoint_SetSpringHertz(p->id, hertz); b3RevoluteJoint_SetSpringDampingRatio(p->id, damping); b3RevoluteJoint_SetTargetAngle(p->id, angle); }
    } else {
        b3SphericalJoint_EnableSpring(p->id, on);
        if (on) {
            b3SphericalJoint_SetSpringHertz(p->id, hertz); b3SphericalJoint_SetSpringDampingRatio(p->id, damping);
            if (target) b3SphericalJoint_SetTargetRotation(p->id, Q(target));
        }
    }
}
void rd_friction(int ri, int j, float t) {
    Joint* p = joint(ri, j); if (!p) return;
    if (p->kind == RD_JOINT_HINGE) { b3RevoluteJoint_EnableMotor(p->id, t > 0.0f); b3RevoluteJoint_SetMotorSpeed(p->id, 0.0f); b3RevoluteJoint_SetMaxMotorTorque(p->id, t); }
    else { b3Vec3 z = {0, 0, 0}; b3SphericalJoint_EnableMotor(p->id, t > 0.0f); b3SphericalJoint_SetMotorVelocity(p->id, z); b3SphericalJoint_SetMaxMotorTorque(p->id, t); }
}
void rd_anchor(int ri, int b, const float pos[3], const float rot[4], float lh, float ah, float damping, float maxf, float maxt, float dt) {
    Rag* r = rag(ri); if (!r || b < 0 || b >= TODRAG_MAX_BODIES) return;
    b3BodyId tb = body(ri, b);
    b3BodyId& an = r->anchor[b]; b3JointId& aj = r->anchor_joint[b];
    if (!(lh > 0.0f) || !okb(tb)) {   // off: remove this body's anchor (its joint goes with it)
        if (okb(an)) b3DestroyBody(an);
        an = b3_nullBodyId; aj = b3_nullJointId;
        return;
    }
    if (!okb(an)) {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_kinematicBody; bd.position = V(pos); bd.rotation = Q(rot); bd.enableSleep = false;
        an = b3CreateBody(r->world, &bd);
        if (!okb(an)) return;
        b3MotorJointDef jd = b3DefaultMotorJointDef();
        jd.base.bodyIdA = an; jd.base.bodyIdB = tb;
        jd.base.localFrameA.p.x = jd.base.localFrameA.p.y = jd.base.localFrameA.p.z = 0.0f; jd.base.localFrameA.q = b3Quat_identity;
        jd.base.localFrameB = jd.base.localFrameA;
        jd.base.collideConnected = false;
        jd.maxVelocityForce = 0.0f; jd.maxVelocityTorque = 0.0f;
        jd.linearHertz = lh; jd.linearDampingRatio = damping; jd.maxSpringForce = maxf;
        jd.angularHertz = ah; jd.angularDampingRatio = damping; jd.maxSpringTorque = maxt;
        aj = b3CreateMotorJoint(r->world, &jd);
    }
    b3WorldTransform t; t.p.x = pos[0]; t.p.y = pos[1]; t.p.z = pos[2]; t.q = Q(rot);
    b3Body_SetTargetTransform(an, t, dt > 1e-4f ? dt : 0.04f, true);
    if (b3Joint_IsValid(aj)) {
        b3MotorJoint_SetLinearHertz(aj, lh); b3MotorJoint_SetLinearDampingRatio(aj, damping); b3MotorJoint_SetMaxSpringForce(aj, maxf);
        b3MotorJoint_SetAngularHertz(aj, ah); b3MotorJoint_SetAngularDampingRatio(aj, damping); b3MotorJoint_SetMaxSpringTorque(aj, maxt);
    }
}
void rd_gravity(int ri, float g) {
    Rag* r = rag(ri); if (!r) return;
    for (int i = 0; i < r->nbody; ++i) if (okb(r->body[i])) b3Body_SetGravityScale(r->body[i], g);
}
void rd_wake(int ri) {
    Rag* r = rag(ri); if (!r) return;
    for (int i = 0; i < r->nbody; ++i) if (okb(r->body[i])) b3Body_SetAwake(r->body[i], true);
}
int rd_raycast(const float from[3], const float to[3], float hit[3], int* bi) {
    b3WorldId w = phsim_world();
    if (!b3World_IsValid(w)) return -1;
    phsim_build_mesh();
    b3QueryFilter f = b3DefaultQueryFilter();
    f.categoryBits = ~0ull; f.maskBits = CAT_RAGDOLL;
    b3Vec3 o = V(from), d = b3Sub(V(to), o);
    b3RayResult res = b3World_CastRayClosest(w, o, d, f);
    if (!res.hit) return -1;
    intptr_t u = (intptr_t)b3Body_GetUserData(b3Shape_GetBody(res.shapeId));
    int ri = (int)(u >> 8) - 1, b = (int)(u & 0xFF) - 1;
    if (!rag(ri) || b < 0 || b >= g_rags[ri].nbody) return -1;
    if (hit) { hit[0] = (float)res.point.x; hit[1] = (float)res.point.y; hit[2] = (float)res.point.z; }
    if (bi) *bi = b;
    return ri;
}
int rd_explode(const float c[3], float radius, float impulse_per_area, float lift) {
    b3WorldId w = phsim_world();
    radius = phsim_f(radius); impulse_per_area = phsim_f(impulse_per_area); lift = phsim_f(lift);   // finite (see ph_explode)
    if (!b3World_IsValid(w) || !(radius > 0.0f)) return 0;
    b3Vec3 cv = V(c);
    b3ExplosionDef ed = b3DefaultExplosionDef();
    ed.maskBits = CAT_RAGDOLL; ed.position = cv; ed.radius = radius; ed.falloff = radius * 0.5f;
    ed.impulsePerArea = impulse_per_area;
    b3World_Explode(w, &ed);
    int n = 0;
    for (size_t ri = 0; ri < g_rags.size(); ++ri) {
        Rag& r = g_rags[ri];
        if (!world_ok(r)) continue;
        bool reached = false;
        for (int i = 0; i < r.nbody; ++i) {
            if (!okb(r.body[i])) continue;
            b3Pos p = b3Body_GetWorldCenter(r.body[i]);
            float dx = (float)p.x - cv.x, dy = (float)p.y - cv.y, dz = (float)p.z - cv.z;
            float d = sqrtf(dx * dx + dy * dy + dz * dz), reach = radius * 1.5f;
            if (!(d <= reach)) continue;   // NaN: no
            reached = true;
            float k = 1.0f - d / reach;
            if (lift > 0.0f) {   // the same upward kick the props get, per body (so the whole body flies)
                float m = b3Body_GetMass(r.body[i]);
                b3Vec3 up = {0.0f, m * lift * k, 0.0f};
                b3Body_ApplyLinearImpulseToCenter(r.body[i], up, true);
            }
        }
        if (reached) {   // some spin to the torso so it tumbles
            b3BodyId t = r.nbody > 1 ? r.body[1] : r.body[0];
            if (okb(t)) {
                float m = b3Body_GetMass(t);
                b3Vec3 spin = {(float)((int)(ri * 7 + 3) % 5 - 2) * 0.6f, 0.0f, (float)((int)(ri * 13 + 1) % 5 - 2) * 0.6f};
                b3Body_ApplyAngularImpulse(t, b3MulSV(m * 0.25f, spin), true);
            }
            ++n;
        }
    }
    return n;
}
void rd_damping(int ri, float lin, float ang) {
    Rag* r = rag(ri); if (!r) return;
    for (int i = 0; i < r->nbody; ++i) if (okb(r->body[i])) { b3Body_SetLinearDamping(r->body[i], lin); b3Body_SetAngularDamping(r->body[i], ang); }
}
int rd_count(void) { int n = 0; for (auto& r : g_rags) if (world_ok(r)) ++n; return n; }

const RdApi g_api = {TODRAG_API_VERSION, rd_create, rd_add_body, rd_add_joint, rd_destroy, rd_valid, rd_get, rd_vel,
                     rd_impulse, rd_set_vel, rd_drive, rd_friction, rd_anchor, rd_gravity, rd_wake, rd_raycast, rd_explode, rd_count,
                     rd_damping};
}  // namespace

extern "C" const RdApi* __cdecl TODPhysics_GetRagdollApi(int version) { return (version == 1 || version == TODRAG_API_VERSION) ? &g_api : nullptr; }

#ifdef TODPHYS_DLL
#pragma comment(linker, "/EXPORT:TODPhysics_GetRagdollApi=_TODPhysics_GetRagdollApi")
#endif
