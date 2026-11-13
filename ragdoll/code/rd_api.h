// rd_api.h - the ragdoll part of TODPhysics.dll (ragdoll/code/rd_sim.cpp, Box3D inside, no game knowledge), used by
// TODGameplay.dll's [ragdoll] glue (ragdoll/code/rd_game.cpp) through TODPhysics_GetRagdollApi. It lives in the SAME
// Box3D world as the [physics] props and the static level mesh (phys_sim.cpp), so ragdolls land on the level and
// knock props about. Plain C types; metres, kilograms, seconds, y up; rotations are TRUE rotations (x, y, z, w).
#pragma once
#include <stdint.h>

#define TODRAG_API_VERSION 2   // 2 (ragdoll-hits): + damping; version 1 callers get the same table
#define TODRAG_MAX_BODIES 16

typedef struct RdBodyDesc {
    float pos[3], rot[4];            // world transform of the body frame
    float lin_vel[3], ang_vel[3];    // hand-off velocities (world, rad/s)
    int   ncap; float c1[2][3], c2[2][3], radius[2];   // capsules in body space
    float mass;                      // kg (inertia from the capsules, scaled)
    float lin_damping, ang_damping, friction, restitution;
} RdBodyDesc;

enum { RD_JOINT_SPHERE = 1, RD_JOINT_HINGE = 2 };
typedef struct RdJointDesc {
    int   body_a, body_b, kind;      // body indices of this ragdoll (a = parent)
    float frame_a_pos[3], frame_a_rot[4], frame_b_pos[3], frame_b_rot[4];   // joint frames in body space
    float cone, twist_lo, twist_hi;  // spherical: cone about frame A z, twist about frame B z (radians)
    float lo, hi;                    // hinge: limits about frame z (radians)
    float friction;                  // N m (a motor at zero speed)
} RdJointDesc;

typedef struct RdApi {
    int version;
    int  (*create)(void);                                         // ragdoll handle >= 0, -1 = no world / full
    int  (*add_body)(int r, const RdBodyDesc* d);                 // body index, -1 failed
    int  (*add_joint)(int r, const RdJointDesc* d);               // joint index (= creation order), -1 failed
    void (*destroy)(int r);
    int  (*valid)(int r);                                         // 1 while its bodies exist (a new world kills them)
    int  (*get)(int r, int b, float pos[3], float rot[4]);        // 1 awake, 0 sleeping, -1 invalid
    void (*vel)(int r, int b, float lin[3], float ang[3]);
    void (*impulse)(int r, int b, const float imp[3], const float point[3]);
    void (*set_vel)(int r, int b, const float lin[3], const float ang[3]);
    // spring toward a target: spherical = rotation of frame B relative to frame A, hinge = angle; hertz 0 = off
    void (*drive)(int r, int j, float hertz, float damping, const float target[4], float angle);
    void (*friction)(int r, int j, float max_torque);
    // pull body b toward a world transform (kinematic anchor + motor joint springs); lin_hertz 0 removes the anchor
    void (*anchor)(int r, int b, const float pos[3], const float rot[4], float lin_hertz, float ang_hertz, float damping,
                   float max_force, float max_torque, float dt);
    void (*gravity)(int r, float scale);
    void (*wake)(int r);
    // dynamic ragdoll bodies only: ragdoll handle (or -1), *body = body index
    int  (*raycast)(const float from[3], const float to[3], float hit[3], int* body);
    // Box3D's area explosion on ragdoll bodies + an upward kick; returns the number of ragdolls reached
    int  (*explode)(const float c[3], float radius, float impulse_per_area, float lift);
    int  (*count)(void);                                          // ragdolls alive
    // version 2: linear / angular damping (1/s) of every body of a ragdoll (a powered body that dies goes limp)
    void (*damping)(int r, float lin, float ang);
} RdApi;

typedef const RdApi* (__cdecl* TODPhysics_GetRagdollApi_t)(int version);
