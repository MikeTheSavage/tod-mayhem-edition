// phys_logic.h - pure logic of the [physics] prototype (TOD Gameplay+): TOD transform conventions <->
// rigid-body transforms, VertexCloud shape preparation, prop selection, impulse numbers, step clock, statistics.
// No game memory and no Box3D here: unit tested by physics/code/test_phys.cpp.
#pragma once
#include <stdint.h>

namespace ph {

struct V3 { float x, y, z; };
struct Q4 { float x, y, z, w; };   // a TRUE rotation (column-vector convention, the one Box3D uses)

V3 v3(float x, float y, float z);
V3 add(V3 a, V3 b);
V3 sub(V3 a, V3 b);
V3 mul(V3 a, float s);
float dot(V3 a, V3 b);
V3 cross(V3 a, V3 b);
float len(V3 a);
V3 norm(V3 a);

Q4 qmul(Q4 a, Q4 b);
Q4 qconj(Q4 q);
Q4 qnorm(Q4 q);
V3 qrot(Q4 q, V3 v);
Q4 qslerp(Q4 a, Q4 b, float t);   // shortest arc; nlerp when nearly equal

// ---- TOD matrices: 16 floats, row-vector form p_world = p_local * M; rows 0..2 = the local x/y/z axes in world
// space (may carry a scale), row 3 = the position (FUN_004842C0, design_renderer_bridge.md 3.2).
// The node's stored quaternion (NodeMatrix +0x00, SetOrient 0x88DB20) is the CONJUGATE of the true rotation.
struct Rigid { V3 p; Q4 q; V3 scale; bool ok; };
Rigid rigid_from_tod(const float m[16]);          // splits rotation / scale / position; ok = false when degenerate or mirrored
void  tod_from_rigid(V3 p, Q4 q, float out[16]);  // unit scale
V3    tod_point(const float m[16], V3 local);     // p_local * M
Q4    stored_from_true(Q4 q);                     // the value SetOrient wants
Q4    true_from_stored(Q4 s);
// local transform under a parent (world = parent o local): the parent's rotation + uniform scale are removed
bool  local_from_world(const float parent[16], V3 wp, Q4 wq, V3* lp, Q4* lq);

// ---- VertexCloud preparation (subsystem_collision.md 3.3: every shape collides as the convex hull of its vertices)
// A shape thinner than min_thickness (single triangles, flat terrain / wall patches) becomes a slab: the points plus a
// copy pushed along -n by thickness (n = the plane normal turned to +y for floors; both ways by half for walls).
// out must hold 2*n points; returns the point count (n or 2n), 0 when degenerate (fewer than 3 distinct points).
int   prepare_hull(const V3* pts, int n, float min_thickness, float slab, V3* out);
float thickness(const V3* pts, int n, V3* normal);   // extent along the best plane normal (Newell over a fan)

// ---- prop selection: case-insensitive substring patterns separated by commas ("barrel,crate,box")
bool  name_match(const char* name, const char* patterns);
struct PropRule { float min_radius = 0.12f, max_radius = 1.6f; };
bool  prop_size_ok(const PropRule& r, float bound_radius);

// ---- impulses
// bullet: impulse along the shot, at the hit point (N s)
V3    bullet_impulse(V3 from, V3 to, float strength);
// a body's lift toward the centre for explosions: Box3D's area model plus an upward kick so things fly (m/s)
float blast_falloff(float dist, float radius);   // 1 at the centre, 0 at radius, linear

// ---- fixed-step clock with interpolation (physics 0.04 s like the game's characters and cars)
struct StepClock {
    float step = 0.04f, acc = 0.0f;
    int   max_steps = 4;
    int   advance(float dt);                     // number of steps to run now (acc keeps the rest; excess dropped)
    float alpha() const { return step > 0.0f ? acc / step : 0.0f; }
};

// ---- timing statistics (milliseconds)
struct Stats {
    double sum = 0, mx = 0; int n = 0;
    void add(double ms) { sum += ms; if (ms > mx) mx = ms; ++n; }
    double avg() const { return n ? sum / n : 0.0; }
    void reset() { sum = mx = 0; n = 0; }
};

// ---- config ([physics] in TODGameplay.ini), clamped
struct Cfg {
    bool  enabled = false;
    float radius = 40.0f;            // static world + props within this distance of Ram at a scan
    float rescan_distance = 18.0f;   // Ram this far from the scan centre (and nothing moving) -> scan again
    int   max_bodies = 64;
    float step = 0.04f; int substeps = 4;
    float gravity = 9.81f;
    float density = 40.0f;           // kg/m^3 of a prop's hull volume (hollow crates, bins, chairs)
    float min_mass = 3.0f, max_mass = 80.0f;
    float friction = 0.6f, restitution = 0.15f;
    float linear_damping = 0.05f, angular_damping = 0.15f;
    float bullet_impulse = 25.0f;    // N s per bullet hit (a shotgun shot is 10 of them)
    float shell_radius = 7.0f, shell_impulse = 450.0f;          // weapon_shell explosions (grenade, rocket, dynamite)
    float destroy_radius = 6.0f, destroy_impulse = 350.0f;      // Destroyable_Object explosions (red barrels ...)
    float blast_lift = 4.0f;         // extra upward speed (m/s) at the centre of a blast
    float slab = 0.25f, min_thickness = 0.02f;
    bool  interpolate = true;
    bool  start_asleep = true;       // props sleep until something touches them (no settling jump at a scan)
    float log_seconds = 5.0f;
    bool  drive_step = true;         // the world also steps every frame while Ram is in a vehicle
    float drive_lead = 0.8f;         // ... and a scan there is centred this many seconds of car travel ahead of him
    void  clamp();
};

// ---- the vehicle frame drive. The world steps in the on-foot input tick (KB_PROC / PAD_PROC in the play state
// 0x740560); while Ram drives the game does not run that tick, so props, wrecks and ragdolls stood still until he got out.
// The DLL's per-pass hook (POST_INPUT) then steps the world itself, paced by the game-ms clock [0xA3DCD4] (REF PS_GAMEMS):
// it stands while paused or loading, goes back on a rewind / load.
struct DriveGate {
    uint32_t last_ms = 0; bool have = false;
    // game ms since the last look; 0 = the clock stood (paused, loading) or went back / jumped (rewind, load: a new base)
    uint32_t advance(uint32_t game_ms);
};
enum { DRV_NONE = 0, DRV_STEP = 1, DRV_REWIND = 2 };
// foot_ticked: the on-foot tick ran in the last pass (it steps the world itself); ms: DriveGate::advance; dt: the game dt
// [0xA3DCC4] (a slow motion below 1 ms a frame leaves the truncated ms clock standing); playmode: 2 paused, 3 rewind
int drive_decision(bool enabled, bool foot_ticked, uint32_t ms, float dt, int playmode, bool cutscene, bool in_vehicle);
// where a scan goes while driving: Ram's position plus lead seconds of his velocity, at most max_ahead metres
V3 drive_scan_point(V3 ram, V3 vel, float lead, float max_ahead);

}  // namespace ph
