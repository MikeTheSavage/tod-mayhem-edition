// gp_difficulty_logic.h - pure logic of [difficulty]: the four menu levels, the runtime tuning
// factors (the same factors as the data option `difficulty` in data/gameplay/options.py), which dictionary rows are
// tuned, the menu value mapping (BRUTAL is stored as the game's Hard + a flag), the settings sidecar file and the
// Brutal reinforcement accounting. No game access here: tests/dll/test_difficulty.cpp tests all of it.
#pragma once
#include <stdint.h>

namespace df {

// Levels above HARD are "extra" levels: stored as the game's Hard (Setting_difficulty 2) plus extra = level - HARD in
// the DLL's sidecar, offered as menu values 4.. (ValueString4 / 5). PSYCHO (enemies with loco moves, a later feature)
// is BRUTAL's tuning and enemy count for now; difficulty_enemy_loco_moves() in gp_glue.h is the hook for that work.
enum Level { EASY = 0, NORMAL = 1, HARD = 2, BRUTAL = 3, PSYCHO = 4, LEVEL_MAX = PSYCHO };
const int EXTRA_MAX = LEVEL_MAX - HARD;             // 2: BRUTAL, PSYCHO
const char* level_name(int lv);
int level_from_name(const char* s);                 // "easy" .. "psycho", -1 otherwise

// The game's Setting_difficulty is 0 / 1 / 2 (Easy / Normal / Hard). An extra level is stored as 2 plus the DLL's
// extra (1 BRUTAL, 2 PSYCHO), so Slot11 never holds a value the vanilla menu cannot show. Out of range -> clamped.
int level_from_setting(int setting, int extra);

// ---- runtime tuning (options.py DIFFICULTY 'hard' / 'brutal'); EASY and NORMAL are vanilla (every factor 1)
enum Target { T_DAMAGE_TAKEN = 0, T_DAMAGE_DEALT, T_SPREAD, T_PAUSE, T_REACT, T_AMMO, T_COUNT };
struct Factors { float k[T_COUNT]; };
Factors factors(int level);
float tuned(int target, float base, int level);     // base x factor (spread: x factor as well; easy's +1.5 deg is gone)

// which rows the data option tunes (options.py gen_difficulty): firearms Specific_ID 0..9, every CharacterDict
// behaviour row but the player's (Specific_ID 7) and every ModelStat row but Ram / Ernesto / Tommy (Model_type 20/10/18)
bool weapon_row_tuned(int specific_id);
bool chartype_row_tuned(int specific_id);
bool modelstat_row_tuned(int model_type);

// The installed data zip may still carry the STATIC data option (difficulty = hard / brutal / easy baked into
// PlayerStatusCtrl): then the runtime tuning would apply twice. Vanilla data: DamageHard 1.4, PlayerDamageMod 1.0.
// Returns the static level found (EASY / HARD / BRUTAL), NORMAL for vanilla data, -1 for values matching no level.
int static_level(float damage_hard, float player_damage_mod);

// ---- menu (OptionValueType1 3 = the difficulty entry)
// after the game set CurrentIntegerValue from the setting: 2 -> 2 + extra when the entry offers that many values
// (an extra level whose value is missing shows the highest one offered)
int menu_shown_value(int current, int extra, int value_count);
// before the game writes the chosen index into Setting_difficulty: index >= 3 -> 2 + extra (index - 2, at most
// EXTRA_MAX); 0..2 -> unchanged, extra 0
struct SetResult { int setting; int extra; };
SetResult menu_chosen(int index);

// ---- sidecar file next to Slot11 ("TODGameplay_difficulty.txt": "level = brutal"; the first build's "brutal = 1"
// is read too). parse: extra 0..EXTRA_MAX
bool parse_sidecar(const char* text, int* extra);
int format_sidecar(int extra, char* out, int cap);

// ---- Brutal reinforcements: every enemy death adds `share` (0.5 = +50 %) to an accumulator; a dead enemy's
// placeholder is re-armed (one more enemy from the pool) while the accumulator holds a whole one.
struct ReinfCfg {
    float share = 0.5f;                 // extra enemies per enemy death (BRUTAL)
    float share_psycho = 0.375f;        // ... on PSYCHO (0.5 x 0.75: 25 % fewer than BRUTAL)
    float min_distance = 18.0f;         // Ram at least this far from the placeholder (no spawn in his face)
    float max_distance = 45.0f;         // ... and within the pool's placeholder radius (45 m: CharacterResourceCtrl)
    float delay_s = 4.0f;               // game-clock seconds after the death before it may come back
    float boss_health = 1000.0f;        // >= : 1000-hp specials are never brought back
};
// the level's own share (BRUTAL share, PSYCHO share_psycho, 0 below BRUTAL); the heat's share is added on top by the caller
float level_share(int level, const ReinfCfg& c);
struct ReinfState {
    float acc = 0.0f;
    int deaths = 0, rearmed = 0;
    void reset() { acc = 0.0f; deaths = 0; rearmed = 0; }
    void on_death(const ReinfCfg& c);            // count a counted enemy's death
    bool take();                                 // one whole reinforcement available? (consumes it)
};
// a placeholder seen with a live enemy is worth bringing back
struct Seen { int faction = -1; float health = 0.0f; int sub_type = -1; bool buddy = false; };
bool reinforcement_candidate(const Seen& s, const ReinfCfg& c, const int* skip_factions, int n_skip);
// a skipped faction's modified copy (faction + 19, the game's provoke rule: a DEA friend Ram hit is faction 29) is skipped too
bool faction_skipped(int faction, const int* skip_factions, int n_skip);
// MaxAllocated to write on BRUTAL / PSYCHO: the ini's max_live_enemies, never above pool_shells - 1 (the game itself never
// hands out the last of its 9 shells: its own values are 6 and 8), and the game's own value in a mission with a buddy,
// escort or friend (a fight that uses every shell starves the mission's own characters: MM013 failed that way)
int live_cap(int requested, int game_value, int pool_shells, bool friend_mission, bool cap_with_friends);
// a re-armed placement's stored DamageTaken: the AI init copies it into the new enemy, so it must be cleared (0) or the
// reinforcement is born dead (DamageTaken >= Health: one hit kills it, other features take it for a corpse)
bool born_dead(float health, float damage_taken);
bool distance_ok(float dist, const ReinfCfg& c);

}  // namespace df
