// psycho_logic.cpp - see psycho_logic.h. Pure: no game reads, no Windows calls.
#include "psycho_logic.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

namespace ps {

static const char* const MOVE_NAMES[MOVE_COUNT] = {"none", "tornado", "toro"};
const char* move_name(int m) { return m >= 0 && m < MOVE_COUNT ? MOVE_NAMES[m] : "?"; }
int move_from_name(const char* s, int n) {
    if (!s || n <= 0) return -1;
    struct { const char* name; int m; } al[] = {{"tornado", MOVE_TORNADO}, {"spin", MOVE_TORNADO}, {"loco", MOVE_TORNADO},
                                                {"toro", MOVE_TORO}, {"eltoro", MOVE_TORO}, {"el_toro", MOVE_TORO}, {"charge", MOVE_TORO}};
    for (auto& a : al) {
        int l = (int)strlen(a.name);
        if (l != n) continue;
        bool eq = true;
        for (int i = 0; i < n && eq; ++i) eq = tolower((unsigned char)s[i]) == a.name[i];
        if (eq) return a.m;
    }
    return -1;
}
unsigned parse_moves(const char* s) {
    unsigned m = 0;
    if (!s) return 0;
    while (*s) {
        while (*s && !isalnum((unsigned char)*s) && *s != '_') ++s;
        const char* b = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_')) ++s;
        int k = move_from_name(b, (int)(s - b));
        if (k > 0) m |= 1u << k;
    }
    return m;
}
bool parse_range(const char* s, float* lo, float* hi) {
    if (!s) return false;
    char* e = nullptr;
    while (*s == ' ' || *s == '\t') ++s;
    float a = strtof(s, &e);
    if (e == s) return false;
    s = e;
    while (*s == ' ' || *s == '\t' || *s == ',' || *s == '-' || *s == ';') ++s;   // "20-40", "20, 40"
    float b = a;
    if (*s) { float t = strtof(s, &e); if (e != s) b = t; }
    if (b < a) { float t = a; a = b; b = t; }
    *lo = a; *hi = b;
    return true;
}

Vec3 sub(const Vec3& a, const Vec3& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 add(const Vec3& a, const Vec3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 mul(const Vec3& a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
float dist_xz(const Vec3& a, const Vec3& b) { float dx = a.x - b.x, dz = a.z - b.z; return sqrtf(dx * dx + dz * dz); }
float dot_xz(const Vec3& a, const Vec3& b) { return a.x * b.x + a.z * b.z; }
Vec3 norm_xz(const Vec3& a) { float l = sqrtf(a.x * a.x + a.z * a.z); return l > 1e-4f ? v3(a.x / l, 0, a.z / l) : v3(0, 0, 1); }
float yaw_of(const Vec3& d) { return atan2f(d.x, d.z); }

static float clampf(float v, float lo, float hi) { return v != v ? lo : v < lo ? lo : v > hi ? hi : v; }
void Cfg::clamp() {
    drop_seconds = clampf(drop_seconds, 3.0f, 600.0f); drop_max = drop_max < 1 ? 1 : drop_max > 5 ? 5 : drop_max;
    drop_track_radius = clampf(drop_track_radius, 10.0f, 80.0f); drop_settle = clampf(drop_settle, 0.2f, 5.0f);
    drop_body_reach = clampf(drop_body_reach, 0.0f, 10.0f); drop_body_drop = clampf(drop_body_drop, 0.0f, 5.0f);
    drop_give_up = clampf(drop_give_up, 1.0f, 120.0f);
    cooldown_min = clampf(cooldown_min, 1.0f, 600.0f); cooldown_max = clampf(cooldown_max, cooldown_min, 600.0f);
    first_delay = clampf(first_delay, 0.0f, 120.0f); global_gap = clampf(global_gap, 0.0f, 120.0f);
    telegraph = clampf(telegraph, 0.3f, 3.0f);
    elite_health = clampf(elite_health, 1.0f, 100000.0f); boss_health = clampf(boss_health, 1.0f, 1000000.0f);
    chance = clampf(chance, 0.0f, 1.0f); check_interval = clampf(check_interval, 0.1f, 5.0f);
    retry_delay = clampf(retry_delay, 0.2f, 30.0f); interrupt_damage = clampf(interrupt_damage, 0.01f, 10000.0f);
    interrupt_cooldown = clampf(interrupt_cooldown, 0.0f, 1.0f); recover = clampf(recover, 0.0f, 3.0f);
    tornado_min = clampf(tornado_min, 0.0f, 50.0f); tornado_max = clampf(tornado_max, tornado_min, 60.0f);
    tornado_radius = clampf(tornado_radius, 1.0f, 40.0f); tornado_damage = clampf(tornado_damage, 0.0f, 10000.0f);
    tornado_hit_interval = clampf(tornado_hit_interval, 0.05f, 5.0f); tornado_fire_speed = clampf(tornado_fire_speed, 0.05f, 2.0f);
    tornado_blank_interval = clampf(tornado_blank_interval, 0.03f, 1.0f);
    tornado_windup_frames = clampf(tornado_windup_frames, 1.0f, 60.0f); tornado_fire_frames = clampf(tornado_fire_frames, 1.0f, 60.0f);
    tornado_clip_frames = clampf(tornado_clip_frames, tornado_windup_frames + tornado_fire_frames, 400.0f);
    toro_min = clampf(toro_min, 0.0f, 50.0f); toro_max = clampf(toro_max, toro_min, 60.0f);
    toro_speed = clampf(toro_speed, 1.0f, 30.0f); toro_overrun = clampf(toro_overrun, 0.0f, 20.0f);
    toro_max_seconds = clampf(toro_max_seconds, 0.3f, 10.0f); toro_hit_radius = clampf(toro_hit_radius, 0.3f, 5.0f);
    toro_damage = clampf(toro_damage, 0.0f, 10000.0f); toro_impulse = clampf(toro_impulse, 0.0f, 50.0f);
    toro_turn_rate = clampf(toro_turn_rate, 0.5f, 50.0f);
}

bool alive(const Cand& c) { return c.sentry && c.health > 0.0f && c.damage < c.health && c.state != 28; }
bool in_combat(int s) { return s == 13 || (s >= 17 && s <= 22) || s == 25; }   // sentry states (AI_ARCH): engaged with a target
bool is_boss(const Cfg& c, const Cand& e) { return e.health >= c.boss_health; }
bool is_elite(const Cfg& c, const Cand& e) { return e.health >= c.elite_health || is_boss(c, e); }

void Registry::clear() { memset(k, 0, sizeof k); }
Known* Registry::find(uint32_t root, uint32_t ai) {
    for (Known& x : k) if (x.used && x.root == root && x.ai == ai) return &x;
    return nullptr;
}
Known* Registry::get(uint32_t root, uint32_t ai, double now, const Cfg& c) { return get(root, ai, now, c, 0, 0.0f); }
Known* Registry::get(uint32_t root, uint32_t ai, double now, const Cfg& c, uint32_t ph, float health) {
    if (Known* f = find(root, ai)) {
        bool other = (ph && f->ph && ph != f->ph) || (health > 0.0f && f->health > 0.0f && fabsf(health - f->health) > 0.5f);
        if (other) { f->first_seen = now; f->next_ok = now + c.first_delay; f->moves = 0; }   // a new enemy in the shell
        if (ph) f->ph = ph;
        if (health > 0.0f) f->health = health;
        return f;
    }
    Known* slot = nullptr;
    for (Known& x : k) if (!x.used) { slot = &x; break; }
    if (!slot) {   // full: the entry seen first is dropped (a long fight with many reinforcements)
        slot = &k[0];
        for (Known& x : k) if (x.first_seen < slot->first_seen) slot = &x;
    }
    slot->used = true; slot->root = root; slot->ai = ai; slot->ph = ph; slot->health = health;
    slot->first_seen = now; slot->next_ok = now + c.first_delay; slot->moves = 0;
    return slot;
}
int Registry::count() const { int n = 0; for (const Known& x : k) n += x.used ? 1 : 0; return n; }

uint32_t Rng::next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
float Rng::f01() { return (next() >> 8) * (1.0f / 16777216.0f); }
float Rng::range(float a, float b) { return a + (b - a) * f01(); }

int moves_for_distance(const Cfg& c, float d, bool has_gun) {
    int m = 0;
    if ((c.moves & (1u << MOVE_TORNADO)) && has_gun && d >= c.tornado_min && d <= c.tornado_max) m |= 1 << MOVE_TORNADO;
    if ((c.moves & (1u << MOVE_TORO)) && d >= c.toro_min && d <= c.toro_max) m |= 1 << MOVE_TORO;
    return m;
}

bool is_ram_voice(int id) { return id >= 106 && id <= 109; }
int sting_sound(int configured) { return configured < 0 || is_ram_voice(configured) ? -1 : configured; }
bool needs_handback(int ai_state) { return ai_state == 2; }   // still "no AI control" (the telegraph's state)

const char* reject_reason(const Cfg& c, Registry& reg, const World& w, const Cand& e) {
    if (!alive(e)) return "dead / not a sentry";
    if (!e.hostile) return "not hostile";
    if (e.buddy) return "buddy";
    Known* k = reg.get(e.root, e.ai, w.t, c, e.ph, e.health);
    if (e.special) return "special (VIP placement / escort / scripted)";
    if (is_boss(c, e) && !c.bosses) return "boss (bosses = 0)";
    if (!in_combat(e.state)) return "not in a fight (state)";
    if (c.elite_only && !is_elite(c, e)) return "not an elite";
    if (w.t < k->next_ok) return k->moves ? "cooling down" : "just arrived (first_delay)";
    if (!move_for_distance(c, e, dist_xz(e.pos, w.ram))) return "distance (or no gun for the tornado)";
    return "";
}

// ------------------------------------------------------------------ the fixed ability, the elite book
int ability_for(uint32_t uid, unsigned moves) {
    bool t = (moves & (1u << MOVE_TORNADO)) != 0, o = (moves & (1u << MOVE_TORO)) != 0;
    if (t && o) {
        uint32_t h = uid * 2654435761u; h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;   // the placements come in runs of ids
        return (h & 1) ? MOVE_TORO : MOVE_TORNADO;
    }
    return t ? MOVE_TORNADO : o ? MOVE_TORO : MOVE_NONE;
}
int goodie_of_move(int move) { return move == MOVE_TORNADO ? 10 : move == MOVE_TORO ? 11 : 0; }
int ability_of(const Cfg& c, const Cand& e) {
    int m = e.ability != MOVE_NONE ? e.ability : ability_for(e.uid, c.moves);
    return (c.moves & (1u << m)) ? m : MOVE_NONE;
}
int move_for_distance(const Cfg& c, const Cand& e, float d) {
    int m = ability_of(c, e);
    return m != MOVE_NONE && (moves_for_distance(c, d, e.has_gun) & (1 << m)) ? m : MOVE_NONE;
}
bool drop_eligible(const Cfg& c, const Cand& e) {
    if (!alive(e) || !e.hostile || e.buddy || e.special) return false;
    if (is_boss(c, e)) return c.bosses;
    return e.health >= c.elite_health;
}
void EliteBook::clear() { for (Elite& x : e) x = Elite(); used = 0; }
Elite* EliteBook::find(uint32_t key) {
    if (!key) return nullptr;
    for (Elite& x : e) if (x.used && x.key == key) return &x;
    return nullptr;
}
Elite* EliteBook::add(uint32_t key, uint32_t uid, int ability) {
    if (!key) return nullptr;
    Elite* x = nullptr;
    for (Elite& y : e) if (!y.used) { x = &y; break; }
    if (!x) {   // full: a finished one goes (dead and out of the running), the earliest death first
        for (Elite& y : e) if (y.dead && !y.dropped && (!x || y.t_dead < x->t_dead)) x = &y;
        if (!x) return nullptr;
    } else ++used;
    *x = Elite(); x->used = true; x->key = key; x->uid = uid; x->ability = ability;
    return x;
}
void Elite::saw(const Cand& c) {
    root = c.root; ai = c.ai; ph = c.ph; alive_seen = true; dead = false; no_drop = false; last_alive = c.pos;
}
bool wants_drop(const Elite& e) { return e.used && e.dead && !e.dropped && !e.no_drop && e.ability != MOVE_NONE; }
int life_of(bool same_shell, float health, float damage, int state) {
    if (!same_shell || state < 0) return LIFE_GONE;
    return damage >= health || state == 28 ? LIFE_DEAD : LIFE_ALIVE;
}
bool body_settled(const Cfg& c, const Elite& e, double t) {
    return t >= e.t_dead + c.drop_settle || (e.rest_ok && t - e.t_rest >= 0.25);
}
Vec3 drop_spot(const Cfg& c, const Vec3& last_alive, const Vec3& body, bool body_ok, bool clear, bool* used_body) {
    bool use = body_ok && clear && dist_xz(body, last_alive) <= c.drop_body_reach && fabsf(body.y - last_alive.y) <= c.drop_body_drop;
    if (used_body) *used_body = use;
    return use ? body : last_alive;
}
bool undo_death(Elite& e, double t) {
    if (!e.dead || t >= e.t_dead) return false;
    bool on_ground = e.dropped && !e.collected && e.t_drop >= e.t_dead;
    e.dead = false; e.no_drop = false; e.rest_ok = false; e.next_try = 0;
    if (on_ground) e.dropped = false;
    return on_ground;
}
bool clock_jump_is_load(double back) { return back > 10.0; }

Pick pick(const Cfg& c, Registry& reg, const World& w, const Cand* cand, int n, Rng& rng, bool any_active, double gap_until) {
    Pick p;
    if (!c.enabled) { p.why = "off"; return p; }
    if (!w.psycho) { p.why = "not PSYCHO"; return p; }
    if (any_active) { p.why = "a move runs"; return p; }
    if (!w.ram_ok || w.ram_busy) { p.why = "Ram busy"; return p; }
    if (w.t < gap_until) { p.why = "global gap"; return p; }
    float best = 1e9f; int seen = 0, elig = 0;
    for (int i = 0; i < n; ++i) {
        const Cand& e = cand[i];
        if (!alive(e) || !e.hostile || e.buddy) continue;
        ++seen;
        if (*reject_reason(c, reg, w, e)) continue;
        float d = dist_xz(e.pos, w.ram);
        ++elig;
        if (rng.f01() >= c.chance) continue;
        if (d < best) { best = d; p.index = i; p.distance = d; p.move = move_for_distance(c, e, d); }
    }
    p.why = p.index >= 0 ? "picked" : !seen ? "no hostile enemy" : !elig ? "none eligible" : "no roll passed";
    return p;
}

static const char* const PHASES[] = {"idle", "telegraph", "active", "recover"};
const char* phase_name(int p) { return p >= 0 && p < 4 ? PHASES[p] : "?"; }
static const char* const ENDS[] = {"-", "done", "interrupted (shot in the wind-up)", "enemy dead", "Ram busy", "enemy lost",
                                   "wall", "hit Ram", "distance", "time"};
const char* end_name(int e) { return e >= 0 && e < (int)(sizeof ENDS / sizeof *ENDS) ? ENDS[e] : "?"; }

float Active::telegraph_speed(const Cfg& c) const {
    if (move != MOVE_TORNADO) return 1.0f;
    return (c.tornado_windup_frames / 25.0f) / c.telegraph;
}

void Active::begin(const Cfg& c, int mv, const Cand& e, const World& w) {
    *this = Active();
    move = mv; phase = PH_TELEGRAPH; root = e.root; ai = e.ai;
    t_phase = t_start = t_last = w.t; damage0 = e.damage; start = e.pos; target = w.ram;
    dir = norm_xz(sub(w.ram, e.pos)); yaw = yaw_of(dir);
    (void)c;
}

static float wrap_pi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

Step Active::step(const Cfg& c, const World& w, const EnemyNow& e) {
    Step s;
    if (phase == PH_IDLE) return s;
    double tp = w.t - t_phase;
    struct LastT { double& r; double v; ~LastT() { r = v; } } last_upd = {t_last, w.t};   // t_last = now after this step
    if (!e.alive) { end = END_ENEMY_DEAD; s.release = true; s.end = end; phase = PH_IDLE; return s; }
    if (w.ram_busy || !w.ram_ok) { end = END_RAM_BUSY; s.release = true; s.end = end; phase = PH_IDLE; return s; }
    if (phase == PH_TELEGRAPH) {
        if (e.damage - damage0 >= c.interrupt_damage) { end = END_INTERRUPTED; s.release = true; s.end = end; phase = PH_IDLE; return s; }
        if (move == MOVE_TORO) {   // the bull keeps its eyes on Ram while it paws the ground
            float want = yaw_of(norm_xz(sub(w.ram, e.pos)));
            float d = wrap_pi(want - yaw);
            float dt = (float)(w.t - t_last); if (dt < 0.0f) dt = 0.0f; if (dt > 0.1f) dt = 0.1f;
            float lim = c.toro_turn_rate * dt;
            if (d > lim) d = lim; if (d < -lim) d = -lim;
            yaw = wrap_pi(yaw + d); s.face = true; s.face_yaw = yaw;
        }
        if (tp >= c.telegraph) {
            phase = PH_ACTIVE; t_phase = w.t; s.go_active = true;
            if (move == MOVE_TORNADO) {
                s.clip_speed = c.tornado_fire_speed;
                next_hit = w.t + c.tornado_hit_interval * 0.5; next_blank = w.t;
            } else {
                start = e.pos; target = w.ram;
                Vec3 to = sub(w.ram, e.pos); to.y = 0;
                float d = dist_xz(w.ram, e.pos);
                dir = norm_xz(to); yaw = yaw_of(dir);
                limit = d + c.toro_overrun; if (limit > c.toro_max + c.toro_overrun) limit = c.toro_max + c.toro_overrun;
                s.face = true; s.face_yaw = yaw; s.clip_speed = 1.0f;
            }
        }
        return s;
    }
    if (phase == PH_ACTIVE) {
        if (move == MOVE_TORNADO) {
            float fire_len = (c.tornado_fire_frames / 25.0f) / c.tornado_fire_speed;
            if (w.t >= next_blank) { s.blank = true; ++blanks; next_blank = w.t + c.tornado_blank_interval; }
            if (w.t >= next_hit) {
                next_hit = w.t + c.tornado_hit_interval;
                float d = dist_xz(w.ram, e.pos);
                if (d > c.tornado_radius) { s.miss = true; s.miss_why = "out of range"; }
                else if (c.dive_dodges && w.ram_dodging) { s.miss = true; s.miss_why = "dodged (shoot-dodge)"; }
                else if (!e.los_ok) { s.miss = true; s.miss_why = "in cover (no line of sight)"; }
                else { s.hit = true; s.hit_damage = c.tornado_damage; s.hit_impulse = mul(norm_xz(sub(w.ram, e.pos)), 1.0f); }
                if (s.hit) { ++hits; dealt += s.hit_damage; } else ++misses;
            }
            if (tp >= fire_len) { phase = PH_RECOVER; t_phase = w.t; end = END_DONE; s.recover = true; s.clip_speed = 1.0f; }
            return s;
        }
        // El Toro: along dir at toro_speed; the enemy's own (root) motion may run ahead, never behind
        float expected = c.toro_speed * (float)tp;
        if (expected > limit) expected = limit;
        float actual = dot_xz(sub(e.pos, start), dir);
        Vec3 here = e.pos;
        if (actual < expected - 0.25f) { here = add(start, mul(dir, expected)); here.y = start.y; s.place = true; s.place_pos = here; actual = expected; }
        travelled = actual;
        int stop = END_NONE;
        if (dist_xz(here, w.ram) <= c.toro_hit_radius) {
            if (c.dive_dodges && w.ram_dodging) { if (!misses) { s.miss = true; s.miss_why = "dodged (shoot-dodge)"; } ++misses; }
            else { s.hit = true; s.hit_damage = c.toro_damage; s.hit_impulse = v3(dir.x * c.toro_impulse, c.toro_impulse / 3.0f, dir.z * c.toro_impulse); ++hits; dealt += s.hit_damage; stop = END_HIT; }
        }
        if (!stop && e.wall_ahead) stop = END_WALL;
        if (!stop && travelled >= limit - 0.05f) stop = END_DISTANCE;
        if (!stop && tp >= c.toro_max_seconds) stop = END_TIME;
        if (stop) {
            end = stop; phase = PH_RECOVER; t_phase = w.t; s.recover = true; s.clip_speed = 1.0f;
        }
        return s;
    }
    // recover: the rest of the Tornado clip (frames 33..end at speed 1) or the bull's stance, then the AI again
    float len = c.recover;
    if (move == MOVE_TORNADO) len += (c.tornado_clip_frames - c.tornado_windup_frames - c.tornado_fire_frames) / 25.0f;
    if (tp >= len) { s.release = true; s.end = end ? end : END_DONE; phase = PH_IDLE; }
    return s;
}

}  // namespace ps
