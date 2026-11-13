// car_logic.h - pure logic of [car_physics] (TOD Gameplay+): the numbers behind cars shoving props, explosions throwing
// cars, and wrecks. No game memory and no Box3D: unit tested by physics/code/test_car.cpp.
// Vectors and rotations use phys_logic.h (TRUE rotations, y up, metres, seconds).
#pragma once
#include "phys_logic.h"

namespace cp {
using ph::V3;
using ph::Q4;

// ---- config ([car_physics] in TODGameplay.ini), clamped. Only active together with [physics] enabled = 1.
struct Cfg {
    bool  enabled = false;
    bool  push_props = true;      // live cars are kinematic boxes in Box3D: they shove the props
    bool  blast_cars = true;      // weapon_shell / destroyable / car explosions throw cars (the game's own car physics)
    bool  wreck_dynamic = true;   // destroyed cars become Box3D bodies: they tumble, flip and settle
    bool  bullets = true;         // bullets nudge and spin wrecks a little
    float impulse_scale = 1.0f;   // one knob for everything: prop kicks, blast speeds and spins
    // pushing props
    float margin = 0.12f;         // the box is this much larger than the car's collision (m, sideways and lengthways)
    float lead = 0.04f;           // the box runs this far ahead in time (s): props move before the game's car meets them
    float push_gain = 0.45f;      // extra prop speed on first touch, as a share of the car's speed
    float push_lift = 3.5f;       // extra upward prop speed (m/s) at push_full_speed and above
    float push_full_speed = 12.0f;
    float push_min_speed = 1.5f;  // below this car speed (m/s) only the contact itself pushes
    float push_spin = 4.0f;       // tumble (rad/s) given to a prop on first touch at push_full_speed
    // explosions -> cars (live cars through the game's own physics state, wrecks through Box3D)
    float blast_gain = 2.0f;      // speed = gain * impulse_per_area * blast_area / mass * falloff
    float blast_area = 8.0f;      // m^2 of a car side facing a blast
    float blast_min_mass = 600.0f, blast_max_mass = 4000.0f;
    float blast_lift_gain = 1.5f; // upward speed = blast_lift * this * falloff + 0.35 * sideways speed
    float blast_spin = 3.5f;      // roll / pitch away from the blast (rad/s) at the centre
    float blast_reach = 1.5f;     // blast radius multiplier (the props use 1.5 too)
    float max_speed = 22.0f;      // cap of the speed change a blast gives a car (m/s)
    float max_spin = 8.0f;        // cap of the spin change (rad/s)
    float self_kick = 0.0f;       // extra jump (m/s) of a car that explodes: the game already throws it ~8 m/s up (run c1)
    float self_spin = 2.5f;       // ... and a random flip (rad/s) the game does not give
    float car_blast_radius = 8.0f, car_blast_impulse = 400.0f;   // a car explosion as a blast for the props and cars around
    // wrecks
    float wreck_mass = 1400.0f;
    float wreck_friction = 0.7f, wreck_restitution = 0.12f;
    float wreck_linear_damping = 0.05f, wreck_angular_damping = 0.25f;
    float wreck_bullet_gain = 10.0f;   // a bullet's impulse on a wreck = [physics] bullet_impulse * this
    // housekeeping
    float teleport_distance = 6.0f;    // a car that jumps further than this in one frame is put there, not swept
    float car_rescan_seconds = 3.0f;   // the scene is walked for cars this often (also at every [physics] scan)
    float car_radius = 70.0f;          // cars within this distance of Ram take part
    int   max_cars = 24;
    void  clamp();
};

// ---- a car's box from its collision points (car frame): half extents + centre offset, grown by margin (x, z)
struct Box { V3 half, center; bool ok; };
Box   box_from_points(const V3* pts, int n, float margin);
Box   default_box();   // a sedan-sized box when the model has no usable collision

// ---- kinematic sweep target: where the box should be at fraction t of the frame (prev -> cur), led by v * lead
struct Pose { V3 p; Q4 q; };
Pose  sweep_target(const Pose& prev, const Pose& cur, float t, V3 v, float lead);
bool  teleported(V3 a, V3 b, float max_dist);

// ---- a blast on a car: the change of linear and angular velocity (world, TRUE rotation convention)
struct Kick { V3 dv, dw; float k; bool hit; };
// c = blast centre, radius / impulse_per_area / lift as the [physics] blast; mass = the car's mass (kg); seed for the
// random part (a blast right under a car still picks a side)
Kick  blast_kick(V3 car_center, float mass, V3 c, float radius, float impulse_per_area, float lift, const Cfg& g, unsigned seed);
// a car that explodes: straight up plus a random flip
Kick  self_kick(const Cfg& g, unsigned seed);
// a prop first touched by a moving car: speed change for the prop (m/s), spin (rad/s); hit = false when too slow
Kick  push_kick(V3 car_vel, V3 normal, const Cfg& g, unsigned seed);

// ---- velocities from two poses dt apart
V3    lin_vel(V3 a, V3 b, float dt);
V3    ang_vel(Q4 a, Q4 b, float dt);   // world-space angular velocity taking a to b (shortest arc)

unsigned hash(unsigned x);
}  // namespace cp
