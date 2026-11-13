// gp_glue.h - what gp_game.cpp shares with the feature modules that live outside dll/src (the Suave move:
// suave/code/suave_game.cpp). The modules use these helpers instead of their own copies, so there is one byte check,
// one patcher, one log and one SendMessage wrapper in the DLL.
#pragma once
#include <windows.h>
#include "gp_logic.h"
#include "todgameplay_shared.h"

// ---- provided by gp_game.cpp
void  gp_log(const char* fmt, ...);
bool  gp_send(DWORD node, int id, void* args);       // Node::SendMessage 0x869E60, script globals saved / restored
bool  gp_ref_ok(const char* id);                     // a REF row of sites.h matches in memory
bool  gp_site_ok(const char* id);                    // a SITE row's shipped bytes match
bool  gp_patch_call(const char* id, void* to);       // retarget the call of a CALL site (byte-checked first)
bool  gp_patch_bytes(const char* id, const void* bytes, int n);   // replace an OPER / DATA site's bytes (byte-checked first)
DWORD gp_cache_data();                               // *( *(0xA3B58C) + 0x18 ), 0 when not there
const gp::Ini& gp_ini();
int   gp_key_cfg(const char* section, const char* key, const char* def);
double gp_now();                                     // QPC seconds
void  gp_bt_force_off(const char* why);              // end the DLL's bullet time now (another move takes the clock)
bool  gp_bt_running();
bool  gp_cutscene();                                 // MASTER_CTRL ->+30h ->+D4h (the explosion handler's test)
// slow the game to `scale` (on) / back to normal (off) with the bullet-time calls; false (nothing done) when the DLL's
// bullet time runs, in exclusive modes / cut-scenes or when the game is already slowed
int   gp_playmode();                                 // Scene +5Ch (2 paused, 3 rewind), -1 unknown
// sets the mouse-turn accumulator (degrees) the next physics turn step consumes (the game's own yaw path); false when
// mouse_turn_fix is not patched or the player is not in mouse mode (the caller then writes PhysicCtrl.turn_speed)
bool  gp_yaw_add(float deg);

// ---- provided by suave/code/suave_game.cpp (all no-ops while [suave] enabled = 0)
void  suave_load_config();
bool  suave_enabled();                               // the ini asks for it (before install)
bool  suave_install(bool tick_ok);                   // patch the use-power-up gates; false = refused / failed
void  suave_input_tick(void* thread, double now);    // input tick (script context, player in control)
// main loop, after the input poll: physical keyboard state (before remap) for the cheat key, the game's keyboard
// arrays (edges / state, after remap) and mouse button dwords for the input lock and the automatic dive
void  suave_post_input(const BYTE* phys_state, BYTE* kb_edges, BYTE* kb_state, DWORD* mouse, double now, bool tick_fresh);
void  suave_publish(TodgpShared* sh);
void* suave_test_stub(const char* name);             // test host only

// ---- provided by smart_ai/code/smart_game.cpp (all no-ops while [smart_ai] enabled = 0)
void  smart_load_config();
bool  smart_enabled();                               // the ini asks for it (before install)
bool  smart_wants_tick();                            // the input tick (squad, morale, reaction stagger, save-restored enemies)
bool  smart_install(bool tick_ok);                   // patch the AI init call site; false = refused / failed
void  smart_tick(void* thread, double now);          // input tick (script context, player in control)
void* smart_test_stub(const char* name);             // test host only

// ---- provided by gang_wars/code/gw_game.cpp ([gang_wars]: rival gangs fight each other; all no-ops while enabled = 0)
void  gw_load_config();
bool  gw_enabled();                                  // the ini asks for it (before install)
bool  gw_wants_tick();                               // the input tick (it does everything there; no code site)
bool  gw_install(bool tick_ok);                      // reference checks; false = refused
void  gw_tick(void* thread, double now);             // input tick (script context, player in control)
void* gw_test_stub(const char* name);                // test host only

// ---- provided by seamless/code/seam_game.cpp (all no-ops while [seamless] enabled = 0)
void  seam_load_config();
bool  seam_enabled();                                // the ini asks for it (before install)
bool  seam_wants_tick();                             // the input tick (prefetch near a loader)
bool  seam_install(bool tick_ok);                    // patch the load controller sites; false = refused / failed
void  seam_tick(void* thread, double now);           // input tick (script context, player in control)
void* seam_test_stub(const char* name);              // test host only

// ---- provided by dll/src/gp_debug.cpp ([debug], all off unless the ini asks; private test copy only)
bool  gp_debug_load_config();                       // reads [debug]; installs the heap tracker when asked
bool  gp_debug_on();
bool  gp_debug_wants_input();                        // key script or stress run: needs the post-input hook / input tick
void  gp_debug_post_input(BYTE* kb_edges, BYTE* kb_state, double now);
void  gp_debug_tick(double now);                     // input tick: the stress swaps
void  gp_debug_swap(bool begin);                     // around every UpdateLoadedBlocks (seamless stubs): HeapValidate
bool  gp_debug_log_has(const char* text);           // the game's own log has TEXT ("N*TEXT": N times); waitlog of the scripts

// ---- provided by dll/src/gp_hang.cpp ([debug] hang_watchdog, on by default: when the frames stop, the stuck game
// thread's stack goes to the log and a minidump next to it; GOG exe only, started by install())
void  hang_load_config();
bool  hang_enabled();
void  hang_start();                                  // after install, once the per-frame hook is patched
void  hang_beat();                                   // game thread, every main-loop pass (start of POST_INPUT)
void  hang_post_input(const BYTE* kb_edges);         // end of POST_INPUT: the test-copy debug_stall_key
void  hang_loading(bool on);                         // around every UpdateLoadedBlocks (seamless stubs): a long frame
void  hang_where(const char* step);                  // the mod step the game thread is in (static text; shown in a report)

// ---- provided by dll/src/gp_pad.cpp ([pad]: controller support, prompts, rumble; off by default)
void  pad_load_config();
bool  pad_enabled();                                 // the ini asks for it (before install)
bool  pad_wants_post();                              // needs the per-frame hook (POST_INPUT)
int   pad_install(bool post_ok);                     // number of patched sites (0 = off)
void  pad_post_input(const BYTE* kb_edges, const DWORD* mouse, double now, float dt);
WORD  pad_xinput_buttons();                          // the active pad as XInput wButtons ([bullettime] pad_button)
void* pad_test_stub(const char* name);               // test host only

// ---- provided by dll/src/gp_fix.cpp ([fixes]: engine bug fixes, installed with any other feature)
void  gp_fix_load_config();
bool  gp_fix_wanted();
int   gp_fix_install();                              // number of patched sites
void* gp_fix_test_stub(const char* name);             // test host only

// ---- provided by dll/src/gp_display.cpp ([display]: 16:9 resolution table, wide HUD, pillarboxed menus; off by default)
void  display_load_config();
bool  display_enabled();                             // the ini asks for it (before install)
bool  display_wants_cull();                          // [display] cull_fix: turns on the [camera] culling-aspect fix
int   display_install();                             // number of parts installed
void* display_test_stub(const char* name);           // test host only

// ---- provided by physics/code/phys_game.cpp ([physics]: Box3D props prototype; all no-ops while [physics] enabled = 0)
void  phys_load_config();
bool  phys_enabled();                                // the ini asks for it (before install)
bool  phys_install(bool tick_ok);                    // load TODPhysics.dll, patch the bullet / explosion call sites
void  phys_tick(void* thread, double now);           // input tick: scan, step, write the props back
void  phys_post_input(BYTE* kb_edges, BYTE* kb_state, DWORD* mouse, double now);   // frame count, test-copy keys
void* phys_test_stub(const char* name);              // test host only
struct PhApi;
int   phys_world_gen();                              // +1 at every scan (the world and every body are recreated)
void  phys_blast(const float* c, float radius, float impulse, const char* who);   // a Box3D blast (props + debris)
void  phys_destroy_blast(const float* c);           // the same with [physics] destroy_radius / destroy_impulse

// ---- provided by mayhem/code/mh_game.cpp ([mayhem]: chain reactions, breakable cover; needs [physics])
void  mprops_load_config();
bool  mprops_enabled();                              // the ini asks for chain reactions / breakable cover (or a debug key)
bool  mprops_install(bool tick_ok);                  // after phys_install: refs, the car explosion sites
bool  mprops_wants_blasts();                         // installed: the physics blast wrappers report every blast
void  mprops_on_blast(int kind, DWORD node, const float* pos);   // 1 shell, 2 destroyable (queued, handled in the tick)
void  mprops_bullet_body(int body, const float* imp, const float* at);   // a bullet ray hit a Box3D body that is no prop
void  mprops_tick(void* thread, double now);         // input tick, after phys_tick
void  mprops_post_input(BYTE* kb_edges, BYTE* kb_state, double now);   // frame count, test-copy keys
void* mprops_test_stub(const char* name);            // test host only

// shared with [ragdoll]: the module, its function table while a world exists, the step clock's interpolation and frames
HMODULE phys_module();
const struct PhApi* phys_api();
bool  phys_world_ready();                            // installed, a world exists (after the first scan), the table loaded
float phys_alpha();
LONG  phys_frame();

// ---- provided by ragdoll/code/rd_game.cpp ([ragdoll]: active ragdolls on the [physics] world; no-ops while off)
void  ragdoll_load_config();                         // called by phys_load_config (the world is needed when it is on)
bool  ragdoll_enabled();
bool  ragdoll_install();                             // after phys_install: patch the character update + take_a_hit sites
void  ragdoll_world_reset();                         // physics rescan: simulated ragdolls freeze / go
bool  ragdoll_busy();                                // limp ragdolls falling: no rescan now
int   ragdoll_substeps(int base);                    // Box3D substeps while ragdolls simulate
void  ragdoll_before_step(float dt);                 // before each fixed physics step
void  ragdoll_after_step(float dt);                  // after it
void  ragdoll_step_cost(double ms);
void  ragdoll_input_tick(double now);                // input tick, after the physics step
void  ragdoll_post_input(BYTE* kb_edges, BYTE* kb_state, DWORD* mouse, double now);   // main loop pass: test-copy keys
bool  ragdoll_bullet(const float* from, const float* to, bool game_hit, DWORD hit_node);   // PH_BULLET segment
void  ragdoll_blast(const float c[3], float radius, float impulse, float lift);           // PH_SHELL / PH_DESTROY
void* ragdoll_test_stub(const char* name);           // test host only

// ---- provided by dll/src/gp_menu.cpp (the game's option menus; shared helper)
// on_get: after the game set the entry's CurrentIntegerValue (data +4Ch) from the settings (block load, every page
//         open); on_set: before the game stores a chosen value, tv = {OptionValueType1, index} (index may be changed).
typedef void (*GpMenuGetFn)(DWORD entry_node, DWORD entry_data);
typedef void (*GpMenuSetFn)(int* type_value);
bool  gp_menu_register(int option_type, GpMenuGetFn get, GpMenuSetFn set, const char* who);   // before gp_menu_install
int   gp_menu_install();                             // patches MN_GET_INIT / MN_GET_RESET / MN_SET once (any handler)
bool  gp_menu_installed();
int   gp_menu_value_count(DWORD entry_data);         // values offered (StringValueList length), -1 unknown
int   gp_menu_value_at(DWORD entry_data, int i);     // text ordinal of value i, -1 out of range
int   gp_menu_append_value(DWORD entry_data, int text_ordinal);   // ValueString<n+1> + list append; new count or -1
int   gp_menu_set_values(DWORD entry_data, const int* values, int n);   // the whole value list (ValueStrings untouched); count or -1
void* gp_menu_test_stub(const char* name);           // test host only

// ---- provided by dll/src/gp_mayhem.cpp ([mayhem_title]: MAYHEM burning under the title-screen logo; off by default)
void  mayhem_load_config();
bool  mayhem_enabled();                              // the ini asks for it (before install)
int   mayhem_install();                              // number of patched sites (0 = off)
void* mayhem_test_stub(const char* name);            // test host only

// ---- provided by dll/src/gp_difficulty.cpp ([difficulty]: EASY / NORMAL / HARD / BRUTAL in the SETTINGS menu)
void  difficulty_load_config();
bool  difficulty_enabled();
bool  difficulty_wants_tick();
bool  difficulty_install(bool tick_ok);
void  difficulty_tick(void* thread, double now);
void* difficulty_test_stub(const char* name);        // test host only
int   difficulty_level();                            // 0 easy .. 3 brutal, 4 psycho; -1 when [difficulty] is off
bool  df_friend_mission();                           // a buddy / escort / friend was seen since the mission started
bool  df_rearm_placeholder(DWORD node);              // a dead placement back in play (its stored DamageTaken cleared)
bool  difficulty_enemy_loco_moves();                 // PSYCHO is on: [psycho] enemy loco moves run (psycho/code/psycho_game.cpp)
bool  difficulty_owns_ammo();                        // [difficulty] tuning writes the BulletDrop bands (with loot_ammo_factor)

// ---- provided by drops/code/drops_game.cpp ([drops]: more ammo, health and the odd power-up from dead enemies)
void  loot_load_config();
bool  loot_enabled();                                // the ini asks for it (before install)
bool  loot_install(bool tick_ok);                    // reference checks only (no code site); false = refused
void  loot_tick(void* thread, double now);           // input tick (script context, player in control)
void  loot_post_input(BYTE* kb_edges, BYTE* kb_state, double now);   // test-copy keys
float loot_ammo_factor(float difficulty_factor);     // the dropped-ammo factor (the difficulty's own while [drops] is off)
void* loot_test_stub(const char* name);              // test host only

// ---- provided by psycho/code/psycho_game.cpp ([psycho]: PSYCHO's enemy loco moves; no-ops while enemy_loco_moves = 0)
void  psycho_load_config();
bool  psycho_enabled();                              // the ini asks for it (before install)
bool  psycho_install(bool tick_ok);                  // reference checks only (no code site); false = refused
void  psycho_tick(void* thread, double now);         // input tick (script context, player in control)
void  psycho_post_input(BYTE* kb_edges, BYTE* kb_state, double now);   // debug start key / test-copy dodge
void* psycho_test_stub(const char* name);            // test host only

// ---- provided by dll/src/gp_overlay.cpp (shared 2D overlay hooks: the Sprite per-frame render, the list executor and
// the end of the frame; one set of patches for [mayhem_title], [minimap] and the radar stage)
// match(sprite): 0 = not mine, 1 = call post(), 2 = post() and remember its submitted list, then draw() right after that
// list ran. post(sprite) runs right after the game built / submitted the sprite's list (it may edit the list's points).
struct IDirect3DDevice9; struct IDirect3DBaseTexture9; struct IDirect3DVertexShader9; struct IDirect3DPixelShader9;
struct IDirect3DVertexDeclaration9; struct IDirect3DVertexBuffer9;
typedef int  (*GpSpriteMatch)(DWORD sprite);
typedef void (*GpSpritePost)(DWORD sprite);
typedef void (*GpSpriteDraw)(DWORD sprite, DWORD list);
typedef void (*GpFrameDraw)();
typedef void (*GpExecDraw)(DWORD list);           // after every executed render list (e.g. a TextBox's own list)
int   gp_overlay_add_sprite(GpSpriteMatch match, GpSpritePost post, GpSpriteDraw draw, const char* who);   // client id or -1
bool  gp_overlay_set_pre(int client, GpSpriteDraw pre);   // also called right BEFORE the remembered list runs (draws under that sprite)
bool  gp_overlay_add_frame(GpFrameDraw draw, const char* who);
bool  gp_overlay_add_exec(GpExecDraw draw, const char* who);   // needs the sprite sites (patched when any sprite / list client exists)
int   gp_overlay_install();                          // after every client registered; number of patched sites
bool  gp_overlay_sprite_ok();
bool  gp_overlay_frame_ok();
volatile DWORD* gp_overlay_pending(int id);          // test host only
IDirect3DDevice9* gp_overlay_device();               // the game's device (renderer +0), nullptr when not a COM object
void* gp_overlay_test_stub(const char* name);        // test host only
// 2D overlay device state: every touched render / stage / sampler state, texture, FVF, shaders, declaration and stream 0
// is read before (false on a pure device: then do not draw) and written back after
struct GpOvState {
    DWORD rs[20]; DWORD ts0[8], ts1[2]; DWORD ss[6];
    IDirect3DBaseTexture9* tex; DWORD fvf; IDirect3DVertexShader9* vs; IDirect3DPixelShader9* ps; IDirect3DVertexDeclaration9* decl;
    IDirect3DVertexBuffer9* vb; UINT vb_off, vb_stride;
};
bool  gp_ov_save(IDirect3DDevice9* d, GpOvState* s);
void  gp_ov_set2d(IDirect3DDevice9* d);              // pre-transformed XYZRHW|DIFFUSE|TEX1, straight alpha blend, no z
void  gp_ov_restore(IDirect3DDevice9* d, GpOvState* s);

// ---- provided by dll/src/gp_hud.cpp ([minimap]: map in the radar, objective markers on the radar and in the world)
void  hud_load_config();
bool  hud_enabled();
int   hud_install();                                 // registers its overlay clients (0 = off)
void  hud_post_input(double now);                    // main loop: radar blips, target positions (POST_INPUT)
void* hud_test_stub(const char* name);               // test host only
// for other features (the mayhem-heat radar tint):
// the radar disc on screen this frame (pixels; false when the radar is not drawn) and a tint for the minimap ink
bool  hud_radar_rect(float* cx, float* cy, float* radius_px, float* unit_px);
void  hud_set_map_tint(DWORD argb);                  // multiplies the minimap ink (0xFFFFFFFF = none)


// ---- provided by dll/src/gp_hudstage.cpp (shared HUD stages)
// The RADAR stage is a sprite client of the shared overlay hooks (gp_overlay.cpp above).
// RADAR stage: the radar disc sprite (HUDAuxCtrl Radar/Radar_BG) tinted by vertex colour, and clients drawing right
// after it (above the disc, under the blips and every later HUD layer)
struct IDirect3DDevice9;
struct GpRadarFrame {
    DWORD sprite;                 // the Radar_BG node
    float P[4][2], UV[4][2];      // the quad the game built this frame: pixels TL, BL, BR, TR and their UVs (radar.BMP 128x128)
    float opacity;                // the sprite's opacity 0..1 (HUD fades)
    float W, H;                   // back buffer
    IDirect3DDevice9* dev;        // in a plain 2D state: XYZRHW|DIFFUSE|TEX1, premultiplied alpha blend (ONE, INVSRCALPHA),
                                  // stage 0 texture x diffuse, linear clamp; restored by the stage after the callbacks
    double now;                   // gp_now()
    DWORD frame;                  // counts radar draws
};
typedef void (*GpRadarDrawFn)(const GpRadarFrame& f);
const int GP_RADAR_TINT_SLOTS = 4;   // 0 mayhem heat, 1 hud / minimap, 2..3 free
bool  gp_radar_stage_install(const char* who);       // idempotent; also installs the sprite hooks
bool  gp_radar_stage_on();
// multiplicative tint of the radar disc (1,1,1,1 = none); the product of all slots is applied to the sprite's own colours
void  gp_radar_set_tint(int slot, float r, float g, float b, float a, const char* who);
bool  gp_radar_add_draw(GpRadarDrawFn fn, int order, const char* who);   // lower order draws first
bool  gp_radar_last_frame(GpRadarFrame* out);        // the quad of the last radar draw (any thread-safe use: copy)
DWORD gp_radar_sprite();                             // 0 until the radar was drawn once
void* gp_hudstage_test_stub(const char* name);       // test host only

// ---- provided by dll/src/gp_heat.cpp ([mayhem_heat]: the mayhem meter / wanted heat; off unless enabled)
void  heat_load_config();
bool  heat_enabled();                                // the ini asks for it (before install)
bool  heat_wants_tick();
int   heat_install(bool tick_ok);                    // number of patched sites (0 = off)
void  heat_tick(void* thread, double now);           // input tick: events, cooling, escalation requests
void* heat_test_stub(const char* name);              // test host only
// asked by gp_difficulty.cpp's reinforcement engine every pass (all zero / 1 when heat is off or cold)
struct HeatDrive { bool active; float share_add; int wave_due; int max_rearms; bool prefer_high; float high_m; int level; };
HeatDrive heat_drive();
void  heat_wave_spent(int n);                        // the engine re-armed n of the requested wave
bool  heat_ini_wants_reinforcements();               // [mayhem_heat] enabled and escalation on (read by difficulty_load_config)

// ---- provided by dll/src/gp_bigfights.cpp ([bigger_fights]: more enemies at once, heat vehicles; off unless enabled)
void  bf_load_config();
bool  bf_enabled();                                  // the ini asks for it (before install)
bool  bf_wants_tick();
bool  bf_install(bool tick_ok);                      // separate save slots + reference checks; false = refused
void  bf_tick(void* thread, double now);             // input tick
void* bf_test_stub(const char* name);                // test host only
// asked by gp_difficulty.cpp
int   bf_pool_shells();                              // character shells in the data (9 without the option)
int   bf_live_cap(int level, int game_value);        // the live cap on this level, 0 = no opinion
bool  bf_cap_wanted();
