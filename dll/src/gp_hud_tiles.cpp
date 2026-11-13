// gp_hud_tiles.cpp - [minimap]: the map in the radar from a tile pack (<map>.mmp, hud/bake/minimap_bake.cpp, read with
// gp_hud_tile.h). A whole map at this resolution would not fit a 32-bit game, so only the tiles around the player are in
// memory: a pool of 12 textures, filled by a background thread that reads, decodes and builds mips, so that neither
// entering a map nor walking off a tile stalls the frame. The render thread uploads at most 2 tiles per frame and draws
// one quad per tile, with the round edge of the disc in a second texture stage.
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gp_glue.h"
#include "gp_hud_logic.h"
#include "gp_hud_tile.h"
#include "gp_hud_tiles.h"

namespace {

const int POOL = 12, BUFS = 3, TW = 512, UPLOADS_PER_FRAME = 2, MASK_W = 128;
const float MARGIN_M = 30.0f;             // a tile is read this far before it comes into the disc
const size_t CHAIN = 349525;              // hud::mip_chain_texels(512)

inline int make_key(int map, int level, int col, int row) { return map << 25 | level << 23 | row << 11 | col; }   // rows 12 bits, cols 11

struct Slot { IDirect3DTexture9* tex; int key; DWORD used; };
Slot g_slot[POOL];
IDirect3DTexture9* g_mask = nullptr;
DWORD g_frame = 0;

// shared with the worker
enum { B_FREE = 0, B_FILLING, B_READY, B_FAILED };
struct Buf { uint32_t* px; volatile LONG key; volatile LONG state; };
Buf g_buf[BUFS];
CRITICAL_SECTION g_cs; HANDLE g_ev = nullptr, g_thread = nullptr;
int g_want_map = -1; char g_want_path[MAX_PATH + 80] = "";      // under g_cs
volatile LONG g_open_map = -1, g_open_failed = -1;
int g_queue[POOL], g_nq = 0;                                     // under g_cs, nearest first
hud::PackHead g_pack;                                            // the worker writes these only while g_open_map is -1
uint32_t* g_table[hud::PACK_MAX_LEVELS];
// statistics: the worker writes, the render thread logs
volatile LONG g_tiles_read = 0, g_read_us_sum = 0, g_read_us_max = 0, g_open_us = 0, g_bytes_read = 0;
// the render thread's
int g_bad[16], g_nbad = 0;
int g_logged_open = -1, g_level_logged = -1;
double g_up_sum = 0, g_up_max = 0; int g_up_n = 0, g_used_slots = 0;

bool read_at(HANDLE f, uint32_t off, void* to, DWORD n) {
    LARGE_INTEGER q; q.QuadPart = off;
    DWORD got = 0;
    return SetFilePointerEx(f, q, nullptr, FILE_BEGIN) && ReadFile(f, to, n, &got, nullptr) && got == n;
}

DWORD WINAPI worker(void*) {
    HANDLE f = INVALID_HANDLE_VALUE; int open = -1;
    uint8_t* scratch = new uint8_t[(size_t)(TW + 1) * TW];
    uint8_t* file = nullptr; DWORD file_cap = 0;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    for (;;) {
        WaitForSingleObject(g_ev, INFINITE);
        for (;;) {
            int wm, key = -1, b = -1; char path[MAX_PATH + 80];
            EnterCriticalSection(&g_cs);
            wm = g_want_map;
            if (wm != open) strcpy_s(path, g_want_path);
            else if (g_nq > 0) {
                for (int i = 0; i < BUFS; ++i) if (g_buf[i].state == B_FREE) { b = i; break; }
                if (b >= 0) {
                    key = g_queue[0];
                    memmove(g_queue, g_queue + 1, sizeof(int) * (size_t)--g_nq);
                    g_buf[b].key = key; g_buf[b].state = B_FILLING;
                }
            }
            LeaveCriticalSection(&g_cs);
            if (wm != open) {
                InterlockedExchange(&g_open_map, -1);
                if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); f = INVALID_HANDLE_VALUE; }
                for (int i = 0; i < hud::PACK_MAX_LEVELS; ++i) { delete[] g_table[i]; g_table[i] = nullptr; }
                open = wm;
                if (wm < 0) continue;
                LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
                bool ok = false;
                f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    uint8_t head[hud::PACK_HEAD_BYTES]; DWORD got = 0;
                    if (ReadFile(f, head, sizeof head, &got, nullptr) && hud::parse_pack_head(head, got, &g_pack)) {
                        ok = true;
                        for (int i = 0; i < g_pack.levels && ok; ++i) {
                            size_t n = (size_t)g_pack.lv[i].cols * g_pack.lv[i].rows * 2;
                            g_table[i] = new uint32_t[n];
                            ok = read_at(f, g_pack.lv[i].table, g_table[i], (DWORD)(n * 4));
                        }
                    }
                }
                QueryPerformanceCounter(&t1);
                InterlockedExchange(&g_open_us, (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart));
                if (ok) InterlockedExchange(&g_open_map, wm); else InterlockedExchange(&g_open_failed, wm);
                continue;
            }
            if (b < 0) break;
            LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
            int level = (key >> 23) & 3, row = (key >> 11) & 4095, col = key & 2047;
            const uint32_t* e = g_table[level] + ((size_t)row * g_pack.lv[level].cols + col) * 2;
            uint32_t off = e[0], size = e[1];
            bool ok = off && size && size < (1u << 24);
            if (ok && size > file_cap) { delete[] file; file_cap = size * 2; file = new uint8_t[file_cap]; }
            ok = ok && read_at(f, off, file, size) && hud::decode_tile(file, size, TW, g_buf[b].px, scratch);
            if (ok) hud::build_mips(g_buf[b].px, TW);
            QueryPerformanceCounter(&t1);
            LONG us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart);
            InterlockedIncrement(&g_tiles_read); InterlockedExchangeAdd(&g_read_us_sum, us); InterlockedExchangeAdd(&g_bytes_read, (LONG)size);
            if (us > g_read_us_max) InterlockedExchange(&g_read_us_max, us);
            InterlockedExchange(&g_buf[b].state, ok ? B_READY : B_FAILED);
        }
    }
}

bool start() {
    if (g_thread) return true;
    for (int i = 0; i < BUFS; ++i) { g_buf[i].px = new uint32_t[CHAIN]; g_buf[i].state = B_FREE; }
    InitializeCriticalSection(&g_cs);
    g_ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 256 * 1024, worker, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
    return g_thread != nullptr;
}

// the disc's round edge: opaque out to 88 % of the radius, then a smooth fall to nothing; the outer texels are 0, so
// everything past the disc clamps to nothing
bool make_mask(IDirect3DDevice9* dev) {
    if (g_mask) return true;
    if (FAILED(dev->CreateTexture(MASK_W, MASK_W, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_mask, nullptr))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(g_mask->LockRect(0, &lr, nullptr, 0))) { g_mask->Release(); g_mask = nullptr; return false; }
    for (int y = 0; y < MASK_W; ++y) {
        DWORD* o = (DWORD*)((BYTE*)lr.pBits + (size_t)y * lr.Pitch);
        for (int x = 0; x < MASK_W; ++x) {
            float dx = (x + 0.5f - MASK_W * 0.5f) / (MASK_W * 0.49f), dy = (y + 0.5f - MASK_W * 0.5f) / (MASK_W * 0.49f);
            float t = (1.0f - sqrtf(dx * dx + dy * dy)) / 0.12f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            o[x] = (DWORD)(t * t * (3.0f - 2.0f * t) * 255.0f + 0.5f) << 24 | 0xFFFFFF;
        }
    }
    g_mask->UnlockRect(0);
    return true;
}

Slot* slot_of(int key) { for (int i = 0; i < POOL; ++i) if (g_slot[i].tex && g_slot[i].key == key) return &g_slot[i]; return nullptr; }

// the slot of a tile that just arrived: an unused texture, else the one out of use longest (never one this frame wants)
Slot* take_slot(IDirect3DDevice9* dev) {
    Slot* best = nullptr;
    for (int i = 0; i < POOL; ++i) {
        Slot& s = g_slot[i];
        if (!s.tex) {
            if (FAILED(dev->CreateTexture(TW, TW, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s.tex, nullptr))) { s.tex = nullptr; return nullptr; }
            s.key = -1; s.used = g_frame; ++g_used_slots;
            return &s;
        }
        if (s.used != g_frame && (!best || (LONG)(s.used - best->used) < 0)) best = &s;
    }
    return best;
}

bool upload(Slot* s, const uint32_t* px) {
    int w = TW; UINT levels = s->tex->GetLevelCount();
    for (UINT l = 0; l < levels && w >= 1; ++l, w /= 2) {
        D3DLOCKED_RECT lr;
        if (FAILED(s->tex->LockRect(l, &lr, nullptr, 0))) return false;
        for (int y = 0; y < w; ++y) memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch, px + (size_t)y * w, (size_t)w * 4);
        s->tex->UnlockRect(l);
        px += (size_t)w * w;
    }
    return true;
}

struct Vtx2 { float x, y, z, rhw; DWORD c; float u, v, mu, mv; };

}  // namespace

bool tiles_draw(IDirect3DDevice9* dev, const hud::Frame& f, float units_per_m, const hud::Disc& disc, int map, const char* path, DWORD colour) {
    if (!start() || !make_mask(dev)) return false;
    ++g_frame;
    EnterCriticalSection(&g_cs);
    bool changed = g_want_map != map;
    if (changed) { g_want_map = map; strcpy_s(g_want_path, path); g_nq = 0; }
    LeaveCriticalSection(&g_cs);
    if (changed) SetEvent(g_ev);
    if (g_open_map != map) {
        if (g_open_failed == map && g_logged_open != -2 - map) { gp_log("minimap: %s is not a readable tile pack\n", path); g_logged_open = -2 - map; }
        return false;
    }
    const hud::PackHead& h = g_pack;
    if (g_logged_open != map) {
        g_logged_open = map; g_level_logged = -1;
        gp_log("minimap: tile pack %s open in %.1f ms on the background thread (%d levels, %.0f m per tile at level 0)\n", path, g_open_us / 1000.0, h.levels, h.lv[0].tile_m);
    }
    float px_per_m = units_per_m * disc.unit;
    int L = hud::pick_level(h, 1.0f / px_per_m);
    if (L != g_level_logged) { g_level_logged = L; gp_log("minimap: %.2f px / m on screen: tile level %d (%.2f m / texel)\n", px_per_m, L, h.texel * (float)(1 << L)); }
    float range = hud::DISC_R / units_per_m;
    hud::TileRef refs[POOL - 2];
    int n = hud::tiles_near(h, L, f.px, f.pz, range + MARGIN_M, refs, POOL - 2);
    // what this frame wants stays in the pool
    for (int i = 0; i < n; ++i) if (Slot* s = slot_of(make_key(map, L, refs[i].col, refs[i].row))) s->used = g_frame;
    // finished tiles go into textures
    int ups = 0;
    for (int i = 0; i < BUFS; ++i) {
        LONG st = g_buf[i].state;
        if (st == B_FAILED) {
            if (g_nbad < 16) g_bad[g_nbad++] = g_buf[i].key;
            gp_log("minimap: tile %08X could not be read\n", (unsigned)g_buf[i].key);
            InterlockedExchange(&g_buf[i].state, B_FREE);
            SetEvent(g_ev);
        } else if (st == B_READY && ups < UPLOADS_PER_FRAME) {
            if (Slot* s = take_slot(dev)) {
                LARGE_INTEGER q0, q1, fq; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&fq);
                s->key = -1;
                if (upload(s, g_buf[i].px)) { s->key = g_buf[i].key; s->used = g_frame; }
                QueryPerformanceCounter(&q1);
                double ms = (q1.QuadPart - q0.QuadPart) * 1000.0 / fq.QuadPart;
                g_up_sum += ms; if (ms > g_up_max) g_up_max = ms;
                ++g_up_n; ++ups;
            }
            InterlockedExchange(&g_buf[i].state, B_FREE);
            SetEvent(g_ev);
        }
    }
    // the rest is the worker's, nearest first
    {
        int q[POOL], nq = 0;
        for (int i = 0; i < n; ++i) {
            int key = make_key(map, L, refs[i].col, refs[i].row);
            const uint32_t* e = g_table[L] + ((size_t)refs[i].row * h.lv[L].cols + refs[i].col) * 2;
            bool skip = !e[0] || slot_of(key) != nullptr;
            for (int b = 0; b < BUFS && !skip; ++b) skip = g_buf[b].state != B_FREE && g_buf[b].key == key;
            for (int b = 0; b < g_nbad && !skip; ++b) skip = g_bad[b] == key;
            if (!skip) q[nq++] = key;
        }
        EnterCriticalSection(&g_cs);
        memcpy(g_queue, q, sizeof(int) * (size_t)nq); g_nq = nq;
        LeaveCriticalSection(&g_cs);
        if (nq) SetEvent(g_ev);
    }
    // the quads: one per visible tile, the same corners for neighbours, so nothing overlaps and nothing gaps
    Vtx2 v[6 * (POOL - 2)]; int nv = 0;
    const float bu = (float)h.border / (float)h.tile_px, ms = 0.49f / hud::DISC_R;
    IDirect3DTexture9* tex[POOL - 2]; int nt = 0;
    for (int i = 0; i < n; ++i) {
        if (refs[i].dist > range + 1.0f) continue;
        Slot* s = slot_of(make_key(map, L, refs[i].col, refs[i].row));
        if (!s) continue;
        hud::TileRect r = hud::tile_rect(h, L, refs[i].col, refs[i].row);
        const float cw[4][2] = {{r.x0, r.z1}, {r.x1, r.z1}, {r.x1, r.z0}, {r.x0, r.z0}};
        const float cu[4][2] = {{bu, bu}, {1 - bu, bu}, {1 - bu, 1 - bu}, {bu, 1 - bu}};
        Vtx2 q[4];
        for (int k = 0; k < 4; ++k) {
            hud::V2 u = hud::to_radar(f, cw[k][0] - f.px, cw[k][1] - f.pz, units_per_m);
            Vtx2 t = {disc.cx + u.x * disc.unit - 0.5f, disc.cy + u.y * disc.unit - 0.5f, 0, 1, colour, cu[k][0], cu[k][1], 0.5f + u.x * ms, 0.5f + u.y * ms};
            q[k] = t;
        }
        tex[nt++] = s->tex;
        v[nv++] = q[0]; v[nv++] = q[1]; v[nv++] = q[2]; v[nv++] = q[0]; v[nv++] = q[2]; v[nv++] = q[3];
    }
    if (!nt) return true;
    // stage 1 multiplies the alpha by the disc mask; everything set here beyond gp_ov_set2d is put back here
    struct TS { D3DTEXTURESTAGESTATETYPE s; DWORD set, old; } ts[] = {{D3DTSS_COLOROP, D3DTOP_SELECTARG2, 0}, {D3DTSS_COLORARG2, D3DTA_CURRENT, 0}, {D3DTSS_ALPHAOP, D3DTOP_MODULATE, 0},
        {D3DTSS_ALPHAARG1, D3DTA_TEXTURE, 0}, {D3DTSS_ALPHAARG2, D3DTA_CURRENT, 0}, {D3DTSS_TEXCOORDINDEX, 1, 0}, {D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE, 0}};
    struct SS { D3DSAMPLERSTATETYPE s; DWORD set, old; } ss[] = {{D3DSAMP_MINFILTER, D3DTEXF_LINEAR, 0}, {D3DSAMP_MAGFILTER, D3DTEXF_LINEAR, 0}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE, 0},
        {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP, 0}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP, 0}, {D3DSAMP_SRGBTEXTURE, FALSE, 0}};
    DWORD s2c = 0, s2a = 0, bias = 0;
    IDirect3DBaseTexture9* t1 = nullptr; dev->GetTexture(1, &t1);
    for (TS& e : ts) { dev->GetTextureStageState(1, e.s, &e.old); dev->SetTextureStageState(1, e.s, e.set); }
    for (SS& e : ss) { dev->GetSamplerState(1, e.s, &e.old); dev->SetSamplerState(1, e.s, e.set); }
    dev->GetTextureStageState(2, D3DTSS_COLOROP, &s2c); dev->GetTextureStageState(2, D3DTSS_ALPHAOP, &s2a);
    dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->GetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, &bias);
    float lod = -0.25f; dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, *(DWORD*)&lod);
    dev->SetTexture(1, g_mask);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2);
    for (int i = 0; i < nt; ++i) {
        dev->SetTexture(0, tex[i]);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v + (size_t)i * 6, sizeof(Vtx2));
    }
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, bias);
    dev->SetTextureStageState(2, D3DTSS_COLOROP, s2c); dev->SetTextureStageState(2, D3DTSS_ALPHAOP, s2a);
    for (SS& e : ss) dev->SetSamplerState(1, e.s, e.old);
    for (TS& e : ts) dev->SetTextureStageState(1, e.s, e.old);
    dev->SetTexture(1, t1); if (t1) t1->Release();
    return true;
}

void tiles_underlay(IDirect3DDevice9* dev, const hud::Disc& disc, DWORD colour) {
    if (!make_mask(dev)) return;
    struct V { float x, y, z, w; DWORD c; float u, v; };
    const float R = hud::DISC_R * disc.unit * 1.03f, x0 = disc.cx - R - 0.5f, y0 = disc.cy - R - 0.5f, x1 = disc.cx + R - 0.5f, y1 = disc.cy + R - 0.5f;
    DWORD c = hud::paper_under(colour);
    V p[4] = {{x0, y0, 0, 1, c, 0.01f, 0.01f}, {x1, y0, 0, 1, c, 0.99f, 0.01f}, {x0, y1, 0, 1, c, 0.01f, 0.99f}, {x1, y1, 0, 1, c, 0.99f, 0.99f}};
    dev->SetTexture(0, g_mask);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, p, sizeof(V));
}

void tiles_device_changed() {
    for (int i = 0; i < POOL; ++i) g_slot[i].tex = nullptr;
    g_mask = nullptr; g_used_slots = 0;
}

void tiles_log_perf(double seconds) {
    if (!g_thread) return;
    LONG n = InterlockedExchange(&g_tiles_read, 0), us = InterlockedExchange(&g_read_us_sum, 0), mx = InterlockedExchange(&g_read_us_max, 0), b = InterlockedExchange(&g_bytes_read, 0);
    MEMORYSTATUSEX m; m.dwLength = sizeof m; GlobalMemoryStatusEx(&m);
    double used = (double)(m.ullTotalVirtual - m.ullAvailVirtual) / (1024.0 * 1024.0), total = (double)m.ullTotalVirtual / (1024.0 * 1024.0);
    double mb = CHAIN * 4.0 / (1024.0 * 1024.0);
    gp_log("minimap perf tiles over %.0f s: %ld read in the background (avg %.1f ms, max %.1f ms, %.1f MB), %d uploads on the render thread (avg %.2f ms, max %.2f ms), pool %d / %d textures %.1f MB + buffers %.1f MB, address space %.0f of %.0f MB\n",
           seconds, n, n ? us / 1000.0 / n : 0.0, mx / 1000.0, b / (1024.0 * 1024.0), g_up_n, g_up_n ? g_up_sum / g_up_n : 0.0, g_up_max, g_used_slots, POOL,
           g_used_slots * mb, BUFS * mb, used, total);
    g_up_sum = g_up_max = 0.0; g_up_n = 0;
}
