// psycho_logic.h - pure logic of PSYCHO's enemy loco moves (no game reads, no Windows): configuration, which enemy may
// start which move, the per-enemy cooldowns, and the move's own clock (telegraph -> active -> recover) with every
// decision the game glue (psycho_game.cpp) turns into the game's own calls. Unit tested by test_psycho.cpp.
//
// Moves of phase 1 (self-contained: an animation, the enemy's own gun / body, a damage area that only hurts Ram):
//   TORNADO  the Tornado's spin (Ram's ram_locomove clip on the enemy): the clip's own wind-up is the telegraph, then the
//            spin with the enemy's gun firing blanks; Ram takes hits while he is in range, in sight and not diving
//   TORO     El Toro's charge (ram_rammove_idle stance as the telegraph, ram_rammove_run): a straight run along the line
//            to where Ram stood when the telegraph ended; touching Ram hits him once, a wall or the distance ends it
#pragma once
#include <stdint.h>

namespace ps {

enum Move { MOVE_NONE = 0, MOVE_TORNADO = 1, MOVE_TORO = 2, MOVE_COUNT = 3 };
const char* move_name(int m);
int move_from_name(const char* s, int n);           // -1 unknown (case-insensitive, first n chars)
unsigned parse_moves(const char* s);                // "tornado, toro" -> bit mask (1 << Move); 0 = none recognised
bool parse_range(const char* s, float* lo, float* hi);   // "20-40", "20,40", "20 40" or "30" (lo = hi)

struct Vec3 { float x, y, z; };
inline Vec3 v3(float x, float y, float z) { Vec3 v = {x, y, z}; return v; }
Vec3 sub(const Vec3& a, const Vec3& b);
Vec3 add(const Vec3& a, const Vec3& b);
Vec3 mul(const Vec3& a, float k);
float dist_xz(const Vec3& a, const Vec3& b);
float dot_xz(const Vec3& a, const Vec3& b);
Vec3 norm_xz(const Vec3& a);                        // horizontal unit vector (0,0,1 when degenerate)
float yaw_of(const Vec3& dir);                      // atan2(x, z)

struct Cfg {
    bool  enabled = false;                          // [psycho] enemy_loco_moves
    unsigned moves = (1u << MOVE_TORNADO) | (1u << MOVE_TORO);
    float cooldown_min = 20.0f, cooldown_max = 40.0f;   // game s between two moves of one enemy (random in the range)
    float first_delay = 6.0f;                       // game s an enemy must have been seen before its first move
    float global_gap = 5.0f;                        // game s after any enemy move ended before the next may start
    float telegraph = 1.0f;                         // game s of wind-up (flash, glow, sound) before the move hits
    bool  elite_only = true;
    float elite_health = 200.0f;                    // Health (max hp) that makes an elite (CMLong 200, Soldier 225)
    float boss_health = 1000.0f;                    // Health from which an enemy is a boss
    bool  bosses = false;                           // [psycho] bosses: 0 = bosses never use a move (they are mission
                                                    // actors driven by the mission script; one froze in a test)
    float chance = 0.35f;                           // roll per eligible enemy per check
    float check_interval = 0.5f;                    // game s between two picks
    float retry_delay = 2.0f;                       // an enemy refused by the line test waits this long
    float interrupt_damage = 0.5f;                  // damage taken during the telegraph that cancels the move
    float interrupt_cooldown = 0.5f;                // cooldown factor after an interrupted move (x the rolled cooldown)
    bool  dive_dodges = true;                       // Ram in a shoot-dodge is not hit
    float recover = 0.5f;                           // game s standing after the move before the AI gets control back
    // Tornado
    float tornado_min = 2.5f, tornado_max = 11.0f;  // Ram's distance for a start
    float tornado_radius = 12.0f;                   // hits reach this far (line of sight needed)
    float tornado_damage = 25.0f;                   // per hit, before the enemy's Damage_mod and the difficulty factor
    float tornado_hit_interval = 0.35f;             // game s between hits while Ram is exposed
    float tornado_fire_speed = 0.4f;                // clip speed in the firing frames (the Tornado runs them at 0.25)
    float tornado_blank_interval = 0.09f;           // game s between two blank shots of the enemy's gun
    float tornado_windup_frames = 19.0f;            // the clip's shoot-start event frame (ram_locomove, 25 fps)
    float tornado_fire_frames = 14.0f;              // frames 19..33 fire (event 33 = shoot stop)
    float tornado_clip_frames = 61.0f;
    // El Toro
    float toro_min = 5.0f, toro_max = 16.0f;        // Ram's distance for a start
    float toro_speed = 9.0f;                        // m per game s (El Toro's move_speed 8 / 10 / 8; the clip's root 11.17)
    float toro_overrun = 3.0f;                      // the charge runs this far past where Ram stood
    float toro_max_seconds = 2.5f;
    float toro_hit_radius = 1.3f;                   // horizontal distance that counts as a hit
    float toro_damage = 60.0f;
    float toro_impulse = 9.0f;                      // m/s of the hit's push (along the charge, plus a third of it up)
    float toro_turn_rate = 6.0f;                    // rad per game s the enemy turns to face Ram during the telegraph
    // Elite drops (psycho_drops.inl): an elite that dies leaves its ability as a pickup
    bool  drops = false;                            // [psycho] elite_drops
    float drop_seconds = 30.0f;                     // how long a drop stays
    int   drop_max = 3;                             // drops on the ground at once (of the pool's 5 goodie props)
    float drop_track_radius = 45.0f;                // m around Ram: elites are watched this far (gang wars' kills count)
    float drop_settle = 1.5f;                       // game s a body has at most to come to rest (ragdoll)
    float drop_body_reach = 3.0f;                   // the body further than this from where the enemy stood: use where it stood
    float drop_body_drop = 1.5f;                    // the same, vertically
    float drop_give_up = 15.0f;                     // game s a drop waits for a free prop before it is given up
    void clamp();
};

// ------------------------------------------------------------------ what the glue reads each check (one per character)
struct Cand {
    uint32_t root = 0, ai = 0;                      // the pooled shell (root and AI stay the same when the pool reuses it)
    uint32_t ph = 0;                                // its character placeholder (changes when the shell is reused)
    uint32_t uid = 0;                               // the placement's stable id (placeholder Entity id; shell id without one)
    int   ability = MOVE_NONE;                      // the fixed move of this enemy (EliteBook), NONE = work it out from uid
    Vec3  pos = {0, 0, 0};
    float health = 0, damage = 0;                   // sentry Health (max) / DamageTaken
    int   state = -1;                               // sentry Current_state
    int   faction = -1;
    bool  hostile = false, buddy = false, has_gun = false, sentry = false;
    bool  special = false;                          // VIP placement, escort / special sub type, scripted AI state
};
bool alive(const Cand& c);
bool in_combat(int state);                          // 13 dispatcher, 17 approach, 18 advance, 19 hold, 20 ranged fight, 21 melee, 22 back-off / dodge, 25 action break (not 4-6 passive / guard, 15 authored run, 23 flee, 28 dead)
bool is_elite(const Cfg& c, const Cand& e);
bool is_boss(const Cfg& c, const Cand& e);

struct World {
    double t = 0;                                   // game seconds (the game-ms clock / 1000)
    Vec3  ram = {0, 0, 0};
    bool  ram_ok = false;
    bool  ram_dodging = false;                      // a shoot-dodge state
    bool  ram_busy = false;                         // vehicle, cut-scene, own loco move, dead, rewind, pause
    bool  psycho = false;                           // the difficulty is PSYCHO
};

// ------------------------------------------------------------------ per-enemy memory (cooldowns)
struct Known { uint32_t root, ai, ph; float health; double first_seen, next_ok; int moves; bool used; };
struct Registry {
    static const int N = 64;
    Known k[N];
    Registry() { clear(); }
    void clear();
    Known* get(uint32_t root, uint32_t ai, double now, const Cfg& c);   // adds an unknown enemy (first_seen = now)
    // the same, for this occupant of the shell: a different placeholder or max Health (the pool gave the shell to a new
    // placement) starts the entry over, so first_delay counts from the new enemy's arrival (0 = not known, not compared)
    Known* get(uint32_t root, uint32_t ai, double now, const Cfg& c, uint32_t ph, float health);
    Known* find(uint32_t root, uint32_t ai);
    int count() const;
};

struct Rng { uint32_t s = 0x9E3779B9u; uint32_t next(); float f01(); float range(float a, float b); };

// ------------------------------------------------------------------ the fixed ability (elite drops)
// One enemy, one move, for good: a pure hash of the placement's stable id, so the same placement has the same move in every
// run, after a reload and in every reinforcement that re-arms it. The gun in hand plays no part: at first sight an enemy
// often has none out yet. (An enemy without a gun cannot spin; it still drops the Tornado it was given.)
int ability_for(uint32_t uid, unsigned moves);
int goodie_of_move(int move);                       // GoodiesDict id: Tornado 10, El Toro 11 (0 = none)
int ability_of(const Cfg& c, const Cand& e);        // e.ability, or worked out

// may this enemy leave a drop when it dies? alive, hostile, an elite, not a buddy / VIP / escort / scripted, no boss unless
// bosses = 1
bool drop_eligible(const Cfg& c, const Cand& e);

struct Elite {
    uint32_t key = 0, uid = 0;                      // the placement (placeholder, shell root without one) / its stable id
    uint32_t root = 0, ai = 0, ph = 0, body = 0;    // the shell it stands in now, its pelvis
    int    ability = MOVE_NONE;
    bool   used = false;
    bool   alive_seen = false;                      // seen alive in this shell: only that can die (a born-dead shell never drops)
    bool   dead = false;                            // died, the clock of its death in t_dead
    bool   no_drop = false;                         // died in a cut-scene or during a rewind
    bool   dropped = false;                         // this placement's one drop was made
    bool   collected = false;                       // ... and Ram took it
    double t_dead = 0, next_try = 0, t_drop = 0;    // t_drop: when this placement's drop was made
    Vec3   last_alive = {0, 0, 0};                  // where it stood (always a place a character can be)
    Vec3   rest = {0, 0, 0};                        // the body, last read
    double t_rest = 0;                              // when the body last moved
    bool   rest_ok = false;
    // seen alive in this shell (a new placement, or the same one back: a reinforcement, a heat car's second trip, in any
    // shell). What it dropped stays with the placement.
    void   saw(const Cand& c);
};
struct EliteBook {
    static const int N = 128;
    Elite e[N];
    int   used = 0;
    void  clear();
    Elite* find(uint32_t key);
    // full: the oldest finished entry goes. Never one that dropped: a placement that is re-armed later would drop again
    Elite* add(uint32_t key, uint32_t uid, int ability);
    int   count() const { return used; }
};
enum Life { LIFE_ALIVE = 0, LIFE_DEAD = 1, LIFE_GONE = 2 };
// what a tracked enemy's shell says now: same_shell = it still holds this AI and placeholder
int  life_of(bool same_shell, float health, float damage, int state);
// the body came to rest (moved < 2 cm for 0.25 s) or its time is up: may the drop be placed?
bool body_settled(const Cfg& c, const Elite& e, double t);
// the place of the drop: the body when it lies near where the enemy stood and the way there is clear, else where it stood.
// body_ok = the body was readable and has ground under it, clear = no wall between the two
Vec3 drop_spot(const Cfg& c, const Vec3& last_alive, const Vec3& body, bool body_ok, bool clear, bool* used_body);
// is a drop still owed for this death? One per placement: a re-armed placement that dropped in an earlier life is owed none
bool wants_drop(const Elite& e);
// a rewind took the clock back past the death: the death never happened. Its drop goes (true = free the prop); one Ram
// collected stays collected, so the enemy cannot drop it twice. A drop made in an earlier life stays that life's: the
// placement keeps its one drop.
bool undo_death(Elite& e, double t);
// after a clock jump of `back` game s: is this a load / retry (the book starts over) rather than a rewind (at most 8 s)?
bool clock_jump_is_load(double back);

// a pick: candidate index and move, or -1
struct Pick { int index = -1; int move = MOVE_NONE; float distance = 0; const char* why = ""; };
// every check_interval: the eligible enemies (alive, hostile, in a fight, elite or boss, cooled down, in range for an
// enabled move), one roll each, the nearest passing roll wins. why = the reason nothing was picked (for the log).
Pick pick(const Cfg& c, Registry& reg, const World& w, const Cand* cand, int n, Rng& rng, bool any_active, double gap_until);
int moves_for_distance(const Cfg& c, float d, bool has_gun);
int move_for_distance(const Cfg& c, const Cand& e, float d);   // the enemy's own move when this distance allows it, else NONE
// SoundDict ids of Ram's own voice lines (StreamedPowerMoves "loco move_01/02", "Ram move_01"): never on an enemy
bool is_ram_voice(int sound_id);
int  sting_sound(int configured);                   // the sting an enemy move plays: -1 (none) for Ram's lines / < 0
// after a move: the enemy still needs the hand-back (its AI in state 2 = no AI control). A rewind restores the enemy as it
// was a few seconds earlier, i.e. in the middle of the move, after the move had already given it back.
bool needs_handback(int ai_state);
const char* reject_reason(const Cfg& c, Registry& reg, const World& w, const Cand& e);   // "" = may start (before the roll)   // bit mask of enabled moves this distance allows

// ------------------------------------------------------------------ the running move
enum Phase { PH_IDLE = 0, PH_TELEGRAPH = 1, PH_ACTIVE = 2, PH_RECOVER = 3 };
const char* phase_name(int p);

enum End { END_NONE = 0, END_DONE, END_INTERRUPTED, END_ENEMY_DEAD, END_RAM_BUSY, END_LOST, END_WALL, END_HIT, END_DISTANCE, END_TIME };
const char* end_name(int e);

struct Step {                                       // what the glue does this frame (all flags reset by step())
    bool  go_active = false;                        // telegraph over: switch clip / speed, kill the glow
    bool  blank = false;                            // fire one blank shot of the enemy's gun
    bool  hit = false; float hit_damage = 0; Vec3 hit_impulse = {0, 0, 0};
    bool  miss = false; const char* miss_why = "";  // a hit tick that did not land (log)
    bool  face = false; float face_yaw = 0;         // turn the enemy to this world yaw (telegraph of the charge)
    bool  place = false; Vec3 place_pos = {0, 0, 0};// put the enemy here (the charge, when its own motion lags)
    bool  recover = false;                          // active over: idle clip
    bool  release = false;                          // give the enemy back to its AI (move finished / cancelled)
    int   end = END_NONE;
    float clip_speed = -1.0f;                       // >= 0: set the enemy's animation speed
};

struct EnemyNow { bool alive = false; float damage = 0; Vec3 pos = {0, 0, 0}; bool los_ok = true; bool wall_ahead = false; };

struct Active {
    int    move = MOVE_NONE, phase = PH_IDLE, end = END_NONE;
    uint32_t root = 0, ai = 0;
    double t_phase = 0, t_start = 0, t_last = 0;    // game s
    float  damage0 = 0;
    Vec3   start = {0, 0, 0}, dir = {0, 0, 1}, target = {0, 0, 0};
    float  limit = 0;                               // charge length (m)
    double next_hit = 0, next_blank = 0;
    int    hits = 0, misses = 0, blanks = 0;
    float  dealt = 0, travelled = 0;
    float  yaw = 0;
    bool   running() const { return phase != PH_IDLE; }
    void   begin(const Cfg& c, int mv, const Cand& e, const World& w);
    // one frame: dt in game seconds; ram / e as read now
    Step   step(const Cfg& c, const World& w, const EnemyNow& e);
    float  telegraph_speed(const Cfg& c) const;     // clip speed so the wind-up frames fill the telegraph
};

}  // namespace ps
