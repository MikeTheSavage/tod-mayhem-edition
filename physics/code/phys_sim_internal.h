// phys_sim_internal.h - what phys_sim.cpp (the [physics] Box3D world) shares with phys_car_sim.cpp ([car_physics]):
// and with rd_sim.cpp ([ragdoll], world + level mesh only): one world, one body list (indices are shared with phys_api.h's body functions), the collision categories.
// Inside TODPhysics.dll (and the unit tests) only; TODGameplay.dll talks to the DLL through phys_api.h / car_api.h.
#pragma once
#include "box3d/box3d.h"
#include "box3d/math_functions.h"

// floats handed in from the game -> Box3D: a NaN / inf (a broken matrix, a division by zero upstream) never reaches the
// world. Box3D's tree descent took a wrong branch on a NaN box and could loop forever (the game thread hung); that is
// patched in third_party/box3d/src/dynamic_tree.c too, this keeps the boxes finite in the first place.
#define PHSIM_MAX_COORD 1.0e5f   // the levels are a few km across; 100 km is never a real position, size or speed
inline float phsim_f(float f) { return (f == f && f <= PHSIM_MAX_COORD && f >= -PHSIM_MAX_COORD) ? f : (f == f ? (f > 0 ? PHSIM_MAX_COORD : -PHSIM_MAX_COORD) : 0.0f); }
inline b3Vec3 phsim_v(const float* p) { b3Vec3 v = {phsim_f(p[0]), phsim_f(p[1]), phsim_f(p[2])}; return v; }
inline bool phsim_finite(const float* a, int n) { for (int i = 0; i < n; ++i) if (!(a[i] == a[i]) || a[i] > 1.0e30f || a[i] < -1.0e30f) return false; return true; }
// a pose / velocity read back for the game: a non-finite one is never handed out (getters report the body invalid / zero)
inline void phsim_zero_if_bad(float* a, int n) { if (!phsim_finite(a, n)) for (int i = 0; i < n; ++i) a[i] = 0.0f; }
inline b3Quat phsim_q(const float* q) {   // non-finite -> identity (b3NormalizeQuat also gives identity for ~0)
    for (int i = 0; i < 4; ++i) if (!(q[i] == q[i]) || q[i] > 1.0e6f || q[i] < -1.0e6f) return b3Quat_identity;
    b3Quat r; r.v.x = q[0]; r.v.y = q[1]; r.v.z = q[2]; r.s = q[3]; return b3NormalizeQuat(r);
}

// collision categories (b3Filter): the static level collides with props and wrecks; props with everything;
// kinematic cars with props and wrecks (Box3D never collides kinematic with static); wrecks with everything
#define PHSIM_CAT_STATIC 1ull
#define PHSIM_CAT_PROP   2ull
#define PHSIM_CAT_RAGDOLL 4ull   // [ragdoll] capsules (ragdoll/code/rd_sim.cpp): the static level, props and each other
#define PHSIM_CAT_CAR    16ull   // 8 is free

#define PHSIM_CAT_WRECK  32ull

// body kinds in the shared list
#define PHSIM_KIND_PROP  0   // phys_api.h body_create: dynamic prop
#define PHSIM_KIND_CAR   1   // car_api.h kin_create: kinematic box driven by a live car's own motion
#define PHSIM_KIND_WRECK 2   // car_api.h wreck_create: dynamic box of a destroyed car

b3WorldId phsim_world();                                   // b3_nullWorldId without a world
int  phsim_add_body(b3BodyId id, void* user, int kind);    // registers a body created on phsim_world(): index or -1
bool phsim_body(int b, b3BodyId* id, int* kind);           // false: no such live body
int  phsim_body_index(b3BodyId id);                        // index of a registered body, -1 otherwise
void phsim_build_mesh();                                   // the level mesh is (re)built lazily: before queries
