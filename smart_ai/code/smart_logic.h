// smart_logic.h - the pure decisions of the "Smart AI" feature of TODGameplay.dll: no game memory, no Windows calls.
// Unit tested by smart_ai/code/test_smart.cpp (built and run by dll/build.bat). The game glue (smart_game.cpp) reads the
// enemy's own character_sentry_ai members, asks these functions what to change, and writes the answer back into the
// same members (the game's own state machine then acts on them).
#pragma once
#include <stdint.h>

namespace sm {

// ------------------------------------------------------------------------------------------------ random
struct Rng {                       // xorshift32: deterministic, seeded per enemy (tests repeat exactly)
    uint32_t s;
    explicit Rng(uint32_t seed = 1) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t u();
    float f();                     // [0, 1)
    float range(float a, float b); // [a, b)
};
uint32_t mix(uint32_t a, uint32_t b);   // seed mixer

// ------------------------------------------------------------------------------------------------ the sentry fields
// The character_sentry_ai members the feature reads or writes (offsets / touch slots in smart_game.cpp SM_FIELDS).
enum Field {
    F_WAIT_AGGR = 0,   // b:Wait_until_Aggressive (s): state 13 CALLs state 14 (stand, no fire) while it is not 0
    F_RETREAT,         // Retreat_Range (m): state 22 backs off while the target is closer (ranged fighters only)
    F_STRAFE_FROM, F_STRAFE_TO, F_CROUCH_FROM, F_CROUCH_TO, F_WAIT_FROM, F_WAIT_TO,   // state 20 action times (s)
    F_PRO_CROUCH, F_PRO_STRAFE, F_PRO_STRAFE_CROUCH, F_PRO_SD, F_PRO_SD_STRAFE_CROUCH, F_PRO_SD_CROUCH,
    F_PRO_CROUCH_SD_STRAFE, F_PRO_STRAFE_SD_STRAFE,                                    // state 20 weights (percent)
    F_STOPDIST,        // StopDistance (m): approach (17/18) ends here; 10 unless the placeholder set a custom range
    F_SD_MIN,          // ShootDodge_Min_dist (m): a shoot-dodge in state 20 only when the target is closer
    F_CS_HEALTHLEFT,   // CS_HealthLeft (percent): morale check, flee when health left < this ...
    F_CS_CHANGE,       // CS_Change_for (percent): ... and a roll < this (Morale 1 only)
    F_COUNT
};
enum { PRO_FIRST = F_PRO_CROUCH, PRO_COUNT = 8 };
const char* field_name(int f);

struct Sentry {                    // one enemy as read at spawn (after the game's own AI init 0x5D4190)
    float v[F_COUNT];
    float health = 100.0f;         // Health (+2Ch)
    int   morale = 1;              // b:Morale: 0 ChickenShit, 1 UseDefault, 2 StandFast
    int   is_buddy = 0;
    int   subtype = 0;             // sentry Sub_Type (4 escort, 7 special, 8 never aggressive)
    int   root_subtype = 0;        // character_root Sub_Type (9 civilian, 8 never aggressive)
    int   faction = 9;             // character_root FactionID (6 civilians, 13 wrestlers)
    int   dont_hunt = 0;
    Sentry();
};

// ------------------------------------------------------------------------------------------------ personality
// factions left alone by default: 6 civilians, 10 DEA (Ram's side: the 1000-hp agents of the post-game Harbour), 13
// wrestlers (special code paths)
const uint64_t DEFAULT_SKIP_FACTIONS = (1ull << 6) | (1ull << 10) | (1ull << 13);
uint64_t parse_factions(const char* list, uint64_t def);   // "6, 10, 13" -> bit mask (0..63); null / empty -> def
enum Role { ROLE_NONE = 0, ROLE_ASSAULT = 1, ROLE_RIFLEMAN = 2, ROLE_CAUTIOUS = 3 };
const char* role_name(int r);
struct PersonaCfg {
    bool  enabled = false;
    bool  reaction = true;         // staggered first reaction (needs the tick to clear it again)
    float react_min = 0.2f, react_max = 1.0f;   // s, only when the placeholder left Wait_until_Aggressive at 0
    float assault_share = 0.30f, cautious_share = 0.25f;   // the rest are riflemen
    float jitter = 0.25f;          // +/- fraction on the action times and the state 20 weights
    float assault_range = 0.70f, cautious_range = 1.35f;   // StopDistance factor (rifleman: 0.85..1.15)
    // Only values the row already has are scaled: a row without shoot-dodge (ShootDodge_Min_dist 0 or
    // every dodge weight 0) or without a back-off (Retreat_Range 0) never gets one - melee rows, defenders and special
    // types have no such moves by design.
    float dodge_bonus = 4.0f;      // percent added over the row's own non-zero shoot-dodge weights (assault x2, cautious x0.5)
    float boss_health = 1000.0f;   // enemies with this much health or more (bosses, specials such as the 1000-hp DEA) are left alone
    uint64_t skip_factions = DEFAULT_SKIP_FACTIONS;   // bit per FactionID left alone
};
struct Persona {
    bool  eligible = false;
    int   role = ROLE_NONE;
    float v[F_COUNT];              // new values (== the input where unchanged)
    uint32_t changed = 0;          // bit per Field written
    Persona();
};
bool    eligible(const Sentry& s, float boss_health, uint64_t skip_factions = DEFAULT_SKIP_FACTIONS);
bool    row_dodges(const float* v);   // the row has a shoot-dodge: ShootDodge_Min_dist > 0 and a dodge weight > 0
// tick_ok: the per-frame tick runs (it clears the reaction delay after the first use; without it no delay is set)
Persona make_persona(const PersonaCfg& c, const Sentry& s, Rng& r, bool tick_ok);
float   pro_sum(const float* v);   // sum of the eight state 20 weights

// should the tick clear the one-shot reaction delay? (the enemy got past state 14 into the fight)
bool reaction_used(int current_state);

// ------------------------------------------------------------------------------------------------ squad (approach tokens)
// At most max_advancing ranged enemies per target hunt (approach to StopDistance); the others get the game's own
// DontHunt and fight from where they stand (states 19/20 from range = covering fire), taking turns.
struct SquadCfg {
    bool  enabled = false;
    int   max_advancing = 2;
    int   min_squad = 3;           // fewer engaged ranged enemies on one target: everybody hunts (vanilla)
    float token_s = 8.0f;          // a token held this long by an enemy already fighting (19/20/22) is passed on
    float cooldown_s = 4.0f;       // ... and that enemy waits this long before it may take one again
    float hold_max_m = 22.0f;      // farther from the target than this: always hunts (would be out of weapon range)
    float stuck_s = 5.0f;          // holding in state 19 (no shot) longer than this: released to hunt
};
struct SquadIn {                   // per enemy, read this tick
    bool     managed = false;      // registered, eligible, not changed by anyone else
    bool     engaged = false;      // a combat state with a current target
    bool     ranged = false;       // not Is_Melee (melee ignore DontHunt in the game anyway)
    int      state = -1;           // Current_state
    uint32_t target = 0;           // CurrentTarget (group key)
    float    target_sqr = 0.0f;    // CurrentTargetSqrDist
};
struct SquadMem {                  // per enemy, kept between ticks
    bool     token = false;
    uint32_t token_since = 0, cooldown_until = 0, hold_since = 0, state_since = 0;
    int      last_state = -1;
    bool     holding = false;      // what the last tick decided (DontHunt wanted)
    bool     beyond = false;         // beyond hold_max_m (hysteresis 2 m)
    uint32_t hunt_until = 0;       // released after holding without a shot: hunts until then
};
// fills want_hold[i] (DontHunt wanted) for every member; returns the number of holders
int squad_assign(const SquadCfg& c, const SquadIn* in, SquadMem* mem, bool* want_hold, int n, uint32_t now_ms);
bool squad_engaged_state(int st);  // 13 aggressive, 17 approach, 18 advance, 19 hold, 20 fight, 22 back-off

// ------------------------------------------------------------------------------------------------ morale (fear)
// An ally dying nearby frightens the enemy: CS_HealthLeft / CS_Change_for are raised by its fear (0..1), which fades
// with game time; the game's own morale check (0x5C98C0, Morale 1 rows) then sends it to the flee state 23.
struct MoraleCfg {
    bool  enabled = false;
    float radius_m = 15.0f;        // an ally dying within this distance counts
    float per_death = 0.30f;       // fear added per ally death
    float last_man = 0.30f;        // extra when no engaged ally of the same faction is left within 2 x radius
    float decay_per_s = 0.04f;     // fear lost per game second
    float health_bonus = 80.0f;    // CS_HealthLeft += fear x this (high fear: flees even unhurt, if the roll passes)
    float max_chance = 25.0f;      // CS_Change_for += fear x this (percent per morale check)
    float boss_health = 1000.0f;
    uint64_t skip_factions = DEFAULT_SKIP_FACTIONS;
};
struct Fear { float f = 0.0f; };
void  fear_add(Fear& fr, float amount);
void  fear_decay(Fear& fr, const MoraleCfg& c, float game_dt);
// the two CS values for a base row and a fear level (fear 0 gives the base back exactly)
void  morale_values(const MoraleCfg& c, float base_hl, float base_cf, float fear, float* hl, float* cf);
float dist2(const float* a, const float* b);

} // namespace sm
