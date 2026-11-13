// rd_game.cpp - the [ragdoll] feature of TODGameplay.dll: our own "active ragdoll" for Total Overdose's
// characters, built on the [physics] Box3D world (TODPhysics.dll, ragdoll/code/rd_sim.cpp). Off unless [ragdoll]
// enabled = 1; when off nothing here touches the game.
//
// How it works (docs: ragdoll/README.md; sites and REF rows: dll/src/sites.h section "Ragdoll"; dll/HOOKS.md):
//   * RD_CHARUPD: Scene::Update's character loop calls Character::Update 0x910CE0 for every visible character
//     (call at 0x897EFD). That update samples the animation layers and the skeleton compose 0x90EA50 writes every
//     bone node's LOCAL rotation (NodeMatrix +00h, stored = conjugate) and Bip's local position, then marks the bones
//     dirty (+14h |= 2); the world matrices follow lazily and the renderer skins on the CPU from them. Our wrapper
//     runs the original, then (a) reads the animated pose (motor targets, hand-off velocities), (b) for a character
//     with a ragdoll overwrites the bone locals with the simulated pose (blended for an active one) and marks the
//     bones dirty again. That is the bone override point: nothing reads the bones between the compose and our write.
//   * RD_HIT: character_root's take_a_hit relay 0x5C4F40 hands every hit to the AI with SendCommand(AI, 0x534) at
//     0x5C535E (esi = the hit {damage, owner, inflictor, volume, impulse, position, type}, ebp = the root): a stub
//     records it; the next update of that character starts an active ragdoll (alive) or uses it as the death push.
//   * Death = the AI's Current_state (+1D4h) 28 (state 0x5D6B70 sets it, then MoveCtrl command_takeahit_death plays
//     the collapse clip). Our limp ragdoll replaces that clip: muscles follow it for death_fade s, then go limp; the
//     character node follows the pelvis (SetWorldPosition + PhysicCtrl reset_to_parent) until it settles; the settled
//     pose is kept (bones written every frame) until the pooled character is reused (state leaves 28) or hidden.
//   * The physics tick (phys_game.cpp) steps the shared Box3D world; before each fixed step the active ragdolls get
//     their joint spring targets (the animated pose), strengths and the pelvis anchor; after it the bodies are read.
//   * Bullets (PH_BULLET ray) push ragdoll bodies; explosions (PH_SHELL / PH_DESTROY) throw them, and a character
//     that dies from a blast within blast_memory s gets the same blast on its new ragdoll.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <initializer_list>
#include "../../dll/src/gp_glue.h"
#include "../../physics/code/phys_api.h"
#include "rd_logic.h"
#include "rd_api.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define RD8(a)  (*(volatile BYTE*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define WRF(a, v)  (*(volatile float*)(uintptr_t)(a) = (float)(v))

using rd::Xf; using rd::V3; using rd::Q4; using rd::NBODY; using rd::MAXB;

// ================================================================================================ game addresses
static const DWORD A_SCENE = 0xA3DCBC, A_GAME_DT = 0xA3DCC4, A_PLAYER_HOLDER = 0xA3B590, A_CUR_NODE = 0xA3B75C,
                   A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764, A_LIST_ENTITY = 0xA3B8A8;
static const DWORD P_WORLDMAT = 0x4842C0, P_SETWPOS = 0x88E600, P_GETPOS = 0x88CA80, P_SETROTY = 0x891360,
                   P_L2W = 0x88C9C0, P_GETPROP = 0x86A6A0, P_CAMMAT = 0x87BC60;
static DWORD R_CHARUPD = 0x910CE0, R_HITSEND = 0x86A340;   // original call targets of RD_CHARUPD / RD_HIT
static const DWORD H_ROOT = 0x5C3060, H_SENTRY = 0x5C90F0, H_SHELL = 0x843D10;
static const DWORD R_SUBTYPE = 0x14, R_ANIM = 0x50, R_AI = 0x58, R_PHYS = 0x68;   // character_root data
static const DWORD R_PLACEHOLDER = 0x24;                                             // character_root data: its placeholder
static const DWORD AI_HEALTH = 0x2C, AI_TAKEN = 0x30, AI_STATE = 0x1D4;         // character_sentry_ai data
static const DWORD C_GAMECAM = 0x260, C_SHELLS = 0x550;                         // script cache data
// messages (rows of suave/code/suave_msgs.h, checked against the class tables by check_suave.py)
static const int MSG_ROOT_TAKE_A_HIT = -2, MSG_AI_NO_CONTROL = -23, MSG_PHYS_RESET = -22, MSG_MOVE_STOP = -5, MSG_MOVE_IDLE = -81;
static const DWORD R_MOVE = 0x48;   // character_root data: MoveCtrl (character_animation_ctrl)
static const int MSG_SHELL_EXPLODE = -9, MSG_SHELL_MORPH = -11, MSG_SHELL_AIM = -12;
static const int PROP_TORSO_CS = 0x69D;
static const int STATE_DEAD = 28;
static const DWORD A_GAME_MS = 0xA3DCD4;   // REF PS_GAMEMS: Scene::Update's game-ms clock (+= game dt x 1000; still while paused)
static_assert(rd::RM_NONE == 0 && rd::RM_ACTIVE == 1 && rd::RM_LIMP == 2 && rd::RM_FROZEN == 3, "rd_logic.h track modes");

static const char* RD_REFS[] = {"RD_COMPOSE_BONES", "RD_COMPOSE_DIRTY", "RD_BONE_SET", "RD_WORLD_DIRTY", "RD_STATE28",
    "SM_RD_STATE", "SM_ROOT_AI", "SV_CLS_ROOT", "SV_CLS_SENTRY", "SV_CLASSOBJ", "SV_MODELRES", "PH_F_PIVCOLL", "PH_F_WORLDMAT",
    "PH_GAME_DT", "SL_PLAYER", "F_GETPOS", "F_SETPOS", "F_SETROTY", "F_L2W", "F_GETPROP"};
static const char* RD_DEBUG_REFS[] = {"RD_CAMERA", "PH_SHELLS", "SV_CLS_SHELL", "H_NO_AI", "H_ROOT_HIT", "SV_GOD_SEND"};

// ================================================================================================ config
static rd::Cfg g_c;
static bool g_installed = false, g_debug_refs = false;
static int  g_key_bring = gp::KEY_NONE, g_key_hit = gp::KEY_NONE, g_key_kill = gp::KEY_NONE, g_key_boom = gp::KEY_NONE,
            g_key_freeze = gp::KEY_NONE, g_key_turn = gp::KEY_NONE, g_key_vanilla = gp::KEY_NONE;
static float g_turn_deg = 90.0f;
static float g_bring_dist = 5.0f, g_bring_side = 0.0f, g_debug_hit_damage = 1.0f, g_debug_hit_impulse = 120.0f,
             g_boom_back = 1.4f, g_boom_side = 0.0f, g_draw_every = 0.1f;
static bool g_debug_god = false, g_debug_ai_off = false; static double g_god_at = 0.0; static DWORD g_god_root = 0;
static bool g_bring_ai_off = true, g_debug_shots = false, g_log_hits = true, g_rest_pose = true;
static int  g_shell_type = 1;
static char g_force_bone[24] = ""; static float g_force_angle = 0.0f;   // test copy: milestone (a), one bone forced

static char g_seq[1024] = ""; static int g_seq_pos = 0; static double g_seq_next = 0.0, g_seq_start = 0.0; static float g_seq_delay = 4.0f;
static volatile LONG g_fire_until_ms = 0; static bool g_fire_prev = false;
void ragdoll_load_config() {
    const gp::Ini& I = gp_ini();
    rd::Cfg c;
    c.enabled = I.geti("ragdoll", "enabled", 0) != 0;
    c.death_ragdoll = I.geti("ragdoll", "death_ragdoll", 1) != 0;
    c.active_hits = I.geti("ragdoll", "active_hits", 1) != 0;
    c.max_ragdolls = I.geti("ragdoll", "max_ragdolls", c.max_ragdolls);
    c.max_frozen = I.geti("ragdoll", "max_frozen", c.max_frozen);
    c.settle_seconds = I.getf("ragdoll", "settle_seconds", c.settle_seconds);
    c.impulse_scale = I.getf("ragdoll", "impulse_scale", c.impulse_scale);
    c.joint_stiffness = I.getf("ragdoll", "joint_stiffness", c.joint_stiffness);
    c.recover_seconds = I.getf("ragdoll", "recover_seconds", c.recover_seconds);
    c.debug_draw = I.geti("ragdoll", "debug_draw", 0) != 0;
    c.scale = I.getf("ragdoll", "scale", c.scale);
    c.total_mass = I.getf("ragdoll", "total_mass", c.total_mass);
    c.joint_damping = I.getf("ragdoll", "joint_damping", c.joint_damping);
    c.hit_strength = I.getf("ragdoll", "hit_strength", c.hit_strength);
    c.hit_neighbour = I.getf("ragdoll", "hit_neighbour", c.hit_neighbour);
    c.bullet_impulse = I.getf("ragdoll", "bullet_impulse", c.bullet_impulse);
    c.game_hit_scale = I.getf("ragdoll", "game_hit_scale", c.game_hit_scale);
    c.blast_scale = I.getf("ragdoll", "blast_scale", c.blast_scale);
    c.blast_lift = I.getf("ragdoll", "blast_lift", c.blast_lift);
    c.blast_memory = I.getf("ragdoll", "blast_memory", c.blast_memory);
    c.death_fade = I.getf("ragdoll", "death_fade", c.death_fade);
    c.limp_friction = I.getf("ragdoll", "limp_friction", c.limp_friction);
    c.active_gravity = I.getf("ragdoll", "active_gravity", c.active_gravity);
    c.balance_hertz = I.getf("ragdoll", "balance_hertz", c.balance_hertz);
    c.balance_force = I.getf("ragdoll", "balance_force", c.balance_force);
    c.balance_torque = I.getf("ragdoll", "balance_torque", c.balance_torque);
    c.balance_damping = I.getf("ragdoll", "balance_damping", c.balance_damping);
    c.foot_hertz = I.getf("ragdoll", "foot_hertz", c.foot_hertz);
    c.foot_force = I.getf("ragdoll", "foot_force", c.foot_force);
    c.foot_damping = I.getf("ragdoll", "foot_damping", c.foot_damping);
    c.blend_in = I.getf("ragdoll", "blend_in", c.blend_in);
    c.blend_out = I.getf("ragdoll", "blend_out", c.blend_out);
    c.stumble_distance = I.getf("ragdoll", "stumble_distance", c.stumble_distance);
    c.stumble_rate = I.getf("ragdoll", "stumble_rate", c.stumble_rate);
    c.substeps = I.geti("ragdoll", "substeps", c.substeps);
    c.max_speed = I.getf("ragdoll", "max_speed", c.max_speed);
    c.track_radius = I.getf("ragdoll", "track_radius", c.track_radius);
    c.root_follow = I.geti("ragdoll", "root_follow", 1) != 0;
    c.civilians = I.geti("ragdoll", "civilians", 1) != 0;
    c.hit_impulse = I.getf("ragdoll", "hit_impulse", c.hit_impulse);
    c.hit_max_dv = I.getf("ragdoll", "hit_max_dv", c.hit_max_dv);
    c.hit_core_share = I.getf("ragdoll", "hit_core_share", c.hit_core_share);
    c.hit_burst = I.getf("ragdoll", "hit_burst", c.hit_burst);
    c.active_friction = I.getf("ragdoll", "active_friction", c.active_friction);
    c.active_ang_damping = I.getf("ragdoll", "active_ang_damping", c.active_ang_damping);
    c.spine_limit_scale = I.getf("ragdoll", "spine_limit_scale", c.spine_limit_scale);
    c.death_max_dv = I.getf("ragdoll", "death_max_dv", c.death_max_dv);
    c.death_core_share = I.getf("ragdoll", "death_core_share", c.death_core_share);
    c.limp_ang_damping = I.getf("ragdoll", "limp_ang_damping", c.limp_ang_damping);
    c.blast_strength = I.getf("ragdoll", "blast_strength", c.blast_strength);
    c.corpse_seconds = I.getf("ragdoll", "corpse_seconds", c.corpse_seconds);
    c.corpse_max = I.geti("ragdoll", "corpse_max", c.corpse_max);
    c.log_seconds = I.getf("ragdoll", "log_seconds", c.log_seconds);
    c.stall_seconds = I.getf("ragdoll", "stall_seconds", c.stall_seconds);
    c.stall_fall_seconds = I.getf("ragdoll", "stall_fall_seconds", c.stall_fall_seconds);
    c.clamp();
    g_c = c;
    g_rest_pose = I.geti("ragdoll", "rest_pose_limits", 1) != 0;
    g_log_hits = I.geti("ragdoll", "log_hits", 1) != 0;
    // test copy only
    g_key_bring = gp_key_cfg("ragdoll", "debug_bring_key", "NONE");
    g_key_hit = gp_key_cfg("ragdoll", "debug_hit_key", "NONE");
    g_key_kill = gp_key_cfg("ragdoll", "debug_kill_key", "NONE");
    g_key_boom = gp_key_cfg("ragdoll", "debug_boom_key", "NONE");
    g_key_freeze = gp_key_cfg("ragdoll", "debug_freeze_key", "NONE");
    g_key_turn = gp_key_cfg("ragdoll", "debug_turn_key", "NONE");          // test copy: Ram turns by debug_turn_deg (the mouse-turn path)
    g_turn_deg = I.getf("ragdoll", "debug_turn_deg", 90.0f);
    g_key_vanilla = gp_key_cfg("ragdoll", "debug_vanilla_key", "NONE");    // test copy: death ragdoll / active hits on <-> off
    g_bring_dist = I.getf("ragdoll", "debug_bring_dist", 5.0f);
    g_bring_side = I.getf("ragdoll", "debug_bring_side", 0.0f);
    g_bring_ai_off = I.geti("ragdoll", "debug_bring_ai_off", 1) != 0;
    g_debug_hit_damage = I.getf("ragdoll", "debug_hit_damage", 1.0f);
    g_debug_hit_impulse = I.getf("ragdoll", "debug_hit_impulse", 120.0f);
    g_boom_back = I.getf("ragdoll", "debug_boom_back", 1.4f);
    g_boom_side = I.getf("ragdoll", "debug_boom_side", 0.0f);
    g_shell_type = I.geti("ragdoll", "debug_shell_type", 1);
    g_debug_shots = I.geti("ragdoll", "debug_shots", 0) != 0;
    g_debug_god = I.geti("ragdoll", "debug_god", 0) != 0;   // test copy: Ram in god mode (the goodie command El Mariachi uses)
    g_debug_ai_off = I.geti("ragdoll", "debug_ai_off", 0) != 0;   // test copy: every tracked character stops fighting
    g_draw_every = I.getf("ragdoll", "debug_draw_seconds", 0.1f);
    g_seq[0] = 0;   // ini values hold 127 characters: debug_sequence, then debug_sequence2 .. 9 appended
    for (int k = 1; k <= 9; ++k) {
        char key[24]; if (k == 1) strcpy(key, "debug_sequence"); else sprintf(key, "debug_sequence%d", k);
        if (const char* v = I.get("ragdoll", key)) { if (g_seq[0]) strncat(g_seq, ", ", sizeof g_seq - strlen(g_seq) - 1); strncat(g_seq, v, sizeof g_seq - strlen(g_seq) - 1); }
    }
    g_seq_delay = I.getf("ragdoll", "debug_sequence_delay", 4.0f);
    if (const char* v = I.get("ragdoll", "debug_force_bone")) { strncpy(g_force_bone, v, 23); g_force_bone[23] = 0; }
    g_force_angle = I.getf("ragdoll", "debug_force_angle", 0.0f);
}
bool ragdoll_enabled() { return g_c.enabled; }

// ================================================================================================ engine calls
typedef void(__fastcall* Mat_t)(DWORD node, void* edx, float* out16);
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, float* v);
typedef void(__fastcall* RotY_t)(DWORD node, void* edx, float y);
typedef void(__fastcall* Get_t)(DWORD node, void* edx, int id, void* out);
typedef void(__fastcall* Upd_t)(DWORD ch, void* edx);
typedef DWORD(__fastcall* Cmd_t)(DWORD node, void* edx, int id, void* args);

static DWORD data(DWORD n) { __try { return n ? RD32(n + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_handler(DWORD node) {   // node -> +4 type -> +28h class object -> +60h handler (REF SV_CLASSOBJ)
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool world_xf(DWORD node, Xf* x) {
    float m[16];
    __try { ((Mat_t)(uintptr_t)P_WORLDMAT)(node, nullptr, m); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    bool ok; *x = rd::xf_from_tod(m, &ok);
    return ok;
}
static bool get_pos(DWORD node, V3* p) {
    float v[3];
    if (!node) return false;
    __try { ((Vec_t)(uintptr_t)P_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    *p = ph::v3(v[0], v[1], v[2]); return true;
}
static bool set_wpos(DWORD node, V3 p) {
    float v[3] = {p.x, p.y, p.z};
    __try { ((Vec_t)(uintptr_t)P_SETWPOS)(node, nullptr, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool get_fwd(DWORD node, V3* f) {   // world direction of local +z
    float b[6] = {0, 0, 0, 0, 0, 1};
    __try { ((Vec_t)(uintptr_t)P_L2W)(node, nullptr, b); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float l = sqrtf(b[0] * b[0] + b[2] * b[2]); if (!(l > 1e-4f)) return false;
    *f = ph::v3(b[0] / l, 0, b[2] / l); return true;
}
static bool set_rot_y(DWORD node, float y) { __try { ((RotY_t)(uintptr_t)P_SETROTY)(node, nullptr, y); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static void set_world_yaw(DWORD node, float want) {   // SetRotationY is local: measure its world effect (suave_game.cpp)
    V3 f0, f1;
    if (!set_rot_y(node, 0.0f) || !get_fwd(node, &f0) || !set_rot_y(node, 1.0f) || !get_fwd(node, &f1)) return;
    float w0 = atan2f(f0.x, f0.z), w1 = atan2f(f1.x, f1.z), d = w1 - w0;
    while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
    float local = fabsf(fabsf(d) - 1.0f) < 0.2f ? (d > 0 ? 1.0f : -1.0f) * (want - w0) : want;
    set_rot_y(node, local);
}
static bool get_prop(DWORD node, int id, void* out) {
    if (!node) return false;
    __try { ((Get_t)(uintptr_t)P_GETPROP)(node, nullptr, id, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DWORD ram_root() { __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static float game_dt() { __try { float d = RDF(A_GAME_DT); return d == d ? d : 0.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; } }
static bool send(DWORD node, int id, void* args) { return node && gp_send(node, id, args); }

// ================================================================================================ state
enum Mode { M_NONE, M_ACTIVE, M_LIMP, M_FROZEN };
static const char* mode_name(int m) { static const char* n[] = {"none", "active", "limp", "frozen"}; return m >= 0 && m < 4 ? n[m] : "?"; }
struct Hit { DWORD root; float dmg; DWORD owner, inflictor, volume; float imp[3], pos[3]; int type; double t; LONG frame; bool used; };
struct Track {
    bool used; DWORD ch, root, ai, phys, asset, bones; int nb;
    DWORD node[MAXB], nm[MAXB];
    rd::Skeleton sk; rd::Map map; bool rest_ok; Xf rest_w[MAXB];
    Xf anim_local[MAXB], anim_world[MAXB], anim_body[NBODY], prev_body[NBODY], char_world;
    LONG anim_frame, prev_frame; float anim_dt; double seen;
    int mode; double t_mode; float weight; bool releasing;
    rd::Layout L; rd::Spawned sp; Xf cur[NBODY], prv[NBODY]; bool have_bodies;
    rd::Strength str; rd::Settle settle; float t_sim;
    Xf frozen[NBODY];
    int  state; bool dead_seen, fade_logged; double hit_t, t_dead; int hits, ray_hits; double ray_hit_t;
    V3 last_root; bool root_moved_by_us; V3 root_target; bool root_target_ok;
    double stumble_t, budget_t; float budget; bool ai_off, deathpose, handed; DWORD src;
    // this occupant of the pooled shell (its placeholder) and whether it was seen alive. A shell the pool hands
    // to a new placement keeps the old AI state 28 / DamageTaken for a moment, and a re-armed reinforcement came with a
    // lethal DamageTaken for good: both were taken for deaths, so live enemies became limp ragdolls pinned to their pelvis
    // (could not move) or frozen in their spawn pose (T-poses).
    DWORD ph; int alive_frames; bool seen_alive, stale_logged; double not28_since;
    // psycho-tune: this death was left to the game's own clip because the physics step had stopped (see stalled_now())
    bool death_game;
};
static const int MAXT = 24;
static Track g_t[MAXT];
static const RdApi* g_ra = nullptr; static bool g_ra_v2 = false;   // v2: damping (a TODPhysics.dll of the same build)
static const PhApi* g_pa = nullptr;
static Hit g_hits[16]; static int g_hit_next = 0;
struct Blast { V3 c; float radius, impulse, lift; double t; };
static Blast g_blasts[8]; static int g_blast_next = 0;
static DWORD g_ignore_ch[64]; static double g_ignore_until[64];
// statistics
static ph::Stats g_st_frame, g_st_step, g_st_write, g_st_spawn;
static int g_n_active = 0, g_n_limp = 0, g_n_frozen = 0, g_started_active = 0, g_started_limp = 0, g_frozen_total = 0,
           g_released = 0, g_spawn_fail = 0, g_cap_skips = 0, g_hits_seen = 0, g_ray_pushes = 0, g_blasts_seen = 0,
           g_root_moves = 0, g_stumbles = 0, g_writes = 0, g_logged_hits = 0, g_no_ground = 0;
static double g_last_log = 0.0, g_last_draw = 0.0;

static void mark(const char* what, int count, int ms) {   // test copy: the harness grabs screenshots on these lines
    if (!g_debug_shots) return;
    gp_log("debug keys: mark burst %s %d %d\n", what, count, ms);
}
static int sims() { int n = 0; for (auto& t : g_t) if (t.used && (t.mode == M_ACTIVE || t.mode == M_LIMP)) ++n; return n; }
static int frozen_count() { int n = 0; for (auto& t : g_t) if (t.used && t.mode == M_FROZEN) ++n; return n; }

// ---- the physics clock (rd_logic.h StepWatch). The world steps only in the player's on-foot input tick: while
// Ram drives the game does not run it, the characters keep updating, and a ragdoll there never moved (frozen
// enemies; one MM013 log had 9 stops of 17-52 s, live enemies held in a stale pose, corpses standing with 0.0 s of
// simulation). While the step is stopped no ragdoll starts and the running ones go back to the game (stall_action).
static bool g_clock_ok = false;
static rd::StepWatch g_steps;
static bool g_stall_on = false, g_stall_noted = false; static double g_stall_t0 = 0.0;
static int g_stalls = 0, g_stall_handed = 0, g_stall_left = 0, g_stall_handed_total = 0, g_stall_left_total = 0;
static bool read_game_ms(uint32_t* ms) { __try { *ms = (uint32_t)RD32(A_GAME_MS); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool stalled_now() {
    if (!g_clock_ok) return false;
    uint32_t ms; if (!read_game_ms(&ms)) return false;
    bool s = g_steps.stalled(ms, (int)(g_c.stall_seconds * 1000.0f));
    if (s && !g_stall_on) { g_stall_on = true; g_stall_noted = false; g_stall_t0 = gp_now(); g_stall_handed = g_stall_left = 0; }
    return s;
}
static void stall_note(bool handed) {   // the first thing a stop changes is logged (a stop that touches nothing stays quiet)
    if (handed) { ++g_stall_handed; ++g_stall_handed_total; } else { ++g_stall_left; ++g_stall_left_total; }
    if (g_stall_noted) return;
    g_stall_noted = true; ++g_stalls;
    gp_log("ragdoll: the physics step stopped %.1f s ago (the game runs no on-foot input tick: Ram drives, sits in a seat or a script "
           "moves him): no new ragdolls, the running ones go back to the game until it steps again\n", gp_now() - g_stall_t0 + g_c.stall_seconds);
}

// ================================================================================================ skeleton read
static bool read_skeleton(Track& t) {   // names, parents, bone node / NodeMatrix pointers, rest pose; once per character + model
    __try {
        DWORD arr = RD32(t.ch + 0x1D8); int n = (int)RD32(t.ch + 0x1DC);
        if (!arr || n < 20 || n > MAXB) return false;
        t.bones = arr; t.nb = n; t.asset = RD32(t.ch + 0x1D0);
        memset(&t.sk, 0, sizeof t.sk); t.sk.n = n;
        for (int i = 0; i < n; ++i) {
            DWORD bn = RD32(arr + (DWORD)i * 4); if (!bn) return false;
            DWORD nm = RD32(bn + 0x3C); if (!nm || RD32(nm + 0x60) != bn) return false;   // NodeMatrix owned by the bone (REF RD_BONE_SET)
            t.node[i] = bn; t.nm[i] = nm;
            const char* s = (const char*)(uintptr_t)RD32(bn + 0x4C);
            if (s && !IsBadStringPtrA(s, 24)) strncpy(t.sk.name[i], s, 23); else t.sk.name[i][0] = 0;
        }
        for (int i = 0; i < n; ++i) {
            DWORD par = RD32(t.node[i] + 0x40); int pi = -2;
            if (par == t.ch) pi = -1; else for (int j = 0; j < n; ++j) if (t.node[j] == par) { pi = j; break; }
            if (pi == -2) return false;
            t.sk.parent[i] = pi;
        }
        // bind pose: the model asset's pivot records (asset +30h, stride 7Ch: +00h position, +0Ch stored rotation),
        // pivot i = bone i (REF PH_F_PIVCOLL, design_renderer_bridge.md)
        t.rest_ok = false;
        if (g_rest_pose && t.asset && (int)RD32(t.asset + 0x34) == n) {
            DWORD recs = RD32(t.asset + 0x30);
            if (recs) {
                for (int i = 0; i < n; ++i) {
                    DWORD r = recs + (DWORD)i * 0x7C;
                    t.sk.rest[i].p = ph::v3(RDF(r), RDF(r + 4), RDF(r + 8));
                    Q4 s = {RDF(r + 0x0C), RDF(r + 0x10), RDF(r + 0x14), RDF(r + 0x18)};
                    t.sk.rest[i].q = ph::true_from_stored(s);
                }
                t.sk.has_rest = true;
                t.rest_ok = true;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool read_locals(Track& t) {   // the pose the compose just wrote
    __try {
        if (RD32(t.ch + 0x1D8) != t.bones || (int)RD32(t.ch + 0x1DC) != t.nb || RD32(t.ch + 0x1D0) != t.asset) return false;
        for (int i = 0; i < t.nb; ++i) {
            DWORD nm = t.nm[i];
            Q4 s = {RDF(nm), RDF(nm + 4), RDF(nm + 8), RDF(nm + 12)};
            t.anim_local[i].q = ph::true_from_stored(s);
            t.anim_local[i].p = ph::v3(RDF(nm + 0x10), RDF(nm + 0x14), RDF(nm + 0x18));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool write_locals(Track& t, const Xf* out) {   // stored quaternion (+ Bip position), bones dirty again
    __try {
        for (int i = 0; i < t.nb; ++i) {
            if (t.map.driver[i] < 0) continue;
            DWORD nm = t.nm[i];
            Q4 s = ph::stored_from_true(out[i].q);
            WRF(nm, s.x); WRF(nm + 4, s.y); WRF(nm + 8, s.z); WRF(nm + 12, s.w);
            if (i == t.map.bip) { WRF(nm + 0x10, out[i].p.x); WRF(nm + 0x14, out[i].p.y); WRF(nm + 0x18, out[i].p.z); }
        }
        for (int i = 0; i < t.nb; ++i) *(volatile BYTE*)(uintptr_t)(t.node[i] + 0x14) |= 2;   // as the compose (REF RD_COMPOSE_DIRTY)
        ++g_writes;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ================================================================================================ tracks
static void forget_sim(Track& t) {
    if (t.sp.r >= 0 && g_ra) g_ra->destroy(t.sp.r);
    t.sp.r = -1; t.have_bodies = false;
}
static void release(Track& t, const char* why) {
    if (t.mode != M_NONE) gp_log("ragdoll: character %08X %s -> released (%s)\n", t.ch, mode_name(t.mode), why);
    forget_sim(t);
    if (t.mode != M_NONE) ++g_released;
    t.mode = M_NONE; t.weight = 0; t.releasing = false;
}
static Track* find_track(DWORD ch) { for (auto& t : g_t) if (t.used && t.ch == ch) return &t; return nullptr; }
static bool ignored(DWORD ch, double now) {
    for (int i = 0; i < 64; ++i) if (g_ignore_ch[i] == ch) return now < g_ignore_until[i];
    return false;
}
static void ignore(DWORD ch, double now, float s) {
    int slot = 0; double oldest = 1e30;
    for (int i = 0; i < 64; ++i) { if (g_ignore_ch[i] == ch) { slot = i; break; } if (g_ignore_until[i] < oldest) { oldest = g_ignore_until[i]; slot = i; } }
    g_ignore_ch[slot] = ch; g_ignore_until[slot] = now + s;
}
// a character worth tracking: Character node under a character_root with a sentry AI, near Ram, not Ram
static bool candidate(DWORD ch, DWORD* root_out, DWORD* ai_out, DWORD* phys_out) {
    DWORD root = 0, rdat = 0, ai = 0, ph = 0;
    __try {
        root = RD32(ch + 0x40); if (!root) return false;
        rdat = RD32(root + 0x18); if (!rdat || RD32(rdat + R_ANIM) != ch) return false;
        ai = RD32(rdat + R_AI); ph = RD32(rdat + R_PHYS);
        if (!g_c.civilians && RD32(rdat + R_SUBTYPE) == 9) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (root == ram_root() || cls_handler(root) != H_ROOT || !ai || cls_handler(ai) != H_SENTRY) return false;
    *root_out = root; *ai_out = ai; *phys_out = ph;
    return true;
}
static int ai_state(const Track& t) { __try { DWORD d = RD32(t.ai + 0x18); return d ? (int)RD32(d + AI_STATE) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static Track* new_track(DWORD ch, DWORD root, DWORD ai, DWORD phys, double now) {
    Track* slot = nullptr;
    for (auto& t : g_t) if (!t.used) { slot = &t; break; }
    if (!slot) {   // the oldest unused-by-ragdoll track goes
        double oldest = 1e30;
        for (auto& t : g_t) if (t.mode == M_NONE && t.seen < oldest) { oldest = t.seen; slot = &t; }
        if (!slot) return nullptr;
    }
    memset(slot, 0, sizeof *slot);
    slot->used = true; slot->ch = ch; slot->root = root; slot->ai = ai; slot->phys = phys; slot->sp.r = -1;
    if (!read_skeleton(*slot) || !rd::build_map(slot->sk, &slot->map)) {
        static int warned = 0;
        if (warned++ < 8) gp_log("ragdoll: character %08X skeleton not usable (%s)\n", ch, slot->map.err[0] ? slot->map.err : "bones");
        slot->used = false;
        ignore(ch, now, 30.0f);
        return nullptr;
    }
    slot->anim_frame = -10; slot->prev_frame = -10; slot->seen = now;
    static int logged = 0;
    if (logged++ < 12) gp_log("ragdoll: tracking character %08X (root %08X): %d bones, rest pose %s, pelvis %d head %d\n", ch, root, slot->nb,
                              slot->rest_ok ? "yes" : "no", slot->map.body_bone[rd::PELVIS], slot->map.body_bone[rd::HEAD]);
    get_pos(root, &slot->last_root);
    return slot;
}

// ================================================================================================ ragdoll start / modes
static void hits_for(DWORD root, double now, float within, Hit* out, int* n, int cap) {
    *n = 0;
    for (int i = 0; i < 16; ++i) { Hit& h = g_hits[i]; if (h.root == root && !h.used && now - h.t <= within && *n < cap) out[(*n)++] = h; }
}
static void consume_hits(DWORD root) { for (auto& h : g_hits) if (h.root == root) h.used = true; }
static bool hit_within(DWORD root, double now, float s) { for (auto& h : g_hits) if (h.root == root && now - h.t <= s) return true; return false; }
// the game's hit impulse: bullets carry a unit direction (|imp| ~ 1), melee ~200, explosives ~1500 (logged in game)
static V3 game_push(const Hit& h) {
    V3 g = ph::v3(h.imp[0], h.imp[1], h.imp[2]);
    float l = ph::len(g);
    if (l < 5.0f) return l > 1e-3f ? ph::mul(g, g_c.bullet_impulse * g_c.impulse_scale / l) : ph::v3(0, 0, 0);
    return ph::mul(g, g_c.game_hit_scale * g_c.impulse_scale);
}
static void push_body(Track& t, int b, V3 imp, V3 at, float max_dv) {
    if (t.sp.r < 0 || b < 0) return;
    float m = t.L.mass[b] * g_c.total_mass / 80.0f;
    float l = ph::len(imp), cap = m * max_dv * 4.0f;   // the whole body shares it through the joints
    if (l > cap && l > 0) imp = ph::mul(imp, cap / l);
    float i3[3] = {imp.x, imp.y, imp.z}, p3[3] = {at.x, at.y, at.z};
    g_ra->impulse(t.sp.r, b, i3, p3);
}
static V3 body_centre(const Track& t, int b) { return rd::xpoint(t.cur[b], ph::mul(ph::add(t.L.c1[b][0], t.L.c2[b][0]), 0.5f)); }
// a shared push (rd::share_push): the hit part at the hit point, the torso's share at the torso's centre
static void apply_push(Track& t, const rd::Push& p, V3 at) {
    if (t.sp.r < 0) return;
    for (int k = 0; k < p.n; ++k) {
        V3 q = k == 0 ? at : body_centre(t, p.body[k]);
        float i3[3] = {p.imp[k].x, p.imp[k].y, p.imp[k].z}, p3[3] = {q.x, q.y, q.z};
        g_ra->impulse(t.sp.r, p.body[k], i3, p3);
    }
}
// a push on a powered (live) body: a burst of hits within 0.5 s shares a budget (a stronger knock, never a back flip),
// then shared out so a limb or head shot is a flinch of the whole body (hit_max_dv, hit_core_share)
static float live_push(Track& t, int b, V3 imp, V3 at, float budget_cap) {
    double now = gp_now();
    if (now - t.budget_t > 0.5) { t.budget_t = now; t.budget = 0.0f; }
    float l = ph::len(imp);
    if (t.budget + l > budget_cap) imp = l > 0 ? ph::mul(imp, (budget_cap - t.budget > 0 ? budget_cap - t.budget : 0.0f) / l) : imp;
    t.budget += ph::len(imp);
    rd::Push p = rd::share_push(t.L, g_c.total_mass / 80.0f, b, imp, g_c.hit_max_dv, g_c.hit_core_share);
    apply_push(t, p, at);
    return ph::len(imp);
}
// a bullet push on a limp body (the killing shot, a shot corpse): heavy, the limbs do not whip (death_max_dv)
static float limp_bullet(Track& t, int b, V3 imp, V3 at) {   // returns the N s applied (after the caps)
    rd::Push p = rd::share_push(t.L, g_c.total_mass / 80.0f, b, imp, g_c.death_max_dv, g_c.death_core_share);
    apply_push(t, p, at);
    float sum = 0.0f; for (int k = 0; k < p.n; ++k) sum += ph::len(p.imp[k]);
    return sum;
}
static void blast_on(Track& t, const Blast& bl, const char* who) {   // a blast as per-body impulses (area model + lift)
    if (t.sp.r < 0) return;
    Xf B[NBODY]; memcpy(B, t.cur, sizeof B);
    int touched = 0;
    for (int b = 0; b < NBODY; ++b) {
        V3 c = rd::xpoint(B[b], ph::mul(ph::add(t.L.c1[b][0], t.L.c2[b][0]), 0.5f));
        V3 d = ph::sub(c, bl.c); float dist = ph::len(d), reach = bl.radius * 1.5f;
        if (dist > reach) continue;
        float k = 1.0f - dist / reach;
        float r = t.L.rad[b][0], l = ph::len(ph::sub(t.L.c2[b][0], t.L.c1[b][0]));
        float area = 2.0f * r * l + 3.14159f * r * r;
        float m = t.L.mass[b] * g_c.total_mass / 80.0f;
        V3 dir = dist > 0.01f ? ph::mul(d, 1.0f / dist) : ph::v3(0, 1, 0);
        float s = g_c.impulse_scale * g_c.blast_scale;
        V3 imp = ph::add(ph::mul(dir, bl.impulse * area * k * s), ph::v3(0, m * bl.lift * k * g_c.impulse_scale, 0));
        float i3[3] = {imp.x, imp.y, imp.z}, p3[3] = {c.x, c.y, c.z};
        g_ra->impulse(t.sp.r, b, i3, p3);
        ++touched;
    }
    // a thrown body tumbles as it always did (explosions are meant to stay exactly as they are): the old
    // angular damping, whatever limp_ang_damping / active_ang_damping say
    if (touched && g_ra_v2) g_ra->damping(t.sp.r, 0.05f, 0.6f);
    if (touched) gp_log("ragdoll: %s blast at (%.1f, %.1f, %.1f) throws character %08X (%d bodies)\n", who, bl.c.x, bl.c.y, bl.c.z, t.ch, touched);
}
static bool start_sim(Track& t, int mode, const Xf* bodies, bool with_vel) {
    if (!g_ra || !phys_world_ready() || stalled_now()) return false;   // a world that does not step: no new ragdoll
    {   // no level geometry under the body = outside the scanned circle (a car outruns it; a far kill): the body
        // would fall through the world (test copy r3_drive: a corpse 73 m from the scan centre ended 55 m below the road)
        V3 pel = bodies[rd::PELVIS].p; float from[3] = {pel.x, pel.y + 1.0f, pel.z}, to[3] = {pel.x, pel.y - 4.0f, pel.z}, h[3];
        if (g_pa && !g_pa->raycast_static(from, to, h)) {
            static int nl = 0;
            if (nl++ < 20) gp_log("ragdoll: character %08X has no level geometry under it (outside the physics circle): left to the game\n", t.ch);
            ++g_no_ground; return false;
        }
    }
    double t0 = gp_now();
    Xf restW[MAXB]; const Xf* rw = nullptr;
    if (t.rest_ok) { rd::fk(t.sk, t.sk.rest, rd::xid(), restW); rw = restW; }
    t.sk.n = t.nb;
    for (int i = 0; i < t.nb; ++i) t.sk.local[i] = t.anim_local[i];   // creation locals (positions stay constant)
    if (!rd::build_layout(t.sk, t.map, t.anim_world, rw, g_c.scale, &t.L)) return false;
    V3 lin[NBODY], ang[NBODY];
    for (int b = 0; b < NBODY; ++b) { lin[b] = ph::v3(0, 0, 0); ang[b] = ph::v3(0, 0, 0); }
    // hand-off velocities from the last two animated frames. A powered ragdoll gets none (its springs follow the
    // animation anyway; in game the first frames after a teleport / an animation switch gave 4-9 m/s spikes that threw
    // a standing enemy flat, test copy r7 / r11); a limp one keeps the body's real motion, capped at 6 m/s.
    if (with_vel && mode == M_LIMP && t.prev_frame == t.anim_frame - 1 && t.anim_dt > 1e-4f) {
        float vmax = g_c.max_speed < 6.0f ? g_c.max_speed : 6.0f;
        for (int b = 0; b < NBODY; ++b) rd::velocity(t.prev_body[b], t.anim_body[b], t.anim_dt, vmax, &lin[b], &ang[b]);
        float pel = ph::len(lin[rd::PELVIS]);   // a body part much faster than the pelvis is a pose jump, not motion
        for (int b = 0; b < NBODY; ++b) if (ph::len(lin[b]) > pel + 3.0f) { lin[b] = lin[rd::PELVIS]; ang[b] = ang[rd::PELVIS]; }
    }
    float fr = mode == M_LIMP ? g_c.limp_friction : g_c.active_friction;
    float tight = rd::tighten_spine(&t.L, bodies, g_c.spine_limit_scale, 0.06f);
    if (!rd::spawn(g_ra, t.L, bodies, lin, ang, g_c, fr, &t.sp, mode == M_ACTIVE)) { ++g_spawn_fail; return false; }
    memcpy(t.cur, bodies, sizeof t.cur); memcpy(t.prv, bodies, sizeof t.prv); t.have_bodies = true;
    t.mode = mode; t.t_mode = gp_now(); t.t_sim = 0; t.settle.quiet = 0; t.str.reset(1.0f); t.releasing = false;
    g_ra->gravity(t.sp.r, mode == M_ACTIVE ? g_c.active_gravity : 1.0f);
    g_st_spawn.add((gp_now() - t0) * 1000.0);
    float sp = ph::len(lin[rd::PELVIS]);
    { float mx = 0; int mb = 0; for (int b = 0; b < NBODY; ++b) if (ph::len(lin[b]) > mx) { mx = ph::len(lin[b]); mb = b; }
      static int nl = 0; if (nl++ < 20) gp_log("ragdoll:   hand-off: fastest body %s %.2f m/s, frames %d -> %d, dt %.4f s\n", rd::body_name(mb), mx, t.prev_frame, t.anim_frame, t.anim_dt); }
    gp_log("ragdoll: character %08X %s ragdoll from the %s pose (pelvis %.2f m/s, rest pose %s, spine limits x%.2f, %d sims, %.2f ms)\n", t.ch,
           mode_name(mode), mode == M_ACTIVE ? "animated" : "current", sp, t.rest_ok ? "yes" : "no", tight, sims(), (gp_now() - t0) * 1000.0);
    return true;
}
static Track* oldest_limp() {
    Track* o = nullptr;
    for (auto& t : g_t) if (t.used && t.mode == M_LIMP && (!o || t.t_mode < o->t_mode)) o = &t;
    return o;
}
static void freeze(Track& t, const char* why) {
    memcpy(t.frozen, t.cur, sizeof t.frozen);
    forget_sim(t);
    t.mode = M_FROZEN; t.t_mode = gp_now();
    ++g_frozen_total;
    gp_log("ragdoll: character %08X frozen (%s) after %.1f s, pelvis at (%.2f, %.2f, %.2f)\n", t.ch, why, t.t_sim,
           t.frozen[rd::PELVIS].p.x, t.frozen[rd::PELVIS].p.y, t.frozen[rd::PELVIS].p.z);
    // over the cap of kept corpses: the oldest goes back to the game's own pose
    if (frozen_count() > g_c.max_frozen) {
        Track* o = nullptr;
        for (auto& x : g_t) if (x.used && x.mode == M_FROZEN && (!o || x.t_mode < o->t_mode)) o = &x;
        if (o) release(*o, "max_frozen");
    }
}
static void apply_death_push(Track& t, double now) {
    Hit h[4]; int n = 0; hits_for(t.root, now, 0.6f, h, &n, 4);
    for (int i = 0; i < n; ++i) {
        V3 imp = game_push(h[i]);
        V3 at = ph::v3(h[i].pos[0], h[i].pos[1], h[i].pos[2]);
        float d; int b = rd::nearest_body(t.L, t.cur, at, 1.5f, &d);
        if (b < 0) { b = rd::CHEST; at = t.cur[rd::CHEST].p; }
        float gl = ph::len(ph::v3(h[i].imp[0], h[i].imp[1], h[i].imp[2]));
        float applied = ph::len(imp);
        if (gl < 5.0f) applied = limp_bullet(t, b, imp, at);   // a bullet (unit direction): a heavy fall
        else push_body(t, b, imp, at, 25.0f);                  // melee / explosive: the game's own impulse, as before
        gp_log("ragdoll: character %08X death push %.0f N s on %s (hit type %d, damage %.0f, %.0f N s applied)\n", t.ch, ph::len(imp), rd::body_name(b), h[i].type, h[i].dmg, applied);
    }
    consume_hits(t.root);
    for (int i = 0; i < 8; ++i) {
        const Blast& bl = g_blasts[i];
        if (bl.t > 0 && now - bl.t <= g_c.blast_memory) blast_on(t, bl, "recent");
    }
}
static void start_death(Track& t, double now) {
    if (!g_c.death_ragdoll) return;
    if (t.mode == M_ACTIVE && t.sp.r >= 0 && g_ra->valid(t.sp.r)) {   // the powered ragdoll goes limp
        t.mode = M_LIMP; t.t_mode = gp_now(); t.t_sim = 0; t.settle.quiet = 0; t.weight = 1.0f; t.releasing = false;
        g_ra->gravity(t.sp.r, 1.0f);
        g_ra->anchor(t.sp.r, rd::PELVIS, nullptr, nullptr, 0, 0, 0, 0, 0, 0);
        g_ra->anchor(t.sp.r, rd::L_CALF, nullptr, nullptr, 0, 0, 0, 0, 0, 0); g_ra->anchor(t.sp.r, rd::R_CALF, nullptr, nullptr, 0, 0, 0, 0, 0, 0);
        for (int b = 1; b < NBODY; ++b) g_ra->friction(t.sp.r, t.sp.joint[b], t.L.friction[b] * g_c.limp_friction * g_c.total_mass / 80.0f);
        if (g_ra_v2) g_ra->damping(t.sp.r, 0.05f, g_c.limp_ang_damping);
        ++g_started_limp;
        gp_log("ragdoll: character %08X died while active: limp\n", t.ch);
        apply_death_push(t, now);
        return;
    }
    if (sims() >= g_c.max_ragdolls) {
        Track* o = oldest_limp();
        if (o) freeze(*o, "cap, a newer death");
        else { ++g_cap_skips; return; }
    }
    Xf bodies[NBODY]; memcpy(bodies, t.anim_body, sizeof bodies);
    if (!start_sim(t, M_LIMP, bodies, true)) return;
    t.weight = 1.0f;
    ++g_started_limp;
    apply_death_push(t, now);
    mark("rd_death", 30, 66);
}
static void start_active(Track& t, double now, const Hit& h) {
    if (!g_c.active_hits) return;
    if (sims() >= g_c.max_ragdolls) { ++g_cap_skips; return; }
    Xf bodies[NBODY]; memcpy(bodies, t.anim_body, sizeof bodies);
    if (!start_sim(t, M_ACTIVE, bodies, true)) return;
    t.weight = g_c.blend_in > 0 ? 0.0f : 1.0f;
    ++g_started_active;
    (void)now; (void)h;
    mark("rd_hit", 24, 50);
}
static void hit_active(Track& t, const Hit& h) {   // a (non-lethal) hit on a powered ragdoll: knock + weak muscles there
    V3 at = ph::v3(h.pos[0], h.pos[1], h.pos[2]);
    float d; int b = rd::nearest_body(t.L, t.cur, at, 2.0f, &d);
    if (b < 0) { b = rd::CHEST; at = t.cur[rd::CHEST].p; }
    V3 imp = game_push(h);
    if (ph::len(imp) < 1e-3f) {   // no impulse in the hit: away from the shooter
        V3 o; V3 dir = ph::v3(0, 0, 0);
        if (h.owner && get_pos(h.owner, &o)) dir = ph::norm(ph::sub(at, o));
        imp = ph::mul(dir, g_c.hit_impulse * g_c.impulse_scale);
    }
    // the bullet ray did not push this one already; a hit whose point is not on the body (cars, falls, scripted
    // damage: logged up to 2 m away) only weakens it
    float pushed = 0.0f;
    if (gp_now() - t.ray_hit_t > 0.06 && d < 0.6f) {
        bool bullet = ph::len(ph::v3(h.imp[0], h.imp[1], h.imp[2])) < 5.0f;
        // bullets share hit_burst x hit_impulse per 0.5 s; melee keeps its old budget (1.6 x bullet_impulse)
        float cap = bullet ? g_c.hit_burst * g_c.hit_impulse * g_c.impulse_scale : 1.6f * g_c.bullet_impulse * g_c.impulse_scale;
        pushed = live_push(t, b, imp, at, cap);
    }
    t.str.hit(b, g_c.hit_strength, g_c.hit_neighbour);
    t.t_mode = gp_now(); t.releasing = false;
    ++t.hits;
    gp_log("ragdoll: character %08X hit on %s (%.1f m from the point), %.0f N s, strength %.2f\n", t.ch, rd::body_name(b), d, pushed, t.str.s[b]);
}

// ================================================================================================ the per-character hook
// ---- the game's corpse: after corpse_seconds the dead character is swapped for a character_deathpose placed at its
// Character node (Take_A_Hit_Move_ctrl corpse state 0x601410), then faded out. The frozen ragdoll pose is handed over to
// that new character so the corpse keeps lying the way it fell.
struct Handoff { Xf bodies[NBODY]; Xf char_world; double t; DWORD from; bool used; };
static Handoff g_hand[8];
static void offer_handoff(const Track& t, const Xf* bodies) {
    Handoff* h = nullptr;
    for (auto& x : g_hand) if (x.used && x.from == t.ch) { h = &x; break; }   // one entry per dead character, kept fresh
    if (!h) { h = &g_hand[0]; for (auto& x : g_hand) { if (!x.used) { h = &x; break; } if (x.t < h->t) h = &x; } }
    memcpy(h->bodies, bodies, sizeof h->bodies); h->char_world = t.char_world; h->t = gp_now(); h->from = t.ch; h->used = true;
}
static int g_handoffs = 0;
static bool try_handoff(DWORD ch, double now) {
    Xf w; bool any = false;
    for (auto& h : g_hand) if (h.used && now - h.t > 3.0) h.used = false; else if (h.used) any = true;
    if (!any || !world_xf(ch, &w)) return false;
    DWORD par = 0; __try { par = RD32(ch + 0x40); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!par || par == ram_root() || cls_handler(par) == H_ROOT) return false;   // only a new non-character_root body (the death pose)
    for (auto& h : g_hand) {
        if (!h.used || h.from == ch || ph::len(ph::sub(w.p, h.char_world.p)) > 0.6f) continue;
        Track* slot = nullptr;
        for (auto& t : g_t) if (!t.used) { slot = &t; break; }
        if (!slot) for (auto& t : g_t) if (t.mode == M_NONE) { slot = &t; break; }
        if (!slot) return false;
        memset(slot, 0, sizeof *slot);
        slot->used = true; slot->ch = ch; slot->sp.r = -1; slot->deathpose = true; slot->seen = now;
        if (!read_skeleton(*slot) || !rd::build_map(slot->sk, &slot->map) || !read_locals(*slot)) { slot->used = false; h.used = false; return false; }
        slot->char_world = w;
        rd::fk(slot->sk, slot->anim_local, w, slot->anim_world);
        for (int i = 0; i < slot->nb; ++i) slot->sk.local[i] = slot->anim_local[i];
        if (!rd::build_layout(slot->sk, slot->map, slot->anim_world, nullptr, g_c.scale, &slot->L)) { slot->used = false; h.used = false; return false; }
        memcpy(slot->frozen, h.bodies, sizeof slot->frozen);
        slot->mode = M_FROZEN; slot->t_mode = now; slot->dead_seen = true; slot->t_dead = now; slot->state = STATE_DEAD;
        slot->src = h.from;
        if (Track* o = find_track(h.from)) o->handed = true;   // the fading original keeps its pose, never simulates again
        h.used = false; ++g_handoffs;
        gp_log("ragdoll: the death-pose corpse %08X takes over the ragdoll pose of %08X\n", ch, h.from);
        return true;
    }
    return false;
}
static void interp_bodies(const Track& t, float a, Xf* out);
static void deathpose_update(Track& t) {   // the game's corpse shows the ragdoll: the original's bodies while it lives, then its own
    if (!read_locals(t) || !world_xf(t.ch, &t.char_world)) { t.used = false; return; }
    if (t.mode == M_FROZEN && t.src)
        if (Track* o = find_track(t.src)) {
            if (o->mode == M_LIMP && o->have_bodies) interp_bodies(*o, phys_alpha(), t.frozen);
            else if (o->mode == M_FROZEN) memcpy(t.frozen, o->frozen, sizeof t.frozen);
        }
    Xf B[NBODY];
    if (t.mode == M_LIMP && t.have_bodies) interp_bodies(t, phys_alpha(), B);
    else if (t.mode == M_FROZEN) memcpy(B, t.frozen, sizeof B);
    else return;
    Xf out[MAXB]; memcpy(out, t.anim_local, sizeof(Xf) * t.nb);
    rd::pose_from_bodies(t.sk, t.map, t.L, B, t.char_world, out);
    write_locals(t, out);
}
static void interp_bodies(const Track& t, float a, Xf* out) {
    for (int b = 0; b < NBODY; ++b) { out[b].p = ph::add(t.prv[b].p, ph::mul(ph::sub(t.cur[b].p, t.prv[b].p), a)); out[b].q = ph::qslerp(t.prv[b].q, t.cur[b].q, a); }
}
static void after_update(DWORD ch) {
    double now = gp_now();
    Track* t = find_track(ch);
    if (!t) {
        if (try_handoff(ch, now)) { t = find_track(ch); if (t) deathpose_update(*t); return; }
        if (ignored(ch, now)) return;
        DWORD root, ai, ph_;
        if (!candidate(ch, &root, &ai, &ph_)) { ignore(ch, now, 2.0f); return; }
        V3 rp, cp; DWORD ram = ram_root();
        if (!ram || !get_pos(ram, &rp) || !get_pos(root, &cp) || ph::len(ph::sub(rp, cp)) > g_c.track_radius) { ignore(ch, now, 1.0f); return; }
        t = new_track(ch, root, ai, ph_, now);
        if (!t) return;
    }
    double t0 = now;
    t->seen = now;
    if (t->deathpose) { deathpose_update(*t); g_st_frame.add((gp_now() - t0) * 1000.0); return; }
    // the character was reused / re-modelled: start over
    DWORD rdat = data(t->root), ai_now = 0;
    __try { ai_now = rdat ? RD32(rdat + R_AI) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD ph_now = 0; __try { ph_now = rdat ? RD32(rdat + R_PLACEHOLDER) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const char* why = RD32(ch + 0x40) != t->root ? "parent changed" : ai_now != t->ai ? "AI pointer changed"
                    : (t->ph && ph_now && ph_now != t->ph) ? "the pool gave the shell to another placement" : !read_locals(*t) ? "bones / model changed" : nullptr;
    if (why) {
        if (t->dead_seen) gp_log("ragdoll: character %08X (dead %.1f s, mode %s): %s\n", t->ch, now - t->t_dead, mode_name(t->mode), why);
        release(*t, why); t->used = false; return;
    }
    {   // the game fades a corpse out (Character opacity +1ACh): measured for the corpse lifetime
        float op = 1.0f; __try { op = RDF(ch + 0x1AC); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (t->dead_seen && !t->fade_logged && op == op && op < 0.99f && op >= 0.0f) {
            t->fade_logged = true;
            gp_log("ragdoll: character %08X starts fading out %.1f s after its death (opacity %.2f, mode %s)\n", t->ch, now - t->t_dead, op, mode_name(t->mode));
            if (t->mode == M_LIMP) freeze(*t, "the game swaps in its corpse");
        }
    }
    // the animated pose of this frame
    LONG f = phys_frame();
    if (!world_xf(ch, &t->char_world)) return;
    if (t->anim_frame != f) { memcpy(t->prev_body, t->anim_body, sizeof t->anim_body); t->prev_frame = t->anim_frame; }
    rd::fk(t->sk, t->anim_local, t->char_world, t->anim_world);
    rd::body_frames(t->map, t->anim_world, t->anim_body);
    t->anim_frame = f; t->anim_dt = game_dt();
    if (!t->ph) t->ph = ph_now;
    int st = ai_state(*t);
    bool lethal = false;
    __try { DWORD d = RD32(t->ai + 0x18); if (d && RDF(d + AI_HEALTH) > 0.0f && RDF(d + AI_TAKEN) >= RDF(d + AI_HEALTH)) lethal = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    // dead = state 28, or a lethal hit the AI has not turned into state 28 yet (a hit on it within the last second: a
    // DamageTaken >= Health without a hit is stale - a reused shell, a re-armed reinforcement - not a death)
    bool recent_hit = hit_within(t->root, now, 1.0f);
    if (rd::life_alive(st, lethal)) { if (++t->alive_frames >= 3) t->seen_alive = true; } else t->alive_frames = 0;
    if (st != STATE_DEAD) { if (t->not28_since <= 0.0) t->not28_since = now; } else t->not28_since = 0.0;
    bool dead = rd::life_dead(st, lethal, recent_hit, t->seen_alive);
    if (!t->seen_alive && !t->stale_logged && (st == STATE_DEAD || lethal)) {   // never seen alive in this placement: leave it to the game
        t->stale_logged = true;
        gp_log("ragdoll: character %08X reads dead (AI state %d, DamageTaken %s Health) but was never seen alive in this placement: no ragdoll (a reused shell's old state)\n",
               t->ch, st, lethal ? ">=" : "<");
    }
    if (dead && !t->dead_seen) { t->dead_seen = true; t->t_dead = now; gp_log("ragdoll: character %08X died (AI state %d%s, mode %s)\n", t->ch, st, st == STATE_DEAD ? "" : ", lethal hit", mode_name(t->mode)); }
    if (!dead && t->dead_seen && (t->mode == M_LIMP || t->mode == M_FROZEN)) { t->dead_seen = false; release(*t, "the pooled character is alive again"); }
    // a corpse whose AI has left state 28 for a second (alive, or a stale DamageTaken that no hit made): not a corpse
    if (dead && st != STATE_DEAD && t->not28_since > 0.0 && now - t->not28_since > 1.0 && (t->mode == M_LIMP || t->mode == M_FROZEN)) {
        t->dead_seen = false; release(*t, "its AI is not in the death state (alive)"); dead = false;
    }
    if (!dead) { t->dead_seen = false; t->death_game = false; }
    t->state = st;
    // a teleport by the game (pool reuse, script) of a ragdolled character
    V3 rp; if (get_pos(t->root, &rp)) {
        if (t->mode != M_NONE && !t->root_moved_by_us && ph::len(ph::sub(rp, t->last_root)) > 4.0f) release(*t, "teleported");
        t->last_root = rp; t->root_moved_by_us = false;
    }
    // sims lost with the world (a rescan)
    if ((t->mode == M_ACTIVE || t->mode == M_LIMP) && (t->sp.r < 0 || !g_ra->valid(t->sp.r))) {
        if (t->mode == M_LIMP && t->have_bodies) freeze(*t, "physics world rebuilt"); else release(*t, "physics world rebuilt");
    }
    // the physics step has stopped (stalled_now): a ragdoll here would not move, so the game keeps (or gets back) the pose
    bool stall = stalled_now();
    if (stall && t->mode != M_NONE) {
        int act = rd::stall_action(t->mode, t->t_sim, g_c.stall_fall_seconds);
        if (act != rd::SA_KEEP) {
            stall_note(true);
            if (act == rd::SA_FREEZE) freeze(*t, "the physics step stopped");
            else {
                if (t->mode == M_LIMP) t->death_game = true;
                release(*t, t->mode == M_LIMP ? "the physics step stopped: the game's own death clip" : "the physics step stopped: back to the animation");
            }
        }
    }
    // transitions
    Hit hb[4]; int nh = 0; hits_for(t->root, now, 0.3f, hb, &nh, 4);
    if (dead && t->mode != M_LIMP && t->mode != M_FROZEN && (stall || t->death_game)) {   // the game's own death clip, now and later
        if (!t->death_game) { t->death_game = true; stall_note(false); }
        consume_hits(t->root);
    } else if (dead && t->mode != M_LIMP && t->mode != M_FROZEN) start_death(*t, now);
    else if (!dead && nh > 0 && stall) { stall_note(false); consume_hits(t->root); }
    else if (!dead && nh > 0) {
        if (t->mode == M_NONE) start_active(*t, now, hb[nh - 1]);
        if (t->mode == M_ACTIVE) for (int i = 0; i < nh; ++i) hit_active(*t, hb[i]);
        consume_hits(t->root);
    }
    // output
    Xf out[MAXB]; memcpy(out, t->anim_local, sizeof(Xf) * t->nb);
    if (t->mode == M_ACTIVE || t->mode == M_LIMP) {
        Xf B[NBODY]; interp_bodies(*t, phys_alpha(), B);
        rd::pose_from_bodies(t->sk, t->map, t->L, B, t->char_world, out);
        float w = t->mode == M_LIMP ? 1.0f : t->weight;
        if (w < 0.999f)
            for (int i = 0; i < t->nb; ++i) {
                if (t->map.driver[i] < 0) continue;
                out[i].q = ph::qslerp(t->anim_local[i].q, out[i].q, w);
                out[i].p = ph::add(t->anim_local[i].p, ph::mul(ph::sub(out[i].p, t->anim_local[i].p), w));
            }
        write_locals(*t, out);
    } else if (t->mode == M_FROZEN) {
        rd::pose_from_bodies(t->sk, t->map, t->L, t->frozen, t->char_world, out);
        write_locals(*t, out);
    } else if (g_force_bone[0] && g_force_angle != 0.0f) {   // test copy, milestone (a): one bone forced, nothing else
        int fb = rd::find_bone(t->sk, g_force_bone);
        if (fb >= 0) {
            out[fb].q = ph::qnorm(ph::qmul(out[fb].q, rd::qaxis(ph::v3(0, 0, 1), g_force_angle * 0.0174533f)));
            __try {
                Q4 sq = ph::stored_from_true(out[fb].q); DWORD nm = t->nm[fb];
                WRF(nm, sq.x); WRF(nm + 4, sq.y); WRF(nm + 8, sq.z); WRF(nm + 12, sq.w);
                for (int i = 0; i < t->nb; ++i) *(volatile BYTE*)(uintptr_t)(t->node[i] + 0x14) |= 2;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            static int logged = 0;
            if (logged++ < 3) gp_log("ragdoll: debug force bone %s (#%d) of character %08X turned %.0f deg about its local z\n", g_force_bone, fb, t->ch, g_force_angle);
        }
    }
    if (t->dead_seen && !t->handed && (t->mode == M_LIMP || t->mode == M_FROZEN)) offer_handoff(*t, t->mode == M_FROZEN ? t->frozen : t->cur);
    if (t->mode != M_NONE) g_st_frame.add((gp_now() - t0) * 1000.0);
}
// RD_CHARUPD: Scene::Update -> Character::Update(ecx = character), then our part
static void __fastcall CharUpdWrap(DWORD ch, void* edx) {
    ((Upd_t)(uintptr_t)R_CHARUPD)(ch, edx);
    if (!g_installed) return;
    __try { after_update(ch); } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int n = 0; if (n++ < 5) gp_log("ragdoll: exception in the character hook (character %08X)\n", ch);
    }
}

// RD_HIT: SendCommand(AI, 0x534 take_a_hit, 0) in character_root's take_a_hit relay; esi = the hit, ebp = the root
static volatile DWORD g_hit_args = 0, g_hit_root = 0;
static void record_hit(DWORD root, DWORD args) {
    Hit& h = g_hits[g_hit_next]; g_hit_next = (g_hit_next + 1) % 16;
    memset(&h, 0, sizeof h);
    __try {
        h.root = root; h.dmg = RDF(args); h.owner = RD32(args + 4); h.inflictor = RD32(args + 8); h.volume = RD32(args + 0xC);
        for (int k = 0; k < 3; ++k) { h.imp[k] = RDF(args + 0x10 + k * 4); h.pos[k] = RDF(args + 0x1C + k * 4); }
        h.type = (int)RD32(args + 0x28);
    } __except (EXCEPTION_EXECUTE_HANDLER) { h.root = 0; return; }
    h.t = gp_now(); h.frame = phys_frame();
    ++g_hits_seen;
    if (g_log_hits && g_logged_hits < 60) {
        ++g_logged_hits;
        V3 rp = ph::v3(0, 0, 0); get_pos(root, &rp);
        gp_log("ragdoll: take_a_hit on %08X: damage %.1f type %d impulse (%.1f, %.1f, %.1f) at (%.2f, %.2f, %.2f) = %.2f m above the root, owner %08X volume %08X\n",
               root, h.dmg, h.type, h.imp[0], h.imp[1], h.imp[2], h.pos[0], h.pos[1], h.pos[2], h.pos[1] - rp.y, h.owner, h.volume);
    }
}
static DWORD __fastcall HitWrap(DWORD ai, void* edx, int id, void* args) {
    DWORD root = g_hit_root, a = g_hit_args;
    if (g_installed && root && a) record_hit(root, a);
    return ((Cmd_t)(uintptr_t)R_HITSEND)(ai, edx, id, args);
}
__declspec(naked) static void HitStub() {
    __asm mov dword ptr [g_hit_args], esi
    __asm mov dword ptr [g_hit_root], ebp
    __asm jmp HitWrap
}

// ================================================================================================ physics callbacks
void ragdoll_world_reset() {   // the physics module is about to rebuild its world: the sims go, the poses stay
    if (!g_installed) return;
    for (auto& t : g_t) if (t.used && (t.mode == M_ACTIVE || t.mode == M_LIMP)) {
        if (t.mode == M_LIMP) freeze(t, "physics rescan"); else release(t, "physics rescan");
    }
}
bool ragdoll_busy() { if (!g_installed) return false; for (auto& t : g_t) if (t.used && t.mode == M_LIMP) return true; return false; }
int ragdoll_substeps(int base) { return (g_installed && sims() > 0 && g_c.substeps > base) ? g_c.substeps : base; }
void ragdoll_before_step(float dt) {
    if (!g_installed) return;
    uint32_t ms; if (g_clock_ok && read_game_ms(&ms)) g_steps.stepped(ms);
    if (g_stall_on) {
        if (g_stall_noted) gp_log("ragdoll: the physics step is back after %.1f s: %d ragdoll(s) had gone back to the game, %d hit(s) / death(s) "
                                  "were left to the game's own animation\n", gp_now() - g_stall_t0 + g_c.stall_seconds, g_stall_handed, g_stall_left);
        g_stall_on = false; g_stall_noted = false;
    }
    if (!g_ra) return;
    for (auto& t : g_t) {
        if (!t.used || t.sp.r < 0 || (t.mode != M_ACTIVE && t.mode != M_LIMP)) continue;
        memcpy(t.prv, t.cur, sizeof t.prv);
        t.t_sim += dt;
        if (t.mode == M_ACTIVE) {
            t.str.recover(dt, g_c.recover_seconds);
            rd::ActiveDrive ad; rd::active_drive(g_c, t.str, &ad);
            for (int b = 1; b < NBODY; ++b) {
                float ang; Q4 q = rd::joint_target(t.L, b, t.anim_body, &ang);
                float tq[4] = {q.x, q.y, q.z, q.w};
                g_ra->drive(t.sp.r, t.sp.joint[b], ad.hz[b], ad.damping[b], tq, ang);
            }
            const Xf& P = t.anim_body[rd::PELVIS];
            float pp[3] = {P.p.x, P.p.y, P.p.z}, pq[4] = {P.q.x, P.q.y, P.q.z, P.q.w};
            g_ra->anchor(t.sp.r, rd::PELVIS, pp, pq, ad.balance_hz, ad.balance_hz, ad.balance_damping, g_c.balance_force, g_c.balance_torque, dt);
            for (int fb : {(int)rd::L_CALF, (int)rd::R_CALF}) {   // the feet stay planted: the body bends, it does not pivot about the hips
                const Xf& F = t.anim_body[fb];
                float fp[3] = {F.p.x, F.p.y, F.p.z}, fq[4] = {F.q.x, F.q.y, F.q.z, F.q.w};
                g_ra->anchor(t.sp.r, fb, fp, fq, g_c.foot_hertz, g_c.foot_hertz * 0.5f, g_c.foot_damping, g_c.foot_force, g_c.foot_force * 0.1f, dt);
            }
            // blend: in at once, out when every part is strong again and the body follows the animation
            float in = g_c.blend_in > 0 ? dt / g_c.blend_in : 1.0f, outr = g_c.blend_out > 0 ? dt / g_c.blend_out : 1.0f;
            bool recovered = t.str.min_value() > 0.98f && gp_now() - t.t_mode > g_c.recover_seconds + 0.15;
            if (recovered) { t.releasing = true; t.weight -= outr; } else t.weight += in;
            if (t.weight > 1.0f) t.weight = 1.0f;
        } else {   // limp: the muscles follow the death animation for death_fade s, then let go
            float k = g_c.death_fade > 0 ? 1.0f - t.t_sim / g_c.death_fade : 0.0f;
            for (int b = 1; b < NBODY; ++b) {
                if (k > 0) {
                    float ang; Q4 q = rd::joint_target(t.L, b, t.anim_body, &ang);
                    float tq[4] = {q.x, q.y, q.z, q.w};
                    g_ra->drive(t.sp.r, t.sp.joint[b], g_c.joint_stiffness * k, g_c.joint_damping, tq, ang);
                } else g_ra->drive(t.sp.r, t.sp.joint[b], 0, 0, nullptr, 0);
            }
        }
    }
}
void ragdoll_after_step(float dt) {
    if (!g_installed || !g_ra) return;
    for (auto& t : g_t) {
        if (!t.used || t.sp.r < 0 || (t.mode != M_ACTIVE && t.mode != M_LIMP)) continue;
        float mv = 0, mw = 0; bool bad = false;
        for (int b = 0; b < NBODY; ++b) {
            float p[3], q[4], lv[3], av[3];
            if (g_ra->get(t.sp.r, b, p, q) < 0) { bad = true; break; }
            if (!(p[0] == p[0] && p[1] == p[1] && p[2] == p[2]) || fabsf(p[1]) > 1e5f) { bad = true; break; }
            t.cur[b].p = ph::v3(p[0], p[1], p[2]); Q4 qq = {q[0], q[1], q[2], q[3]}; t.cur[b].q = qq;
            g_ra->vel(t.sp.r, b, lv, av);
            float s = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]), w = sqrtf(av[0] * av[0] + av[1] * av[1] + av[2] * av[2]);
            if (s > mv) mv = s; if (w > mw) mw = w;
        }
        if (bad) { release(t, "body lost"); continue; }
        if (t.mode == M_ACTIVE && t.releasing && t.weight <= 0.0f) {
            gp_log("ragdoll: character %08X recovered after %.2f s (%d hits): back to the animation\n", t.ch, t.t_sim, t.hits);
            forget_sim(t); t.mode = M_NONE; t.weight = 0; t.hits = 0;
            continue;
        }
        if (t.mode == M_LIMP) {
            // fell out of the scanned level (no floor): keep the last good pose
            if (t.cur[rd::PELVIS].p.y < t.L.body0[rd::PELVIS].p.y - 60.0f && t.t_sim > 1.0f) { memcpy(t.cur, t.prv, sizeof t.cur); freeze(t, "fell out of the level"); continue; }
            if (t.settle.update(mv, mw, dt, 0.12f, 0.6f, 0.6f)) { freeze(t, "settled"); continue; }
            if (t.t_sim > g_c.settle_seconds) { freeze(t, "settle_seconds"); continue; }
        }
    }
}
// input tick (script context): the corpse's character node follows the pelvis; pushed active characters step along
static void move_root(Track& t, V3 p) {
    if (!set_wpos(t.root, p)) return;
    send(t.phys, MSG_PHYS_RESET, nullptr);
    t.root_moved_by_us = true; t.last_root = p;
    ++g_root_moves;
}
void ragdoll_tick(double now) {
    if (!g_installed) return;
    float dt = game_dt();
    for (auto& t : g_t) {
        if (!t.used) continue;
        if (now - t.seen > 3.0) {   // not updated for a while (hidden, despawned, far): let it go
            if (t.dead_seen) gp_log("ragdoll: character %08X no longer updated (hidden / despawned), last seen %.1f s after its death\n", t.ch, t.seen - t.t_dead);
            if (t.mode != M_NONE) release(t, "not updated");
            t.used = false; continue;
        }
        if (t.mode == M_LIMP && g_c.root_follow && t.have_bodies && ai_state(t) == STATE_DEAD) {   // never pin a live AI to a ragdoll
            V3 pel = t.cur[rd::PELVIS].p, cur;
            if (!get_pos(t.root, &cur)) continue;
            V3 want = ph::v3(pel.x, cur.y, pel.z);
            float from[3] = {pel.x, pel.y + 0.3f, pel.z}, to[3] = {pel.x, pel.y - 3.0f, pel.z}, hit[3];
            if (g_pa && g_pa->raycast_static(from, to, hit)) want.y = hit[1];
            else want.y = pel.y - 0.9f;
            if (ph::len(ph::sub(want, cur)) > 0.08f) move_root(t, want);
        }
        if (t.mode == M_ACTIVE && t.have_bodies && g_c.stumble_rate > 0 && dt > 0) {   // pushed off balance: step along
            V3 d = ph::sub(t.cur[rd::PELVIS].p, t.anim_body[rd::PELVIS].p); d.y = 0;
            float l = ph::len(d);
            if (l > 1.5f) { release(t, "the game moved the character away from its ragdoll"); continue; }
            if (l > g_c.stumble_distance) {
                V3 cur; if (!get_pos(t.root, &cur)) continue;
                float k = g_c.stumble_rate * dt; if (k > 0.5f) k = 0.5f;
                V3 step = ph::mul(d, (l - g_c.stumble_distance * 0.5f) / l * k);
                if (ph::len(step) > 0.12f) step = ph::mul(ph::norm(step), 0.12f);   // a step, never a jump
                V3 from3 = ph::v3(cur.x, cur.y + 0.5f, cur.z), to3 = ph::add(from3, ph::mul(ph::norm(step), ph::len(step) + 0.3f));
                float f3[3] = {from3.x, from3.y, from3.z}, t3[3] = {to3.x, to3.y, to3.z}, h3[3];
                if (g_pa && g_pa->raycast_static(f3, t3, h3)) continue;   // a wall in the way
                move_root(t, ph::add(cur, step));
                if (now - t.stumble_t > 0.5) { ++g_stumbles; gp_log("ragdoll: character %08X stumbles %.2f m (pelvis %.2f m off balance)\n", t.ch, ph::len(step), l); }
                t.stumble_t = now;
            }
        }
    }
    int a = 0, l = 0, f = 0; for (auto& t : g_t) if (t.used) { if (t.mode == M_ACTIVE) ++a; else if (t.mode == M_LIMP) ++l; else if (t.mode == M_FROZEN) ++f; }
    g_n_active = a; g_n_limp = l; g_n_frozen = f;
    if (g_c.debug_draw && now - g_last_draw >= g_draw_every) {
        g_last_draw = now;
        DWORD scene = 0, cam = 0; float m[16]; bool camok = false;
        if (g_debug_refs) {
            __try { scene = RD32(A_SCENE); cam = scene ? RD32(scene + 0x6C) : 0; if (cam) { ((Mat_t)(uintptr_t)P_CAMMAT)(cam, nullptr, m); camok = true; } }
            __except (EXCEPTION_EXECUTE_HANDLER) { camok = false; }
        }
        if (camok && (a + l + f) > 0) {
            gp_log("ragdoll: draw cam %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f %.3f fov %.2f\n", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14], RDF(cam + 0xB4));
            for (auto& t : g_t) {
                if (!t.used || t.mode == M_NONE) continue;
                const Xf* B = t.mode == M_FROZEN ? t.frozen : t.cur;
                for (int b = 0; b < NBODY; ++b) for (int k = 0; k < t.L.ncap[b]; ++k) {
                    V3 p1 = rd::xpoint(B[b], t.L.c1[b][k]), p2 = rd::xpoint(B[b], t.L.c2[b][k]);
                    gp_log("ragdoll: draw cap %s %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", mode_name(t.mode), b, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z, t.L.rad[b][k]);
                }
            }
            { V3 rp; if (get_pos(ram_root(), &rp)) gp_log("ragdoll: draw ram %.3f %.3f %.3f\n", rp.x, rp.y, rp.z); }   // overlay calibration
            gp_log("ragdoll: draw end\n");
        }
    }
    if (now - g_last_log >= g_c.log_seconds && (a + l + f > 0 || g_st_frame.n > 0)) {
        gp_log("ragdoll: stats %d active, %d limp, %d frozen | started %d active / %d limp, %d frozen, %d released, %d over the cap, "
               "%d spawn failures, %d outside the circle | hits seen %d, ray pushes %d, blasts %d, root moves %d, stumbles %d | per ragdolled character "
               "%.3f ms avg %.3f max (%d) | spawn %.2f ms avg | Box3D step (all bodies) %.3f ms avg %.3f max | step stops %d "
               "(%d ragdolls back to the game, %d hits / deaths left to it)\n",
               a, l, f, g_started_active, g_started_limp, g_frozen_total, g_released, g_cap_skips, g_spawn_fail, g_no_ground, g_hits_seen,
               g_ray_pushes, g_blasts_seen, g_root_moves, g_stumbles, g_st_frame.avg(), g_st_frame.mx, g_st_frame.n,
               g_st_spawn.avg(), g_st_step.avg(), g_st_step.mx, g_stalls, g_stall_handed_total, g_stall_left_total);
        g_st_frame.reset(); g_st_step.reset(); g_last_log = now;
    }
}
void ragdoll_step_cost(double ms) { if (g_installed && sims() > 0) g_st_step.add(ms); }
// a bullet segment (PH_BULLET): push the ragdoll body it crosses; a frozen corpse comes back to life
bool ragdoll_bullet(const float* from, const float* to, bool game_hit, DWORD hit_node) {
    if (!g_installed || !g_ra) return false;
    (void)game_hit; (void)hit_node;
    float h[3]; int b = -1;
    int r = g_ra->raycast(from, to, h, &b);
    V3 f = ph::v3(from[0], from[1], from[2]), tt = ph::v3(to[0], to[1], to[2]);
    V3 dir = ph::norm(ph::sub(tt, f));
    if (r >= 0) {
        for (auto& t : g_t) {
            if (!t.used || t.sp.r != r) continue;
            V3 at = ph::v3(h[0], h[1], h[2]);
            if (t.mode == M_ACTIVE) live_push(t, b, ph::mul(dir, g_c.hit_impulse * g_c.impulse_scale), at, g_c.hit_burst * g_c.hit_impulse * g_c.impulse_scale);
            else limp_bullet(t, b, ph::mul(dir, g_c.bullet_impulse * g_c.impulse_scale), at);
            g_ra->wake(t.sp.r);
            if (t.mode == M_LIMP) t.settle.quiet = 0;
            t.ray_hit_t = gp_now(); ++t.ray_hits; ++g_ray_pushes;
            return true;
        }
        return false;
    }
    // frozen corpses: capsule ray test, then a new limp ragdoll from the frozen pose with the push (not while the step is stopped)
    if (stalled_now()) return false;
    for (auto& t : g_t) {
        if (!t.used || t.mode != M_FROZEN || t.handed) continue;
        for (int bb = 0; bb < NBODY; ++bb) for (int k = 0; k < t.L.ncap[bb]; ++k) {
            float s;
            if (rd::ray_capsule(f, tt, rd::xpoint(t.frozen[bb], t.L.c1[bb][k]), rd::xpoint(t.frozen[bb], t.L.c2[bb][k]), t.L.rad[bb][k], &s) >= 0) continue;
            if (sims() >= g_c.max_ragdolls) return false;
            Xf bodies[NBODY]; memcpy(bodies, t.frozen, sizeof bodies);
            if (!rd::spawn(g_ra, t.L, bodies, nullptr, nullptr, g_c, g_c.limp_friction, &t.sp)) return false;
            memcpy(t.cur, bodies, sizeof t.cur); memcpy(t.prv, bodies, sizeof t.prv); t.have_bodies = true;
            t.mode = M_LIMP; t.t_mode = gp_now(); t.t_sim = g_c.death_fade + 1.0f; t.settle.quiet = 0;
            V3 at = ph::add(f, ph::mul(ph::sub(tt, f), s));
            limp_bullet(t, bb, ph::mul(dir, g_c.bullet_impulse * g_c.impulse_scale), at);
            ++g_ray_pushes;
            gp_log("ragdoll: shot corpse %08X wakes up (%s)\n", t.ch, rd::body_name(bb));
            return true;
        }
    }
    return false;
}
void ragdoll_blast(const float c[3], float radius, float impulse, float lift) {
    if (!g_installed) return;
    Blast& bl = g_blasts[g_blast_next]; g_blast_next = (g_blast_next + 1) % 8;
    bl.c = ph::v3(c[0], c[1], c[2]); bl.radius = radius; bl.impulse = impulse; bl.lift = lift > g_c.blast_lift ? lift : g_c.blast_lift; bl.t = gp_now();
    ++g_blasts_seen;
    for (auto& t : g_t) if (t.used && !t.handed && (t.mode == M_ACTIVE || t.mode == M_LIMP) && t.sp.r >= 0) {
        blast_on(t, bl, "live");
        if (t.mode == M_ACTIVE) {   // a powered body a blast reaches flies as before the ragdoll-hits tuning
            for (int b = 0; b < NBODY; ++b) t.str.s[b] = g_c.blast_strength;
            g_ra->gravity(t.sp.r, 0.5f);
            for (int b = 1; b < NBODY; ++b) g_ra->friction(t.sp.r, t.sp.joint[b], 0.0f);
        }
    }
    // frozen corpses in reach come back to life and fly (not while the step is stopped: they would hang in the air)
    if (stalled_now()) { mark("rd_blast", 30, 66); return; }
    for (auto& t : g_t) {
        if (!t.used || t.mode != M_FROZEN || t.handed) continue;
        if (ph::len(ph::sub(t.frozen[rd::PELVIS].p, bl.c)) > radius * 1.5f || sims() >= g_c.max_ragdolls) continue;
        Xf bodies[NBODY]; memcpy(bodies, t.frozen, sizeof bodies);
        if (!rd::spawn(g_ra, t.L, bodies, nullptr, nullptr, g_c, g_c.limp_friction, &t.sp)) continue;
        memcpy(t.cur, bodies, sizeof t.cur); memcpy(t.prv, bodies, sizeof t.prv); t.have_bodies = true;
        t.mode = M_LIMP; t.t_mode = gp_now(); t.t_sim = g_c.death_fade + 1.0f; t.settle.quiet = 0;
        blast_on(t, bl, "corpse");
    }
    mark("rd_blast", 30, 66);
}

// ================================================================================================ test copy rig
static Track* nearest_alive(V3 rp, float maxd) {
    Track* best = nullptr; float bd = maxd;
    for (auto& t : g_t) {
        if (!t.used || t.state == STATE_DEAD || t.mode == M_LIMP || t.mode == M_FROZEN) continue;
        V3 p; if (!get_pos(t.root, &p)) continue;
        float d = ph::len(ph::sub(p, rp)); if (d < bd) { bd = d; best = &t; }
    }
    return best;
}
static V3 view_dir(V3 ram) {
    DWORD cd = gp_cache_data(), cam = 0;
    __try { cam = cd ? RD32(cd + C_GAMECAM) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { cam = 0; }
    Xf w; if (cam && world_xf(cam, &w)) {
        V3 f = ph::norm(ph::v3(ph::qrot(w.q, ph::v3(0, 0, 1)).x, 0, ph::qrot(w.q, ph::v3(0, 0, 1)).z));
        if (ph::dot(f, ph::sub(ram, w.p)) < 0) f = ph::mul(f, -1.0f);
        if (ph::len(f) > 0.5f) return f;
    }
    V3 f; if (get_fwd(ram_root(), &f)) return f;
    return ph::v3(0, 0, 1);
}
static volatile LONG g_req_bring = 0, g_req_hit = 0, g_req_kill = 0, g_req_boom = 0, g_req_freeze = 0, g_req_turn = 0, g_req_vanilla = 0;
static DWORD g_target = 0;   // the root the debug keys act on
static Track* target_track() { for (auto& t : g_t) if (t.used && t.root == g_target) return &t; return nullptr; }
static void debug_bring(V3 rp) {
    Track* t = nearest_alive(rp, 60.0f);
    if (!t) { gp_log("ragdoll: debug bring: no living character tracked within 60 m\n"); return; }
    V3 f = view_dir(rp), r = ph::v3(f.z, 0, -f.x);
    V3 at = ph::add(ph::add(rp, ph::mul(f, g_bring_dist)), ph::mul(r, g_bring_side));
    float from[3] = {at.x, rp.y + 2.0f, at.z}, to[3] = {at.x, rp.y - 4.0f, at.z}, h[3];
    if (g_pa && g_pa->raycast_static(from, to, h)) at.y = h[1]; else at.y = rp.y;
    if (g_bring_ai_off) {   // no AI, and the locomotion it left running stops (else root motion keeps it running)
        send(t->ai, MSG_AI_NO_CONTROL, nullptr);
        DWORD mv = 0; __try { DWORD d = RD32(t->root + 0x18); mv = d ? RD32(d + R_MOVE) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        send(mv, MSG_MOVE_STOP, nullptr); send(mv, MSG_MOVE_IDLE, nullptr);
    }
    set_wpos(t->root, at);
    set_world_yaw(t->root, atan2f(rp.x - at.x, rp.z - at.z));
    send(t->phys, MSG_PHYS_RESET, nullptr);
    t->last_root = at; t->root_moved_by_us = true;
    g_target = t->root;
    gp_log("ragdoll: debug bring: character %08X (root %08X) put %.1f m ahead of Ram at (%.2f, %.2f, %.2f)%s\n", t->ch, t->root,
           g_bring_dist, at.x, at.y, at.z, g_bring_ai_off ? ", AI off" : "");
    mark("rd_bring", 3, 300);
}
static void debug_hit(V3 rp, float dmg) {
    Track* t = target_track(); if (!t) t = nearest_alive(rp, 40.0f);
    if (!t) { gp_log("ragdoll: debug hit: no target\n"); return; }
    V3 chest = t->anim_body[rd::CHEST].p;
    V3 d = ph::sub(chest, rp); d.y = 0; d = ph::norm(d);
    DWORD vol = 0; get_prop(t->phys, PROP_TORSO_CS, &vol);
    DWORD ram = ram_root();
    struct { float dmg; DWORD owner, inflictor, volume; float imp[3]; float pos[3]; int type; } h =
        {dmg, ram, ram, vol, {d.x * g_debug_hit_impulse, 0.15f * g_debug_hit_impulse, d.z * g_debug_hit_impulse}, {chest.x, chest.y, chest.z}, 0};
    bool ok = send(t->root, MSG_ROOT_TAKE_A_HIT, &h);
    gp_log("ragdoll: debug %s: take_a_hit %.0f damage on character %08X%s\n", dmg > 100 ? "kill" : "hit", dmg, t->ch, ok ? "" : " FAILED");
}
static DWORD g_boom_shell = 0; static double g_boom_at = 0.0; static V3 g_boom_c;
static void debug_boom(V3 rp, double now) {
    Track* t = target_track(); if (!t) t = nearest_alive(rp, 40.0f);
    if (!t) { gp_log("ragdoll: debug boom: no target\n"); return; }
    V3 tp = t->anim_body[rd::PELVIS].p;
    V3 f = ph::sub(tp, rp); f.y = 0; f = ph::norm(f); V3 r = ph::v3(f.z, 0, -f.x);
    V3 c = ph::add(ph::add(tp, ph::mul(f, -g_boom_back)), ph::mul(r, g_boom_side)); c.y = tp.y - 0.6f;
    DWORD cd = gp_cache_data(), L = 0, shell = 0;
    __try { L = cd ? RD32(cd + C_SHELLS) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    int n = 0; __try { n = L ? (int)RD32(L + 8) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (int i = 0; i < n && !shell; ++i) {
        DWORD sh = 0;
        __try { DWORD tt = RD32(A_LIST_ENTITY); DWORD k = tt ? RD32(tt + 0x24) : 0; DWORD a = RD32(L + 4); DWORD dd = a ? RD32(a + 4) : 0; sh = (k && dd) ? RD32(dd + (DWORD)i * 4 * k) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { sh = 0; }
        if (!sh || cls_handler(sh) != H_SHELL) continue;
        __try { DWORD d = RD32(sh + 0x18); if (d && RD32(d + 0x1C)) shell = sh; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!shell) { gp_log("ragdoll: debug boom: no ready shell in the pool (%d)\n", n); return; }
    int type = g_shell_type;
    __try { DWORD d = RD32(shell + 0x18); WR32(d + 0x24, shell); } __except (EXCEPTION_EXECUTE_HANDLER) {}   // as physics debug_boom
    send(shell, MSG_SHELL_MORPH, &type);
    float v[3] = {c.x, c.y, c.z}; send(shell, MSG_SHELL_AIM, v);
    set_wpos(shell, c);
    g_boom_shell = shell; g_boom_at = now + 0.3; g_boom_c = c;
    gp_log("ragdoll: debug boom: shell type %d armed at (%.2f, %.2f, %.2f), %.1f m from character %08X\n", type, c.x, c.y, c.z, ph::len(ph::sub(tp, c)), t->ch);
}
static void debug_boom_tick(double now) {
    if (!g_boom_shell || now < g_boom_at) return;
    DWORD shell = g_boom_shell; g_boom_shell = 0;
    set_wpos(shell, g_boom_c);
    __try { DWORD d = RD32(shell + 0x18); WR32(d + 0x24, ram_root()); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    struct { int explode, water; float v[3]; } e = {1, 0, {0, 0, 0}};
    bool ok = send(shell, MSG_SHELL_EXPLODE, &e);
    gp_log("ragdoll: debug boom: command_explode sent%s\n", ok ? "" : " FAILED");
    mark("rd_boom", 36, 50);
}
// ---- test copy: a scripted sequence run from the input tick ([ragdoll] debug_sequence = turn, wait 1.5, bring, ...),
// so it starts when Ram is really in control (fixed key-script waits drifted with the load time). Commands: wait S,
// turn, bring, hit, kill, boom, freeze, vanilla, fire S (left button held), shot NAME, burst NAME N MS (harness marks), end.
static void seq_step(double now) {
    if (!g_seq[0] || g_seq_pos < 0) return;
    if (g_seq_start == 0.0) {   // only in a level (the title screen also runs the input tick, with no level collision)
        PhCounts pc; memset(&pc, 0, sizeof pc); if (g_pa && phys_world_ready()) g_pa->counts(&pc);
        if (pc.static_shapes < 50) return;
    }
    if (g_seq_start == 0.0) { g_seq_start = now; g_seq_next = now + g_seq_delay; gp_log("ragdoll: debug sequence starts in %.1f s\n", g_seq_delay); }
    while (now >= g_seq_next && g_seq_pos >= 0) {
        const char* p = g_seq + g_seq_pos; while (*p == ' ' || *p == ',') ++p;
        if (!*p) { g_seq_pos = -1; break; }
        char cmd[96]; int n = 0; while (p[n] && p[n] != ',' && n < 95) { cmd[n] = p[n]; ++n; } cmd[n] = 0;
        g_seq_pos = (int)(p - g_seq) + n;
        char w[16] = "", a1[48] = ""; float f1 = 0; int i1 = 0, i2 = 0;
        sscanf(cmd, "%15s", w);
        if (!strcmp(w, "wait")) { sscanf(cmd, "%*s %f", &f1); g_seq_next = now + f1; break; }
        else if (!strcmp(w, "turn")) InterlockedExchange(&g_req_turn, 1);
        else if (!strcmp(w, "bring")) InterlockedExchange(&g_req_bring, 1);
        else if (!strcmp(w, "hit")) InterlockedExchange(&g_req_hit, 1);
        else if (!strcmp(w, "kill")) InterlockedExchange(&g_req_kill, 1);
        else if (!strcmp(w, "boom")) InterlockedExchange(&g_req_boom, 1);
        else if (!strcmp(w, "freeze")) InterlockedExchange(&g_req_freeze, 1);
        else if (!strcmp(w, "vanilla")) InterlockedExchange(&g_req_vanilla, 1);
        else if (!strcmp(w, "fire")) { sscanf(cmd, "%*s %f", &f1); InterlockedExchange(&g_fire_until_ms, (LONG)((now + f1) * 1000.0)); gp_log("ragdoll: debug fire %.2f s\n", f1); }
        else if (!strcmp(w, "shot")) { sscanf(cmd, "%*s %47s", a1); gp_log("debug keys: mark shot %s\n", a1); }
        else if (!strcmp(w, "burst")) { sscanf(cmd, "%*s %47s %d %d", a1, &i1, &i2); gp_log("debug keys: mark burst %s %d %d\n", a1, i1, i2); }
        else if (!strcmp(w, "end")) { gp_log("debug keys: mark rd_end\n"); g_seq_pos = -1; break; }
        else gp_log("ragdoll: debug sequence: unknown command '%s'\n", cmd);
    }
}
void ragdoll_input_tick(double now) {   // script context: the debug requests + the per-frame work above
    if (!g_installed) return;
    if (g_debug_refs) seq_step(now);
    DWORD ram = ram_root(); V3 rp;
    if (g_debug_refs && ram && get_pos(ram, &rp)) {
        if (InterlockedExchange(&g_req_bring, 0)) debug_bring(rp);
        if (InterlockedExchange(&g_req_hit, 0)) debug_hit(rp, g_debug_hit_damage);
        if (InterlockedExchange(&g_req_kill, 0)) debug_hit(rp, 10000.0f);
        if (InterlockedExchange(&g_req_boom, 0)) debug_boom(rp, now);
        if (InterlockedExchange(&g_req_freeze, 0)) { for (auto& t : g_t) if (t.used && t.mode == M_LIMP) freeze(t, "debug key"); }
        if (InterlockedExchange(&g_req_turn, 0)) { bool ok = gp_yaw_add(g_turn_deg); gp_log("ragdoll: debug turn %.0f deg%s\n", g_turn_deg, ok ? "" : " (not in mouse mode: nothing)"); }
        if (InterlockedExchange(&g_req_vanilla, 0)) {
            g_c.death_ragdoll = !g_c.death_ragdoll; g_c.active_hits = g_c.death_ragdoll;
            gp_log("ragdoll: debug toggle: death ragdoll / active hits %s\n", g_c.death_ragdoll ? "ON" : "OFF (vanilla deaths and hit reactions)");
        }
        debug_boom_tick(now);
        if (g_debug_god && (ram != g_god_root || now - g_god_at > 10.0)) {
            DWORD goodie = 0; __try { DWORD d = RD32(ram + 0x18); goodie = d ? RD32(d + 0x88) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            float secs = 120.0f; bool ok = send(goodie, -8, &secs);   // character_goodie_ctrl command_induce_god_mode (REF SV_GOD_SEND)
            static int nlog = 0;
            if (nlog++ < 3 || !ok) gp_log("ragdoll: debug god mode for Ram%s\n", ok ? "" : " FAILED");
            g_god_root = ram; g_god_at = now;
        }
        if (g_debug_ai_off)   // test copy: every living tracked character stops fighting (command_no_ai_control, once)
            for (auto& t : g_t) if (t.used && !t.ai_off && t.state != STATE_DEAD && t.state >= 0) {
                t.ai_off = true;
                bool ok = send(t.ai, MSG_AI_NO_CONTROL, nullptr);
                gp_log("ragdoll: debug AI off for character %08X%s\n", t.ch, ok ? "" : " FAILED");
            }
    }
    ragdoll_tick(now);
}
void ragdoll_post_input(BYTE* kb_edges, BYTE* kb_state, DWORD* mouse, double now) {
    (void)kb_state;
    if (!g_installed || !kb_edges) return;
    if (mouse) {   // test-copy sequence 'fire S': the left mouse button held (as [physics] debug_fire_key does)
        bool down = now * 1000.0 < (double)g_fire_until_ms;
        __try {
            if (down || g_fire_prev) *((BYTE*)mouse + 0x48) = 1;
            if (down && !g_fire_prev) mouse[0] |= 3; else if (down) mouse[0] |= 1; else if (g_fire_prev) mouse[0] = (mouse[0] & ~1u) | 4;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_fire_prev = down;
    }
    auto pressed = [&](int k) { return k >= 0 && k < 256 && (kb_edges[k] & 1); };
    if (pressed(g_key_bring)) InterlockedExchange(&g_req_bring, 1);
    if (pressed(g_key_hit)) InterlockedExchange(&g_req_hit, 1);
    if (pressed(g_key_kill)) InterlockedExchange(&g_req_kill, 1);
    if (pressed(g_key_boom)) InterlockedExchange(&g_req_boom, 1);
    if (pressed(g_key_freeze)) InterlockedExchange(&g_req_freeze, 1);
    if (pressed(g_key_turn)) InterlockedExchange(&g_req_turn, 1);
    if (pressed(g_key_vanilla)) InterlockedExchange(&g_req_vanilla, 1);
}

// ================================================================================================ corpse delay
// RD_CORPSE: the Take_A_Hit corpse state (thread in edi) is about to sleep 0.5 s before it swaps in the death-pose corpse
// and fades the body; an enemy gets corpse_seconds instead (the ragdoll has time to fall and settle), the player and
// any corpse over corpse_max waiting at once keep the vanilla 0.5 s (each waiting corpse holds a pooled character).
static DWORD R_CORPSE_BACK = 0x606D4B;
static volatile DWORD g_corpse_imm = 0x3F000000;
static double g_corpse_until[16];
static int g_corpse_long = 0, g_corpse_short = 0;
static DWORD __cdecl CorpseSecsC(DWORD thread) {
    float secs = 0.5f;
    DWORD root = 0;
    __try {   // thread +40h current frame, frame +0 the entity (TakeAHitMoveCtrl); its character_root is a few parents up
        DWORD fr = RD32(thread + 0x40); DWORD n = fr ? RD32(fr) : 0;
        for (int i = 0; i < 6 && n && !root; ++i) { if (cls_handler(n) == H_ROOT) root = n; else n = RD32(n + 0x40); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { root = 0; }
    double now = gp_now(); int waiting = 0, slot = -1;
    for (int i = 0; i < 16; ++i) { if (g_corpse_until[i] > now) ++waiting; else if (slot < 0) slot = i; }
    if (root && root != ram_root() && waiting < g_c.corpse_max && slot >= 0) {
        secs = g_c.corpse_seconds; g_corpse_until[slot] = now + secs; ++g_corpse_long;
    } else ++g_corpse_short;
    if (g_corpse_long + g_corpse_short <= 12)
        gp_log("ragdoll: corpse of %08X waits %.1f s before the game swaps in its death pose (%d waiting)\n", root, secs, waiting);
    union { float f; DWORD d; } u; u.f = secs;
    return u.d;
}
__declspec(naked) static void CorpseStub() {
    __asm pushad
    __asm pushfd
    __asm push edi
    __asm call CorpseSecsC
    __asm add esp, 4
    __asm mov g_corpse_imm, eax
    __asm popfd
    __asm popad
    __asm push 0
    __asm push dword ptr [g_corpse_imm]
    __asm jmp dword ptr [R_CORPSE_BACK]
}

// ================================================================================================ install
bool ragdoll_install() {
    if (!g_c.enabled) return false;
    for (const char* r : RD_REFS) if (!gp_ref_ok(r)) { gp_log("ragdoll: reference %s differs: OFF, nothing patched\n", r); return false; }
    g_debug_refs = true;
    for (const char* r : RD_DEBUG_REFS) if (!gp_ref_ok(r)) g_debug_refs = false;
    HMODULE m = phys_module();
    TODPhysics_GetRagdollApi_t get = m ? (TODPhysics_GetRagdollApi_t)GetProcAddress(m, "TODPhysics_GetRagdollApi") : nullptr;
    g_ra = get ? get(TODRAG_API_VERSION) : nullptr; g_ra_v2 = g_ra != nullptr;
    if (!g_ra && get) g_ra = get(1);   // an older TODPhysics.dll: no damping change when a powered body dies
    g_pa = phys_api();
    if (!g_ra || !g_pa) { gp_log("ragdoll: TODPhysics.dll has no ragdoll API (or [physics] did not load it): OFF\n"); return false; }
    if (!gp_site_ok("RD_CHARUPD") || !gp_site_ok("RD_HIT")) { gp_log("ragdoll: hook sites differ: OFF\n"); return false; }
    if (!gp_patch_call("RD_CHARUPD", (void*)&CharUpdWrap)) { gp_log("ragdoll: character update site not patched: OFF\n"); return false; }
    if (!gp_patch_call("RD_HIT", (void*)&HitStub)) gp_log("ragdoll: take_a_hit site not patched: no active hits, no death push\n");
    if (fabsf(g_c.corpse_seconds - 0.5f) > 1e-3f && g_c.corpse_max > 0 &&   // RD_CORPSE: enemies wait corpse_seconds
        !gp_patch_call("RD_CORPSE", (void*)&CorpseStub)) gp_log("ragdoll: corpse delay site differs: corpses keep the vanilla 0.5 s\n");
    for (auto& t : g_t) { t.used = false; t.sp.r = -1; }
    g_clock_ok = gp_ref_ok("PS_GAMEMS");
    if (!g_clock_ok) gp_log("ragdoll: the game-ms clock reference differs: no check that the physics steps (a ragdoll may hang while Ram drives)\n");
    g_installed = true;
    gp_log("ragdoll: on (death %d, active hits %d, max %d sims / %d frozen, stiffness %.1f Hz, recover %.2f s, impulse x%.2f, "
           "substeps %d, debug keys %s, debug draw %d)\n", g_c.death_ragdoll ? 1 : 0, g_c.active_hits ? 1 : 0, g_c.max_ragdolls,
           g_c.max_frozen, g_c.joint_stiffness, g_c.recover_seconds, g_c.impulse_scale, g_c.substeps, g_debug_refs ? "on" : "OFF (refs)",
           g_c.debug_draw ? 1 : 0);
    return true;
}

void* ragdoll_test_stub(const char* name) {
    if (!strcmp(name, "rd_charupd")) return (void*)&CharUpdWrap;
    if (!strcmp(name, "rd_hit")) return (void*)&HitStub;
    if (!strcmp(name, "rd_r_charupd")) return (void*)&R_CHARUPD;   // the host redirects the originals to stand-ins
    if (!strcmp(name, "rd_r_hit")) return (void*)&R_HITSEND;
    if (!strcmp(name, "rd_hits_seen")) return (void*)&g_hits_seen;
    if (!strcmp(name, "rd_corpse")) return (void*)&CorpseStub;
    if (!strcmp(name, "rd_r_corpse")) return (void*)&R_CORPSE_BACK;
    if (!strcmp(name, "rd_clock_ok")) return (void*)&g_clock_ok;
    return nullptr;
}
