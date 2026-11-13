// gw_logic.h - pure logic of "gang wars" ([gang_wars] in TODGameplay.ini): which enemy belongs to which gang, when a
// gang member goes to war with a rival gang near it and when it calms down again, and what the game's own faction
// relation matrix must hold for that. No Windows, no game memory; unit tests in gang_wars/code/test_gw.cpp.
//
// How the game decides who shoots whom (research: gang_wars/README.md, dll/HOOKS.md "Gang wars"):
//   * every character root has a FactionID (root data +18h); the FactionTable holds a relation matrix
//     FactionRelations[my faction][other faction] (codes 1 friend, 2 neutral, 4 hostile, 8 self) for 19 base factions
//     and their 19 "modified" copies (faction + 19);
//   * the vision scan of every enemy asks the TargetDatabase for the targets whose relation from its faction has the
//     hostile bit (query_target_list(1, my faction, 4)) and picks the best one (TargetPriority, then distance);
//   * a hit from a NEUTRAL shooter moves the victim (and its friends of the same faction it knows) to the modified copy
//     of its faction and makes that copy hostile to the shooter - the game's own "provoke" rule;
//   * a death counts as an enemy kill (EnemiesKilledOnCurrentMission, logic kill events) when Ram's row holds 4 for the
//     victim's faction, whoever the killer was.
// Every hostile enemy of the shipped missions is faction 9 (EnemyFaction); the gangs differ only by model. Gang wars
// moves a gang member that has a rival gang member within range into a spare faction of its own (factions no shipped
// entity uses), whose relations are faction 9's except: hostile to the rival gangs, friends with the other gangs and
// with faction 9 (excluded enemies are never targeted by rivals), and - with provoke - neutral towards Ram, so the
// game's provoke rule turns exactly the members Ram shoots back on him.
#pragma once
#include <stdint.h>

namespace gw {

enum { MAX_GROUPS = 16, MAX_MODELS = 8, MAX_SPARES = 8, NAME_LEN = 16 };
enum Rel { REL_FRIEND = 1, REL_NEUTRAL = 2, REL_HOSTILE = 4, REL_SELF = 8 };
enum { FACTION_PLAYER = 0, FACTION_ENEMY = 9 };

struct Group { char name[NAME_LEN]; int models[MAX_MODELS]; int nmodels; };

struct Cfg {
    bool  enabled = false;
    float range = 25.0f;          // a rival gang member this close (m) starts the fight
    float hysteresis = 8.0f;      // ... and while one is within range + hysteresis the fight goes on
    float calm_s = 8.0f;          // no rival that close for this long (game s): back to the game's own faction
    float idle_s = 6.0f;          // at war but no rival as its own target for this long (walls, out of sight): back too
    float retry_s = 12.0f;        // ... and no new war for this long (it hunts Ram meanwhile)
    bool  provoke = true;         // while at war a gang ignores Ram until he shoots one of them (the game's own rule)
    bool  spark = false;          // a fight starts only when Ram shoots a gang member while a rival is within range
    float spark_radius = 40.0f;   // members within this of the spark join that fight
    float boss_health = 1000.0f;  // bosses / specials (health >= this) are never moved
    int   enemy_faction = FACTION_ENEMY;
    Group groups[MAX_GROUPS]; int ngroups = 0;
    bool  hostile[MAX_GROUPS][MAX_GROUPS] = {};
    int   spares[MAX_SPARES] = {4, 16, 17, 3}; int nspares = 4;
};

// ---- configuration parsing (ini values)
int  parse_ints(const char* s, int* out, int cap);                    // "0, 1 ,6" -> count (non-numbers skipped)
int  add_group(Cfg& c, const char* name, const char* models);          // index or -1 (full / bad name / no models)
int  find_group(const Cfg& c, const char* name);
int  parse_pairs(Cfg& c, const char* s, char* bad, int bad_len);       // "a-b, c-d": pairs set; unknown names in bad
void default_groups(Cfg& c);                                           // the shipped gangs (README table)
int  group_of_model(const Cfg& c, int model);                          // -1 = no gang
bool any_rival(const Cfg& c, int g);                                   // g has a hostile pair at all

// ---- faction allocation and the relation matrix
// alloc[g] = spare faction given to group g, -1 = none. A member at war has faction alloc[g]; provoked (by Ram, the
// game's rule or the spark) alloc[g] + base (the game's modified copy of it).
struct Design { int base = 19; int alloc[MAX_GROUPS]; bool provoke = true; int enemy = FACTION_ENEMY; Design() { for (int& a : alloc) a = -1; } };
// owner of a faction id: group (or -1) and whether it is the provoked copy
int  owner(const Cfg& c, const Design& d, int faction, bool* provoked);
bool is_spare(const Cfg& c, const Design& d, int faction);             // a spare or its modified copy (rows we own)
// The value cell (r, c) must hold, or -1 = not ours (leave it). vanilla(r, c) = the game's value captured before we
// wrote anything (the spare rows / columns are restored to it when the spare is not given to a group).
typedef int (*VanillaFn)(void* ctx, int r, int c);
int  desired_cell(const Cfg& cfg, const Design& d, int r, int c, VanillaFn vanilla, void* ctx);
// give a free spare to g (-1 = none left); release a spare
int  alloc_spare(const Cfg& c, Design& d, int g);
void release_spare(Design& d, int g);

// ---- war state per member
enum State { ST_CALM = 0, ST_WAR = 1, ST_PROVOKED = 2, ST_RELEASED = 3 };
struct Member {
    int   id = 0;                 // caller's handle
    int   group = -1;             // -1 no gang
    bool  eligible = false;       // may be moved (not a buddy / boss / special / VIP / mounted gunner ...)
    bool  alive = false;
    bool  has_pos = false;
    float pos[3] = {0, 0, 0};
    int   faction = -1;           // what the root holds now
    int   orig = -1;              // the game's own faction (at first sight)
    int   state = ST_CALM;
    float calm_t = 0.0f;          // game seconds without a rival near (while at war)
    bool  ram_hit = false;        // this tick: Ram's hit landed on it (DamageTaken rose, Take_a_hit_owner = Ram)
    bool  target_rival = false;   // this tick: the game's own target of it is a rival gang member
    float idle_t = 0.0f;          // game seconds at war without a rival target
    float cooldown_t = 0.0f;      // game seconds before it may go to war again
};
struct Spark { bool active = false; float pos[3] = {0, 0, 0}; float quiet_t = 0.0f; };
struct Out { int want_faction = -1; bool changed = false; const char* why = ""; int rival = -1; float rival_d = 0.0f; };

float dist(const float* a, const float* b);
// nearest eligible live rival of member i within r (index into m[], -1 none)
int  nearest_rival(const Cfg& c, const Member* m, int n, int i, float r, float* d);
// One step over all members (dt = game seconds since the last step). Decides each member's state and the faction it
// must hold (out[i].want_faction, -1 = leave the root alone). Allocates / releases spares in d.
void step(const Cfg& c, Design& d, Member* m, int n, Spark& sp, float dt, Out* out);

const char* state_name(int s);

}  // namespace gw
