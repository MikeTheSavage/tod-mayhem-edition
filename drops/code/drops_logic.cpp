// drops_logic.cpp - see drops_logic.h
#include "drops_logic.h"
#include <math.h>
#include <string.h>

namespace dr {

const int POWERUP_GOODIE[N_POWERUPS] = {G_REWIND, G_GOLDEN_GUN, G_TORNADO, G_TORO, G_SOMBRERO, G_WRESTLER, G_MARIACHI, G_PINATA};
const int POWERUP_SLOT[N_POWERUPS] = {1, 2, 3, 4, 6, 8, 9, 10};
const char* const POWERUP_NAME[N_POWERUPS] = {"rewind", "golden_gun", "tornado", "toro", "sombrero", "wrestler", "mariachi", "pinata"};

const char* goodie_name(int g) {
    switch (g) {
    case G_MEDIUM_HEALTH: return "medium health"; case G_LARGE_HEALTH: return "large health";
    default: break;
    }
    for (int i = 0; i < N_POWERUPS; ++i) if (POWERUP_GOODIE[i] == g) return POWERUP_NAME[i];
    return "?";
}
int slot_of_goodie(int g) { for (int i = 0; i < N_POWERUPS; ++i) if (POWERUP_GOODIE[i] == g) return POWERUP_SLOT[i]; return -1; }

float dist_xz(const Vec3& a, const Vec3& b) { float dx = a.x - b.x, dz = a.z - b.z; return sqrtf(dx * dx + dz * dz); }

static float clampf(float v, float lo, float hi) { return !(v == v) ? lo : v < lo ? lo : v > hi ? hi : v; }
void Cfg::clamp() {
    ammo_scale = clampf(ammo_scale, 0.0f, 5.0f);
    health_chance = clampf(health_chance, 0.0f, 1.0f);
    health_hurt_bonus = clampf(health_hurt_bonus, 0.0f, 1.0f);
    large_health_share = clampf(large_health_share, 0.0f, 1.0f);
    powerup_chance = clampf(powerup_chance, 0.0f, 1.0f);
    for (int& w : weight) { if (w < 0) w = 0; if (w > 100) w = 100; }
    drop_seconds = clampf(drop_seconds, 5.0f, 300.0f);
    if (max_on_ground < 1) max_on_ground = 1; if (max_on_ground > 5) max_on_ground = 5;
    track_radius = clampf(track_radius, 10.0f, 80.0f);
    settle = clampf(settle, 0.0f, 4.0f);
    body_reach = clampf(body_reach, 0.5f, 10.0f); body_drop = clampf(body_drop, 0.3f, 10.0f);
    give_up = clampf(give_up, 1.0f, 60.0f);
    steal_distance = clampf(steal_distance, 10.0f, 200.0f);
    gun_gap = clampf(gun_gap, 0.0f, 2.0f);
    lift = clampf(lift, -0.5f, 1.5f);
    footprint = clampf(footprint, 0.0f, 0.6f);
    max_step = clampf(max_step, 0.05f, 1.0f);
    calibrate_max = clampf(calibrate_max, 0.0f, 3.0f);
}

float ammo_factor(const Cfg& c, float difficulty_factor) {
    if (!c.enabled) return difficulty_factor;
    return c.keep_difficulty_ammo ? c.ammo_scale * difficulty_factor : c.ammo_scale;
}

uint32_t Rng::next() { uint32_t x = s ? s : 1u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s = x; return x; }
float Rng::f01() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }

float health_chance(const Cfg& c, float frac) {
    frac = clampf(frac, 0.0f, 1.0f);
    return clampf(c.health_chance + c.health_hurt_bonus * (1.0f - frac), 0.0f, 1.0f);
}

int pick_powerup(const Cfg& c, Rng& rng, const RollIn& in) {
    int w[N_POWERUPS], total = 0;
    for (int i = 0; i < N_POWERUPS; ++i) {
        int s = POWERUP_SLOT[i];
        w[i] = c.weight[i];
        if (s == in.excluded_slot) w[i] = 0;
        if (in.counts && in.maxima) {
            int n = in.counts[s], m = in.maxima[s];
            if (n < 0 || m <= 0 || n >= m) w[i] = 0;   // unknown, locked (max 0) or full: the game would refuse it
        }
        total += w[i];
    }
    if (total <= 0) return -1;
    int r = (int)(rng.f01() * (float)total);
    if (r >= total) r = total - 1;
    for (int i = 0; i < N_POWERUPS; ++i) { if (r < w[i]) return POWERUP_GOODIE[i]; r -= w[i]; }
    return -1;
}

Roll roll(const Cfg& c, Rng& rng, const RollIn& in) {
    Roll r;
    r.p_health = health_chance(c, in.ram_health_frac);
    r.p_powerup = in.elite_drop && c.elite_skip_powerup ? 0.0f : c.powerup_chance;
    float a = rng.f01(), b = rng.f01();
    if (a < r.p_powerup) {
        int g = pick_powerup(c, rng, in);
        if (g >= 0) { r.goodie = g; r.why = "power-up roll"; return r; }
    }
    if (b < r.p_health) {
        r.goodie = rng.f01() < c.large_health_share ? G_LARGE_HEALTH : G_MEDIUM_HEALTH;
        r.why = "health roll"; return r;
    }
    r.why = in.elite_drop && c.elite_skip_powerup ? "nothing (the elite leaves its loco move)" : "nothing rolled";
    return r;
}

float rest_y(const float* ground, const bool* hit, int n, float lift, float max_step) {
    bool any = false; float base = 0.0f;
    if (n > 0 && hit[0]) { base = ground[0]; any = true; }
    else for (int i = 1; i < n; ++i) if (hit[i] && (!any || ground[i] < base)) { base = ground[i]; any = true; }   // no centre: the lowest
    if (!any) return NAN;
    float best = base;
    for (int i = 0; i < n; ++i) if (hit[i] && ground[i] > best && ground[i] <= base + max_step) best = ground[i];
    return best + lift;
}

Vec3 drop_spot(const Cfg& c, const Vec3& stood, const Vec3& body, bool body_ok, bool clear, bool* on_body) {
    bool ok = body_ok && clear && dist_xz(stood, body) <= c.body_reach && stood.y - body.y <= c.body_drop;
    *on_body = ok;
    return ok ? body : stood;
}

Vec3 beside(const Vec3& body, const Vec3& ram, float gap) {
    float dx = ram.x - body.x, dz = ram.z - body.z, l = sqrtf(dx * dx + dz * dz);
    if (!(l > 1e-3f) || gap <= 0.0f) return body;
    if (gap > l * 0.5f) gap = l * 0.5f;            // never past half the way to Ram
    return v3(body.x + dx / l * gap, body.y, body.z + dz / l * gap);
}

int life_of(bool same, float hp, float dmg, int st) {
    if (!same) return LIFE_GONE;
    return (st == 28 || dmg >= hp) ? LIFE_DEAD : LIFE_ALIVE;
}

Body* Book::find(uint32_t key) { for (Body& x : b) if (x.used && x.key == key) return &x; return nullptr; }
Body* Book::add(uint32_t key, double t) {
    Body* f = nullptr;
    for (Body& x : b) if (!x.used) { f = &x; break; }
    if (!f) for (Body& x : b) if (x.done && (!f || x.t_dead < f->t_dead)) f = &x;   // the oldest finished one
    if (!f) for (Body& x : b) if (!x.dead && (!f || x.t_seen < f->t_seen)) f = &x;  // the one seen longest ago (out of range)
    if (!f) return nullptr;
    *f = Body(); f->used = true; f->key = key; f->t_seen = t;
    return f;
}
void Book::clear() { for (Body& x : b) x = Body(); }
int Book::count() const { int n = 0; for (const Body& x : b) n += x.used; return n; }

bool body_settled(const Cfg& c, const Body& b, double t) {
    if (t >= b.t_dead + c.settle) return true;
    return b.rest_ok && t - b.t_rest >= 0.25 && t - b.t_dead >= 0.3;
}

bool clock_jump_is_load(double back) { return back > 10.0; }

float floor_offset(const Cfg& c, float feet_y, float line_y, bool line_hit) {
    if (!line_hit || !(feet_y == feet_y) || !(line_y == line_y)) return 0.0f;
    float d = feet_y - line_y;
    return d > 0.03f && d <= c.calibrate_max ? d : 0.0f;
}

}  // namespace dr
