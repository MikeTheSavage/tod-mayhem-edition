// gp_heat_logic.cpp - pure logic of [mayhem_heat]; see gp_heat_logic.h. Tested by tests/dll/test_heat.cpp.
#include "gp_heat_logic.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace ht {

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

const char* ev_name(int ev) {
    static const char* n[EV_COUNT] = {"enemy kill", "cop kill", "civilian kill", "death nearby", "explosion",
                                      "destruction", "barrel", "car explosion", "loco move", "style points"};
    return ev >= 0 && ev < EV_COUNT ? n[ev] : "?";
}

static int maxlv(const Cfg& c) { return c.max_level < 1 ? 1 : (c.max_level > LEVELS ? LEVELS : c.max_level); }
static float cap_eff(const Cfg& c) { int m = maxlv(c); return m < LEVELS ? c.thr[m] : (c.cap > c.thr[LEVELS - 1] ? c.cap : c.thr[LEVELS - 1] + 1.0f); }

float level_start(const Cfg& c, int level) {
    if (level <= 0) return 0.0f;
    if (level > maxlv(c)) return cap_eff(c) + 1.0f;
    return c.thr[level - 1];
}
int level_of(const Cfg& c, float heat) {
    int m = maxlv(c), lv = 0;
    for (int l = 1; l <= m; ++l) if (heat >= c.thr[l - 1]) lv = l;
    return lv;
}
// heat <-> continuous level coordinate (each band is one unit, so cooling takes decay_seconds per level)
static float band_top(const Cfg& c, int level) { return level >= maxlv(c) ? cap_eff(c) : level_start(c, level + 1); }
static float to_x(const Cfg& c, float heat) {
    int l = level_of(c, heat);
    float a = level_start(c, l), b = band_top(c, l);
    return (float)l + (b > a ? clampf((heat - a) / (b - a), 0.0f, 1.0f) : 0.0f);
}
static float from_x(const Cfg& c, float x) {
    if (x <= 0.0f) return 0.0f;
    int l = (int)floorf(x); if (l > maxlv(c)) return cap_eff(c);
    float a = level_start(c, l), b = band_top(c, l);
    return a + (b - a) * (x - (float)l);
}

void State::event(const Cfg& c, int ev, float amount, bool by_ram) {
    if (ev < 0 || ev >= EV_COUNT || !(amount > 0.0f)) return;
    float p = c.pts[ev] * amount * c.gain * (by_ram ? 1.0f : c.own_weight);
    if (!(p > 0.0f)) return;
    heat = clampf(heat + p, 0.0f, cap_eff(c));
    if (by_ram) { since_event = 0.0f; cooling = false; }
}

int State::tick(const Cfg& c, float dt, int enemies_near) {
    if (!(dt >= 0.0f)) dt = 0.0f;
    since_event += dt;
    cooling = heat > 0.0f && since_event >= c.calm_seconds;
    if (cooling) {
        float rate = (c.decay_seconds > 0.1f ? 1.0f / c.decay_seconds : 10.0f) * (enemies_near > 0 ? c.near_factor : 1.0f);
        float x = to_x(c, heat) - rate * dt;
        heat = x <= 0.0f ? 0.0f : from_x(c, x);
    }
    int nl = level_of(c, heat);
    if (nl > level) { ++ups; last_up_level = nl; }
    level = nl;
    if (level > peak) peak = level;
    if (heat <= 0.0f) { peak = 0; last_up_level = 0; }
    return level;
}

// ---- radar look
Rgb tint_for(float shown, float strength) {
    static const Rgb k[LEVELS + 1] = {{1.00f, 1.00f, 1.00f}, {1.00f, 0.86f, 0.66f}, {1.00f, 0.74f, 0.46f},
                                      {1.00f, 0.60f, 0.32f}, {1.00f, 0.40f, 0.20f}, {1.00f, 0.18f, 0.11f}};
    shown = clampf(shown, 0.0f, (float)LEVELS);
    int i = (int)floorf(shown); if (i >= LEVELS) i = LEVELS - 1;
    float t = shown - (float)i;
    Rgb r = {lerpf(k[i].r, k[i + 1].r, t), lerpf(k[i].g, k[i + 1].g, t), lerpf(k[i].b, k[i + 1].b, t)};
    float s = clampf(strength, 0.0f, 1.0f);
    return Rgb{1.0f + (r.r - 1.0f) * s, 1.0f + (r.g - 1.0f) * s, 1.0f + (r.b - 1.0f) * s};
}
float follow(float shown, int level, float dt) {
    const float speed = 2.0f;                  // levels per second: one step reads as a short fade
    float target = (float)level, d = target - shown, step = speed * (dt > 0.0f ? dt : 0.0f);
    if (fabsf(d) <= step) return target;
    return shown + (d > 0.0f ? step : -step);
}
Ring edge_ring(float shown, int max_level, double t) {
    Ring r = {0, 0, 0, 0};
    float top = (float)(max_level < 1 ? 1 : (max_level > LEVELS ? LEVELS : max_level));
    float a = clampf((shown - (top - 0.5f)) * 2.0f, 0.0f, 1.0f);
    if (a <= 0.0f) return r;
    const double period = 1.6;                 // slow: red <-> dark
    float p = (float)(0.5 + 0.5 * sin(t * 6.283185307179586 / period));
    float cr = lerpf(0.06f, 1.00f, p), cg = lerpf(0.00f, 0.10f, p), cb = lerpf(0.00f, 0.05f, p);
    r.a = a * lerpf(0.80f, 0.92f, p);
    r.r = cr * r.a; r.g = cg * r.a; r.b = cb * r.a;
    return r;
}
float icon_fill(const Cfg& c, float heat, int i) {
    if (i < 0 || i >= maxlv(c)) return 0.0f;
    float a = level_start(c, i), b = level_start(c, i + 1);
    if (b <= a) return heat >= b ? 1.0f : 0.0f;
    return clampf((heat - a) / (b - a), 0.0f, 1.0f);
}
float icon_glow(const State& s, int i, double t) {
    if (!s.cooling || s.level < 1 || i != s.level - 1) return 1.0f;
    return (float)(0.78 + 0.22 * cos(t * 6.283185307179586 / 1.1));
}

// ---- geometry
bool uv_affine(const float P[4][2], const float UV[4][2], Aff* o) {
    float du1 = UV[1][0] - UV[0][0], dv1 = UV[1][1] - UV[0][1], du3 = UV[3][0] - UV[0][0], dv3 = UV[3][1] - UV[0][1];
    float det = du1 * dv3 - du3 * dv1;
    if (fabsf(det) < 1e-6f) return false;
    float i00 = dv3 / det, i01 = -du3 / det, i10 = -dv1 / det, i11 = du1 / det;   // inverse of [[du1 du3][dv1 dv3]]
    float px1 = P[1][0] - P[0][0], py1 = P[1][1] - P[0][1], px3 = P[3][0] - P[0][0], py3 = P[3][1] - P[0][1];
    o->a = px1 * i00 + px3 * i10; o->b = px1 * i01 + px3 * i11;
    o->c = py1 * i00 + py3 * i10; o->d = py1 * i01 + py3 * i11;
    o->tx = P[0][0] - (o->a * UV[0][0] + o->b * UV[0][1]);
    o->ty = P[0][1] - (o->c * UV[0][0] + o->d * UV[0][1]);
    return fabsf(o->a * o->d - o->b * o->c) > 1e-3f;   // a quad squashed to a line / point maps nothing
}
void aff_apply(const Aff& m, float u, float v, float* x, float* y) { *x = m.a * u + m.b * v + m.tx; *y = m.c * u + m.d * v + m.ty; }
void icon_place(int i, int n, float scale, float* u, float* v, float* size) {
    float s = 0.17f * (scale > 0.2f ? scale : 0.2f);
    float r = DISC_R + s * 0.62f;
    const float start = 0.95f;                 // 54 degrees clockwise from the top: below the game's combo counter (x N) above the radar
    float step = s * 1.10f / r;
    (void)n;
    float th = start + step * (float)i;
    *u = DISC_U + r * sinf(th);
    *v = DISC_V - r * cosf(th);
    *size = s;
}

// ---- escalation
Esc escalation(int level, float scale) {
    static const Esc T[LEVELS + 1] = {
        {0.00f, 0, 0.0f, 1, false},
        {0.25f, 0, 0.0f, 1, false},
        {0.50f, 2, 45.0f, 1, false},
        {0.75f, 3, 35.0f, 2, true},
        {1.00f, 4, 28.0f, 2, true},
        {1.25f, 5, 22.0f, 3, true},
    };
    if (level < 0) level = 0; if (level > LEVELS) level = LEVELS;
    Esc e = T[level];
    if (!(scale > 0.0f)) return Esc{};
    e.share_add *= scale;
    if (e.wave > 0) { int w = (int)floorf((float)e.wave * scale + 0.5f); e.wave = w < 1 ? 1 : w; }
    if (e.interval_s > 0.0f) e.interval_s /= clampf(scale, 0.5f, 2.0f);
    return e;
}
float difficulty_scale(int d) {
    switch (d) { case 0: return 0.4f; case 1: return 0.7f; case 2: return 0.85f; case 3: return 1.0f; case 4: return 1.25f; default: return 0.7f; }
}

// ---- score multiplier
float score_multiplier(int level, int max_level, float top) {
    if (!(top > 1.0f)) return 1.0f;
    if (top > 10.0f) top = 10.0f;
    if (max_level > LEVELS) max_level = LEVELS;
    if (max_level < 2 || level < 2) return 1.0f;
    if (level > max_level) level = max_level;
    return 1.0f + (top - 1.0f) * (float)(level - 1) / (float)(max_level - 1);
}
int scale_score(int amount, float mult) {
    if (amount <= 0 || !(mult > 1.0f)) return amount;
    double v = floor((double)amount * (double)mult + 0.5);
    return v > 2000000000.0 ? 2000000000 : (int)v;
}
int score_label(float mult, char* out, int cap) {
    if (!out || cap < 1) return 0;
    out[0] = 0;
    if (!(mult > 1.004f)) return 0;
    int h = (int)floorf(mult * 100.0f + 0.5f);              // hundredths
    char b[16]; int k;
    if (h % 100 == 0) k = snprintf(b, sizeof b, "x%d", h / 100);
    else if (h % 10 == 0) k = snprintf(b, sizeof b, "x%d.%d", h / 100, (h / 10) % 10);
    else k = snprintf(b, sizeof b, "x%d.%02d", h / 100, h % 100);
    if (k < 0 || k >= cap) return 0;
    memcpy(out, b, (size_t)k + 1);
    return k;
}

}  // namespace ht
