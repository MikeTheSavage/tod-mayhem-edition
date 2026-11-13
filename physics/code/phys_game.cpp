// phys_game.cpp - the [physics] prototype of TODGameplay.dll: loose props (barrels, crates, bottles ...)
// become Box3D rigid bodies inside the original game. Off unless [physics] enabled = 1; when off nothing here touches
// the game and TODPhysics.dll is not even loaded.
//
// How it works (docs: physics/README.md; sites and REF rows: dll/src/sites.h section "Physics"; dll/HOOKS.md):
//   * Scan (Ram in control, then again when he walks rescan_distance away while nothing moves): the scene tree
//     (Scene [0xA3DCBC], first child +44h, next sibling +34h) is walked once. Every Model node (vtable 0x9CCE48) within
//     `radius` of Ram gives its collision to Box3D: the model's HullList (asset +54h, model space) and each pivot's
//     CollisionList (Model::GetPivotCollision 0x884860, pivot space, the pivot node from Model +A0h); CollisionBox
//     entities (vtable 0x9D02F8) give their box. Only Solid shapes (list flag byte +10h bit 0 or the shape's surface
//     word bit 10). VertexClouds become convex hulls; flat patches (terrain, walls) become slabs (phys_logic).
//     Models whose asset name matches `props` (and not `exclude`), small, with collision on pivot 0 only, become
//     dynamic bodies (sleeping until something touches them) instead of static geometry.
//   * Each frame (input tick, once per main-loop pass): the game's dt [0xA3DCC4] (slow motion included, 0 when paused)
//     feeds a fixed 0.04 s step (the game's own character / car step); awake bodies are interpolated between the last
//     two steps and written back with the engine's own setters (SetPos 0x88D6C0 / SetOrient 0x88DB20, local to the
//     parent), which also move the node's collision and its quadtree entry, so the game draws them where Box3D is.
//   * Bullets: the bullet step's line test (call 0x8B7000 at 0x8C95A4) is wrapped; a segment that hits a body (and the
//     game's own hit node is that prop, or the game saw no hit) pushes it at the hit point.
//   * Explosions: weapon_shell command_explode's blast (call 0x8460B0 at 0x848BD2: grenades, rockets, dynamite) and
//     Destroyable_Object's explosion (call 0x668BA0 at 0x67145B: red barrels, gas pumps ...) -> Box3D explosion at the
//     node's position plus an upward kick.
//   * Rewind (PlayMode 3): no steps, no writes; afterwards every body is put back where its node now is.
//   * In a vehicle: the game runs no on-foot input tick while Ram drives, so the per-pass POST_INPUT hook steps
//     the world instead (phys_post_input -> drive_frame), paced by the game-ms clock [0xA3DCD4] (REF PS_GAMEMS: still while
//     paused or loading, back on a rewind) and never in a cut-scene. "In a vehicle" = Ram's character_root data +58h
//     (player_ai, class 0x5ACE10) data +24h holds the vehicle (written by command_drive_vehicle 0x5B5B10, REF
//     PH_PAI_VEHICLE) and that vehicle's vehicle_ai ControlState is 1 (the player's). Scans there lead the car.
//   * Liveness: a body's node is checked every frame in the entity registry (AssetManager [0xA3D7C4] vectors +130h;
//     ~Entity clears the slot), its vtable, model asset and the disabled bit; a dead node drops the body.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "../../dll/src/gp_glue.h"
#include "phys_logic.h"
#include "phys_api.h"
#include "car_glue.h"   // [car_physics]: the game's cars in this world (car_game.cpp)

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define RDB(a)  (*(volatile BYTE*)(uintptr_t)(a))

// ================================================================================================ game addresses
// each proven by a sites.h REF row (checked in the exe by check_sites.py and in memory before install)
static const DWORD A_SCENE = 0xA3DCBC, A_ASSETMGR = 0xA3D7C4, A_GAME_DT = 0xA3DCC4, A_PLAYER_HOLDER = 0xA3B590,
                   A_CUR_NODE = 0xA3B75C, A_GAME_MS = 0xA3DCD4;
// the vehicle frame drive: class handlers (class +60h) and members, REF SV_CLS_PLAYERAI, CP_CLS_VROOT, CP_CLS_VAI,
// CP_CLS_CAR, SM_ROOT_AI, CP_VROOT_AI, PH_PAI_VEHICLE, PS_GAMEMS
static const DWORD H_PLAYER_AI = 0x5ACE10, H_VROOT = 0x824A70, H_VAI = 0x803A50;
static const DWORD ROOT_AI = 0x58, PAI_VEHICLE = 0x24, VR_AICTRL = 0x4C, VA_CTRL = 0x18, N_PARENT = 0x40;
static const char* PH_DRIVE_REFS[] = {"PS_GAMEMS", "SV_CLS_PLAYERAI", "CP_CLS_VROOT", "CP_CLS_VAI", "SM_ROOT_AI",
    "CP_VROOT_AI", "PH_PAI_VEHICLE", "G_CUTSCENE"};
static const DWORD VT_MODEL = 0x9CCE48, VT_CBOX = 0x9D02F8, VT_VCLOUD = 0x9D05A0, VT_OBOX = 0x9D02CC;
static DWORD P_PH_WORLDMAT = 0x4842C0, P_PH_SETPOS = 0x88D6C0, P_PH_SETORIENT = 0x88DB20, P_PH_PIVCOLL = 0x884860,
             P_PH_MODELNAME = 0x884350, P_PH_GETPOS = 0x88CA80, P_PH_SETWPOS = 0x88E600;
// original call targets of the three CALL sites (check_sites.py compares them with the shipped calls)
static DWORD R_PH_LINE = 0x8B7000, R_PH_SHELL = 0x8460B0, R_PH_DESTROY = 0x668BA0;

static const char* PH_DEBUG_REFS[] = {"F_SETPOS", "F_SENDMSG", "SV_CLS_SHELL", "PH_SHELLS", "SV_QUERY_FREE", "SV_SHAKE"};
static const char* PH_REFS[] = {"PH_F_WORLDMAT", "PH_F_SETPOS", "PH_F_SETORIENT", "PH_SETROTY_Q", "PH_F_PIVCOLL",
    "PH_F_MODELNAME", "PH_VT_MODEL", "PH_VT_CBOX", "PH_CBOX_FIELDS", "PH_VT_VCLOUD", "PH_VT_OBOX", "PH_VC_VERTS",
    "PH_VC_COUNT", "PH_OBOX_QUAT", "PH_HULLLIST", "PH_HULL_ELEM", "PH_PIVNODE", "PH_LIST_FLAGS", "PH_LIST_COUNT",
    "PH_LIST_DATA", "PH_SHAPE_SURF", "PH_NODE_OFF", "PH_NODE_PARENT", "PH_TREE_CHILD", "PH_TREE_SIBLING",
    "PH_GAME_DT", "PH_REGISTRY", "PH_REG_VEC", "SL_PLAYER", "F_GETPOS"};

// ================================================================================================ config
static ph::Cfg g_c;
static char g_props[512] = "barrel,crate,box,bottle,can_,cans,bucket,trash,/bins/,rubbish,garbage,chair,stool,bench,cone_traffic,trafficcone,tire,tyre,keg,jar,pot_,vase,basket,sack,cooler,plank,pallet,melon,fruit,cardboard,carton,jerrycan,canister,paint,dustbin,suitcase,cup,glass,plate,bowl,pumpkin,tin_";
static char g_exclude[512] = "building,house,terrain,chunk,road,bridge,_wall,fence,door,gate,window,vehicle,car_,truck,boat,train,station,temple,roof,stair,ladder,lod,_shadow,skybox,sky_,light,lamp,glow,hydrant,telepole,sign,breakableglass";
static int  g_gather_key = gp::KEY_NONE; static bool g_gather_req = false;   // test copy: gather on a key (the title level already ticks)
static int  g_kick_key = gp::KEY_NONE, g_fire_key = gp::KEY_NONE, g_dump_key = gp::KEY_NONE, g_boom_key = gp::KEY_NONE;
static bool g_vanilla = false;          // test copy: debug keys / placement only, no simulation (the "before" runs)
static float g_place_delay = 2.0f;      // test rig: seconds in control before the props are gathered
static char g_gather[256] = ""; static float g_gather_dist = 7.0f, g_gather_drop = 0.8f, g_gather_radius = 220.0f;
static int g_gather_max = 8; static bool g_gathered = false, g_force_scan = false; static float g_gather_side = 0.0f;
static DWORD g_gather_nodes[32]; static int g_ngather = 0;
static bool gathered(DWORD m) { for (int i = 0; i < g_ngather; ++i) if (g_gather_nodes[i] == m) return true; return false; }
static float g_boom_back = 1.0f; static int g_shell_type = 1;
static bool g_debug_shots = false, g_scan_dump = false, g_hooks_bullet = true, g_hooks_blast = true;
static float g_kick_impulse = 700.0f, g_kick_back = 1.2f;
static int  g_max_static = 12000;
static float g_dump_radius = 120.0f;
static bool g_probe_on = false; static float g_probe_x = 0, g_probe_z = 0;   // test copy: debug_probe = x,z
static bool g_dump_static = false;   // test copy: every static hull's points into the log (offline Box3D replay)

static bool g_world_only = false;   // [ragdoll] on, [physics] off: the level world for the ragdolls, no props
void phys_load_config() {
    const gp::Ini& I = gp_ini();
    ragdoll_load_config();   // [ragdoll] needs this module's world (ragdoll/code/rd_game.cpp)
    ph::Cfg c;
    c.enabled = I.geti("physics", "enabled", 0) != 0;
    c.radius = I.getf("physics", "radius", c.radius);
    c.rescan_distance = I.getf("physics", "rescan_distance", c.rescan_distance);
    c.max_bodies = I.geti("physics", "max_bodies", c.max_bodies);
    c.step = I.getf("physics", "step", c.step);
    c.substeps = I.geti("physics", "substeps", c.substeps);
    c.gravity = I.getf("physics", "gravity", c.gravity);
    c.density = I.getf("physics", "density", c.density);
    c.min_mass = I.getf("physics", "min_mass", c.min_mass); c.max_mass = I.getf("physics", "max_mass", c.max_mass);
    c.friction = I.getf("physics", "friction", c.friction); c.restitution = I.getf("physics", "restitution", c.restitution);
    c.linear_damping = I.getf("physics", "linear_damping", c.linear_damping);
    c.angular_damping = I.getf("physics", "angular_damping", c.angular_damping);
    c.bullet_impulse = I.getf("physics", "bullet_impulse", c.bullet_impulse);
    c.shell_radius = I.getf("physics", "shell_radius", c.shell_radius); c.shell_impulse = I.getf("physics", "shell_impulse", c.shell_impulse);
    c.destroy_radius = I.getf("physics", "destroy_radius", c.destroy_radius);
    c.destroy_impulse = I.getf("physics", "destroy_impulse", c.destroy_impulse);
    c.blast_lift = I.getf("physics", "blast_lift", c.blast_lift);
    c.slab = I.getf("physics", "slab", c.slab); c.min_thickness = I.getf("physics", "min_thickness", c.min_thickness);
    c.interpolate = I.geti("physics", "interpolate", 1) != 0;
    c.start_asleep = I.geti("physics", "start_asleep", 1) != 0;
    c.log_seconds = I.getf("physics", "log_seconds", c.log_seconds);
    c.drive_step = I.geti("physics", "drive_step", 1) != 0;
    c.drive_lead = I.getf("physics", "drive_lead_seconds", c.drive_lead);
    c.clamp();
    g_c = c;
    g_world_only = !c.enabled && ragdoll_enabled();
    if (const char* v = I.get("physics", "props")) { strncpy(g_props, v, sizeof g_props - 1); g_props[sizeof g_props - 1] = 0; }
    if (const char* ex = car_extra_props()) {   // [car_physics]: street stands and carts the cars plough through
        if (*ex && strlen(g_props) + strlen(ex) + 2 < sizeof g_props) { strcat(g_props, ","); strcat(g_props, ex); }
    }
    if (const char* v = I.get("physics", "exclude")) { strncpy(g_exclude, v, sizeof g_exclude - 1); g_exclude[sizeof g_exclude - 1] = 0; }
    g_hooks_bullet = I.geti("physics", "bullets", 1) != 0;
    g_hooks_blast = I.geti("physics", "explosions", 1) != 0;
    g_max_static = I.geti("physics", "max_static_shapes", 12000); if (g_max_static < 100) g_max_static = 100; if (g_max_static > 60000) g_max_static = 60000;
    // test copy only
    g_kick_key = gp_key_cfg("physics", "debug_kick_key", "NONE");
    g_fire_key = gp_key_cfg("physics", "debug_fire_key", "NONE");
    g_dump_key = gp_key_cfg("physics", "debug_dump_key", "NONE");
    g_kick_impulse = I.getf("physics", "debug_kick_impulse", 700.0f);
    g_kick_back = I.getf("physics", "debug_kick_back", 1.2f);
    g_debug_shots = I.geti("physics", "debug_shots", 0) != 0;
    g_scan_dump = I.geti("physics", "debug_scan_dump", 0) != 0;
    g_dump_radius = I.getf("physics", "debug_dump_radius", 120.0f);
    g_boom_key = gp_key_cfg("physics", "debug_boom_key", "NONE");
    g_boom_back = I.getf("physics", "debug_boom_back", 1.0f);
    g_shell_type = I.geti("physics", "debug_shell_type", 1);
    g_vanilla = I.geti("physics", "debug_vanilla", 0) != 0;
    if (const char* v = I.get("physics", "debug_gather")) { strncpy(g_gather, v, sizeof g_gather - 1); g_gather[sizeof g_gather - 1] = 0; }
    g_gather_dist = I.getf("physics", "debug_gather_dist", 7.0f);
    g_gather_drop = I.getf("physics", "debug_gather_drop", 0.8f);
    g_gather_radius = I.getf("physics", "debug_gather_radius", 220.0f);
    g_gather_max = I.geti("physics", "debug_gather_max", 8);
    g_gather_side = I.getf("physics", "debug_gather_side", 0.0f);
    g_dump_static = I.geti("physics", "debug_dump_static", 0) != 0;
    if (const char* v = I.get("physics", "debug_probe")) { if (sscanf(v, "%f , %f", &g_probe_x, &g_probe_z) == 2) g_probe_on = true; }
    g_place_delay = I.getf("physics", "debug_gather_delay", 2.0f);
    g_gather_key = gp_key_cfg("physics", "debug_gather_key", "NONE");
    car_load_config();
}
bool phys_enabled() { return g_c.enabled || g_world_only; }

// ================================================================================================ engine calls
typedef void(__fastcall* Mat_t)(DWORD node, void* edx, float* out16);            // ret 4
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, const float* v);          // ret 4
typedef DWORD(__fastcall* PivColl_t)(DWORD model, void* edx, int pivot);         // ret 4
typedef const char*(__fastcall* Name_t)(DWORD model, void* edx);                 // ret 0
typedef unsigned(__fastcall* Line_t)(DWORD probe, void* edx, int a, float* from, float* to);   // ret 0Ch
typedef DWORD(__cdecl* Script2_t)(DWORD thread, DWORD arg);

static bool world_matrix(DWORD node, float m[16]) {
    if (!node) return false;
    __try { ((Mat_t)(uintptr_t)P_PH_WORLDMAT)(node, nullptr, m); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return m[12] == m[12] && m[13] == m[13] && m[14] == m[14];
}
static bool node_pos(DWORD node, ph::V3* p) {
    float v[3];
    __try { ((Vec_t)(uintptr_t)P_PH_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    *p = ph::v3(v[0], v[1], v[2]); return true;
}
static DWORD vtable(DWORD node) { __try { return node ? RD32(node) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static bool node_off(DWORD node) {   // the collision probe's test: (flags | flags >> 16) & 1 (REF PH_NODE_OFF)
    __try { DWORD f = RD32(node + 0x28); return ((f | (f >> 16)) & 1) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
static const char* model_name(DWORD model) {
    __try { const char* s = ((Name_t)(uintptr_t)P_PH_MODELNAME)(model, nullptr); if (s && !IsBadStringPtrA(s, 256)) return s; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "?";
}
static DWORD pivot_collision(DWORD model, int i) {
    __try { return ((PivColl_t)(uintptr_t)P_PH_PIVCOLL)(model, nullptr, i); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD pivot_node(DWORD model, int i) {   // REF PH_PIVNODE: Model +64h high half 0 -> Model +A0h [i*8], else the model
    __try {
        if (i == 0) return model;
        if (RD32(model + 0x64) & 0xFFFF0000) return model;
        DWORD a = RD32(model + 0xA0); if (!a) return 0;
        if (i >= (int)RD32(model + 0xA4)) return 0;
        return RD32(a + (DWORD)i * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
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
static DWORD ram_root() {   // [0xA3B590] ->+18h ->+84h (REF SL_PLAYER)
    __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static float game_dt() { __try { float d = RDF(A_GAME_DT); return d == d ? d : 0.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; } }

// ================================================================================================ state
struct PBody {
    DWORD node, parent, asset; int b; char name[72];
    ph::V3 prev_p, cur_p, start_p; ph::Q4 prev_q, cur_q, start_q;
    bool active, sleeping_written;
};
static const PhApi* g_api = nullptr;
static HMODULE g_mod = nullptr;
static bool g_installed = false, g_world = false, g_rewinding = false, g_pending_resync = false;
static std::vector<PBody> g_bodies;
static ph::V3 g_center = {0, 0, 0};
static ph::StepClock g_clock;
static ph::Stats g_st_step, g_st_write, g_st_frame, g_st_scan;
static double g_last_log = 0.0, g_last_scan_t = -100.0;
static volatile LONG g_frame = 0; static LONG g_tick_frame = -1;
static int g_drive_spans = 0, g_drive_frames_total = 0;   // vehicle drives so far (drive_frame below)
static int g_hits = 0, g_hit_misses = 0, g_blasts = 0, g_writes = 0, g_steps = 0, g_dropped = 0, g_scans = 0;
static int g_static_hulls = 0, g_static_boxes = 0, g_static_skipped = 0, g_peak_awake = 0;
static bool g_kick_req = false, g_dump_req = false, g_boom_req = false, g_fire_down = false, g_fire_prev = false;
static double g_last_mark = 0.0;

static void mark(const char* what, int count, int ms) {   // test copy: the harness grabs screenshots on these lines
    if (!g_debug_shots) return;
    double now = gp_now();
    if (now - g_last_mark < 0.5) return;
    g_last_mark = now;
    gp_log("debug keys: mark burst %s %d %d\n", what, count, ms);
}

// ================================================================================================ scan
// (no C++ objects in functions with __try: every guarded read is its own small function)
struct Scan {
    ph::V3 c; float r;
    int models, cboxes, props, prop_nocoll, prop_multi, static_shapes, failed, far_models, visited;
};
static float d2xz(ph::V3 a, ph::V3 b) { float dx = a.x - b.x, dz = a.z - b.z; return dx * dx + dz * dz; }
enum { MAXPTS = 256 };

// one collision shape -> points (VertexCloud vertices or OrientedBox corners), through M when given; 0 = unknown
static int shape_points(DWORD s, const float* M, ph::V3 sc, ph::V3* out, DWORD* surf) {
    __try {
        DWORD vt = RD32(s); *surf = RD32(s + 0x24);
        if (vt == VT_VCLOUD) {   // count = byte +2Dh, vertices = +30h & ~3 (support 0x8B0150)
            int n = RDB(s + 0x2D);
            const float* v = (const float*)(uintptr_t)(RD32(s + 0x30) & ~3u);
            if (n < 3 || !v) return 0;
            for (int i = 0; i < n; ++i) {
                ph::V3 l = ph::v3(v[3 * i] * sc.x, v[3 * i + 1] * sc.y, v[3 * i + 2] * sc.z);
                out[i] = M ? ph::tod_point(M, l) : l;
            }
            return n;
        }
        if (vt == VT_OBOX) {   // full size +28h, centre +34h, rotation +40h (support 0x8ABB10 reads +40h..+4Ch)
            ph::V3 h = ph::v3(RDF(s + 0x28) * 0.5f, RDF(s + 0x2C) * 0.5f, RDF(s + 0x30) * 0.5f);
            ph::V3 c = ph::v3(RDF(s + 0x34), RDF(s + 0x38), RDF(s + 0x3C));
            ph::Q4 q = {RDF(s + 0x40), RDF(s + 0x44), RDF(s + 0x48), RDF(s + 0x4C)};
            q = ph::true_from_stored(q);   // the game's stored convention (identity on every CollisionBox)
            if (!(h.x > 0 && h.y > 0 && h.z > 0) || h.x > 500 || h.y > 500 || h.z > 500) return 0;
            for (int i = 0; i < 8; ++i) {
                ph::V3 l = ph::add(c, ph::qrot(q, ph::v3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z)));
                l = ph::v3(l.x * sc.x, l.y * sc.y, l.z * sc.z);
                out[i] = M ? ph::tod_point(M, l) : l;
            }
            return 8;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static bool solid(DWORD list_flags, DWORD surf) { return (list_flags & 1) || ((surf >> 10) & 1); }
// CollisionList {data +0, count +4, flag bytes +10h}; data = shape pointers with 2 tag bits
static bool list_info(DWORD list, DWORD* data, DWORD* n, DWORD* flags) {
    __try { *data = RD32(list); *n = RD32(list + 4); *flags = RDB(list + 0x10); return *n <= 4096; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DWORD arr_ptr(DWORD data, DWORD i) { __try { return RD32(data + i * 4) & ~3u; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD deref(DWORD p) { __try { return p ? RD32(p) & ~3u : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static bool hulllist_info(DWORD asset, DWORD* data, DWORD* n) {   // HullList asset +54h & ~3: {data, count}; element +0 = shape
    __try { DWORD hl = RD32(asset + 0x54) & ~3u; if (!hl) return false; *data = RD32(hl); *n = RD32(hl + 4); return *n <= 200000; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static ph::V3 g_pts[MAXPTS], g_slab[2 * MAXPTS];

static void probe_shape(const char* who, DWORD s, const ph::V3* w, int n, DWORD flags, DWORD surf, bool is_solid) {
    if (!g_probe_on || n < 1) return;
    float x0 = w[0].x, x1 = w[0].x, z0 = w[0].z, z1 = w[0].z, y0 = w[0].y, y1 = w[0].y;
    for (int i = 1; i < n; ++i) { x0 = min(x0, w[i].x); x1 = max(x1, w[i].x); z0 = min(z0, w[i].z); z1 = max(z1, w[i].z); y0 = min(y0, w[i].y); y1 = max(y1, w[i].y); }
    if (g_probe_x < x0 - 0.3f || g_probe_x > x1 + 0.3f || g_probe_z < z0 - 0.3f || g_probe_z > z1 + 0.3f) return;
    ph::V3 nrm; float th = ph::thickness(w, n, &nrm);
    DWORD vt = 0; __try { vt = RD32(s); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    gp_log("physics:   probe %s shape %08X vt %06X n %d x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f list %02X surf %08X solid %d thick %.3f normal (%.2f %.2f %.2f)\n",
           who, s, vt, n, x0, x1, y0, y1, z0, z1, flags, surf, is_solid ? 1 : 0, th, nrm.x, nrm.y, nrm.z);
    char buf[2048]; int k = 0;   // the points, for an offline Box3D repro
    for (int i = 0; i < n && k < (int)sizeof buf - 64; ++i) k += _snprintf(buf + k, sizeof buf - k, " %.3f,%.3f,%.3f", w[i].x, w[i].y, w[i].z);
    buf[sizeof buf - 1] = 0;
    gp_log("physics:   probe points%s\n", buf);
}
static void add_static_points(Scan& S, const ph::V3* w, int n) {
    if (S.static_shapes >= g_max_static) { ++g_static_skipped; return; }
    ph::V3 lo = w[0], hi = w[0];   // near the scan centre only (the points' bounds against the circle)
    for (int i = 1; i < n; ++i) {
        lo.x = min(lo.x, w[i].x); lo.y = min(lo.y, w[i].y); lo.z = min(lo.z, w[i].z);
        hi.x = max(hi.x, w[i].x); hi.y = max(hi.y, w[i].y); hi.z = max(hi.z, w[i].z);
    }
    float cx = max(lo.x, min(S.c.x, hi.x)), cz = max(lo.z, min(S.c.z, hi.z));
    float dx = cx - S.c.x, dz = cz - S.c.z;
    if (dx * dx + dz * dz > S.r * S.r) return;
    if (hi.y < S.c.y - 60.0f || lo.y > S.c.y + 80.0f) return;
    int k = ph::prepare_hull(w, n, g_c.min_thickness, g_c.slab, g_slab);
    if (k < 4) { ++S.failed; return; }
    if (g_api->add_static_hull(&g_slab[0].x, k, g_c.friction)) ++S.static_shapes; else ++S.failed;
    if (g_dump_static) {   // chunks of 30 points (gp_log lines are at most 1 KB): "SH id count first x,y,z ..."
        static int sh_id = 0; ++sh_id;
        for (int i0 = 0; i0 < k; i0 += 20) {
            char buf[1000]; int m = 0;
            for (int i = i0; i < k && i < i0 + 20; ++i) m += _snprintf(buf + m, sizeof buf - m, " %.9g,%.9g,%.9g", g_slab[i].x, g_slab[i].y, g_slab[i].z);
            buf[sizeof buf - 1] = 0;
            gp_log("physics:   SH %d %d %d%s\n", sh_id, k, i0, buf);
        }
    }
}
static void add_list_static(Scan& S, DWORD list, DWORD pnode, const char* who) {
    DWORD data, n, flags;
    if (!list || !pnode || node_off(pnode) || !list_info(list, &data, &n, &flags)) return;
    float M[16]; if (!world_matrix(pnode, M)) return;
    ph::V3 one = ph::v3(1, 1, 1);
    for (DWORD i = 0; i < n; ++i) {
        DWORD s = arr_ptr(data, i), surf = 0;
        int k = s ? shape_points(s, M, one, g_pts, &surf) : 0;
        if (k >= 3) probe_shape(who, s, g_pts, k, flags, surf, solid(flags, surf));
        if (k >= 3 && solid(flags, surf)) add_static_points(S, g_pts, k);
    }
}
static void add_hulllist_static(Scan& S, DWORD asset, const float M[16], const char* who) {
    DWORD data, n; if (!hulllist_info(asset, &data, &n)) return;
    ph::V3 one = ph::v3(1, 1, 1);
    for (DWORD i = 0; i < n; ++i) {
        DWORD s = deref(arr_ptr(data, i)), surf = 0;
        int k = s ? shape_points(s, M, one, g_pts, &surf) : 0;
        if (k >= 3) probe_shape(who, s, g_pts, k, 0, surf, solid(0, surf));
        if (k >= 3 && solid(0, surf)) add_static_points(S, g_pts, k);
    }
}
static int asset_pivots(DWORD asset) { __try { return (int)RD32(asset + 0x34); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
static DWORD node_asset(DWORD m) { __try { return RD32(m + 0x50); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD node_parent(DWORD n) { __try { return RD32(n + 0x40); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

static const char* node_name(DWORD n) {   // Node +4Ch
    __try { const char* s = n ? (const char*)(uintptr_t)RD32(n + 0x4C) : nullptr; if (s && !IsBadStringPtrA(s, 64)) return s; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "";
}
// a prop: the whole model is one rigid body (its pivots never move apart); returns the body index or -1
static int make_prop(Scan& S, DWORD model, DWORD asset, const char* name, const float M[16]) {
    ph::Rigid r = ph::rigid_from_tod(M);
    if (!r.ok) return -1;
    int npiv = asset_pivots(asset);
    if (npiv < 1 || npiv > 64) return -1;
    // every pivot's collision (most props keep it on a child pivot, e.g. CrateSmall / bins / chairs: 4 pivots, none on
    // pivot 0), taken through the pivot node's world matrix into the model's rigid frame: the whole model is one body
    // Havoc props name their collision pivot "CM" and carry the broken version's collision on "Replace_Model": with a
    // CM pivot only CM counts, else every pivot but Replace_*; pivots switched off (node flags) never count
    bool has_cm = false;
    for (int pi = 0; pi < npiv; ++pi) if (pivot_collision(model, pi) && !_stricmp(node_name(pivot_node(model, pi)), "CM")) has_cm = true;
    std::vector<std::vector<ph::V3>> hulls;
    float rad = 0.0f; int lists = 0;
    for (int pi = 0; pi < npiv; ++pi) {
        DWORD list = pivot_collision(model, pi), data, n, flags;
        if (!list || !list_info(list, &data, &n, &flags) || n == 0) continue;
        DWORD pn = pivot_node(model, pi); float PW[16];
        if (!pn || (pi > 0 && node_off(pn))) continue;
        const char* pname = node_name(pn);
        if (has_cm ? _stricmp(pname, "CM") != 0 : !_strnicmp(pname, "Replace", 7)) continue;
        if (!world_matrix(pn, PW)) continue;
        ++lists;
        for (DWORD i = 0; i < n && hulls.size() < 48; ++i) {
            DWORD s = arr_ptr(data, i), surf = 0;
            int k = s ? shape_points(s, PW, ph::v3(1, 1, 1), g_pts, &surf) : 0;
            if (k < 3) continue;
            for (int j = 0; j < k; ++j) {
                g_pts[j] = ph::qrot(ph::qconj(r.q), ph::sub(g_pts[j], r.p));   // world -> the body (model) frame
                float d = ph::len(g_pts[j]); if (d > rad) rad = d;
            }
            hulls.push_back(std::vector<ph::V3>(g_pts, g_pts + k));
        }
    }
    if (lists > 1) ++S.prop_multi;
    if (hulls.empty()) { ++S.prop_nocoll; return -1; }
    ph::PropRule rule;
    rule.max_radius = car_prop_max_radius(name, rule.max_radius);   // [car_physics] extra_props: stands, carts
    if (!ph::prop_size_ok(rule, rad)) return -1;
    PhBodyDesc d; memset(&d, 0, sizeof d);
    d.pos[0] = r.p.x; d.pos[1] = r.p.y; d.pos[2] = r.p.z; d.rot[0] = r.q.x; d.rot[1] = r.q.y; d.rot[2] = r.q.z; d.rot[3] = r.q.w;
    d.lin_damping = g_c.linear_damping; d.ang_damping = g_c.angular_damping; d.asleep = (g_c.start_asleep && !gathered(model)) ? 1 : 0;
    d.user = (void*)(uintptr_t)model;
    int b = g_api->body_create(&d);
    if (b < 0) return -1;
    for (auto& h : hulls) {
        int k = ph::prepare_hull(h.data(), (int)h.size(), g_c.min_thickness, 0.04f, g_slab);
        if (k >= 4) g_api->body_add_hull(b, &g_slab[0].x, k, g_c.density, g_c.friction, g_c.restitution);
    }
    if (!g_api->body_finish(b, g_c.min_mass, g_c.max_mass)) return -1;
    if (g_probe_on && !hulls.empty()) {
        char buf[2048]; int k = 0;
        for (auto& v : hulls[0]) if (k < (int)sizeof buf - 64) k += _snprintf(buf + k, sizeof buf - k, " %.3f,%.3f,%.3f", v.x, v.y, v.z);
        buf[sizeof buf - 1] = 0;
        gp_log("physics:   prop #%d hull0 rot (%.4f %.4f %.4f %.4f) points%s\n", b, r.q.x, r.q.y, r.q.z, r.q.w, buf);
    }
    if (g_probe_on || gathered(model)) {   // what is under the new body (level geometry only)
        float f[3] = {r.p.x, r.p.y + 0.5f, r.p.z}, t[3] = {r.p.x, r.p.y - 20.0f, r.p.z}, h[3] = {0, 0, 0};
        int hit = g_api->raycast_static(f, t, h);
        gp_log("physics:   prop #%d ground ray: %s (%.2f, %.2f, %.2f)\n", b, hit ? "level geometry at" : "NOTHING below, ray end", hit ? h[0] : t[0], hit ? h[1] : t[1], hit ? h[2] : t[2]);
    }
    PBody pb; memset(&pb, 0, sizeof pb);
    pb.node = model; pb.asset = asset; pb.b = b; pb.parent = node_parent(model);
    { const char* sl = name ? strrchr(name, '/') : nullptr; strncpy(pb.name, sl ? sl + 1 : (name ? name : "?"), sizeof pb.name - 1); }
    pb.prev_p = pb.cur_p = pb.start_p = r.p; pb.prev_q = pb.cur_q = pb.start_q = r.q;
    g_bodies.push_back(pb);
    ++S.props;
    if (g_scan_dump || S.props <= 40)
        gp_log("physics:   prop #%d %s at (%.2f, %.2f, %.2f), %d hull(s), radius %.2f m, %.1f kg\n", b, name, r.p.x, r.p.y, r.p.z,
               (int)hulls.size(), rad, g_api->body_mass(b));
    return b;
}
static void pivot_desc(DWORD model, int npiv, char* out, size_t cap) {   // "i:name:shapes/flags" of every pivot with collision
    out[0] = 0; size_t k = 0;
    for (int i = 0; i < npiv && i < 64 && k + 40 < cap; ++i) {
        DWORD l = pivot_collision(model, i), data, n = 0, flags = 0;
        if (!l || !list_info(l, &data, &n, &flags)) continue;
        k += (size_t)_snprintf(out + k, cap - k, "%s%d:%.16s:%u/%02X", k ? " " : "", i, node_name(pivot_node(model, i)), n, flags);
    }
}
static bool model_info(DWORD model, DWORD* asset, bool* has_hl, bool* owner) {
    __try {
        *asset = RD32(model + 0x50); if (!*asset) return false;
        *has_hl = (RD32(*asset + 0x54) & ~3u) != 0;
        DWORD nm = RD32(model + 0x3C); *owner = nm && RD32(nm + 0x60) == model;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void scan_model(Scan& S, DWORD model) {
    ++S.models;
    if (node_off(model)) return;
    float M[16]; if (!world_matrix(model, M)) return;
    ph::V3 pos = ph::v3(M[12], M[13], M[14]);
    DWORD asset = 0; bool has_hl = false, owner = false;
    if (!model_info(model, &asset, &has_hl, &owner)) return;
    int npiv = asset_pivots(asset);
    float dd = d2xz(pos, S.c);
    if (g_scan_dump && dd <= g_dump_radius * g_dump_radius)   // diagnostics: every model near Ram, before any filter
    {
        char pd[256]; pivot_desc(model, npiv, pd, sizeof pd);
        gp_log("physics:   model %s at (%.1f, %.1f, %.1f) d %.1f pivots %d hulllist %d owner %d parent %08X coll [%s]\n", model_name(model),
               pos.x, pos.y, pos.z, sqrtf(dd), npiv, has_hl ? 1 : 0, owner ? 1 : 0, node_parent(model), pd);
    }
    // big level models (HullList) are tested shape by shape; everything else by its node position
    if (!has_hl && dd > (S.r + 30.0f) * (S.r + 30.0f)) { ++S.far_models; return; }
    char name[160]; strncpy(name, model_name(model), sizeof name - 1); name[sizeof name - 1] = 0;
    if (!g_world_only && !has_hl && owner && dd <= S.r * S.r && (int)g_bodies.size() < g_c.max_bodies && ph::name_match(name, g_props) &&
        !ph::name_match(name, g_exclude)) {
        if (make_prop(S, model, asset, name, M) >= 0) return;
    }
    if (g_scan_dump && dd <= S.r * S.r)
        gp_log("physics:   static %s at (%.1f, %.1f, %.1f) pivots %d hulllist %d\n", name, pos.x, pos.y, pos.z, npiv, has_hl ? 1 : 0);
    if (has_hl) add_hulllist_static(S, asset, M, name);
    for (int i = 0; i < npiv && i < 256; ++i) {
        DWORD l = pivot_collision(model, i);
        if (l) add_list_static(S, l, pivot_node(model, i), name);
    }
}
static void scan_cbox(Scan& S, DWORD node) {   // CollisionBox entity: list +50h, the box shape centred on the node
    ++S.cboxes;
    if (node_off(node)) return;
    DWORD list = 0; __try { list = RD32(node + 0x50); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (list) add_list_static(S, list, node, "CollisionBox");
}
static bool read_links(DWORD n, DWORD* vt, DWORD* sib, DWORD* child) {
    __try { *vt = RD32(n); *sib = RD32(n + 0x34); *child = RD32(n + 0x44); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void scan(ph::V3 c) {
    double t0 = gp_now();
    ragdoll_world_reset();   // the old world (and every ragdoll body in it) is about to go
    g_bodies.clear();
    g_world = g_api->create(-g_c.gravity, 0.0f) != 0;
    if (!g_world) { gp_log("physics: Box3D world not created\n"); return; }
    Scan S; memset(&S, 0, sizeof S); S.c = c; S.r = g_c.radius;
    g_static_skipped = 0;
    DWORD scene = deref(A_SCENE), root = 0, vt, sib, child;
    if (scene && read_links(scene, &vt, &sib, &child)) root = child;
    std::vector<DWORD> stack; stack.reserve(4096);
    if (root) stack.push_back(root);
    while (!stack.empty() && S.visited < 400000) {
        DWORD n = stack.back(); stack.pop_back();
        if (!read_links(n, &vt, &sib, &child)) continue;
        ++S.visited;
        if (sib) stack.push_back(sib);
        car_scan_node(n);                                            // [car_physics]: car_move_ctrl nodes
        if (vt == VT_MODEL && car_scan_skip_model(n)) continue;     // a car (and its parts): a kinematic box, not level
        if (vt == VT_MODEL) scan_model(S, n);
        else if (vt == VT_CBOX) scan_cbox(S, n);
        if (child) stack.push_back(child);
    }
    g_center = c; g_clock.acc = 0.0f; g_clock.step = g_c.step;
    double ms = (gp_now() - t0) * 1000.0;
    g_st_scan.add(ms); ++g_scans; g_last_scan_t = gp_now();
    PhCounts pc; g_api->counts(&pc);
    gp_log("physics: scan #%d at (%.1f, %.1f, %.1f) r %.0f m: %d nodes, %d models (%d far), %d collision boxes -> %d static "
           "hulls (%d failed, %d over the cap), %d props as bodies (%d prop names without collision, %d with collision on several pivots); %.1f ms\n",
           g_scans, c.x, c.y, c.z, S.r, S.visited, S.models, S.far_models, S.cboxes, pc.static_shapes, pc.static_failed + S.failed,
           g_static_skipped, (int)g_bodies.size(), S.prop_nocoll, S.prop_multi, ms);
    car_after_scan();
}

// ================================================================================================ per frame
static void drop_body(size_t i, const char* why) {
    PBody& p = g_bodies[i];
    gp_log("physics: body #%d %s dropped (%s)\n", p.b, p.name, why);
    g_api->body_remove(p.b);
    g_bodies.erase(g_bodies.begin() + (ptrdiff_t)i);
    ++g_dropped;
}
static void validate() {
    for (size_t i = g_bodies.size(); i-- > 0;) {
        PBody& p = g_bodies[i];
        if (!alive(p.node, VT_MODEL)) { drop_body(i, "node gone"); continue; }
        DWORD a = 0; __try { a = RD32(p.node + 0x50); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (a != p.asset) { drop_body(i, "model changed (destroyed?)"); continue; }
        if (node_off(p.node)) { drop_body(i, "disabled"); continue; }
        DWORD par = 0; __try { par = RD32(p.node + 0x40); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (par != p.parent) { drop_body(i, "re-parented"); continue; }
    }
}
static int body_of_node(DWORD node) {   // the hit node is the model or one of its pivot nodes (up to 3 parents)
    for (int up = 0; up < 4 && node; ++up) {
        for (size_t i = 0; i < g_bodies.size(); ++i) if (g_bodies[i].node == node) return (int)i;
        __try { node = RD32(node + 0x40); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    return -1;
}
static void write_body(PBody& p, float alpha) {
    ph::V3 wp = p.cur_p; ph::Q4 wq = p.cur_q;
    if (g_c.interpolate && alpha < 1.0f) {
        wp = ph::add(p.prev_p, ph::mul(ph::sub(p.cur_p, p.prev_p), alpha));
        wq = ph::qslerp(p.prev_q, p.cur_q, alpha);
    }
    ph::V3 lp = wp; ph::Q4 lq = wq;
    if (p.parent) {
        float PM[16];
        if (!world_matrix(p.parent, PM) || !ph::local_from_world(PM, wp, wq, &lp, &lq)) return;
    }
    ph::Q4 sq = ph::stored_from_true(lq);
    float pos[3] = {lp.x, lp.y, lp.z}, q[4] = {sq.x, sq.y, sq.z, sq.w};
    __try {
        ((Vec_t)(uintptr_t)P_PH_SETPOS)(p.node, nullptr, pos);
        ((Vec_t)(uintptr_t)P_PH_SETORIENT)(p.node, nullptr, q);
        ++g_writes;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void resync_from_nodes() {   // after a rewind: bodies go back to where the game put the nodes
    for (auto& p : g_bodies) {
        float M[16]; if (!world_matrix(p.node, M)) continue;
        ph::Rigid r = ph::rigid_from_tod(M); if (!r.ok) continue;
        float pos[3] = {r.p.x, r.p.y, r.p.z}, q[4] = {r.q.x, r.q.y, r.q.z, r.q.w};
        g_api->body_set(p.b, pos, q, 0);
        p.prev_p = p.cur_p = r.p; p.prev_q = p.cur_q = r.q;
    }
    gp_log("physics: rewind ended, %d bodies put back where the game has them\n", (int)g_bodies.size());
    car_rewind_end();
}
static void explode_at(ph::V3 c, float radius, float impulse, const char* who) {
    if (!g_world) return;
    float cc[3] = {c.x, c.y, c.z};
    int n = g_api->explode(cc, radius, impulse, g_c.blast_lift);
    ragdoll_blast(cc, radius, impulse, g_c.blast_lift);   // [ragdoll]: thrown bodies, and the ones this blast kills
    ++g_blasts;
    gp_log("physics: %s blast at (%.2f, %.2f, %.2f) r %.1f m: %d bodies in reach\n", who, c.x, c.y, c.z, radius, n);
    if (n > 0) mark("ph_blast", 12, 120);
    car_blast(c.x, c.y, c.z, radius, impulse, g_c.blast_lift, who);   // [car_physics]: cars and wrecks too
}
// [car_physics] (car_game.cpp): the blast of a car's own explosion; a rescan after the drive rig moved props
void phys_blast_props(float x, float y, float z, float radius, float impulse, const char* who) { explode_at(ph::v3(x, y, z), radius, impulse, who); }
void phys_request_scan() { g_force_scan = true; }
bool phys_world_area(float centre[3], float* radius) {
    if (!g_world) return false;
    centre[0] = g_center.x; centre[1] = g_center.y; centre[2] = g_center.z; *radius = g_c.radius;
    return true;
}
static void debug_kick() {   // test copy: a blast 1.2 m from the nearest prop, on Ram's side (proves the pipeline)
    DWORD ram = ram_root(); ph::V3 rp;
    if (!ram || !node_pos(ram, &rp) || g_bodies.empty()) { gp_log("physics: debug kick: no prop / no Ram\n"); return; }
    int best = -1; float bd = 1e30f;
    for (size_t i = 0; i < g_bodies.size(); ++i) { float d = ph::len(ph::sub(g_bodies[i].cur_p, rp)); if (d < bd) { bd = d; best = (int)i; } }
    if (best < 0 || bd > 25.0f) { gp_log("physics: debug kick: nearest prop %.1f m away, nothing done\n", bd); return; }
    ph::V3 bp = g_bodies[best].cur_p;
    ph::V3 dir = ph::norm(ph::v3(rp.x - bp.x, 0, rp.z - bp.z));
    ph::V3 c = ph::add(bp, ph::mul(dir, g_kick_back)); c.y = bp.y - 0.1f;
    gp_log("physics: debug kick next to %s (%.1f m from Ram)\n", g_bodies[best].name, bd);
    explode_at(c, 5.0f, g_kick_impulse, "debug kick");
}
// ---- test copy only: props gathered in front of Ram, a real game explosion among them
static const DWORD C_GAMECAM = 0x260, C_SHELLS = 0x550, H_SHELL = 0x843D10, A_LIST_ENTITY = 0xA3B8A8;
static const int MSG_SHELL_EXPLODE = -9, MSG_SHELL_MORPH = -11, MSG_SHELL_AIM = -12;   // weapon_shell (suave/code/suave_msgs.h, check_suave.py)
static double g_ctl_since = 0.0; static bool g_debug_refs = false;
static DWORD cache_ptr(DWORD off) { DWORD cd = gp_cache_data(); __try { return cd ? RD32(cd + off) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_handler(DWORD node) {   // script class object (node -> +4 type -> +28h) -> +60h handler (as suave_game.cpp)
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD c = t ? RD32(t + 0x28) : 0; return c ? RD32(c + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD list_at(DWORD L, int i) {   // element i of an engine entity list (stride from the list type [0xA3B8A8] +24h)
    __try {
        if (!L || i < 0 || i >= (int)RD32(L + 8)) return 0;
        DWORD t = RD32(A_LIST_ENTITY); DWORD k = t ? RD32(t + 0x24) : 0; if (!k) return 0;
        DWORD a = RD32(L + 4); DWORD d = a ? RD32(a + 4) : 0; if (!d) return 0;
        return RD32(d + (DWORD)i * 4 * k);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int list_count(DWORD L) { __try { return L ? (int)RD32(L + 8) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static bool set_world_pos(DWORD node, ph::V3 p) {
    float v[3] = {p.x, p.y, p.z};
    __try { ((Vec_t)(uintptr_t)P_PH_SETWPOS)(node, nullptr, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static ph::V3 view_dir(ph::V3 ram) {   // the game camera's z axis, flat, pointing from the camera toward Ram
    float M[16]; DWORD cam = cache_ptr(C_GAMECAM);
    if (cam && world_matrix(cam, M)) {
        ph::V3 f = ph::norm(ph::v3(M[8], 0.0f, M[10]));
        ph::V3 c = ph::v3(M[12], M[13], M[14]);
        if (ph::len(f) > 0.5f) { if (ph::dot(f, ph::sub(ram, c)) < 0.0f) f = ph::mul(f, -1.0f); return f; }
    }
    return ph::v3(0, 0, 0);
}
// The test rig: the game's teleport of Ram does not stick (his physics controller keeps its own position), so the props
// come to him instead. debug_gather = patterns: up to debug_gather_max matching models within debug_gather_radius are put
// debug_gather_dist m ahead of Ram (the camera's view), two rows 1.4 m apart, their collision's lowest point
// debug_gather_drop m above his feet (with physics they fall and settle; vanilla: drop 0, standing). Then a scan.
static float collision_floor(DWORD model, float my) {   // lowest collision point below the node origin (m), 0 if none
    int npiv = asset_pivots(node_asset(model)); float lo = 0.0f; bool any = false;
    for (int pi = 0; pi < npiv && pi < 64; ++pi) {
        DWORD list = pivot_collision(model, pi), data, n, flags;
        if (!list || !list_info(list, &data, &n, &flags)) continue;
        DWORD pn = pivot_node(model, pi); float PW[16];
        if (!pn || (pi > 0 && node_off(pn)) || !_strnicmp(node_name(pn), "Replace", 7) || !world_matrix(pn, PW)) continue;
        for (DWORD i = 0; i < n; ++i) {
            DWORD sh = arr_ptr(data, i), surf = 0;
            int k = sh ? shape_points(sh, PW, ph::v3(1, 1, 1), g_pts, &surf) : 0;
            for (int j = 0; j < k; ++j) { float d = g_pts[j].y - my; if (!any || d < lo) { lo = d; any = true; } }
        }
    }
    return any ? lo : 0.0f;
}
struct GCand { DWORD node; float d; };
static void gather_walk(ph::V3 rp, GCand* out, int* n, int cap) {
    DWORD scene = deref(A_SCENE), root = 0, vt, sib, child;
    if (scene && read_links(scene, &vt, &sib, &child)) root = child;
    static DWORD stack[8192]; int sp = 0; int visited = 0;
    if (root) stack[sp++] = root;
    while (sp > 0 && visited < 400000) {
        DWORD nd = stack[--sp];
        if (!read_links(nd, &vt, &sib, &child)) continue;
        ++visited;
        if (sib && sp < 8191) stack[sp++] = sib;
        if (child && sp < 8191) stack[sp++] = child;
        if (vt != VT_MODEL || node_off(nd)) continue;
        const char* nm = model_name(nd);
        if (!ph::name_match(nm, g_gather) || ph::name_match(nm, g_exclude)) continue;
        ph::V3 p; if (!node_pos(nd, &p)) continue;
        float d = ph::len(ph::sub(p, rp)); if (d > g_gather_radius) continue;
        if (*n < cap) { out[(*n)++] = GCand{nd, d}; }
        else { int w = 0; for (int i = 1; i < cap; ++i) if (out[i].d > out[w].d) w = i; if (d < out[w].d) out[w] = GCand{nd, d}; }
    }
}
static void debug_gather(ph::V3 rp, double now) {
    if (!g_gather[0] || g_gathered || !g_debug_refs) return;
    if (g_gather_key != gp::KEY_NONE) {   // a key: the delay below counts from the title level, which also ticks
        if (!g_gather_req) return;
        g_gather_req = false;
    } else {
        if (g_ctl_since == 0.0) g_ctl_since = now;
        if (now - g_ctl_since < g_place_delay) return;
    }
    g_gathered = true;
    GCand c[32]; int n = 0; int cap = g_gather_max < 32 ? g_gather_max : 32;
    gather_walk(rp, c, &n, cap);
    for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j) if (c[j].d < c[i].d) { GCand t = c[i]; c[i] = c[j]; c[j] = t; }
    ph::V3 f = view_dir(rp); if (ph::len(f) < 0.5f) f = ph::v3(1, 0, 0);
    ph::V3 r = ph::v3(-f.z, 0.0f, f.x);
    float drop = g_vanilla ? 0.0f : g_gather_drop;
    int row = (n + 1) / 2;
    for (int i = 0; i < n; ++i) {
        int k = i < row ? i : i - row; int cnt = i < row ? row : n - row;
        ph::V3 at = ph::add(ph::add(rp, ph::mul(f, g_gather_dist + (i < row ? 0.0f : 1.6f))), ph::mul(r, g_gather_side + (k - (cnt - 1) * 0.5f) * 1.4f));
        ph::V3 p0; node_pos(c[i].node, &p0);
        at.y = rp.y - collision_floor(c[i].node, p0.y) + drop;
        set_world_pos(c[i].node, at);
        if (g_ngather < 32) g_gather_nodes[g_ngather++] = c[i].node;
        gp_log("physics: debug gather: %s from (%.1f, %.1f, %.1f) [%.0f m] to (%.2f, %.2f, %.2f)\n", model_name(c[i].node), p0.x, p0.y, p0.z, c[i].d, at.x, at.y, at.z);
    }
    gp_log("physics: debug gather: %d prop(s) put %.1f m ahead of Ram at (%.1f, %.1f, %.1f), view (%.2f, %.2f), drop %.2f m\n",
           n, g_gather_dist, rp.x, rp.y, rp.z, f.x, f.z, drop);
    g_force_scan = true;
    if (g_debug_shots) { g_last_mark = 0.0; mark("ph_gather", 12, 120); }
}
// a pooled weapon_shell: morphed, aimed (owner = the shell, not the player: armed, fuse lit, as the Suave dynamite), put next to the props and
// exploded by the game itself 0.3 s later (the explode handler of a never-aimed shell gave no blast, run p1)
static DWORD g_boom_shell = 0; static double g_boom_at = 0.0; static ph::V3 g_boom_c = {0, 0, 0};
static void arm_shell_at(ph::V3 c, double now);
// [car_physics] test rig: a real game shell explosion at a point (car_game.cpp debug_wreck_key)
bool phys_debug_shell_at(float x, float y, float z) { if (!g_debug_refs) return false; arm_shell_at(ph::v3(x, y, z), gp_now()); return true; }
static void debug_boom(ph::V3 rp, double now) {
    if (!g_debug_refs) { gp_log("physics: debug boom: references differ\n"); return; }
    ph::V3 t = rp; bool have = false;
    if (!g_bodies.empty()) {   // the middle of the live prop bodies within 20 m (a destroyed havoc prop's node jumps
        ph::V3 sum = ph::v3(0, 0, 0); int k = 0;   // back to its placed spot, run p2)
        for (auto& b : g_bodies) if (ph::len(ph::sub(b.cur_p, rp)) < 20.0f) { sum = ph::add(sum, b.cur_p); ++k; }
        if (k) { t = ph::mul(sum, 1.0f / k); have = true; }
    }
    if (!have && g_ngather > 0) {   // vanilla runs: the gathered props still standing within 20 m
        ph::V3 sum = ph::v3(0, 0, 0); int k = 0;
        for (int i = 0; i < g_ngather; ++i) {
            ph::V3 p; if (node_off(g_gather_nodes[i]) || !node_pos(g_gather_nodes[i], &p) || ph::len(ph::sub(p, rp)) > 20.0f) continue;
            sum = ph::add(sum, p); ++k;
        }
        if (k) { t = ph::mul(sum, 1.0f / k); have = true; }
    }
    if (!have) { gp_log("physics: debug boom: nothing to blow up\n"); return; }
    ph::V3 dir = ph::norm(ph::v3(rp.x - t.x, 0, rp.z - t.z));
    ph::V3 c = ph::add(t, ph::mul(dir, g_boom_back)); c.y = rp.y + 0.2f;
    arm_shell_at(c, now);
}
static void arm_shell_at(ph::V3 c, double now) {
    DWORD L = cache_ptr(C_SHELLS); int n = list_count(L); DWORD shell = 0;
    for (int i = 0; i < n && !shell; ++i) {
        DWORD sh = list_at(L, i);
        if (!sh || cls_handler(sh) != H_SHELL) continue;
        DWORD d = 0; __try { d = RD32(sh + 0x18); if (d && RD32(d + 0x1C)) shell = sh; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!shell) { gp_log("physics: debug boom: no ready shell in the pool (%d)\n", n); return; }
    int type = g_shell_type;
    __try { DWORD d = RD32(shell + 0x18); WR32(d + 0x24, shell); } __except (EXCEPTION_EXECUTE_HANDLER) {}   // owner 0 crashed the aim (run p2)
    gp_send(shell, MSG_SHELL_MORPH, &type);
    float v[3] = {c.x, c.y, c.z};
    gp_send(shell, MSG_SHELL_AIM, v);
    set_world_pos(shell, c);
    g_boom_shell = shell; g_boom_at = now + 0.3; g_boom_c = c;
    gp_log("physics: debug boom: shell type %d armed at (%.2f, %.2f, %.2f)\n", type, c.x, c.y, c.z);
}
static void debug_boom_tick(double now) {
    if (!g_boom_shell || now < g_boom_at) return;
    DWORD shell = g_boom_shell; g_boom_shell = 0;
    set_world_pos(shell, g_boom_c);
    __try { DWORD d = RD32(shell + 0x18); WR32(d + 0x24, ram_root()); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    struct { int explode, water; float v[3]; } e = {1, 0, {0, 0, 0}};
    bool ok = gp_send(shell, MSG_SHELL_EXPLODE, &e);
    gp_log("physics: debug boom: command_explode sent%s (the game's blast follows; physics logs a 'shell blast' line)\n", ok ? "" : " FAILED");
    if (g_debug_shots) { g_last_mark = 0.0; mark("ph_boom", 18, 80); }
}

static void log_stats(double now) {
    PhCounts pc; g_api->counts(&pc);
    int active = 0; for (auto& p : g_bodies) if (p.active) ++active;
    gp_log("physics: stats %d bodies (%d awake, peak %d, %d moved so far), %d static hulls | step %.3f ms avg %.3f max "
           "(%d steps) | write %.3f ms avg %.3f max | frame total %.3f ms avg %.3f max | hits %d (%d on non-props), blasts %d, "
           "writes %d, dropped %d | vehicle frames %d (%d drives)\n",
           pc.bodies, pc.awake, g_peak_awake, active, pc.static_shapes, g_st_step.avg(), g_st_step.mx, g_st_step.n,
           g_st_write.avg(), g_st_write.mx, g_st_frame.avg(), g_st_frame.mx, g_hits, g_hit_misses, g_blasts, g_writes, g_dropped,
           g_drive_frames_total, g_drive_spans);
    int shown = 0;   // which bodies keep moving (a body that never sleeps costs a write every frame)
    for (auto& p : g_bodies) {
        float pos[3], q[4], v[3], w[3];
        if (shown >= 4 || g_api->body_get(p.b, pos, q) != 1) continue;
        g_api->body_vel(p.b, v, w); ++shown;
        gp_log("physics:   awake #%d %s at (%.2f, %.2f, %.2f) speed %.3f m/s spin %.3f rad/s\n", p.b, p.name, pos[0], pos[1], pos[2],
               sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]));
    }
    g_st_step.reset(); g_st_write.reset(); g_st_frame.reset(); g_last_log = now;
    car_log_stats();
}

// ---- the vehicle frame drive: Ram in a vehicle = the on-foot input tick does not run (see the header)
static bool g_drive_ok = false, g_drive_on = false, g_drive_unknown_logged = false;
static LONG g_foot_frame = -100;                 // the last pass the on-foot input tick ran in
static ph::DriveGate g_gate;
static double g_drive_t0 = 0.0;
static int g_drive_frames = 0, g_drive_steps0 = 0, g_drive_scans0 = 0;   // this drive (g_drive_spans / g_drive_frames_total: state)
static ph::V3 g_drive_prev = {0, 0, 0}, g_drive_vel = {0, 0, 0}; static bool g_drive_have_prev = false;
static DWORD p_data(DWORD n) { __try { return n ? RD32(n + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD p_cls(DWORD n) {   // the node's script class main handler (class +60h)
    __try { if (!n) return 0; DWORD t = RD32(n + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD p_rd(DWORD a) { __try { return a ? RD32(a) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
// Ram's vehicle (player_ai data +24h) when that vehicle's vehicle_ai has ControlState 1; *veh_out = the node, *cls_out = its class.
// 1 = he drives it, 0 = not, -1 = a vehicle node of a class not understood (logged once)
static int ram_vehicle(DWORD* veh_out, DWORD* cls_out) {
    *veh_out = 0; *cls_out = 0;
    DWORD rd = p_data(ram_root()); if (!rd) return 0;
    DWORD ai = p_rd(rd + ROOT_AI); if (p_cls(ai) != H_PLAYER_AI) return 0;
    DWORD veh = p_rd(p_data(ai) + PAI_VEHICLE); if (!veh) return 0;
    *veh_out = veh;
    DWORD vai = 0, n = veh;
    for (int up = 0; up < 4 && n && !vai; ++up) {   // the vehicle_root, its vehicle_ai, or a part of the car (parent +40h)
        DWORD c = p_cls(n); if (up == 0) *cls_out = c;
        if (c == H_VAI) vai = n;
        else if (c == H_VROOT) { vai = p_rd(p_data(n) + VR_AICTRL); break; }
        else n = p_rd(n + N_PARENT);
    }
    if (!vai || p_cls(vai) != H_VAI) return -1;
    return (int)p_rd(p_data(vai) + VA_CTRL) == 1 ? 1 : 0;
}
static bool read_game_ms(uint32_t* ms) { __try { *ms = (uint32_t)RD32(A_GAME_MS); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

static void world_frame(double now, bool driving);
void phys_tick(void* thread, double now) {
    (void)thread;
    if (!g_installed) return;
    LONG f = g_frame;
    g_foot_frame = f;                // the on-foot tick runs: it steps the world itself (the vehicle drive stays off)
    if (f == g_tick_frame) return;   // the kb and pad procedures may both tick: once per main-loop pass
    g_tick_frame = f;
    world_frame(now, false);
}
static void drive_end(const char* why) {
    if (!g_drive_on) return;
    g_drive_on = false;
    gp_log("physics: vehicle drive ended (%s) after %.1f s: %d frames, %d steps, %d scans\n", why, gp_now() - g_drive_t0, g_drive_frames,
           g_steps - g_drive_steps0, g_scans - g_drive_scans0);
}
// every main-loop pass (POST_INPUT): the world's step while Ram is in a vehicle
static void drive_frame(double now) {
    uint32_t ms = 0; bool clk = read_game_ms(&ms);
    uint32_t adv = clk ? g_gate.advance(ms) : 0;
    if (!g_drive_ok || !g_c.drive_step) return;
    LONG f = g_frame;
    if (g_foot_frame >= f - 1) {   // the on-foot tick ran in the last pass (or already in this one): it steps the world
        drive_end("the on-foot tick runs again"); g_drive_have_prev = false; return;
    }
    DWORD veh = 0, cls = 0;
    int iv = ram_vehicle(&veh, &cls);
    if (iv < 0 && !g_drive_unknown_logged) {
        g_drive_unknown_logged = true;
        gp_log("physics: Ram's vehicle node %08X has class %08X, no vehicle_ai found: the world does not step while he sits in it\n", veh, cls);
    }
    if (iv != 1) { drive_end(iv < 0 ? "vehicle not understood" : "not in a vehicle"); g_drive_have_prev = false; return; }
    int dec = ph::drive_decision(true, false, adv, game_dt(), gp_playmode(), gp_cutscene(), true);
    if (dec == ph::DRV_REWIND) { if (g_world) g_rewinding = true; return; }
    if (dec != ph::DRV_STEP) return;
    if (!g_drive_on) {
        g_drive_on = true; ++g_drive_spans; g_drive_t0 = gp_now(); g_drive_frames = 0; g_drive_steps0 = g_steps; g_drive_scans0 = g_scans;
        ph::V3 rp = {0, 0, 0}, vp = {0, 0, 0}; bool rok = node_pos(ram_root(), &rp), vok = node_pos(veh, &vp);
        gp_log("physics: Ram is in a vehicle (node %08X class %08X, control state 1; Ram %s(%.1f, %.1f, %.1f), the vehicle %s(%.1f, %.1f, %.1f)): "
               "the world steps every frame from the game clock until he is on foot\n", veh, cls, rok ? "" : "not read ", rp.x, rp.y, rp.z,
               vok ? "" : "not read ", vp.x, vp.y, vp.z);
    }
    if (f == g_tick_frame) return;   // already stepped in this pass
    g_tick_frame = f;
    ++g_drive_frames; ++g_drive_frames_total;
    world_frame(now, true);
}

static void world_frame(double now, bool driving) {
    double t0 = gp_now();
    int pm = gp_playmode();
    if (pm == 3) { g_rewinding = true; return; }        // rewind: the game restores the nodes
    if (g_rewinding) { g_rewinding = false; g_pending_resync = true; }
    if (pm == 2) return;                                // paused
    DWORD ram = ram_root(); ph::V3 rp;
    if (!ram || !node_pos(ram, &rp)) return;
    ph::V3 sp = rp;   // where a scan is centred (and measured from): ahead of the car while driving
    if (driving) {
        float gdt0 = game_dt();
        if (g_drive_have_prev && gdt0 > 0.0f) {
            ph::V3 v = ph::mul(ph::sub(rp, g_drive_prev), 1.0f / gdt0);
            if (ph::len(v) > 80.0f) v = ph::v3(0, 0, 0);     // a teleport
            g_drive_vel = ph::add(ph::mul(g_drive_vel, 0.8f), ph::mul(v, 0.2f));
        } else if (!g_drive_have_prev) g_drive_vel = ph::v3(0, 0, 0);
        g_drive_prev = rp; g_drive_have_prev = true;
        sp = ph::drive_scan_point(rp, g_drive_vel, g_c.drive_lead, g_c.radius * 0.4f);
    }
    debug_gather(rp, now);
    if (g_boom_req) { g_boom_req = false; debug_boom(rp, now); }
    debug_boom_tick(now);
    if (g_vanilla) return;                              // test copy "before" runs: no simulation
    if (g_world) validate();
    if (g_world && g_pending_resync) { resync_from_nodes(); g_pending_resync = false; }
    float gdt = game_dt();
    if (g_world) car_frame(now, gdt);                   // [car_physics]: car list, poses, explosions of cars, wrecks
    PhCounts pc; if (g_world) g_api->counts(&pc); else memset(&pc, 0, sizeof pc);
    bool moved_far = ph::len(ph::sub(sp, g_center)) > g_c.rescan_distance;
    if (!g_world || g_force_scan || (moved_far && pc.awake == 0 && !ragdoll_busy() && now - g_last_scan_t > 2.0)) { g_force_scan = false; scan(sp); }
    if (!g_world) return;
    if (g_kick_req) { g_kick_req = false; debug_kick(); }
    if (g_dump_req) { g_dump_req = false; bool d = g_scan_dump; g_scan_dump = true; scan(rp); g_scan_dump = d; }
    // fixed steps
    int n = g_clock.advance(gdt);
    for (int s = 0; s < n; ++s) {
        for (auto& p : g_bodies) { p.prev_p = p.cur_p; p.prev_q = p.cur_q; }
        car_before_step(s, n, g_clock.step);            // [car_physics]: kinematic car boxes sweep to the cars' poses
        ragdoll_before_step(g_clock.step);   // [ragdoll]: joint targets / strengths / pelvis anchors
        double sms = g_api->step(g_clock.step, ragdoll_substeps(g_c.substeps));
        g_st_step.add(sms); ragdoll_step_cost(sms); ++g_steps;
        ragdoll_after_step(g_clock.step);    // [ragdoll]: read the bodies, settle / recover
        car_after_step();                               // [car_physics]: prop kicks on first touch, wreck poses
        for (auto& p : g_bodies) {
            float pos[3], q[4];
            int aw = g_api->body_get(p.b, pos, q);
            if (aw < 0) continue;
            p.cur_p = ph::v3(pos[0], pos[1], pos[2]); ph::Q4 qq = {q[0], q[1], q[2], q[3]}; p.cur_q = qq;
            if (aw == 1) { p.active = true; p.sleeping_written = false; }
        }
    }
    // write back the moving ones (and once more when they fall asleep)
    double w0 = gp_now(); int awake = 0;
    float alpha = g_clock.alpha();
    for (auto& p : g_bodies) {
        if (!p.active) continue;
        float pos[3], q[4];
        int aw = g_api->body_get(p.b, pos, q);
        if (aw == 1) { ++awake; write_body(p, alpha); }
        else if (aw == 0 && !p.sleeping_written) {
            p.prev_p = p.cur_p; p.prev_q = p.cur_q; write_body(p, 1.0f); p.sleeping_written = true;
            float M[16]; ph::V3 drawn = p.cur_p;   // what the game now has (its world matrix after our write)
            if (world_matrix(p.node, M)) drawn = ph::v3(M[12], M[13], M[14]);
            float turn = 2.0f * acosf(fminf(1.0f, fabsf(p.start_q.x * p.cur_q.x + p.start_q.y * p.cur_q.y + p.start_q.z * p.cur_q.z + p.start_q.w * p.cur_q.w))) * 57.29578f;
            gp_log("physics: body #%d %s settled at (%.2f, %.2f, %.2f): moved %.2f m, turned %.0f deg from its placed pose; the game's node is at (%.2f, %.2f, %.2f) (%.3f m off)\n",
                   p.b, p.name, p.cur_p.x, p.cur_p.y, p.cur_p.z, ph::len(ph::sub(p.cur_p, p.start_p)), turn, drawn.x, drawn.y, drawn.z,
                   ph::len(ph::sub(drawn, p.cur_p)));
        }
    }
    car_after_frame(alpha);                             // [car_physics]: wrecks written back
    if (awake > g_peak_awake) g_peak_awake = awake;
    double t1 = gp_now();
    if (awake) g_st_write.add((t1 - w0) * 1000.0);
    if (n > 0 || awake) g_st_frame.add((t1 - t0) * 1000.0);
    if (now - g_last_log >= g_c.log_seconds && !g_world_only) log_stats(now);
    ragdoll_input_tick(now);   // [ragdoll]: corpses follow their pelvis, stumbles, test-copy keys, statistics
}

// main loop pass, after the input poll: frame counter, test-copy keys (the key script writes the game's arrays)
void phys_post_input(BYTE* kb_edges, BYTE* kb_state, DWORD* mouse, double now) {
    (void)now;
    if (!g_installed) return;
    InterlockedIncrement(&g_frame);
    ragdoll_post_input(kb_edges, kb_state, mouse, now);
    drive_frame(now);   // Ram in a vehicle (no on-foot tick): the world steps here
    if (!kb_edges || !kb_state) return;
    auto pressed = [&](int k) { return k >= 0 && k < 256 && (kb_edges[k] & 1); };
    auto held = [&](int k) { return k >= 0 && k < 256 && (kb_state[k] & 0x80); };
    if (pressed(g_kick_key)) g_kick_req = true;
    if (pressed(g_gather_key)) g_gather_req = true;
    if (pressed(g_dump_key)) g_dump_req = true;
    if (pressed(g_boom_key)) g_boom_req = true;
    car_post_input(kb_edges, kb_state, now);
    if (g_fire_key >= 0 && mouse) {   // the fire key holds the left mouse button (bit 0 held, bit 1 pressed, bit 2 released)
        g_fire_down = held(g_fire_key);
        __try {
            if (g_fire_down || g_fire_prev) *((BYTE*)mouse + 0x48) = 1;   // mouse object "active" byte (poll 0x43B670)
            if (g_fire_down && !g_fire_prev) mouse[0] |= 3;
            else if (g_fire_down) mouse[0] |= 1;
            else if (g_fire_prev) mouse[0] = (mouse[0] & ~1u) | 4;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_fire_prev = g_fire_down;
    }
}

// ================================================================================================ hooks
static int g_seg_logged = 0;
// PH_BULLET: the bullet step's line test, thiscall(probe, -1, from, to) ret 0Ch, al = hit; probe +80h = the hit node
static unsigned __fastcall PhLineWrap(DWORD probe, void* edx, int a, float* from, float* to) {
    unsigned res = ((Line_t)(uintptr_t)R_PH_LINE)(probe, edx, a, from, to);
    if (g_debug_shots && g_seg_logged < 60) {
        ++g_seg_logged;
        __try { gp_log("physics: bullet segment (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) game hit %d\n", from[0], from[1], from[2], to[0], to[1], to[2], res & 0xFF); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!g_world || !g_hooks_bullet) return res;
    __try { ragdoll_bullet(from, to, (res & 0xFF) != 0, (res & 0xFF) ? RD32(probe + 0x80) : 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        bool game_hit = (res & 0xFF) != 0;
        DWORD hit_node = game_hit ? RD32(probe + 0x80) : 0;
        float f[3] = {from[0], from[1], from[2]}, t[3] = {to[0], to[1], to[2]}, h[3]; void* user = nullptr;
        car_bullet(f, t, g_c.bullet_impulse);           // [car_physics]: wrecks
        int b = g_api->raycast(f, t, h, &user);
        if (b < 0) return res;
        int bi = -1; for (size_t i = 0; i < g_bodies.size(); ++i) if (g_bodies[i].b == b) { bi = (int)i; break; }
        if (bi < 0) {   // not a prop: a [mayhem] debris piece takes the push
            if (!game_hit) { ph::V3 imp = ph::bullet_impulse(ph::v3(f[0], f[1], f[2]), ph::v3(t[0], t[1], t[2]), g_c.bullet_impulse * 0.3f);
                             float iv[3] = {imp.x, imp.y, imp.z}; mprops_bullet_body(b, iv, h); }
            return res;
        }
        if (game_hit && hit_node && body_of_node(hit_node) != bi) { ++g_hit_misses; return res; }   // something else was hit first
        ph::V3 imp = ph::bullet_impulse(ph::v3(f[0], f[1], f[2]), ph::v3(t[0], t[1], t[2]), g_c.bullet_impulse);
        float iv[3] = {imp.x, imp.y + g_c.bullet_impulse * 0.15f, imp.z};
        g_api->impulse(b, iv, h);
        ++g_hits;
        if (g_hits <= 30 || g_hits % 50 == 0)
            gp_log("physics: bullet hit %s at (%.2f, %.2f, %.2f)%s, %.0f N s\n", g_bodies[bi].name, h[0], h[1], h[2],
                   game_hit ? "" : " (the game's own test missed it)", g_c.bullet_impulse);
        mark("ph_hit", 10, 100);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return res;
}
// PH_SHELL: weapon_shell command_explode -> cdecl 0x8460B0(thread, &result), [0xA3B75C] = the shell node
static DWORD __cdecl PhShellWrap(DWORD thread, DWORD out) {
    DWORD node = 0; __try { node = RD32(A_CUR_NODE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ph::V3 c; bool have = node && ((g_world && g_hooks_blast) || mprops_wants_blasts()) && node_pos(node, &c);
    DWORD r = ((Script2_t)(uintptr_t)R_PH_SHELL)(thread, out);
    if (have && g_world && g_hooks_blast) explode_at(c, g_c.shell_radius, g_c.shell_impulse, "shell");
    if (have) { float p[3] = {c.x, c.y, c.z}; mprops_on_blast(1, node, p); }   // [mayhem] chain reactions (queued)
    return r;
}
// PH_DESTROY: Destroyable_Object command_destroy_object -> cdecl 0x668BA0(thread, 0), [0xA3B75C] = the object
static DWORD __cdecl PhDestroyWrap(DWORD thread, DWORD zero) {
    DWORD node = 0; __try { node = RD32(A_CUR_NODE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ph::V3 c; bool have = node && ((g_world && g_hooks_blast) || mprops_wants_blasts()) && node_pos(node, &c);
    DWORD r = ((Script2_t)(uintptr_t)R_PH_DESTROY)(thread, zero);
    if (have && g_world && g_hooks_blast) explode_at(c, g_c.destroy_radius, g_c.destroy_impulse, "destroyable");
    if (have) { float p[3] = {c.x, c.y, c.z}; mprops_on_blast(2, node, p); }   // [mayhem] chain reactions (queued)
    return r;
}
// ---- for [mayhem] (mayhem/code/mh_game.cpp): the Box3D world, its generation (a scan recreates it) and blasts
// (phys_api / phys_world_ready: one accessor each, with [ragdoll]'s below)
int  phys_world_gen() { return g_scans; }
void phys_blast(const float* c, float radius, float impulse, const char* who) { if (c) explode_at(ph::v3(c[0], c[1], c[2]), radius, impulse, who); }
void phys_destroy_blast(const float* c) { if (c && g_hooks_blast) explode_at(ph::v3(c[0], c[1], c[2]), g_c.destroy_radius, g_c.destroy_impulse, "destroyable"); }

// ================================================================================================ install
bool phys_install(bool tick_ok) {
    if (!g_c.enabled && !g_world_only) return false;
    if (g_world_only) gp_log("physics: [physics] is off, [ragdoll] is on: the level world only (no props)\n");
    if (!tick_ok) { gp_log("physics: the input tick is not installed: OFF\n"); return false; }
    for (const char* r : PH_REFS) if (!gp_ref_ok(r)) { gp_log("physics: reference %s differs: OFF, nothing patched\n", r); return false; }
    g_debug_refs = true;
    for (const char* r : PH_DEBUG_REFS) if (!gp_ref_ok(r)) g_debug_refs = false;
    if (g_vanilla) {   // test copy: the same debug keys and placement, no Box3D, no hook sites (the vanilla comparison)
        g_installed = true;
        gp_log("physics: VANILLA comparison mode (debug_vanilla = 1): no simulation, no hook sites; debug keys %s\n", g_debug_refs ? "on" : "OFF (refs)");
        return true;
    }
    char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) sl[1] = 0; else path[0] = 0;
    strncat(path, "TODPhysics.dll", MAX_PATH - strlen(path) - 1);
    g_mod = LoadLibraryA(path);
    TODPhysics_GetApi_t get = g_mod ? (TODPhysics_GetApi_t)GetProcAddress(g_mod, "TODPhysics_GetApi") : nullptr;
    g_api = get ? get(TODPHYS_API_VERSION) : nullptr;
    if (!g_api) { gp_log("physics: %s not loaded (or wrong version): OFF\n", path); return false; }
    g_clock.step = g_c.step;
    int hooks = 0;
    if (g_hooks_bullet) { if (gp_site_ok("PH_BULLET") && gp_patch_call("PH_BULLET", (void*)&PhLineWrap)) ++hooks; else gp_log("physics: bullet site differs: bullets push nothing\n"); }
    if (g_hooks_blast) {
        if (gp_site_ok("PH_SHELL") && gp_patch_call("PH_SHELL", (void*)&PhShellWrap)) ++hooks; else gp_log("physics: shell explosion site differs\n");
        if (gp_site_ok("PH_DESTROY") && gp_patch_call("PH_DESTROY", (void*)&PhDestroyWrap)) ++hooks; else gp_log("physics: destroyable explosion site differs\n");
    }
    g_installed = true;
    g_drive_ok = true;
    for (const char* r : PH_DRIVE_REFS) if (!gp_ref_ok(r)) { g_drive_ok = false; gp_log("physics: reference %s differs: the world does not step while Ram drives\n", r); break; }
    car_install(g_api, g_mod);                          // [car_physics] (logs its own state)
    gp_log("physics: Box3D loaded (%s), %d hook site(s); radius %.0f m, step %.3f s x %d substeps, max %d bodies, props '%s'; "
           "steps in a vehicle %s (scan lead %.1f s)\n", path, hooks, g_c.radius, g_c.step, g_c.substeps, g_c.max_bodies, g_props,
           !g_c.drive_step ? "off (drive_step = 0)" : g_drive_ok ? "on" : "OFF (references)", g_c.drive_lead);
    return true;
}

// [ragdoll] (ragdoll/code/rd_game.cpp) shares the world, the clock and the module
HMODULE phys_module() { return g_mod; }
const PhApi* phys_api() { return g_api; }   // its calls check the world themselves
bool phys_world_ready() { return g_installed && g_world && g_api; }
float phys_alpha() { float a = g_clock.alpha(); return g_c.interpolate ? (a < 0 ? 0 : (a > 1 ? 1 : a)) : 1.0f; }
LONG phys_frame() { return g_frame; }

void* phys_test_stub(const char* name) {
    if (!strcmp(name, "ph_line")) return (void*)&PhLineWrap;
    if (!strcmp(name, "ph_shell")) return (void*)&PhShellWrap;
    if (!strcmp(name, "ph_destroy")) return (void*)&PhDestroyWrap;
    if (!strcmp(name, "ph_r_line")) return (void*)&R_PH_LINE;         // the host redirects the originals to stand-ins
    if (!strcmp(name, "ph_r_shell")) return (void*)&R_PH_SHELL;
    if (!strcmp(name, "ph_r_destroy")) return (void*)&R_PH_DESTROY;
    return nullptr;
}
