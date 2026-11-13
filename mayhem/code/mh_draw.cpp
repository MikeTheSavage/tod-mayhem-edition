// mh_draw.cpp - draws the [mayhem] debris pieces (Box3D bodies and grit of mayhem/code/mh_game.cpp) and their dust with
// the game's own Direct3D 9 device, inside the game's world pass: GfxInternal::Render 0x421B30 draws the world layers
// 0..0x14 (0x421C66), then calls the frame-end motion blur 0x45D5E0 (0x421C71) and only then the overlay layers (HUD,
// menus). The MH_DRAW site wraps that motion-blur call: the pieces are drawn first, so they are depth-tested against the
// world, blurred with it and stay under the HUD. The camera is the device's own VIEW / PROJECTION transform of the world
// layers (the game sets them with SetTransform per layer, subsystem_camera.md); a frame whose projection is not a
// perspective one draws nothing.
// Every render / texture-stage / sampler state, the shaders, the FVF / declaration, texture 0 and the WORLD transform
// that is touched is read first and written back afterwards (the pattern of gp_mayhem.cpp). The procedural textures are
// made once per device in D3DPOOL_MANAGED (they survive a Reset; nothing here holds a D3DPOOL_DEFAULT resource or a state
// block, so a device Reset is never blocked).
// Branch mayhem-polish: style 1 pieces show a patch of the broken prop's OWN texture (the game's managed texture,
// AddRef'd by the debris while it lives), and soft dust puffs (alpha blended, no depth write) are drawn after them.
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "../../dll/src/gp_glue.h"
#include "mh_draw.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
static const DWORD A_RENDERER = 0xA39F14;   // renderer; +0 = IDirect3DDevice9* (REF MT_DEVICE)
static const DWORD VT_MODEL = 0x9CCE48;     // Model node vtable (REF PH_VT_MODEL)

static IDirect3DDevice9* g_dev = nullptr;
static IDirect3DTexture9* g_tex[MH_MATERIALS] = {};
static IDirect3DTexture9* g_puff_tex = nullptr;
static bool g_failed = false;
static float g_last_view[16], g_last_proj[16]; static bool g_have_cam = false;

static uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
static float noise2(int x, int y, int seed) { return (float)(hash32((uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663) ^ (uint32_t)seed) & 0xFFFF) / 65535.0f; }
static float smooth_noise(float x, float y, int seed) {   // value noise, bilinear
    int ix = (int)floorf(x), iy = (int)floorf(y); float fx = x - ix, fy = y - iy;
    float a = noise2(ix, iy, seed), b = noise2(ix + 1, iy, seed), c = noise2(ix, iy + 1, seed), d = noise2(ix + 1, iy + 1, seed);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
static DWORD rgb(float r, float g, float b) {
    auto c = [](float v) { int i = (int)(v * 255.0f + 0.5f); return (DWORD)(i < 0 ? 0 : (i > 255 ? 255 : i)); };
    return 0xFF000000u | c(r) << 16 | c(g) << 8 | c(b);
}
static DWORD argb(float a, float r, float g, float b) {
    auto c = [](float v) { int i = (int)(v * 255.0f + 0.5f); return (DWORD)(i < 0 ? 0 : (i > 255 ? 255 : i)); };
    return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
}
// procedural textures (no asset): weathered wood planks along u, dull metal, painted red metal (style 0 barrels / tanks),
// charred metal (style 1 fallback for a blast whose prop texture is unknown)
static void make_pixels(int mat, DWORD* px, int W, int H) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float u = (float)x / W, v = (float)y / H, r, g, b;
            if (mat == MH_WOOD) {
                float grain = 0.5f + 0.5f * sinf((v * 9.0f + smooth_noise(u * 6.0f, v * 3.0f, 7) * 2.5f) * 6.2831853f);
                float fine = smooth_noise(u * 48.0f, v * 6.0f, 3);
                float base = 0.62f + 0.18f * grain + 0.12f * fine;
                float seam = (y % (H / 4)) < 2 ? 0.55f : 1.0f;                 // plank seams
                float knot = smooth_noise(u * 5.0f, v * 5.0f, 11); knot = knot > 0.86f ? 0.7f : 1.0f;
                r = 0.56f * base * seam * knot; g = 0.40f * base * seam * knot; b = 0.24f * base * seam * knot;
            } else if (mat == MH_METAL) {
                float n = smooth_noise(u * 20.0f, v * 20.0f, 5) * 0.5f + smooth_noise(u * 80.0f, v * 4.0f, 9) * 0.5f;
                float s = 0.45f + 0.25f * n; r = s * 0.95f; g = s * 0.97f; b = s;
            } else if (mat == MH_RED) {   // chipped red paint over dark metal
                float n = smooth_noise(u * 16.0f, v * 16.0f, 13), chip = smooth_noise(u * 40.0f, v * 40.0f, 17);
                float band = (y % (H / 3)) < 3 ? 0.6f : 1.0f;
                if (chip > 0.78f) { r = 0.25f; g = 0.22f; b = 0.2f; }
                else { r = (0.62f + 0.2f * n) * band; g = (0.10f + 0.05f * n) * band; b = (0.07f + 0.04f * n) * band; }
            } else {   // MH_CHAR: sooty, scorched metal with a little rust
                float n = smooth_noise(u * 12.0f, v * 12.0f, 21) * 0.6f + smooth_noise(u * 50.0f, v * 50.0f, 23) * 0.4f;
                float rust = smooth_noise(u * 7.0f, v * 7.0f, 29);
                float s = 0.12f + 0.16f * n;
                r = s; g = s * 0.93f; b = s * 0.88f;
                if (rust > 0.7f) { float k = (rust - 0.7f) / 0.3f; r += 0.20f * k; g += 0.08f * k; b += 0.02f * k; }
            }
            px[y * W + x] = rgb(r, g, b);
        }
}
static void fill_mips(IDirect3DTexture9* t, const DWORD* px0, int W, int H, bool* ok) {
    static DWORD lv[128 * 128];
    int w = W, h = H;
    memcpy(lv, px0, (size_t)W * H * 4);
    for (UINT l = 0; l < t->GetLevelCount(); ++l) {
        D3DLOCKED_RECT lr;
        if (FAILED(t->LockRect(l, &lr, nullptr, 0))) { *ok = false; return; }
        for (int y = 0; y < h; ++y) memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch, lv + (size_t)y * w, (size_t)w * 4);
        t->UnlockRect(l);
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;   // next level: 2x2 box filter
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                DWORD s[4] = {lv[(2 * y) * w + 2 * x], lv[(2 * y) * w + (w > 1 ? 2 * x + 1 : 2 * x)], lv[(h > 1 ? 2 * y + 1 : 2 * y) * w + 2 * x],
                              lv[(h > 1 ? 2 * y + 1 : 2 * y) * w + (w > 1 ? 2 * x + 1 : 2 * x)]};
                DWORD o = 0;
                for (int sh = 0; sh < 32; sh += 8) { DWORD acc = 0; for (int k = 0; k < 4; ++k) acc += (s[k] >> sh) & 255; o |= ((acc + 2) / 4) << sh; }
                lv[y * nw + x] = o;
            }
        w = nw; h = nh;
    }
    *ok = true;
}
static IDirect3DTexture9* make_tex(IDirect3DDevice9* d, int mat) {
    const int W = 128, H = 128;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(d->CreateTexture(W, H, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
    static DWORD px[128 * 128];
    make_pixels(mat, px, W, H);
    bool ok = false; fill_mips(t, px, W, H, &ok);
    if (!ok) { t->Release(); return nullptr; }
    return t;
}
// the dust puff: a soft round cloud (white; the vertex colour tints it), lumpy edge, alpha falls off to 0 at the rim
static IDirect3DTexture9* make_puff(IDirect3DDevice9* d) {
    const int W = 64, H = 64;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(d->CreateTexture(W, H, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
    static DWORD px[64 * 64];
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float u = (x + 0.5f) / W * 2.0f - 1.0f, v = (y + 0.5f) / H * 2.0f - 1.0f;
            float r = sqrtf(u * u + v * v);
            float n = smooth_noise(x * 0.11f, y * 0.11f, 41) * 0.6f + smooth_noise(x * 0.27f, y * 0.27f, 43) * 0.4f;
            float edge = 1.0f - r * (0.85f + 0.3f * n);
            float a = edge <= 0.0f ? 0.0f : (edge >= 0.55f ? 1.0f : edge / 0.55f);
            a = a * a * (3.0f - 2.0f * a) * (0.65f + 0.35f * n);
            float s = 0.86f + 0.14f * n;
            px[y * W + x] = argb(a, s, s, s);
        }
    bool ok = false; fill_mips(t, px, W, H, &ok);
    if (!ok) { t->Release(); return nullptr; }
    return t;
}
static bool is_com(void* p) {   // a COM object of a loaded module, not garbage
    if (!p) return false;
    DWORD vt = 0, fn = 0;
    __try { vt = RD32((DWORD)(uintptr_t)p); fn = vt ? RD32(vt) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    HMODULE m = nullptr;
    return fn && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)fn, &m);
}
static IDirect3DDevice9* device() {
    DWORD ren = 0; __try { ren = RD32(A_RENDERER); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    IDirect3DDevice9* d = nullptr; __try { d = ren ? (IDirect3DDevice9*)(uintptr_t)RD32(ren) : nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return is_com(d) ? d : nullptr;
}
static bool resources(IDirect3DDevice9* d) {
    if (d == g_dev && g_tex[0] && g_puff_tex) return true;
    if (g_failed && d == g_dev) return false;
    if (g_dev && d != g_dev) { gp_log("mayhem: draw: the device changed (%p -> %p): new textures\n", g_dev, d); for (auto& t : g_tex) t = nullptr; g_puff_tex = nullptr; }
    g_dev = d; g_failed = false;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(d->GetCreationParameters(&cp)) || (cp.BehaviorFlags & D3DCREATE_PUREDEVICE)) { gp_log("mayhem: draw: pure device (no state read-back): debris not drawn\n"); g_failed = true; return false; }
    for (int m = 0; m < MH_MATERIALS; ++m) {
        g_tex[m] = make_tex(d, m);
        if (!g_tex[m]) { gp_log("mayhem: draw: texture %d could not be made: debris not drawn\n", m); g_failed = true; return false; }
    }
    g_puff_tex = make_puff(d);
    if (!g_puff_tex) { gp_log("mayhem: draw: dust texture could not be made: debris not drawn\n"); g_failed = true; return false; }
    gp_log("mayhem: draw: debris textures ready on device %p\n", d);
    return true;
}

// ---------------------------------------------------------------- state: read, set, write back
enum { NRS = 20 };
struct Saved {
    DWORD rs[NRS]; DWORD ts0[8], ts1[2]; DWORD ss[7];
    IDirect3DBaseTexture9* tex; DWORD fvf; IDirect3DVertexShader9* vs; IDirect3DPixelShader9* ps; IDirect3DVertexDeclaration9* decl;
    D3DMATRIX world, view, proj;
};
static const D3DRENDERSTATETYPE RS[NRS] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
    D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_STENCILENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE,
    D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_SPECULARENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_COLORVERTEX, D3DRS_NORMALIZENORMALS,
    D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP};
static const DWORD RS_SET[NRS] = {D3DZB_TRUE, TRUE, D3DCMP_LESSEQUAL, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, 0xF, FALSE, FALSE,
    D3DFILL_SOLID, D3DSHADE_GOURAUD, FALSE, 0, TRUE, FALSE, D3DBLEND_ONE, D3DBLEND_ZERO, D3DBLENDOP_ADD};
static const D3DTEXTURESTAGESTATETYPE TS0[8] = {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
    D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS};
static const DWORD TS0_SET[8] = {D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_DIFFUSE, 0, D3DTTFF_DISABLE};
static const D3DSAMPLERSTATETYPE SS[7] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE, D3DSAMP_MAXANISOTROPY};
static const DWORD SS_SET[7] = {D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, FALSE, 1};
static void save_state(IDirect3DDevice9* d, Saved* s) {
    for (int i = 0; i < NRS; ++i) d->GetRenderState(RS[i], &s->rs[i]);
    for (int i = 0; i < 8; ++i) d->GetTextureStageState(0, TS0[i], &s->ts0[i]);
    d->GetTextureStageState(1, D3DTSS_COLOROP, &s->ts1[0]); d->GetTextureStageState(1, D3DTSS_ALPHAOP, &s->ts1[1]);
    for (int i = 0; i < 7; ++i) d->GetSamplerState(0, SS[i], &s->ss[i]);
    s->tex = nullptr; d->GetTexture(0, &s->tex);
    s->fvf = 0; d->GetFVF(&s->fvf);
    s->vs = nullptr; d->GetVertexShader(&s->vs); s->ps = nullptr; d->GetPixelShader(&s->ps);
    s->decl = nullptr; d->GetVertexDeclaration(&s->decl);
    d->GetTransform(D3DTS_WORLD, &s->world); d->GetTransform(D3DTS_VIEW, &s->view); d->GetTransform(D3DTS_PROJECTION, &s->proj);
}
static void restore_state(IDirect3DDevice9* d, Saved* s) {
    for (int i = 0; i < NRS; ++i) d->SetRenderState(RS[i], s->rs[i]);
    for (int i = 0; i < 8; ++i) d->SetTextureStageState(0, TS0[i], s->ts0[i]);
    d->SetTextureStageState(1, D3DTSS_COLOROP, s->ts1[0]); d->SetTextureStageState(1, D3DTSS_ALPHAOP, s->ts1[1]);
    for (int i = 0; i < 7; ++i) d->SetSamplerState(0, SS[i], s->ss[i]);
    d->SetTexture(0, s->tex); if (s->tex) s->tex->Release();
    d->SetVertexShader(s->vs); if (s->vs) s->vs->Release();
    d->SetPixelShader(s->ps); if (s->ps) s->ps->Release();
    if (s->decl) { d->SetVertexDeclaration(s->decl); s->decl->Release(); } else d->SetFVF(s->fvf);
    if (s->decl && s->fvf) d->SetFVF(s->fvf);
    d->SetTransform(D3DTS_WORLD, &s->world); d->SetTransform(D3DTS_VIEW, &s->view); d->SetTransform(D3DTS_PROJECTION, &s->proj);
}
static bool perspective(const D3DMATRIX& p) {   // a plain LH perspective: _34 = 1, _44 = 0, no shear (DpProjOk of TOD HD)
    return fabsf(p._34 - 1.0f) < 1e-4f && fabsf(p._44) < 1e-4f && fabsf(p._11) > 1e-6f && fabsf(p._22) > 1e-6f &&
           fabsf(p._12) < 1e-5f && fabsf(p._21) < 1e-5f && fabsf(p._14) < 1e-5f && fabsf(p._24) < 1e-5f;
}

// ---------------------------------------------------------------- drawing
struct V { float x, y, z; DWORD c; float u, v; };
static void qrot(const float* q, const float* v, float* o) {   // TRUE rotation (x, y, z, w)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty); o[1] = v[1] + w * ty + (z * tx - x * tz); o[2] = v[2] + w * tz + (x * ty - y * tx);
}
static const float SUN[3] = {0.36f, 0.82f, 0.44f};   // light from above, a little from the side
static int box_tris(const MhPiece& p, V* out, float light) {
    static const float N[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};   // -x +x -y +y -z +z
    float h[3] = {p.half[0] * p.scale, p.half[1] * p.scale, p.half[2] * p.scale};
    float c[8][3];
    for (int i = 0; i < 8; ++i) {
        float l[3] = {(i & 4) ? h[0] : -h[0], (i & 2) ? h[1] : -h[1], (i & 1) ? h[2] : -h[2]}, w[3];
        qrot(p.rot, l, w);
        c[i][0] = p.pos[0] + w[0]; c[i][1] = p.pos[1] + w[1]; c[i][2] = p.pos[2] + w[2];
    }
    // corner index bits: 4 = +x, 2 = +y, 1 = +z -> faces in that numbering
    static const int FB[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    float lmax = p.half[0] > p.half[1] ? p.half[0] : p.half[1]; if (p.half[2] > lmax) lmax = p.half[2];
    int k = 0;
    for (int f = 0; f < 6; ++f) {
        float n[3]; qrot(p.rot, N[f], n);
        float lam = n[0] * SUN[0] + n[1] * SUN[1] + n[2] * SUN[2];
        float b = (0.42f + 0.58f * (lam > 0 ? lam : 0.0f) + (n[1] < 0 ? -0.1f : 0.0f)) * light * p.tint;
        DWORD col = rgb(b, b, b);
        // the face's two extents in metres (u along the longer one)
        int a0 = f / 2 == 0 ? 1 : 0, a1 = f / 2 == 2 ? 1 : 2;
        float e0 = 2 * h[a0], e1 = 2 * h[a1];
        bool swap = e1 > e0;
        float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        V q[4];
        for (int j = 0; j < 4; ++j) {
            const float* P = c[FB[f][j]];
            q[j].x = P[0]; q[j].y = P[1]; q[j].z = P[2]; q[j].c = col;
            if (p.side > 0.0f) {   // style 1: the prop's own texture, a patch scaled to the face (the grain runs along the piece)
                static const int UA[6] = {2, 1, 0, 2, 1, 0}, VA[6] = {1, 2, 2, 0, 0, 1};   // the axes of edges 0->1 / 1->2 (FB)
                float su = p.side * (p.half[UA[f]] / (lmax > 1e-6f ? lmax : 1.0f));
                float sv = p.side * (p.half[VA[f]] / (lmax > 1e-6f ? lmax : 1.0f));
                float fo = (float)f * 0.013f * p.side;   // each face a little shifted (no mirrored twins)
                q[j].u = p.u0 + fo + uv[j][0] * su; q[j].v = p.v0 + uv[j][1] * sv;
            } else {               // style 0: 1 texture repeat per 0.8 m
                float uu = uv[j][0] * (swap ? e1 : e0) / 0.8f, vv = uv[j][1] * (swap ? e0 : e1) / 0.8f;
                q[j].u = uu + p.uv_off; q[j].v = vv + p.uv_off * 0.37f;
            }
        }
        out[k++] = q[0]; out[k++] = q[1]; out[k++] = q[2];
        out[k++] = q[0]; out[k++] = q[2]; out[k++] = q[3];
    }
    return k;
}
// a texture is usable on this device (it may outlive a device change; then the procedural one is drawn)
static bool tex_on(IDirect3DTexture9* t, IDirect3DDevice9* d) {
    if (!t) return false;
    IDirect3DDevice9* td = nullptr; bool ok = false;
    __try { ok = SUCCEEDED(t->GetDevice(&td)) && td == d; if (td) td->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

static V g_v[MH_DRAW_MAX * 36];
static int g_order[MH_DRAW_MAX];
// pieces shrink away inside 1.8 m of the camera and are gone at 0.5 m: a chunk flying past the lens would fill the
// screen as a big flat slab (the cover the player hides behind breaks right next to the camera)
static float near_fade(const D3DMATRIX& v, const float* p) {
    float z = p[0] * v._13 + p[1] * v._23 + p[2] * v._33 + v._43;
    float t = (z - 0.5f) / 1.3f;
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    return t * t * (3.0f - 2.0f * t);
}
// the textures made ahead (at the first world frame), not in the frame of the first break: ~7-9 ms once
bool mh_draw_warm() { IDirect3DDevice9* d = device(); return d && resources(d); }
int mh_draw_pieces(const MhPiece* pieces, int n, const MhPuff* puffs, int np, float light, double* ms) {
    if (n <= 0 && np <= 0) return 0;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    IDirect3DDevice9* d = device();
    if (!d || !resources(d)) return 0;
    D3DMATRIX view, proj;
    if (FAILED(d->GetTransform(D3DTS_VIEW, &view)) || FAILED(d->GetTransform(D3DTS_PROJECTION, &proj))) return 0;
    if (!perspective(proj)) {   // not the world camera (should not happen after the world layers): the last good one
        if (!g_have_cam) return 0;
        memcpy(&view, g_last_view, 64); memcpy(&proj, g_last_proj, 64);
    } else { memcpy(g_last_view, &view, 64); memcpy(g_last_proj, &proj, 64); g_have_cam = true; }
    if (n > MH_DRAW_MAX) n = MH_DRAW_MAX;
    if (np > MH_PUFF_MAX) np = MH_PUFF_MAX;
    Saved sv; save_state(d, &sv);
    for (int i = 0; i < NRS; ++i) d->SetRenderState(RS[i], RS_SET[i]);
    for (int i = 0; i < 8; ++i) d->SetTextureStageState(0, TS0[i], TS0_SET[i]);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (int i = 0; i < 7; ++i) d->SetSamplerState(0, SS[i], SS_SET[i]);
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    D3DMATRIX I; memset(&I, 0, sizeof I); I._11 = I._22 = I._33 = I._44 = 1.0f;
    d->SetTransform(D3DTS_WORLD, &I);
    d->SetTransform(D3DTS_VIEW, &view); d->SetTransform(D3DTS_PROJECTION, &proj);
    // opaque pieces: one draw call per texture (the prop's own ones, then the procedural materials)
    int drawn = 0, left = 0;
    for (int i = 0; i < n; ++i) if (pieces[i].scale > 0.01f) g_order[left++] = i;
    while (left > 0) {
        const MhPiece& first = pieces[g_order[0]];
        IDirect3DTexture9* t = (IDirect3DTexture9*)first.tex;
        bool own = tex_on(t, d);
        int mat = first.mat < 0 || first.mat >= MH_MATERIALS ? MH_WOOD : first.mat;
        int k = 0, keep = 0;
        for (int j = 0; j < left; ++j) {
            const MhPiece& p = pieces[g_order[j]];
            bool same = first.tex ? p.tex == first.tex : (!p.tex && p.mat == first.mat);
            if (!same) { g_order[keep++] = g_order[j]; continue; }
            MhPiece q = p;
            if (!own) { q.side = q.tex ? 0.0f : q.side; }   // the prop texture is gone: the material tiling
            q.scale *= near_fade(view, q.pos);
            if (q.scale <= 0.01f) continue;
            k += box_tris(q, g_v + k, light); ++drawn;
        }
        left = keep;
        if (!k) continue;
        d->SetTexture(0, own ? (IDirect3DBaseTexture9*)t : g_tex[mat]);
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(k / 3), g_v, sizeof(V));
    }
    // dust: camera-facing puffs, alpha blended over the world, no depth write (far ones first)
    if (np > 0) {
        float rx = view._11, ry = view._21, rz = view._31, ux = view._12, uy = view._22, uz = view._32;   // camera right / up
        float fx = view._13, fy = view._23, fz = view._33;                                                // camera forward
        int ord[MH_PUFF_MAX]; float dep[MH_PUFF_MAX];
        for (int i = 0; i < np; ++i) { ord[i] = i; dep[i] = puffs[i].pos[0] * fx + puffs[i].pos[1] * fy + puffs[i].pos[2] * fz; }
        for (int i = 1; i < np; ++i) { int o = ord[i]; int j = i; while (j > 0 && dep[ord[j - 1]] < dep[o]) { ord[j] = ord[j - 1]; --j; } ord[j] = o; }
        int k = 0;
        for (int ii = 0; ii < np; ++ii) {
            const MhPuff& p = puffs[ord[ii]];
            if (!(p.alpha > 0.003f) || !(p.radius > 0.0f)) continue;
            DWORD col = argb(p.alpha, p.rgb[0] * light, p.rgb[1] * light, p.rgb[2] * light);
            float r = p.radius;
            // pulled toward the camera by 0.6 radius: a flat card cutting into the prop it came from shows a hard edge
            float c3[3] = {p.pos[0] - fx * 0.6f * r, p.pos[1] - fy * 0.6f * r, p.pos[2] - fz * 0.6f * r};
            float cx[4] = {-1, 1, 1, -1}, cy[4] = {-1, -1, 1, 1}, tu[4] = {0, 1, 1, 0}, tv[4] = {1, 1, 0, 0};
            float rot = (float)(ord[ii] * 1.7);   // each puff turned differently (no stamped look)
            float cs = cosf(rot), sn = sinf(rot);
            V q[4];
            for (int j = 0; j < 4; ++j) {
                float a = cx[j] * cs - cy[j] * sn, b = cx[j] * sn + cy[j] * cs;
                q[j].x = c3[0] + (rx * a + ux * b) * r; q[j].y = c3[1] + (ry * a + uy * b) * r; q[j].z = c3[2] + (rz * a + uz * b) * r;
                q[j].c = col; q[j].u = tu[j]; q[j].v = tv[j];
            }
            g_v[k++] = q[0]; g_v[k++] = q[1]; g_v[k++] = q[2]; g_v[k++] = q[0]; g_v[k++] = q[2]; g_v[k++] = q[3];
        }
        if (k) {
            d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            d->SetTexture(0, g_puff_tex);
            d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(k / 3), g_v, sizeof(V));
        }
    }
    restore_state(d, &sv);
    QueryPerformanceCounter(&t1);
    if (ms) *ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    return drawn;
}

// ---------------------------------------------------------------- the broken prop's own texture
// ModelAsset +20h TextureSets (8 bytes: TextureAsset*, index), TextureAsset +28h & ~3 = Texture*, Texture +4 & ~3 =
// IDirect3DTexture9* (REF MH_TEXSETS: ModelAsset::GetPivotMeshTexture 0x856F10; REF MH_D3DTEX: Texture::GetDirect3DTexture
// 0x4634A0). The sets are an engine vector: +24h count, +28h capacity (the append of 0x856F60's caller grows it 1.5x).
// A candidate must be a texture of this device's own class: its vtable is the one of the procedural textures above (a
// wrapped or foreign object is never called; the debris then shows the procedural material).
static void decode_565(unsigned c, float* o) { o[0] = ((c >> 11) & 31) / 31.0f; o[1] = ((c >> 5) & 63) / 63.0f; o[2] = (c & 31) / 31.0f; }
// one level read back into RGBA floats (w x h <= 64 x 64); false = a format this does not read
static bool read_level(IDirect3DTexture9* t, UINT lvl, D3DFORMAT fmt, int w, int h, float (*px)[4]) {
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(lvl, &lr, nullptr, D3DLOCK_READONLY))) return false;
    bool ok = true;
    __try {
        const BYTE* b = (const BYTE*)lr.pBits;
        if (fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5) {
            int bs = fmt == D3DFMT_DXT1 ? 8 : 16;
            for (int by = 0; by < (h + 3) / 4; ++by)
                for (int bx = 0; bx < (w + 3) / 4; ++bx) {
                    const BYTE* k = b + (size_t)by * lr.Pitch + (size_t)bx * bs;
                    const BYTE* cb = bs == 16 ? k + 8 : k;
                    unsigned c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8; DWORD idx = *(const DWORD*)(cb + 4);
                    float pal[4][4]; decode_565(c0, pal[0]); decode_565(c1, pal[1]);
                    pal[0][3] = pal[1][3] = 1.0f;
                    bool four = bs == 16 || c0 > c1;
                    for (int a = 0; a < 3; ++a) {
                        pal[2][a] = four ? (2 * pal[0][a] + pal[1][a]) / 3 : (pal[0][a] + pal[1][a]) / 2;
                        pal[3][a] = four ? (pal[0][a] + 2 * pal[1][a]) / 3 : 0.0f;
                    }
                    pal[2][3] = 1.0f; pal[3][3] = four ? 1.0f : 0.0f;
                    float al[8] = {1, 1, 1, 1, 1, 1, 1, 1};
                    if (fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5) {
                        float a0 = k[0] / 255.0f, a1 = k[1] / 255.0f; al[0] = a0; al[1] = a1;
                        for (int i = 2; i < 8; ++i) al[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7 : (i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : (i == 6 ? 0.0f : 1.0f));
                    }
                    unsigned long long abits = 0; for (int i = 0; i < 6; ++i) abits |= (unsigned long long)k[2 + i] << (8 * i);
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) {
                            int X = bx * 4 + x, Y = by * 4 + y; if (X >= w || Y >= h) continue;
                            int i = y * 4 + x;
                            const float* c = pal[(idx >> (2 * i)) & 3];
                            float a = c[3];
                            if (fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3) a = ((k[i / 2] >> (4 * (i & 1))) & 15) / 15.0f;
                            else if (fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5) a = al[(abits >> (3 * i)) & 7];
                            float* o = px[Y * w + X]; o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = a;
                        }
                }
        } else {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const BYTE* r = b + (size_t)y * lr.Pitch;
                    float* o = px[y * w + x];
                    if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
                        DWORD c = ((const DWORD*)r)[x];
                        o[0] = ((c >> 16) & 255) / 255.0f; o[1] = ((c >> 8) & 255) / 255.0f; o[2] = (c & 255) / 255.0f;
                        o[3] = fmt == D3DFMT_A8R8G8B8 ? (c >> 24) / 255.0f : 1.0f;
                    } else if (fmt == D3DFMT_R8G8B8) {
                        const BYTE* c = r + 3 * x; o[0] = c[2] / 255.0f; o[1] = c[1] / 255.0f; o[2] = c[0] / 255.0f; o[3] = 1.0f;
                    } else if (fmt == D3DFMT_R5G6B5) {
                        decode_565(((const WORD*)r)[x], o); o[3] = 1.0f;
                    } else if (fmt == D3DFMT_A1R5G5B5 || fmt == D3DFMT_X1R5G5B5) {
                        WORD c = ((const WORD*)r)[x];
                        o[0] = ((c >> 10) & 31) / 31.0f; o[1] = ((c >> 5) & 31) / 31.0f; o[2] = (c & 31) / 31.0f;
                        o[3] = fmt == D3DFMT_A1R5G5B5 ? (float)(c >> 15) : 1.0f;
                    } else if (fmt == D3DFMT_A4R4G4B4) {
                        WORD c = ((const WORD*)r)[x];
                        o[0] = ((c >> 8) & 15) / 15.0f; o[1] = ((c >> 4) & 15) / 15.0f; o[2] = (c & 15) / 15.0f; o[3] = (c >> 12) / 15.0f;
                    } else { ok = false; break; }
                }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    t->UnlockRect(lvl);
    return ok;
}
static void analyse(IDirect3DTexture9* t, MhTexInfo* o) {
    o->avg[0] = o->avg[1] = o->avg[2] = 0.45f; o->ncell = 0;
    UINT levels = t->GetLevelCount(); D3DSURFACE_DESC sd;
    UINT lvl = 0;
    for (UINT l = 0; l < levels; ++l) {
        if (FAILED(t->GetLevelDesc(l, &sd))) return;
        lvl = l;
        if (sd.Width <= 64 && sd.Height <= 64) break;
    }
    if (FAILED(t->GetLevelDesc(lvl, &sd)) || sd.Width > 64 || sd.Height > 64 || sd.Width < 1 || sd.Height < 1) return;
    static float px[64 * 64][4];
    int w = (int)sd.Width, h = (int)sd.Height;
    if (!read_level(t, lvl, sd.Format, w, h, px)) return;
    double s[3] = {0, 0, 0}; double n = 0;
    float cs[16 * 16][4]; memset(cs, 0, sizeof cs);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float* p = px[y * w + x];
            int cx = x * 16 / w, cy = y * 16 / h;
            float* c = cs[cy * 16 + cx];
            c[0] += p[3]; c[1] += 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]; c[2] += 1.0f;
            if (p[3] >= 0.5f) { for (int a = 0; a < 3; ++a) s[a] += p[a]; n += 1.0; }
        }
    for (int i = 0; i < 256; ++i) {
        float k = cs[i][2]; bool used = k > 0.0f && cs[i][0] / k >= 0.5f && cs[i][1] / k >= 0.035f;   // opaque, not the empty black
        o->cell[i] = used ? 1 : 0; o->ncell += used ? 1 : 0;
    }
    if (n > 0.0) for (int a = 0; a < 3; ++a) o->avg[a] = (float)(s[a] / n);
}
bool mh_prop_texture(DWORD model, MhTexInfo* out) {
    memset(out, 0, sizeof *out);
    IDirect3DDevice9* d = device();
    if (!d || !resources(d)) { strcpy(out->why, "no device"); return false; }
    DWORD tex_vt = RD32((DWORD)(uintptr_t)g_tex[0]);
    IDirect3DTexture9* best = nullptr; UINT best_area = 0; DWORD best_ta = 0;
    int n = 0, n_null = 0, n_vt = 0, n_dev = 0, n_small = 0; DWORD vt_seen = 0;
    __try {
        if (!model) { strcpy(out->why, "no model"); return false; }
        if (RD32(model) != VT_MODEL) { _snprintf(out->why, sizeof out->why - 1, "node %08X vtable %08X is no Model", model, RD32(model)); return false; }
        DWORD asset = RD32(model + 0x50); if (!asset) { strcpy(out->why, "no ModelAsset"); return false; }
        DWORD sets = RD32(asset + 0x20); n = (int)RD32(asset + 0x24);
        if (!sets || n <= 0 || n > 64) { _snprintf(out->why, sizeof out->why - 1, "TextureSets %08X count %d", sets, n); return false; }
        for (int i = 0; i < n; ++i) {
            DWORD ta = RD32(sets + (DWORD)i * 8) & ~3u; if (!ta) { ++n_null; continue; }
            DWORD tx = RD32(ta + 0x28) & ~3u; if (!tx) { ++n_null; continue; }
            IDirect3DTexture9* t = (IDirect3DTexture9*)(uintptr_t)(RD32(tx + 4) & ~3u);
            if (!t) { ++n_null; continue; }
            if (RD32((DWORD)(uintptr_t)t) != tex_vt) { ++n_vt; vt_seen = RD32((DWORD)(uintptr_t)t); continue; }
            if (!tex_on(t, d)) { ++n_dev; continue; }
            D3DSURFACE_DESC sd; if (FAILED(t->GetLevelDesc(0, &sd)) || sd.Type != D3DRTYPE_SURFACE) { ++n_small; continue; }
            UINT area = sd.Width * sd.Height;
            if (sd.Width >= 8 && sd.Height >= 8 && area > best_area) { best = t; best_area = area; best_ta = ta; out->w = (int)sd.Width; out->h = (int)sd.Height; }
            else if (!(sd.Width >= 8 && sd.Height >= 8)) ++n_small;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy(out->why, "exception reading the model"); return false; }
    if (!best) {
        _snprintf(out->why, sizeof out->why - 1, "%d set(s): %d empty, %d other vtable (%08X, ours %08X), %d other device, %d too small",
                  n, n_null, n_vt, vt_seen, tex_vt, n_dev, n_small);
        return false;
    }
    __try {
        const char* nm = (const char*)(uintptr_t)(RD32(best_ta + 4) & ~3u);   // Asset +4 m_ResourcePath (tagged pointer)
        if (nm && !IsBadStringPtrA(nm, 200)) { strncpy(out->name, nm, sizeof out->name - 1); out->name[sizeof out->name - 1] = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { out->name[0] = 0; }
    best->AddRef();
    out->tex = best;
    __try { analyse(best, out); } __except (EXCEPTION_EXECUTE_HANDLER) { out->ncell = 0; }
    return true;
}
void mh_tex_addref(void* tex) {
    if (!tex) return;
    __try { ((IDirect3DTexture9*)tex)->AddRef(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void mh_tex_release(void* tex) {
    if (!tex) return;
    __try { ((IDirect3DTexture9*)tex)->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
