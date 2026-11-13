// gp_heat_logic.h - pure logic of [mayhem_heat] (the mayhem meter / wanted heat): heat points from chaos events,
// five levels, cooling step by step once Ram is out of the fight, the radar tint and edge pulse per level, the icon
// fill, and the escalation table (reinforcement share, waves, rooftop preference) per level. No game access here:
// tests/dll/test_heat.cpp tests all of it; dll/src/gp_heat.cpp feeds it events and draws it.
#pragma once
#include <stdint.h>

namespace ht {

const int LEVELS = 5;                          // icons / levels above "clean"

// ---- events (what the DLL sees in the game; dll/HOOKS.md "Mayhem heat")
enum Ev {
    EV_KILL_ENEMY = 0,   // Ram killed a hostile character (mission counter +28h path)
    EV_KILL_COP,         // Ram killed a police officer (faction 14)
    EV_KILL_CIVIL,       // Ram killed a civilian (faction 6 / Sub_Type 9)
    EV_KILL_NPC,         // a death Ram did not cause (crossfire, an enemy's grenade): chaos around him, small
    EV_SHELL,            // one of Ram's grenades / rockets / dynamite / molotovs exploded
    EV_DESTROY,          // a destroyable object broke (crates, glass, props)
    EV_BARREL,           // an explodeable destroyable blew up (barrels, gas tanks)
    EV_CAR,              // a vehicle exploded
    EV_LOCO,             // Ram started a loco move (Tornado, El Toro, Golden Gun, Mariachi, Pinata)
    EV_STYLE,            // combo action points (per point: style kills heat up faster)
    EV_COUNT
};
const char* ev_name(int ev);

struct Cfg {
    float pts[EV_COUNT] = {10.0f, 24.0f, 18.0f, 2.0f, 6.0f, 3.0f, 9.0f, 16.0f, 14.0f, 0.04f};
    float thr[LEVELS] = {30.0f, 90.0f, 170.0f, 270.0f, 390.0f};   // heat at which level 1..5 starts
    float cap = 460.0f;                // heat never goes above this (a little room above level 5)
    int max_level = 5;                 // 1..5: levels (and icons) above this are not used
    float decay_seconds = 10.0f;       // while cooling: seconds to lose one level
    float calm_seconds = 7.0f;         // no event of Ram's for this long = he left the chaos, cooling starts
    float near_factor = 0.3f;          // cooling speed while hostiles are still close (he is hiding, not gone)
    float gain = 1.0f;                 // all event points x gain (ini heat_gain)
    float own_weight = 1.0f;           // destruction / explosions Ram did not cause count x this (unknown culprit: 0.4)
};
// heat of the level boundaries with max_level applied: thr[i] for i < max_level, cap afterwards
float level_start(const Cfg& c, int level);    // level 0 -> 0
int level_of(const Cfg& c, float heat);        // 0 .. max_level

struct State {
    float heat = 0.0f;
    int level = 0, peak = 0;                   // level now / the highest level of this fight
    float since_event = 1e9f;                  // seconds since Ram's last event
    bool cooling = false;
    float shown = 0.0f;                        // the level the radar shows (follows `level` smoothly, 0..5)
    int ups = 0;                               // level-ups seen (wave triggers)
    int last_up_level = 0;
    void reset() { *this = State(); }
    // an event: its points x amount x gain; by_ram = false: x own_weight (and does not reset the calm clock)
    void event(const Cfg& c, int ev, float amount, bool by_ram);
    // per tick: dt real seconds while the player is in control; enemies_near = live hostiles close to Ram
    // returns the new level (and fills level-up bookkeeping)
    int tick(const Cfg& c, float dt, int enemies_near);
};

// ---- radar look
struct Rgb { float r, g, b; };
// the radar's vertex tint for a (smooth) level 0..5: 0 white, building heat warm orange, 5 red
Rgb tint_for(float shown_level, float strength);
// the shown level stepped toward the real level (smooth, ~0.5 s per level step, never overshoots)
float follow(float shown, int level, float dt);
// at the top level the radar edge pulses red <-> dark: returns the edge ring colour (premultiplied rgb) and alpha
struct Ring { float r, g, b, a; };
Ring edge_ring(float shown_level, int max_level, double t);
// icon i (0-based) of max_level: fill 0..1 (the next icon fills up while heat builds toward its level)
float icon_fill(const Cfg& c, float heat, int i);
// the top filled icon blinks gently while the heat cools (0..1 brightness factor)
float icon_glow(const State& s, int i, double t);

// ---- radar geometry (UV space of the game's radar.BMP: the disc and the arc of icons beside it)
struct Aff { float a, b, c, d, tx, ty; };      // x' = a u + b v + tx, y' = c u + d v + ty
// affine map from texture UV to the screen through three corners (P0 at UV0, P1 at UV1, P3 at UV3); false if degenerate
bool uv_affine(const float P[4][2], const float UV[4][2], Aff* out);
void aff_apply(const Aff& m, float u, float v, float* x, float* y);
const float DISC_U = 0.504f, DISC_V = 0.516f, DISC_R = 0.457f, RIM_R = 0.435f;   // measured from radar.BMP (128 x 128)
// centre (UV) of icon i of n along the arc outside the disc's upper right; size = icon edge in UV units
void icon_place(int i, int n, float scale, float* u, float* v, float* size);

// ---- escalation (what a level asks of the reinforcement engine in gp_difficulty.cpp)
struct Esc {
    float share_add = 0.0f;            // extra reinforcements per enemy death (BRUTAL's own 0.5 comes on top)
    int wave = 0;                      // reinforcements re-armed together when this level is reached
    float interval_s = 0.0f;           // ... and again every interval while the level holds and Ram keeps fighting
    int max_rearms = 1;                // how often one dead placement may come back
    bool prefer_high = false;          // pick placements above Ram first (rooftops, balconies, catwalks)
};
Esc escalation(int level, float scale);
// difficulty level (0 easy .. 4 psycho, -1 unknown) -> escalation scale factor
float difficulty_scale(int difficulty_level);

// ---- score multiplier: the heat level scales the score Ram earns, on top of the game's own
// combo multiplier (the game adds CurrentActionPoints x ComboMultiplier to OverallScore at the end of a combo; this
// factor scales that sum). No heat and level 1 = x1; then linear up to `top` at max_level (5 levels, top 2: x1.25 /
// x1.5 / x1.75 / x2 at levels 2..5).
float score_multiplier(int level, int max_level, float top);
// the scaled award: amount x mult rounded to the nearest point; only positive amounts are scaled; never overflows
int scale_score(int amount, float mult);
// the label text for a multiplier ("x1.25", "x1.5", "x2"); empty for x1 (nothing to show); returns the length
int score_label(float mult, char* out, int cap);

}  // namespace ht
