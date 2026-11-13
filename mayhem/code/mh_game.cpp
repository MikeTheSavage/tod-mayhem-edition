// mh_game.cpp - [mayhem] props features of TODGameplay.dll: chain reactions and breakable cover,
// built on the [physics] Box3D module (physics/code/phys_game.cpp). Off unless [mayhem] chain_reactions / breakable_cover
// = 1 AND [physics] enabled = 1 (Box3D loaded); then every MH_ REF row must match first. Docs: mayhem/README.md,
// dll/HOOKS.md "Mayhem props".
//
//   * Explosions: the [physics] wrappers of weapon_shell's blast (PH_SHELL) and Destroyable_Object's explosion
//     (PH_DESTROY) report every blast here, and two own call sites (MH_CAR_A / MH_CAR_B) wrap vehicle_ai's explosion
//     0x806860. Nothing is done inside the game's script call: the blast is queued and handled in the input tick.
//   * Chain reactions: the tick asks the quadtree (as Destroyable_Object do_explosion 0x66A600 does: mask 440h, mode 1)
//     for what is within chain_radius of each blast; explosive Destroyable_Objects (IsExplodeable, alive, not Locked,
//     not ImmuneToDamage) and cars get a detonation scheduled 0.15 .. 0.6 s of game time later, nearer ones first.
//     A detonation is the game's own take_a_hit (global command 215 by Node::SendCommand 0x86A340, the 11-value
//     argument block do_explosion sends: damage, owner, inflictor, collision volume, impulse, position, type 1), so the
//     game's own explosion, fire, sound, damage, score and Box3D blast follow. Depth, rate and queue are capped.
//   * Breakable cover: non-explosive destroyables whose model / placeholder name is cover (crates, tables, stalls ...)
//     get their Health (data +114h, the threshold Destroyable_Objects_AI_Ctrl take_a_hit compares DamageTaken with)
//     scaled down at run time (no notify: never in saves; re-applied after the pool re-initialises a resource). When one
//     dies (IsDead +144h) it bursts into Box3D debris pieces and every enemy crouched within expose_radius is made to
//     stand (command_crouch_out 1054 to its character_animation_ctrl).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include "../../dll/src/gp_glue.h"
#include "../../physics/code/phys_api.h"
#include "mh_logic.h"
#include "mh_draw.h"
#include "../../physics/code/phys_logic.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))
#define WRF(a, v) (*(volatile float*)(uintptr_t)(a) = (float)(v))

// ================================================================================================ game addresses
static const DWORD A_CUR_NODE = 0xA3B75C, A_CUR_2 = 0xA3B760, A_CUR_3 = 0xA3B764, A_QUADTREE = 0xA3B578,
                   A_LIST_ENTITY = 0xA3B8A8, A_PLAYER_HOLDER = 0xA3B590, A_GAME_DT = 0xA3DCC4, A_ASSETMGR = 0xA3D7C4,
                   A_SCENE = 0xA3DCBC;
static DWORD P_MH_CMD = 0x86A340, P_MH_QUERY = 0x486950, P_MH_LISTFREE = 0x8718D0, P_MH_GETPOS = 0x88CA80,
             P_MH_SETWPOS = 0x88E600, P_MH_MODELNAME = 0x884350, P_MH_PIVCOLL = 0x884860, P_MH_WORLDMAT = 0x4842C0, P_MH_SETFLAGS = 0x500BE0;
static DWORD R_MH_CAR = 0x806860;   // vehicle_ai explosion cdecl(thread, 0), [0xA3B75C] = the vehicle_ai node
bool car_blasts_explosions();   // [car_physics] (physics/code/car_game.cpp): it blasts car explosions itself
static DWORD R_MH_DOHIT = 0x86A340; // do_explosion's take_a_hit: Node::SendCommand thiscall(entity, 215, args) ret 8
static DWORD R_MH_QUERY = 0x486950; // do_explosion's ExplosiveRange quadtree query thiscall(quadtree, &query) ret 4 (explosive destroyables only)
static DWORD R_MH_BLUR = 0x45D5E0;  // GfxInternal::Render's frame-end motion blur thiscall(renderer [0xA39F14]), after the world layers
// script class handlers (class object +60h, REF MH_CLS_*)
static const DWORD H_DO = 0x6691A0, H_DOPH = 0x67BD80, H_VROOT = 0x824A70, H_VAI = 0x803A50, H_ROOT = 0x5C3060,
                   H_SENTRY = 0x5C90F0, H_SHELL = 0x843D10;
static const DWORD VT_MODEL = 0x9CCE48, VT_VCLOUD = 0x9D05A0, VT_OBOX = 0x9D02CC;
// Destroyable_Object script data (class registry offsets; REF MH_DO_*)
enum : DWORD {
    DO_SUBTYPE = 0x18, DO_OWNER = 0x20, DO_LOCKED = 0x24, DO_PLACEHOLDER = 0x28, DO_AICTRL = 0x30, DO_IMMUNE = 0x64,
    DO_SHOOTABLE = 0x78, DO_EXPLODEABLE = 0x7C, DO_MOVEABLE = 0x80, DO_BREAK2 = 0x88, DO_HASREPL = 0x98,
    DO_DMG_BALL = 0xA0, DO_DMG_IMPACT = 0xA4, DO_DMG_MELEE = 0xA8, DO_DAMAGE = 0xBC, DO_MASS = 0x110, DO_HEALTH = 0x114,
    DO_EXPL_RANGE = 0x118, DO_EXPL_DAMAGE = 0x11C, DO_DAMAGED_MESH = 0x12C, DO_DEBRIS = 0x130, DO_DEFAULT_MESH = 0x134,
    DO_CURRENT_MESH = 0x138, DO_COM = 0x13C, DO_REPLACEMENT = 0x140, DO_ISDEAD = 0x144
};
enum : DWORD { VR_AICTRL = 0x4C, VA_OWNER = 0x28, VA_HEALTH = 0xB0, VA_DAMAGE = 0xB4, VA_DESTROYED = 0xC8 };
enum : DWORD { CR_ANIM = 0x50, CR_AI = 0x58, SE_HEALTH = 0x2C, SE_CROUCHING = 0x120 };
static const int CMD_TAKE_A_HIT = 215, CMD_CROUCH_OUT = 1054, MASK_EXPLOSION = 0x440;

static const char* MH_REFS[] = {"F_CMD", "F_QUERY", "F_LISTFREE", "F_GETPOS", "SV_CLASSOBJ", "MH_CLS_DO", "MH_CLS_DOPH",
    "MH_CLS_VROOT", "MH_CLS_VAI", "MH_DO_EXPLODEABLE", "MH_DO_ISDEAD", "MH_DO_RANGE", "MH_DO_HEALTH",
    "MH_VAI_DESTROYED", "PH_REGISTRY", "PH_REG_VEC", "PH_GAME_DT", "SL_PLAYER", "PH_F_MODELNAME", "PH_F_PIVCOLL",
    "PH_F_WORLDMAT", "PH_PIVNODE", "PH_VT_MODEL", "PH_VT_VCLOUD", "PH_VT_OBOX", "PH_VC_VERTS", "PH_OBOX_QUAT", "MT_DEVICE", "F_SETFLAGS", "PH_NODE_OFF"};

// ================================================================================================ config
static mh::Cfg g_c;
static char g_cover[512] = "crate,box,table,stall,stand,cart,bench,chair,stool,sofa,couch,desk,shelf,cabinet,pallet,"
                           "barrel,bin,dumpster,kiosk,fruit,melon,basket,sack,cooler,keyboard,monitor,computer,tv,crt";
static char g_cover_x[256] = "sign,pole,phone,hydrant,glass,light,lamp,window,door,fence,toilet,bathtub";
static bool g_dbg_survey = false; static float g_survey_radius = 60.0f, g_survey_seconds = 0.0f;
static int  g_ignite_key = gp::KEY_NONE, g_survey_key = gp::KEY_NONE, g_hit_key = gp::KEY_NONE;
static int  g_place_key = gp::KEY_NONE; static bool g_place_req = false;   // test copy: place on a key (the title level already ticks)
static char g_place[128] = ""; static int g_place_max = 3; static float g_place_dist = 6.0f, g_place_gap = 3.5f, g_place_delay = 3.0f;
static bool g_place_cover = false; static float g_cover_place_dist = 1.0f;
static bool g_dbg_shots = false, g_hide_wreck = true;
static char g_place_ph[128] = "";
static float g_log_seconds = 5.0f;

void mprops_load_config() {
    const gp::Ini& I = gp_ini();
    mh::Cfg c;
    c.chain = I.geti("mayhem", "chain_reactions", 0) != 0;
    c.chain_radius = I.getf("mayhem", "chain_radius", c.chain_radius);
    c.delay_min = I.getf("mayhem", "chain_delay_min", c.delay_min);
    c.delay_max = I.getf("mayhem", "chain_delay_max", c.delay_max);
    c.max_per_second = I.getf("mayhem", "chain_max_per_second", c.max_per_second);
    c.max_per_frame = I.geti("mayhem", "chain_max_per_frame", c.max_per_frame);
    c.max_depth = I.geti("mayhem", "chain_max_depth", c.max_depth);
    c.chain_damage = I.getf("mayhem", "chain_damage", c.chain_damage);
    c.chain_cars = I.geti("mayhem", "chain_cars", 0) != 0;   // default 0 (a chained car blew up next to two DEA buddies in MM055)
    c.car_blast_radius = I.getf("mayhem", "car_blast_radius", c.car_blast_radius);
    c.car_blast_impulse = I.getf("mayhem", "car_blast_impulse", c.car_blast_impulse);
    c.cover = I.geti("mayhem", "breakable_cover", 0) != 0;
    c.cover_health_scale = I.getf("mayhem", "cover_health_scale", c.cover_health_scale);
    c.debris_max = I.geti("mayhem", "debris_max", c.debris_max);
    c.debris_life = I.getf("mayhem", "debris_life", c.debris_life);
    c.debris_pieces = I.geti("mayhem", "debris_pieces", c.debris_pieces);
    c.debris_impulse = I.getf("mayhem", "debris_impulse", c.debris_impulse);
    c.debris_style = I.geti("mayhem", "debris_style", c.debris_style);
    c.debris_size = I.getf("mayhem", "debris_size", c.debris_size);
    c.debris_bits = I.geti("mayhem", "debris_bits", c.debris_bits);
    c.debris_dust = I.geti("mayhem", "debris_dust", c.debris_dust);
    c.expose_radius = I.getf("mayhem", "expose_radius", c.expose_radius);
    c.cover_blast_radius = I.getf("mayhem", "cover_blast_radius", c.cover_blast_radius);
    c.clamp();
    g_c = c;
    if (const char* v = I.get("mayhem", "cover_props")) { strncpy(g_cover, v, sizeof g_cover - 1); g_cover[sizeof g_cover - 1] = 0; }
    if (const char* v = I.get("mayhem", "cover_exclude")) { strncpy(g_cover_x, v, sizeof g_cover_x - 1); g_cover_x[sizeof g_cover_x - 1] = 0; }
    g_log_seconds = I.getf("mayhem", "log_seconds", 5.0f);
    g_hide_wreck = I.geti("mayhem", "cover_hide_wreck", 1) != 0;
    // test copy only
    g_dbg_survey = I.geti("mayhem", "debug_survey", 0) != 0;
    g_survey_radius = I.getf("mayhem", "debug_survey_radius", 60.0f);
    g_survey_seconds = I.getf("mayhem", "debug_survey_seconds", 0.0f);
    g_survey_key = gp_key_cfg("mayhem", "debug_survey_key", "NONE");
    g_ignite_key = gp_key_cfg("mayhem", "debug_ignite_key", "NONE");
    g_hit_key = gp_key_cfg("mayhem", "debug_hit_key", "NONE");
    if (const char* v = I.get("mayhem", "debug_place")) { strncpy(g_place, v, sizeof g_place - 1); g_place[sizeof g_place - 1] = 0; }
    g_place_max = I.geti("mayhem", "debug_place_max", 3);
    g_place_dist = I.getf("mayhem", "debug_place_dist", 6.0f);
    g_place_gap = I.getf("mayhem", "debug_place_gap", 3.5f);
    g_place_delay = I.getf("mayhem", "debug_place_delay", 3.0f);
    g_place_key = gp_key_cfg("mayhem", "debug_place_key", "NONE");
    g_place_cover = I.geti("mayhem", "debug_place_cover", 0) != 0;
    if (const char* v = I.get("mayhem", "debug_place_ph")) { strncpy(g_place_ph, v, sizeof g_place_ph - 1); g_place_ph[sizeof g_place_ph - 1] = 0; }
    g_cover_place_dist = I.getf("mayhem", "debug_cover_dist", 1.0f);
    g_dbg_shots = I.geti("mayhem", "debug_shots", 0) != 0;
}
static bool debug_wanted() { return g_dbg_survey || g_ignite_key >= 0 || g_survey_key >= 0 || g_hit_key >= 0 || g_place[0] || g_place_cover || g_place_ph[0]; }
bool mprops_enabled() { return g_c.chain || g_c.cover || debug_wanted(); }

// ================================================================================================ engine helpers
typedef char(__fastcall* Cmd_t)(DWORD node, void* edx, int id, void* args);      // Node::SendCommand, ret 8
typedef void(__fastcall* Query_t)(DWORD qt, void* edx, void* q);                   // quadtree query, ret 4
typedef void(__fastcall* Free_t)(DWORD type, void* edx, DWORD* plist);             // list release, ret 4
typedef void(__fastcall* Vec_t)(DWORD node, void* edx, const float* v);           // ret 4
typedef void(__fastcall* Mat_t)(DWORD node, void* edx, float* out16);             // ret 4
typedef const char*(__fastcall* Name_t)(DWORD model, void* edx);                  // ret 0
typedef DWORD(__fastcall* PivColl_t)(DWORD model, void* edx, int pivot);          // ret 4
typedef DWORD(__cdecl* Script2_t)(DWORD thread, DWORD arg);

struct Cur { DWORD a, b, c; };
static Cur cur_save() { Cur k = {0, 0, 0}; __try { k.a = RD32(A_CUR_NODE); k.b = RD32(A_CUR_2); k.c = RD32(A_CUR_3); } __except (EXCEPTION_EXECUTE_HANDLER) {} return k; }
static void cur_restore(const Cur& k) {
    __try { if (RD32(A_CUR_NODE) != k.a) WR32(A_CUR_NODE, k.a); if (RD32(A_CUR_2) != k.b) WR32(A_CUR_2, k.b); if (RD32(A_CUR_3) != k.c) WR32(A_CUR_3, k.c); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static DWORD sdata(DWORD n) { __try { return n ? RD32(n + 0x18) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD cls_of(DWORD n) {   // node +4 type, +28h script class object, +60h handler (REF SV_CLASSOBJ)
    __try { if (!n) return 0; DWORD t = RD32(n + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD rd(DWORD a) { __try { return a ? RD32(a) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static float rdf(DWORD a) { __try { return a ? RDF(a) : 0.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; } }
static bool alive(DWORD node) {   // the entity registry slot still holds the node (REF PH_REGISTRY / PH_REG_VEC)
    __try {
        if (!node) return false;
        DWORD am = RD32(A_ASSETMGR); if (!am) return false;
        DWORD v = RD32(node + 0x14) >> 8;
        int k = (int)((v >> 20) & 7) - 1; DWORD slot = v & 0x000FFFFF;
        if (k < 0 || k > 6) return false;
        DWORD vec = am + 0x130 + (DWORD)k * 0x10;
        if (slot >= RD32(vec + 4)) return false;
        return RD32(RD32(vec) + slot * 4) == node;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool get_pos(DWORD node, float* p) {
    if (!node) return false;
    float v[3] = {0, 0, 0}; bool ok = true;
    Cur k = cur_save();
    __try { ((Vec_t)(uintptr_t)P_MH_GETPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    if (!ok || !(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;
    p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
    return true;
}
static bool world_matrix(DWORD node, float m[16]) {
    if (!node) return false;
    __try { ((Mat_t)(uintptr_t)P_MH_WORLDMAT)(node, nullptr, m); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return m[12] == m[12] && m[13] == m[13] && m[14] == m[14];
}
static bool set_world_pos(DWORD node, const float* p) {
    float v[3] = {p[0], p[1], p[2]}; bool ok = true;
    Cur k = cur_save();
    __try { ((Vec_t)(uintptr_t)P_MH_SETWPOS)(node, nullptr, v); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok;
}
static bool send_cmd(DWORD node, int id, void* args) {
    if (!node) return false;
    bool ok = true;
    Cur k = cur_save();
    __try { ((Cmd_t)(uintptr_t)P_MH_CMD)(node, nullptr, id, args); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok;
}
static void model_name(DWORD model, char* out, size_t cap) {   // the getter returns a shared buffer: copy at once
    out[0] = 0;
    __try {
        if (!model || RD32(model) != VT_MODEL) return;
        const char* s = ((Name_t)(uintptr_t)P_MH_MODELNAME)(model, nullptr);
        if (s && !IsBadStringPtrA(s, 256)) { strncpy(out, s, cap - 1); out[cap - 1] = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}
static const char* node_name(DWORD n) {   // Node +4Ch
    __try { const char* s = n ? (const char*)(uintptr_t)RD32(n + 0x4C) : nullptr; if (s && !IsBadStringPtrA(s, 64)) return s; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "";
}
static DWORD ram_root() {   // [0xA3B590] ->+18h ->+84h (REF SL_PLAYER)
    __try { DWORD h = RD32(A_PLAYER_HOLDER); DWORD d = h ? RD32(h + 0x18) : 0; return d ? RD32(d + 0x84) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static float game_dt() { __try { float d = RDF(A_GAME_DT); return d == d && d >= 0.0f && d < 1.0f ? d : 0.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; } }
static DWORD sp_mask(DWORD node) {   // NodeSpatial type mask (node +30h -> +1Ch, low 24 bits; 0x48F390)
    __try { DWORD s = RD32(node + 0x30); return s ? (RD32(s + 0x1C) & 0xFFFFFF) : 0xFFFFFFFF; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
}
// quadtree query {out, pos, radius, mask, mode} (as do_explosion 0x66A600: mask 440h, mode 1); the list is released by query_free
static DWORD query(const float* c, float radius, int mask, int mode) {
    struct { DWORD out; float x, y, z, radius; int mask; int mode; } q = {0, c[0], c[1], c[2], radius, mask, mode};
    DWORD qt = rd(A_QUADTREE); if (!qt) return 0;
    Cur k = cur_save(); bool ok = true;
    __try { ((Query_t)(uintptr_t)P_MH_QUERY)(qt, nullptr, &q); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok ? q.out : 0;
}
static void query_free(DWORD L) {
    if (!L) return;
    Cur k = cur_save();
    __try { DWORD lt = RD32(A_LIST_ENTITY); if (lt) ((Free_t)(uintptr_t)P_MH_LISTFREE)(lt, nullptr, &L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    cur_restore(k);
}
static int list_count(DWORD L) { __try { DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0; return (L && k) ? (int)(RD32(L + 8) / k) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static DWORD list_at(DWORD L, int i) {
    __try { DWORD lt = RD32(A_LIST_ENTITY); DWORD k = lt ? RD32(lt + 0x24) : 0; DWORD a = RD32(L + 4); DWORD el = a ? RD32(a + 4) : 0;
            return el ? RD32(el + (DWORD)i * 4 * k) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD pivot_collision(DWORD model, int i) {
    __try { return ((PivColl_t)(uintptr_t)P_MH_PIVCOLL)(model, nullptr, i); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
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
static int asset_pivots(DWORD model) { __try { DWORD a = RD32(model + 0x50); return a ? (int)RD32(a + 0x34) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
// one collision shape (VertexCloud vertices / OrientedBox corners, as phys_game.cpp's shape_points) through M into world
static int mh_shape_points(DWORD s, const float* M, ph::V3* out, int cap) {
    __try {
        DWORD vt = RD32(s);
        if (vt == VT_VCLOUD) {   // count = byte +2Dh, vertices = +30h & ~3 (REF PH_VC_VERTS)
            int n = *(volatile BYTE*)(uintptr_t)(s + 0x2D);
            const float* v = (const float*)(uintptr_t)(RD32(s + 0x30) & ~3u);
            if (n < 3 || !v) return 0;
            if (n > cap) n = cap;
            for (int i = 0; i < n; ++i) out[i] = ph::tod_point(M, ph::v3(v[3 * i], v[3 * i + 1], v[3 * i + 2]));
            return n;
        }
        if (vt == VT_OBOX && cap >= 8) {   // full size +28h, centre +34h, stored rotation +40h (REF PH_OBOX_QUAT)
            ph::V3 h = ph::v3(RDF(s + 0x28) * 0.5f, RDF(s + 0x2C) * 0.5f, RDF(s + 0x30) * 0.5f), c = ph::v3(RDF(s + 0x34), RDF(s + 0x38), RDF(s + 0x3C));
            ph::Q4 q = ph::true_from_stored(ph::Q4{RDF(s + 0x40), RDF(s + 0x44), RDF(s + 0x48), RDF(s + 0x4C)});
            if (!(h.x > 0 && h.y > 0 && h.z > 0) || h.x > 50 || h.y > 50 || h.z > 50) return 0;
            for (int i = 0; i < 8; ++i)
                out[i] = ph::tod_point(M, ph::add(c, ph::qrot(q, ph::v3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z))));
            return 8;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static float dist3(const float* a, const float* b) { float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z); }

// the Model nodes under a node (first child +44h, next sibling +34h; REF PH_TREE_CHILD / PH_TREE_SIBLING), depth <= 4
static int models_under(DWORD n, DWORD* out, int cap, int depth = 0) {
    int k = 0;
    DWORD c = rd(n + 0x44);
    for (int guard = 0; c && guard < 64 && k < cap; ++guard, c = rd(c + 0x34)) {
        if (rd(c) == VT_MODEL) out[k++] = c;
        if (depth < 3 && k < cap) k += models_under(c, out + k, cap - k, depth + 1);
    }
    return k;
}
static DWORD query(const float* c, float radius, int mask, int mode);
static void query_free(DWORD L);
static int list_count(DWORD L);
static DWORD list_at(DWORD L, int i);
static float dist3(const float* a, const float* b);
// the visible model of a destroyable. Its mesh entities (Current_mesh +138h ...) are plain nodes (vtable 0x9D1F98, a
// no-op Render, run s3); the drawn Model is a separate Model node at the object's place: the nearest visible Model
// (quadtree, plain nodes mask 10h; the query skips hidden nodes, so after the break it is the wreck the game shows)
static DWORD destroyable_model(DWORD n, bool current) {
    DWORD d = sdata(n), m = rd(d + (current ? DO_CURRENT_MESH : DO_DEFAULT_MESH)), out[4];
    if (m && rd(m) == VT_MODEL) return m;
    if (m && models_under(m, out, 4) > 0) return out[0];
    float p[3]; if (!get_pos(n, p)) return 0;
    DWORD L = query(p, 1.6f, 0x10, 1), best = 0; float bd = 1e9f;
    for (int i = 0, c = list_count(L); i < c && i < 64; ++i) {
        DWORD x = list_at(L, i); float q[3];
        if (!x || rd(x) != VT_MODEL || (rd(x + 0x28) & 1) || !get_pos(x, q)) continue;
        float dd = dist3(p, q); if (dd < bd) { bd = dd; best = x; }
    }
    query_free(L);
    return bd < 1.6f ? best : 0;
}
static bool g_own_tex = false;   // REF MH_TEXSETS / MH_D3DTEX: the prop's own texture can be read
static bool g_draw_on = false;
// The prop's visible Model for its texture. destroyable_model() finds none for the Ambush
// crates and barrels (no Model under the mesh entities, none of mask 10h within 1.6 m), so more routes are tried: the
// havoc placeholder's / the object's +60h (the "owner" link the physics scan reads: model +3Ch -> node, node +60h ==
// model), then the nearest visible Model of any mask within 2.5 m. Found while the prop is whole (cover scan, 0.5 s)
// and kept per object; the debris reads it at the break.
static int g_pm_logs = 0;
static bool is_model(DWORD m) { return m && rd(m) == VT_MODEL; }
static DWORD prop_model(DWORD n, int* route) {
    *route = 0;
    DWORD m = destroyable_model(n, false); if (is_model(m)) { *route = 1; return m; }
    m = destroyable_model(n, true); if (is_model(m)) { *route = 2; return m; }
    DWORD ph = rd(sdata(n) + DO_PLACEHOLDER);
    if (ph && is_model(rd(ph + 0x60))) { *route = 3; return rd(ph + 0x60); }
    if (is_model(rd(n + 0x60))) { *route = 4; return rd(n + 0x60); }
    float p[3]; if (!get_pos(n, p)) return 0;
    DWORD L = query(p, 2.5f, 0xFFFFFF, 0), best = 0; float bd = 1e9f;
    for (int i = 0, c = list_count(L); i < c && i < 256; ++i) {
        DWORD x = list_at(L, i); float q[3];
        if (!is_model(x) || (rd(x + 0x28) & 1) || !get_pos(x, q)) continue;
        float dd = dist3(p, q); if (dd < bd) { bd = dd; best = x; }
    }
    query_free(L);
    if (best && bd < 2.5f) { *route = 5; return best; }
    return 0;
}
struct PropModel { DWORD node, guard, model, asset; };
static std::vector<PropModel> g_pmodels;
static void prop_model_learn(DWORD n, const char* name) {
    DWORD guard = rd(sdata(n) + DO_PLACEHOLDER);
    for (auto& e : g_pmodels) if (e.node == n && e.guard == guard) return;
    int route = 0; DWORD m = prop_model(n, &route);
    if (g_pmodels.size() >= 256) g_pmodels.erase(g_pmodels.begin());
    g_pmodels.push_back(PropModel{n, guard, m, m ? rd(m + 0x50) : 0});
    if (g_pm_logs < 40) {
        ++g_pm_logs; char mn[96]; model_name(m, mn, sizeof mn);
        gp_log("mayhem: debris: %s %08X: model %08X '%s' (route %d)\n", name && name[0] ? name : "destroyable", n, m, mn, route);
    }
}
// the model learnt while the prop was whole (still the same Model with the same asset), else a search now
static DWORD prop_model_of(DWORD n) {
    DWORD guard = rd(sdata(n) + DO_PLACEHOLDER);
    for (auto& e : g_pmodels)
        if (e.node == n && e.guard == guard && e.model && is_model(e.model) && rd(e.model + 0x50) == e.asset) return e.model;
    int route = 0; return prop_model(n, &route);
}
// what a found node is
enum What { W_OTHER = 0, W_DESTROYABLE, W_CAR };
static What what_of(DWORD n) { DWORD h = cls_of(n); return h == H_DO ? W_DESTROYABLE : (h == H_VROOT ? W_CAR : W_OTHER); }

// ================================================================================================ state
static bool g_installed = false, g_chain_on = false, g_cover_on = false;
static const PhApi* g_api = nullptr;
static mh::Sched g_sched;
static mh::FiredLog g_fired;
static mh::Rng g_rng;
static float g_t = 0.0f;                   // game time (s) since install (slow motion scaled, paused = frozen)
static volatile LONG g_frame = 0; static LONG g_tick_frame = -1;
static bool g_rewind = false;
static mh::Stats g_st_tick, g_st_fire, g_st_query;
static double g_last_log = 0.0;
static int g_n_blasts = 0, g_n_sched = 0, g_n_fired = 0, g_n_skipped = 0, g_n_car_blasts = 0, g_n_breaks = 0, g_n_exposed = 0;
static int g_n_scaled = 0, g_n_debris = 0, g_n_held = 0;
static bool g_survey_req = false, g_ignite_req = false, g_hit_req = false;
static double g_ctl_since = 0.0, g_last_survey = -1e9;

// blasts reported by the hooks (inside the game's script call): handled in the tick
struct Blast { int kind; DWORD node; float pos[3]; DWORD owner; bool phys_done; };
enum { BL_SHELL = 1, BL_DESTROYABLE = 2, BL_CAR = 3 };
static Blast g_blasts[32]; static volatile LONG g_nblasts = 0;
static void push_blast(int kind, DWORD node, const float* pos, DWORD owner, bool phys_done = false) {
    LONG i = g_nblasts;
    if (i >= 32) return;
    for (LONG j = 0; j < i; ++j)   // the same destroyable reported by PH_DESTROY and by MH_DO_QUERY: once
        if (g_blasts[j].node == node && g_blasts[j].kind == kind) { g_blasts[j].phys_done |= phys_done; return; }
    g_blasts[i].kind = kind; g_blasts[i].node = node; g_blasts[i].owner = owner; g_blasts[i].phys_done = phys_done;
    g_blasts[i].pos[0] = pos[0]; g_blasts[i].pos[1] = pos[1]; g_blasts[i].pos[2] = pos[2];
    g_nblasts = i + 1;
}
static void mark(const char* what) { if (g_dbg_shots) gp_log("debug keys: mark shot %s\n", what); }
struct Exploded { DWORD node, guard; float t; };
static std::vector<Exploded> g_exploded;
static bool exploded(DWORD n, DWORD guard) {
    for (auto& e : g_exploded) if (e.node == n && e.guard == guard) return true;
    return false;
}
static bool g_burst_done = false;
static void mh_cover_blast(const Blast& b);
static void blast_shards(const Blast& b);

// ================================================================================================ hooks into [physics]
// called by physics/code/phys_game.cpp right after the game's own blast (node = the shell / the destroyable)
void mprops_on_blast(int kind, DWORD node, const float* pos) {
    if (!g_installed || !pos) return;
    DWORD owner = 0, d = sdata(node);
    if (kind == BL_DESTROYABLE) owner = rd(d + 0x20);      // DestructorOwner
    else if (kind == BL_SHELL) owner = rd(d + 0x24);       // the shell's owner (as the Suave dynamite sets it)
    push_blast(kind, node, pos, owner, true);              // the physics wrapper threw the props itself
}
// MH_CAR_A / MH_CAR_B: vehicle_ai's explosion cdecl(thread, 0) 0x806860, [0xA3B75C] = the vehicle_ai node
static bool car_pos(DWORD vai, float* p) {   // the vehicle_ai node or its first parent with a position
    DWORD n = vai;
    for (int up = 0; up < 5 && n; ++up) {
        if (get_pos(n, p) && (fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2])) > 0.01f) return true;
        n = rd(n + 0x40);
    }
    return false;
}
static DWORD __cdecl MhCarWrap(DWORD thread, DWORD zero) {
    DWORD vai = rd(A_CUR_NODE);
    float p[3] = {0, 0, 0};
    bool ok = g_installed && vai && car_pos(vai, p);
    DWORD owner = ok ? rd(sdata(vai) + VA_OWNER) : 0;
    DWORD r = ((Script2_t)(uintptr_t)R_MH_CAR)(thread, zero);
    if (ok) {
        push_blast(BL_CAR, vai, p, owner);
        ++g_n_car_blasts;
    }
    return r;
}

// MH_DO_QUERY: do_explosion 0x66A600's ExplosiveRange query (IsExplodeable branch; [0xA3B75C] = the exploding object):
// every explosive destroyable that blows up passes here once - also the ones whose death never runs
// command_destroy_object's explosion call (PH_DESTROY), e.g. the Downtown burrito wagon (run s2)
static void __fastcall MhQueryWrap(DWORD qt, void* edx, void* q) {
    ((void(__fastcall*)(DWORD, void*, void*))(uintptr_t)R_MH_QUERY)(qt, edx, q);
    if (!g_installed || !q) return;
    float p[3];
    __try { const float* f = (const float*)((BYTE*)q + 4); p[0] = f[0]; p[1] = f[1]; p[2] = f[2]; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    DWORD node = rd(A_CUR_NODE); float np[3];
    if (!node || cls_of(node) != H_DO || !get_pos(node, np) || dist3(np, p) > 1.0f) {   // not the object: the destroyable at the centre
        node = 0;
        DWORD L = query(p, 1.0f, MASK_EXPLOSION, 1); float bd = 1e9f;
        for (int i = 0, c = list_count(L); i < c && i < 64; ++i) {
            DWORD x = list_at(L, i); float xp[3];
            if (!x || cls_of(x) != H_DO || !rd(sdata(x) + DO_EXPLODEABLE) || !get_pos(x, xp)) continue;
            float d = dist3(xp, p); if (d < bd) { bd = d; node = x; }
        }
        query_free(L);
        if (!node) { gp_log("mayhem: a destroyable blast at (%.2f, %.2f, %.2f) without its object\n", p[0], p[1], p[2]); node = 0xFFFFFFF0u; }
    }
    push_blast(BL_DESTROYABLE, node, p, node == 0xFFFFFFF0u ? 0 : rd(sdata(node) + DO_OWNER), false);
}

// ================================================================================================ chain reactions
struct Hit { float dmg; DWORD owner, inflictor, collvol; float imp[3]; float pos[3]; int type; };   // take_a_hit args
static_assert(sizeof(Hit) == 44, "take_a_hit argument block");

static bool destroyable_target(DWORD n, bool want_explosive, DWORD* guard) {
    DWORD d = sdata(n); if (!d) return false;
    __try {
        if (RD32(d + DO_ISDEAD) || RD32(d + DO_LOCKED) || RD32(d + DO_IMMUNE)) return false;
        if (want_explosive && exploded(n, RD32(d + DO_PLACEHOLDER))) return false;
        if (want_explosive && !RD32(d + DO_EXPLODEABLE)) return false;
        *guard = RD32(d + DO_PLACEHOLDER);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool car_target(DWORD root, DWORD* vai_out) {
    DWORD vai = rd(sdata(root) + VR_AICTRL);
    if (!vai || cls_of(vai) != H_VAI) return false;
    DWORD d = sdata(vai); if (!d) return false;
    if (rd(d + VA_DESTROYED)) return false;
    *vai_out = vai;
    return true;
}
// A chain link never goes off next to a buddy / escort / friend (faction 10 DEA, 18 Mexican buddies and their
// provoked copies 29 / 37): in an MM055 test a chained car exploded and killed two DEA buddies at once; a mission whose
// buddy or escort dies fails. Vanilla blasts are untouched (only the links this feature adds are held back).
static bool friend_near(const float* p, float r, DWORD* who) {
    DWORD L = query(p, r, 0x40, 0); if (!L) return false;
    bool hit = false; DWORD ram = ram_root();
    for (int i = 0, n = list_count(L); i < n && i < 128 && !hit; ++i) {
        DWORD c = list_at(L, i);
        if (!c || c == ram || cls_of(c) != H_ROOT) continue;
        __try {
            DWORD rdat = sdata(c); DWORD ai = rdat ? RD32(rdat + CR_AI) : 0; DWORD d = sdata(ai);
            if (!d || cls_of(ai) != H_SENTRY || (int)RD32(d + 0x1D4) == 28) continue;
            int fac = (int)RD32(rdat + 0x18), sbt = (int)RD32(d + 0x28);
            if (RD32(d + 0xB8) || sbt == 4 || fac == 10 || fac == 18 || fac == 29 || fac == 37) { hit = true; *who = c; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    query_free(L);
    return hit;
}
static void schedule_around(const Blast& b, int depth, DWORD owner) {
    double q0 = gp_now();
    DWORD L = query(b.pos, g_c.chain_radius, MASK_EXPLOSION, 1);
    g_st_query.add((gp_now() - q0) * 1000.0);
    if (!L) return;
    int n = list_count(L);
    float rp[3] = {0, 0, 0}; DWORD ram = ram_root(); bool have_ram = ram && get_pos(ram, rp);
    for (int i = 0; i < n && i < 256; ++i) {
        DWORD t = list_at(L, i);
        if (!t || t == b.node) continue;
        What w = what_of(t);
        mh::Pending p; DWORD guard = 0, vai = 0;
        if (w == W_DESTROYABLE) {
            if (!destroyable_target(t, true, &guard)) continue;
            p.kind = mh::K_DESTROYABLE;
        } else if (w == W_CAR && g_c.chain_cars) {
            if (!car_target(t, &vai) || vai == b.node) continue;
            p.kind = mh::K_CAR;
        } else continue;
        float tp[3]; if (!get_pos(t, tp)) continue;
        if (p.kind == mh::K_CAR && have_ram && dist3(tp, rp) < 3.5f) continue;   // never the car Ram sits in
        float d = dist3(tp, b.pos);
        if (d > g_c.chain_radius) continue;
        DWORD fr = 0;
        if (friend_near(tp, p.kind == mh::K_CAR ? g_c.car_blast_radius + 2.0f : 6.0f, &fr)) {
            gp_log("mayhem: chain: %s %08X not set off: a buddy / escort / friend (character %08X) is within its blast\n", p.kind == mh::K_CAR ? "car" : "destroyable", t, fr);
            continue;
        }
        p.node = t; p.guard = guard; p.depth = depth + 1; p.owner = owner; p.source = b.node;
        p.due = g_t + mh::chain_delay(g_c, d, g_rng.f01());
        for (int a = 0; a < 3; ++a) { p.pos[a] = tp[a]; p.src[a] = b.pos[a]; }
        if (g_sched.add(g_c, p)) {
            ++g_n_sched;
            char nm[96] = ""; if (p.kind == mh::K_DESTROYABLE) model_name(rd(sdata(t) + DO_CURRENT_MESH), nm, sizeof nm);
            gp_log("mayhem: chain: %s %08X %s %.1f m from the %s blast, depth %d, goes off in %.2f s\n",
                   p.kind == mh::K_CAR ? "car" : "destroyable", t, nm[0] ? nm : node_name(t), d,
                   b.kind == BL_SHELL ? "shell" : (b.kind == BL_CAR ? "car" : "destroyable"), p.depth, p.due - g_t);
        }
    }
    query_free(L);
}
static void handle_blast(const Blast& b) {
    ++g_n_blasts;
    if (b.kind == BL_DESTROYABLE && b.node != 0xFFFFFFF0u) {
        g_exploded.erase(std::remove_if(g_exploded.begin(), g_exploded.end(), [](const Exploded& e) { return g_t - e.t > 60.0f || g_t < e.t - 1.0f; }), g_exploded.end());
        if (g_exploded.size() < 256) g_exploded.push_back(Exploded{b.node, rd(sdata(b.node) + DO_PLACEHOLDER), g_t});
    }
    if (g_dbg_shots && !g_burst_done) { g_burst_done = true; gp_log("debug keys: mark burst mh_cv 110 30\n"); }   // test copy: the clip of the first blast
    int depth = 0; DWORD owner = b.owner;
    { int dd; uint32_t ow; if (g_fired.find(b.node, g_t, &dd, &ow)) { depth = dd; if (!owner) owner = ow; } }
    gp_log("mayhem: %s blast %08X at (%.2f, %.2f, %.2f), depth %d%s\n", b.kind == BL_SHELL ? "shell" : (b.kind == BL_CAR ? "car" : "destroyable"),
           b.node, b.pos[0], b.pos[1], b.pos[2], depth, g_chain_on ? "" : " (chain reactions off)");
    // a car blast: [car_physics] (when on) already blasts every car explosion it sees, cars and wrecks included
    // (car_game.cpp -> phys_blast_props -> car_blast): only one blast per car, never two
    if (b.kind == BL_CAR && g_c.car_blast_radius > 0.0f && !car_blasts_explosions()) phys_blast(b.pos, g_c.car_blast_radius, g_c.car_blast_impulse, "car");
    if (b.kind == BL_DESTROYABLE && !b.phys_done) phys_destroy_blast(b.pos);   // PH_DESTROY did not see this one
    if (g_chain_on) schedule_around(b, depth, owner);
    if (g_cover_on) mh_cover_blast(b);
    blast_shards(b);
}
static bool fire(const mh::Pending& p) {
    double f0 = gp_now();
    bool ok = false; DWORD vai = 0, guard = 0;
    if (!alive(p.node)) { gp_log("mayhem: chain: %08X is gone, skipped\n", p.node); ++g_n_skipped; return false; }
    float tp[3];
    if (!get_pos(p.node, tp) || dist3(tp, p.pos) > 60.0f) { gp_log("mayhem: chain: %08X is far from where it was (reused?), skipped\n", p.node); ++g_n_skipped; return false; }
    if (p.kind == mh::K_DESTROYABLE) {
        if (cls_of(p.node) != H_DO || !destroyable_target(p.node, true, &guard) || guard != p.guard) {
            gp_log("mayhem: chain: destroyable %08X already went off / changed, skipped\n", p.node); ++g_n_skipped; return false;
        }
    } else {
        if (cls_of(p.node) != H_VROOT || !car_target(p.node, &vai)) { gp_log("mayhem: chain: car %08X already destroyed, skipped\n", p.node); ++g_n_skipped; return false; }
    }
    Hit h; memset(&h, 0, sizeof h);
    h.dmg = g_c.chain_damage;
    h.owner = alive(p.owner) ? p.owner : 0;
    h.inflictor = (p.source && alive(p.source)) ? p.source : p.node;   // the blast that set it off (pooled nodes stay registered)
    h.collvol = p.node;
    float dir[3] = {tp[0] - p.src[0], tp[1] - p.src[1] + 0.5f, tp[2] - p.src[2]};
    float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]); if (l < 1e-3f) { dir[1] = 1; l = 1; }
    for (int a = 0; a < 3; ++a) { h.imp[a] = dir[a] / l * 10.0f; h.pos[a] = p.src[a]; }
    h.type = 1;               // explosion (do_explosion's type)
    ok = send_cmd(p.node, CMD_TAKE_A_HIT, &h);
    g_fired.add(p.node, p.depth, g_t, p.owner);
    if (vai) g_fired.add(vai, p.depth, g_t, p.owner);
    ++g_n_fired;
    g_st_fire.add((gp_now() - f0) * 1000.0);
    gp_log("mayhem: chain: %s %08X set off (depth %d, take_a_hit %.0f)%s\n", p.kind == mh::K_CAR ? "car" : "destroyable", p.node, p.depth, h.dmg, ok ? "" : " FAILED");
    mark("mh_chain");
    return ok;
}

// MH_DO_HIT: do_explosion's take_a_hit to each entity in ExplosiveRange. An explosive destroyable is not hit at once
// (vanilla: the whole cluster goes off in the same frame) but scheduled like every other link of the chain.
static bool intercept_hit(DWORD node, const Hit* a) {
    DWORD guard = 0;
    if (cls_of(node) != H_DO || !destroyable_target(node, true, &guard)) return false;
    if (g_sched.pending(node)) return true;   // already scheduled by the chain: it goes off anyway
    DWORD src = 0; float sp[3] = {0, 0, 0};
    __try { src = a->inflictor; sp[0] = a->pos[0]; sp[1] = a->pos[1]; sp[2] = a->pos[2]; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float tp[3]; if (!get_pos(node, tp)) return false;
    int depth = 0; uint32_t owner = 0;
    if (!g_fired.find(src, g_t, &depth, &owner)) { depth = 0; owner = 0; }
    DWORD own = 0; __try { own = a->owner; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    mh::Pending p; p.node = node; p.guard = guard; p.kind = mh::K_DESTROYABLE; p.depth = depth + 1; p.owner = own ? own : owner; p.source = src;
    float d = dist3(tp, sp);
    p.due = g_t + mh::chain_delay(g_c, d, g_rng.f01());
    for (int i = 0; i < 3; ++i) { p.pos[i] = tp[i]; p.src[i] = sp[i]; }
    if (!g_sched.add(g_c, p)) return false;   // too deep / queue full: the game's own instant hit
    ++g_n_sched; ++g_n_held;
    return true;
}
static char __fastcall MhDoHitWrap(DWORD node, void* edx, int id, void* args) {
    if (g_installed && g_chain_on && id == CMD_TAKE_A_HIT && args && intercept_hit(node, (const Hit*)args)) return 1;
    return ((Cmd_t)(uintptr_t)R_MH_DOHIT)(node, edx, id, args);
}

// ================================================================================================ breakable cover
struct Cover { DWORD node, guard; float health_org, health_set; bool dead, broke, have_box; float dead_t; int hidden; char name[64]; float pos[3]; float bp[3], bq[4]; mh::Box box; };
static std::vector<Cover> g_cover_list;
static double g_last_cover_scan = -1e9;
// the cover prop's model (Current_mesh / Default_mesh) name or its placeholder's name
static void cover_name(DWORD n, char* out, size_t cap) {
    DWORD d = sdata(n);
    model_name(destroyable_model(n, true), out, cap);
    if (!out[0]) model_name(destroyable_model(n, false), out, cap);
    const char* s = node_name(rd(d + DO_PLACEHOLDER));   // the placeholder's name too ("Crate_Small", "Metal Chair" ...)
    if (s[0]) { size_t k = strlen(out); if (k + 2 < cap) { if (k) { out[k++] = ' '; out[k] = 0; } strncat(out, s, cap - k - 1); } }
}
static bool is_cover(DWORD n, char* name, size_t cap) {
    DWORD d = sdata(n); if (!d) return false;
    if (rd(d + DO_EXPLODEABLE)) return false;
    cover_name(n, name, cap);
    return mh::name_match(name, g_cover) && !mh::name_match(name, g_cover_x);
}
static Cover* cover_find(DWORD n) { for (auto& c : g_cover_list) if (c.node == n) return &c; return nullptr; }
static void scale_health(Cover& c) {
    DWORD d = sdata(c.node); if (!d) return;
    __try {
        float h = RDF(d + DO_HEALTH);
        if (h > 0.0f && h != c.health_set) {   // first sight, or the pool re-initialised it from the dictionary
            c.health_org = h; c.health_set = h * g_c.cover_health_scale;
            if (c.health_set < 0.5f) c.health_set = 0.5f;
            WRF(d + DO_HEALTH, c.health_set);
            ++g_n_scaled;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void mh_debris_burst(DWORD destroyable, const float* pos, const float* push_dir, const Cover* c);   // below
static bool cover_box_of(DWORD destroyable, float* p, float* q, mh::Box* box);   // below
static void expose_enemies(const float* c) {
    if (!(g_c.expose_radius > 0.0f)) return;
    DWORD L = query(c, g_c.expose_radius, 0x40, 0);   // characters (the Tornado's query)
    if (!L) { gp_log("mayhem: cover: no character within %.1f m of the break\n", g_c.expose_radius); return; }
    DWORD ram = ram_root(); int seen = 0, exposed = 0;
    for (int i = 0, n = list_count(L); i < n && i < 32; ++i) {
        DWORD r = list_at(L, i);
        if (!r || r == ram || cls_of(r) != H_ROOT) continue;
        ++seen;
        DWORD rdat = sdata(r); DWORD ai = rd(rdat + CR_AI), anim = rd(rdat + CR_ANIM);
        if (!ai || cls_of(ai) != H_SENTRY) continue;
        DWORD sd = sdata(ai);
        if (!(rdf(sd + SE_HEALTH) > 0.0f)) continue;
        bool crouching = rd(sd + SE_CROUCHING) != 0;
        bool ok = anim ? send_cmd(anim, CMD_CROUCH_OUT, nullptr) : false;
        ++g_n_exposed; ++exposed;
        gp_log("mayhem: cover: enemy %08X behind it exposed (was %s; crouch_out %s)\n", r, crouching ? "crouching" : "standing", ok ? "sent" : "FAILED");
    }
    query_free(L);
    if (!exposed) gp_log("mayhem: cover: no live enemy within %.1f m of the break (%d character(s) there)\n", g_c.expose_radius, seen);
}
static void cover_broke(Cover& c) {
    c.dead = true; c.broke = true; c.dead_t = g_t; ++g_n_breaks;
    float p[3] = {c.pos[0], c.pos[1], c.pos[2]}; get_pos(c.node, p);
    gp_log("mayhem: cover: %s %08X broke at (%.2f, %.2f, %.2f) (health %.0f -> %.0f)\n", c.name, c.node, p[0], p[1], p[2], c.health_org, c.health_set);
    float rp[3] = {0, 0, 0}; DWORD ram = ram_root(); float dir[3] = {0, 0, 0};
    if (ram && get_pos(ram, rp)) { float l = dist3(p, rp); if (l > 0.1f) for (int a = 0; a < 3; ++a) dir[a] = (p[a] - rp[a]) / l; dir[1] = 0; }
    mh_debris_burst(c.node, p, dir, &c);
    expose_enemies(p);
    static bool burst_cb = false;   // test copy: the clip of the first cover break
    if (g_dbg_shots && !burst_cb) { burst_cb = true; gp_log("debug keys: mark burst mh_cb 90 30\n"); } else mark("mh_cover_break");
}
typedef void(__fastcall* SetFlags_t)(DWORD node, void* edx, int flags);   // Node::SetFlags thunk 0x500BE0 (REF F_SETFLAGS)
static bool hide_node(DWORD n) {   // node flag 1 = hidden: not drawn, no collision (the probe's (flags | flags >> 16) & 1 test)
    if (!n) return false;
    bool ok = true; Cur k = cur_save();
    __try { DWORD fl = RD32(n + 0x28); if (fl & 1) ok = false; else ((SetFlags_t)(uintptr_t)P_MH_SETFLAGS)(n, nullptr, (int)((fl | 1) & 0xFFF)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok;
}
static bool show_node(DWORD n) {
    bool ok = true; Cur k = cur_save();
    __try { DWORD fl = RD32(n + 0x28); if (!(fl & 1)) ok = false; else ((SetFlags_t)(uintptr_t)P_MH_SETFLAGS)(n, nullptr, (int)(fl & 0xFFE)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    cur_restore(k);
    return ok;
}
// models we hid: shown again when the pool gives the destroyable to another placeholder or it is alive again (rewind)
struct Hidden { DWORD model, node, guard; };
static std::vector<Hidden> g_hidden;
static void unhide_check() {
    for (size_t i = g_hidden.size(); i-- > 0;) {
        Hidden& h = g_hidden[i];
        DWORD d = sdata(h.node);
        bool reused = !alive(h.node) || rd(d + DO_PLACEHOLDER) != h.guard || !rd(d + DO_ISDEAD);
        if (!reused) continue;
        bool ok = alive(h.model) && rd(h.model) == VT_MODEL && show_node(h.model);
        gp_log("mayhem: cover: destroyable %08X back in use / alive: model %08X shown again%s\n", h.node, h.model, ok ? "" : " (it was not hidden any more)");
        g_hidden.erase(g_hidden.begin() + (ptrdiff_t)i);
    }
}
// the wreck the game swaps in (Current_mesh = Damaged_mesh in the dying thread 0x66C1C0) is hidden: the pieces replace it
static void hide_wreck(Cover& c) {
    DWORD m[2] = {destroyable_model(c.node, true), destroyable_model(c.node, false)};
    for (int i = 0; i < 2; ++i) {
        if (!m[i] || (i == 1 && m[1] == m[0]) || rd(m[i]) != VT_MODEL || !hide_node(m[i])) continue;
        ++c.hidden;
        if (g_hidden.size() < 256) g_hidden.push_back(Hidden{m[i], c.node, c.guard});
        gp_log("mayhem: cover: %s %08X wreck model %08X hidden\n", c.name, c.node, m[i]);
    }
}
static void cover_tick(const float* rp, double now) {
    // watch the tracked ones every frame (cheap): death = IsDead set
    for (auto& c : g_cover_list) {
        if (c.broke && g_hide_wreck && g_t - c.dead_t < 2.0f) hide_wreck(c);   // the swap may come a few frames later
        if (c.dead) continue;
        DWORD d = sdata(c.node);
        if (!alive(c.node) || cls_of(c.node) != H_DO || rd(d + DO_PLACEHOLDER) != c.guard) { c.dead = true; continue; }
        if (rd(d + DO_ISDEAD)) { cover_broke(c); continue; }
        scale_health(c);
    }
    if (now - g_last_cover_scan < 0.5) return;
    g_last_cover_scan = now;
    unhide_check();
    // forget the far / finished ones, find new ones within 45 m
    for (size_t i = g_cover_list.size(); i-- > 0;) {
        Cover& c = g_cover_list[i];
        if ((c.dead && (!c.broke || g_t - c.dead_t > 2.5f)) || dist3(c.pos, rp) > 60.0f) g_cover_list.erase(g_cover_list.begin() + (ptrdiff_t)i);
    }
    DWORD L = query(rp, 45.0f, MASK_EXPLOSION, 1);
    if (!L) return;
    for (int i = 0, n = list_count(L); i < n && i < 512; ++i) {
        DWORD t = list_at(L, i);
        if (!t || cls_of(t) != H_DO) continue;
        if (g_draw_on && g_c.debris_style == 1 && g_own_tex) { char nm[96]; cover_name(t, nm, sizeof nm); prop_model_learn(t, nm); }
        if (cover_find(t)) continue;
        DWORD guard = 0; if (!destroyable_target(t, false, &guard)) continue;
        Cover c; memset(&c, 0, sizeof c);
        if (!is_cover(t, c.name, sizeof c.name)) continue;
        c.node = t; c.guard = guard; get_pos(t, c.pos);
        c.have_box = cover_box_of(t, c.bp, c.bq, &c.box);
        scale_health(c);
        g_cover_list.push_back(c);
        if (g_cover_list.size() <= 64)
            gp_log("mayhem: cover: %s %08X at (%.1f, %.1f, %.1f): health %.0f -> %.0f\n", c.name, t, c.pos[0], c.pos[1], c.pos[2], c.health_org, c.health_set);
        if (g_cover_list.size() >= 128) break;
    }
    query_free(L);
}
// blasts also chip cover props near them (an extra take_a_hit: the blast's own damage often stops at the barrel's range)
static void mh_cover_blast(const Blast& b) {
    if (!(g_c.cover_blast_radius > 0.0f)) return;
    for (auto& c : g_cover_list) {
        if (c.dead) continue;
        float p[3]; if (!get_pos(c.node, p)) continue;
        float d = dist3(p, b.pos);
        if (d > g_c.cover_blast_radius) continue;
        Hit h; memset(&h, 0, sizeof h);
        h.dmg = (c.health_set > 0.0f ? c.health_set : 50.0f) * (1.5f - d / g_c.cover_blast_radius);
        h.owner = alive(b.owner) ? b.owner : 0; h.inflictor = c.node; h.collvol = c.node; h.type = 1;
        for (int a = 0; a < 3; ++a) { h.pos[a] = b.pos[a]; h.imp[a] = (p[a] - b.pos[a]) / (d > 0.1f ? d : 0.1f) * 10.0f; }
        bool ok = send_cmd(c.node, CMD_TAKE_A_HIT, &h);
        gp_log("mayhem: cover: %s %08X %.1f m from the blast: take_a_hit %.0f%s\n", c.name, c.node, d, h.dmg, ok ? "" : " FAILED");
    }
}

// ================================================================================================ debris (Box3D + mh_draw)
// Each piece is a Box3D body in the [physics] world (it collides with the level mesh, the props and the other pieces,
// blasts and bullets push it) drawn by mh_draw.cpp in the game's world pass. Poses are read once per input tick (after
// the physics step) and interpolated between the last two distinct poses when drawn.
// debris_style 1: small chunks (Box3D bodies), grit (cheap particles, no body) and a dust puff,
// all showing the broken prop's own texture (a patch of it per piece); debris_style 0 = the slats of the first version.
struct Debris {
    int b; float half[3]; int mat; float tint, uv; float born;
    float p0[3], q0[4], p1[3], q1[4]; double t1; bool have;
    void* tex; float u0, v0, side;   // style 1: the prop's texture (one reference per piece) and the patch
};
struct Grit { mh::Bit b; float hx, hy, hz; void* tex; float u0, v0, side; int mat; float tint, born; };
struct Dust { mh::Puff f; float rgb[3]; };
static std::vector<Debris> g_debris;
static std::vector<Grit> g_grit;
static std::vector<Dust> g_dust;
enum { GRIT_MAX = 256 };
static float g_fx_t = 0.0f;   // g_t of the last grit / dust step
static int g_world_gen = -1;
static mh::Stats g_st_draw;
static int g_drawn_last = 0;
static double g_last_draw_log = 0.0;
void mprops_bullet_body(int body, const float* imp, const float* at) {
    if (!g_installed || !g_api || body < 0 || !imp || !at) return;
    for (auto& d : g_debris) if (d.b == body) { g_api->impulse(body, imp, at); return; }
}
static void debris_remove(size_t i) {
    if (g_api && phys_world_gen() == g_world_gen) g_api->body_remove(g_debris[i].b);
    mh_tex_release(g_debris[i].tex);
    g_debris.erase(g_debris.begin() + (ptrdiff_t)i);
}
static void grit_remove(size_t i) { mh_tex_release(g_grit[i].tex); g_grit.erase(g_grit.begin() + (ptrdiff_t)i); }
static void debris_clear() {   // the world went (rescan, level change): the bodies are gone, the texture references dropped
    for (auto& d : g_debris) mh_tex_release(d.tex);
    for (auto& g : g_grit) mh_tex_release(g.tex);
    g_debris.clear(); g_grit.clear(); g_dust.clear();
}
static int material_of(const char* name, bool explosive) {
    if (explosive) {
        if (mh::name_match(name, "fuel,gas,tank,pump,pressure") && !mh::name_match(name, "barrel")) return MH_METAL;
        return g_c.debris_style == 0 ? MH_RED : MH_CHAR;   // style 1: never the flat red of "really large red bricks"
    }
    if (mh::name_match(name, "bin,dumpster,cabinet,locker,metal,monitor,computer,keyboard,tv,crt,steel,tank,can,drum")) return MH_METAL;
    return MH_WOOD;
}
// the look of one burst (style 1)
struct Look {
    int shape;            // mh::SH_SPLINTER / SH_PLATE / SH_LUMP
    float min_len, max_len;
    MhTexInfo ti; bool have_tex;
    float prop_len;       // the prop's longest edge (m): a piece shows side = its length / prop_len of the texture
};
static void pick_patch(const Look& lk, float len, float* u0, float* v0, float* side) {
    *u0 = *v0 = 0.0f; *side = 0.0f;
    if (!lk.have_tex) return;
    float s = len / (lk.prop_len > 0.05f ? lk.prop_len : 0.05f);
    s = s < 0.04f ? 0.04f : (s > 0.3f ? 0.3f : s);
    float cu = g_rng.f01(), cv = g_rng.f01();
    if (lk.ti.ncell > 0) {   // a used cell of the texture (not its empty / transparent part)
        int want = (int)(g_rng.next() % (uint32_t)lk.ti.ncell), k = 0;
        for (int i = 0; i < 256; ++i)
            if (lk.ti.cell[i] && k++ == want) { cu = ((i % 16) + g_rng.f01()) / 16.0f; cv = ((i / 16) + g_rng.f01()) / 16.0f; break; }
    }
    mh::uv_patch(cu, cv, s, u0, v0);
    *side = s;
}
// a world box (centre p, rotation q) burst into pieces: Box3D bodies with the pieces' speeds (lk null = style 0 slats)
static int spawn_pieces(const float* p, const float* q, const mh::Box& box, int n, float burst, const float* dir, float push, int mat, const Look* lk) {
    if (!g_api || !phys_world_ready() || g_c.debris_max <= 0) return 0;
    int gen = phys_world_gen();
    if (gen != g_world_gen) { debris_clear(); g_world_gen = gen; }   // a rescan rebuilt the world: the old pieces are gone
    mh::Piece pc[24];
    float ldir[3] = {0, 0, 0};
    if (dir) { ph::Q4 qi = ph::qconj(ph::Q4{q[0], q[1], q[2], q[3]}); ph::V3 l = ph::qrot(qi, ph::v3(dir[0], dir[1], dir[2])); ldir[0] = l.x; ldir[1] = l.y; ldir[2] = l.z; }
    int k = lk ? mh::make_chunks(box, n, lk->min_len, lk->max_len, lk->shape, burst, ldir, push, g_rng.next(), pc, 24)
               : mh::make_debris(box, n, burst, ldir, push, g_rng.next(), pc, 24);
    ph::Q4 Q = {q[0], q[1], q[2], q[3]};
    int made = 0;
    for (int i = 0; i < k; ++i) {
        while ((int)g_debris.size() >= g_c.debris_max) debris_remove(0);   // oldest first
        if (lk) for (int a = 0; a < 3; ++a) if (pc[i].box.h[a] < 0.012f) pc[i].box.h[a] = 0.012f;   // Box3D: no paper-thin bodies
        ph::V3 c = ph::add(ph::v3(p[0], p[1], p[2]), ph::qrot(Q, ph::v3(pc[i].box.c[0], pc[i].box.c[1], pc[i].box.c[2])));
        PhBodyDesc d; memset(&d, 0, sizeof d);
        d.pos[0] = c.x; d.pos[1] = c.y; d.pos[2] = c.z; d.rot[0] = q[0]; d.rot[1] = q[1]; d.rot[2] = q[2]; d.rot[3] = q[3];
        d.lin_damping = 0.05f; d.ang_damping = 0.25f; d.asleep = 0; d.user = nullptr;
        int b = g_api->body_create(&d);
        if (b < 0) continue;
        const float* h = pc[i].box.h;
        float pts[24];
        for (int j = 0; j < 8; ++j) { pts[3 * j] = (j & 4) ? h[0] : -h[0]; pts[3 * j + 1] = (j & 2) ? h[1] : -h[1]; pts[3 * j + 2] = (j & 1) ? h[2] : -h[2]; }
        if (!g_api->body_add_hull(b, pts, 8, 350.0f, 0.6f, 0.25f) || !g_api->body_finish(b, 0.8f, 30.0f)) { g_api->body_remove(b); continue; }
        float m = g_api->body_mass(b);
        ph::V3 v = ph::qrot(Q, ph::v3(pc[i].vel[0], pc[i].vel[1], pc[i].vel[2]));
        float imp[3] = {v.x * m, v.y * m, v.z * m};
        ph::V3 off = ph::qrot(Q, ph::v3(h[0] * g_rng.range(-0.6f, 0.6f), h[1] * g_rng.range(-0.6f, 0.6f), h[2] * g_rng.range(-0.6f, 0.6f)));
        float at[3] = {c.x + off.x, c.y + off.y, c.z + off.z};   // off-centre: the piece spins
        g_api->impulse(b, imp, at);
        Debris db; memset(&db, 0, sizeof db);
        db.b = b; db.half[0] = h[0]; db.half[1] = h[1]; db.half[2] = h[2]; db.mat = mat;
        db.tint = g_rng.range(0.8f, 1.1f); db.uv = g_rng.f01(); db.born = g_t;
        if (lk) {
            db.tint = g_rng.range(0.85f, 1.05f);
            float len = 2.0f * (std::max)(h[0], (std::max)(h[1], h[2]));
            pick_patch(*lk, len, &db.u0, &db.v0, &db.side);
            if (lk->have_tex) { db.tex = lk->ti.tex; mh_tex_addref(db.tex); }
        }
        g_debris.push_back(db);
        ++made; ++g_n_debris;
    }
    return made;
}
// grit: n tiny bits from inside the box (world centre p, rotation q), flying out and settling on the ground under them
static int spawn_grit(const float* p, const float* q, const mh::Box& box, int n, float burst, const float* dir, float push, int mat, const Look& lk) {
    if (n <= 0 || !g_api || !phys_world_ready()) return 0;
    ph::Q4 Q = {q[0], q[1], q[2], q[3]};
    float ground0 = p[1] - 30.0f;
    {   // the ground under the prop
        float a[3] = {p[0], p[1] + 0.5f, p[2]}, b[3] = {p[0], p[1] - 30.0f, p[2]}, hit[3];
        if (g_api->raycast_static(a, b, hit) == 1) ground0 = hit[1];
    }
    // the ground 1.5 m out in 4 directions (one raycast each, not one per bit): a bit lands on its quadrant's ground
    float gq[4];
    for (int k = 0; k < 4; ++k) {
        float dx = (k & 1) ? 1.06f : -1.06f, dz = (k & 2) ? 1.06f : -1.06f, hit[3];
        float a[3] = {p[0] + dx, p[1] + 0.6f, p[2] + dz}, b[3] = {p[0] + dx, ground0 - 3.0f, p[2] + dz};
        gq[k] = g_api->raycast_static(a, b, hit) == 1 ? hit[1] : ground0;
        if (gq[k] > p[1] + 0.5f) gq[k] = ground0;   // a wall or a step up there: the ground under the prop
    }
    const float G = 9.81f;
    int made = 0;
    for (int i = 0; i < n; ++i) {
        while ((int)g_grit.size() >= GRIT_MAX) grit_remove(0);
        Grit g; memset(&g, 0, sizeof g);
        float l[3]; for (int a = 0; a < 3; ++a) l[a] = box.c[a] + g_rng.range(-0.85f, 0.85f) * box.h[a];
        ph::V3 c = ph::add(ph::v3(p[0], p[1], p[2]), ph::qrot(Q, ph::v3(l[0], l[1], l[2])));
        g.b.p[0] = c.x; g.b.p[1] = c.y; g.b.p[2] = c.z;
        float o[3] = {c.x - p[0], 0.0f, c.z - p[2]};
        float ol = sqrtf(o[0] * o[0] + o[2] * o[2]);
        if (ol < 1e-3f) { float a = g_rng.range(0.0f, 6.2831853f); o[0] = cosf(a); o[2] = sinf(a); ol = 1.0f; }
        float sp = burst * g_rng.range(0.35f, 1.15f);
        g.b.v[0] = o[0] / ol * sp + (dir ? dir[0] * push * g_rng.range(0.4f, 1.2f) : 0.0f);
        g.b.v[2] = o[2] / ol * sp + (dir ? dir[2] * push * g_rng.range(0.4f, 1.2f) : 0.0f);
        g.b.v[1] = burst * g_rng.range(0.3f, 0.95f);
        float t = g_rng.f01(); t *= t;
        g.b.half = (0.008f + 0.03f * t) * g_c.debris_size;
        g.hx = g.b.half; g.hy = g.b.half * g_rng.range(0.4f, 0.9f); g.hz = g.b.half * g_rng.range(0.6f, 1.0f);
        g.b.ax[0] = g_rng.range(-1, 1); g.b.ax[1] = g_rng.range(-1, 1); g.b.ax[2] = g_rng.range(-1, 1);
        float al = sqrtf(g.b.ax[0] * g.b.ax[0] + g.b.ax[1] * g.b.ax[1] + g.b.ax[2] * g.b.ax[2]);
        if (al < 1e-3f) { g.b.ax[0] = 0; g.b.ax[1] = 1; g.b.ax[2] = 0; al = 1; }
        for (int a = 0; a < 3; ++a) g.b.ax[a] /= al;
        g.b.ang = g_rng.range(0.0f, 6.2831853f); g.b.spin = g_rng.range(-18.0f, 18.0f);
        // the ground where it lands: under the prop when it falls within 0.75 m, else its quadrant's
        float dh = g.b.p[1] - ground0; if (dh < 0) dh = 0;
        float tf = (g.b.v[1] + sqrtf(g.b.v[1] * g.b.v[1] + 2.0f * G * dh)) / G;
        float lx = g.b.v[0] * tf, lz = g.b.v[2] * tf;
        g.b.ground = lx * lx + lz * lz < 0.75f * 0.75f ? ground0 : gq[(lx > 0 ? 1 : 0) | (lz > 0 ? 2 : 0)];
        g.mat = mat; g.tint = g_rng.range(0.75f, 1.0f); g.born = g_t + g_rng.range(-1.5f, 0.0f);   // grit goes a little before the chunks
        pick_patch(lk, 2.0f * g.b.half, &g.u0, &g.v0, &g.side);
        if (lk.have_tex) { g.tex = lk.ti.tex; mh_tex_addref(g.tex); }
        g_grit.push_back(g);
        ++made;
    }
    return made;
}
// dust: n soft puffs in the prop's colour, low in the box, drifting out along the push
static int spawn_dust(const float* p, const float* q, const mh::Box& box, int n, const float* rgb, float alpha, const float* dir) {
    ph::Q4 Q = {q[0], q[1], q[2], q[3]};
    float size = (std::max)(box.h[0], (std::max)(box.h[1], box.h[2]));
    int made = 0;
    for (int i = 0; i < n && (int)g_dust.size() < MH_PUFF_MAX; ++i) {
        Dust d; memset(&d, 0, sizeof d);
        float l[3] = {box.c[0] + g_rng.range(-0.6f, 0.6f) * box.h[0], box.c[1] + g_rng.range(-0.7f, 0.2f) * box.h[1], box.c[2] + g_rng.range(-0.6f, 0.6f) * box.h[2]};
        ph::V3 c = ph::add(ph::v3(p[0], p[1], p[2]), ph::qrot(Q, ph::v3(l[0], l[1], l[2])));
        d.f.p[0] = c.x; d.f.p[1] = c.y; d.f.p[2] = c.z;
        float o[3] = {c.x - p[0], 0.0f, c.z - p[2]}, ol = sqrtf(o[0] * o[0] + o[2] * o[2]) + 1e-3f;
        float sp = g_rng.range(0.5f, 1.4f);
        for (int a = 0; a < 3; a += 2) d.f.v[a] = o[a] / ol * sp + (dir ? dir[a] * g_rng.range(0.3f, 0.9f) : 0.0f);
        d.f.v[1] = g_rng.range(0.15f, 0.6f);
        d.f.r0 = (0.18f + 0.35f * size) * g_rng.range(0.7f, 1.1f);
        d.f.r1 = (0.55f + 1.1f * size) * g_rng.range(0.8f, 1.25f);
        d.f.life = g_rng.range(1.3f, 2.3f);
        d.f.alpha = alpha * g_rng.range(0.7f, 1.0f);
        float k = g_rng.range(0.9f, 1.08f);
        for (int a = 0; a < 3; ++a) d.rgb[a] = (std::min)(1.0f, rgb[a] * k);
        g_dust.push_back(d);
        ++made;
    }
    return made;
}
// the cover prop's collision box in its model's frame (CM pivot, else every pivot but Replace_*); false = no collision
static bool cover_box(DWORD model, float* p, float* q, mh::Box* box) {
    float M[16]; if (!model || rd(model) != VT_MODEL || !world_matrix(model, M)) return false;
    ph::Rigid r = ph::rigid_from_tod(M); if (!r.ok) return false;
    int np = asset_pivots(model); if (np < 1 || np > 64) return false;
    bool has_cm = false;
    for (int i = 0; i < np; ++i) if (pivot_collision(model, i) && !_stricmp(node_name(pivot_node(model, i)), "CM")) has_cm = true;
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}; int pts = 0;
    for (int i = 0; i < np; ++i) {
        DWORD l = pivot_collision(model, i); if (!l) continue;
        DWORD pn = pivot_node(model, i); const char* nm = node_name(pn);
        if (has_cm ? _stricmp(nm, "CM") != 0 : !_strnicmp(nm, "Replace", 7)) continue;
        float PW[16]; if (!pn || !world_matrix(pn, PW)) continue;
        DWORD data = rd(l), cnt = rd(l + 4); if (cnt > 64) cnt = 64;
        for (DWORD s = 0; s < cnt; ++s) {
            DWORD sh = rd(data + s * 4) & ~3u; if (!sh) continue;
            ph::V3 v[256]; int k = mh_shape_points(sh, PW, v, 256);
            for (int j = 0; j < k; ++j) {
                ph::V3 lp = ph::qrot(ph::qconj(r.q), ph::sub(v[j], r.p));
                float a[3] = {lp.x, lp.y, lp.z};
                for (int x = 0; x < 3; ++x) { if (a[x] < lo[x]) lo[x] = a[x]; if (a[x] > hi[x]) hi[x] = a[x]; }
                ++pts;
            }
        }
    }
    if (pts < 4) return false;
    for (int x = 0; x < 3; ++x) {
        box->c[x] = 0.5f * (lo[x] + hi[x]); box->h[x] = 0.5f * (hi[x] - lo[x]);
        if (box->h[x] > 2.5f) box->h[x] = 2.5f;
        if (box->h[x] < 0.04f) box->h[x] = 0.04f;
    }
    p[0] = r.p.x; p[1] = r.p.y; p[2] = r.p.z; q[0] = r.q.x; q[1] = r.q.y; q[2] = r.q.z; q[3] = r.q.w;
    return true;
}
// a default box by the prop's name when its collision cannot be read (half extents, centre above the origin)
static mh::Box default_box(const char* name) {
    struct D { const char* pat; float hx, hy, hz; } T[] = {{"crate_small,smallcrate,cratesmall", 0.32f, 0.3f, 0.32f}, {"crate,box,pallet", 0.45f, 0.4f, 0.45f},
        {"chair,stool", 0.25f, 0.45f, 0.25f}, {"bench", 0.9f, 0.25f, 0.25f}, {"table,desk", 0.6f, 0.38f, 0.4f}, {"stall,stand,kiosk,cart,wagon", 1.0f, 0.6f, 0.6f},
        {"sofa,couch", 0.9f, 0.4f, 0.45f}, {"bin,dumpster", 0.4f, 0.5f, 0.4f}, {"monitor,keyboard,computer,tv,crt,bottle", 0.2f, 0.15f, 0.2f}};
    for (auto& d : T) if (mh::name_match(name, d.pat)) return mh::Box{{0, d.hy, 0}, {d.hx, d.hy, d.hz}};
    return mh::Box{{0, 0.4f, 0}, {0.45f, 0.4f, 0.45f}};
}
static bool cover_box_of(DWORD destroyable, float* p, float* q, mh::Box* box) {
    return cover_box(destroyable_model(destroyable, false), p, q, box) || cover_box(destroyable_model(destroyable, true), p, q, box);
}
// the look of a style 1 burst: chunk shape and sizes by material, the prop's own texture when it can be read
static void make_look(Look* lk, DWORD destroyable, int mat, bool blast, const mh::Box& box) {
    memset(lk, 0, sizeof *lk);
    lk->shape = mat == MH_WOOD ? mh::SH_SPLINTER : (mat == MH_METAL || mat == MH_CHAR ? mh::SH_PLATE : mh::SH_LUMP);
    lk->min_len = (blast ? 0.03f : 0.05f) * g_c.debris_size;
    lk->max_len = (blast ? 0.11f : 0.16f) * g_c.debris_size;
    lk->prop_len = 2.0f * (std::max)(box.h[0], (std::max)(box.h[1], box.h[2]));
    if (!g_own_tex) return;
    DWORD m0 = prop_model_of(destroyable), m1 = destroyable_model(destroyable, true);
    lk->have_tex = mh_prop_texture(m0, &lk->ti);
    if (!lk->have_tex && m1 && m1 != m0) {
        char why[160]; memcpy(why, lk->ti.why, sizeof why);
        lk->have_tex = mh_prop_texture(m1, &lk->ti);
        if (!lk->have_tex) _snprintf(lk->ti.why, sizeof lk->ti.why - 1, "default %s; current %s", why, lk->ti.why);
    }
}
static void dust_colour(const Look& lk, int mat, float* rgb) {
    static const float DUST[3] = {0.70f, 0.65f, 0.57f};
    float base[3] = {0.55f, 0.45f, 0.33f};
    if (lk.have_tex) memcpy(base, lk.ti.avg, 12);
    else if (mat == MH_METAL || mat == MH_CHAR) { base[0] = base[1] = base[2] = 0.45f; }
    for (int a = 0; a < 3; ++a) rgb[a] = 0.55f * base[a] + 0.45f * DUST[a];
}
static void log_look(const char* what, int chunks, int grit, int dust, const char* name, const Look& lk, int mat, const double* t) {
    double look = (t[1] - t[0]) * 1000.0, ch = (t[2] - t[1]) * 1000.0, rest = (t[3] - t[2]) * 1000.0;
    static const char* MAT[MH_MATERIALS] = {"wood", "metal", "red", "charred"};
    if (lk.have_tex)
        gp_log("mayhem: debris: %s %s: %d chunk(s) %.0f..%.0f cm + %d grit + %d dust puff(s), texture '%s' %dx%d (mean %.2f %.2f %.2f, %d/256 cells used), %d alive, %.2f ms (look %.2f, chunks %.2f, grit + dust %.2f)\n",
               name, what, chunks, lk.min_len * 100.0f, lk.max_len * 100.0f, grit, dust, lk.ti.name[0] ? lk.ti.name : "?", lk.ti.w, lk.ti.h,
               lk.ti.avg[0], lk.ti.avg[1], lk.ti.avg[2], lk.ti.ncell, (int)(g_debris.size() + g_grit.size()), look + ch + rest, look, ch, rest);
    else
        gp_log("mayhem: debris: %s %s: %d chunk(s) %.0f..%.0f cm + %d grit + %d dust puff(s), no prop texture (%s): procedural %s, %d alive, %.2f ms (look %.2f, chunks %.2f, grit + dust %.2f)\n",
               name, what, chunks, lk.min_len * 100.0f, lk.max_len * 100.0f, grit, dust, g_own_tex ? lk.ti.why : "references differ", MAT[mat],
               (int)(g_debris.size() + g_grit.size()), look + ch + rest, look, ch, rest);
}
void mh_debris_burst(DWORD destroyable, const float* pos, const float* push_dir, const Cover* cv) {
    if (!g_draw_on) return;
    char name[96]; cover_name(destroyable, name, sizeof name);
    float p[3] = {pos[0], pos[1], pos[2]}, q[4] = {0, 0, 0, 1};
    mh::Box box = default_box(name);
    bool have = false;
    if (cv && cv->have_box) { box = cv->box; memcpy(p, cv->bp, 12); memcpy(q, cv->bq, 16); have = true; }
    else have = cover_box_of(destroyable, p, q, &box);
    float burst = 3.5f * g_c.debris_impulse, push = 3.0f * g_c.debris_impulse;
    int mat = material_of(name, false);
    if (g_c.debris_style == 0) {
        int n = spawn_pieces(p, q, box, g_c.debris_pieces, burst, push_dir, push, mat, nullptr);
        gp_log("mayhem: debris: %d piece(s) from %s (%s box %.2f x %.2f x %.2f m), %d alive\n", n, name, have ? "collision" : "default",
               2 * box.h[0], 2 * box.h[1], 2 * box.h[2], (int)g_debris.size());
        return;
    }
    double t[4]; t[0] = gp_now();
    Look lk; make_look(&lk, destroyable, mat, false, box);
    t[1] = gp_now();
    int n = spawn_pieces(p, q, box, g_c.debris_pieces, burst, push_dir, push, mat, &lk);
    t[2] = gp_now();
    int g = spawn_grit(p, q, box, g_c.debris_bits, burst * 0.8f, push_dir, push * 0.6f, mat, lk);
    int d = 0;
    if (g_c.debris_dust) { float rgb[3]; dust_colour(lk, mat, rgb); d = spawn_dust(p, q, box, 5, rgb, 0.55f, push_dir); }
    t[3] = gp_now();
    char what[64]; _snprintf(what, sizeof what, "broke (%s box %.2f x %.2f x %.2f m)", have ? "collision" : "default", 2 * box.h[0], 2 * box.h[1], 2 * box.h[2]);
    what[sizeof what - 1] = 0;
    log_look(what, n, g, d, name, lk, mat, t);
    mh_tex_release(lk.ti.tex);   // the pieces hold their own references
}
// shards of an exploding destroyable (barrels, tanks): style 0 small fast boxes, red / metal; style 1 small chunks in the
// prop's own texture plus grit (the game's own fire and smoke stand for the dust)
static void blast_shards(const Blast& b) {
    if (!g_draw_on || g_c.blast_debris <= 0 || b.kind != BL_DESTROYABLE) return;
    char name[96]; cover_name(b.node, name, sizeof name);
    float q[4] = {0, 0, 0, 1};
    mh::Box box = {{0, 0.5f, 0}, {0.35f, 0.5f, 0.35f}};
    int mat = material_of(name, true);
    if (g_c.debris_style == 0) {
        int n = spawn_pieces(b.pos, q, box, g_c.blast_debris, 9.0f * g_c.debris_impulse, nullptr, 0.0f, mat, nullptr);
        gp_log("mayhem: debris: %d shard(s) from the %s blast, %d alive\n", n, name[0] ? name : "destroyable", (int)g_debris.size());
        return;
    }
    double t[4]; t[0] = gp_now();
    float p[3] = {b.pos[0], b.pos[1], b.pos[2]};
    if (!cover_box_of(b.node, p, q, &box)) { memcpy(p, b.pos, 12); q[0] = q[1] = q[2] = 0; q[3] = 1; box = mh::Box{{0, 0.5f, 0}, {0.35f, 0.5f, 0.35f}}; }
    Look lk; make_look(&lk, b.node, mat, true, box);
    t[1] = gp_now();
    int n = spawn_pieces(p, q, box, g_c.blast_debris, 7.0f * g_c.debris_impulse, nullptr, 0.0f, mat, &lk);
    t[2] = gp_now();
    int g = spawn_grit(p, q, box, (g_c.debris_bits * 13 + 5) / 10, 6.0f * g_c.debris_impulse, nullptr, 0.0f, mat, lk);
    t[3] = gp_now();
    log_look("blast", n, g, 0, name[0] ? name : "destroyable", lk, mat, t);
    mh_tex_release(lk.ti.tex);
}
static void debris_tick(double now) {
    float dt = g_t - g_fx_t; g_fx_t = g_t;
    if (dt < 0.0f || dt > 0.25f) dt = 0.0f;
    if (g_debris.empty() && g_grit.empty() && g_dust.empty()) return;
    if (phys_world_gen() != g_world_gen || !phys_world_ready()) { debris_clear(); return; }
    for (size_t i = g_debris.size(); i-- > 0;) {
        Debris& d = g_debris[i];
        if (g_t - d.born > g_c.debris_life || g_t < d.born - 1.0f) { debris_remove(i); continue; }
        float p[3], q[4];
        if (g_api->body_get(d.b, p, q) < 0) { mh_tex_release(d.tex); g_debris.erase(g_debris.begin() + (ptrdiff_t)i); continue; }
        if (p[1] < -500.0f) { debris_remove(i); continue; }   // fell out of the world
        if (!d.have) { memcpy(d.p0, p, 12); memcpy(d.q0, q, 16); memcpy(d.p1, p, 12); memcpy(d.q1, q, 16); d.t1 = now; d.have = true; continue; }
        if (memcmp(p, d.p1, 12) || memcmp(q, d.q1, 16)) { memcpy(d.p0, d.p1, 12); memcpy(d.q0, d.q1, 16); memcpy(d.p1, p, 12); memcpy(d.q1, q, 16); d.t1 = now; }
    }
    for (size_t i = g_grit.size(); i-- > 0;) {
        Grit& g = g_grit[i];
        if (g_t - g.born > g_c.debris_life || g_t < g.born - 3.0f) { grit_remove(i); continue; }
        mh::bit_step(g.b, dt, 9.81f);
    }
    for (size_t i = g_dust.size(); i-- > 0;) {
        mh::puff_step(g_dust[i].f, dt);
        float r, a; if (!mh::puff_at(g_dust[i].f, &r, &a)) g_dust.erase(g_dust.begin() + (ptrdiff_t)i);
    }
}
static bool draw_guarded(const MhPiece* p, int n, const MhPuff* f, int nf, double* ms) {
    __try { g_drawn_last = mh_draw_pieces(p, n, f, nf, 1.0f, ms); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static float fade_scale(float left) { return left < 0.8f ? (left > 0 ? left / 0.8f : 0.0f) : 1.0f; }
// MH_DRAW: GfxInternal::Render's call of the frame-end motion blur 0x45D5E0 (thiscall(renderer), after the world layers)
static void __fastcall MhBlurWrap(DWORD ren, void* edx) {
    static bool warm = false;   // the textures made in the first world frame, not in the frame of the first break
    if (!warm && g_installed && g_draw_on) { warm = true; __try { mh_draw_warm(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (g_installed && g_draw_on && !(g_debris.empty() && g_grit.empty() && g_dust.empty())) {
        static MhPiece pieces[MH_DRAW_MAX];
        static MhPuff puffs[MH_PUFF_MAX];
        int n = 0, nf = 0; double now = gp_now();
        for (auto& d : g_debris) {
            if (!d.have || n >= MH_DRAW_MAX) continue;
            MhPiece& p = pieces[n++];
            float a = (float)((now - d.t1) / 0.04); a = a < 0 ? 0 : (a > 1 ? 1 : a);
            for (int k = 0; k < 3; ++k) p.pos[k] = d.p0[k] + (d.p1[k] - d.p0[k]) * a;
            ph::Q4 q = ph::qslerp(ph::Q4{d.q0[0], d.q0[1], d.q0[2], d.q0[3]}, ph::Q4{d.q1[0], d.q1[1], d.q1[2], d.q1[3]}, a);
            p.rot[0] = q.x; p.rot[1] = q.y; p.rot[2] = q.z; p.rot[3] = q.w;
            memcpy(p.half, d.half, 12);
            p.scale = fade_scale(g_c.debris_life - (g_t - d.born));
            p.tint = d.tint; p.uv_off = d.uv; p.mat = d.mat;
            p.tex = d.tex; p.u0 = d.u0; p.v0 = d.v0; p.side = d.side;
        }
        for (auto& g : g_grit) {
            if (n >= MH_DRAW_MAX) break;
            MhPiece& p = pieces[n++];
            memcpy(p.pos, g.b.p, 12);
            float s = sinf(0.5f * g.b.ang), c = cosf(0.5f * g.b.ang);
            p.rot[0] = g.b.ax[0] * s; p.rot[1] = g.b.ax[1] * s; p.rot[2] = g.b.ax[2] * s; p.rot[3] = c;
            p.half[0] = g.hx; p.half[1] = g.hy; p.half[2] = g.hz;
            p.scale = fade_scale(g_c.debris_life - (g_t - g.born));
            p.tint = g.tint; p.uv_off = 0.0f; p.mat = g.mat;
            p.tex = g.tex; p.u0 = g.u0; p.v0 = g.v0; p.side = g.side;
        }
        for (auto& d : g_dust) {
            float r, a; if (nf >= MH_PUFF_MAX || !mh::puff_at(d.f, &r, &a)) continue;
            MhPuff& f = puffs[nf++];
            memcpy(f.pos, d.f.p, 12); f.radius = r; f.alpha = a; memcpy(f.rgb, d.rgb, 12);
        }
        double ms = 0;
        if (!draw_guarded(pieces, n, puffs, nf, &ms)) { g_draw_on = false; gp_log("mayhem: draw: exception: debris drawing OFF\n"); }
        g_st_draw.add(ms);
    }
    ((void(__fastcall*)(DWORD, void*))(uintptr_t)R_MH_BLUR)(ren, edx);
}

// ================================================================================================ survey (test copy)
static void survey_destroyable(DWORD n, const float* rp) {
    DWORD d = sdata(n); float p[3] = {0, 0, 0}; get_pos(n, p);
    char cur[96], def[96], dmg[96], deb[96], rep[96];
    model_name(rd(d + DO_CURRENT_MESH), cur, sizeof cur); model_name(rd(d + DO_DEFAULT_MESH), def, sizeof def);
    model_name(rd(d + DO_DAMAGED_MESH), dmg, sizeof dmg); model_name(rd(d + DO_DEBRIS), deb, sizeof deb);
    model_name(rd(d + DO_REPLACEMENT), rep, sizeof rep);
    DWORD ph = rd(d + DO_PLACEHOLDER);
    gp_log("mayhem: survey DO %08X '%s' ph %08X '%s' at (%.1f, %.1f, %.1f) %.1f m mask %06X sub %d | expl %d shoot %d move %d break2 %d repl %d "
           "ball %d impact %d melee %d immune %d locked %d dead %d | health %.1f taken %.1f mass %.1f range %.1f dmg %.1f | cur %08X '%s' def %08X '%s' "
           "damaged %08X '%s' debris %08X '%s' repl %08X '%s' com %08X\n",
           n, node_name(n), ph, node_name(ph), p[0], p[1], p[2], dist3(p, rp), sp_mask(n), (int)rd(d + DO_SUBTYPE),
           (int)rd(d + DO_EXPLODEABLE), (int)rd(d + DO_SHOOTABLE), (int)rd(d + DO_MOVEABLE), (int)rd(d + DO_BREAK2), (int)rd(d + DO_HASREPL),
           (int)rd(d + DO_DMG_BALL), (int)rd(d + DO_DMG_IMPACT), (int)rd(d + DO_DMG_MELEE), (int)rd(d + DO_IMMUNE), (int)rd(d + DO_LOCKED),
           (int)rd(d + DO_ISDEAD), rdf(d + DO_HEALTH), rdf(d + DO_DAMAGE), rdf(d + DO_MASS), rdf(d + DO_EXPL_RANGE), rdf(d + DO_EXPL_DAMAGE),
           rd(d + DO_CURRENT_MESH), cur, rd(d + DO_DEFAULT_MESH), def, rd(d + DO_DAMAGED_MESH), dmg, rd(d + DO_DEBRIS), deb,
           rd(d + DO_REPLACEMENT), rep, rd(d + DO_COM));
    {   // what the mesh entities are, and the Model nodes under them / under the destroyable
        DWORD cm = rd(d + DO_CURRENT_MESH), dm = rd(d + DO_DAMAGED_MESH), rp2 = rd(d + DO_REPLACEMENT), mods[8];
        int nm1 = cm ? models_under(cm, mods, 8) : 0;
        char a[96] = "", b2[96] = "";
        if (nm1) model_name(mods[0], a, sizeof a);
        DWORD mods2[8]; int nm2 = models_under(n, mods2, 8);
        if (nm2) model_name(mods2[0], b2, sizeof b2);
        gp_log("mayhem: survey   mesh vtables cur %06X dmg %06X repl %06X; models under cur %d (%08X '%s' flags %03X), under the node %d (%08X '%s' flags %03X); cur parent %08X\n",
               rd(cm), rd(dm), rd(rp2), nm1, nm1 ? mods[0] : 0, a, nm1 ? rd(mods[0] + 0x28) & 0xFFF : 0, nm2, nm2 ? mods2[0] : 0, b2,
               nm2 ? rd(mods2[0] + 0x28) & 0xFFF : 0, rd(cm + 0x40));
    }
    // the model's pivots: name, collision shapes, node flags
    DWORD m = destroyable_model(n, true);
    if (m && rd(m) == VT_MODEL) {
        char buf[900]; size_t k = 0; buf[0] = 0;
        int np = asset_pivots(m);
        for (int i = 0; i < np && i < 40 && k + 48 < sizeof buf; ++i) {
            DWORD pn = pivot_node(m, i), l = pivot_collision(m, i);
            k += (size_t)_snprintf(buf + k, sizeof buf - k, " %d:%.18s:%s:%03X", i, node_name(pn), l ? "C" : "-", pn ? rd(pn + 0x28) & 0xFFF : 0xFFF);
        }
        buf[sizeof buf - 1] = 0;
        gp_log("mayhem: survey   model %08X pivots %d:%s | parent %08X model parent %08X\n", m, np, buf, rd(n + 0x40), rd(m + 0x40));
    }
}
static void survey(const float* rp) {
    DWORD L = query(rp, g_survey_radius, 0xFFFFFF, 1);
    int n = list_count(L);
    struct H { DWORD h; int n; DWORD mask; } hist[48]; int nh = 0;
    int nd = 0, nv = 0;
    for (int i = 0; i < n && i < 4096; ++i) {
        DWORD t = list_at(L, i); if (!t) continue;
        DWORD h = cls_of(t); DWORD m = sp_mask(t);
        int j = 0; for (; j < nh; ++j) if (hist[j].h == h) break;
        if (j == nh && nh < 48) { hist[nh].h = h; hist[nh].n = 0; hist[nh].mask = m; ++nh; }
        if (j < nh) { ++hist[j].n; hist[j].mask |= m; }
        if (h == H_DO) { ++nd; survey_destroyable(t, rp); }
        else if (h == H_VROOT) {
            ++nv; DWORD vai = rd(sdata(t) + VR_AICTRL); DWORD vd = sdata(vai); float p[3] = {0, 0, 0}; get_pos(t, p);
            gp_log("mayhem: survey CAR %08X '%s' at (%.1f, %.1f, %.1f) %.1f m mask %06X ai %08X cls %06X health %.0f taken %.0f destroyed %d\n",
                   t, node_name(t), p[0], p[1], p[2], dist3(p, rp), sp_mask(t), vai, cls_of(vai), rdf(vd + VA_HEALTH), rdf(vd + VA_DAMAGE), (int)rd(vd + VA_DESTROYED));
        }
    }
    query_free(L);
    char buf[900]; size_t k = 0; buf[0] = 0;
    for (int j = 0; j < nh && k + 32 < sizeof buf; ++j) k += (size_t)_snprintf(buf + k, sizeof buf - k, " %06X:%d(m%06X)", hist[j].h, hist[j].n, hist[j].mask);
    gp_log("mayhem: survey at (%.1f, %.1f, %.1f) r %.0f: %d nodes, %d destroyables, %d cars; classes%s\n", rp[0], rp[1], rp[2], g_survey_radius, n, nd, nv, buf);
    // also the placeholders of havoc objects (they own the pooled destroyables)
    DWORD P = query(rp, g_survey_radius, 0xFFFFFF, 0);
    int np = list_count(P), nph = 0;
    for (int i = 0; i < np && i < 4096; ++i) {
        DWORD t = list_at(P, i);
        if (!t || cls_of(t) != H_DOPH) continue;
        ++nph;
        DWORD d = sdata(t); float p[3] = {0, 0, 0}; get_pos(t, p);
        if (nph <= 60) gp_log("mayhem: survey PH %08X '%s' at (%.1f, %.1f, %.1f) %.1f m sub %d resource %08X mask %06X\n", t, node_name(t), p[0], p[1], p[2], dist3(p, rp),
                              (int)rd(d + 8), rd(d + 0x18), sp_mask(t));
    }
    query_free(P);
    gp_log("mayhem: survey: %d havoc placeholders (mode 0 query, %d nodes)\n", nph, np);
}
// test copy: the nearest explosive destroyable takes a take_a_hit as if shot (the first explosion of a proof run)
static DWORD nearest_destroyable(const float* rp, float radius, bool explosive, float* dout) {
    DWORD L = query(rp, radius, MASK_EXPLOSION, 1), best = 0; float bd = 1e9f;
    for (int i = 0, n = list_count(L); i < n && i < 512; ++i) {
        DWORD t = list_at(L, i), guard = 0;
        if (!t || cls_of(t) != H_DO || !destroyable_target(t, explosive, &guard)) continue;
        char nm[64]; if (!explosive && !is_cover(t, nm, sizeof nm)) continue;
        float p[3]; if (!get_pos(t, p)) continue;
        float d = dist3(p, rp); if (d < bd) { bd = d; best = t; }
    }
    query_free(L);
    if (dout) *dout = bd;
    return best;
}
static void debug_ignite(const float* rp) {
    float d = 0; DWORD t = nearest_destroyable(rp, 60.0f, true, &d);
    if (!t) { gp_log("mayhem: debug ignite: no explosive destroyable within 60 m\n"); return; }
    Hit h; memset(&h, 0, sizeof h);
    float p[3]; get_pos(t, p);
    h.dmg = 5000.0f; h.owner = ram_root(); h.inflictor = t; h.collvol = t; h.type = 1;
    for (int a = 0; a < 3; ++a) { h.pos[a] = p[a]; h.imp[a] = 0; }
    bool ok = send_cmd(t, CMD_TAKE_A_HIT, &h);
    gp_log("mayhem: debug ignite: destroyable %08X %.1f m away: take_a_hit 5000%s\n", t, d, ok ? "" : " FAILED");
    mark("mh_ignite");
}
// test copy: debug_place = patterns ("explosive" = every explosive one): up to debug_place_max destroyables are put
// debug_place_dist m ahead of Ram (the camera's view), debug_place_gap m apart across the view; debug_place_cover = 1
// also puts the nearest cover prop between Ram and the nearest enemy (debug_cover_dist m in front of the enemy)
static bool g_placed = false;
static DWORD g_cover_target = 0, g_cover_target_guard = 0;
static DWORD g_place_nodes[16]; static int g_nplace = 0;
static const DWORD C_GAMECAM = 0x260;
static bool view_flat(const float* rp, float* f) {   // the game camera's flat forward, from the camera toward Ram
    DWORD cd = gp_cache_data(); DWORD cam = cd ? rd(cd + C_GAMECAM) : 0; float M[16];
    if (!cam || !world_matrix(cam, M)) return false;
    float x = M[8], z = M[10], l = sqrtf(x * x + z * z); if (l < 0.3f) return false;
    x /= l; z /= l;
    if (x * (rp[0] - M[12]) + z * (rp[2] - M[14]) < 0.0f) { x = -x; z = -z; }
    f[0] = x; f[1] = 0; f[2] = z; return true;
}
static void debug_place(const float* rp, double now) {
    if (g_placed || !(g_place[0] || g_place_cover || g_place_ph[0])) return;
    if (g_place_key != gp::KEY_NONE) {   // a key: the delay counts from the title level, which also ticks
        if (!g_place_req) return;
        g_place_req = false;
    } else if (now - g_ctl_since < g_place_delay) return;
    g_placed = true;
    float f[3] = {1, 0, 0}; view_flat(rp, f);
    float r[3] = {-f[2], 0, f[0]};
    if (g_place[0]) {
        DWORD L = query(rp, 150.0f, MASK_EXPLOSION, 1);
        DWORD pick[16]; float pd[16]; int n = 0;
        for (int i = 0, c = list_count(L); i < c && i < 512; ++i) {
            DWORD t = list_at(L, i), guard = 0;
            if (!t || cls_of(t) != H_DO) continue;
            bool expl = rd(sdata(t) + DO_EXPLODEABLE) != 0;
            char nm[96]; cover_name(t, nm, sizeof nm);
            bool want = (mh::name_match("explosive", g_place) && expl) || mh::name_match(nm, g_place);
            if (!want || !destroyable_target(t, expl, &guard)) continue;
            float p[3]; if (!get_pos(t, p)) continue;
            float d = dist3(p, rp);
            if (n < g_place_max && n < 16) { pick[n] = t; pd[n] = d; ++n; }
            else { int w = 0; for (int k = 1; k < n; ++k) if (pd[k] > pd[w]) w = k; if (n && d < pd[w]) { pick[w] = t; pd[w] = d; } }
        }
        query_free(L);
        for (int i = 0; i < n; ++i) {
            float off = (i - (n - 1) * 0.5f) * g_place_gap;
            float at[3] = {rp[0] + f[0] * g_place_dist + r[0] * off, rp[1] + 0.05f, rp[2] + f[2] * g_place_dist + r[2] * off};
            float p0[3]; get_pos(pick[i], p0);
            bool ok = set_world_pos(pick[i], at);
            if (g_nplace < 16) g_place_nodes[g_nplace++] = pick[i];
            char nm[96]; cover_name(pick[i], nm, sizeof nm);
            gp_log("mayhem: debug place: %s %08X from (%.1f, %.1f, %.1f) to (%.2f, %.2f, %.2f)%s\n", nm, pick[i], p0[0], p0[1], p0[2], at[0], at[1], at[2], ok ? "" : " FAILED");
        }
        gp_log("mayhem: debug place: %d destroyable(s) %.1f m ahead of Ram, %.1f m apart (view %.2f, %.2f)\n", n, g_place_dist, g_place_gap, f[0], f[2]);
    }
    if (g_place_ph[0]) {   // havoc placeholders (the pool then gives them their destroyables at the new place)
        DWORD L = query(rp, 150.0f, 0xFFFFFF, 0);
        DWORD pick[16]; float pd[16]; int n = 0;
        for (int i = 0, c = list_count(L); i < c && i < 1024; ++i) {
            DWORD x = list_at(L, i);
            if (!x || cls_of(x) != H_DOPH || !mh::name_match(node_name(x), g_place_ph)) continue;
            float p[3]; if (!get_pos(x, p)) continue;
            float d = dist3(p, rp);
            if (n < g_place_max && n < 16) { pick[n] = x; pd[n] = d; ++n; }
            else { int w = 0; for (int k = 1; k < n; ++k) if (pd[k] > pd[w]) w = k; if (n && d < pd[w]) { pick[w] = x; pd[w] = d; } }
        }
        query_free(L);
        for (int i = 0; i < n; ++i) {
            float off = (i - (n - 1) * 0.5f) * g_place_gap;
            float at[3] = {rp[0] + f[0] * g_place_dist + r[0] * off, rp[1], rp[2] + f[2] * g_place_dist + r[2] * off};
            float p0[3]; get_pos(pick[i], p0);
            bool ok = set_world_pos(pick[i], at);
            DWORD res = rd(sdata(pick[i]) + 0x18);   // Resource (the destroyable it owns now)
            if (res && cls_of(res) == H_DO) set_world_pos(res, at);
            gp_log("mayhem: debug place placeholder: '%s' %08X (resource %08X) from (%.1f, %.1f, %.1f) to (%.2f, %.2f, %.2f)%s\n", node_name(pick[i]), pick[i], res,
                   p0[0], p0[1], p0[2], at[0], at[1], at[2], ok ? "" : " FAILED");
        }
        gp_log("mayhem: debug place placeholder: %d moved %.1f m ahead of Ram, %.1f m apart\n", n, g_place_dist, g_place_gap);
    }
    if (g_place_cover) {   // the nearest live enemy and the nearest cover prop
        DWORD L = query(rp, 60.0f, 0x40, 0), enemy = 0; float ed = 1e9f, ep[3] = {0, 0, 0};
        DWORD ram = ram_root();
        for (int i = 0, c = list_count(L); i < c && i < 64; ++i) {
            DWORD x = list_at(L, i);
            if (!x || x == ram || cls_of(x) != H_ROOT) continue;
            DWORD ai = rd(sdata(x) + CR_AI); if (!ai || cls_of(ai) != H_SENTRY || !(rdf(sdata(ai) + SE_HEALTH) > 0.0f)) continue;
            float p[3]; if (!get_pos(x, p)) continue;
            float d = dist3(p, rp); if (d < ed) { ed = d; enemy = x; ep[0] = p[0]; ep[1] = p[1]; ep[2] = p[2]; }
        }
        query_free(L);
        float d = 0; DWORD cov = nearest_destroyable(rp, 150.0f, false, &d);
        if (enemy && cov) {
            float dx = rp[0] - ep[0], dz = rp[2] - ep[2], l = sqrtf(dx * dx + dz * dz); if (l < 0.1f) l = 0.1f;
            float at[3] = {ep[0] + dx / l * g_cover_place_dist, ep[1] + 0.05f, ep[2] + dz / l * g_cover_place_dist};
            bool ok = set_world_pos(cov, at);
            DWORD ph = rd(sdata(cov) + DO_PLACEHOLDER);
            if (ph && cls_of(ph) == H_DOPH) set_world_pos(ph, at);   // its placeholder too: the pool keeps it there
            g_cover_target = cov; g_cover_target_guard = rd(sdata(cov) + DO_PLACEHOLDER);
            char nm[96]; cover_name(cov, nm, sizeof nm);
            gp_log("mayhem: debug place cover: %s %08X put %.1f m in front of enemy %08X (%.1f m from Ram) at (%.2f, %.2f, %.2f)%s\n", nm, cov,
                   g_cover_place_dist, enemy, ed, at[0], at[1], at[2], ok ? "" : " FAILED");
        } else gp_log("mayhem: debug place cover: enemy %08X, cover prop %08X - nothing placed\n", enemy, cov);
    }
    mark("mh_placed");
}
static void debug_hit(const float* rp) {   // one bullet-sized hit on the placed cover prop, else the nearest one
    float d = 0; DWORD t = 0;
    if (g_cover_target && alive(g_cover_target) && !rd(sdata(g_cover_target) + DO_ISDEAD)) {
        float p[3]; if (get_pos(g_cover_target, p)) { t = g_cover_target; d = dist3(p, rp); }
    }
    if (!t) t = nearest_destroyable(rp, 40.0f, false, &d);
    if (!t) { gp_log("mayhem: debug hit: no cover prop within 40 m\n"); return; }
    Hit h; memset(&h, 0, sizeof h);
    float p[3]; get_pos(t, p);
    h.dmg = 10.0f; h.owner = ram_root(); h.inflictor = ram_root(); h.collvol = t; h.type = 0;
    for (int a = 0; a < 3; ++a) { h.pos[a] = p[a]; h.imp[a] = (p[a] - rp[a]) / (d > 0.1f ? d : 0.1f); }
    bool ok = send_cmd(t, CMD_TAKE_A_HIT, &h);
    DWORD dd = sdata(t);
    gp_log("mayhem: debug hit: cover %08X %.1f m: take_a_hit 10 (type 0)%s; damage taken %.1f of %.1f, dead %d\n", t, d, ok ? "" : " FAILED",
           rdf(dd + DO_DAMAGE), rdf(dd + DO_HEALTH), (int)rd(dd + DO_ISDEAD));
}

// ================================================================================================ tick
static void log_stats(double now) {
    gp_log("mayhem: stats blasts %d (cars %d), scheduled %d (%d held from the game's own blast), set off %d, skipped %d, pending %d, dropped full/depth/dup %d/%d/%d | "
           "cover tracked %d, scaled %d, broke %d, enemies exposed %d, debris %d | tick %.3f ms avg %.3f max (%d) | query %.3f avg %.3f max | fire %.3f avg %.3f max | draw %.3f avg %.3f max (%d pieces)\n",
           g_n_blasts, g_n_car_blasts, g_n_sched, g_n_held, g_n_fired, g_n_skipped, (int)g_sched.q.size(), g_sched.dropped_full, g_sched.dropped_depth,
           g_sched.dropped_dup, (int)g_cover_list.size(), g_n_scaled, g_n_breaks, g_n_exposed, g_n_debris, g_st_tick.avg(), g_st_tick.mx, g_st_tick.n,
           g_st_query.avg(), g_st_query.mx, g_st_fire.avg(), g_st_fire.mx, g_st_draw.avg(), g_st_draw.mx, g_drawn_last);
    g_st_tick.reset(); g_st_query.reset(); g_st_fire.reset(); g_st_draw.reset(); g_last_log = now;
}
void mprops_tick(void* thread, double now) {
    (void)thread;
    if (!g_installed) return;
    LONG f = g_frame;
    if (f == g_tick_frame) return;   // kb and pad procedures may both tick: once per main-loop pass
    g_tick_frame = f;
    double t0 = gp_now();
    int pm = gp_playmode();
    if (pm == 3) {   // rewind: the game puts the world back; pending detonations would hit restored objects
        if (!g_rewind) { g_rewind = true; if (!g_sched.q.empty()) gp_log("mayhem: rewind: %d pending detonation(s) dropped\n", (int)g_sched.q.size()); g_sched.clear(); }
        g_nblasts = 0;
        return;
    }
    g_rewind = false;
    if (pm == 2) return;
    g_t += game_dt();
    DWORD ram = ram_root(); float rp[3];
    if (!ram || !get_pos(ram, rp)) return;
    if (g_ctl_since == 0.0) g_ctl_since = now;
    // test copy
    if (g_dbg_survey && now - g_ctl_since > 3.0 && (g_last_survey < -1e8 || (g_survey_seconds > 0.0f && now - g_last_survey > g_survey_seconds))) { g_last_survey = now; survey(rp); }
    if (g_survey_req) { g_survey_req = false; survey(rp); }
    if (g_ignite_req) { g_ignite_req = false; debug_ignite(rp); }
    if (g_hit_req) { g_hit_req = false; debug_hit(rp); }
    debug_place(rp, now);
    // blasts reported by the hooks
    LONG nb = g_nblasts;
    if (nb > 0) {
        Blast local[32]; int k = nb > 32 ? 32 : (int)nb;
        memcpy(local, g_blasts, sizeof(Blast) * (size_t)k);
        g_nblasts = 0;
        for (int i = 0; i < k; ++i) handle_blast(local[i]);
    }
    // due detonations
    if (g_chain_on) {
        mh::Pending due[16];
        int n = g_sched.pop_due(g_c, g_t, due, 16);
        for (int i = 0; i < n; ++i) fire(due[i]);
        g_fired.expire(g_t);
    }
    if (g_cover_on) cover_tick(rp, now);
    debris_tick(now);
    double ms = (gp_now() - t0) * 1000.0;
    g_st_tick.add(ms);
    if (now - g_last_log >= g_log_seconds) log_stats(now);
}
void mprops_post_input(BYTE* kb_edges, BYTE* kb_state, double now) {
    (void)now; (void)kb_state;
    if (!g_installed) return;
    InterlockedIncrement(&g_frame);
    if (!kb_edges) return;
    auto pressed = [&](int k) { return k >= 0 && k < 256 && (kb_edges[k] & 1); };
    if (pressed(g_survey_key)) g_survey_req = true;
    if (pressed(g_ignite_key)) g_ignite_req = true;
    if (pressed(g_hit_key)) g_hit_req = true;
    if (pressed(g_place_key)) g_place_req = true;
}

// ================================================================================================ install
bool mprops_install(bool tick_ok) {
    if (!mprops_enabled()) return false;
    if (!tick_ok) { gp_log("mayhem: the input tick is not installed: OFF\n"); return false; }
    g_api = phys_api();
    if (!g_api) { gp_log("mayhem: chain reactions / breakable cover need [physics] enabled = 1 and TODPhysics.dll: OFF\n"); return false; }
    for (const char* r : MH_REFS) if (!gp_ref_ok(r)) { gp_log("mayhem: reference %s differs: OFF, nothing patched\n", r); return false; }
    int sites = 0;
    if (g_c.chain || debug_wanted()) {
        bool a = gp_site_ok("MH_CAR_A"), b = gp_site_ok("MH_CAR_B");
        if (a && b) { if (gp_patch_call("MH_CAR_A", (void*)&MhCarWrap)) ++sites; if (gp_patch_call("MH_CAR_B", (void*)&MhCarWrap)) ++sites; }
        else gp_log("mayhem: car explosion sites differ: cars do not start chains\n");
    }
    {   // every explosive destroyable's blast is reported (chain links, shards, the Box3D blast)
        if (gp_site_ok("MH_DO_QUERY") && gp_patch_call("MH_DO_QUERY", (void*)&MhQueryWrap)) ++sites;
        else gp_log("mayhem: do_explosion query site differs: only PH_DESTROY reports destroyable blasts\n");
    }
    if (g_c.chain) {   // the destroyable blast's own instant hits on explosive neighbours become chain links (delayed)
        if (gp_site_ok("MH_DO_HIT") && gp_patch_call("MH_DO_HIT", (void*)&MhDoHitWrap)) ++sites;
        else gp_log("mayhem: do_explosion hit site differs: barrels in a blast's own range still go off at once\n");
    }
    if (g_c.cover || (g_c.chain && g_c.blast_debris > 0)) {   // debris pieces drawn in the world pass
        if (gp_ref_ok("MH_GFX_LAYERS") && gp_site_ok("MH_DRAW") && gp_patch_call("MH_DRAW", (void*)&MhBlurWrap)) { ++sites; g_draw_on = true; }
        else gp_log("mayhem: world-pass draw site differs: no debris pieces (cover still breaks)\n");
        g_own_tex = g_c.debris_style == 1 && gp_ref_ok("MH_TEXSETS") && gp_ref_ok("MH_D3DTEX");
        if (g_draw_on && g_c.debris_style == 1 && !g_own_tex) gp_log("mayhem: model texture references differ: debris in procedural materials\n");
    }
    g_chain_on = g_c.chain; g_cover_on = g_c.cover;
    g_rng.s = GetTickCount() | 1u;
    g_installed = true;
    gp_log("mayhem: installed (%d car site(s)); chain reactions %s (radius %.1f m, delay %.2f..%.2f s, %.1f/s, depth %d, cars %d), "
           "breakable cover %s (health x%.2f, debris %d max, %.1f s, %d pieces, expose %.1f m), debris drawn %s (style %d: size x%.2f, grit %d, dust %d, prop textures %s)\n",
           sites, g_chain_on ? "ON" : "off", g_c.chain_radius, g_c.delay_min, g_c.delay_max, g_c.max_per_second, g_c.max_depth, g_c.chain_cars ? 1 : 0,
           g_cover_on ? "ON" : "off", g_c.cover_health_scale, g_c.debris_max, g_c.debris_life, g_c.debris_pieces, g_c.expose_radius, g_draw_on ? "yes" : "no",
           g_c.debris_style, g_c.debris_size, g_c.debris_bits, g_c.debris_dust, g_own_tex ? "yes" : "no");
    return true;
}

bool mprops_wants_blasts() { return g_installed; }

void* mprops_test_stub(const char* name) {
    if (!strcmp(name, "mh_car")) return (void*)&MhCarWrap;
    if (!strcmp(name, "mh_r_car")) return (void*)&R_MH_CAR;
    if (!strcmp(name, "mh_dohit")) return (void*)&MhDoHitWrap;
    if (!strcmp(name, "mh_r_dohit")) return (void*)&R_MH_DOHIT;
    if (!strcmp(name, "mh_blur")) return (void*)&MhBlurWrap;
    if (!strcmp(name, "mh_query")) return (void*)&MhQueryWrap;
    if (!strcmp(name, "mh_r_query")) return (void*)&R_MH_QUERY;
    if (!strcmp(name, "mh_r_blur")) return (void*)&R_MH_BLUR;
    return nullptr;
}
