// gp_bigfights_logic.h - the rules of "Bigger Fights" ([bigger_fights]), free of game addresses so the unit tests can
// run them. Game side: gp_bigfights.cpp. Data side: data/gameplay/bigger_fights.py.
#pragma once

namespace bf {

enum { EASY = 0, NORMAL = 1, HARD = 2, BRUTAL = 3, PSYCHO = 4 };
enum { TIER_MAIN = 1, TIER_PSYCHO = 2, TIER_FRIEND = 3 };   // 3 = the smaller share in a block with buddies or an escort

struct Cfg {
    bool  enabled = false, separate_saves = true, clones = true, squads = true, log = true;
    float cap_brutal = 1.5f, cap_psycho = 1.3f;      // the game's live cap (6 or 8) x this (PSYCHO 1.85 until psycho-tune)
    float wake_share_psycho = 0.6f;                  // PSYCHO: this share of the clones of each tier wakes (psycho-tune)
    float wake_m = 90.0f;                            // a clone wakes when its donor is this close to Ram
    float join_m = 30.0f;                            // a live clone this close to Ram is told to fight him at once
    int   wakes_per_tick = 6;
    int   weapon_add = 8, wear_add = 3;              // above the game's 15 / 6, at most the props the data added
    int   force_tier = -1;                           // test copy: 0 none, 1 BRUTAL's, 2 PSYCHO's
    bool  squads_in_missions = true;                 // vehicles also in missions without buddies
    float squad_every_s = 30.0f;                     // at most one new vehicle per this many seconds
    float squad_min_m = 12.0f, squad_max_m = 34.0f;  // where the track ends: around Ram
    float squad_far_m = 110.0f;                      // ... or the nearest road this close when he is off the roads
    float squad_run_m = 80.0f;                       // how far the vehicle drives in
    float squad_ttl_s = 150.0f;                      // a vehicle's squad is given up after this long
    int   squad_cars_l3 = 1, squad_cars_l4 = 2, squad_cars_l5 = 3;
    int   force_heat = 0;                            // test copy
    bool  debug_stats = false;                       // test copy: fight counts also without the data
};

// "BF1_012" -> tier 1, serial 12; false for anything else (the clone placeholder "BF1_012e", the squads)
bool parse_clone_folder(const char* name, int* tier, int* serial);
// does a clone of this tier wake on this level (-1 = [difficulty] off); force >= 0 replaces the level rule
bool tier_wakes(int tier, int level, int force_tier);
// is serial s (1, 2, ...) one of the `share` picked ones? Even spread, deterministic: 0.6 picks 1, 2, 4, 6, 7, 9, ... (3 of 5)
bool share_pick(int serial, float share);
// does this clone wake: its tier's rule, and on PSYCHO (or force_tier 2) only the wake_share_psycho share of the serials
bool clone_wakes(int tier, int serial, int level, const Cfg& c);
// the live cap on this level: game_value x scale, never below the game's, at most shells - 1 (the spare shell the
// resource controller needs: 0x7660E0 asks for a free shell before it would steal one); 0 = leave the game's value
int live_cap(int level, int game_value, int shells, const Cfg& c);
// the save slot format: a game slot goes to BFSlotNN, slot 11 (the settings file) is shared
const char* slot_format(int index, const char* game_format, bool separate);
// i-th spot around a donor (rings of 8 at 2.2 / 3.4 / 4.6 m, each clone starting at another angle); false past the end
bool ring_spot(int i, int seed, float* dx, float* dz);
// the intermissions (the game's numbers): free roam
bool is_free_roam(int mission);
// how many vehicles a heat level asks for
int squads_for_heat(int heat, const Cfg& c);
// may a new vehicle start now? (the game has 4 car slots, 5 shells)
bool squad_may_start(int heat, int alive_cars, int game_cars_busy, bool friend_mission, bool cutscene, bool in_mission, const Cfg& c);

}  // namespace bf
