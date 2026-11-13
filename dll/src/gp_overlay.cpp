// gp_overlay.cpp - the shared 2D overlay hooks of TODGameplay.dll. Several features draw their own pictures through
// the game's Direct3D device at a well defined moment of the game's own 2D drawing: [mayhem_title] (right after the
// title picture), [minimap] (right after the radar background, under the blips; at the end of the frame for the world
// markers). One set of patches serves them all, so two features never patch the same bytes.
//
//   MT_SPRITE_VT  the Sprite class's per-frame render method (vtable 0x9D2F50 slot +20h = 0x8F6CC0) -> SpriteWrap: the
//                 original, then every client whose match() wants this sprite: post() (may edit the list it just
//                 built) and, when the list was submitted this frame (+14h bit 20h), the list is remembered.
//   MT_EXEC_A/_B  the two calls of the list executor 0x434290 in the layer renderer 0x421530 -> ExecWrap: the original,
//                 then, when that list is one a client remembered, the client's draw(). Everything the game draws
//                 later in the frame stays on top of it.
//   OV_FRAME      the call of the layer renderer 0x421530 in GfxInternal::Render 0x421B30 (every layer of the frame)
//                 -> FrameWrap: the original, then every frame client (drawn on top of the whole frame).
// Each client's work runs inside __try: a fault turns that client off for the session (logged), never the game.
// The patches are made only when a client registered (the feature is on); with every client off nothing is patched.
#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include "gp_glue.h"

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))

static DWORD R_MT_SPRITE = 0x8F6CC0;     // MT_SPRITE_VT: the original vtable entry (Sprite per-frame render, thiscall, ret)
static DWORD R_MT_EXEC = 0x434290;       // MT_EXEC_A / _B original call target (render list execute, thiscall, ret)
static DWORD R_OV_FRAME = 0x421530;      // OV_FRAME original call target (layer renderer, thiscall(first, last, flag), ret 0Ch)
static const DWORD A_RENDERER = 0xA39F14;   // renderer; +0 = IDirect3DDevice9* (REF MT_DEVICE)

struct SpriteClient { GpSpriteMatch match; GpSpritePost post; GpSpriteDraw draw, pre; const char* who; volatile DWORD pending, pending_sprite; bool dead; };
struct FrameClient { GpFrameDraw draw; const char* who; bool dead; };
struct ExecClient { GpExecDraw draw; const char* who; bool dead; };
static ExecClient g_ec[8]; static int g_nec = 0;
static SpriteClient g_sc[8]; static int g_nsc = 0;
static FrameClient g_fc[8]; static int g_nfc = 0;
static bool g_sprite_patched = false, g_frame_patched = false;
static DWORD g_fault_addr = 0, g_fault_code = 0;
static int fault_note(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode; g_fault_addr = (DWORD)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

int gp_overlay_add_sprite(GpSpriteMatch match, GpSpritePost post, GpSpriteDraw draw, const char* who) {
    if (!match || g_nsc >= 8) return -1;
    SpriteClient& c = g_sc[g_nsc]; c.match = match; c.post = post; c.draw = draw; c.pre = nullptr; c.who = who; c.pending = 0; c.pending_sprite = 0; c.dead = false;
    return g_nsc++;
}
bool gp_overlay_set_pre(int client, GpSpriteDraw pre) {
    if (client < 0 || client >= g_nsc) return false;
    g_sc[client].pre = pre;
    return true;
}
bool gp_overlay_add_exec(GpExecDraw draw, const char* who) {   // after EVERY list the layer renderer executes (keep it cheap)
    if (!draw || g_nec >= 8) return false;
    g_ec[g_nec].draw = draw; g_ec[g_nec].who = who; g_ec[g_nec].dead = false; ++g_nec;
    return true;
}
bool gp_overlay_add_frame(GpFrameDraw draw, const char* who) {
    if (!draw || g_nfc >= 8) return false;
    g_fc[g_nfc].draw = draw; g_fc[g_nfc].who = who; g_fc[g_nfc].dead = false; ++g_nfc;
    return true;
}
volatile DWORD* gp_overlay_pending(int id) { return id >= 0 && id < g_nsc ? &g_sc[id].pending : nullptr; }

IDirect3DDevice9* gp_overlay_device() {
    __try {
        DWORD ren = RD32(A_RENDERER);
        IDirect3DDevice9* d = ren ? (IDirect3DDevice9*)(uintptr_t)RD32(ren) : nullptr;
        if (!d) return nullptr;
        DWORD vt = RD32((DWORD)(uintptr_t)d); DWORD fn = vt ? RD32(vt) : 0; HMODULE m = nullptr;   // a COM object of a loaded module
        if (!fn || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)fn, &m)) return nullptr;
        return d;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---------------------------------------------------------------- hooks
typedef DWORD(__fastcall* Thiscall0_t)(DWORD self, void* edx);
typedef DWORD(__fastcall* Thiscall3_t)(DWORD self, void* edx, DWORD a, DWORD b, DWORD c);

static void sprite_client(SpriteClient& c, DWORD self) {
    __try {
        int m = c.match(self);
        if (!m) return;
        if (c.post) c.post(self);
        if (m >= 2 && c.draw) {
            DWORD list = RD32(self + 0x94);               // the sprite's own render list (REF MT_SPR_LIST)
            if (list && (RD32(list + 0x14) & 0x20)) { c.pending = list; c.pending_sprite = self; }   // submitted (REF MT_SUBMIT)
        }
    } __except (fault_note(GetExceptionInformation())) {
        c.dead = true; gp_log("overlay: fault %08X at %08X in %s (sprite): it is OFF for this session\n", g_fault_code, g_fault_addr, c.who);
    }
}
static DWORD __fastcall SpriteWrap(DWORD self, void* edx) {
    DWORD r = ((Thiscall0_t)(uintptr_t)R_MT_SPRITE)(self, edx);
    for (int i = 0; i < g_nsc; ++i) if (!g_sc[i].dead) sprite_client(g_sc[i], self);
    return r;
}
static void draw_client(SpriteClient& c, DWORD sprite, DWORD list) {
    __try { c.draw(sprite, list); }
    __except (fault_note(GetExceptionInformation())) {
        c.dead = true; gp_log("overlay: fault %08X at %08X in %s (draw): it is OFF for this session\n", g_fault_code, g_fault_addr, c.who);
    }
}
static void pre_client(SpriteClient& c, DWORD sprite, DWORD list) {
    __try { c.pre(sprite, list); }
    __except (fault_note(GetExceptionInformation())) {
        c.dead = true; gp_log("overlay: fault %08X at %08X in %s (pre): it is OFF for this session\n", g_fault_code, g_fault_addr, c.who);
    }
}
static void exec_client(ExecClient& c, DWORD list) {
    __try { c.draw(list); }
    __except (fault_note(GetExceptionInformation())) {
        c.dead = true; gp_log("overlay: fault %08X at %08X in %s (exec): it is OFF for this session\n", g_fault_code, g_fault_addr, c.who);
    }
}
static DWORD __fastcall ExecWrap(DWORD list, void* edx) {
    if (list)
        for (int i = 0; i < g_nsc; ++i) {
            SpriteClient& c = g_sc[i];
            if (c.pre && c.pending == list && !c.dead) pre_client(c, c.pending_sprite, list);
        }
    DWORD r = ((Thiscall0_t)(uintptr_t)R_MT_EXEC)(list, edx);
    if (list)
        for (int i = 0; i < g_nec; ++i) if (!g_ec[i].dead) exec_client(g_ec[i], list);
    if (list)
        for (int i = 0; i < g_nsc; ++i) {
            SpriteClient& c = g_sc[i];
            if (c.pending == list && !c.dead) { DWORD sp = c.pending_sprite; c.pending = 0; draw_client(c, sp, list); }
        }
    return r;
}
static void frame_client(FrameClient& c) {
    __try { c.draw(); }
    __except (fault_note(GetExceptionInformation())) {
        c.dead = true; gp_log("overlay: fault %08X at %08X in %s (frame): it is OFF for this session\n", g_fault_code, g_fault_addr, c.who);
    }
}
static DWORD __fastcall FrameWrap(DWORD views, void* edx, DWORD first, DWORD last, DWORD flag) {
    DWORD r = ((Thiscall3_t)(uintptr_t)R_OV_FRAME)(views, edx, first, last, flag);
    for (int i = 0; i < g_nfc; ++i) if (!g_fc[i].dead) frame_client(g_fc[i]);
    return r;
}

int gp_overlay_install() {
    int n = 0;
    if ((g_nsc || g_nec) && !g_sprite_patched) {
        static const char* refs[] = {"MT_DEVICE", "MT_EXEC_CLR", "MT_SUBMIT", "MT_SPR_LIST", "PD_NODE_NAME", "PH_NODE_PARENT"};
        bool ok = true;
        for (const char* r : refs) if (!gp_ref_ok(r)) { gp_log("overlay: reference %s differs: sprite overlays OFF\n", r); ok = false; break; }
        if (ok && (!gp_site_ok("MT_SPRITE_VT") || !gp_site_ok("MT_EXEC_A") || !gp_site_ok("MT_EXEC_B"))) {
            gp_log("overlay: a sprite / list-execute site holds other bytes (another mod?): sprite overlays OFF\n"); ok = false;
        }
        if (ok) {
            n += gp_patch_call("MT_EXEC_A", (void*)&ExecWrap) ? 1 : 0;
            n += gp_patch_call("MT_EXEC_B", (void*)&ExecWrap) ? 1 : 0;
            DWORD p = (DWORD)(uintptr_t)&SpriteWrap;
            n += gp_patch_bytes("MT_SPRITE_VT", &p, 4) ? 1 : 0;   // last: nothing is pending before both executors are hooked
            g_sprite_patched = true;
            for (int i = 0; i < g_nsc; ++i) gp_log("overlay: sprite client %s\n", g_sc[i].who);
            for (int i = 0; i < g_nec; ++i) gp_log("overlay: list client %s\n", g_ec[i].who);
        } else {
            for (int i = 0; i < g_nsc; ++i) g_sc[i].dead = true;
            for (int i = 0; i < g_nec; ++i) g_ec[i].dead = true;
        }
    }
    if (g_nfc && !g_frame_patched) {
        if (gp_ref_ok("MT_DEVICE") && gp_site_ok("OV_FRAME") && gp_patch_call("OV_FRAME", (void*)&FrameWrap)) {
            ++n; g_frame_patched = true;
            for (int i = 0; i < g_nfc; ++i) gp_log("overlay: frame client %s\n", g_fc[i].who);
        } else {
            gp_log("overlay: the frame site holds other bytes: end-of-frame overlays OFF\n");
            for (int i = 0; i < g_nfc; ++i) g_fc[i].dead = true;
        }
    }
    return n;
}
bool gp_overlay_sprite_ok() { return g_sprite_patched; }
bool gp_overlay_frame_ok() { return g_frame_patched; }

void* gp_overlay_test_stub(const char* name) {
    if (!strcmp(name, "ov_sprite_wrap") || !strcmp(name, "mt_sprite_wrap")) return (void*)&SpriteWrap;
    if (!strcmp(name, "ov_exec_wrap") || !strcmp(name, "mt_exec_wrap")) return (void*)&ExecWrap;
    if (!strcmp(name, "ov_frame_wrap")) return (void*)&FrameWrap;
    if (!strcmp(name, "ov_sprite_orig") || !strcmp(name, "mt_sprite_orig")) return (void*)&R_MT_SPRITE;
    if (!strcmp(name, "ov_exec_orig") || !strcmp(name, "mt_exec_orig")) return (void*)&R_MT_EXEC;
    if (!strcmp(name, "ov_frame_orig")) return (void*)&R_OV_FRAME;
    return nullptr;
}

// ---------------------------------------------------------------- device state for 2D overlays (read, set, write back)
static const D3DRENDERSTATETYPE RS[20] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
    D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_LIGHTING, D3DRS_STENCILENABLE, D3DRS_COLORWRITEENABLE,
    D3DRS_SCISSORTESTENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE,
    D3DRS_SPECULARENABLE, D3DRS_TEXTUREFACTOR, D3DRS_CLIPPLANEENABLE};
static const DWORD RS_SET[20] = {FALSE, FALSE, FALSE, TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, D3DCULL_NONE, FALSE, FALSE, FALSE,
    0xF, FALSE, FALSE, FALSE, D3DFILL_SOLID, D3DSHADE_GOURAUD, FALSE, 0xFFFFFFFF, 0};
static const D3DTEXTURESTAGESTATETYPE TS0[8] = {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
    D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS};
static const DWORD TS0_SET[8] = {D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, 0, D3DTTFF_DISABLE};
static const D3DSAMPLERSTATETYPE SS[6] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE};
static const DWORD SS_SET[6] = {D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE};
bool gp_ov_save(IDirect3DDevice9* d, GpOvState* s) {
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(d->GetCreationParameters(&cp)) || (cp.BehaviorFlags & D3DCREATE_PUREDEVICE)) return false;   // no read-back
    for (int i = 0; i < 20; ++i) d->GetRenderState(RS[i], &s->rs[i]);
    for (int i = 0; i < 8; ++i) d->GetTextureStageState(0, TS0[i], &s->ts0[i]);
    d->GetTextureStageState(1, D3DTSS_COLOROP, &s->ts1[0]); d->GetTextureStageState(1, D3DTSS_ALPHAOP, &s->ts1[1]);
    for (int i = 0; i < 6; ++i) d->GetSamplerState(0, SS[i], &s->ss[i]);
    s->tex = nullptr; d->GetTexture(0, &s->tex);
    s->fvf = 0; d->GetFVF(&s->fvf);
    s->vs = nullptr; d->GetVertexShader(&s->vs); s->ps = nullptr; d->GetPixelShader(&s->ps);
    s->decl = nullptr; d->GetVertexDeclaration(&s->decl);
    s->vb = nullptr; s->vb_off = s->vb_stride = 0; d->GetStreamSource(0, &s->vb, &s->vb_off, &s->vb_stride);
    return true;
}
void gp_ov_set2d(IDirect3DDevice9* d) {
    for (int i = 0; i < 20; ++i) d->SetRenderState(RS[i], RS_SET[i]);
    for (int i = 0; i < 8; ++i) d->SetTextureStageState(0, TS0[i], TS0_SET[i]);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (int i = 0; i < 6; ++i) d->SetSamplerState(0, SS[i], SS_SET[i]);
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
}
void gp_ov_restore(IDirect3DDevice9* d, GpOvState* s) {
    for (int i = 0; i < 20; ++i) d->SetRenderState(RS[i], s->rs[i]);
    for (int i = 0; i < 8; ++i) d->SetTextureStageState(0, TS0[i], s->ts0[i]);
    d->SetTextureStageState(1, D3DTSS_COLOROP, s->ts1[0]); d->SetTextureStageState(1, D3DTSS_ALPHAOP, s->ts1[1]);
    for (int i = 0; i < 6; ++i) d->SetSamplerState(0, SS[i], s->ss[i]);
    d->SetTexture(0, s->tex); if (s->tex) s->tex->Release();
    d->SetVertexShader(s->vs); if (s->vs) s->vs->Release();
    d->SetPixelShader(s->ps); if (s->ps) s->ps->Release();
    if (s->decl) { d->SetVertexDeclaration(s->decl); s->decl->Release(); } else d->SetFVF(s->fvf);
    if (s->decl && s->fvf) d->SetFVF(s->fvf);         // an FVF set after a declaration wins in D3D9: keep the game's choice
    d->SetStreamSource(0, s->vb, s->vb_off, s->vb_stride); if (s->vb) s->vb->Release();
}
