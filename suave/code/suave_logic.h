// suave_logic.h - the pure logic of the "Suave" loco move (power-up slot 11) in TODGameplay.dll: no game memory, no
// Windows calls. Unit tested by suave/code/test_suave.cpp (built and run by dll/build.bat). The game glue
// (suave/code/suave_game.cpp) reads a snapshot of the game, calls Suave::tick and turns the returned actions into the
// game's own calls (docs: suave/README.md, dll/HOOKS.md section "Suave").
//
// Timeline (every number is in TODGameplay.ini [suave]; B = one beat = 60 / bpm s; c = the move's clock in GAME
// seconds, c = 0 when the activation freeze ends = music start (a downbeat of the segment) = intro clip frame 0):
//   FREEZE  real 0..freeze_seconds  charge spent by the game, armor (god mode), partner + ring taken, world slowed,
//                                   camera push-in on Ram, faces: Ram slicksmile, partner surprise
//   INTRO   c 0..intro_beats        Ram suave_intro_<p>, partner suave_grabbed_<p>; camera push-in / over Ram's
//                                   shoulder / two-shot; the ring starts walking gather_lead_beats before the end
//   GATHER  ..ring formed           pair dances (lead / follow from the intro's end); high orbiting wide shot
//   DANCE   ..release               ring claps; camera cuts every cut_beats: low hero / over the partner's shoulder /
//                                   a ring member's view / wide
//   PLANT   release..+dive_at       push / stumble clips, dynamite from Ram's hand into the partner's hand, close-up then the low
//                                   front angle on Ram; optional ending slow motion
//   DIVE    ..takeoff (+ shots)     Ram released, turned away from the partner by the game's own turning (the dive clock
//                                   waits), then the game's own shoot dodge (injected, or the player's);
//                                   in the air: cut to the partner looking down at the dynamite (suave_lookdown_<p>),
//                                   cut back to the front angle, the explosion while Ram is airborne
//   AFTER   ..landing               ring thrown, Ram lands; camera blends back to gameplay; armor ends; survivors'
//                                   AI back
#pragma once
#include <stdint.h>

namespace sv {

struct Vec3 { float x = 0, y = 0, z = 0; };
Vec3  v3(float x, float y, float z);
Vec3  add(const Vec3& a, const Vec3& b);
Vec3  sub(const Vec3& a, const Vec3& b);
Vec3  mul(const Vec3& a, float k);
Vec3  flat_dir(const Vec3& from, const Vec3& to, const Vec3& fallback);   // unit xz direction (y = 0)
float dist(const Vec3& a, const Vec3& b);
float dist_xz(const Vec3& a, const Vec3& b);
float yaw_to(const Vec3& from, const Vec3& to);     // radians, atan2(dx, dz): 0 = +z, +pi/2 = +x
float yaw_of(const Vec3& dir);                      // atan2(dir.x, dir.z)
float signed_yaw(const Vec3& from, const Vec3& to);  // yaw_of(to) - yaw_of(from), wrapped to -pi..pi

// ------------------------------------------------------------------------------------------------ the dive (fourth test)
// TOD's movement keys are Ram-relative. The dive key for the wanted world direction d with Ram facing f (flat units):
// forward when d is within cone_deg of f, back when within cone_deg of -f, else the side (side_sign +1: D dives
// toward right_of(f)). Returns 0 W, 1 S, 2 A, 3 D; *world = the direction that key dives, *cam = where the low
// front camera stands: his face for a forward or side dive, his profile for a back dive.
enum { DIVE_W = 0, DIVE_S, DIVE_A, DIVE_D };
int dive_key_rel(const Vec3& f, const Vec3& d, float side_sign, float cone_deg, Vec3* world, Vec3* cam);
// Turning Ram through the game's own yaw (mouse turn / PhysicCtrl.turn_speed: the physics turns him by the requested
// degrees on its next step). One request at a time; once his measured facing moved, the next request corrects the
// rest, and a request that made it worse flips the sign (the yaw sign of turn_speed is not proven statically).
// Fifth test: a request is consumed over several physics steps, so the next request waits until his facing has
// settled (two still frames), and the sign is judged by the SIGNED yaw change (|error| across the wrap misled it);
// the yaw sign of the mouse turn measured in test 5 is +1 (deg > 0 turns yaw_of up).
struct TurnCtl {
    bool  on = false; Vec3 want; float sign = 1.0f; float tol = 0.26f; float t = 0.0f, max_t = 1.0f;
    bool  kicked = false; float err_kick = 0.0f, yaw_kick = 0.0f, yaw_prev = 0.0f; int wait = 0, still = 0, kicks = 0, flips = 0;
    void  begin(const Vec3& want_dir, float tol_rad, float max_seconds);
    // every frame with Ram's facing: 0 still turning (*deg = degrees to request now, 0 = none), 1 turned, 2 out of time
    int   step(const Vec3& facing, float dt, float* deg);
    float err(const Vec3& facing) const { return signed_yaw(facing, want); }
};

// ------------------------------------------------------------------------------------------------ pairs (the Suave clips)
enum Pair { PAIR_SOLDIER = 0, PAIR_CIVIL = 1, PAIR_DEA = 2 };
// "dea_taskforce" -> DEA, "civil" -> CIVIL, else SOLDIER (the clip fallback); *matched = one of the three was named
Pair pair_from_model(const char* model_path, bool* matched = nullptr);
const char* pair_name(Pair p);                      // "soldier" / "civil" / "dea" (the %s of the clip names)
// A model none of the three pairings names: the closest pairing by the partner's limb lengths (pose-independent sums
// of the left arm UpperArm-Forearm-Hand and leg Thigh-Calf-Foot segments, metres; every TOD human shares the Biped
// layout). Reference values from the bind poses of the pairing skeletons (suave/anim/clips/skel_*.json).
Pair pair_by_limbs(float arm, float leg);

// Facial expressions: database_facial_ID Specific_ID of AnimDict PlayerAnims/Facials (data, overdose.main)
enum Face { FACE_ANGER = 0, FACE_DEFAULT = 1, FACE_DISBELIEF = 2, FACE_JOY = 3, FACE_PAIN = 4, FACE_SCEPTIC = 5,
            FACE_SLICKSMILE = 6, FACE_SURPRISE = 7 };

// ------------------------------------------------------------------------------------------------ config
struct Cfg {
    bool  enabled = false;
    // selection
    float grab_radius = 5.0f;       // partner: nearest hostile within this (m, 3D)
    float pull_radius = 15.0f;      // ring: hostiles within this join (m)
    int   ring_max = 8;             // at most this many ring members (0..8)
    float ring_radius = 3.0f;       // ring slots on this circle around Ram (m)
    float max_height = 3.0f;        // ignore characters more than this above / below Ram (other floors)
    // tempo and timeline (beats of the song; B = 60 / bpm)
    float bpm = 124.17f;
    float freeze_seconds = 0.6f;    // REAL seconds of the activation freeze (push-in)
    float freeze_scale = 0.05f;     // world time scale during the freeze
    float freeze_ramp_in = 0.08f, freeze_ramp_out = 0.15f;
    float intro_beats = 6.0f;       // suave_intro / suave_grabbed (they end on the dance's first frame)
    float gather_lead_beats = 2.0f; // the ring starts walking this many beats before the intro ends
    float gather_beats = 4.0f;      // walk-in time; late members are placed on their slot
    float orbit_beats = 4.0f;       // high orbiting wide shot after the intro
    float cut_beats = 4.0f;         // dance camera cuts on bar lines
    float dance_beats = 24.0f;      // one dance cycle (24 beats = 11.597 s)
    int   dance_cycles = 1;
    float release_beat = 19.0f;     // release (push) on this beat of the last cycle (frame 230 of 290)
    float attach_seconds = 0.4f;    // dynamite handed to the partner this long after the release (push frame 10)
    float dynamite_show = 0.8f;     // the dynamite appears in Ram's hand this long before the release (he shows it)
    float push_seconds = 2.2f;      // push / stumble length (55 frames); the partner's look-down follows the stumble
    // intro placement: Ram's Bip at intro frame 0 is at (intro_x, intro_z) of the shared node (+x right, +z
    // facing); the node goes where his Bip must land = where Ram stands. face_offset = the partner's start direction
    // seen from Ram's start (atan2(1.08 - 0.23, 0.58 + 0.28) = 44.7 deg).
    bool  intro_place = true;
    float intro_x = 0.23f, intro_z = -0.28f;
    float face_offset_deg = 44.7f;
    float closeup_seconds = 1.1f;   // plant close-up (the hand rests on the back for push frames 0..28)
    float dive_at_seconds = 1.4f;   // Ram released (and the dive injected) this long after the release
    float ending_scale = 0.3f;      // world slow motion from the front angle on (0 = leave it to the dive's own)
    float plant_scale = 0.4f;       // world slow motion from the dynamite plant (the close-up reads; 0 = none)
    float dive_scale = 0.15f;       // world slow motion at the takeoff (the in-air cuts and the blast; landing ends it)
    float takeoff_fallback = 0.45f; // real s after the injected dive keys: count as airborne even if not detected
    float front_height = 1.2f;      // height (m) of the front cameras above Ram's root (shots 4b and 6)
    float front_distance = 5.0f;    // the low front camera this far ahead of Ram along his ACTUAL dive (fifth test)
    float dive_cam_distance = 4.5f; // the in-air front camera (moves with Ram) this far ahead of him
    float ending_ramp = 0.2f;
    // the dive and the explosion (after the takeoff: the earlier of the real and the game time wins)
    bool  auto_dive = true;         // true: the game's forward shoot dodge is triggered for the cinematic
    float dive_wait = 1.5f;         // game s without a takeoff -> the explosion anyway
    // the panic cut (sixth round: 0.45 s on screen was too short to notice). After the takeoff: the cut to the partner
    // looking down at the bundle at lookdown_real / _game; it stays panic_min .. panic_max real s and cuts back once Ram
    // is panic_lift m up (the jump leaves the floor about 1.3 s real after the keys); Ram's own animation runs at
    // panic_ram_speed while he is airborne in it (the world slow motion does not slow the player), ramps back at the
    // cut back; the blast blast_after real s later, while he is still in the air. boom_real / _game: the latest blast.
    float lookdown_real = 0.3f, lookdown_game = 0.3f;
    float panic_min = 1.4f, panic_max = 2.2f, panic_lift = 0.5f, panic_ram_speed = 0.3f, blast_after = 0.3f;
    float boom_real = 4.5f, boom_game = 1.5f;
    float land_hold = 0.5f;         // real s after the landing before the camera blends back
    float after_max = 6.0f;         // real s after the explosion at the latest (the dive's slow motion is long)
    float god_margin = 2.0f;        // extra armor seconds beyond the planned end (the end cuts the rest)
    // ring behaviour
    float arrive_distance = 0.6f;   // a walking member counts as arrived within this of its slot (m, horizontal)
    bool  place_late_members = true;
    float ring_phase_beats = 1.0f;  // circle loop phase offset per member (one sway per beat -> 1 beat)
    // input / camera / faces
    bool  lock_input = true;        // keyboard + mouse buttons ignored while Ram dances (Escape still works)
    bool  camera = true;            // scripted camera shots (off: the gameplay camera stays)
    float camera_blend_back = 1.0f;
    bool  faces = true;             // facial expressions per phase
    float face_stay = 4.0f;         // seconds a face holds (the face controller returns to default afterwards)
    // boom
    bool  dynamite = true;          // the game's dynamite shell on the partner (fuse sparks, the explosion effect)
    float boom_radius = 7.0f;       // blast radius (m) around the partner; must cover the ring
    float boom_damage = 10000.0f;   // take_a_hit damage of the kill pass (owner = Ram: full damage, combo credit)
    float boom_impulse = 15.0f;     // push-back like a barrel blast (x (R - d) / R, + up)
    float boom_slowmo = 0.5f;       // TIME_CTRL do_explosion size (0 = none)
    bool  shake = true;             // camera shake at the boom
    // music: segment per partner model (0 = random among 1..music_segments)
    bool  music = true;
    int   music_segments = 4;
    int   music_soldier = 1, music_civil = 2, music_dea = 3, music_other = 4;
    // robustness
    bool  abort_on_partner_death = true;   // before the release; afterwards the ending goes on
    float partner_drift = 0.05f;    // partner node re-placed on Ram's when it drifted more than this (m)
    float partner_yaw_drift = 0.035f;      // ... or turned more than this (rad, 2 deg)

    float beat() const { return 60.0f / (bpm > 1.0f ? bpm : 124.17f); }
    float intro_end() const { return intro_beats * beat(); }
    float gather_start() const { float g = (intro_beats - gather_lead_beats) * beat(); return g > 0 ? g : 0; }
    float gather_end() const { return gather_start() + gather_beats * beat(); }
    float orbit_end() const { return intro_end() + orbit_beats * beat(); }
    float release_t() const { return intro_end() + ((dance_cycles - 1) * dance_beats + release_beat) * beat(); }
    float attach_t() const { return release_t() + attach_seconds; }
    float closeup_end() const { return release_t() + closeup_seconds; }
    float dive_t() const { return release_t() + dive_at_seconds; }
    float planned_end() const { return dive_t() + dive_wait + boom_game + 1.0f; }   // game s, for the armor
    float god_seconds() const { return freeze_seconds + planned_end() + after_max + god_margin; }
    int   segment_for(Pair p, bool matched) const;
    void  clamp();                  // keep every value in a sane range (called after loading)
};

// ------------------------------------------------------------------------------------------------ selection
struct Cand {                       // a character found around Ram (read by the glue)
    Vec3 pos;
    bool hostile = false;           // FactionRelations[player][faction] == 4 (the Tornado's test)
    bool alive = false;             // Health > DamageTaken and not in the death state 28
    bool killable = true;           // root Sub_Type != 8 (take_a_hit never damages 8)
    bool has_ai = false;            // root data +0x58 AICtrl is a character_sentry_ai
};
enum { MAX_RING = 8, MAX_CAND = 64 };
struct Choice { int partner = -1; int members[MAX_RING]; int n = 0; };
Choice choose(const Cfg& c, const Vec3& ram, const Cand* cand, int ncand);

// Ring slots: members keep their circular order around the centre (no crossing paths); the ring is rotated to the
// circular mean of their angles. Slot j of the sorted order gets angle a0 + j * 2 pi / n. slot[i] is member i's slot.
void ring_slots(const Cfg& c, const Vec3& centre, const Vec3* member_pos, int n, Vec3* slot, float* face_yaw);

// ------------------------------------------------------------------------------------------------ the move
enum Phase { P_OFF = 0, P_FREEZE = 1, P_INTRO = 2, P_GATHER = 3, P_DANCE = 4, P_PLANT = 5, P_DIVE = 6, P_AFTER = 7 };
const char* phase_name(int p);
enum MemberState { M_NONE = 0, M_WAIT = 1, M_WALK = 2, M_CLAP = 3, M_DEAD = 4, M_RELEASED = 5 };

// Camera shots (the glue keeps one free camera node; eye positions come from here, targets are bone nodes)
enum Shot { SH_NONE = 0, SH_PUSHIN, SH_OTS_PARTNER, SH_TWO, SH_ORBIT, SH_HERO_LOW, SH_OTS_RAM, SH_MEMBER_POV, SH_WIDE,
            SH_PLANT_CLOSE, SH_FRONT_LOW, SH_LOOKDOWN, SH_FRONT_DIVE, SH_COUNT };
const char* shot_name(int s);
enum Target { TGT_RAM_HEAD = 0, TGT_RAM_CHEST, TGT_RAM_PELVIS, TGT_PARTNER_HEAD, TGT_PARTNER_CHEST, TGT_PARTNER_PELVIS };

struct Snap {                       // the game, read by the glue before every tick
    bool  ok = false;               // pointers valid, the player in control (input tick running)
    bool  paused = false;           // Scene PlayMode 2
    bool  cutscene = false;
    bool  vehicle = false;          // Ram's MoveCtrl runs the inside-vehicle controller
    bool  rewind = false;           // Scene PlayMode 3
    bool  player_dead = false;
    bool  level_changed = false;    // mission index or Ram's root changed since the start
    bool  camera_lost = false;      // another system took the camera (mission shot, vehicle, death hold)
    float time_mult = 1.0f;         // Scene TimeMultiplier
    Vec3  ram, ram_fwd;             // Ram root position, unit forward (xz)
    bool  ram_diving = false;       // Ram's MoveState is a shoot-dodge flight state
    bool  dive_turning = false;     // the glue is still turning Ram before the dive keys (the dive clock waits)
    Vec3  partner_chest;            // partner's Spine2 (world); = partner_body when unknown
    float ram_height = 0.0f;        // Ram's root above the takeoff floor (m) once the dive keys are in
    float free_min = 99.0f;         // the glue's free-space probe at the start: the nearest wall around Ram (m)
    bool  dive_known = false;       // the dive key is chosen: dive_world = the world direction it dives,
    Vec3  dive_world, dive_cam;     // dive_cam = the side the low front camera goes (Ram's face, or his profile)
    bool  partner_alive = false; Vec3 partner; float partner_yaw_err = 0;   // node, |yaw(partner) - yaw(Ram)|
    Vec3  partner_body;             // partner's pelvis bone (world); = partner when unknown
    bool  member_alive[MAX_RING] = {}; Vec3 member[MAX_RING];
};

enum ActKind {
    A_NONE = 0,
    A_FREEZE_ON,        // f0 scale, f1 ramp (real s)
    A_FREEZE_OFF,       // f1 ramp
    A_SLOWMO,           // f0 scale, f1 ramp (the ending slow motion; scale 1 = back to normal)
    A_GOD_ON,           // f0 seconds (GoodieCtrl induce_god_mode: the armor)
    A_GOD_CUT,          // f0 seconds to take back from the god time
    A_HUD_START, A_HUD_END,
    A_STOP_OTHER_MUSIC, // El Mariachi's stream may still play on the slot we borrow
    A_MUSIC_START,      // i segment (1-based)
    A_MUSIC_STOP,       // stop our stream; the glue restores the borrowed SoundSlot when it is safe
    A_RAM_TAKE,         // park Ram's MoveCtrl, weapon special-move state, speed 1.0; turn to yaw f3, node to f0..f2
    A_RAM_RELEASE,      // Normal controller, weapon back, speed back
    A_PARTNER_TAKE,     // AI off, collision off, park, place on Ram
    A_PARTNER_PLACE,    // re-place the partner node on Ram's (drift)
    A_PARTNER_FREEZE,   // stop following Ram (after the release the partner stays where the push leaves him)
    A_PLAY,             // who: -1 Ram, 0 partner, 1..8 member who-1; i clip (CLIP_*), f0 start PlayPos (0..1)
    A_FACE,             // who as A_PLAY; i Face; f0 stay seconds
    A_MEMBER_TAKE,      // who 1..8: AI off, weapon lowered
    A_MEMBER_WALK,      // walk to its slot (f0..f2)
    A_MEMBER_ARRIVE,    // stop, park, place when i = 1 (f0..f2), face f3 (yaw rad); an A_PLAY circle follows
    A_DYNAMITE_ARM,     // take a free dynamite shell (fuse sparks) and put it in Ram's hand (he shows it)
    A_DYNAMITE_ATTACH,  // hand it over: off Ram's hand, onto the partner's hand
    A_BOOM,             // the explosion at the partner + kill pass over partner and ring members (alive mask in i)
    A_MEMBER_RELEASE,   // who 1..8: AI back (survivors)
    A_PARTNER_RELEASE,  // AI back, collision back
    A_DYNAMITE_DROP,    // abort: detach + return the shell to the pool
    A_INPUT_LOCK,       // i 1 lock / 0 unlock
    A_AUTO_DIVE,        // turn Ram to f0..f2 (unit direction) and inject a forward shoot dodge
    A_CAM_TAKE,         // camera to real time, saved fov
    A_CAM,              // who = Shot, i = Target, f0..f2 eye, f3 blend s (0 cut, < 0 = move the eye only), f4 fov
    A_CAM_RELEASE,      // f0 blend back to the gameplay camera
    A_RAM_SPEED,        // Ram's own animation speed: f0 (<= 0: the player's normal speed), ramped over f1 real s
    A_END,              // i 0 normal end, 1 abort (reason in Suave::reason)
};
enum Clip { CLIP_LEAD = 0, CLIP_FOLLOW, CLIP_PUSH, CLIP_STUMBLE, CLIP_CIRCLE, CLIP_INTRO, CLIP_GRABBED, CLIP_LOOKDOWN,
            CLIP_COUNT };
struct Act { int kind = A_NONE; int who = 0; int i = 0; float f[6] = {0, 0, 0, 0, 0, 0}; };
enum { MAX_ACTS = 96 };
struct Acts {
    Act a[MAX_ACTS]; int n = 0;
    Act& add(int kind, int who = 0, int i = 0) { static Act dummy; if (n >= MAX_ACTS) return dummy; a[n] = Act(); a[n].kind = kind; a[n].who = who; a[n].i = i; return a[n++]; }
    int count(int kind) const { int k = 0; for (int j = 0; j < n; ++j) k += a[j].kind == kind; return k; }
    const Act* find(int kind, int who = -999) const { for (int j = 0; j < n; ++j) if (a[j].kind == kind && (who == -999 || a[j].who == who)) return &a[j]; return nullptr; }
    void append(const Acts& o) { for (int j = 0; j < o.n && n < MAX_ACTS; ++j) a[n++] = o.a[j]; }
};

struct Suave {
    int   phase = P_OFF;
    float real_t = 0.0f;            // real seconds since the start
    float c = 0.0f;                 // the move's clock (game seconds since the music start)
    float phase_t = 0.0f;           // seconds (real in FREEZE, game afterwards) in the current phase
    int   n = 0;                    // ring members
    int   mstate[MAX_RING] = {};
    Vec3  slot[MAX_RING]; float slot_yaw[MAX_RING] = {};
    Vec3  centre;
    Pair  pair = PAIR_SOLDIER;
    int   segment = 0;              // music segment 1..N (0 = none)
    bool  attached = false, boomed = false, ram_taken = false, partner_taken = false, input_locked = false;
    bool  partner_frozen = false, dance_started = false, ring_started = false, cam_taken = false, slowmo_on = false;
    bool  ram_released = false, lookdown_done = false, back_done = false, landed = false, lookdown_played = false;
    bool  ram_slowed = false; float panic_t = -1.0f, back_t = -1.0f;   // real s in the panic cut / since the cut back
    // ending bookkeeping
    float dive_real = -1, dive_game = -1;       // since the dive was started (Ram released)
    float air_real = -1, air_game = -1;         // since the takeoff (-1 = not airborne yet)
    float boom_real = -1, land_real = -1;
    Vec3  dive_dir;                 // Ram's dive direction (away from the partner)
    Vec3  cam_dir;                  // where the low front cameras stand from Ram (= dive_dir for a forward dive)
    float ring_r = 3.0f;            // the ring radius used (smaller in a tight space)
    bool  armed = false, dive_final = false, ending_on = false;
    Vec3  front_eye;                // the low front camera position (fixed for 4b)
    Vec3  lookdown_eye;
    // camera
    int   shot = SH_NONE; float shot_t = 0.0f; int cut_index = 0; float orbit_a0 = 0.0f; int pov_member = -1;
    int   face_step = -1;
    const char* reason = "";
    uint32_t rng = 0x12345678u;
    int   last_segment = 0;
    uint32_t uses = 0, aborts = 0;

    bool running() const { return phase != P_OFF; }
    // Start (called from the use-power-up gate when the game is about to spend the charge). members are the chosen
    // ring in member order; their slots are computed here. Returns the actions to perform at once.
    Acts start(const Cfg& c, const Snap& s, Pair p, bool model_matched, const Vec3* members, int nmembers, uint32_t seed);
    // Every input tick (script context). dt_real = real seconds since the last tick.
    Acts tick(const Cfg& c, const Snap& s, float dt_real);
    // Abort from anywhere: hard = the level is going away (no messages to NPCs, only to the player / time / sound).
    Acts abort(const Cfg& c, const char* why, bool hard);
    int   pick_segment(int nseg);
    float phase_len(const Cfg& c) const;   // planned length of the current phase (s), for the shared memory
    // the camera director (also used by the unit tests): eye for a shot from the snapshot
    bool  shot_eye(const Cfg& c, int shot, const Snap& s, Vec3* eye, int* target, float* fov) const;
private:
    void  camera(const Cfg& c, const Snap& s, Acts& a, int want, float blend);
    void  face(const Cfg& c, Acts& a, int who, int f);
    void  end_common(const Cfg& c, const Snap& s, Acts& a);
};

} // namespace sv
