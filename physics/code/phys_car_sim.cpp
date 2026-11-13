// phys_car_sim.cpp - the [car_physics] part of TODPhysics.dll (car_api.h): kinematic boxes that follow the game's own
// cars and shove the props, dynamic boxes for wrecks, begin-touch reports. Lives in the world of phys_sim.cpp and
// registers its bodies in the same list (phys_sim_internal.h), so phys_api.h's body functions work on them too.
// No game knowledge here; also linked straight into the unit tests (physics/code/test_car.cpp).
#include <windows.h>
#include <string.h>
#include <math.h>
#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "box3d/math_functions.h"
#include "car_api.h"
#include "phys_sim_internal.h"

namespace {
b3Vec3 V(const float* p) { return phsim_v(p); }   // finite, |x| <= 1e5 (phys_sim_internal.h)
b3Quat Q(const float* q) { return phsim_q(q); }   // non-finite -> identity
bool world_ok() { return b3World_IsValid(phsim_world()); }

int make_box_body(b3BodyType type, uint64_t cat, uint64_t mask, const float half[3], const float off[3], const float pos[3],
                  const float rot[4], float density, float friction, float restitution, bool events, void* user, int kind) {
    if (!world_ok() || !half || !pos || !rot) return -1;
    float h[3];
    for (int i = 0; i < 3; ++i) { h[i] = half[i]; if (!(h[i] > 0.05f)) h[i] = 0.05f; if (h[i] > 20.0f) h[i] = 20.0f; }
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = type;
    bd.position = V(pos); bd.rotation = Q(rot);
    bd.isAwake = true;
    b3BodyId id = b3CreateBody(phsim_world(), &bd);
    if (!b3Body_IsValid(id)) return -1;
    b3Vec3 o = {0, 0, 0}; if (off) o = V(off);
    b3BoxHull bh = b3MakeOffsetBoxHull(h[0], h[1], h[2], o);
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.filter.categoryBits = cat; sd.filter.maskBits = mask;
    sd.density = density; sd.baseMaterial.friction = friction; sd.baseMaterial.restitution = restitution;
    sd.enableContactEvents = events;
    b3ShapeId s = b3CreateHullShape(id, &sd, &bh.base);
    if (!b3Shape_IsValid(s)) { b3DestroyBody(id); return -1; }
    int idx = phsim_add_body(id, user, kind);
    if (idx < 0) b3DestroyBody(id);
    return idx;
}

int car_kin_create(const float half[3], const float off[3], const float pos[3], const float rot[4], void* user) {
    // kinematic: infinite mass, moved by its velocity; collides with props and wrecks only (never with the level)
    return make_box_body(b3_kinematicBody, PHSIM_CAT_CAR, PHSIM_CAT_PROP | PHSIM_CAT_WRECK, half, off, pos, rot, 1.0f, 0.6f,
                         0.1f, true, user, PHSIM_KIND_CAR);
}
void car_kin_move(int b, const float pos[3], const float rot[4], float dt, int teleport) {
    b3BodyId id; int kind;
    if (!phsim_body(b, &id, &kind) || kind != PHSIM_KIND_CAR) return;
    if (teleport || !(dt > 0.0f)) {
        b3Body_SetTransform(id, V(pos), Q(rot));
        b3Vec3 z = {0, 0, 0}; b3Body_SetLinearVelocity(id, z); b3Body_SetAngularVelocity(id, z);
        return;
    }
    b3WorldTransform t; t.p = V(pos); t.q = Q(rot);
    b3Body_SetTargetTransform(id, t, dt, true);
}
int car_wreck_create(const float half[3], const float off[3], const float pos[3], const float rot[4], float mass, float friction,
                     float restitution, float lin_damping, float ang_damping, void* user) {
    int b = make_box_body(b3_dynamicBody, PHSIM_CAT_WRECK, PHSIM_CAT_STATIC | PHSIM_CAT_PROP | PHSIM_CAT_CAR | PHSIM_CAT_WRECK, half,
                          off, pos, rot, 1.0f, friction, restitution, true, user, PHSIM_KIND_WRECK);
    b3BodyId id;
    if (b < 0 || !phsim_body(b, &id, nullptr)) return -1;
    b3Body_SetLinearDamping(id, lin_damping); b3Body_SetAngularDamping(id, ang_damping);
    b3Body_ApplyMassFromShapes(id);
    b3MassData md = b3Body_GetMassData(id);
    if (md.mass > 0.0f && mass > 0.0f) {   // the box at density 1, scaled to the wanted mass (inertia with it)
        float k = mass / md.mass;
        md.mass = mass;
        md.inertia.cx = b3MulSV(k, md.inertia.cx); md.inertia.cy = b3MulSV(k, md.inertia.cy); md.inertia.cz = b3MulSV(k, md.inertia.cz);
        b3Body_SetMassData(id, md);
    }
    b3Body_EnableSleep(id, true);
    return b;
}
void car_set_vel(int b, const float lin[3], const float ang[3]) {
    b3BodyId id; if (!phsim_body(b, &id, nullptr)) return;
    b3Body_SetAwake(id, true);
    if (lin) b3Body_SetLinearVelocity(id, V(lin));
    if (ang) b3Body_SetAngularVelocity(id, V(ang));
}
int car_touches(PhTouch* out, int cap) {
    if (!world_ok() || !out || cap <= 0) return 0;
    b3ContactEvents ev = b3World_GetContactEvents(phsim_world());
    int n = 0;
    for (int i = 0; i < ev.beginCount && n < cap; ++i) {
        const b3ContactBeginTouchEvent& e = ev.beginEvents[i];
        if (!b3Shape_IsValid(e.shapeIdA) || !b3Shape_IsValid(e.shapeIdB)) continue;
        b3BodyId ba = b3Shape_GetBody(e.shapeIdA), bb = b3Shape_GetBody(e.shapeIdB);
        int ia = phsim_body_index(ba), ib = phsim_body_index(bb), ka = -1, kb = -1;
        if (ia < 0 || ib < 0 || !phsim_body(ia, nullptr, &ka) || !phsim_body(ib, nullptr, &kb)) continue;
        bool a_car = ka == PHSIM_KIND_CAR || ka == PHSIM_KIND_WRECK, b_car = kb == PHSIM_KIND_CAR || kb == PHSIM_KIND_WRECK;
        int car, prop; float sign;
        if (a_car && kb == PHSIM_KIND_PROP) { car = ia; prop = ib; sign = 1.0f; }
        else if (b_car && ka == PHSIM_KIND_PROP) { car = ib; prop = ia; sign = -1.0f; }
        else continue;
        PhTouch& t = out[n++];
        t.car = car; t.prop = prop;
        b3Vec3 pc = b3Body_GetWorldCenter(b3Shape_GetBody(car == ia ? e.shapeIdA : e.shapeIdB));
        b3Vec3 pp = b3Body_GetWorldCenter(b3Shape_GetBody(car == ia ? e.shapeIdB : e.shapeIdA));
        b3Vec3 p = b3MulSV(0.5f, b3Add(pc, pp)), nrm = b3Normalize(b3Sub(pp, pc));
        if (b3Contact_IsValid(e.contactId)) {   // the manifold's point and normal when the contact still exists
            b3ContactData cd = b3Contact_GetData(e.contactId);
            if (cd.manifoldCount > 0 && cd.manifolds && cd.manifolds[0].pointCount > 0) {
                b3Vec3 ca = b3Body_GetWorldCenter(b3Shape_GetBody(cd.shapeIdA));
                p = b3Add(ca, cd.manifolds[0].points[0].anchorA);
                b3Vec3 mn = cd.manifolds[0].normal;   // A -> B of the contact's own order
                bool car_is_a = b3Body_IsValid(b3Shape_GetBody(cd.shapeIdA)) && phsim_body_index(b3Shape_GetBody(cd.shapeIdA)) == car;
                nrm = car_is_a ? mn : b3Neg(mn);
            }
        }
        (void)sign;
        t.point[0] = p.x; t.point[1] = p.y; t.point[2] = p.z;
        t.normal[0] = nrm.x; t.normal[1] = nrm.y; t.normal[2] = nrm.z;
    }
    return n;
}
int car_raycast_wreck(const float from[3], const float to[3], float hit[3]) {
    if (!world_ok()) return -1;
    phsim_build_mesh();
    b3QueryFilter f = b3DefaultQueryFilter();
    f.categoryBits = ~0ull; f.maskBits = PHSIM_CAT_WRECK;
    b3Vec3 o = V(from), d = b3Sub(V(to), o);
    b3RayResult r = b3World_CastRayClosest(phsim_world(), o, d, f);
    if (!r.hit) return -1;
    int idx = phsim_body_index(b3Shape_GetBody(r.shapeId));
    if (idx >= 0 && hit) { hit[0] = r.point.x; hit[1] = r.point.y; hit[2] = r.point.z; }
    return idx;
}
int car_kind(int b) { int k; return phsim_body(b, nullptr, &k) ? k : -1; }

const PhCarApi g_car_api = {TODPHYS_CAR_API_VERSION, car_kin_create, car_kin_move, car_wreck_create, car_set_vel, car_touches,
                            car_raycast_wreck, car_kind};
}  // namespace

extern "C" const PhCarApi* __cdecl TODPhysics_GetCarApi(int version) {
    return version == TODPHYS_CAR_API_VERSION ? &g_car_api : nullptr;
}

#ifdef TODPHYS_DLL
#pragma comment(linker, "/EXPORT:TODPhysics_GetCarApi=_TODPhysics_GetCarApi")
#endif
