// mh_logic.cpp - pure logic of [mayhem] chain reactions and breakable cover (see mh_logic.h).
#include "mh_logic.h"
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <algorithm>

namespace mh {

static float clampf(float v, float a, float b) { return v != v ? a : (v < a ? a : (v > b ? b : v)); }
static int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

void Cfg::clamp() {
    chain_radius = clampf(chain_radius, 1.0f, 40.0f);
    delay_min = clampf(delay_min, 0.0f, 5.0f);
    delay_max = clampf(delay_max, delay_min, 8.0f);
    max_per_second = clampf(max_per_second, 0.5f, 60.0f);
    max_per_frame = clampi(max_per_frame, 1, 16);
    max_depth = clampi(max_depth, 1, 64);
    max_pending = clampi(max_pending, 1, 256);
    chain_damage = clampf(chain_damage, 1.0f, 1.0e6f);
    car_blast_radius = clampf(car_blast_radius, 0.0f, 40.0f);
    car_blast_impulse = clampf(car_blast_impulse, 0.0f, 5000.0f);
    cover_health_scale = clampf(cover_health_scale, 0.02f, 1.0f);
    debris_max = clampi(debris_max, 0, 256);
    debris_life = clampf(debris_life, 0.5f, 120.0f);
    debris_pieces = clampi(debris_pieces, 1, 24);
    debris_impulse = clampf(debris_impulse, 0.0f, 10.0f);
    expose_radius = clampf(expose_radius, 0.0f, 10.0f);
    cover_blast_radius = clampf(cover_blast_radius, 0.0f, 30.0f);
    blast_debris = clampi(blast_debris, 0, 24);
    debris_style = clampi(debris_style, 0, 1);
    debris_size = clampf(debris_size, 0.3f, 3.0f);
    debris_bits = clampi(debris_bits, 0, 64);
    debris_dust = clampi(debris_dust, 0, 1);
}

uint32_t Rng::next() { uint32_t x = s ? s : 0x9E3779B9u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s = x; return x; }
float Rng::f01() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
float Rng::range(float a, float b) { return a + (b - a) * f01(); }

float chain_delay(const Cfg& c, float dist, float r01) {
    float span = c.delay_max - c.delay_min;
    float near01 = clampf(dist / (c.chain_radius > 0.0f ? c.chain_radius : 1.0f), 0.0f, 1.0f);
    // 60 % of the span follows the distance (the blast front reaches the far ones later), 40 % random
    return c.delay_min + span * (0.6f * near01 + 0.4f * clampf(r01, 0.0f, 1.0f));
}

bool Sched::pending(uint32_t node) const {
    for (auto& p : q) if (p.node == node) return true;
    return false;
}
bool Sched::add(const Cfg& c, const Pending& p) {
    if (!p.node) return false;
    if (pending(p.node)) { ++dropped_dup; return false; }
    if (p.depth > c.max_depth) { ++dropped_depth; return false; }
    if ((int)q.size() >= c.max_pending) { ++dropped_full; return false; }
    q.push_back(p);
    return true;
}
int Sched::pop_due(const Cfg& c, float now, Pending* out, int cap) {
    float burst = c.max_per_second < 1.0f ? 1.0f : c.max_per_second;
    if (last_t < 0.0f) { last_t = now; tokens = burst; }
    float dt = now - last_t;
    if (dt > 0.0f) tokens = std::min(burst, tokens + dt * c.max_per_second);
    if (dt < 0.0f) tokens = burst;   // the clock went back (load / retry): start afresh
    last_t = now;
    int n = 0;
    int lim = std::min(cap, c.max_per_frame);
    while (n < lim && tokens >= 1.0f) {
        int best = -1;
        for (int i = 0; i < (int)q.size(); ++i)
            if (q[i].due <= now && (best < 0 || q[i].due < q[best].due)) best = i;
        if (best < 0) break;
        out[n++] = q[best];
        q.erase(q.begin() + best);
        tokens -= 1.0f;
    }
    return n;
}

void FiredLog::add(uint32_t node, int depth, float t, uint32_t owner) {
    for (auto& f : v) if (f.node == node) { f.depth = depth; f.t = t; f.owner = owner; return; }
    if (v.size() >= 128) v.erase(v.begin());
    v.push_back(Fired{node, depth, t, owner});
}
bool FiredLog::find(uint32_t node, float now, int* depth, uint32_t* owner) const {
    for (auto& f : v)
        if (f.node == node && now - f.t <= 10.0f && now >= f.t - 0.001f) { *depth = f.depth; *owner = f.owner; return true; }
    return false;
}
void FiredLog::expire(float now) {
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Fired& f) { return now - f.t > 10.0f || now < f.t - 1.0f; }), v.end());
}

bool name_match(const char* name, const char* patterns) {
    if (!name || !patterns || !*name) return false;
    char low[256]; size_t n = strlen(name); if (n > 255) n = 255;
    for (size_t i = 0; i < n; ++i) low[i] = (char)tolower((unsigned char)name[i]);
    low[n] = 0;
    const char* p = patterns;
    while (*p) {
        while (*p == ',' || *p == ' ') ++p;
        const char* e = p; while (*e && *e != ',') ++e;
        const char* t = e; while (t > p && t[-1] == ' ') --t;
        size_t k = (size_t)(t - p);
        if (k > 0 && k < 64) {
            char pat[64];
            for (size_t i = 0; i < k; ++i) pat[i] = (char)tolower((unsigned char)p[i]);
            pat[k] = 0;
            if (strstr(low, pat)) return true;
        }
        p = e;
    }
    return false;
}

int make_debris(const Box& prop, int n, float burst, const float dir[3], float push, uint32_t seed, Piece* out, int cap) {
    if (n < 1 || cap < 1) return 0;
    if (n > cap) n = cap;
    float h[3] = {std::max(prop.h[0], 0.05f), std::max(prop.h[1], 0.05f), std::max(prop.h[2], 0.05f)};
    // the long horizontal axis is cut into slats (planks); the others into a 2 x m grid of chunks
    int ax = h[0] >= h[2] ? 0 : 2, side = ax == 0 ? 2 : 0;
    int rows = n >= 6 ? 2 : 1;                   // two layers (top / bottom) when there are enough pieces
    int per = (n + rows - 1) / rows;             // slats per layer
    Rng r; r.s = seed ? seed : 0xA511E9B3u;
    int k = 0;
    for (int row = 0; row < rows && k < n; ++row) {
        int m = std::min(per, n - k);
        for (int j = 0; j < m && k < n; ++j, ++k) {
            Piece& p = out[k];
            // a slat across the side axis: full length along ax / m ... alternate the cut so the pile looks broken
            float len = h[ax], thick = h[side] / (float)m;
            p.box.h[ax] = len * r.range(0.55f, 0.95f);
            p.box.h[side] = std::max(thick * 0.8f, 0.02f);
            p.box.h[1] = std::max(h[1] / (float)rows * 0.35f, 0.02f);
            p.box.c[ax] = prop.c[ax] + r.range(-0.15f, 0.15f) * len;
            p.box.c[side] = prop.c[side] - h[side] + thick * (2.0f * (float)j + 1.0f);
            p.box.c[1] = prop.c[1] - h[1] + (h[1] / (float)rows) * (2.0f * (float)row + 1.0f);
            // outward from the prop centre, upward lift, a push along the shot, some randomness
            float o[3] = {p.box.c[0] - prop.c[0], p.box.c[1] - prop.c[1] + h[1] * 0.5f, p.box.c[2] - prop.c[2]};
            float ol = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (ol < 1e-4f) { o[0] = 0; o[1] = 1; o[2] = 0; ol = 1; }
            for (int a = 0; a < 3; ++a) {
                float d = dir ? dir[a] : 0.0f;
                p.vel[a] = o[a] / ol * burst * r.range(0.6f, 1.2f) + d * push * r.range(0.7f, 1.3f);
            }
            p.vel[1] += burst * r.range(0.4f, 0.9f);
            for (int a = 0; a < 3; ++a) p.spin[a] = r.range(-8.0f, 8.0f) * (burst > 0.0f ? 1.0f : 0.0f);
        }
    }
    return k;
}

int make_chunks(const Box& prop, int n, float min_len, float max_len, int shape, float burst, const float dir[3], float push,
                uint32_t seed, Piece* out, int cap) {
    if (n < 1 || cap < 1) return 0;
    if (n > cap) n = cap;
    float h[3] = {std::max(prop.h[0], 0.02f), std::max(prop.h[1], 0.02f), std::max(prop.h[2], 0.02f)};
    float big = 2.0f * std::min(h[0], std::min(h[1], h[2])) + 2.0f * std::max(h[0], std::max(h[1], h[2]));
    big *= 0.5f;                                                  // the mean of the prop's smallest and largest edge
    min_len = clampf(min_len, 0.005f, 2.0f); max_len = clampf(max_len, min_len, 2.0f);
    if (max_len > big) max_len = big;
    if (min_len > max_len) min_len = max_len;
    Rng r; r.s = seed ? seed : 0xC0FFEE11u;
    for (int k = 0; k < n; ++k) {
        Piece& p = out[k];
        float t = r.f01(); t = t * t;                             // most pieces small, a few near max_len
        float L = min_len + (max_len - min_len) * t;
        float e[3];
        if (shape == SH_SPLINTER) { e[0] = L; e[1] = L * r.range(0.10f, 0.22f); e[2] = L * r.range(0.22f, 0.45f); }
        else if (shape == SH_PLATE) { e[0] = L; e[1] = L * r.range(0.06f, 0.14f); e[2] = L * r.range(0.45f, 0.9f); }
        else { e[0] = L; e[1] = L * r.range(0.55f, 0.95f); e[2] = L * r.range(0.6f, 1.0f); }
        int ax = (int)(r.next() % 3);                             // which prop axis the long edge follows
        for (int a = 0; a < 3; ++a) p.box.h[a] = std::max(0.5f * e[(a - ax + 3) % 3], 0.004f);
        for (int a = 0; a < 3; ++a) p.box.c[a] = prop.c[a] + r.range(-0.8f, 0.8f) * h[a];
        float o[3] = {p.box.c[0] - prop.c[0], p.box.c[1] - prop.c[1] + h[1] * 0.5f, p.box.c[2] - prop.c[2]};
        float ol = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (ol < 1e-4f) { o[0] = 0; o[1] = 1; o[2] = 0; ol = 1; }
        for (int a = 0; a < 3; ++a) {
            float d = dir ? dir[a] : 0.0f;
            p.vel[a] = o[a] / ol * burst * r.range(0.5f, 1.3f) + d * push * r.range(0.6f, 1.3f);
        }
        p.vel[1] += burst * r.range(0.35f, 0.9f);
        for (int a = 0; a < 3; ++a) p.spin[a] = burst > 0.0f ? r.range(-14.0f, 14.0f) : 0.0f;
    }
    return n;
}

void bit_step(Bit& b, float dt, float g) {
    if (b.rest || !(dt > 0.0f)) return;
    if (dt > 0.1f) dt = 0.1f;
    b.v[1] -= g * dt;
    for (int a = 0; a < 3; ++a) b.v[a] *= 1.0f - 0.25f * dt;    // a little air drag
    for (int a = 0; a < 3; ++a) b.p[a] += b.v[a] * dt;
    b.ang += b.spin * dt;
    float floor_y = b.ground + b.half;
    if (b.p[1] < floor_y) {
        b.p[1] = floor_y;
        if (b.v[1] < 0.0f) b.v[1] = -b.v[1] * 0.3f;
        b.v[0] *= 0.55f; b.v[2] *= 0.55f; b.spin *= 0.5f;
        if (b.v[1] < 0.45f && b.v[0] * b.v[0] + b.v[2] * b.v[2] < 0.09f) { b.rest = 1; b.v[0] = b.v[1] = b.v[2] = 0.0f; b.spin = 0.0f; }
    }
}
bool puff_at(const Puff& f, float* r, float* a) {
    if (!(f.life > 0.0f) || f.age >= f.life || f.age < 0.0f) { *r = 0.0f; *a = 0.0f; return false; }
    float t = f.age / f.life;
    float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);        // ease-out growth
    *r = f.r0 + (f.r1 - f.r0) * e;
    float in = f.age < 0.08f ? f.age / 0.08f : 1.0f;
    *a = f.alpha * in * (1.0f - t) * (1.0f - t);
    return true;
}
void puff_step(Puff& f, float dt) {
    if (!(dt > 0.0f)) return;
    f.age += dt;
    for (int a = 0; a < 3; ++a) f.p[a] += f.v[a] * dt;
    float k = 1.0f - 1.6f * dt; if (k < 0.0f) k = 0.0f;            // the burst slows quickly; it still rises a little
    f.v[0] *= k; f.v[2] *= k; f.v[1] = f.v[1] * k + 0.25f * dt;
}
void uv_patch(float cu, float cv, float side, float* u0, float* v0) {
    side = clampf(side, 0.0f, 1.0f);
    *u0 = clampf(cu - 0.5f * side, 0.0f, 1.0f - side);
    *v0 = clampf(cv - 0.5f * side, 0.0f, 1.0f - side);
}

int DebrisPool::take(int cap, float now, int* evicted) {
    *evicted = -1;
    if (cap <= 0) return -1;
    for (int i = 0; i < (int)s.size(); ++i) if (!s[i].used) { s[i].used = true; s[i].born = now; s[i].id = -1; return i; }
    if ((int)s.size() < cap) { s.push_back(DebrisSlot{-1, now, true}); return (int)s.size() - 1; }
    int old = 0;
    for (int i = 1; i < (int)s.size(); ++i) if (s[i].born < s[old].born) old = i;
    *evicted = s[old].id; s[old].born = now; s[old].id = -1;
    return old;
}
int DebrisPool::expired(float now, float life, int* ids, int cap) {
    int n = 0;
    for (auto& d : s)
        if (d.used && (now - d.born > life || now < d.born - 1.0f) && n < cap) { ids[n++] = d.id; d.used = false; d.id = -1; }
    return n;
}

float cover_extra_damage(float hit_damage, float health_scale) {
    if (!(hit_damage > 0.0f) || !(health_scale > 0.0f) || health_scale >= 1.0f) return 0.0f;
    return hit_damage * (1.0f / health_scale - 1.0f);
}

}  // namespace mh
