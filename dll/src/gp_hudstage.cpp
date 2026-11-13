// gp_hudstage.cpp - shared HUD drawing stages of TODGameplay.dll.
//
// 1. Sprite hooks: the shared overlay hooks of dll/src/gp_overlay.cpp (MT_SPRITE_VT, MT_EXEC_A / _B, patched once for
//    every client by gp_overlay_install()). The RADAR stage is one sprite client there: match = Radar/Radar_BG (2: its
//    submitted list is remembered), post = the tint (written right after the sprite's render, so the quad the game
//    rebuilds next frame carries it), draw = the stage's draw right after the radar's list ran. [mayhem_title] and
//    [minimap] are clients of the same layer.
// 2. RADAR stage: the in-game radar is one Sprite, HUDAuxCtrl.fragment Radar/Radar_BG (texture radar.BMP, virtual
//    HUD, layer 0; HUD_radar keeps it in its RadarBackGround member +28h), drawn as one quad (op 32h one colour /
//    33h four colours) in its own render list. The stage
//    * tints it: the 4 corner colours (Sprite +70h..+7Ch, RGBA bytes, R lowest) = the colours the sprite had x the
//      product of every client's tint, written right before the sprite's render, with the colour setter's own side
//      effects (+14h bit 3 = rebuild the quad, +9Ch bit 10 = all corners equal) and WITHOUT the member-write notify
//      (rewind / saves never see it). Identity tint = the sprite's own colours back.
//    * calls the clients' draw callbacks right after the radar's list ran (above the disc, under the blips, under
//      every later HUD layer and the menus), with the quad the game built this frame (pixels + UVs), its opacity and
//      the device in a plain 2D state (read back and restored around the callbacks). Nothing is drawn on a frame the
//      game does not draw the radar (cut-scenes, menus, HUD off).
//    API: gp_glue.h "RADAR stage".
#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include "gp_glue.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WR32(a, v) (*(volatile DWORD*)(uintptr_t)(a) = (DWORD)(v))

static const DWORD A_RENDERER = 0xA39F14;   // renderer; +0 = IDirect3DDevice9* (REF MT_DEVICE), +328h / +32Ch back buffer (REF DS_SCREEN)

// ================================================================================================ 2. RADAR stage
static volatile DWORD g_radar = 0;                 // Radar/Radar_BG (cached by name; overdose.main never unloads)
static int g_client = -1;                          // overlay sprite client id (its pending list = the radar list this frame)
static bool g_stage = false, g_stage_dead = false;
static DWORD g_fault_addr = 0, g_fault_code = 0;
static int fault_note(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode; g_fault_addr = (DWORD)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
static bool name_is(DWORD node, const char* want) {
    DWORD p = RD32(node + 0x4C);                      // Node +4Ch name (REF PD_NODE_NAME)
    if (!p) return false;
    const char* s = (const char*)(uintptr_t)p;
    for (int i = 0;; ++i) { if (s[i] != want[i]) return false; if (!want[i]) return true; }
}

// ---- tint
struct Tint { float r, g, b, a; const char* who; };
static Tint g_tint[GP_RADAR_TINT_SLOTS];
static DWORD g_own[4], g_written[4]; static bool g_own_ok = false, g_tinted = false;
void gp_radar_set_tint(int slot, float r, float g, float b, float a, const char* who) {
    if (slot < 0 || slot >= GP_RADAR_TINT_SLOTS) return;
    g_tint[slot] = Tint{r, g, b, a, who};
}
static BYTE mulb(DWORD c, int sh, float k) {
    float v = (float)((c >> sh) & 255) * k + 0.5f;
    return (BYTE)(v < 0.0f ? 0 : (v > 255.0f ? 255 : (int)v));
}
static void apply_tint(DWORD spr) {
    float r = 1, g = 1, b = 1, a = 1;
    for (const Tint& t : g_tint) if (t.who) { r *= t.r; g *= t.g; b *= t.b; a *= t.a; }
    DWORD cur[4]; for (int i = 0; i < 4; ++i) cur[i] = RD32(spr + 0x70 + 4 * i);
    if (!g_own_ok) { memcpy(g_own, cur, sizeof g_own); memcpy(g_written, cur, sizeof g_written); g_own_ok = true; }
    else if (memcmp(cur, g_written, sizeof cur) != 0) {   // the game (or a data edit) changed them: its colours from now on
        memcpy(g_own, cur, sizeof g_own);
        gp_log("radar stage: Radar_BG colours changed by the game (%08X): new base\n", cur[0]);
    }
    bool identity = r == 1 && g == 1 && b == 1 && a == 1;
    DWORD want[4];
    for (int i = 0; i < 4; ++i) {
        DWORD c = g_own[i];
        want[i] = identity ? c : (DWORD)mulb(c, 0, r) | (DWORD)mulb(c, 8, g) << 8 | (DWORD)mulb(c, 16, b) << 16 | (DWORD)mulb(c, 24, a) << 24;
    }
    if (memcmp(want, cur, sizeof want) != 0) {
        for (int i = 0; i < 4; ++i) WR32(spr + 0x70 + 4 * i, want[i]);   // colour 1..4 (REF HS_SPR_COLSET / HS_SPR_COLRD)
        bool same = want[0] == want[1] && want[0] == want[2] && want[0] == want[3];
        DWORD f = RD32(spr + 0x9C); WR32(spr + 0x9C, same ? (f | 0x400) : (f & ~0x400u));   // one-colour quad (op 32h)
        *(volatile BYTE*)(uintptr_t)(spr + 0x14) |= 8;                   // rebuild the quad this frame (REF HS_SPR_DIRTY)
    }
    memcpy(g_written, want, sizeof want);
    g_tinted = !identity;
}

// ---- finding the radar, remembering its list
static void stage_post(DWORD self) {               // overlay post: right after the radar sprite's render
    if (self != g_radar || g_stage_dead) return;
    __try { apply_tint(self); } __except (fault_note(GetExceptionInformation())) {
        g_stage_dead = true; gp_log("radar stage: fault %08X at %08X while tinting: stage OFF\n", g_fault_code, g_fault_addr);
    }
}
static int stage_match(DWORD self) {               // overlay match: 2 = the radar (post, then draw after its list)
    if (g_stage_dead) return 0;
    if (self != g_radar) {
        if (g_radar) return 0;                         // found already
        DWORD p = RD32(self + 0x4C);
        if (!p || *(const char*)(uintptr_t)p != 'R' || !name_is(self, "Radar_BG")) return 0;
        DWORD parent = RD32(self + 0x40);              // Node +40h parent (REF PH_NODE_PARENT)
        if (!parent || !name_is(parent, "Radar")) return 0;
        if (!(RD32(self + 0x9C) & 0x40)) return 0;     // the HUD one uses the virtual HUD (the TextureDict copy does not)
        g_radar = self;
        gp_log("radar stage: Radar/Radar_BG found at %08X (colours %08X, flags %08X)\n", self, RD32(self + 0x70), RD32(self + 0x9C));
    }
    return 2;
}

// ---- drawing clients
struct Drawer { GpRadarDrawFn fn; int order; const char* who; };
static Drawer g_dr[6]; static int g_ndr = 0;
bool gp_radar_add_draw(GpRadarDrawFn fn, int order, const char* who) {
    for (int i = 0; i < g_ndr; ++i) if (g_dr[i].fn == fn) return true;
    if (g_ndr >= 6) return false;
    int k = g_ndr++;
    while (k > 0 && g_dr[k - 1].order > order) { g_dr[k] = g_dr[k - 1]; --k; }
    g_dr[k] = Drawer{fn, order, who};
    return true;
}
static GpRadarFrame g_last; static bool g_last_ok = false; static DWORD g_last_frame_no = 0;
bool gp_radar_last_frame(GpRadarFrame* out) { if (!g_last_ok) return false; *out = g_last; return true; }

static bool is_com(void* p) {
    if (!p) return false;
    DWORD vt = RD32((DWORD)(uintptr_t)p); DWORD fn = vt ? RD32(vt) : 0; HMODULE m = nullptr;
    return fn && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)fn, &m);
}
// the quad the sprite render 0x8F4C40 put in its list this frame: op 32h / 33h, texture, 4 points, 4 UVs; the index
// of the first point is kept in +9Ch bits 25..31 (REF MT_SPR_IDX, MT_QUAD, MT_SPR_UV)
static bool radar_quad(DWORD spr, DWORD list, float P[4][2], float UV[4][2]) {
    DWORD buf = RD32(list + 0x1C), cnt = RD32(list + 0x20);
    if (!buf || cnt > 4096) return false;
    DWORD idx = RD32(spr + 0x9C) >> 25;
    if (idx < 2 || idx + 16 > cnt) return false;
    DWORD op = RD32(buf + (idx - 2) * 4);
    if (op != 0x32 && op != 0x33) return false;
    for (int i = 0; i < 4; ++i) {
        P[i][0] = RDF(buf + (idx + 2 * i) * 4); P[i][1] = RDF(buf + (idx + 2 * i + 1) * 4);
        UV[i][0] = RDF(buf + (idx + 8 + 2 * i) * 4); UV[i][1] = RDF(buf + (idx + 9 + 2 * i) * 4);
        if (!(P[i][0] > -1e5f && P[i][0] < 1e5f && P[i][1] > -1e5f && P[i][1] < 1e5f && UV[i][0] > -16 && UV[i][0] < 16 && UV[i][1] > -16 && UV[i][1] < 16)) return false;
    }
    return true;
}

// device state: what the clients may touch is read before and written back after (no state block: those would have
// to be released before a Reset)
struct Saved {
    DWORD rs[20]; DWORD ts0[8], ts1[2]; DWORD ss[6];
    IDirect3DBaseTexture9* tex; DWORD fvf; IDirect3DVertexShader9* vs; IDirect3DPixelShader9* ps; IDirect3DVertexDeclaration9* decl;
    IDirect3DVertexBuffer9* vb; UINT vb_off, vb_stride;
};
static const D3DRENDERSTATETYPE RS[20] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
    D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_LIGHTING, D3DRS_STENCILENABLE, D3DRS_COLORWRITEENABLE,
    D3DRS_SCISSORTESTENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE,
    D3DRS_SPECULARENABLE, D3DRS_TEXTUREFACTOR, D3DRS_CLIPPLANEENABLE};
static const DWORD RS_SET[20] = {FALSE, FALSE, FALSE, TRUE, D3DBLEND_ONE, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, D3DCULL_NONE, FALSE, FALSE, FALSE,
    0xF, FALSE, FALSE, FALSE, D3DFILL_SOLID, D3DSHADE_GOURAUD, FALSE, 0xFFFFFFFF, 0};
static const D3DTEXTURESTAGESTATETYPE TS0[8] = {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
    D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS};
static const DWORD TS0_SET[8] = {D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, 0, D3DTTFF_DISABLE};
static const D3DSAMPLERSTATETYPE SS[6] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE};
static const DWORD SS_SET[6] = {D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE};
static void save_state(IDirect3DDevice9* d, Saved* s) {
    for (int i = 0; i < 20; ++i) d->GetRenderState(RS[i], &s->rs[i]);
    for (int i = 0; i < 8; ++i) d->GetTextureStageState(0, TS0[i], &s->ts0[i]);
    d->GetTextureStageState(1, D3DTSS_COLOROP, &s->ts1[0]); d->GetTextureStageState(1, D3DTSS_ALPHAOP, &s->ts1[1]);
    for (int i = 0; i < 6; ++i) d->GetSamplerState(0, SS[i], &s->ss[i]);
    s->tex = nullptr; d->GetTexture(0, &s->tex);
    s->fvf = 0; d->GetFVF(&s->fvf);
    s->vs = nullptr; d->GetVertexShader(&s->vs); s->ps = nullptr; d->GetPixelShader(&s->ps);
    s->decl = nullptr; d->GetVertexDeclaration(&s->decl);
    s->vb = nullptr; s->vb_off = s->vb_stride = 0; d->GetStreamSource(0, &s->vb, &s->vb_off, &s->vb_stride);
}
static void set_state(IDirect3DDevice9* d) {
    for (int i = 0; i < 20; ++i) d->SetRenderState(RS[i], RS_SET[i]);
    for (int i = 0; i < 8; ++i) d->SetTextureStageState(0, TS0[i], TS0_SET[i]);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (int i = 0; i < 6; ++i) d->SetSamplerState(0, SS[i], SS_SET[i]);
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
}
static void restore_state(IDirect3DDevice9* d, Saved* s) {
    for (int i = 0; i < 20; ++i) d->SetRenderState(RS[i], s->rs[i]);
    for (int i = 0; i < 8; ++i) d->SetTextureStageState(0, TS0[i], s->ts0[i]);
    d->SetTextureStageState(1, D3DTSS_COLOROP, s->ts1[0]); d->SetTextureStageState(1, D3DTSS_ALPHAOP, s->ts1[1]);
    for (int i = 0; i < 6; ++i) d->SetSamplerState(0, SS[i], s->ss[i]);
    d->SetTexture(0, s->tex); if (s->tex) s->tex->Release();
    d->SetVertexShader(s->vs); if (s->vs) s->vs->Release();
    d->SetPixelShader(s->ps); if (s->ps) s->ps->Release();
    if (s->decl) { d->SetVertexDeclaration(s->decl); s->decl->Release(); } else d->SetFVF(s->fvf);
    if (s->decl && s->fvf) d->SetFVF(s->fvf);
    d->SetStreamSource(0, s->vb, s->vb_off, s->vb_stride); if (s->vb) s->vb->Release();
}
static bool g_logged_quad = false, g_logged_pure = false;
static void radar_draw(DWORD spr, DWORD list) {
    float P[4][2], UV[4][2];
    if (!radar_quad(spr, list, P, UV)) {
        if (!g_logged_quad) { gp_log("radar stage: the radar's quad was not found in its list (+9Ch %08X): nothing drawn\n", RD32(spr + 0x9C)); g_logged_quad = true; }
        return;
    }
    DWORD ren = RD32(A_RENDERER);
    IDirect3DDevice9* dev = ren ? (IDirect3DDevice9*)(uintptr_t)RD32(ren) : nullptr;
    if (!is_com(dev)) return;
    GpRadarFrame f;
    f.sprite = spr; memcpy(f.P, P, sizeof P); memcpy(f.UV, UV, sizeof UV);
    f.opacity = (float)(RD32(spr + 0x98) & 255) / 255.0f;     // Sprite +98h opacity (REF HS_SPR_OPACITY)
    f.W = (float)(int)RD32(ren + 0x328); f.H = (float)(int)RD32(ren + 0x32C);
    f.dev = dev; f.now = gp_now(); f.frame = ++g_last_frame_no;
    if (!g_logged_quad) {
        gp_log("radar stage: radar quad (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) uv (%.2f,%.2f)-(%.2f,%.2f), opacity %.2f, back buffer %.0fx%.0f\n",
               P[0][0], P[0][1], P[1][0], P[1][1], P[2][0], P[2][1], P[3][0], P[3][1], UV[0][0], UV[0][1], UV[2][0], UV[2][1], f.opacity, f.W, f.H);
        g_logged_quad = true;
    }
    g_last = f; g_last_ok = true;
    if (!g_ndr) return;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(dev->GetCreationParameters(&cp)) || (cp.BehaviorFlags & D3DCREATE_PUREDEVICE)) {
        if (!g_logged_pure) { gp_log("radar stage: pure device (no state read-back): nothing drawn\n"); g_logged_pure = true; }
        return;
    }
    Saved sv; save_state(dev, &sv); set_state(dev);
    for (int i = 0; i < g_ndr; ++i) g_dr[i].fn(f);
    restore_state(dev, &sv);
}
static void stage_draw(DWORD spr, DWORD list) {    // overlay draw: right after the radar's submitted list ran
    if (g_stage_dead || spr != g_radar) return;
    __try { radar_draw(spr, list); }
    __except (fault_note(GetExceptionInformation())) {
        g_stage_dead = true;
        gp_log("radar stage: fault %08X at %08X while drawing: stage OFF for this session\n", g_fault_code, g_fault_addr);
    }
}
bool gp_radar_stage_install(const char* who) {
    if (g_stage) return true;
    static const char* refs[] = {"MT_DEVICE", "MT_SPR_IDX", "MT_QUAD", "MT_SPR_UV", "DS_SCREEN", "PD_NODE_NAME", "PH_NODE_PARENT",
                                 "HS_SPR_COLRD", "HS_SPR_COLSET", "HS_SPR_DIRTY", "HS_SPR_OPACITY"};
    for (const char* r : refs) if (!gp_ref_ok(r)) { gp_log("radar stage (%s): reference %s differs: not installed\n", who, r); return false; }
    if (!gp_site_ok("MT_SPRITE_VT") || !gp_site_ok("MT_EXEC_A") || !gp_site_ok("MT_EXEC_B")) {
        gp_log("radar stage (%s): a sprite / list-execute site holds other bytes (another mod?): not installed\n", who); return false;
    }
    g_client = gp_overlay_add_sprite(&stage_match, &stage_post, &stage_draw, "radar stage");
    if (g_client < 0) { gp_log("radar stage (%s): no overlay client slot left: not installed\n", who); return false; }
    g_stage = true;
    gp_log("radar stage: on (asked by %s)\n", who);
    return true;
}
bool gp_radar_stage_on() { return g_stage && !g_stage_dead; }
DWORD gp_radar_sprite() { return g_radar; }

// ---- test host only
void* gp_hudstage_test_stub(const char* name) {
    if (!strcmp(name, "sprite_wrap")) return gp_overlay_test_stub("ov_sprite_wrap");
    if (!strcmp(name, "exec_wrap")) return gp_overlay_test_stub("ov_exec_wrap");
    if (!strcmp(name, "sprite_orig")) return gp_overlay_test_stub("ov_sprite_orig");
    if (!strcmp(name, "exec_orig")) return gp_overlay_test_stub("ov_exec_orig");
    if (!strcmp(name, "radar_node")) return (void*)&g_radar;
    if (!strcmp(name, "radar_pending")) return (void*)gp_overlay_pending(g_client);
    return nullptr;
}
