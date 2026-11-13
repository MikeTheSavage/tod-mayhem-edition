// gp_mayhem_logic.cpp - see gp_mayhem_logic.h. Pure C++ (no Windows, no Direct3D), unit tested by
// tests/dll/test_mayhem.cpp.
#include "gp_mayhem_logic.h"
#include <math.h>
#include <string.h>

namespace mt {

// ================================================================ helpers
uint32_t xorshift(uint32_t* s) { uint32_t x = *s ? *s : 0x9E3779B9u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }
float frand(uint32_t* s) { return (float)(xorshift(s) >> 8) * (1.0f / 16777216.0f); }
float smoothstep(float a, float b, float x) {
    if (b == a) return x < a ? 0.0f : 1.0f;
    float t = (x - a) / (b - a); t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3.0f - 2.0f * t);
}
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

uint32_t premul(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }

void noise_tile(uint8_t* out, int w, int h, int cell_x, int cell_y, int octaves, uint32_t seed) {
    static float acc[256 * 256];
    static float g[256 * 256];
    if (w * h > 256 * 256 || w <= 0 || h <= 0) return;
    for (int i = 0; i < w * h; ++i) acc[i] = 0.0f;
    uint32_t s = seed * 2654435761u + 1;
    float amp = 1.0f;
    for (int o = 0; o < octaves; ++o, amp *= 0.5f, cell_x = cell_x > 2 ? cell_x / 2 : 2, cell_y = cell_y > 2 ? cell_y / 2 : 2) {
        int gw = w / cell_x; if (gw < 1) gw = 1;
        int gh = h / cell_y; if (gh < 1) gh = 1;
        for (int i = 0; i < gw * gh; ++i) g[i] = frand(&s);
        for (int y = 0; y < h; ++y) {
            float fy = (float)y * gh / h; int y0 = (int)fy; float ty = fy - y0; ty = ty * ty * (3 - 2 * ty);
            int ya = y0 % gh, yb = (y0 + 1) % gh;
            for (int x = 0; x < w; ++x) {
                float fx = (float)x * gw / w; int x0 = (int)fx; float tx = fx - x0; tx = tx * tx * (3 - 2 * tx);
                int xa = x0 % gw, xb = (x0 + 1) % gw;
                float a = g[ya * gw + xa] + (g[ya * gw + xb] - g[ya * gw + xa]) * tx;
                float b = g[yb * gw + xa] + (g[yb * gw + xb] - g[yb * gw + xa]) * tx;
                acc[y * w + x] += amp * (a + (b - a) * ty);
            }
        }
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < w * h; ++i) { if (acc[i] < lo) lo = acc[i]; if (acc[i] > hi) hi = acc[i]; }
    float k = hi > lo ? 255.0f / (hi - lo) : 0.0f;
    for (int i = 0; i < w * h; ++i) out[i] = (uint8_t)clampi((int)((acc[i] - lo) * k + 0.5f), 0, 255);
}

// ================================================================ affine maps
Aff aff_id() { Aff r = {1, 0, 0, 1, 0, 0}; return r; }
Aff aff_mul(const Aff& A, const Aff& B) {
    Aff r;
    r.a = A.a * B.a + A.b * B.c; r.b = A.a * B.b + A.b * B.d;
    r.c = A.c * B.a + A.d * B.c; r.d = A.c * B.b + A.d * B.d;
    r.tx = A.a * B.tx + A.b * B.ty + A.tx; r.ty = A.c * B.tx + A.d * B.ty + A.ty;
    return r;
}
bool aff_inv(const Aff& A, Aff* out) {
    float det = A.a * A.d - A.b * A.c;
    if (fabsf(det) < 1e-12f) return false;
    float i = 1.0f / det;
    Aff r; r.a = A.d * i; r.b = -A.b * i; r.c = -A.c * i; r.d = A.a * i;
    r.tx = -(r.a * A.tx + r.b * A.ty); r.ty = -(r.c * A.tx + r.d * A.ty);
    *out = r; return true;
}
void aff_apply(const Aff& A, float x, float y, float* ox, float* oy) { *ox = A.a * x + A.b * y + A.tx; *oy = A.c * x + A.d * y + A.ty; }
bool uv_to_screen(const float p[4][2], const float uv[4][2], Aff* out) {
    // columns: corner 1 - corner 0 (down the left edge), corner 3 - corner 0 (along the top edge)
    float u1 = uv[1][0] - uv[0][0], v1 = uv[1][1] - uv[0][1], u3 = uv[3][0] - uv[0][0], v3 = uv[3][1] - uv[0][1];
    float x1 = p[1][0] - p[0][0], y1 = p[1][1] - p[0][1], x3 = p[3][0] - p[0][0], y3 = p[3][1] - p[0][1];
    float det = u1 * v3 - u3 * v1;
    if (fabsf(det) < 1e-9f) return false;
    float i = 1.0f / det;
    // inverse of [[u1 u3][v1 v3]] = i * [[v3 -u3][-v1 u1]]
    Aff r;
    r.a = (x1 * v3 - x3 * v1) * i; r.b = (-x1 * u3 + x3 * u1) * i;
    r.c = (y1 * v3 - y3 * v1) * i; r.d = (-y1 * u3 + y3 * u1) * i;
    r.tx = p[0][0] - (r.a * uv[0][0] + r.b * uv[0][1]);
    r.ty = p[0][1] - (r.c * uv[0][0] + r.d * uv[0][1]);
    if (!(fabsf(r.a) + fabsf(r.b) > 1e-6f) || !(fabsf(r.c) + fabsf(r.d) > 1e-6f)) return false;
    *out = r; return true;
}
Aff art_to_screen(const Aff& uv) { Aff s = {1.0f / ART_W, 0, 0, 1.0f / ART_H, 0, 0}; return aff_mul(uv, s); }

void title_quad(float W, float H, bool wide_flag, float um, float vm, float P[4][2], float UV[4][2]) {
    float sx = W * 0.00125f, sy = wide_flag ? H * 0.0022222223f : sx;
    float w = 800.0f * sx, h = 600.0f * sy, x0 = (W - w) * 0.5f, y0 = (H - h) * 0.5f;
    float p[4][2] = {{x0, y0}, {x0, y0 + h}, {x0 + w, y0 + h}, {x0 + w, y0}}, uv[4][2] = {{0, 0}, {0, vm}, {um, vm}, {um, 0}};
    memcpy(P, p, sizeof p); memcpy(UV, uv, sizeof uv);
}

// ================================================================ region and placement
bool Region::from_header(const float res[11]) {
    if (!(res[0] > 0.3f && res[0] < 5.0f) || !(res[4] > res[2]) || !(res[5] > res[3]) || !(res[6] > 1.0f)) return false;
    aspect = res[0]; bx0 = res[2]; by0 = res[3]; bx1 = res[4]; by1 = res[5]; band_px = res[6];
    if (res[9] > 0.01f) { fire_base0 = res[7]; fire_slope = res[8]; fire_band = res[9]; }
    return true;
}
Place place_default() { Place p = {103.0f, 238.0f, 254.0f}; return p; }   // ~760 px wide at 1080p, over the barrels
Rect e_rect(const Region& r, const Place& p) {
    Rect e;
    e.w = p.bw / (r.bx1 - r.bx0); e.h = e.w / r.aspect;
    e.x0 = p.bx - r.bx0 * e.w; e.y0 = p.by - r.by0 * e.h;
    return e;
}

// ================================================================ burn constants
float burn_time(const BurnK& k, const Region& reg, float u, float v, float n01) {
    float du = (u - k.iu) * reg.aspect, dv = v - k.iv;
    return sqrtf(du * du + dv * dv) * k.kT + (n01 - 0.5f) * k.noise;
}
bool burn_setup(const uint8_t* R, const uint8_t* A, int fw, int fh, const Region& reg, float iu, float iv, BurnK* k) {
    if (!R || fw <= 0 || fh <= 0) return false;
    (void)A;
    // the ignition must be inside the hole (R < 128): else the nearest hole texel
    int ix = clampi((int)(iu * fw), 0, fw - 1), iy = clampi((int)(iv * fh), 0, fh - 1);
    bool any = false; float best = 1e30f; int bx = ix, by = iy;
    if (R[iy * fw + ix] >= 128) {
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) {
                if (R[y * fw + x] >= 128) continue;
                float dx = (x - ix) * reg.aspect * fh / (float)fw, dy = (float)(y - iy);
                float d = dx * dx + dy * dy;
                if (d < best) { best = d; bx = x; by = y; }
            }
        iu = (bx + 0.5f) / fw; iv = (by + 0.5f) / fh;
    }
    k->iu = iu; k->iv = iv;
    float dmax = 0.0f;
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x) {
            if (R[y * fw + x] >= 128) continue;
            any = true;
            float du = ((x + 0.5f) / fw - iu) * reg.aspect, dv = (y + 0.5f) / fh - iv;
            float d = du * du + dv * dv;
            if (d > dmax) dmax = d;
        }
    if (!any || dmax <= 0.0f) return false;
    dmax = sqrtf(dmax);
    k->kT = 0.82f / dmax;
    k->ke = k->kT * (2.0f * reg.band_px) / fh;
    k->rim = 0.016f * k->kT; k->curl = 0.014f * k->kT; k->scorch = 0.050f * k->kT;
    k->noise = 0.055f * 2.0f * k->kT;
    k->inv_ramp = 1.0f / 0.16f;
    return true;
}

// ================================================================ timeline
void Timeline::start(bool already_burnt) { t = already_burnt ? delay + reveal + 10.0f : 0.0f; }
float Timeline::front() const {
    if (t < delay) return 0.0f;
    float x = (t - delay) / (reveal > 0.05f ? reveal : 0.05f);
    if (x >= 1.0f) return 1.02f;
    return 0.02f + (1.0f - powf(1.0f - x, 1.6f));            // fast ignition, slowing as it spreads
}
float Timeline::calm() const {
    float after = t - delay - reveal;
    return after <= 0.0f ? 0.0f : clampf(after / 1.2f, 0.0f, 1.0f);
}

// ================================================================ sparks
void Sparks::spawn(float u, float v, float speed, float life) {
    if (n >= MAX) return;
    Spark& s = p[n++];
    s.u = u; s.v = v;
    s.vu = (frand(&rng) - 0.5f) * 0.10f * speed;
    s.vv = -(0.10f + frand(&rng) * 0.20f) * speed;
    s.age = 0.0f; s.life = life * (0.6f + 0.8f * frand(&rng));
    s.size = 0.5f + frand(&rng) * 0.8f;
    s.phase = frand(&rng) * 6.2831853f;
}
void Sparks::step(float dt, float aspect) {
    int k = 0;
    for (int i = 0; i < n; ++i) {
        Spark s = p[i];
        s.age += dt;
        if (s.age >= s.life) continue;
        float wob = sinf(s.age * 7.0f + s.phase) * 0.05f;
        s.u += (s.vu + wob) * dt / aspect; s.v += s.vv * dt;
        s.vv += 0.05f * dt; s.vu *= 1.0f - 0.8f * dt;
        p[k++] = s;
    }
    n = k;
}
uint32_t spark_colour(const Spark& s, float intensity, float* size_out) {
    float t = s.life > 0 ? s.age / s.life : 1.0f; t = clampf(t, 0, 1);
    float r, g, b;
    if (t < 0.25f) { float u = t / 0.25f; r = 255; g = 236 - 80 * u; b = 160 - 120 * u; }
    else if (t < 0.65f) { float u = (t - 0.25f) / 0.4f; r = 255; g = 156 - 96 * u; b = 40 - 28 * u; }
    else { float u = (t - 0.65f) / 0.35f; r = 255 - 90 * u; g = 60 - 40 * u; b = 12 - 8 * u; }
    float fade = (1.0f - t); fade = fade * sqrtf(fade);
    float twinkle = 0.75f + 0.25f * sinf(s.age * 31.0f + s.phase * 3.0f);
    float m = clampf(fade * twinkle * intensity, 0, 1);
    if (size_out) *size_out = s.size * (1.0f - 0.45f * t);
    return premul((uint8_t)(r * m), (uint8_t)(g * m), (uint8_t)(b * m), 0);
}

// ================================================================ fire flipbook
void fire_frames(float t, float c4[4], float* blend) {
    float fr = t * 12.0f; if (fr < 0) fr = 0;
    int f0 = (int)fr; float w = fr - f0; f0 %= 48; int f1 = (f0 + 1) % 48;
    c4[0] = (f0 % 8) / 8.0f; c4[1] = (f0 / 8) / 8.0f; c4[2] = (f1 % 8) / 8.0f; c4[3] = (f1 / 8) / 8.0f;
    *blend = w;
}

}  // namespace mt
