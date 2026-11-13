// phys_api.h - the interface of TODPhysics.dll (Box3D inside, no game knowledge), used by TODGameplay.dll's [physics]
// glue (physics/code/phys_game.cpp) through TODPhysics_GetApi. Plain C types only; one simulation world per process.
// Units: metres, kilograms, seconds; y up (the game's world space). Rotations are TRUE rotations (x, y, z, w),
// column-vector convention (phys_logic.h converts to and from the game's stored quaternion).
#pragma once
#include <stdint.h>

#define TODPHYS_API_VERSION 1

typedef struct PhBodyDesc {
    float pos[3], rot[4];
    float lin_damping, ang_damping;
    int   asleep;            // created sleeping (woken by a touch, an impulse or an explosion)
    void* user;              // the caller's handle (returned by raycasts)
} PhBodyDesc;

typedef struct PhCounts { int bodies, awake, static_shapes, static_failed; } PhCounts;

typedef struct PhApi {
    int  version;
    // world
    int  (*create)(float gravity_y, float restitution_threshold);   // 1 = ok (an existing world is destroyed first)
    void (*destroy)(void);
    // static level geometry: one convex hull in world space (n points, x y z), up to 128 kept by the hull builder
    int  (*add_static_hull)(const float* pts, int n, float friction);
    // dynamic bodies: create, add convex hulls in body space, then finish (mass from density, clamped)
    int  (*body_create)(const PhBodyDesc* d);                       // index >= 0, -1 = failed
    int  (*body_add_hull)(int b, const float* pts, int n, float density, float friction, float restitution);
    int  (*body_finish)(int b, float min_mass, float max_mass);    // 1 = ok (the body has a shape)
    void (*body_remove)(int b);
    int  (*body_get)(int b, float pos[3], float rot[4]);           // returns 1 awake, 0 sleeping, -1 invalid
    void (*body_set)(int b, const float pos[3], const float rot[4], int keep_velocity);
    float (*body_mass)(int b);
    void (*body_vel)(int b, float lin[3], float ang[3]);           // velocities (m/s, rad/s); zero when invalid
    // forces
    void (*impulse)(int b, const float imp[3], const float point[3]);
    int  (*raycast)(const float from[3], const float to[3], float hit[3], void** user);   // dynamic bodies only; body index or -1
    int  (*explode)(const float c[3], float radius, float impulse_per_area, float lift);  // bodies touched
    int  (*raycast_static)(const float from[3], const float to[3], float hit[3]);          // level geometry only: 1 hit, 0 none
    // time
    double (*step)(float dt, int substeps);                       // milliseconds spent in b3World_Step
    void (*counts)(PhCounts* out);
} PhApi;

typedef const PhApi* (__cdecl* TODPhysics_GetApi_t)(int version);
