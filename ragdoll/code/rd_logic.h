// rd_logic.h - pure logic of the [ragdoll] feature (TOD Gameplay+): which bones become which bodies,
// capsule sizes from the rig, joint frames and limits, forward kinematics of the game's bone nodes, the pose written
// back from the bodies, motor targets from the animated pose, hit strength / recovery, settle detection and the
// config. No game memory and no Box3D here: unit tested by ragdoll/code/test_rd.cpp on real skeletons
// (rd_fixture.inc, from a renderer-bridge bone probe).
//
// Conventions (the game's, measured by the bridge, design_renderer_bridge.md 3.4):
//   * a bone node's NodeMatrix +00h holds the STORED quaternion = conjugate of the true local rotation, +10h the local
//     position; world = parent o local (true rotations, column-vector), the root parent is the Character node.
//   * every limb bone's long axis is its local -X (child local positions (-len, 0, 0)); elbows and knees bend about
//     the child's local +Y (forearm / calf locals are pure Y rotations, positive = flexion).
#pragma once
#include <stdint.h>
#include "../../physics/code/phys_logic.h"
#include "rd_api.h"

namespace rd {
using ph::V3;
using ph::Q4;

struct Xf { V3 p; Q4 q; };          // rigid transform (true rotation)
Xf    xid();
Xf    xmul(Xf a, Xf b);              // a o b (apply b first)
Xf    xinv(Xf a);
V3    xpoint(Xf a, V3 v);
Q4    qid();
Q4    qaxis(V3 axis, float rad);
Q4    qarc(V3 from, V3 to);          // shortest rotation taking unit vector from onto unit vector to
float qangle(Q4 q);                  // rotation angle in [0, pi]
V3    qlog(Q4 q);                    // axis * angle (shortest)
float hinge_angle(Q4 q);             // signed angle about +z of a (mostly) z rotation
Xf    xf_from_tod(const float m[16], bool* ok);   // a game world matrix (unit scale expected)

enum { MAXB = 64 };
enum Body { PELVIS, SPINE, CHEST, HEAD, L_UPPER, L_FORE, R_UPPER, R_FORE, L_THIGH, L_CALF, R_THIGH, R_CALF, NBODY };
enum JointKind { J_NONE, J_SPHERE, J_HINGE };

struct BodyDef {
    const char* name;
    const char* bone;      // the bone whose frame the body takes (the body's own frame = this bone's world frame)
    const char* end;       // capsule end: this bone's origin ("" = special: pelvis across the hips, head = own axis)
    float end_extra;       // metres added past the end bone along the segment (hands, head top)
    float radius, mass;
    int   parent;          // parent body (-1 for the pelvis)
    int   kind;            // joint to the parent
    float cone, twist;     // spherical: cone half-angle, twist +-limit (radians)
    float lo, hi;          // hinge limits (radians, 0 = straight, + = flexion)
    float friction;        // joint friction torque of a limp body (N m)
};
extern const BodyDef BODIES[NBODY];
const char* body_name(int b);

// ---- skeleton as read from the game (Character +1D8h bone nodes, names at node +4Ch, parents at node +40h)
struct Skeleton {
    int  n;
    int  parent[MAXB];          // bone index, -1 = the Character node
    char name[MAXB][24];
    Xf   local[MAXB];           // current local transforms (true rotations)
    Xf   rest[MAXB];            // bind pose locals (model asset pivots), valid when has_rest
    bool has_rest;
};
struct Map {
    bool ok; char err[96];
    int  body_bone[NBODY], end_bone[NBODY];
    int  driver[MAXB];          // body that drives the bone, -1 = not written (GamePivot, Dummy ...)
    int  order[MAXB], norder;   // driven bones, parents first
    int  bip, l_thigh, r_thigh, neck, head;
};
int  find_bone(const Skeleton& s, const char* name);
bool build_map(const Skeleton& s, Map* m);
// world transforms of every bone from locals (root = the Character node's world transform)
void fk(const Skeleton& s, const Xf* locals, Xf root, Xf* world);

// ---- the ragdoll built from one pose
struct Layout {
    Xf    body0[NBODY];               // body world transforms at creation (= the driving bones' world frames)
    int   ncap[NBODY]; V3 c1[NBODY][2], c2[NBODY][2]; float rad[NBODY][2];
    float mass[NBODY];
    Xf    frameA[NBODY], frameB[NBODY];   // joint of body b (b > 0) to BODIES[b].parent, in body space
    int   kind[NBODY]; float cone[NBODY], twist_lo[NBODY], twist_hi[NBODY], lo[NBODY], hi[NBODY], friction[NBODY];
    Xf    off[MAXB];                  // bone world = driver body world o off[bone] (rotation; Bip also position)
    float hinge_sign[NBODY];          // +1, or -1 when the rig bends the other way (measured, logged)
};
// world = bone world transforms of the creation pose; rest_world = bind pose in the Character node frame (or null:
// joint limits are then centred on the creation pose)
bool build_layout(const Skeleton& s, const Map& m, const Xf* world, const Xf* rest_world, float scale, Layout* L);
// body world frames of an (animated) bone pose: the driving bones' world transforms
void body_frames(const Map& m, const Xf* world, Xf* bodies);
// locals of every driven bone from body transforms (Bip also gets its local position); out[i] untouched for others
void pose_from_bodies(const Skeleton& s, const Map& m, const Layout& L, const Xf* bodies, Xf root, Xf* out);
// joint motor target (frame B relative to frame A) for an animated pose of the bodies; hinges: angle in *angle
Q4   joint_target(const Layout& L, int b, const Xf* anim_bodies, float* angle);
// velocities from two transforms dt apart (clamped)
void velocity(Xf prev, Xf cur, float dt, float vmax, V3* v, V3* w);
// the body nearest to a world point (capsule distance), -1 when farther than maxd
int  nearest_body(const Layout& L, const Xf* bodies, V3 p, float maxd, float* dist);
// segment distance helper (exposed for the tests / the frozen-corpse ray test)
float ray_capsule(V3 from, V3 to, V3 a, V3 b, float r, float* t);

// ---- a Layout into TODPhysics (rd_api.h): bodies at the given transforms / velocities, joints with their limits and
// friction (x friction_scale); out->joint[b] = joint index of body b (b > 0)
struct Spawned { int r; int joint[NBODY]; };
struct Cfg;
// the spine / chest / head cone and twist limits x scale, never tighter than the given pose (+ margin) so a ragdoll made
// from a bent animation does not snap into its limits; returns the narrowest scale used (for the log)
float tighten_spine(Layout* L, const Xf* bodies, float scale, float margin);
// powered: angular damping active_ang_damping (else limp_ang_damping)
bool spawn(const RdApi* A, const Layout& L, const Xf* bodies, const V3* lin, const V3* ang, const Cfg& c,
           float friction_scale, Spawned* out, bool powered = false);

// ---- muscle strength of an active ragdoll: per body 0..1, a hit drops it, it recovers linearly
struct Strength {
    float s[NBODY];
    void reset(float v);
    void hit(int body, float floor_value, float neighbour_value);
    void recover(float dt, float seconds);
    float joint(int b) const;          // strength of the joint of body b (b and its parent)
    float min_value() const;
};

// ---- pushes on a ragdoll (ragdoll-hits): the hit part gets (1 - core_share) of the impulse, at most max_dv (m/s) of
// velocity change for its own mass; the rest goes to the torso body it hangs on (arms / head -> chest, legs -> pelvis),
// so a shot in the arm or the head reads as a flinch of the whole body and not as a 50 m/s whip of a 2 kg forearm
// (the "inflatable tube man" of the first tuning: 110 N s on any part, capped only at 48 m/s).
struct Push { int n; int body[2]; V3 imp[2]; };
int  core_of(int b);
Push share_push(const Layout& L, float mass_k, int b, V3 imp, float max_dv, float core_share);

// ---- per-step drive of a powered (active) ragdoll from its muscle strengths: joint spring hertz per body (b > 0), the
// pelvis balance hertz. Shared by the game (rd_game.cpp) and the offline hit lab (test_rd.cpp RD_HITLAB=1).
struct Cfg;
struct ActiveDrive { float hz[NBODY]; float damping[NBODY]; float balance_hz, balance_damping; };
void active_drive(const Cfg& c, const Strength& s, ActiveDrive* d);

// ---- settle detector (limp ragdolls): every body slower than the thresholds for hold seconds
struct Settle {
    float quiet = 0.0f;
    bool update(float max_speed, float max_spin, float dt, float speed_tol, float spin_tol, float hold);
};

// ---- config ([ragdoll] in TODGameplay.ini), clamped
struct Cfg {
    bool  enabled = false;
    bool  death_ragdoll = true;        // limp ragdoll on death
    bool  active_hits = true;          // powered ragdoll on non-lethal hits
    int   max_ragdolls = 6;            // simulated at once (active + limp); frozen corpses are not counted
    int   max_frozen = 16;             // settled corpses whose pose is kept
    float settle_seconds = 6.0f;       // a limp ragdoll freezes at the latest after this
    float impulse_scale = 1.0f;        // every push on a ragdoll (bullets, blasts, the game's hit impulse)
    float joint_stiffness = 18.0f;     // active ragdoll joint springs (Hz)
    float recover_seconds = 0.7f;      // a hit body part back to full strength
    bool  debug_draw = false;          // log capsule segments + camera for the overlay tool (test copy)
    // tuning (the ini may set them; defaults are the tuned values)
    float scale = 1.0f;                // capsule radius / mass scale
    float total_mass = 80.0f;
    float joint_damping = 2.5f;        // damping ratio of the springs (ragdoll-hits: 1.1 -> 2.5, no swing back)
    float hit_strength = 0.4f;         // strength left in the hit body part (ragdoll-hits: 0.15 -> 0.4)
    float hit_neighbour = 0.75f;       // strength left in the bodies next to it (0.55 -> 0.75)
    float bullet_impulse = 110.0f;     // N s per bullet on a limp body (the killing shot, a shot corpse; x impulse_scale)
    float game_hit_scale = 1.0f;       // x the game's own take_a_hit impulse (x impulse_scale)
    float blast_scale = 1.6f;          // x the physics blast impulse per area on ragdoll bodies (x impulse_scale)
    float blast_lift = 5.0f;           // extra upward speed at a blast centre (m/s)
    float blast_memory = 0.5f;         // seconds a blast still throws a body that dies after it
    float death_fade = 0.35f;          // seconds the muscles follow the death animation before going limp
    float limp_friction = 1.0f;        // x the joint friction torques of a limp body
    float active_gravity = 1.0f;       // gravity scale while powered (0.5 -> 1: half gravity looked floaty, inflatable)
    float balance_hertz = 8.0f;        // pelvis held to the animated pelvis (Hz) ...
    float balance_force = 6000.0f;     // ... up to this force (N): a harder push makes it stagger
    float balance_torque = 4000.0f;    // ... and this torque (N m)
    float balance_damping = 2.0f;      // ... with this damping ratio
    float foot_hertz = 10.0f;          // powered feet held to the animated feet (Hz) ...
    float foot_force = 2500.0f;        // ... up to this force (N): a push bends the body over its planted feet
    float foot_damping = 1.0f;         // ... with this damping ratio (higher: no sway back past the pose)
    float blend_in = 0.05f, blend_out = 0.3f;   // seconds from / back to the animation
    float stumble_distance = 0.30f;    // pelvis pushed this far (m) from the animated one: the character steps along
    float stumble_rate = 2.5f;         // fraction of the offset per second the character is moved
    int   substeps = 8;                // Box3D substeps of the 0.04 s step while ragdolls simulate
    float max_speed = 40.0f;           // clamp of the hand-off velocities (m/s)
    float track_radius = 70.0f;        // characters farther from Ram are not tracked
    bool  root_follow = true;          // a corpse's character node follows the pelvis (culling, LOD, shadow)
    bool  civilians = true;            // civilians too (every character_sentry_ai)
    // live hits and deaths (tuned against the "inflatable tube man" look): a shot is a firm flinch, a death a
    // heavy fall. Blasts keep their own path and strength (blast_scale, blast_lift, blast_strength).
    float hit_impulse = 60.0f;         // N s of a bullet on a powered (live) ragdoll (x impulse_scale; was 110)
    float hit_max_dv = 4.0f;           // the hit part gets at most this velocity change (m/s), the rest goes to the torso
    float hit_core_share = 0.5f;       // share of a live push given to the torso body the part hangs on
    float hit_burst = 1.6f;            // pushes within 0.5 s share a budget of this many hit_impulse
    float active_friction = 4.0f;      // x the joint friction torques while powered (no sway back past the pose)
    float active_ang_damping = 2.0f;   // angular damping of powered bodies (1/s; was 0.6)
    float spine_limit_scale = 0.7f;    // x the spine / chest / neck cone and twist limits (powered and limp)
    float death_max_dv = 4.0f;         // a bullet death push gives one part at most this velocity change (m/s), the rest the torso
    float death_core_share = 0.4f;     // share of a bullet death push given to the torso
    float limp_ang_damping = 1.5f;     // angular damping of limp bodies (1/s; was 0.6); linear damping stays (blasts unchanged)
    float blast_strength = 0.15f;      // strength left in every part of a powered body a blast reaches
    float corpse_seconds = 4.0f;       // the dead character stays this long before the game swaps in its death-pose
                                       // corpse and fades it (vanilla 0.5); its pooled shell is held that long
    int   corpse_max = 3;              // at most this many corpses wait at once (the rest get the vanilla 0.5 s)
    float log_seconds = 5.0f;
    float stall_seconds = 0.3f;        // game s without a physics step: the world counts as stopped (steps come every 0.04 s)
    float stall_fall_seconds = 0.8f;   // a limp corpse simulated this long keeps its pose when the step stops (younger: the game's clip)
    void clamp();
};

// ---- life of a tracked character. A pooled shell handed to a new placement keeps the last occupant's AI
// state 28 for a moment, and a re-armed reinforcement came with its placement's stored, lethal DamageTaken: both were taken
// for deaths (live enemies turned into limp ragdolls pinned to their pelvis, or frozen in their spawn pose).
// alive: not the death state and DamageTaken below Health. A death needs the character seen alive in this placement first,
// and a DamageTaken death (state not 28 yet) needs a hit on it within the last second.
bool life_alive(int ai_state, bool lethal);
bool life_dead(int ai_state, bool lethal, bool recent_hit, bool seen_alive);

// ---- the physics clock (the "enemies freeze" problem). The [physics] world steps in the player's
// input tick, and the game calls that tick only while Ram is on foot and in control: while he drives (or sits in a seat,
// or a script moves him) it stops for tens of seconds while every character keeps updating. A ragdoll started or held in
// that time never moved: a live enemy hit just before stood in its old pose at its old place while its AI walked on, and
// a killed one stood upright as a statue instead of falling (its "frozen" corpse pose had 0.0 s of simulation).
// StepWatch: game milliseconds since the last physics step; the clock going back (a load, a retry, a rewind) is a new base.
struct StepWatch {
    uint32_t last_ms = 0; bool have = false;
    void stepped(uint32_t game_ms) { last_ms = game_ms; have = true; }
    bool stalled(uint32_t game_ms, int limit_ms);
};
enum { RM_NONE = 0, RM_ACTIVE = 1, RM_LIMP = 2, RM_FROZEN = 3 };   // the track modes of rd_game.cpp
enum { SA_KEEP = 0, SA_RELEASE = 1, SA_FREEZE = 2 };
// a track's step when the world has stopped stepping: a powered ragdoll goes back to the animation; a limp corpse that has
// fallen for at least fall_s keeps the pose it reached (frozen), a younger one goes back to the game's own death clip;
// frozen corpses need no step and keep theirs
int stall_action(int mode, float t_sim, float fall_s);

}  // namespace rd
