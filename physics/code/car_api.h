// car_api.h - the [car_physics] part of TODPhysics.dll (physics/code/phys_car_sim.cpp), next to phys_api.h: kinematic car
// boxes that push the props, dynamic wreck boxes, contact reports. Same world and the same body indices as phys_api.h
// (body_get / body_set / body_vel / impulse / body_remove work on these bodies too). Plain C types only.
// Units: metres, kilograms, seconds; y up; TRUE rotations (x, y, z, w) as in phys_api.h.
#pragma once
#include <stdint.h>

#define TODPHYS_CAR_API_VERSION 1

// a car (kinematic) or wreck (dynamic) began touching a prop during the last step
typedef struct PhTouch {
    int car;           // body index of the car / wreck
    int prop;          // body index of the prop
    float point[3];    // a contact point (world), the centre of the manifold
    float normal[3];   // from the car toward the prop
} PhTouch;

typedef struct PhCarApi {
    int version;
    // a live car: one box (half extents, centre offset in the car frame), moved only by kin_move; index or -1
    int  (*kin_create)(const float half[3], const float off[3], const float pos[3], const float rot[4], void* user);
    // the box goes to pos/rot over dt (Box3D derives its velocity, so it shoves props); teleport = put it there at once
    void (*kin_move)(int b, const float pos[3], const float rot[4], float dt, int teleport);
    // a wreck: one dynamic box with the given mass; index or -1
    int  (*wreck_create)(const float half[3], const float off[3], const float pos[3], const float rot[4], float mass,
                         float friction, float restitution, float lin_damping, float ang_damping, void* user);
    void (*set_vel)(int b, const float lin[3], const float ang[3]);     // wakes the body
    // begin-touch reports of cars / wrecks against props since the last call (call after every step); count written
    int  (*touches)(PhTouch* out, int cap);
    // closest wreck along a segment: body index or -1
    int  (*raycast_wreck)(const float from[3], const float to[3], float hit[3]);
    int  (*kind)(int b);                                                // 0 prop, 1 car, 2 wreck, -1 none
} PhCarApi;

typedef const PhCarApi* (__cdecl* TODPhysics_GetCarApi_t)(int version);
