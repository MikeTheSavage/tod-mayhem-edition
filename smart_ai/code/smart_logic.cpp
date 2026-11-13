// smart_logic.cpp - pure decisions of the Smart AI feature (see smart_logic.h). No game memory, no Windows calls.
#include "smart_logic.h"
#include <math.h>
#include <string.h>

namespace sm {

// ------------------------------------------------------------------------------------------------ random
uint32_t Rng::u() { uint32_t x = s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s = x ? x : 0x9E3779B9u; return s; }
float Rng::f() { return (float)(u() >> 8) * (1.0f / 16777216.0f); }
float Rng::range(float a, float b) { return a + (b - a) * f(); }
uint32_t mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return h ? h : 1u;
}

static const char* const FIELD_NAMES[F_COUNT] = {
    "Wait_until_Aggressive", "Retreat_Range", "Strafe_time_from", "Strafe_time_to", "Crouch_time_from", "Crouch_time_to",
    "Wait_time_from", "Wait_time_to", "ProCrouch", "ProStrafe", "ProStrafeCrouch", "ProShootDodge",
    "ProShootDodgeStrafeCrouch", "ProShootDodgeCrouch", "ProCrouchShootDodgeStrafe", "ProStrafeShootdodgeStrafe",
    "StopDistance", "ShootDodge_Min_dist", "CS_HealthLeft", "CS_Change_for"};
const char* field_name(int f) { return f >= 0 && f < F_COUNT ? FIELD_NAMES[f] : "?"; }
const char* role_name(int r) { return r == ROLE_ASSAULT ? "assault" : r == ROLE_RIFLEMAN ? "rifleman" : r == ROLE_CAUTIOUS ? "cautious" : "none"; }

Sentry::Sentry() { for (int i = 0; i < F_COUNT; ++i) v[i] = 0.0f; v[F_STOPDIST] = 10.0f; }
Persona::Persona() { for (int i = 0; i < F_COUNT; ++i) v[i] = 0.0f; }

static bool finite_ok(float x) { return x == x && x > -1e6f && x < 1e6f; }

// ------------------------------------------------------------------------------------------------ personality
uint64_t parse_factions(const char* list, uint64_t def) {
    if (!list) return def;
    uint64_t m = 0; bool any = false; int cur = -1;
    for (const char* p = list;; ++p) {
        if (*p >= '0' && *p <= '9') { cur = (cur < 0 ? 0 : cur) * 10 + (*p - '0'); if (cur > 1000) cur = 1000; continue; }
        if (cur >= 0 && cur < 64) { m |= 1ull << cur; any = true; }
        cur = -1;
        if (!*p || *p == ';') break;
    }
    return any ? m : (*list && list[strspn(list, " \t")] == '-' ? 0 : def);   // "-" = nobody skipped by faction
}

bool eligible(const Sentry& s, float boss_health, uint64_t skip_factions) {
    if (s.is_buddy) return false;                                         // Ram's friends (Marco, DEA helpers)
    if (s.subtype == 4 || s.subtype == 7 || s.subtype == 8 || s.subtype == 9) return false;   // escort / special / passive / civil
    if (s.root_subtype == 7 || s.root_subtype == 8 || s.root_subtype == 9) return false;
    if (s.faction >= 0 && s.faction < 64 && ((skip_factions >> s.faction) & 1u)) return false;   // civilians, DEA, wrestlers
    if (!finite_ok(s.health) || s.health <= 0.0f || s.health >= boss_health) return false;   // bosses / specials keep their design
    for (int i = 0; i < F_COUNT; ++i) if (!finite_ok(s.v[i])) return false;
    return true;
}
bool row_dodges(const float* v) {
    if (!(v[F_SD_MIN] > 0.0f)) return false;
    return v[F_PRO_SD] > 0.0f || v[F_PRO_SD_STRAFE_CROUCH] > 0.0f || v[F_PRO_SD_CROUCH] > 0.0f ||
           v[F_PRO_CROUCH_SD_STRAFE] > 0.0f || v[F_PRO_STRAFE_SD_STRAFE] > 0.0f;
}
float pro_sum(const float* v) { float t = 0.0f; for (int i = 0; i < PRO_COUNT; ++i) t += v[PRO_FIRST + i]; return t; }

static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static void scale_pair(float* v, int from, int to, float k) {
    if (v[from] > 0.0f) v[from] *= k;
    if (v[to] > 0.0f) v[to] *= k;
    if (v[to] < v[from]) v[to] = v[from];
}

Persona make_persona(const PersonaCfg& c, const Sentry& s, Rng& r, bool tick_ok) {
    Persona p;
    for (int i = 0; i < F_COUNT; ++i) p.v[i] = s.v[i];
    if (!c.enabled || !eligible(s, c.boss_health, c.skip_factions)) return p;
    p.eligible = true;
    float x = r.f();
    p.role = x < c.assault_share ? ROLE_ASSAULT : (x < c.assault_share + c.cautious_share ? ROLE_CAUTIOUS : ROLE_RIFLEMAN);
    float j = clampf(c.jitter, 0.0f, 0.6f);
    float* v = p.v;
    // action times of state 20 (one factor per pair keeps from <= to); assault waits less, cautious crouches longer
    float kw = r.range(1.0f - j, 1.0f + j) * (p.role == ROLE_ASSAULT ? 0.8f : p.role == ROLE_CAUTIOUS ? 1.2f : 1.0f);
    float ks = r.range(1.0f - j, 1.0f + j);
    float kc = r.range(1.0f - j, 1.0f + j) * (p.role == ROLE_CAUTIOUS ? 1.2f : 1.0f);
    scale_pair(v, F_WAIT_FROM, F_WAIT_TO, kw);
    scale_pair(v, F_STRAFE_FROM, F_STRAFE_TO, ks);
    scale_pair(v, F_CROUCH_FROM, F_CROUCH_TO, kc);
    // state 20 weights
    for (int i = 0; i < PRO_COUNT; ++i) {
        float& w = v[PRO_FIRST + i];
        if (w <= 0.0f) continue;
        float k = r.range(1.0f - j, 1.0f + j);
        bool strafe = PRO_FIRST + i == F_PRO_STRAFE || PRO_FIRST + i == F_PRO_STRAFE_SD_STRAFE;
        bool crouch = PRO_FIRST + i == F_PRO_CROUCH || PRO_FIRST + i == F_PRO_STRAFE_CROUCH;
        if (p.role == ROLE_ASSAULT) k *= strafe ? 1.25f : (crouch ? 0.8f : 1.0f);
        if (p.role == ROLE_CAUTIOUS) k *= crouch ? 1.3f : (strafe ? 0.9f : 1.0f);
        w *= k;
    }
    // shoot-dodge: the game dodges in state 20 only when the target is closer than ShootDodge_Min_dist. Only a row that
    // already dodges gets more (spread over its own non-zero dodge weights); ShootDodge_Min_dist is never invented.
    if (row_dodges(s.v) && c.dodge_bonus > 0.0f) {
        static const int SD[5] = {F_PRO_SD, F_PRO_SD_STRAFE_CROUCH, F_PRO_SD_CROUCH, F_PRO_CROUCH_SD_STRAFE, F_PRO_STRAFE_SD_STRAFE};
        float bonus = c.dodge_bonus * (p.role == ROLE_ASSAULT ? 2.0f : (p.role == ROLE_CAUTIOUS ? 0.5f : 1.0f));
        float have = 0.0f; for (int f : SD) have += v[f];
        if (have > 0.0f) for (int f : SD) if (v[f] > 0.0f) v[f] += bonus * v[f] / have;
    }
    float sum = pro_sum(v);
    if (sum > 95.0f) for (int i = 0; i < PRO_COUNT; ++i) v[PRO_FIRST + i] *= 95.0f / sum;   // keep >= 5 % plain waits
    // engagement distance: only the default 10 m (the placeholder's custom range is left alone)
    if (fabsf(s.v[F_STOPDIST] - 10.0f) < 1e-3f) {
        float k = p.role == ROLE_ASSAULT ? c.assault_range : (p.role == ROLE_CAUTIOUS ? c.cautious_range : r.range(0.85f, 1.15f));
        v[F_STOPDIST] = clampf(10.0f * k, 3.0f, 25.0f);
    }
    // keep-distance back-off of state 22: only a row that has one (the game zeroes it for melee enemies); cautious keep
    // more distance
    if (p.role == ROLE_CAUTIOUS && v[F_RETREAT] > 0.0f) v[F_RETREAT] *= 1.25f;
    if (v[F_RETREAT] > v[F_STOPDIST] * 0.6f) v[F_RETREAT] = v[F_STOPDIST] * 0.6f;   // must stay well inside the fight range
    // staggered first reaction (state 14 waits it out unless the target is within 8 m)
    if (c.reaction && tick_ok && s.v[F_WAIT_AGGR] == 0.0f && c.react_max > 0.0f) {
        float lo = clampf(c.react_min, 0.0f, 5.0f), hi = clampf(c.react_max, lo, 5.0f);
        float w = r.range(lo, hi);
        if (p.role == ROLE_ASSAULT) w *= 0.6f;
        v[F_WAIT_AGGR] = w > 0.05f ? w : 0.05f;
    }
    for (int i = 0; i < F_COUNT; ++i) if (fabsf(p.v[i] - s.v[i]) > 1e-6f) p.changed |= 1u << i;
    return p;
}

bool reaction_used(int st) {
    switch (st) {
    case 12: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 25: case 26: case 27: case 29:
        return true;
    default: return false;
    }
}

// ------------------------------------------------------------------------------------------------ squad
bool squad_engaged_state(int st) { return st == 13 || st == 17 || st == 18 || st == 19 || st == 20 || st == 22; }

int squad_assign(const SquadCfg& c, const SquadIn* in, SquadMem* mem, bool* want_hold, int n, uint32_t now) {
    const uint32_t token_ms = (uint32_t)(c.token_s * 1000.0f), cool_ms = (uint32_t)(c.cooldown_s * 1000.0f),
                   stuck_ms = (uint32_t)(c.stuck_s * 1000.0f);
    const float hold_max2 = c.hold_max_m * c.hold_max_m;
    if (n > 64) n = 64;              // the glue keeps at most 32 enemies
    bool done[64], forced[64];
    int holders = 0;
    for (int i = 0; i < n; ++i) {
        want_hold[i] = false; done[i] = false; forced[i] = false;
        if (in[i].state != mem[i].last_state) { mem[i].last_state = in[i].state; mem[i].state_since = now; }
        bool member = c.enabled && in[i].managed && in[i].engaged && in[i].ranged && in[i].target != 0;
        if (!member) { mem[i].token = false; mem[i].holding = false; done[i] = true; }
    }
    for (int i = 0; i < n; ++i) {
        if (done[i]) continue;
        // the group: every member engaged on the same target
        int idx[64]; int g = 0;
        for (int k = i; k < n && g < 64; ++k) if (!done[k] && in[k].target == in[i].target) { idx[g++] = k; done[k] = true; }
        if (g < c.min_squad) {
            for (int q = 0; q < g; ++q) { mem[idx[q]].token = false; mem[idx[q]].holding = false; }
            continue;
        }
        // forced hunters: too far to fight from here, or holding without a shot for too long
        for (int q = 0; q < g; ++q) {
            int k = idx[q];
            float back = c.hold_max_m - 2.0f; if (back < 0.0f) back = 0.0f;
            mem[k].beyond = in[k].target_sqr > (mem[k].beyond ? back * back : hold_max2);
            bool stuck = mem[k].holding && in[k].state == 19 && now - mem[k].state_since >= stuck_ms;
            if (stuck) mem[k].hunt_until = now + token_ms;
            forced[k] = mem[k].beyond || (int32_t)(now - mem[k].hunt_until) < 0;
            if (forced[k]) { mem[k].token = false; }
        }
        // pass on tokens held long enough by enemies that already fight at their range
        int tokens = 0;
        for (int q = 0; q < g; ++q) {
            int k = idx[q];
            if (!mem[k].token || forced[k]) continue;
            bool fighting = in[k].state == 19 || in[k].state == 20 || in[k].state == 22;
            if (fighting && now - mem[k].token_since >= token_ms) { mem[k].token = false; mem[k].cooldown_until = now + cool_ms; }
            else ++tokens;
        }
        // too many (the limit was lowered): drop the newest
        while (tokens > c.max_advancing) {
            int best = -1;
            for (int q = 0; q < g; ++q) { int k = idx[q]; if (mem[k].token && !forced[k] && (best < 0 || mem[k].token_since > mem[best].token_since)) best = k; }
            if (best < 0) break;
            mem[best].token = false; --tokens;
        }
        // hand out free tokens: the longest holder first, then the closest
        while (tokens < c.max_advancing) {
            int best = -1; uint32_t bkey = 0; float bd = 0.0f;
            for (int q = 0; q < g; ++q) {
                int k = idx[q];
                if (mem[k].token || forced[k] || (int32_t)(now - mem[k].cooldown_until) < 0) continue;
                uint32_t key = mem[k].holding ? now - mem[k].hold_since : 0;   // larger = waited longer
                if (best < 0 || key > bkey || (key == bkey && in[k].target_sqr < bd)) { best = k; bkey = key; bd = in[k].target_sqr; }
            }
            if (best < 0) break;
            mem[best].token = true; mem[best].token_since = now; ++tokens;
        }
        for (int q = 0; q < g; ++q) {
            int k = idx[q];
            bool h = !forced[k] && !mem[k].token;
            if (h && !mem[k].holding) mem[k].hold_since = now;
            mem[k].holding = h;
            want_hold[k] = h;
            if (h) ++holders;
        }
    }
    return holders;
}

// ------------------------------------------------------------------------------------------------ morale
void fear_add(Fear& fr, float a) { fr.f = clampf(fr.f + a, 0.0f, 1.0f); }
void fear_decay(Fear& fr, const MoraleCfg& c, float dt) { if (dt > 0.0f) fr.f = clampf(fr.f - c.decay_per_s * dt, 0.0f, 1.0f); }
void morale_values(const MoraleCfg& c, float base_hl, float base_cf, float fear, float* hl, float* cf) {
    if (fear <= 0.0f) { *hl = base_hl; *cf = base_cf; return; }
    float f = clampf(fear, 0.0f, 1.0f);
    float h = base_hl + f * c.health_bonus, k = base_cf + f * c.max_chance;
    *hl = h > base_hl ? (h < 200.0f ? h : (base_hl > 200.0f ? base_hl : 200.0f)) : base_hl;
    *cf = k > base_cf ? (k < 100.0f ? k : (base_cf > 100.0f ? base_cf : 100.0f)) : base_cf;
}
float dist2(const float* a, const float* b) { float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return x * x + y * y + z * z; }

} // namespace sm
