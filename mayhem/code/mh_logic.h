// mh_logic.h - pure logic of the [mayhem] props features (chain reactions, breakable cover) of TOD Gameplay+.
// No game memory, no Box3D: unit tested by mayhem/code/test_mh.cpp. The game glue is mayhem/code/mh_game.cpp.
#pragma once
#include <stdint.h>
#include <vector>

namespace mh {

// ---- config ([mayhem] in TODGameplay.ini), clamped
struct Cfg {
    // chain reactions: an explosion (grenade / rocket / dynamite, an exploding destroyable, a car) sets off the
    // explosive destroyables and the cars within chain_radius, each after its own delay, through the game's own
    // take_a_hit (so the game's own explosion, fire, sound, damage and score follow)
    bool  chain = false;
    float chain_radius = 9.0f;          // m from the explosion centre
    float delay_min = 0.15f, delay_max = 0.6f;   // s of game time (slow motion stretches a chain)
    float max_per_second = 6.0f;        // detonations started per second (token bucket, burst = max(1, this))
    int   max_per_frame = 2;            // detonations started in one frame
    int   max_depth = 12;               // a chain stops after this many links
    int   max_pending = 32;             // scheduled detonations at most
    float chain_damage = 5000.0f;       // hit points of the take_a_hit that sets a neighbour off
    bool  chain_cars = false;           // cars in reach take the hit too (they burn, then blow up the game's way); off by default
    float car_blast_radius = 8.0f, car_blast_impulse = 450.0f;   // Box3D blast of an exploding car (props thrown)
    // breakable cover: cover destroyables (crates, tables, stalls, bins ...) take extra damage from bullets and
    // blasts; when one breaks it bursts into debris that Box3D throws, and enemies crouched behind it stand up
    bool  cover = false;
    float cover_health_scale = 0.35f;   // effective health: a hit counts 1 / scale times (0.35 -> ~3x the damage)
    int   debris_max = 48;              // debris bodies alive at once (oldest removed first)
    float debris_life = 12.0f;          // s a piece lives (it fades by sinking once it sleeps or at the end)
    int   debris_pieces = 8;            // pieces per broken cover prop
    float debris_impulse = 1.0f;        // scale of the burst speed of the pieces
    float expose_radius = 2.6f;         // m: enemies this close to a broken cover prop are made to stand up
    float cover_blast_radius = 6.0f;    // m: blasts also hit cover props within this (extra take_a_hit)
    int   blast_debris = 6;             // shards thrown by every exploding destroyable (0 = none; drawn like the cover debris)
    // debris look (the first version's slats looked like "really large red bricks"): 1 = small chunks,
    // splinters and grit in the broken prop's own texture plus a puff of dust; 0 = the first version's big boxes
    int   debris_style = 1;
    float debris_size = 1.0f;           // x the chunk sizes of style 1 (cover chunks 6..24 cm, blast shards 3..11 cm)
    int   debris_bits = 14;             // grit per broken prop (cheap particles, not Box3D bodies; blasts throw 1.3x)
    int   debris_dust = 1;              // a puff of dust in the prop's colour when it breaks
    void  clamp();
};

// ---- deterministic random numbers (xorshift32)
struct Rng {
    uint32_t s = 0x9E3779B9u;
    uint32_t next();
    float    f01();                      // [0, 1)
    float    range(float a, float b);    // [a, b)
};

// the delay before a neighbour at dist goes off: nearer ones first, plus a random part (believable ripple)
float chain_delay(const Cfg& c, float dist, float r01);

// ---- the detonation schedule
enum Kind { K_DESTROYABLE = 1, K_CAR = 2 };
struct Pending {
    uint32_t node = 0;       // the target (Destroyable_Object or vehicle root)
    uint32_t guard = 0;      // a value that must still match when it fires (placeholder of a pooled destroyable)
    float    due = 0;        // game time
    int      depth = 0;      // links from the first explosion
    int      kind = 0;
    float    pos[3] = {0, 0, 0};   // where the target was when it was scheduled
    float    src[3] = {0, 0, 0};   // the explosion that set it off
    uint32_t owner = 0;      // who gets the credit (the first explosion's owner)
    uint32_t source = 0;     // the exploding node that set it off (the take_a_hit's inflictor)
};
struct Sched {
    std::vector<Pending> q;
    float tokens = 0.0f, last_t = -1.0f;
    int   dropped_full = 0, dropped_depth = 0, dropped_dup = 0;
    // false: already pending / too deep / the queue is full
    bool  add(const Cfg& c, const Pending& p);
    bool  pending(uint32_t node) const;
    // the due entries allowed now (token bucket of max_per_second, at most max_per_frame), oldest due first;
    // tokens refill with game time; entries that wait for a token stay in the queue
    int   pop_due(const Cfg& c, float now, Pending* out, int cap);
    void  clear() { q.clear(); tokens = 0.0f; last_t = -1.0f; }
};

// ---- explosions the chain itself caused: node -> depth, so the next link knows how deep it is
struct Fired { uint32_t node; int depth; float t; uint32_t owner; };
struct FiredLog {
    std::vector<Fired> v;
    void add(uint32_t node, int depth, float t, uint32_t owner);
    bool find(uint32_t node, float now, int* depth, uint32_t* owner) const;   // within 10 s
    void expire(float now);
};

// ---- name patterns (case-insensitive substrings, comma separated)
bool name_match(const char* name, const char* patterns);

// ---- debris: a broken cover prop (its collision box in its own frame) becomes pieces
struct Box { float c[3]; float h[3]; };   // centre, half extents
struct Piece {
    Box   box;                // in the prop's frame
    float vel[3];             // initial velocity (m/s, prop frame) - outward burst + lift
    float spin[3];            // initial angular velocity (rad/s)
};
// planks / chunks filling the prop's box: n pieces, a long axis split into slats, outward speed `burst` m/s plus
// `push` m/s along `dir` (prop frame, e.g. the shot direction); deterministic for a seed
int  make_debris(const Box& prop, int n, float burst, const float dir[3], float push, uint32_t seed, Piece* out, int cap);

// ---- debris style 1: small chunks instead of slats
enum ChunkShape { SH_SPLINTER = 0, SH_PLATE = 1, SH_LUMP = 2 };   // wood / metal sheet / anything else
// n chunks spread through the prop's box: each a small box whose longest edge is min_len..max_len (most near the small
// end) and never longer than the prop, shaped by `shape` (splinters long and thin, plates flat, lumps chunky); speeds
// as make_debris (outward burst + lift + push along dir). Deterministic for a seed.
int  make_chunks(const Box& prop, int n, float min_len, float max_len, int shape, float burst, const float dir[3], float push,
                 uint32_t seed, Piece* out, int cap);
// grit: a cheap particle (no Box3D body) that flies, bounces on the ground under it and settles
struct Bit {
    float p[3], v[3];        // world position / velocity
    float half;              // half edge (m)
    float ground;            // ground height under it (raycast once at the burst)
    float ax[3], ang, spin;  // tumble axis, angle, angular speed (rad/s)
    int   rest;              // 1 = settled
};
void bit_step(Bit& b, float dt, float gravity);
// dust: a soft camera-facing puff that grows and fades
struct Puff { float p[3], v[3]; float r0, r1, life, age, alpha; };
// radius and opacity at the puff's age (r0 -> r1 with an ease-out, alpha in for 0.08 s, then fading out); false = gone
bool puff_at(const Puff& f, float* r, float* a);
void puff_step(Puff& f, float dt);
// texture patch of a piece: a square of side `side` (0..1 UV) centred on (cu, cv), kept inside the texture
void uv_patch(float cu, float cv, float side, float* u0, float* v0);

// ---- bounded pool of debris lifetimes: which piece to remove when full / expired
struct DebrisSlot { int id; float born; bool used; };
struct DebrisPool {
    std::vector<DebrisSlot> s;
    int  take(int cap, float now, int* evicted);   // a slot index; *evicted = the id of a removed oldest piece (or -1)
    int  expired(float now, float life, int* ids, int cap);   // pieces older than life (slots freed)
};

// ---- per-frame cost statistics (milliseconds)
struct Stats {
    double sum = 0, mx = 0; int n = 0;
    void add(double ms) { sum += ms; if (ms > mx) mx = ms; ++n; }
    double avg() const { return n ? sum / n : 0.0; }
    void reset() { sum = mx = 0; n = 0; }
};

// ---- the extra damage a cover prop takes per hit for cover_health_scale (0 when the scale is 1 or more)
float cover_extra_damage(float hit_damage, float health_scale);

}  // namespace mh
