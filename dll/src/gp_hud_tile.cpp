// gp_hud_tile.cpp - pure logic of the [minimap] tile packs (see gp_hud_tile.h)
#include "gp_hud_tile.h"
#include <math.h>
#include <string.h>

namespace hud {

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float rdf(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool parse_pack_head(const uint8_t* d, size_t n, PackHead* h) {
    if (n < 32 || memcmp(d, "MMP1", 4) || rd32(d + 4) != 1) return false;
    memset(h, 0, sizeof *h);
    h->x0 = rdf(d + 8); h->z1 = rdf(d + 12); h->texel = rdf(d + 16);
    h->tile_px = (int)rd32(d + 20); h->border = (int)rd32(d + 24); h->levels = (int)rd32(d + 28);
    if (h->tile_px != 512 || h->border < 0 || h->border > 32 || !(h->texel > 0.01f) || h->levels < 1 || h->levels > PACK_MAX_LEVELS) return false;
    if (n < 32 + (size_t)h->levels * 12) return false;
    for (int i = 0; i < h->levels; ++i) {
        const uint8_t* p = d + 32 + i * 12;
        PackLevel& l = h->lv[i];
        l.cols = (int)rd32(p); l.rows = (int)rd32(p + 4); l.table = rd32(p + 8);
        l.tile_m = (float)(h->tile_px - 2 * h->border) * h->texel * (float)(1 << i);
        if (l.cols <= 0 || l.rows <= 0 || l.cols > 2047 || l.rows > 4095 || l.table < 32) return false;
    }
    return true;
}

uint32_t paper_under(uint32_t colour) {
    const uint32_t paper[3] = {0xB0, 0xE4, 0xF6};    // b, g, r of radar.BMP's paper
    uint32_t out = (((colour >> 24) * 216u / 255u) << 24);
    for (int i = 0; i < 3; ++i) out |= (((colour >> (8 * i)) & 255u) * paper[i] / 255u) << (8 * i);
    return out;
}

int pick_level(const PackHead& h, float mpp) {
    int L = 0;
    while (L + 1 < h.levels && mpp >= h.texel * (float)(1 << L) * 1.8f) ++L;
    return L;
}

TileRect tile_rect(const PackHead& h, int level, int col, int row) {
    float t = h.lv[level].tile_m;
    TileRect r;
    r.x0 = h.x0 + (float)col * t; r.x1 = h.x0 + (float)(col + 1) * t;
    r.z1 = h.z1 - (float)row * t; r.z0 = h.z1 - (float)(row + 1) * t;
    return r;
}

int tiles_near(const PackHead& h, int level, float px, float pz, float radius, TileRef* out, int cap) {
    const PackLevel& l = h.lv[level];
    int c0 = (int)floorf((px - radius - h.x0) / l.tile_m), c1 = (int)floorf((px + radius - h.x0) / l.tile_m);
    int r0 = (int)floorf((h.z1 - pz - radius) / l.tile_m), r1 = (int)floorf((h.z1 - pz + radius) / l.tile_m);
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 >= l.cols) c1 = l.cols - 1;
    if (r1 >= l.rows) r1 = l.rows - 1;
    int n = 0;
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c) {
            TileRect q = tile_rect(h, level, c, r);
            float dx = px < q.x0 ? q.x0 - px : (px > q.x1 ? px - q.x1 : 0.0f), dz = pz < q.z0 ? q.z0 - pz : (pz > q.z1 ? pz - q.z1 : 0.0f);
            float d = sqrtf(dx * dx + dz * dz);
            if (d > radius) continue;
            int at = n;
            while (at > 0 && out[at - 1].dist > d) --at;
            if (at >= cap) continue;
            int last = n < cap ? n : cap - 1;
            for (int k = last; k > at; --k) out[k] = out[k - 1];
            out[at].col = c; out[at].row = r; out[at].dist = d;
            if (n < cap) ++n;
        }
    return n;
}

size_t mip_chain_texels(int w) { size_t n = 0; for (;;) { n += (size_t)w * w; if (w == 1) return n; w /= 2; } }

// ---------------------------------------------------------------- inflate (RFC 1951): canonical codes, one bit at a time
namespace {
struct In { const uint8_t* p; const uint8_t* end; uint32_t acc; int n; bool bad; };
inline int bits(In& s, int need) {
    uint32_t v = s.acc;
    while (s.n < need) {
        if (s.p >= s.end) { s.bad = true; return 0; }
        v |= (uint32_t)*s.p++ << s.n; s.n += 8;
    }
    s.acc = v >> need; s.n -= need;
    return (int)(v & ((1u << need) - 1u));
}
struct Huff { short count[16]; short symbol[320]; };
inline int decode(In& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= bits(s, 1);
        if (s.bad) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
void construct(Huff& h, const uint8_t* len, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; ++i) ++h.count[len[i]];
    h.count[0] = 0;
    short offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; ++i) offs[i + 1] = (short)(offs[i] + h.count[i]);
    for (int i = 0; i < n; ++i) if (len[i]) h.symbol[offs[len[i]]++] = (short)i;
}
const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
bool codes(In& s, const Huff& hl, const Huff& hd, uint8_t* out, size_t cap, size_t& at) {
    for (;;) {
        int sym = decode(s, hl);
        if (sym < 0) return false;
        if (sym < 256) { if (at >= cap) return false; out[at++] = (uint8_t)sym; continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = LBASE[sym] + bits(s, LEXT[sym]);
        int ds = decode(s, hd);
        if (ds < 0 || ds >= 30) return false;
        size_t dist = (size_t)DBASE[ds] + (size_t)bits(s, DEXT[ds]);
        if (s.bad || dist > at || at + (size_t)len > cap) return false;
        for (int i = 0; i < len; ++i, ++at) out[at] = out[at - dist];
    }
}
bool inflate_raw(const uint8_t* d, size_t n, uint8_t* out, size_t want) {
    In s = {d, d + n, 0, 0, false};
    size_t at = 0;
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    Huff hl, hd; uint8_t len[320];
    for (;;) {
        int last = bits(s, 1), type = bits(s, 2);
        if (s.bad) return false;
        if (type == 0) {
            s.acc = 0; s.n = 0;
            if (s.end - s.p < 4) return false;
            size_t l = (size_t)s.p[0] | (size_t)s.p[1] << 8; s.p += 4;
            if ((size_t)(s.end - s.p) < l || at + l > want) return false;
            memcpy(out + at, s.p, l); s.p += l; at += l;
        } else if (type == 1) {
            int i = 0;
            for (; i < 144; ++i) len[i] = 8;
            for (; i < 256; ++i) len[i] = 9;
            for (; i < 280; ++i) len[i] = 7;
            for (; i < 288; ++i) len[i] = 8;
            construct(hl, len, 288);
            for (i = 0; i < 30; ++i) len[i] = 5;
            construct(hd, len, 30);
            if (!codes(s, hl, hd, out, want, at)) return false;
        } else if (type == 2) {
            int nl = bits(s, 5) + 257, nd = bits(s, 5) + 1, nc = bits(s, 4) + 4;
            if (s.bad || nl > 286 || nd > 30) return false;
            memset(len, 0, 19);
            for (int i = 0; i < nc; ++i) len[ORDER[i]] = (uint8_t)bits(s, 3);
            construct(hl, len, 19);
            int i = 0;
            while (i < nl + nd) {
                int sym = decode(s, hl);
                if (sym < 0) return false;
                if (sym < 16) { len[i++] = (uint8_t)sym; continue; }
                int prev = 0, rep;
                if (sym == 16) { if (!i) return false; prev = len[i - 1]; rep = 3 + bits(s, 2); }
                else if (sym == 17) rep = 3 + bits(s, 3);
                else rep = 11 + bits(s, 7);
                if (i + rep > nl + nd) return false;
                while (rep--) len[i++] = (uint8_t)prev;
            }
            construct(hd, len + nl, nd);
            construct(hl, len, nl);
            if (!codes(s, hl, hd, out, want, at)) return false;
        } else return false;
        if (last) break;
    }
    return !s.bad && at == want;
}
}  // namespace

bool decode_tile(const uint8_t* d, size_t n, int w, uint32_t* argb, uint8_t* scratch) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (n < 33 || memcmp(d, SIG, 8)) return false;
    uint32_t pal[256]; memset(pal, 0, sizeof pal);
    int colours = 0; bool head = false;
    const uint8_t* idat = nullptr; size_t idat_n = 0;
    for (size_t at = 8; at + 12 <= n; ) {
        uint32_t len = be32(d + at);
        if (len > n - at - 12) return false;
        const uint8_t* t = d + at + 4; const uint8_t* b = t + 4;
        if (!memcmp(t, "IHDR", 4)) {
            if (len != 13 || (int)be32(b) != w || (int)be32(b + 4) != w || b[8] != 8 || b[9] != 3 || b[12] != 0) return false;
            head = true;
        } else if (!memcmp(t, "PLTE", 4)) {
            if (len % 3 || len > 768) return false;
            colours = (int)len / 3;
            for (int i = 0; i < colours; ++i) pal[i] = 0xFF000000u | (uint32_t)b[i * 3] << 16 | (uint32_t)b[i * 3 + 1] << 8 | b[i * 3 + 2];
        } else if (!memcmp(t, "tRNS", 4)) {
            if (len > 256) return false;
            for (uint32_t i = 0; i < len; ++i) pal[i] = (pal[i] & 0xFFFFFFu) | (uint32_t)b[i] << 24;
        } else if (!memcmp(t, "IDAT", 4)) {
            if (idat) return false;
            idat = b; idat_n = len;
        } else if (!memcmp(t, "IEND", 4)) break;
        at += 12 + (size_t)len;
    }
    if (!head || !colours || !idat || idat_n < 6) return false;
    size_t want = (size_t)(w + 1) * w;
    if (!inflate_raw(idat + 2, idat_n - 2, scratch, want)) return false;
    for (int y = 0; y < w; ++y) {
        const uint8_t* row = scratch + (size_t)y * (w + 1);
        if (row[0]) return false;
        uint32_t* o = argb + (size_t)y * w;
        for (int x = 0; x < w; ++x) o[x] = pal[row[x + 1]];
    }
    return true;
}

void build_mips(uint32_t* c, int w) {
    while (w > 1) {
        uint32_t* d = c + (size_t)w * w;
        int nw = w / 2;
        for (int y = 0; y < nw; ++y) {
            const uint32_t* a = c + (size_t)(2 * y) * w; const uint32_t* b = a + w;
            uint32_t* o = d + (size_t)y * nw;
            for (int x = 0; x < nw; ++x) {
                uint32_t s[4] = {a[2 * x], a[2 * x + 1], b[2 * x], b[2 * x + 1]};
                uint32_t A = 0, R = 0, G = 0, B = 0;
                for (int k = 0; k < 4; ++k) { uint32_t q = s[k] >> 24; A += q; R += ((s[k] >> 16) & 255) * q; G += ((s[k] >> 8) & 255) * q; B += (s[k] & 255) * q; }
                o[x] = A ? ((A + 2) / 4) << 24 | (R / A) << 16 | (G / A) << 8 | (B / A) : 0;
            }
        }
        c = d; w = nw;
    }
}

}  // namespace hud
