// drops_logic.h - pure logic of [drops] (no game reads, no Windows): what a dead enemy leaves besides its gun, the
// dropped-ammo factor, where a pickup rests, and the book of watched enemies. Unit tested by drops/code/test_drops.cpp;
// the game glue is drops/code/drops_game.cpp.
//
// Vanilla (GOG TOD.exe, see docs/mods/drops.md): a dead enemy drops its gun (and its hat). Nothing else: the kill
// (character_placeholder command_kill 0x5A8950) and the sentry's dying state request no goodie. The gun carries
// ClipSize x a random 10..250 % (BulletDrop bands, by drop level). Health and power-ups come only from placed goodies and
// combo kill awards. [drops] adds, on every difficulty:
//   * ammo:     the BulletDrop bands x ammo_scale (it replaces the difficulty's own factor: BRUTAL / PSYCHO x0.5)
//   * health:   a chance per kill of a medium (150) or large (400) health goodie, higher while Ram is hurt
//   * power-up: a small chance per kill of one of the game's power-up goodies whose slot is not full
// Both goodies are the game's own pooled goodie props (DefaultGoodieRessources), placed at the body.
#pragma once
#include <stdint.h>

namespace dr {

// goodie ids (character_goodie_ctrl activate_goodie, GoodiesDict member order) and their power-up slot
enum Goodie { G_MEDIUM_HEALTH = 0, G_LARGE_HEALTH = 1, G_REWIND = 8, G_GOLDEN_GUN = 9, G_TORNADO = 10, G_TORO = 11,
              G_SOMBRERO = 12, G_WRESTLER = 14, G_MARIACHI = 15, G_PINATA = 16 };
enum { N_POWERUPS = 8 };
extern const int POWERUP_GOODIE[N_POWERUPS];        // rewind, golden gun, tornado, toro, sombrero, wrestler, mariachi, pinata
extern const int POWERUP_SLOT[N_POWERUPS];          // character_powerup_ctrl slot of each (1, 2, 3, 4, 6, 8, 9, 10)
extern const char* const POWERUP_NAME[N_POWERUPS];  // ini key suffix: rewind, golden_gun, tornado, toro, sombrero, wrestler, mariachi, pinata
const char* goodie_name(int goodie);
int slot_of_goodie(int goodie);                     // -1 for health

struct Vec3 { float x, y, z; };
inline Vec3 v3(float x, float y, float z) { Vec3 v = {x, y, z}; return v; }
float dist_xz(const Vec3& a, const Vec3& b);

struct Cfg {
    bool  enabled = false;                          // [drops] enabled
    // ammo
    float ammo_scale = 1.75f;                       // dropped guns carry this x the vanilla ammo, on every difficulty
    bool  keep_difficulty_ammo = false;             // 1 = multiply the difficulty's own factor too (BRUTAL / PSYCHO x0.5)
    // health
    float health_chance = 0.20f;                    // per kill
    float health_hurt_bonus = 0.30f;                // + this x the share of Ram's health that is missing
    float large_health_share = 0.25f;               // of the health drops, this share is a large one
    // power-ups
    float powerup_chance = 0.05f;                   // per kill
    int   weight[N_POWERUPS] = {3, 1, 2, 2, 1, 1, 1, 2};   // rewind, golden gun, tornado, toro, sombrero, wrestler, mariachi, pinata
    bool  elite_skip_powerup = true;                // an elite that leaves its own loco move (PSYCHO elite drops) gets no power-up
    // the pickups
    float drop_seconds = 30.0f;                     // how long a pickup stays
    int   max_on_ground = 3;                        // pickups of [drops] on the ground at once (the pool has 5 goodie props)
    float track_radius = 45.0f;                     // m around Ram: enemies are watched this far
    float settle = 1.5f;                            // game s a body has at most to come to rest (ragdoll)
    float body_reach = 3.0f, body_drop = 1.5f;      // the body further than this from where it stood (m): use where it stood
    float give_up = 10.0f;                          // game s a pickup waits for a free goodie prop
    float steal_distance = 25.0f;                   // a level goodie at least this far from Ram may lend its prop
    float gun_gap = 0.7f;                           // m between the pickup and the body (the dropped gun lies there)
    float lift = 0.75f;                             // m the pickup floats above the floor under it: the level's own goodies
                                                    // float 0.64-0.89 m above theirs (probe, MM010), a goodie at the floor is
                                                    // half sunk (as an early elite drop was)
    float footprint = 0.25f;                        // m from the centre to the four outer floor probes
    float max_step = 0.35f;                         // a probe higher than this above the centre is a wall / ledge
    float calibrate_max = 0.0f;                     // m: the line test may read the floor this much too low (see floor_offset);
                                                    // 0 = off: in the probe it read the floor right (0.11 m under Ram's root)
    void clamp();
};

// the dropped-ammo factor written to the BulletDrop bands: with [drops] on it replaces the difficulty's factor
float ammo_factor(const Cfg& c, float difficulty_factor);

struct Rng { uint32_t s = 0x2545F491u; uint32_t next(); float f01(); };

// what one kill leaves (besides its gun). counts / maxima: Ram's power-up counts by slot (index 0..11; -1 = unknown).
struct RollIn {
    float ram_health_frac = 1.0f;                   // health left / max (0..1)
    const int* counts = nullptr; const int* maxima = nullptr;
    bool elite_drop = false;                        // PSYCHO elite drops leave this enemy's move
    int  excluded_slot = -1;                        // a slot another feature took over (Suave on the Pinata's slot 10)
};
struct Roll { int goodie = -1; const char* why = ""; float p_health = 0, p_powerup = 0; };
Roll roll(const Cfg& c, Rng& rng, const RollIn& in);
float health_chance(const Cfg& c, float ram_health_frac);
// the power-up of a roll: weighted among the slots that are not full (-1 = none can take one)
int pick_powerup(const Cfg& c, Rng& rng, const RollIn& in);

// ---- where a pickup rests. A pickup of ours stands on the highest floor point under its footprint (ground[0] the centre,
// the others around it: on a slope or a stair edge it never sinks into the step), lifted by `lift`. A point more than
// max_step above the centre is a wall or a ledge, not floor under the pickup: it is left out.
float rest_y(const float* ground, const bool* hit, int n, float lift, float max_step);   // NAN when nothing hit
// the drop spot: the body, unless it lies too far from where the enemy stood (or there is no ground / a wall between)
Vec3 drop_spot(const Cfg& c, const Vec3& stood, const Vec3& body, bool body_ok, bool clear, bool* on_body);
// a spot gun_gap from the body toward Ram (the dropped gun lies on the body); dir from the body to Ram
Vec3 beside(const Vec3& body, const Vec3& ram, float gap);

// ---- the book: hostile enemies seen alive, their death, their body
enum Life { LIFE_ALIVE = 0, LIFE_DEAD = 1, LIFE_GONE = 2 };
int life_of(bool same_shell, float health, float damage, int ai_state);   // state 28 = dying
struct Body {
    bool used = false, alive_seen = false, dead = false, done = false, elite = false;
    uint32_t key = 0, root = 0, ai = 0, ph = 0, pelvis = 0;
    float health = 0; uint32_t gun = 0; int goodie = -2;    // the gun in hand while alive; the roll (-2 = not rolled yet)
    Vec3 last_alive = {0, 0, 0}, rest = {0, 0, 0}; bool rest_ok = false;
    double t_seen = 0, t_dead = 0, t_rest = 0, next_try = 0;
    bool no_drop = false;
};
struct Book {
    enum { N = 64 };
    Body b[N];
    Body* find(uint32_t key);
    Body* add(uint32_t key, double t);              // a free entry, else the oldest finished one; nullptr when full
    void clear();
    int count() const;
};
bool body_settled(const Cfg& c, const Body& b, double t);   // the pelvis has rested 0.25 s, or settle s passed
bool clock_jump_is_load(double back);
// In case the scene line test (Navigator::IsPointOccluded) misses the floor the characters walk on and hits a surface under
// it: a character's root stands on the real floor, the line test at that spot tells how far off it reads there. The offset to add to the line test's floor (0 when it reads right,
// reads high, or is off by more than calibrate_max: then the test hit something else).
float floor_offset(const Cfg& c, float feet_y, float line_y, bool line_hit);               // the game clock went back more than a rewind can (a load / retry)

}  // namespace dr
