// car_game.cpp - [car_physics] of TODGameplay.dll: the game's cars in the [physics] Box3D world.
// Off unless [car_physics] enabled = 1 AND [physics] enabled = 1 (cars live in the props' world); when off nothing here
// touches the game.
//
// What it does (docs: physics/CARS.md; addresses: dll/src/sites.h section "Car physics", dll/HOOKS.md):
//   * Cars: every car_move_ctrl node (script class handler 0x4DB730) found when [physics] walks the scene, and again
//     every car_rescan_seconds. Its parent is the vehicle_root Model (handler 0x824A70, the node the game moves and draws),
//     whose data +4Ch is the vehicle_ai (handler 0x803A50: ControlState +18h, Destroyed +C8h).
//   * Pushing props: each live car near Ram is a KINEMATIC Box3D box (its model's collision bounds + margin) swept from
//     the car's last pose to its new one each 0.04 s step (a little ahead), so Box3D shoves every prop body it meets;
//     the first touch adds a kick (forward with the car, out to the side, up over the bonnet, a tumble). The car's own
//     motion stays the game's: nothing is written into it. Vehicle models are left out of the static level mesh.
//   * Blasts on cars: the [physics] blast hooks (weapon_shell, Destroyable_Object) and a car's own explosion call
//     car_blast: a live car gets the game's own "hit with impulse" treatment (vehicle_ai take_a_hit 0x80D750 does the
//     same): command_activate_without_first_frame (377) to its car_move_ctrl, tumbling = 1, the spinning frictions,
//     in_control = 0, then linear velocity (RigidBody setter 0x500C20) and angular velocity (0x932560) changed by
//     car_logic's blast_kick. The game's out-of-control physics (0x4F6FC0) flies, tumbles and lands it.
//   * Wrecks (wreck_dynamic): a destroyed car becomes a DYNAMIC Box3D box (velocity taken over from the game's
//     RigidBody), and its pose is written to the vehicle_root node each frame (SetPos / SetOrient, as the props), and
//     to the RigidBody's Position_t / Orientation_t with zero velocity, until it settles.
//   * Bullets: a segment through a wreck box pushes it at the hit point.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "../../dll/src/gp_glue.h"
#include "car_glue.h"
#include "car_logic.h"
#include "car_api.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define RDB(a)  (*(volatile BYTE*)(uintptr_t)(a))

using ph::V3; using ph::Q4;

// [physics] (phys_game.cpp) helpers this module uses
void phys_request_scan();
void phys_blast_props(float x, float y, float z, float radius, float impulse_per_area, const char* who);
bool phys_debug_shell_at(float x, float y, float z);   // test copy: a real weapon_shell explosion there
bool phys_world_area(float centre[3], float* radius);  // the circle the [physics] scan put level geometry in

// ================================================================================================ game addresses
// proven by sites.h REF rows (CP_*, PH_*), checked in the exe by check_sites.py and in memory before install
static const DWORD A_SCENE = 0xA3DCBC, A_ASSETMGR = 0xA3D7C4, A_PLAYER_HOLDER = 0xA3B590;
static const DWORD VT_VCLOUD = 0x9D05A0, VT_OBOX = 0x9D02CC;
static const DWORD F_WORLDMAT = 0x4842C0, F_SETPOS = 0x88D6C0, F_SETORIENT = 0x88DB20, F_PIVCOLL = 0x884860,
                   F_MODELNAME = 0x884350, F_GETPOS = 0x88CA80, F_SETWPOS = 0x88E600;
static const DWORD H_CAR = 0x4DB730, H_VROOT = 0x824A70, H_VAI = 0x803A50;   // script class main handlers (class +60h)
static const DWORD F_GETLINVEL = 0x4CEEF0, F_SETLINVEL = 0x500C20, F_GETANGVEL = 0x932530, F_SETANGVEL = 0x932560,
                   F_SETPOST = 0x4CF040, F_SETORIT = 0x4CEFD0, F_DIRTY = 0x86B4B0;
// car_move_ctrl data (node +18h) members: offset / member index for the rewind record (Dirty 0x86B4B0)
static const DWORD M_IN_CONTROL = 0x4C, I_IN_CONTROL = 4, M_MAX_ENGINE = 0x50, M_ENGINE = 0x54, I_ENGINE = 6,
                   M_SPIN_AFRIC = 0x80, M_SPIN_LFRIC = 0x84, M_TUMBLING = 0xD8, I_TUMBLING = 39, M_MASS = 0x168,
                   M_L_FRIC = 0x3BC, I_L_FRIC = 171, M_A_FRIC = 0x3C0, I_A_FRIC = 172, M_CTRL_STATE = 0x470,
                   M_KEY_ACCEL = 0x384, I_KEY_ACCEL = 159, M_ACCEL_NUM = 0x388, I_ACCEL_NUM = 160,
                   M_KEY_DECEL = 0x38C, I_KEY_DECEL = 161, M_KEY_BRAKE = 0x3A4, I_KEY_BRAKE = 167, M_KEY_SLIDE = 0x3A8,
                   I_KEY_SLIDE = 168;
// RigidBody (the car_move_ctrl node itself): coll mass +50h, Position_t +ECh, Orientation_t +FCh (stored quaternion)
static const DWORD RB_MASS = 0x50, RB_POS_T = 0xEC, RB_ORI_T = 0xFC;
// vehicle_root data: movectrl +48h, AICtrl +4Ch; vehicle_ai data: ControlState +18h, Health +B0h, DamageTaken +B4h, Destroyed +C8h
static const DWORD VR_MOVECTRL = 0x48, VR_AICTRL = 0x4C, VA_CTRL = 0x18, VA_HEALTH = 0xB0, VA_DAMAGE = 0xB4, VA_DESTROYED = 0xC8;
static const int CMD_ACTIVATE_NO_FIRST = 377, CMD_TRAFFIC_ACCIDENT = 382;

static const char* CP_REFS[] = {"CP_CLS_CAR", "CP_CLS_VROOT", "CP_CLS_VAI", "CP_F_GETLINVEL", "CP_F_SETLINVEL", "CP_F_GETANGVEL",
    "CP_F_SETANGVEL", "CP_F_SETPOST", "CP_F_SETORIT", "CP_F_DIRTY", "CP_IN_CONTROL", "CP_CTRL_STATE", "CP_TUMBLING",
    "CP_FRICTIONS", "CP_KEYS", "CP_KEYNUM", "CP_ENGINE", "CP_VROOT_AI", "CP_VAI_DESTROYED", "CP_VAI_DAMAGE", "CP_VAI_MOVECTRL",
    "CP_CMD_ACTIVATE", "CP_CMD_ACCIDENT", "CP_KEYBRAKE", "CP_AXLE_VEL", "CP_AXLE_POS",
    "PH_F_WORLDMAT", "PH_F_SETPOS", "PH_F_SETORIENT", "PH_F_PIVCOLL", "PH_F_MODELNAME", "PH_VT_VCLOUD", "PH_VT_OBOX",
    "PH_VC_VERTS", "PH_OBOX_QUAT", "PH_LIST_DATA", "PH_LIST_COUNT", "PH_PIVNODE", "PH_NODE_OFF", "PH_TREE_CHILD",
    "PH_TREE_SIBLING", "PH_REGISTRY", "PH_REG_VEC", "SL_PLAYER", "F_GETPOS", "F_SENDMSG"};

// ================================================================================================ config
static cp::Cfg g_c;
static bool g_debug_log = false, g_debug_shots = false;
static int  g_kick_key = gp::KEY_NONE, g_drive_key = gp::KEY_NONE, g_list_key = gp::KEY_NONE, g_wreck_key = gp::KEY_NONE,
            g_place_key = gp::KEY_NONE, g_traffic_key = gp::KEY_NONE, g_enter_key = gp::KEY_NONE;
static char g_drive_props[256] = "crate,/bins/,chair,bench,stand,cart";
static float g_drive_ahead = 13.0f, g_drive_side = 16.0f, g_drive_seconds = 4.0f, g_drive_delay = 1.5f;
static int g_drive_max_props = 8;
static float g_kick_offset = 2.5f;
static bool g_axis_neg = false;   // debug_forward_negz: the car's forward is -z (it is +z: front_point_t ahead of the root, run c1)
static float g_place_hold_cfg = 0.6f, g_drive_speed = 16.0f, g_drive_launch = 0.3f, g_drive_offset = 4.5f, g_traffic_lead = 2.2f;

void car_load_config() {
    const gp::Ini& I = gp_ini();
    cp::Cfg c;
    c.enabled = I.geti("car_physics", "enabled", 0) != 0;
    c.push_props = I.geti("car_physics", "push_props", 1) != 0;
    c.blast_cars = I.geti("car_physics", "blast_cars", 1) != 0;
    c.wreck_dynamic = I.geti("car_physics", "wreck_dynamic", 1) != 0;
    c.bullets = I.geti("car_physics", "bullets", 1) != 0;
    c.impulse_scale = I.getf("car_physics", "impulse_scale", c.impulse_scale);
    c.margin = I.getf("car_physics", "margin", c.margin);
    c.lead = I.getf("car_physics", "lead", c.lead);
    c.push_gain = I.getf("car_physics", "push_gain", c.push_gain);
    c.push_lift = I.getf("car_physics", "push_lift", c.push_lift);
    c.push_full_speed = I.getf("car_physics", "push_full_speed", c.push_full_speed);
    c.push_min_speed = I.getf("car_physics", "push_min_speed", c.push_min_speed);
    c.push_spin = I.getf("car_physics", "push_spin", c.push_spin);
    c.blast_gain = I.getf("car_physics", "blast_gain", c.blast_gain);
    c.blast_area = I.getf("car_physics", "blast_area", c.blast_area);
    c.blast_lift_gain = I.getf("car_physics", "blast_lift_gain", c.blast_lift_gain);
    c.blast_spin = I.getf("car_physics", "blast_spin", c.blast_spin);
    c.blast_reach = I.getf("car_physics", "blast_reach", c.blast_reach);
    c.max_speed = I.getf("car_physics", "max_speed", c.max_speed);
    c.max_spin = I.getf("car_physics", "max_spin", c.max_spin);
    c.self_kick = I.getf("car_physics", "self_kick", c.self_kick);
    c.self_spin = I.getf("car_physics", "self_spin", c.self_spin);
    c.car_blast_radius = I.getf("car_physics", "car_blast_radius", c.car_blast_radius);
    c.car_blast_impulse = I.getf("car_physics", "car_blast_impulse", c.car_blast_impulse);
    c.wreck_mass = I.getf("car_physics", "wreck_mass", c.wreck_mass);
    c.wreck_friction = I.getf("car_physics", "wreck_friction", c.wreck_friction);
    c.wreck_restitution = I.getf("car_physics", "wreck_restitution", c.wreck_restitution);
    c.wreck_linear_damping = I.getf("car_physics", "wreck_linear_damping", c.wreck_linear_damping);
    c.wreck_angular_damping = I.getf("car_physics", "wreck_angular_damping", c.wreck_angular_damping);
    c.wreck_bullet_gain = I.getf("car_physics", "wreck_bullet_gain", c.wreck_bullet_gain);
    c.teleport_distance = I.getf("car_physics", "teleport_distance", c.teleport_distance);
    c.car_rescan_seconds = I.getf("car_physics", "car_rescan_seconds", c.car_rescan_seconds);
    c.car_radius = I.getf("car_physics", "car_radius", c.car_radius);
    c.max_cars = I.geti("car_physics", "max_cars", c.max_cars);
    c.clamp();
    g_c = c;
    // test copy only
    g_debug_log = I.geti("car_physics", "debug_log", 0) != 0;
    g_debug_shots = I.geti("car_physics", "debug_shots", 0) != 0;
    g_kick_key = gp_key_cfg("car_physics", "debug_kick_key", "NONE");
    g_drive_key = gp_key_cfg("car_physics", "debug_drive_key", "NONE");
    g_list_key = gp_key_cfg("car_physics", "debug_list_key", "NONE");
    g_wreck_key = gp_key_cfg("car_physics", "debug_wreck_key", "NONE");
    g_place_key = gp_key_cfg("car_physics", "debug_place_key", "NONE");
    g_traffic_key = gp_key_cfg("car_physics", "debug_traffic_key", "NONE");
    g_enter_key = gp_key_cfg("car_physics", "debug_enter_key", "NONE");
    g_traffic_lead = I.getf("car_physics", "debug_traffic_lead", 2.2f);
    if (const char* v = I.get("car_physics", "debug_drive_props")) { strncpy(g_drive_props, v, sizeof g_drive_props - 1); g_drive_props[sizeof g_drive_props - 1] = 0; }
    g_drive_ahead = I.getf("car_physics", "debug_drive_ahead", g_drive_ahead);
    g_drive_side = I.getf("car_physics", "debug_drive_side", g_drive_side);
    g_drive_seconds = I.getf("car_physics", "debug_drive_seconds", g_drive_seconds);
    g_drive_delay = I.getf("car_physics", "debug_drive_delay", g_drive_delay);
    g_drive_max_props = I.geti("car_physics", "debug_drive_max_props", g_drive_max_props);
    g_kick_offset = I.getf("car_physics", "debug_kick_offset", g_kick_offset);
    g_place_hold_cfg = I.getf("car_physics", "debug_place_hold", 0.6f);
    g_drive_speed = I.getf("car_physics", "debug_drive_speed", 16.0f);
    g_drive_launch = I.getf("car_physics", "debug_drive_launch", 0.3f);
    g_drive_offset = I.getf("car_physics", "debug_drive_offset", 4.5f);
    g_axis_neg = I.geti("car_physics", "debug_forward_negz", 0) != 0;
}

// ================================================================================================ engine calls
typedef void(__fastcall* Mat_t)(DWORD node, void* edx, float* out16);
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, const float* v);
typedef float*(__fastcall* Get4_t)(DWORD node, void* edx, float* out4);
typedef DWORD(__fastcall* PivColl_t)(DWORD model, void* edx, int pivot);
typedef const char*(__fastcall* Name_t)(DWORD model, void* edx);
typedef void(__fastcall* Dirty_t)(DWORD node, void* edx, int member);

static const PhApi* g_api = nullptr;
static const PhCarApi* g_capi = nullptr;
static bool g_installed = false;

static bool world_matrix(DWORD node, float m[16]) {
    if (!node) return false;
    __try { ((Mat_t)(uintptr_t)F_WORLDMAT)(node, nullptr, m); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return m[12] == m[12] && m[13] == m[13] && m[14] == m[14];
}
static bool node_pos(DWORD node, V3* p) {
    float v[3];
    __try { ((Vec_t)(uintptr_t)F_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    *p = ph::v3(v[0], v[1], v[2]); return true;
}
static DWORD rd(DWORD a) { __try { return a ? RD32(a) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static float rdf(DWORD a) { __try { float f = a ? RDF(a) : 0.0f; return f == f ? f : 0.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; } }
static DWORD data_of(DWORD node) { return rd(node ? node + 0x18 : 0); }
static bool node_off(DWORD node) {   // REF PH_NODE_OFF: (flags | flags >> 16) & 1
    __try { DWORD f = RD32(node + 0x28); return ((f | (f >> 16)) & 1) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
static DWORD cls_handler(DWORD node) {   // node -> +4 type -> +28h class -> +60h main handler
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD c = t ? RD32(t + 0x28) : 0; return c ? RD32(c + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool alive(DWORD node, DWORD vt) {   // registry slot still holds the node (REF PH_REGISTRY / PH_REG_VEC)
    __try {
        if (RD32(node) != vt) return false;
        DWORD am = RD32(A_ASSETMGR); if (!am) return false;
        DWORD v = RD32(node + 0x14) >> 8;
        int k = (int)((v >> 20) & 7) - 1; DWORD slot = v & 0x000FFFFF;
        if (k < 0 || k > 6) return false;
        DWORD vec = am + 0x130 + (DWORD)k * 0x10;
        if (slot >= RD32(vec + 4)) return false;
        return RD32(RD32(vec) + slot * 4) == node;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DWORD ram_root() { DWORD h = rd(A_PLAYER_HOLDER); DWORD d = rd(h ? h + 0x18 : 0); return rd(d ? d + 0x84 : 0); }
static const char* model_name(DWORD model) {
    __try { const char* s = ((Name_t)(uintptr_t)F_MODELNAME)(model, nullptr); if (s && !IsBadStringPtrA(s, 256)) return s; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "?";
}
static bool get4(DWORD f, DWORD node, float out[4]) {
    __try { ((Get4_t)(uintptr_t)f)(node, nullptr, out); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return out[0] == out[0] && out[1] == out[1] && out[2] == out[2];
}
static bool set4(DWORD f, DWORD node, const float in[4]) {
    __try { ((Vec_t)(uintptr_t)f)(node, nullptr, in); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void dirty_write(DWORD node, int member, DWORD off, DWORD value) {   // the rewind record first, as the game does
    DWORD d = data_of(node); if (!d) return;
    __try { ((Dirty_t)(uintptr_t)F_DIRTY)(node, nullptr, member); RD32(d + off) = value; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static DWORD fbits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }
static bool set_pose(DWORD node, V3 wp, Q4 wq) {   // world pose -> SetPos / SetOrient (local to the parent)
    V3 lp = wp; Q4 lq = wq;
    DWORD par = rd(node + 0x40);
    if (par) { float PM[16]; if (!world_matrix(par, PM) || !ph::local_from_world(PM, wp, wq, &lp, &lq)) return false; }
    Q4 sq = ph::stored_from_true(lq);
    float pos[3] = {lp.x, lp.y, lp.z}, q[4] = {sq.x, sq.y, sq.z, sq.w};
    __try {
        ((Vec_t)(uintptr_t)F_SETPOS)(node, nullptr, pos);
        ((Vec_t)(uintptr_t)F_SETORIENT)(node, nullptr, q);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ================================================================================================ cars
struct Car {
    DWORD mc, mc_vt, root, root_vt, ai, asset;
    char name[64];
    cp::Box box;
    int kb, wb;                       // kinematic box / wreck body (-1 none)
    cp::Pose pose, prev_pose, kin_last; bool have_pose, kin_init;
    V3 vel;                           // m/s, from the poses of the last frame
    bool destroyed, wreck, wreck_settled, wreck_failed, wreck_game, active;   // wreck_game: handed back to the game
    int ctrl;                         // vehicle_ai ControlState (1 = the player's)
    double destroyed_t, last_touch_log;
    cp::Pose w_prev, w_cur;           // wreck: the last two steps (interpolated write)
    V3 w_start; double w_since;
    int touches, kicks;
};
static std::vector<Car> g_cars;
static std::vector<DWORD> g_found;    // car_move_ctrl nodes seen by the last walk(s)
static double g_last_walk = -100.0, g_now = 0.0;
static float g_frame_dt = 0.0f;
static int g_skip_car = -1;           // the exploding car itself during its own blast
static int g_stat_kicks = 0, g_stat_touches = 0, g_stat_blasts = 0, g_stat_cars_hit = 0, g_stat_wrecks = 0, g_stat_bullets = 0;
static ph::Stats g_st_frame, g_st_walk;

static void mark(const char* what, int count, int ms) {   // test copy: the harness grabs screenshots on these lines
    if (!g_debug_shots) return;
    static double last = 0.0;
    double now = gp_now();
    if (now - last < 0.5) return;
    last = now;
    gp_log("debug keys: mark burst %s %d %d\n", what, count, ms);
}

bool car_active() { return g_installed; }
bool car_blasts_explosions() { return g_installed && g_c.blast_cars; }   // [mayhem] props then leaves car blasts to us
float car_prop_max_radius(const char* name, float def) {   // stands and carts are larger than a crate
    const char* ex = car_extra_props();
    return (ex && name && ph::name_match(name, ex)) ? (def > 2.8f ? def : 2.8f) : def;
}
const char* car_extra_props() {   // read straight from the ini: [physics] reads its props before [car_physics] is set up
    static char buf[256];
    const gp::Ini& I = gp_ini();
    if (I.geti("car_physics", "enabled", 0) == 0) return nullptr;
    const char* v = I.get("car_physics", "extra_props");
    strncpy(buf, v ? v : "stand,cart,stall,kiosk", sizeof buf - 1); buf[sizeof buf - 1] = 0;
    return buf;
}

// ---- collision points of the vehicle model (car frame) -> the box
static DWORD pivot_collision(DWORD model, int i) {
    __try { return ((PivColl_t)(uintptr_t)F_PIVCOLL)(model, nullptr, i); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD pivot_node(DWORD model, int i) {   // REF PH_PIVNODE
    __try {
        if (i == 0) return model;
        if (RD32(model + 0x64) & 0xFFFF0000) return model;
        DWORD a = RD32(model + 0xA0); if (!a) return 0;
        if (i >= (int)RD32(model + 0xA4)) return 0;
        return RD32(a + (DWORD)i * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static const char* node_name(DWORD n) {
    __try { const char* s = n ? (const char*)(uintptr_t)RD32(n + 0x4C) : nullptr; if (s && !IsBadStringPtrA(s, 64)) return s; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "";
}
static int shape_points(DWORD s, const float* M, V3* out) {   // as phys_game.cpp: VertexCloud vertices / OrientedBox corners
    __try {
        DWORD vt = RD32(s);
        if (vt == VT_VCLOUD) {
            int n = RDB(s + 0x2D);
            const float* v = (const float*)(uintptr_t)(RD32(s + 0x30) & ~3u);
            if (n < 3 || !v) return 0;
            for (int i = 0; i < n; ++i) out[i] = ph::tod_point(M, ph::v3(v[3 * i], v[3 * i + 1], v[3 * i + 2]));
            return n;
        }
        if (vt == VT_OBOX) {
            V3 h = ph::v3(RDF(s + 0x28) * 0.5f, RDF(s + 0x2C) * 0.5f, RDF(s + 0x30) * 0.5f);
            V3 c = ph::v3(RDF(s + 0x34), RDF(s + 0x38), RDF(s + 0x3C));
            Q4 q = {RDF(s + 0x40), RDF(s + 0x44), RDF(s + 0x48), RDF(s + 0x4C)};
            q = ph::true_from_stored(q);
            if (!(h.x > 0 && h.y > 0 && h.z > 0) || h.x > 50 || h.y > 50 || h.z > 50) return 0;
            for (int i = 0; i < 8; ++i)
                out[i] = ph::tod_point(M, ph::add(c, ph::qrot(q, ph::v3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z))));
            return 8;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static bool list_info(DWORD list, DWORD* data, DWORD* n) {
    __try { *data = RD32(list); *n = RD32(list + 4); return *n <= 4096; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct BoxCache { DWORD asset; cp::Box box; int np; };
static BoxCache g_box_cache[32]; static int g_nbox_cache = 0;
static cp::Box measure_box_raw(DWORD root, const ph::Rigid& r, int* npts_out);
static cp::Box measure_box(DWORD root, const ph::Rigid& r, int* npts_out) {
    DWORD asset = rd(root + 0x50);
    for (int i = 0; i < g_nbox_cache; ++i) if (g_box_cache[i].asset == asset) { *npts_out = g_box_cache[i].np; return g_box_cache[i].box; }
    cp::Box b = measure_box_raw(root, r, npts_out);
    if (asset && *npts_out >= 4) { BoxCache& e = g_box_cache[g_nbox_cache < 32 ? g_nbox_cache++ : 31]; e.asset = asset; e.box = b; e.np = *npts_out; }
    return b;
}
static cp::Box measure_box_raw(DWORD root, const ph::Rigid& r, int* npts_out) {
    static V3 pts[4096]; int np = 0; V3 tmp[256];
    int npiv = (int)rd(rd(root + 0x50) + 0x34);
    for (int pi = 0; pi < npiv && pi < 128; ++pi) {
        DWORD list = pivot_collision(root, pi), data, n;
        if (!list || !list_info(list, &data, &n) || n == 0) continue;
        DWORD pn = pivot_node(root, pi); float PW[16];
        if (!pn || (pi > 0 && node_off(pn)) || !_strnicmp(node_name(pn), "Replace", 7) || !world_matrix(pn, PW)) continue;
        int np0 = np;
        for (DWORD i = 0; i < n && i < 256; ++i) {
            DWORD s = rd(data + i * 4) & ~3u;
            int k = s ? shape_points(s, PW, tmp) : 0;
            for (int j = 0; j < k && np < 4096; ++j) pts[np++] = ph::qrot(ph::qconj(r.q), ph::sub(tmp[j], r.p));   // world -> car frame
        }
        if (g_debug_log && np > np0) {   // which pivot gives what (the box is their bounds)
            V3 lo = pts[np0], hi = pts[np0];
            for (int j = np0; j < np; ++j) { lo.x = fminf(lo.x, pts[j].x); lo.y = fminf(lo.y, pts[j].y); lo.z = fminf(lo.z, pts[j].z);
                                             hi.x = fmaxf(hi.x, pts[j].x); hi.y = fmaxf(hi.y, pts[j].y); hi.z = fmaxf(hi.z, pts[j].z); }
            gp_log("car_physics:   pivot %d %s: %u shapes, x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n", pi, node_name(pn), n, lo.x, hi.x, lo.y, hi.y, lo.z, hi.z);
        }
    }
    *npts_out = np;
    cp::Box b = cp::box_from_points(pts, np, g_c.margin);
    if (!b.ok) { b = cp::default_box(); b.half.x += g_c.margin; b.half.z += g_c.margin; }
    return b;
}

static bool car_pose(DWORD root, cp::Pose* p) {
    float M[16]; if (!world_matrix(root, M)) return false;
    ph::Rigid r = ph::rigid_from_tod(M); if (!r.ok) return false;
    p->p = r.p; p->q = r.q; return true;
}
static V3 world_center(const Car& c) { return ph::add(c.pose.p, ph::qrot(c.pose.q, c.box.center)); }

static void drop_bodies(Car& c) {
    if (g_api && c.kb >= 0) g_api->body_remove(c.kb);
    if (g_api && c.wb >= 0) g_api->body_remove(c.wb);
    c.kb = c.wb = -1; c.kin_init = false;
}
static void make_kin(Car& c) {
    if (!g_capi || !g_c.push_props || c.kb >= 0 || c.wreck || !c.have_pose) return;
    float h[3] = {c.box.half.x, c.box.half.y, c.box.half.z}, o[3] = {c.box.center.x, c.box.center.y, c.box.center.z};
    float p[3] = {c.pose.p.x, c.pose.p.y, c.pose.p.z}, q[4] = {c.pose.q.x, c.pose.q.y, c.pose.q.z, c.pose.q.w};
    c.kb = g_capi->kin_create(h, o, p, q, (void*)(uintptr_t)c.root);
    c.kin_last = c.pose; c.kin_init = c.kb >= 0;
}

// ---- the walk: every car_move_ctrl node of the scene (also called from the [physics] scan for every node)
void car_scan_node(DWORD node) {
    if (!g_installed) return;
    if (cls_handler(node) != H_CAR) return;
    for (DWORD f : g_found) if (f == node) return;
    if ((int)g_found.size() < 256) g_found.push_back(node);
}
bool car_scan_skip_model(DWORD model) {   // a vehicle_root: its collision is the car's own box, not static level
    return g_installed && g_c.push_props && cls_handler(model) == H_VROOT;
}
static bool read_links(DWORD n, DWORD* sib, DWORD* child) {
    __try { *sib = RD32(n + 0x34); *child = RD32(n + 0x44); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void walk_cars() {
    double t0 = gp_now();
    g_found.clear();
    DWORD scene = rd(A_SCENE), root = scene ? rd(scene + 0x44) : 0, sib, child;
    static DWORD stack[16384]; int sp = 0, visited = 0;
    if (root) stack[sp++] = root;
    while (sp > 0 && visited < 400000) {
        DWORD n = stack[--sp];
        if (!read_links(n, &sib, &child)) continue;
        ++visited;
        if (sib && sp < 16383) stack[sp++] = sib;
        if (child && sp < 16383) stack[sp++] = child;
        car_scan_node(n);
    }
    g_last_walk = gp_now();
    g_st_walk.add((g_last_walk - t0) * 1000.0);
    if (g_debug_log) gp_log("car_physics: walk %d nodes, %d car_move_ctrl, %.2f ms\n", visited, (int)g_found.size(), (g_last_walk - t0) * 1000.0);
}

static int find_car(DWORD mc) { for (size_t i = 0; i < g_cars.size(); ++i) if (g_cars[i].mc == mc) return (int)i; return -1; }

static void read_flags(Car& c) {
    DWORD ad = data_of(c.ai);
    c.destroyed = ad && rd(ad + VA_DESTROYED) != 0;
    c.ctrl = ad ? (int)rd(ad + VA_CTRL) : -1;
}

static void log_car(const char* why, const Car& c) {
    DWORD md = data_of(c.mc), ad = data_of(c.ai);
    float v[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};
    get4(F_GETLINVEL, c.mc, v); get4(F_GETANGVEL, c.mc, w);
    V3 up = ph::qrot(c.pose.q, ph::v3(0, 1, 0)), fw = ph::qrot(c.pose.q, ph::v3(0, 0, 1));
    V3 rbp = ph::v3(rdf(c.mc + RB_POS_T), rdf(c.mc + RB_POS_T + 4), rdf(c.mc + RB_POS_T + 8));
    V3 fp = ph::v3(rdf(c.mc + 0x2A8), rdf(c.mc + 0x2AC), rdf(c.mc + 0x2B0)), rp = ph::v3(rdf(c.mc + 0x2B8), rdf(c.mc + 0x2BC), rdf(c.mc + 0x2C0));
    gp_log("car_physics: %s %s mc %08X root %08X ai %08X at (%.2f, %.2f, %.2f) up (%.2f %.2f %.2f) fwd(+z) (%.2f %.2f %.2f) | rb pos_t (%.2f, %.2f, %.2f) "
           "v (%.2f %.2f %.2f) w (%.2f %.2f %.2f) mass %.0f | in_control %u tumbling %u ctrl_state %u | ai ctrl %d destroyed %d health %.0f damage %.0f | "
           "front_t (%.2f, %.2f, %.2f) rear_t (%.2f, %.2f, %.2f) | off %d kb %d wb %d\n",
           why, c.name, c.mc, c.root, c.ai, c.pose.p.x, c.pose.p.y, c.pose.p.z, up.x, up.y, up.z, fw.x, fw.y, fw.z, rbp.x, rbp.y, rbp.z,
           v[0], v[1], v[2], w[0], w[1], w[2], rdf(md + M_MASS), md ? rd(md + M_IN_CONTROL) : 0, md ? rd(md + M_TUMBLING) : 0,
           md ? rd(md + M_CTRL_STATE) : 0, c.ctrl, c.destroyed ? 1 : 0, rdf(ad + VA_HEALTH), rdf(ad + VA_DAMAGE), fp.x, fp.y, fp.z,
           rp.x, rp.y, rp.z, node_off(c.root) ? 1 : 0, c.kb, c.wb);
}

static void refresh_list(V3 ram) {
    // drop dead ones
    for (size_t i = g_cars.size(); i-- > 0;) {
        Car& c = g_cars[i];
        if (!alive(c.mc, c.mc_vt) || !alive(c.root, c.root_vt) || rd(c.mc + 0x40) != c.root) {
            if (g_debug_log) gp_log("car_physics: car %s %08X gone\n", c.name, c.mc);
            drop_bodies(c); g_cars.erase(g_cars.begin() + (ptrdiff_t)i);
        }
    }
    for (DWORD mc : g_found) {
        if (find_car(mc) >= 0 || (int)g_cars.size() >= g_c.max_cars) continue;
        DWORD root = rd(mc + 0x40);
        if (!root || cls_handler(root) != H_VROOT) continue;
        DWORD ai = rd(data_of(root) + VR_AICTRL);
        if (!ai || cls_handler(ai) != H_VAI) continue;
        if (rd(data_of(root) + VR_MOVECTRL) != mc) continue;
        Car c; memset(&c, 0, sizeof c);
        c.mc = mc; c.mc_vt = rd(mc); c.root = root; c.root_vt = rd(root); c.ai = ai; c.asset = rd(root + 0x50);
        c.kb = c.wb = -1;
        const char* nm = model_name(root); const char* sl = strrchr(nm, '/');
        if (sl && sl > nm) { const char* s2 = sl - 1; while (s2 > nm && *s2 != '/') --s2; nm = *s2 == '/' ? s2 + 1 : s2; }
        strncpy(c.name, nm, sizeof c.name - 1);
        g_cars.push_back(c);
    }
    (void)ram;
}

// ---- the game's own "hit with impulse" on a live car (vehicle_ai take_a_hit 0x80D750 does exactly this)
static void game_kick(Car& c, V3 dv, V3 dw, const char* why) {
    DWORD md = data_of(c.mc); if (!md) return;
    gp_send(c.mc, CMD_ACTIVATE_NO_FIRST, nullptr);              // a parked car starts simulating (idle state only)
    if (c.ctrl != 1) gp_send(c.ai, CMD_TRAFFIC_ACCIDENT, nullptr);   // a rail traffic car becomes a physics car
    // the RigidBody's own pose may be stale on a parked car: take the node's (take_a_hit does the same)
    float rp[4] = {c.pose.p.x, c.pose.p.y, c.pose.p.z, 0.0f};
    V3 rbp = ph::v3(rdf(c.mc + RB_POS_T), rdf(c.mc + RB_POS_T + 4), rdf(c.mc + RB_POS_T + 8));
    if (ph::len(ph::sub(rbp, c.pose.p)) > 0.25f) {
        Q4 sq = ph::stored_from_true(c.pose.q); float q4[4] = {sq.x, sq.y, sq.z, sq.w};
        set4(F_SETPOST, c.mc, rp); set4(F_SETORIT, c.mc, q4);
    }
    dirty_write(c.mc, I_TUMBLING, M_TUMBLING, 1);
    dirty_write(c.mc, I_A_FRIC, M_A_FRIC, rd(md + M_SPIN_AFRIC));
    dirty_write(c.mc, I_L_FRIC, M_L_FRIC, rd(md + M_SPIN_LFRIC));
    dirty_write(c.mc, I_IN_CONTROL, M_IN_CONTROL, 0);
    float v[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};
    get4(F_GETLINVEL, c.mc, v); get4(F_GETANGVEL, c.mc, w);
    float nv[4] = {v[0] + dv.x, v[1] + dv.y, v[2] + dv.z, 0.0f}, nw[4] = {w[0] + dw.x, w[1] + dw.y, w[2] + dw.z, 0.0f};
    set4(F_SETLINVEL, c.mc, nv); set4(F_SETANGVEL, c.mc, nw);
    ++g_stat_cars_hit;
    gp_log("car_physics: %s throws %s: speed +(%.1f %.1f %.1f) m/s, spin +(%.1f %.1f %.1f) rad/s (was v (%.1f %.1f %.1f))\n", why, c.name,
           dv.x, dv.y, dv.z, dw.x, dw.y, dw.z, v[0], v[1], v[2]);
}
static void wreck_kick(Car& c, V3 dv, V3 dw, const char* why) {
    float v[3], w[3]; g_api->body_vel(c.wb, v, w);
    float nv[3] = {v[0] + dv.x, v[1] + dv.y, v[2] + dv.z}, nw[3] = {w[0] + dw.x, w[1] + dw.y, w[2] + dw.z};
    g_capi->set_vel(c.wb, nv, nw);
    c.wreck_settled = false; c.w_since = g_now;
    ++g_stat_cars_hit;
    gp_log("car_physics: %s throws the wreck of %s: speed +(%.1f %.1f %.1f) m/s, spin +(%.1f %.1f %.1f) rad/s\n", why, c.name, dv.x, dv.y, dv.z, dw.x, dw.y, dw.z);
}
static void kick(Car& c, const cp::Kick& k, const char* why) {
    if (!k.hit) return;
    if (c.wreck && c.wb >= 0) wreck_kick(c, k.dv, k.dw, why);
    else game_kick(c, k.dv, k.dw, why);
}

// ---- wrecks: the game's car hands over to a dynamic Box3D box
static void take_over_wreck(Car& c, V3 extra_v, V3 extra_w) {
    if (!g_capi || c.wb >= 0 || !c.have_pose) return;
    if (c.kb >= 0) { g_api->body_remove(c.kb); c.kb = -1; c.kin_init = false; }
    float v[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};
    get4(F_GETLINVEL, c.mc, v); get4(F_GETANGVEL, c.mc, w);
    cp::Box b = c.box; b.half.x -= g_c.margin; b.half.z -= g_c.margin;
    float h[3] = {b.half.x, b.half.y, b.half.z}, o[3] = {b.center.x, b.center.y, b.center.z};
    float p[3] = {c.pose.p.x, c.pose.p.y, c.pose.p.z}, q[4] = {c.pose.q.x, c.pose.q.y, c.pose.q.z, c.pose.q.w};
    c.wb = g_capi->wreck_create(h, o, p, q, g_c.wreck_mass, g_c.wreck_friction, g_c.wreck_restitution, g_c.wreck_linear_damping,
                                g_c.wreck_angular_damping, (void*)(uintptr_t)c.root);
    if (c.wb < 0) { c.wreck_failed = true; gp_log("car_physics: wreck body for %s not created\n", c.name); return; }
    float lv[3] = {v[0] + extra_v.x, v[1] + extra_v.y, v[2] + extra_v.z}, aw[3] = {w[0] + extra_w.x, w[1] + extra_w.y, w[2] + extra_w.z};
    g_capi->set_vel(c.wb, lv, aw);
    c.wreck = true; c.wreck_settled = false; c.w_prev = c.w_cur = c.pose; c.w_start = c.pose.p; c.w_since = g_now;
    ++g_stat_wrecks;
    gp_log("car_physics: %s is a wreck now: Box3D body #%d (%.0f kg, box %.2f x %.2f x %.2f m), speed (%.1f %.1f %.1f), spin (%.1f %.1f %.1f)\n",
           c.name, c.wb, g_c.wreck_mass, h[0] * 2, h[1] * 2, h[2] * 2, lv[0], lv[1], lv[2], aw[0], aw[1], aw[2]);
    mark("cp_wreck", 30, 100);
}
static void write_wreck(Car& c, float alpha) {
    cp::Pose p;
    p.p = ph::add(c.w_prev.p, ph::mul(ph::sub(c.w_cur.p, c.w_prev.p), alpha));
    p.q = ph::qslerp(c.w_prev.q, c.w_cur.q, alpha);
    set_pose(c.root, p.p, p.q);
    // the RigidBody follows, standing still, so the game's own car code (if it still runs) starts from here
    float pp[4] = {p.p.x, p.p.y, p.p.z, 0.0f}; Q4 sq = ph::stored_from_true(p.q); float q4[4] = {sq.x, sq.y, sq.z, sq.w};
    float z[4] = {0, 0, 0, 0};
    set4(F_SETPOST, c.mc, pp); set4(F_SETORIT, c.mc, q4); set4(F_SETLINVEL, c.mc, z); set4(F_SETANGVEL, c.mc, z);
    c.pose = p;
}

// Box3D only has the level near Ram (the [physics] scan circle): a wreck that flies out of it would fall through
// nothing, so the game's own car physics takes it back there, with the wreck's speed and spin
static bool in_world(V3 p) {
    float c[3], r;
    if (!phys_world_area(c, &r)) return false;
    float dx = p.x - c[0], dz = p.z - c[2];
    return dx * dx + dz * dz < (r - 4.0f) * (r - 4.0f) && p.y > c[1] - 40.0f;
}
static void hand_back_wreck(Car& c, const char* why) {
    float v[3] = {0, 0, 0}, w[3] = {0, 0, 0};
    g_api->body_vel(c.wb, v, w);
    write_wreck(c, 1.0f);
    float v4[4] = {v[0], v[1], v[2], 0.0f}, w4[4] = {w[0], w[1], w[2], 0.0f};
    gp_send(c.mc, CMD_ACTIVATE_NO_FIRST, nullptr);
    dirty_write(c.mc, I_TUMBLING, M_TUMBLING, 1);
    dirty_write(c.mc, I_IN_CONTROL, M_IN_CONTROL, 0);
    set4(F_SETLINVEL, c.mc, v4); set4(F_SETANGVEL, c.mc, w4);
    g_api->body_remove(c.wb); c.wb = -1; c.wreck = false; c.wreck_game = true;
    gp_log("car_physics: wreck %s handed back to the game (%s) at (%.1f, %.1f, %.1f), speed (%.1f %.1f %.1f)\n", c.name, why,
           c.pose.p.x, c.pose.p.y, c.pose.p.z, v[0], v[1], v[2]);
}

// ================================================================================================ blasts
void car_blast(float cx, float cy, float cz, float radius, float ipa, float lift, const char* who) {
    if (!g_installed || !g_c.blast_cars) return;
    V3 c = ph::v3(cx, cy, cz);
    ++g_stat_blasts;
    int hit = 0;
    for (size_t i = 0; i < g_cars.size(); ++i) {
        Car& car = g_cars[i];
        if ((int)i == g_skip_car || !car.have_pose || node_off(car.root)) continue;
        float mass = rdf(data_of(car.mc) + M_MASS);
        if (car.wreck) mass = g_c.wreck_mass;
        cp::Kick k = cp::blast_kick(world_center(car), mass, c, radius, ipa, lift, g_c, cp::hash((unsigned)car.mc ^ (unsigned)(g_now * 1000)));
        if (!k.hit) continue;
        char why[64]; _snprintf(why, sizeof why, "%s blast (%.0f%%)", who, k.k * 100.0f); why[sizeof why - 1] = 0;
        kick(car, k, why);
        ++hit;
    }
    if (hit) mark("cp_blast", 30, 100);
}

void car_bullet(const float from[3], const float to[3], float bullet_impulse) {
    if (!g_installed || !g_c.bullets || !g_capi) return;
    float h[3];
    int b = g_capi->raycast_wreck(from, to, h);
    if (b < 0) return;
    V3 d = ph::norm(ph::sub(ph::v3(to[0], to[1], to[2]), ph::v3(from[0], from[1], from[2])));
    float j = bullet_impulse * g_c.wreck_bullet_gain * g_c.impulse_scale;
    float imp[3] = {d.x * j, d.y * j + j * 0.1f, d.z * j};
    g_api->impulse(b, imp, h);
    for (auto& c : g_cars) if (c.wb == b) { c.wreck_settled = false; c.w_since = g_now; }
    ++g_stat_bullets;
}

// ================================================================================================ per frame
static bool g_kick_req = false, g_drive_req = false, g_list_req = false, g_wreck_req = false, g_place_req = false, g_traffic_req = false, g_enter_req = false;
static int g_track = -1; static double g_track_until = 0.0, g_track_next = 0.0;

static int nearest_car(V3 rp, bool want_live, float maxd) {
    int best = -1; float bd = maxd;
    for (size_t i = 0; i < g_cars.size(); ++i) {
        const Car& c = g_cars[i];
        if (!c.have_pose || c.ctrl == 1) continue;
        if (want_live && (c.destroyed || c.wreck)) continue;
        float d = ph::len(ph::sub(c.pose.p, rp));
        if (d < bd) { bd = d; best = (int)i; }
    }
    return best;
}
static void track(int i, double seconds) { g_track = i; g_track_until = g_now + seconds; g_track_next = 0.0; }

static V3 view_dir(V3 ram);   // below (debug rig)
static void rig_start(V3 rp, bool drive);
static void traffic_rig(V3 rp);
static void enter_start(V3 rp);
static void enter_tick(V3 rp);
static void rig_tick();

void car_frame(double now, float game_dt) {
    if (!g_installed) return;
    double t0 = gp_now();
    g_now = now; g_frame_dt = game_dt;
    DWORD ram = ram_root(); V3 rp;
    if (!ram || !node_pos(ram, &rp)) return;
    if (now - g_last_walk >= g_c.car_rescan_seconds) walk_cars();
    refresh_list(rp);
    int measured = 0;   // the pivot walk of a new car model costs milliseconds: spread over frames
    for (size_t i = 0; i < g_cars.size(); ++i) {
        Car& c = g_cars[i];
        bool was_destroyed = c.destroyed;
        read_flags(c);
        cp::Pose p;
        bool ok = car_pose(c.root, &p);
        bool near_ram = ok && ph::len(ph::sub(p.p, rp)) <= g_c.car_radius;
        bool hidden = node_off(c.root);
        if (!ok || hidden || !near_ram) {   // out of play: no bodies
            if (c.kb >= 0 || c.wb >= 0) {
                if (g_debug_log || c.wb >= 0)
                    gp_log("car_physics: %s leaves play (%s)%s\n", c.name, !ok ? "no pose" : hidden ? "hidden by the game" : "far from Ram", c.wb >= 0 ? ", its wreck body dropped" : "");
                drop_bodies(c);
            }
            c.wreck = false; c.have_pose = false; c.active = false;
            continue;
        }
        DWORD asset = rd(c.root + 0x50);
        if (!c.active || asset != c.asset) {   // first time near Ram (or a new model: measure again), one car per frame
            if (measured++ > 0) { c.have_pose = false; continue; }
            ph::Rigid r; float M[16]; world_matrix(c.root, M); r = ph::rigid_from_tod(M);
            int np = 0; c.box = measure_box(c.root, r, &np); c.asset = asset;
            const char* nm = model_name(c.root); const char* sl = strrchr(nm, '/');
            if (sl && sl > nm) { const char* s2 = sl - 1; while (s2 > nm && *s2 != '/') --s2; nm = *s2 == '/' ? s2 + 1 : s2; }
            strncpy(c.name, nm, sizeof c.name - 1);
            gp_log("car_physics: car %s (%08X) %.1f m from Ram: box %.2f x %.2f x %.2f m centre (%.2f, %.2f, %.2f) from %d collision points%s\n",
                   c.name, c.mc, ph::len(ph::sub(p.p, rp)), c.box.half.x * 2, c.box.half.y * 2, c.box.half.z * 2, c.box.center.x,
                   c.box.center.y, c.box.center.z, np, np < 4 ? " (default box)" : "");
            if (c.kb >= 0) { g_api->body_remove(c.kb); c.kb = -1; c.kin_init = false; }
            c.active = true; c.have_pose = false;
        }
        if (!c.wreck) {
            c.prev_pose = c.have_pose ? c.pose : p;
            V3 raw = c.have_pose ? cp::lin_vel(c.prev_pose.p, p.p, game_dt) : ph::v3(0, 0, 0);
            if (game_dt > 0.0f) c.vel = c.have_pose ? ph::add(ph::mul(c.vel, 0.7f), ph::mul(raw, 0.3f)) : raw;
            c.pose = p; c.have_pose = true;
            if (cp::teleported(c.prev_pose.p, p.p, g_c.teleport_distance)) { c.kin_last = p; c.vel = ph::v3(0, 0, 0); c.kin_init = false; }
        }
        // a car blew up: it jumps (its own blast), props and other cars around are thrown
        if (c.destroyed && !was_destroyed && c.have_pose) {
            c.destroyed_t = now;
            gp_log("car_physics: %s destroyed (vehicle_ai Destroyed, control state %d)\n", c.name, c.ctrl);
            log_car("destroyed", c);
            if (g_c.blast_cars) {
                V3 at = world_center(c);
                g_skip_car = (int)i;
                phys_blast_props(at.x, at.y, at.z, g_c.car_blast_radius, g_c.car_blast_impulse, "car explosion");   // -> car_blast too
                g_skip_car = -1;
            }
            cp::Kick k = cp::self_kick(g_c, cp::hash((unsigned)c.mc ^ (unsigned)(now * 1000)));
            bool player_inside = c.ctrl == 1;
            if (g_c.wreck_dynamic && !player_inside && in_world(c.pose.p)) take_over_wreck(c, g_c.blast_cars ? k.dv : ph::v3(0, 0, 0), g_c.blast_cars ? k.dw : ph::v3(0, 0, 0));
            else if (g_c.blast_cars && !player_inside) game_kick(c, k.dv, k.dw, "its own explosion");
            if (g_debug_log) track((int)i, 6.0);
        }
        if (c.destroyed && !c.wreck && !c.wreck_failed && !c.wreck_game && g_c.wreck_dynamic && c.ctrl != 1 && c.have_pose && in_world(c.pose.p))
            take_over_wreck(c, ph::v3(0, 0, 0), ph::v3(0, 0, 0));   // destroyed before we saw it, or the player left it
        if (!c.destroyed && c.wreck) {   // repaired / respawned in place: the game owns it again
            if (c.wb >= 0) { g_api->body_remove(c.wb); c.wb = -1; }
            c.wreck = false;
        }
        if (!c.destroyed) { c.wreck_game = false; c.wreck_failed = false; }
        if (c.wreck && c.wb >= 0 && !in_world(c.pose.p)) hand_back_wreck(c, "it left the simulated area");
        if (!c.wreck) make_kin(c);
    }
    if (g_list_req) { g_list_req = false; for (auto& c : g_cars) if (c.active) log_car("list", c); }
    if (g_kick_req) {
        g_kick_req = false;
        int i = nearest_car(rp, false, 80.0f);
        if (i < 0) gp_log("car_physics: debug kick: no car near Ram\n");
        else {   // a blast g_kick_offset m to the side of the car (the side facing Ram), as a grenade there would
            Car& c = g_cars[i];
            V3 side = ph::qrot(c.pose.q, ph::v3(1, 0, 0));
            if (ph::dot(side, ph::sub(rp, c.pose.p)) < 0) side = ph::mul(side, -1.0f);
            V3 at = ph::add(world_center(c), ph::mul(side, g_kick_offset)); at.y = c.pose.p.y + 0.2f;
            log_car("before kick", c);
            gp_log("car_physics: debug kick: a shell-sized blast %.1f m beside %s\n", g_kick_offset, c.name);
            phys_blast_props(at.x, at.y, at.z, 7.0f, 450.0f, "debug");   // [physics] shell numbers: props and cars
            track(i, 6.0);
            mark("cp_kick", 40, 100);
        }
    }
    if (g_wreck_req) {   // test copy: a real game shell explodes beside the nearest live car (the game damages it)
        g_wreck_req = false;
        int i = nearest_car(rp, true, 80.0f);
        if (i >= 0) {
            Car& c = g_cars[i];
            V3 side = ph::qrot(c.pose.q, ph::v3(1, 0, 0));
            if (ph::dot(side, ph::sub(rp, c.pose.p)) < 0) side = ph::mul(side, -1.0f);
            V3 at = ph::add(world_center(c), ph::mul(side, g_kick_offset)); at.y = c.pose.p.y + 0.3f;
            bool ok = phys_debug_shell_at(at.x, at.y, at.z);
            gp_log("car_physics: debug wreck: a game shell beside %s (health %.0f, damage %.0f)%s\n", c.name, rdf(data_of(c.ai) + VA_HEALTH),
                   rdf(data_of(c.ai) + VA_DAMAGE), ok ? "" : " NOT armed (references)");
            log_car("before shell", c);
            track(i, 8.0);
        }
    }
    if (g_drive_req) { g_drive_req = false; rig_start(rp, true); }
    if (g_traffic_req) { g_traffic_req = false; traffic_rig(rp); }
    if (g_enter_req) { g_enter_req = false; enter_start(rp); }
    enter_tick(rp);
    if (g_place_req) { g_place_req = false; rig_start(rp, false); }
    rig_tick();
    if (g_track >= 0 && g_track < (int)g_cars.size() && now < g_track_until && now >= g_track_next) {
        g_track_next = now + 0.2; log_car("track", g_cars[g_track]);
    }
    g_st_frame.add((gp_now() - t0) * 1000.0);
}

void car_before_step(int i, int n, float step) {
    if (!g_installed || !g_c.push_props || !g_capi) return;
    float t = n > 0 ? (float)(i + 1) / (float)n : 1.0f;
    for (auto& c : g_cars) {
        if (c.kb < 0 || c.wreck || !c.have_pose) continue;
        cp::Pose tg = cp::sweep_target(c.kin_last, c.pose, t, c.vel, g_c.lead);
        float p[3] = {tg.p.x, tg.p.y, tg.p.z}, q[4] = {tg.q.x, tg.q.y, tg.q.z, tg.q.w};
        g_capi->kin_move(c.kb, p, q, step, c.kin_init ? 0 : 1);
        c.kin_init = true;
        if (i == n - 1) c.kin_last = c.pose;
    }
}

struct Kicked { int prop; double t; };
static Kicked g_kicked[64]; static int g_kick_slot = 0;
void car_after_step() {
    if (!g_installed || !g_capi) return;
    PhTouch t[64];
    int n = g_capi->touches(t, 64);
    for (int k = 0; k < n; ++k) {
        int ci = -1;
        for (size_t i = 0; i < g_cars.size(); ++i) if (g_cars[i].kb == t[k].car || g_cars[i].wb == t[k].car) { ci = (int)i; break; }
        if (ci < 0) continue;
        Car& c = g_cars[ci];
        ++c.touches; ++g_stat_touches;
        V3 cv = c.vel;
        if (c.wreck) { float v[3], w[3]; g_api->body_vel(c.wb, v, w); cv = ph::v3(v[0], v[1], v[2]); }
        bool recent = false;
        for (int r = 0; r < 64; ++r) if (g_kicked[r].prop == t[k].prop && g_now - g_kicked[r].t < 0.5) { recent = true; break; }
        if (recent) continue;
        g_kicked[g_kick_slot].prop = t[k].prop; g_kicked[g_kick_slot].t = g_now; g_kick_slot = (g_kick_slot + 1) & 63;
        cp::Kick kk = cp::push_kick(cv, ph::v3(t[k].normal[0], t[k].normal[1], t[k].normal[2]), g_c, cp::hash((unsigned)t[k].prop * 7919u + (unsigned)g_stat_touches));
        if (!kk.hit) continue;
        float m = g_api->body_mass(t[k].prop);
        float imp[3] = {kk.dv.x * m, kk.dv.y * m, kk.dv.z * m};
        float at[3] = {t[k].point[0], t[k].point[1] + 0.15f, t[k].point[2]};   // a bit high: the prop tips over the bumper
        g_api->impulse(t[k].prop, imp, at);
        float v[3], w[3]; g_api->body_vel(t[k].prop, v, w);
        float nw[3] = {w[0] + kk.dw.x, w[1] + kk.dw.y, w[2] + kk.dw.z};
        g_capi->set_vel(t[k].prop, nullptr, nw);
        ++c.kicks; ++g_stat_kicks;
        if (g_stat_kicks <= 40 || g_debug_log)
            gp_log("car_physics: %s (%.1f m/s) hits prop #%d (%.0f kg): +(%.1f %.1f %.1f) m/s\n", c.name, ph::len(cv), t[k].prop, m, kk.dv.x, kk.dv.y, kk.dv.z);
        mark("cp_push", 24, 80);
    }
    // wreck poses after this step
    for (auto& c : g_cars) {
        if (!c.wreck || c.wb < 0) continue;
        float p[3], q[4];
        int aw = g_api->body_get(c.wb, p, q);
        if (aw < 0) continue;
        c.w_prev = c.w_cur;
        c.w_cur.p = ph::v3(p[0], p[1], p[2]); Q4 qq = {q[0], q[1], q[2], q[3]}; c.w_cur.q = qq;
        if (aw == 0 && !c.wreck_settled) {
            c.wreck_settled = true;
            V3 up = ph::qrot(c.w_cur.q, ph::v3(0, 1, 0));
            gp_log("car_physics: wreck %s settled at (%.2f, %.2f, %.2f) after %.1f s, %.1f m from where it blew up, %s (up y %.2f)\n", c.name,
                   p[0], p[1], p[2], g_now - c.w_since, ph::len(ph::sub(c.w_cur.p, c.w_start)), up.y < -0.5f ? "on its roof" : up.y < 0.5f ? "on its side" : "on its wheels", up.y);
        }
    }
}

void car_after_frame(float alpha) {
    if (!g_installed) return;
    for (auto& c : g_cars) {
        if (!c.wreck || c.wb < 0) continue;
        write_wreck(c, c.wreck_settled ? 1.0f : alpha);
    }
}

void car_after_scan() {
    if (!g_installed) return;
    for (auto& c : g_cars) {   // the old world is gone with every body in it
        c.kb = -1; c.kin_init = false;
        if (c.wb >= 0) {   // a wreck at rest stays a wreck (a scan never runs while one moves)
            c.wb = -1;
            if (c.wreck && c.have_pose) { c.wreck = false; take_over_wreck(c, ph::v3(0, 0, 0), ph::v3(0, 0, 0)); }
        }
    }
}
void car_rewind_end() {
    if (!g_installed) return;
    for (auto& c : g_cars) {
        cp::Pose p;
        if (!car_pose(c.root, &p)) continue;
        c.pose = c.prev_pose = c.kin_last = p; c.kin_init = false; c.vel = ph::v3(0, 0, 0);
        if (c.wb >= 0) {
            float pp[3] = {p.p.x, p.p.y, p.p.z}, q[4] = {p.q.x, p.q.y, p.q.z, p.q.w};
            g_api->body_set(c.wb, pp, q, 0);
            c.w_prev = c.w_cur = p;
        }
    }
}

// ================================================================================================ debug rig (test copy)
static V3 view_dir(V3 ram) {   // flat, from the game camera's position toward Ram (the camera sits behind him)
    DWORD cd = gp_cache_data(); DWORD cam = cd ? rd(cd + 0x260) : 0; float M[16];
    if (cam && world_matrix(cam, M)) {
        V3 f = ph::norm(ph::v3(ram.x - M[12], 0.0f, ram.z - M[14]));
        if (ph::len(f) > 0.5f) return f;
    }
    return ph::v3(1, 0, 0);
}
static void set_world_pos(DWORD node, V3 p) {
    float v[3] = {p.x, p.y, p.z};
    __try { ((Vec_t)(uintptr_t)F_SETWPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static bool read_kids(DWORD n, DWORD* vt, DWORD* sib, DWORD* child) {
    __try { *vt = RD32(n); *sib = RD32(n + 0x34); *child = RD32(n + 0x44); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The rig (test copy). Every car near a spot the camera sees is traffic on rails (control state 2: the traffic admin
// puts it on its section every frame, run c1), so the nearest live car is first made a physics car the game's way
// (command_traffic_accident, as a crash does), then held for debug_place_hold s at its start pose (node, RigidBody
// pose, no speed) until the game leaves it there.
//   place (debug_place_key): broadside debug_drive_ahead m in front of Ram, standing (kick / wreck keys then hit it)
//   drive (debug_drive_key): debug_drive_side m left of that spot, nose across the view; props matching
//         debug_drive_props are lined up on its path around the spot and made bodies by a [physics] scan; after
//         debug_drive_delay s its accelerator is held for debug_drive_seconds (the game's own car code drives it).
struct Rig { int car; int phase; bool drive; double t0, go, until; V3 start, spot; Q4 q; };
static Rig g_rig = {-1, 0, false, 0, 0, 0, {0, 0, 0}, {0, 0, 0}, {0, 0, 0, 1}};
#define g_place_hold g_place_hold_cfg

static void rig_props(V3 rp, V3 spot, V3 f, V3 r) {
    struct Cand { DWORD n; float d; } cand[32]; int nc = 0;
    DWORD scene = rd(A_SCENE), root = scene ? rd(scene + 0x44) : 0, vt, sib, child;
    static DWORD stack[16384]; int sp = 0, visited = 0;
    if (root) stack[sp++] = root;
    while (sp > 0 && visited < 400000) {
        DWORD nd = stack[--sp];
        if (!read_kids(nd, &vt, &sib, &child)) continue;
        ++visited;
        if (sib && sp < 16383) stack[sp++] = sib;
        if (child && sp < 16383) stack[sp++] = child;
        if (vt != 0x9CCE48 || node_off(nd) || cls_handler(nd) == H_VROOT) continue;
        const char* nm = model_name(nd);
        if (!ph::name_match(nm, g_drive_props) || ph::name_match(nm, "vehicle,replace,_static,lod,door")) continue;
        V3 p; if (!node_pos(nd, &p)) continue;
        float d = ph::len(ph::sub(p, rp)); if (d > 250.0f) continue;
        if (nc < 32) cand[nc++] = Cand{nd, d};
        else { int w = 0; for (int k = 1; k < 32; ++k) if (cand[k].d > cand[w].d) w = k; if (d < cand[w].d) cand[w] = Cand{nd, d}; }
    }
    for (int a = 0; a < nc; ++a) for (int b = a + 1; b < nc; ++b) if (cand[b].d < cand[a].d) { Cand t = cand[a]; cand[a] = cand[b]; cand[b] = t; }
    int np = nc < g_drive_max_props ? nc : g_drive_max_props;
    for (int k = 0; k < np; ++k) {   // a staggered double row along the path, centred on the spot
        float along = ((float)(k / 2) - (float)((np + 1) / 2 - 1) * 0.5f) * 1.6f, across = (k & 1) ? 0.8f : -0.8f;
        V3 at = ph::add(ph::add(spot, ph::mul(r, along)), ph::mul(f, across));
        at.y = rp.y + 0.3f;
        set_world_pos(cand[k].n, at);
        gp_log("car_physics: rig: prop %s put on the path at (%.2f, %.2f, %.2f)\n", model_name(cand[k].n), at.x, at.y, at.z);
    }
    phys_request_scan();
    gp_log("car_physics: rig: %d props on the path around (%.2f, %.2f, %.2f)\n", np, spot.x, spot.y, spot.z);
}
static void rig_hold(Car& c) {   // node + RigidBody at the start pose, standing still
    set_pose(c.root, g_rig.start, g_rig.q);
    float pp[4] = {g_rig.start.x, g_rig.start.y, g_rig.start.z, 0.0f}; Q4 sq = ph::stored_from_true(g_rig.q);
    float q4[4] = {sq.x, sq.y, sq.z, sq.w}, z[4] = {0, 0, 0, 0};
    set4(F_SETPOST, c.mc, pp); set4(F_SETORIT, c.mc, q4); set4(F_SETLINVEL, c.mc, z); set4(F_SETANGVEL, c.mc, z);
    // OverdoseVehicle front_point_t +2A8h / rear_point_t +2B8h (vehicles doc 2.3): the root is their midpoint (run c1)
    V3 fp = ph::v3(rdf(c.mc + 0x2A8), rdf(c.mc + 0x2AC), rdf(c.mc + 0x2B0)), bp = ph::v3(rdf(c.mc + 0x2B8), rdf(c.mc + 0x2BC), rdf(c.mc + 0x2C0));
    float half = ph::len(ph::sub(fp, bp)) * 0.5f;
    if (half > 0.3f && half < 6.0f) {
        V3 fw = ph::qrot(g_rig.q, ph::v3(0, 0, 1));
        V3 nf = ph::add(g_rig.start, ph::mul(fw, half)), nb = ph::sub(g_rig.start, ph::mul(fw, half));
        __try {
            RDF(c.mc + 0x2A8) = nf.x; RDF(c.mc + 0x2AC) = nf.y; RDF(c.mc + 0x2B0) = nf.z;
            RDF(c.mc + 0x2B8) = nb.x; RDF(c.mc + 0x2BC) = nb.y; RDF(c.mc + 0x2C0) = nb.z;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    c.pose.p = g_rig.start; c.pose.q = g_rig.q; c.prev_pose = c.pose; c.kin_last = c.pose; c.kin_init = false; c.vel = ph::v3(0, 0, 0);
}
static float collision_floor(DWORD model, float node_y) {   // lowest collision point below the node origin (m)
    static V3 tmp[256]; float lo = 0.0f; bool any = false;
    int npiv = (int)rd(rd(model + 0x50) + 0x34);
    for (int pi = 0; pi < npiv && pi < 64; ++pi) {
        DWORD list = pivot_collision(model, pi), data, n;
        if (!list || !list_info(list, &data, &n)) continue;
        DWORD pn = pivot_node(model, pi); float PW[16];
        if (!pn || (pi > 0 && node_off(pn)) || !_strnicmp(node_name(pn), "Replace", 7) || !world_matrix(pn, PW)) continue;
        for (DWORD i = 0; i < n && i < 256; ++i) {
            DWORD sh = rd(data + i * 4) & ~3u;
            int k = sh ? shape_points(sh, PW, tmp) : 0;
            for (int j = 0; j < k; ++j) { float d = tmp[j].y - node_y; if (!any || d < lo) { lo = d; any = true; } }
        }
    }
    return any ? lo : 0.0f;
}
// test copy: the moving rail traffic car whose position debug_traffic_lead s ahead is nearest to Ram (and ahead of him)
// gets the debug_drive_props laid on the road there, standing on the level (Box3D ray), across its lane
static void traffic_rig(V3 rp) {
    V3 f = view_dir(rp);
    int best = -1; float bd = 1e30f; V3 hit = ph::v3(0, 0, 0), dir = ph::v3(0, 0, 1);
    for (size_t i = 0; i < g_cars.size(); ++i) {
        const Car& c = g_cars[i];
        V3 vh = ph::v3(c.vel.x, 0.0f, c.vel.z);
        float sp = ph::len(vh);
        if (!c.have_pose || c.ctrl != 2 || c.destroyed || sp < 4.0f || sp > 40.0f) continue;
        float tb = g_traffic_lead, db = 1e30f;
        for (float t = 1.0f; t <= 4.0f; t += 0.1f) { float dd = ph::len(ph::sub(ph::add(c.pose.p, ph::mul(vh, t)), rp)); if (dd < db) { db = dd; tb = t; } }
        V3 h = ph::add(c.pose.p, ph::mul(vh, tb));
        float d = ph::len(ph::sub(h, rp));
        if (d > 32.0f) continue;
        V3 rr = ph::v3(-f.z, 0.0f, f.x); float lateral = fabsf(ph::dot(ph::sub(h, rp), rr));
        float score = d - 0.5f * ph::dot(ph::sub(h, rp), f) + 2.0f * fabsf(lateral - 8.0f);   // in view, beside Ram (not behind him)
        if (score < bd) { bd = score; best = (int)i; hit = h; dir = ph::mul(vh, 1.0f / sp); }
    }
    if (best < 0) {
        gp_log("car_physics: traffic rig: no moving rail car within 32 m of its lead point\n");
        for (auto& c : g_cars) if (c.have_pose)
            gp_log("car_physics:   %s ctrl %d speed %.1f m/s at %.1f m\n", c.name, c.ctrl, ph::len(c.vel), ph::len(ph::sub(c.pose.p, rp)));
        return;
    }
    Car& c = g_cars[best];
    V3 side = ph::v3(-dir.z, 0.0f, dir.x);
    struct Cand { DWORD n; float d; } cand[32]; int nc = 0;
    DWORD scene = rd(A_SCENE), root = scene ? rd(scene + 0x44) : 0, vt, sib, child;
    static DWORD stack[16384]; int sp = 0, visited = 0;
    if (root) stack[sp++] = root;
    while (sp > 0 && visited < 400000) {
        DWORD nd = stack[--sp];
        if (!read_kids(nd, &vt, &sib, &child)) continue;
        ++visited;
        if (sib && sp < 16383) stack[sp++] = sib;
        if (child && sp < 16383) stack[sp++] = child;
        if (vt != 0x9CCE48 || node_off(nd) || cls_handler(nd) == H_VROOT) continue;
        const char* nm = model_name(nd);
        if (!ph::name_match(nm, g_drive_props) || ph::name_match(nm, "vehicle,replace,_static,lod,door")) continue;
        V3 p; if (!node_pos(nd, &p)) continue;
        float d = ph::len(ph::sub(p, rp)); if (d > 250.0f) continue;
        if (nc < 32) cand[nc++] = Cand{nd, d};
        else { int w = 0; for (int k = 1; k < 32; ++k) if (cand[k].d > cand[w].d) w = k; if (d < cand[w].d) cand[w] = Cand{nd, d}; }
    }
    for (int a = 0; a < nc; ++a) for (int b = a + 1; b < nc; ++b) if (cand[b].d < cand[a].d) { Cand t = cand[a]; cand[a] = cand[b]; cand[b] = t; }
    int np = nc < g_drive_max_props ? nc : g_drive_max_props;
    for (int k = 0; k < np; ++k) {   // pairs across the lane, 1.8 m apart along it, the first pair at the hit point
        V3 at = ph::add(ph::add(hit, ph::mul(dir, (float)(k / 2) * 1.8f)), ph::mul(side, (k & 1) ? 0.9f : -0.9f));
        float from[3] = {at.x, c.pose.p.y + 4.0f, at.z}, to[3] = {at.x, c.pose.p.y - 6.0f, at.z}, g[3];
        float ground = g_api->raycast_static(from, to, g) ? g[1] : c.pose.p.y - 1.0f;
        V3 p0; node_pos(cand[k].n, &p0);
        at.y = ground - collision_floor(cand[k].n, p0.y) + 0.03f;
        set_world_pos(cand[k].n, at);
        gp_log("car_physics: traffic rig: %s on the road at (%.2f, %.2f, %.2f) (ground %.2f)\n", model_name(cand[k].n), at.x, at.y, at.z, ground);
    }
    phys_request_scan();
    gp_log("car_physics: traffic rig: %s (%.1f m/s) meets %d props at (%.2f, %.2f, %.2f) in %.1f s\n", c.name, ph::len(c.vel), np,
           hit.x, hit.y, hit.z, g_traffic_lead);
    track(best, g_traffic_lead + 5.0);
    mark("cp_traffic", 90, 50);
}
static int g_enter_side = 1; static double g_enter_t = 0.0; static int g_enter_car = -1;
static void enter_place(V3 rp, int side) {
    Car& c = g_cars[g_enter_car];
    V3 f = view_dir(rp);
    float yaw = atan2f(f.x, f.z);
    Q4 q = {0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
    g_rig.q = q;
    V3 door = ph::v3(-(float)side * (c.box.half.x - g_c.margin + 0.45f), 0.0f, 0.45f);   // side 1: Ram at the left door
    g_rig.start = ph::sub(rp, ph::qrot(q, door));
    g_rig.start.y = rp.y + 0.15f;
    g_rig.car = g_enter_car; g_rig.drive = false; g_rig.phase = 1; g_rig.t0 = g_now;
    rig_hold(c);
    gp_log("car_physics: enter rig: %s parked beside Ram on side %d at (%.2f, %.2f, %.2f), nose along the view\n", c.name, side,
           g_rig.start.x, g_rig.start.y, g_rig.start.z);
}
static void enter_start(V3 rp) {
    int i = nearest_car(rp, true, 150.0f);
    if (i < 0) { gp_log("car_physics: enter rig: no live car\n"); return; }
    Car& c = g_cars[i];
    if (c.ctrl == 2) gp_send(c.ai, CMD_TRAFFIC_ACCIDENT, nullptr);
    gp_send(c.mc, CMD_ACTIVATE_NO_FIRST, nullptr);
    g_enter_car = i; g_enter_side = 1; g_enter_t = g_now;
    enter_place(rp, 1);
    V3 f = view_dir(rp), r = ph::v3(-f.z, 0.0f, f.x);
    rig_props(rp, ph::add(rp, ph::mul(f, g_drive_ahead)), r, f);   // a row along the view, in front of the car
    track(i, 14.0);
}
static void enter_tick(V3 rp) {   // 2.5 s after the first place: not in the car yet -> the other side (the driver door)
    if (g_enter_car < 0 || g_enter_car >= (int)g_cars.size()) { g_enter_car = -1; return; }
    Car& c = g_cars[g_enter_car];
    V3 local = ph::qrot(ph::qconj(c.pose.q), ph::sub(rp, c.pose.p));
    bool inside = fabsf(local.x) < c.box.half.x && fabsf(local.z) < c.box.half.z && fabsf(local.y - c.box.center.y) < c.box.half.y + 0.5f;
    if (c.ctrl == 1 || inside) {
        gp_log("car_physics: enter rig: Ram is in %s (control state %d, %.1f s after the placement)\n", c.name, c.ctrl, g_now - g_enter_t);
        mark("cp_enter", 1, 100); g_enter_car = -1; return;
    }
    if (g_now - g_enter_t > 10.0) { gp_log("car_physics: enter rig: Ram did not get in\n"); g_enter_car = -1; }
}
static void rig_start(V3 rp, bool drive) {
    int i = nearest_car(rp, true, 150.0f);
    if (i < 0) { gp_log("car_physics: rig: no live car within 150 m\n"); return; }
    Car& c = g_cars[i];
    V3 f = view_dir(rp), r = ph::v3(-f.z, 0.0f, f.x);
    g_rig.spot = ph::add(ph::add(rp, ph::mul(f, g_drive_ahead)), ph::mul(r, g_drive_offset));   // beside Ram in the view, not behind him
    V3 fwd = g_axis_neg ? ph::mul(r, -1.0f) : r;              // the car's +z along r: across the view, left to right
    float yaw = atan2f(fwd.x, fwd.z);
    Q4 q = {0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
    g_rig.q = q;
    g_rig.start = drive ? ph::sub(g_rig.spot, ph::mul(r, g_drive_side)) : g_rig.spot;
    g_rig.start.y = rp.y + 0.15f;
    g_rig.car = i; g_rig.drive = drive; g_rig.phase = 1; g_rig.t0 = g_now;
    log_car("rig before", c);
    if (c.ctrl == 2) gp_send(c.ai, CMD_TRAFFIC_ACCIDENT, nullptr);   // off its rail
    gp_send(c.mc, CMD_ACTIVATE_NO_FIRST, nullptr);
    rig_hold(c);
    gp_log("car_physics: rig: %s (control state %d) to (%.2f, %.2f, %.2f) yaw %.0f deg, %s\n", c.name, c.ctrl, g_rig.start.x,
           g_rig.start.y, g_rig.start.z, yaw * 57.29578f, drive ? "will drive across the view" : "parked broadside in view");
    if (drive) rig_props(rp, g_rig.spot, f, r);
    track(i, g_place_hold + (drive ? g_drive_delay + g_drive_seconds + 3.0 : 2.0));
}
static void rig_tick() {
    if (g_rig.phase == 0) return;
    if (g_rig.car < 0 || g_rig.car >= (int)g_cars.size()) { g_rig.phase = 0; return; }
    Car& c = g_cars[g_rig.car];
    double age = g_now - g_rig.t0;
    if (g_rig.phase == 1) {   // hold the pose until the game keeps it
        rig_hold(c);
        if (age >= g_place_hold) {
            log_car("rig placed", c);
            if (!g_rig.drive) { g_rig.phase = 0; mark("cp_placed", 1, 100); return; }
            g_rig.phase = 2; g_rig.go = g_now + g_drive_delay; g_rig.until = g_rig.go + g_drive_seconds;
        }
        return;
    }
    if (g_rig.phase == 2) {   // waiting for the props to settle, then the accelerator
        if (g_now < g_rig.go) return;
        V3 fw = ph::qrot(c.pose.q, ph::v3(0, 0, 1)); fw.y = 0.0f; fw = ph::norm(fw);
        game_kick(c, ph::mul(fw, g_drive_speed), ph::v3(0, 0, 0), "rig launch");   // 377, tumbling, frictions, speed
        gp_log("car_physics: rig: %s launched at %.1f m/s, accelerator held for %.1f s\n", c.name, g_drive_speed, g_drive_seconds);
        mark("cp_drive", 70, 70);
        g_rig.phase = 3;
    }
    DWORD md = data_of(c.mc);
    if (g_now > g_rig.until) {
        dirty_write(c.mc, I_KEY_ACCEL, M_KEY_ACCEL, 0); dirty_write(c.mc, I_ACCEL_NUM, M_ACCEL_NUM, 0);
        gp_log("car_physics: rig: accelerator released\n"); log_car("drive end", c);
        g_rig.phase = 0; return;
    }
    if (md && rdf(md + M_ENGINE) <= 0.0f) dirty_write(c.mc, I_ENGINE, M_ENGINE, rd(md + M_MAX_ENGINE));
    // the crashed traffic car's AI holds the brake while its driver gets out (run c3: the accelerator alone did not move
    // it): brake / decelerate / handbrake keys off, accelerator on, and for the first moments the car is launched at
    // debug_drive_speed (both axle points and the RigidBody), as if it came down the street at that speed
    dirty_write(c.mc, I_KEY_BRAKE, M_KEY_BRAKE, 0);
    dirty_write(c.mc, I_KEY_DECEL, M_KEY_DECEL, 0);
    dirty_write(c.mc, I_KEY_SLIDE, M_KEY_SLIDE, 0);
    dirty_write(c.mc, I_KEY_ACCEL, M_KEY_ACCEL, 1);
    dirty_write(c.mc, I_ACCEL_NUM, M_ACCEL_NUM, fbits(1.0f));
}

void car_post_input(const BYTE* kb_edges, const BYTE* kb_state, double now) {
    (void)kb_state; (void)now;
    if (!g_installed || !kb_edges) return;
    auto pressed = [&](int k) { return k >= 0 && k < 256 && (kb_edges[k] & 1); };
    if (pressed(g_kick_key)) g_kick_req = true;
    if (pressed(g_drive_key)) g_drive_req = true;
    if (pressed(g_list_key)) g_list_req = true;
    if (pressed(g_wreck_key)) g_wreck_req = true;
    if (pressed(g_place_key)) g_place_req = true;
    if (pressed(g_traffic_key)) g_traffic_req = true;
    if (pressed(g_enter_key)) g_enter_req = true;
}

void car_log_stats() {
    if (!g_installed) return;
    int act = 0, kin = 0, wr = 0;
    for (auto& c : g_cars) { if (c.active) ++act; if (c.kb >= 0) ++kin; if (c.wb >= 0) ++wr; }
    gp_log("car_physics: stats %d cars known (%d near Ram, %d kinematic boxes, %d wrecks) | car frame %.3f ms avg %.3f max | walk %.2f ms avg "
           "%.2f max (%d walks) | touches %d, prop kicks %d, blasts %d, cars thrown %d, wrecks %d, bullet hits on wrecks %d\n",
           (int)g_cars.size(), act, kin, wr, g_st_frame.avg(), g_st_frame.mx, g_st_walk.avg(), g_st_walk.mx, g_st_walk.n, g_stat_touches,
           g_stat_kicks, g_stat_blasts, g_stat_cars_hit, g_stat_wrecks, g_stat_bullets);
    g_st_frame.reset(); g_st_walk.reset();
}

// ================================================================================================ install
bool car_install(const PhApi* api, HMODULE dll) {
    if (!g_c.enabled) return false;
    for (const char* r : CP_REFS) if (!gp_ref_ok(r)) { gp_log("car_physics: reference %s differs: OFF\n", r); return false; }
    TODPhysics_GetCarApi_t get = dll ? (TODPhysics_GetCarApi_t)GetProcAddress(dll, "TODPhysics_GetCarApi") : nullptr;
    g_capi = get ? get(TODPHYS_CAR_API_VERSION) : nullptr;
    g_api = api;
    if (!g_capi || !g_api) { gp_log("car_physics: TODPhysics.dll has no car interface (old DLL?): OFF\n"); return false; }
    g_installed = true;
    gp_log("car_physics: on: push_props %d, blast_cars %d, wreck_dynamic %d, bullets %d, impulse_scale %.2f (blast gain %.1f, spin %.1f, "
           "push gain %.2f lift %.1f, wreck %.0f kg)\n", g_c.push_props, g_c.blast_cars, g_c.wreck_dynamic, g_c.bullets, g_c.impulse_scale,
           g_c.blast_gain, g_c.blast_spin, g_c.push_gain, g_c.push_lift, g_c.wreck_mass);
    return true;
}
