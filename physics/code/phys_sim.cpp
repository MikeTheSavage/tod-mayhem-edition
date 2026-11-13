// phys_sim.cpp - TODPhysics.dll: the Box3D world behind phys_api.h. Knows nothing about the game: the [physics] glue
// in TODGameplay.dll reads the level and the props from the game and drives this through the function table.
// Single-threaded on purpose (workerCount 1, no Box3D threads): everything runs on the game's thread inside its tick.
// Also linked straight into physics/code/test_phys.cpp (unit tests, no DLL).
#include <windows.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "box3d/box3d.h"   // C API; the headers carry their own extern "C"
#include "box3d/collision.h"
#include "box3d/math_functions.h"
#include "phys_api.h"
#include "phys_sim_internal.h"   // [car_physics] (phys_car_sim.cpp) shares this world: categories, body list

namespace {
// categories: phys_sim_internal.h (PHSIM_CAT_*: static level, props, kinematic cars, wreck bodies)
enum : uint64_t { CAT_STATIC = PHSIM_CAT_STATIC, CAT_PROP = PHSIM_CAT_PROP, CAT_RAGDOLL = PHSIM_CAT_RAGDOLL, CAT_CAR = PHSIM_CAT_CAR, CAT_WRECK = PHSIM_CAT_WRECK };
struct Body { b3BodyId id; bool used; bool has_shape; void* user; int kind; };   // kind: PHSIM_KIND_*
b3WorldId g_world = b3_nullWorldId;
b3BodyId g_static = b3_nullBodyId;
std::vector<Body> g_bodies;
int g_static_shapes = 0, g_static_failed = 0;
// The level is ONE static triangle mesh: every static convex piece is first made a Box3D hull (validated, see make_hull)
// and its outward faces are appended here; the mesh shape is (re)built before the next step or query. Hull-vs-hull SAT
// on near-coplanar level pieces let a crate fall through the plaza (run p7, offline replay of the exact floats); the
// mesh contact of the same faces holds it (replay after this change).
std::vector<b3Vec3> g_mv; std::vector<int32_t> g_mi;
b3MeshData* g_mesh = nullptr; b3ShapeId g_mesh_shape = b3_nullShapeId; bool g_mesh_dirty = false; float g_mesh_friction = 0.6f;
LARGE_INTEGER g_qpf;

bool live() { return b3World_IsValid(g_world); }
Body* get(int b) {
    if (b < 0 || b >= (int)g_bodies.size() || !g_bodies[b].used || !live()) return nullptr;
    if (!b3Body_IsValid(g_bodies[b].id)) return nullptr;
    return &g_bodies[b];
}
b3Vec3 V(const float* p) { return phsim_v(p); }   // finite, |x| <= 1e5 (phys_sim_internal.h)
b3Quat Q(const float* q) { return phsim_q(q); }   // non-finite -> identity

void ph_destroy() {
    if (live()) b3DestroyWorld(g_world);
    if (g_mesh) b3DestroyMesh(g_mesh);   // after the world: the shape referenced it
    g_mesh = nullptr; g_mesh_shape = b3_nullShapeId; g_mesh_dirty = false;
    g_mv.clear(); g_mi.clear();
    g_world = b3_nullWorldId; g_static = b3_nullBodyId;
    g_bodies.clear(); g_static_shapes = g_static_failed = 0;
}
void build_mesh() {   // the accumulated faces -> one mesh shape on the static body
    if (!g_mesh_dirty || !live()) return;
    g_mesh_dirty = false;
    if (b3Shape_IsValid(g_mesh_shape)) b3DestroyShape(g_mesh_shape, false);
    g_mesh_shape = b3_nullShapeId;
    b3MeshData* old = g_mesh; g_mesh = nullptr;
    if (g_mi.size() >= 3) {
        b3MeshDef md; memset(&md, 0, sizeof md);
        md.vertices = g_mv.data(); md.stride = 0; md.indices = g_mi.data(); md.materialIndices = nullptr;
        md.vertexCount = (int)g_mv.size(); md.triangleCount = (int)(g_mi.size() / 3);
        md.weldVertices = true; md.weldTolerance = 0.002f; md.identifyEdges = true; md.clockWiseWinding = false;
        g_mesh = b3CreateMesh(&md, nullptr, 0);
        if (g_mesh) {
            b3ShapeDef sd = b3DefaultShapeDef();
            sd.filter.categoryBits = CAT_STATIC; sd.filter.maskBits = CAT_PROP | CAT_RAGDOLL | CAT_WRECK;
            sd.baseMaterial.friction = g_mesh_friction;
            b3Vec3 one = {1, 1, 1};
            g_mesh_shape = b3CreateMeshShape(g_static, &sd, g_mesh, one);
        }
    }
    if (old) b3DestroyMesh(old);   // its shape is gone
}
int ph_create(float gravity_y, float restitution_threshold) {
    ph_destroy();
    QueryPerformanceFrequency(&g_qpf);
    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity.x = 0.0f; wd.gravity.y = gravity_y; wd.gravity.z = 0.0f;
    wd.workerCount = 1; wd.enqueueTask = nullptr; wd.finishTask = nullptr;   // no threads inside the game
    if (restitution_threshold > 0.0f) wd.restitutionThreshold = restitution_threshold;
    wd.enableSleep = true; wd.enableContinuous = true;
    g_world = b3CreateWorld(&wd);
    if (!live()) return 0;
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_staticBody;
    g_static = b3CreateBody(g_world, &bd);
    return b3Body_IsValid(g_static) ? 1 : 0;
}
// A hull must hold every input point (within tol) and have every face plane in front of its centre. Near-coplanar
// level patches (the Downtown plaza curb, 22 points with mm-level bumps) gave b3CreateHull a hull that did NOT cover
// its points: a crate fell through the plaza in game (run p7) and in an offline replay of the exact floats.
static int g_hull_retries = 0, g_hull_boxes = 0;
static bool hull_ok(const b3HullData* h, const b3Vec3* p, int n, float tol) {
    if (!h || h->vertexCount < 4 || h->faceCount < 4) return false;
    const b3Plane* pl = b3GetHullPlanes(h);
    for (int f = 0; f < h->faceCount; ++f)
        if (b3Dot(pl[f].normal, h->center) - pl[f].offset >= 0.0f) return false;
    for (int i = 0; i < n; ++i) {
        float worst = -1e30f;
        for (int f = 0; f < h->faceCount; ++f) { float s = b3Dot(pl[f].normal, p[i]) - pl[f].offset; if (s > worst) worst = s; }
        if (worst > tol) return false;
    }
    return true;
}
b3HullData* make_hull(const float* pts, int n) {
    if (!pts || n < 4) return nullptr;
    std::vector<b3Vec3> v((size_t)n);
    b3Vec3 lo = V(pts), hi = lo;
    for (int i = 0; i < n; ++i) {
        v[i] = V(pts + 3 * i);   // finite
        lo = b3Min(lo, v[i]); hi = b3Max(hi, v[i]);
    }
    float tol = 0.02f;
    b3HullData* h = b3CreateHull(v.data(), n, 32);
    if (hull_ok(h, v.data(), n, tol)) return h;
    if (h) b3DestroyHull(h);
    ++g_hull_retries;
    // retry on a 5 mm grid around the first point (removes the mm-level non-coplanarity that confuses the builder)
    std::vector<b3Vec3> q(v);
    for (auto& a : q) {
        a.x = v[0].x + floorf((a.x - v[0].x) / 0.005f + 0.5f) * 0.005f;
        a.y = v[0].y + floorf((a.y - v[0].y) / 0.005f + 0.5f) * 0.005f;
        a.z = v[0].z + floorf((a.z - v[0].z) / 0.005f + 0.5f) * 0.005f;
    }
    h = b3CreateHull(q.data(), n, 32);
    if (hull_ok(h, v.data(), n, 0.03f)) return h;
    if (h) b3DestroyHull(h);
    // last resort: the points' axis-aligned box (covers more than the shape, never less)
    ++g_hull_boxes;
    b3Vec3 c = b3MulSV(0.5f, b3Add(lo, hi)), e = b3MulSV(0.5f, b3Sub(hi, lo));
    e.x = e.x < 0.02f ? 0.02f : e.x; e.y = e.y < 0.02f ? 0.02f : e.y; e.z = e.z < 0.02f ? 0.02f : e.z;
    b3Vec3 box[8];
    for (int i = 0; i < 8; ++i) { box[i].x = c.x + ((i & 1) ? e.x : -e.x); box[i].y = c.y + ((i & 2) ? e.y : -e.y); box[i].z = c.z + ((i & 4) ? e.z : -e.z); }
    return b3CreateHull(box, 8, 8);
}
int ph_add_static_hull(const float* pts, int n, float friction) {
    if (!live()) return 0;
    b3HullData* h = make_hull(pts, n);
    if (!h) { ++g_static_failed; return 0; }
    // the hull's faces (CCW seen from outside = Box3D's front side) -> fan triangles of the level mesh
    const b3Vec3* hp = b3GetHullPoints(h);
    const b3HullHalfEdge* he = b3GetHullEdges(h);
    const b3HullFace* hf = b3GetHullFaces(h);
    int32_t base = (int32_t)g_mv.size();
    for (int i = 0; i < h->vertexCount; ++i) g_mv.push_back(hp[i]);
    for (int f = 0; f < h->faceCount; ++f) {
        int e0 = hf[f].edge, e = he[e0].next, guard = 0;
        int a = he[e0].origin, b = he[e].origin;
        e = he[e].next;
        while (e != e0 && guard++ < 255) {
            int c = he[e].origin;
            g_mi.push_back(base + a); g_mi.push_back(base + b); g_mi.push_back(base + c);
            b = c; e = he[e].next;
        }
    }
    b3DestroyHull(h);
    g_mesh_friction = friction; g_mesh_dirty = true;
    ++g_static_shapes;
    return 1;
}
int ph_body_create(const PhBodyDesc* d) {
    if (!live() || !d) return -1;
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position = V(d->pos); bd.rotation = Q(d->rot);
    bd.linearDamping = d->lin_damping; bd.angularDamping = d->ang_damping;
    bd.isAwake = d->asleep ? false : true;
    int idx = (int)g_bodies.size();
    bd.userData = (void*)(intptr_t)(idx + 1);
    b3BodyId id = b3CreateBody(g_world, &bd);
    if (!b3Body_IsValid(id)) return -1;
    Body b; b.id = id; b.used = true; b.has_shape = false; b.user = d->user; b.kind = PHSIM_KIND_PROP;
    g_bodies.push_back(b);
    return idx;
}
int ph_body_add_hull(int b, const float* pts, int n, float density, float friction, float restitution) {
    Body* p = get(b); if (!p) return 0;
    b3HullData* h = make_hull(pts, n);
    if (!h) return 0;
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.filter.categoryBits = CAT_PROP; sd.filter.maskBits = CAT_STATIC | CAT_PROP | CAT_RAGDOLL | CAT_CAR | CAT_WRECK;
    sd.density = density; sd.baseMaterial.friction = friction; sd.baseMaterial.restitution = restitution;
    sd.updateBodyMass = false;
    b3ShapeId s = b3CreateHullShape(p->id, &sd, h);
    b3DestroyHull(h);
    if (!b3Shape_IsValid(s)) return 0;
    p->has_shape = true;
    return 1;
}
int ph_body_finish(int b, float min_mass, float max_mass) {
    Body* p = get(b); if (!p) return 0;
    if (!p->has_shape) { b3DestroyBody(p->id); p->used = false; return 0; }
    b3Body_ApplyMassFromShapes(p->id);
    b3MassData md = b3Body_GetMassData(p->id);
    float m = md.mass, want = m;
    if (want < min_mass) want = min_mass;
    if (want > max_mass) want = max_mass;
    if (m > 0.0f && want != m) {   // scale mass and inertia together (same shape, other density)
        float k = want / m;
        md.mass = want;
        md.inertia.cx = b3MulSV(k, md.inertia.cx);
        md.inertia.cy = b3MulSV(k, md.inertia.cy);
        md.inertia.cz = b3MulSV(k, md.inertia.cz);
        b3Body_SetMassData(p->id, md);
    }
    return 1;
}
void ph_body_remove(int b) { Body* p = get(b); if (p) { b3DestroyBody(p->id); p->used = false; } }
int ph_body_get(int b, float pos[3], float rot[4]) {
    Body* p = get(b); if (!p) return -1;
    b3WorldTransform t = b3Body_GetTransform(p->id);
    pos[0] = t.p.x; pos[1] = t.p.y; pos[2] = t.p.z;
    rot[0] = t.q.v.x; rot[1] = t.q.v.y; rot[2] = t.q.v.z; rot[3] = t.q.s;
    if (!phsim_finite(pos, 3) || !phsim_finite(rot, 4)) return -1;   // a blown-up body: the game keeps its last pose
    return b3Body_IsAwake(p->id) ? 1 : 0;
}
void ph_body_set(int b, const float pos[3], const float rot[4], int keep_velocity) {
    Body* p = get(b); if (!p) return;
    b3Body_SetTransform(p->id, V(pos), Q(rot));
    if (!keep_velocity) { b3Vec3 z = {0, 0, 0}; b3Body_SetLinearVelocity(p->id, z); b3Body_SetAngularVelocity(p->id, z); }
}
float ph_body_mass(int b) { Body* p = get(b); return p ? b3Body_GetMass(p->id) : 0.0f; }
void ph_body_vel(int b, float lin[3], float ang[3]) {
    Body* p = get(b);
    b3Vec3 v = {0, 0, 0}, w = {0, 0, 0};
    if (p) { v = b3Body_GetLinearVelocity(p->id); w = b3Body_GetAngularVelocity(p->id); }
    lin[0] = v.x; lin[1] = v.y; lin[2] = v.z; ang[0] = w.x; ang[1] = w.y; ang[2] = w.z;
    phsim_zero_if_bad(lin, 3); phsim_zero_if_bad(ang, 3);
}
void ph_impulse(int b, const float imp[3], const float point[3]) {
    Body* p = get(b); if (!p) return;
    b3Body_ApplyLinearImpulse(p->id, V(imp), V(point), true);
}
int ph_raycast(const float from[3], const float to[3], float hit[3], void** user) {
    if (!live()) return -1;
    build_mesh();
    b3QueryFilter f = b3DefaultQueryFilter();
    f.categoryBits = ~0ull; f.maskBits = CAT_PROP;
    b3Vec3 o = V(from), d = b3Sub(V(to), o);
    b3RayResult r = b3World_CastRayClosest(g_world, o, d, f);
    if (!r.hit) return -1;
    b3BodyId bid = b3Shape_GetBody(r.shapeId);
    intptr_t u = (intptr_t)b3Body_GetUserData(bid);
    int idx = (int)u - 1;
    if (idx < 0 || idx >= (int)g_bodies.size() || !g_bodies[idx].used) return -1;
    if (hit) { hit[0] = r.point.x; hit[1] = r.point.y; hit[2] = r.point.z; }
    if (user) *user = g_bodies[idx].user;
    return idx;
}
int ph_raycast_static(const float from[3], const float to[3], float hit[3]) {
    if (!live()) return 0;
    build_mesh();
    b3QueryFilter f = b3DefaultQueryFilter();
    f.categoryBits = ~0ull; f.maskBits = CAT_STATIC;
    b3Vec3 o = V(from), d = b3Sub(V(to), o);
    b3RayResult r = b3World_CastRayClosest(g_world, o, d, f);
    if (!r.hit) return 0;
    if (hit) { hit[0] = r.point.x; hit[1] = r.point.y; hit[2] = r.point.z; }
    return 1;
}
int ph_explode(const float c[3], float radius, float impulse_per_area, float lift) {
    if (!live()) return 0;
    // finite inputs only: a NaN centre made every distance NaN, "d > reach" false, and NaN impulses for every prop
    b3Vec3 cv = V(c);
    radius = phsim_f(radius); impulse_per_area = phsim_f(impulse_per_area); lift = phsim_f(lift);
    if (radius <= 0.0f) return 0;
    b3ExplosionDef ed = b3DefaultExplosionDef();
    ed.maskBits = CAT_PROP; ed.position = cv; ed.radius = radius; ed.falloff = radius * 0.5f;
    ed.impulsePerArea = impulse_per_area;
    b3World_Explode(g_world, &ed);
    int n = 0;
    for (size_t i = 0; i < g_bodies.size(); ++i) {
        Body& b = g_bodies[i];
        if (!b.used || b.kind != PHSIM_KIND_PROP || !b3Body_IsValid(b.id)) continue;   // cars / wrecks: [car_physics]
        b3Vec3 p = b3Body_GetWorldCenter(b.id);
        float dx = p.x - cv.x, dy = p.y - cv.y, dz = p.z - cv.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float reach = radius * 1.5f;
        if (!(d <= reach)) continue;   // NaN: no
        float k = 1.0f - d / reach;
        ++n;
        if (lift > 0.0f) {   // an upward kick so props fly (the area model alone pushes them along the ground)
            float m = b3Body_GetMass(b.id);
            b3Vec3 up = {0.0f, m * lift * k, 0.0f};
            b3Body_ApplyLinearImpulseToCenter(b.id, up, true);
            b3Vec3 spin = {(float)((int)(i * 7919) % 5 - 2) * 0.3f * k, 0.0f, (float)((int)(i * 104729) % 5 - 2) * 0.3f * k};
            b3Body_ApplyAngularImpulse(b.id, b3MulSV(m * 0.2f, spin), true);
        }
    }
    return n;
}
double ph_step(float dt, int substeps) {
    if (!live() || !(dt > 0.0f)) return 0.0;
    build_mesh();
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    b3World_Step(g_world, dt, substeps);
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)(g_qpf.QuadPart ? g_qpf.QuadPart : 1);
}
void ph_counts(PhCounts* o) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->static_shapes = g_static_shapes; o->static_failed = g_static_failed;
    for (size_t i = 0; i < g_bodies.size(); ++i) {
        if (!g_bodies[i].used || !live() || !b3Body_IsValid(g_bodies[i].id)) continue;
        if (g_bodies[i].kind == PHSIM_KIND_CAR) continue;   // kinematic cars move all the time: not a reason to hold a rescan
        ++o->bodies;
        if (b3Body_IsAwake(g_bodies[i].id)) ++o->awake;
    }
}

const PhApi g_api = {TODPHYS_API_VERSION, ph_create, ph_destroy, ph_add_static_hull, ph_body_create, ph_body_add_hull,
                     ph_body_finish, ph_body_remove, ph_body_get, ph_body_set, ph_body_mass, ph_body_vel, ph_impulse, ph_raycast,
                     ph_explode, ph_raycast_static, ph_step, ph_counts};
}  // namespace

// ---- shared with phys_car_sim.cpp (phys_sim_internal.h)
b3WorldId phsim_world() { return g_world; }
int phsim_add_body(b3BodyId id, void* user, int kind) {
    if (!live() || !b3Body_IsValid(id)) return -1;
    int idx = (int)g_bodies.size();
    b3Body_SetUserData(id, (void*)(intptr_t)(idx + 1));
    Body b; b.id = id; b.used = true; b.has_shape = true; b.user = user; b.kind = kind;
    g_bodies.push_back(b);
    return idx;
}
bool phsim_body(int b, b3BodyId* id, int* kind) {
    Body* p = get(b); if (!p) return false;
    if (id) *id = p->id;
    if (kind) *kind = p->kind;
    return true;
}
int phsim_body_index(b3BodyId id) {
    if (!live() || !b3Body_IsValid(id)) return -1;
    int idx = (int)(intptr_t)b3Body_GetUserData(id) - 1;
    return (idx >= 0 && idx < (int)g_bodies.size() && g_bodies[idx].used) ? idx : -1;
}
void phsim_build_mesh() { build_mesh(); }

extern "C" const PhApi* __cdecl TODPhysics_GetApi(int version) { return version == TODPHYS_API_VERSION ? &g_api : nullptr; }

#ifdef TODPHYS_DLL
#pragma comment(linker, "/EXPORT:TODPhysics_GetApi=_TODPhysics_GetApi")
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
#endif
