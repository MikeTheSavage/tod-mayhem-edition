// gp_hud.cpp - [minimap] of TODGameplay.dll: the game's own round radar (bottom left) becomes a minimap and the
// mission objectives get markers, in the original HUD's look.
//  * Map: a top-down map of the level (baked from the player's own game files by hud/tools/bake_minimap.py into
//    TODGameplay_minimap\, never shipped; tiles streamed by gp_hud_tiles.cpp) is drawn inside the radar's parchment disc, right after the game drew the radar
//    background and before it draws the blips (overlay client of the Radar_BG sprite, dll/src/gp_overlay.cpp). It turns
//    and scrolls with the same frame the game uses for its blips (HUD_radar PlayerPos / PlayerView), or north up
//    (rotate = 0: the game's blips, the north blip and the background are turned into the north frame in their own lists).
//  * Objectives on the radar: the game already shows the mission's blips (Trigger_Activate_Radar: waypoints, objective
//    enemies / allies / objects, story missions) and clamps far ones to the rim; an objective beyond the radar's range
//    gets an arrow just outside the rim pointing at it (objective_marker).
//  * Objectives in the world: each objective blip's target (its world position read every frame) is projected through
//    the game's camera (view 0 of the renderer's views: the matrix the frame was drawn with) and gets a small diamond
//    with the distance; off screen or behind the camera it becomes an arrow at the screen edge (world_marker).
//  * Enemy dots: the game's own (kept as they are).
// Nothing here writes game memory except the radar's range (zoom != 1: RadarRadius / one_over / Factor, no member-write
// notify, never in a save) and, north up, the points of the radar sprites' own render lists of the frame.
#include <windows.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <objidl.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "gp_glue.h"
#include "gp_hud_logic.h"
#include "gp_hud_draw.h"
#include "gp_hud_tiles.h"
using namespace hudd;

#define RD32(a) (*(volatile DWORD*)(uintptr_t)(a))
#define RDF(a)  (*(volatile float*)(uintptr_t)(a))
#define WRF(a, v) (*(volatile float*)(uintptr_t)(a) = (v))

static DWORD P_HR_GETPOS = 0x88CA80;            // Node::GetWorldPosition thiscall(float out[3]), ret 4 (REF F_GETPOS)
static const DWORD H_RADAR = 0x6F0C80;          // HUD_radar class handler (REF HR_CLASS)
static const DWORD A_RENDERER = 0xA39F14, A_VIEWS = 0xA35E04, A_WIDE = 0xA39F12, A_LIST_BLIP = 0xA3BB40;
// HUD_radar script data (REF rows HR_*)
static const DWORD R_RADIUS = 0x0C, R_ONEOVER = 0x10, R_FACTOR = 0x1C, R_BLIPS = 0x34, R_POS = 0x68, R_POSY = 0x74, R_VIEW = 0x78;

// ---------------------------------------------------------------- config
struct HudCfg {
    bool enabled = false, rotate = true, objective_marker = true, world_marker = true, map = true, log = true;
    float zoom = 1.0f, map_opacity = 0.9f, marker_scale = 1.0f; int markers = 3;
    float perf_seconds = 30.0f;
    char dir[MAX_PATH] = "";
};
static HudCfg g_hc;
static hud::MapInfo g_maps[64]; static hud::Occ g_occ[64]; static int g_nmaps = 0;
static char g_gamedir[MAX_PATH] = "";
static void load_index() {
    g_nmaps = 0;
    char p[MAX_PATH + 64]; sprintf_s(p, "%s\\minimap.txt", g_hc.dir);
    FILE* f = nullptr; if (fopen_s(&f, p, "rb") || !f) { gp_log("minimap: no %s: the radar stays a radar (objective markers still work)\n", p); return; }
    char line[512];
    while (fgets(line, sizeof line, f) && g_nmaps < 64) if (hud::parse_map_line(line, &g_maps[g_nmaps])) ++g_nmaps;
    fclose(f);
    int nocc = 0;
    for (int i = 0; i < g_nmaps; ++i) {   // <name>.occ: where each map has ground (the city maps' rectangles overlap)
        char q[MAX_PATH + 80]; sprintf_s(q, "%s\\%s.occ", g_hc.dir, g_maps[i].name);
        FILE* o = nullptr; if (fopen_s(&o, q, "rb") || !o) continue;
        fseek(o, 0, SEEK_END); long n = ftell(o); fseek(o, 0, SEEK_SET);
        if (n > 0 && n < (1 << 22)) {
            char* t = new char[n + 1]; size_t got = fread(t, 1, n, o); t[got] = 0;
            if (hud::parse_occ(t, &g_occ[i])) ++nocc;
            delete[] t;
        }
        fclose(o);
    }
    gp_log("minimap: %d baked map(s) in %s (%d with ground cells)\n", g_nmaps, g_hc.dir, nocc);
}
void hud_load_config() {
    const gp::Ini& ini = gp_ini();
    HudCfg c;
    c.enabled = ini.geti("minimap", "enabled", 0) != 0;
    c.rotate = ini.geti("minimap", "rotate", 1) != 0;
    c.objective_marker = ini.geti("minimap", "objective_marker", 1) != 0;
    c.world_marker = ini.geti("minimap", "world_marker", 1) != 0;
    c.map = ini.geti("minimap", "map", 1) != 0;
    c.log = ini.geti("minimap", "log", 1) != 0;
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    c.zoom = clampf(ini.getf("minimap", "zoom", 1.0f), 0.25f, 4.0f);
    c.map_opacity = clampf(ini.getf("minimap", "map_opacity", 0.9f), 0.0f, 1.0f);
    c.marker_scale = clampf(ini.getf("minimap", "marker_scale", 1.0f), 0.5f, 2.5f);
    c.markers = ini.geti("minimap", "world_markers_max", 3); if (c.markers < 1) c.markers = 1; if (c.markers > 8) c.markers = 8;
    c.perf_seconds = ini.getf("minimap", "perf_log_seconds", 30.0f);
    GetModuleFileNameA(nullptr, g_gamedir, MAX_PATH); { char* s = strrchr(g_gamedir, '\\'); if (s) *s = 0; }
    const char* d = ini.get("minimap", "map_dir");
    if (d && *d) { if (d[1] == ':' || d[0] == '\\') strcpy_s(c.dir, d); else sprintf_s(c.dir, "%s\\%s", g_gamedir, d); }
    else sprintf_s(c.dir, "%s\\TODGameplay_minimap", g_gamedir);
    g_hc = c;
    if (c.enabled && c.map) load_index();
}
bool hud_enabled() { return g_hc.enabled; }

// ---------------------------------------------------------------- game reads
static int g_fault_line = 0;
static DWORD cls_handler(DWORD node) {   // script class object (node -> +4 type -> +28h) -> +60h handler
    __try { if (!node) return 0; DWORD t = RD32(node + 4); DWORD s = t ? RD32(t + 0x28) : 0; return s ? RD32(s + 0x60) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool name_is(DWORD node, const char* want) {
    __try {
        DWORD p = RD32(node + 0x4C); if (!p) return false;                  // Node +4Ch name (REF PD_NODE_NAME)
        const char* s = (const char*)(uintptr_t)p;
        for (int i = 0;; ++i) { if (s[i] != want[i]) return false; if (!want[i]) return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
typedef void(__fastcall* GetPos_t)(DWORD node, void* edx, float* out);
static bool world_pos(DWORD node, hud::V3* v) {
    if (!node) return false;
    float p[3] = {0, 0, 0};
    __try { ((GetPos_t)(uintptr_t)P_HR_GETPOS)(node, nullptr, p); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!(fabsf(p[0]) < 1e5f && fabsf(p[1]) < 1e5f && fabsf(p[2]) < 1e5f)) return false;
    v->x = p[0]; v->y = p[1]; v->z = p[2];
    return true;
}

// ---------------------------------------------------------------- state
static volatile DWORD g_bg = 0;            // Radar/Radar_BG sprite node
static volatile DWORD g_radar = 0;         // the HUD_radar node (the background's parent), class checked
static int g_client = -1;
static bool g_dead = false;
struct Blip { int type; DWORD target; hud::V3 pos; bool ok; };
static Blip g_blips[24]; static volatile int g_nblips = 0;
static hud::V3 g_player = {0, 0, 0}; static bool g_player_ok = false;
static double g_snap_t = 0.0;
// the disc of the last drawn radar frame (overlay / other features)
static hud::Disc g_disc = {0, 0, 0, 0, false};
static DWORD g_frame_no = 0, g_radar_frame = 0xFFFFFFFF;
static float g_radar_alpha = 1.0f;
static DWORD g_tint = 0xFFFFFFFF;
static bool g_logged_zoom = false, g_logged_cam = false, g_logged_disc = false;
static int g_cam_mode = 0;                 // 1 view +18h is camera-to-world, 2 it is world-to-camera
static float g_units_per_m = hud::BLIP_R / hud::GAME_RADIUS_M;

bool hud_radar_rect(float* cx, float* cy, float* r, float* unit) {
    if (!g_disc.ok || g_radar_frame + 1 < g_frame_no) return false;
    *cx = g_disc.cx; *cy = g_disc.cy; *r = g_disc.r; *unit = g_disc.unit; return true;
}
void hud_set_map_tint(DWORD argb) { g_tint = argb; }

static DWORD radar_data() {
    DWORD n = g_radar; if (!n) return 0;
    __try { return RD32(n + 0x18); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// main loop, every pass: the radar's blips and their targets' positions (engine calls here, not while drawing)
void hud_post_input(double now) {
    if (!g_hc.enabled || g_dead) return;
    DWORD d = radar_data(); if (!d) { g_nblips = 0; return; }
    int n = 0;
    __try {
        // zoom: the radar's range (the game sets 50 m at its init; the blips follow the same three members)
        if (g_hc.zoom != 1.0f) {
            float want = hud::GAME_RADIUS_M / g_hc.zoom;
            if (fabsf(RDF(d + R_RADIUS) - want) > 1e-3f) {
                float before = RDF(d + R_RADIUS);
                WRF(d + R_RADIUS, want); WRF(d + R_ONEOVER, 1.0f / want); WRF(d + R_FACTOR, hud::BLIP_R / want);
                if (!g_logged_zoom) { gp_log("minimap: radar range %.1f m -> %.1f m (zoom %.2f)\n", before, want, g_hc.zoom); g_logged_zoom = true; }
            }
        }
        float r = RDF(d + R_RADIUS);
        g_units_per_m = (r > 1.0f && r < 1000.0f) ? hud::BLIP_R / r : hud::BLIP_R / hud::GAME_RADIUS_M;
        g_player.x = RDF(d + R_POS); g_player.z = RDF(d + R_POS + 8); g_player.y = RDF(d + R_POSY);
        g_player_ok = fabsf(g_player.x) < 1e5f && fabsf(g_player.z) < 1e5f;
        // RadarBlips: list(s_RadarBlip) -> +4 array -> +4 buffer of struct pointers; count = +8 / element size (REF HR_BLIPS)
        DWORD L = RD32(d + R_BLIPS), lt = RD32(A_LIST_BLIP);
        DWORD es = lt ? RD32(lt + 0x24) : 0;
        if (L && es && es < 16) {
            DWORD cnt = RD32(L + 8) / es; DWORD a = RD32(L + 4); DWORD buf = a ? RD32(a + 4) : 0;
            for (DWORD i = 0; buf && i < cnt && i < 64 && n < 24; ++i) {
                DWORD e = RD32(buf + es * i * 4); if (!e) continue;
                DWORD tgt = RD32(e + 8); if (!tgt) continue;           // a free blip
                Blip& b = g_blips[n];
                b.type = (int)RD32(e + 0xC); b.target = tgt;
                b.ok = world_pos(tgt, &b.pos);
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    g_nblips = n; g_snap_t = now;
    // the blip set changed: one log line (types, targets, distances), at most 120 per session
    static DWORD sig_last = 0; static int nlog = 0;
    DWORD sig = 2166136261u;
    for (int i = 0; i < n; ++i) { sig = (sig ^ (DWORD)g_blips[i].type) * 16777619u; sig = (sig ^ g_blips[i].target) * 16777619u; }
    if (sig != sig_last && g_hc.log && nlog < 120) {
        sig_last = sig; ++nlog;
        char line[900]; int k = sprintf_s(line, "minimap: %d blip(s) at player (%.1f %.1f %.1f):", n, g_player.x, g_player.y, g_player.z);
        for (int i = 0; i < n && k < 820; ++i) {
            const Blip& b = g_blips[i];
            float dx = b.pos.x - g_player.x, dz = b.pos.z - g_player.z;
            k += sprintf_s(line + k, sizeof line - k, " [type %d%s %08X %.0f m]", b.type, hud::is_objective(b.type) ? " obj" : "", b.target, b.ok ? sqrtf(dx * dx + dz * dz) : -1.0f);
        }
        gp_log("%s\n", line);
    }
}

// ---------------------------------------------------------------- textures (marker shapes + digits; maps)
static IDirect3DDevice9* g_dev = nullptr;
static bool g_setup_work = false;                // this frame built a texture / loaded a map (not counted in the cost)
static IDirect3DTexture9* g_tex_ui = nullptr;      // 512 x 128: shape cells (64 x 64) in row 0, glyphs in row 1
static IDirect3DTexture9* g_tex_map = nullptr; static int g_map_loaded = -1; static bool g_map_failed[64];
static const int UI_W = 1024, UI_H = 128;
struct Glyph { float u0, v0, u1, v1, w, h; };      // texels
static Glyph g_glyph[128];
static float g_glyph_h = 0.0f;
static ULONG_PTR g_gdip = 0;
static bool gdip() {
    if (g_gdip) return true;
    Gdiplus::GdiplusStartupInput in;
    return Gdiplus::GdiplusStartup(&g_gdip, &in, nullptr) == Gdiplus::Ok;
}
static IDirect3DTexture9* make_tex(int w, int h, int levels) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(g_dev->CreateTexture(w, h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    return t;
}
// premultiplied-safe box mip chain from a straight-alpha BGRA image (alpha-weighted colour)
static bool upload_mips(IDirect3DTexture9* t, uint32_t* img, int w, int h) {
    UINT levels = t->GetLevelCount();
    for (UINT l = 0; l < levels; ++l) {
        D3DLOCKED_RECT lr;
        if (FAILED(t->LockRect(l, &lr, nullptr, 0))) return false;
        for (int y = 0; y < h; ++y) memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch, img + (size_t)y * w, (size_t)w * 4);
        t->UnlockRect(l);
        if (w == 1 && h == 1) break;
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                uint32_t s[4] = {img[(2 * y) * w + 2 * x], img[(2 * y) * w + (w > 1 ? 2 * x + 1 : 2 * x)],
                                 img[(h > 1 ? 2 * y + 1 : 2 * y) * w + 2 * x], img[(h > 1 ? 2 * y + 1 : 2 * y) * w + (w > 1 ? 2 * x + 1 : 2 * x)]};
                uint32_t A = 0, R = 0, G = 0, B = 0;
                for (int k = 0; k < 4; ++k) { uint32_t a = s[k] >> 24; A += a; R += ((s[k] >> 16) & 255) * a; G += ((s[k] >> 8) & 255) * a; B += (s[k] & 255) * a; }
                uint32_t o = 0;
                if (A) o = ((A + 2) / 4) << 24 | (R / A) << 16 | (G / A) << 8 | (B / A);
                img[y * nw + x] = o;
            }
        w = nw; h = nh;
    }
    return true;
}
static void build_ui_image(uint32_t* img) {
    using namespace Gdiplus;
    Bitmap bmp(UI_W, UI_H, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        SolidBrush white(Color(255, 255, 255, 255));
        // shapes in 64 x 64 cells, drawn white (tinted when drawn): outline = the shape grown by 5 px
        auto cell = [](int i) { return PointF((REAL)(i * 64), 0.0f); };
        PointF c0 = cell(C_DIAMOND_O), c1 = cell(C_DIAMOND_F), c2 = cell(C_ARROW_O), c3 = cell(C_ARROW_F);
        PointF dO[4] = {{c0.X + 32, 2}, {c0.X + 62, 32}, {c0.X + 32, 62}, {c0.X + 2, 32}};
        PointF dF[4] = {{c1.X + 32, 8}, {c1.X + 56, 32}, {c1.X + 32, 56}, {c1.X + 8, 32}};
        g.FillPolygon(&white, dO, 4); g.FillPolygon(&white, dF, 4);
        // arrow pointing right (+x), centred in its cell
        PointF aO[3] = {{c2.X + 60, 32}, {c2.X + 10, 4}, {c2.X + 10, 60}};
        PointF aF[3] = {{c3.X + 52, 32}, {c3.X + 16, 12}, {c3.X + 16, 52}};
        g.FillPolygon(&white, aO, 3); g.FillPolygon(&white, aF, 3);
        // soft glow
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                float dx = (x + 0.5f - 32) / 32, dy = (y + 0.5f - 32) / 32, d2 = dx * dx + dy * dy;
                float a = d2 < 1 ? (1 - d2) * (1 - d2) : 0.0f;
                bmp.SetPixel(C_GLOW * 64 + x, y, Color((BYTE)(a * 255), 255, 255, 255));
            }
        // glyphs: digits, '.', 'k', 'm', ' ', '%', 'x' and the infinity sign, white with a dark outline baked in (alpha only
        // carries the shape; the outline is drawn dark through the colour channels)
        const wchar_t* chars = L"0123456789.km x/~";
        FontFamily* fam = nullptr;
        const wchar_t* names[] = {L"Impact", L"Arial Black", L"Arial"};
        for (const wchar_t* nm : names) { fam = new FontFamily(nm); if (fam->GetLastStatus() == Ok) break; delete fam; fam = nullptr; }
        if (fam) {
            GraphicsPath path; StringFormat sf(StringFormat::GenericTypographic());
            float x = 2.0f, size = 40.0f;
            for (int i = 0; chars[i]; ++i) {
                wchar_t s[2] = {chars[i] == L'~' ? (wchar_t)0x221E : chars[i], 0};
                path.Reset();
                if (chars[i] == L'~') { FontFamily ar(L"Arial"); path.AddString(s, 1, &ar, FontStyleBold, size * 1.5f, PointF(x + 4, 64.0f - 6.0f), &sf); }
                else path.AddString(s, 1, fam, FontStyleRegular, size, PointF(x + 4, 64.0f + 6.0f), &sf);
                RectF b; path.GetBounds(&b);
                float adv = chars[i] == L' ' ? size * 0.28f : b.Width + 2.0f;
                if (chars[i] == L' ') b = RectF(x + 4, 64.0f + 6.0f, adv, size);
                Pen op(Color(255, 30, 20, 12), 7.0f); op.SetLineJoin(LineJoinRound);
                g.DrawPath(&op, &path);
                SolidBrush fill(Color(255, 255, 255, 255));
                g.FillPath(&fill, &path);
                Glyph& gl = g_glyph[(int)chars[i]];
                gl.u0 = x; gl.v0 = 64.0f; gl.u1 = x + adv + 8.0f; gl.v1 = 128.0f; gl.w = adv + 8.0f; gl.h = 64.0f;
                x += adv + 10.0f;
                if (x > UI_W - 48) break;
            }
            g_glyph_h = 64.0f;
            delete fam;
        }
    }
    Rect rc(0, 0, UI_W, UI_H); BitmapData bd;
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
        for (int y = 0; y < UI_H; ++y) memcpy(img + (size_t)y * UI_W, (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride, UI_W * 4);
        bmp.UnlockBits(&bd);
    }
}
bool hudd::ensure_ui(IDirect3DDevice9* dev) {
    if (dev == g_dev && g_tex_ui) return true;
    if (g_dev && dev != g_dev) { gp_log("minimap: the device changed: new textures\n"); g_tex_ui = nullptr; g_tex_map = nullptr; g_map_loaded = -1; tiles_device_changed(); }
    g_dev = dev; g_setup_work = true;
    if (!gdip()) { gp_log("minimap: GDI+ start failed: markers OFF\n"); return false; }
    static uint32_t img[UI_W * UI_H];
    memset(img, 0, sizeof img);
    build_ui_image(img);
    g_tex_ui = make_tex(UI_W, UI_H, 0);
    if (!g_tex_ui || !upload_mips(g_tex_ui, img, UI_W, UI_H)) { gp_log("minimap: UI texture failed\n"); if (g_tex_ui) g_tex_ui->Release(); g_tex_ui = nullptr; return false; }
    gp_log("minimap: marker / digit texture ready on device %p (glyph height %.0f)\n", dev, g_glyph_h);
    return true;
}
static bool load_map(int i) {
    if (i == g_map_loaded) return g_tex_map != nullptr;
    if (i < 0 || i >= g_nmaps || g_map_failed[i]) return false;
    if (g_tex_map) { g_tex_map->Release(); g_tex_map = nullptr; }
    g_map_loaded = i; g_setup_work = true;
    double t0 = gp_now();
    char p[MAX_PATH + 80]; sprintf_s(p, "%s\\%s", g_hc.dir, g_maps[i].file);
    wchar_t wp[MAX_PATH + 80]; MultiByteToWideChar(CP_ACP, 0, p, -1, wp, MAX_PATH + 80);
    bool ok = false;
    if (gdip()) {
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(wp);
        if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
            int w = (int)bmp->GetWidth(), h = (int)bmp->GetHeight();
            Gdiplus::Rect rc(0, 0, w, h); Gdiplus::BitmapData bd;
            if (w > 0 && h > 0 && w <= 8192 && h <= 8192 && bmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                uint32_t* img = new uint32_t[(size_t)w * h];
                for (int y = 0; y < h; ++y) memcpy(img + (size_t)y * w, (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride, (size_t)w * 4);
                bmp->UnlockBits(&bd);
                g_tex_map = make_tex(w, h, 0);
                ok = g_tex_map && upload_mips(g_tex_map, img, w, h);
                delete[] img;
            }
        }
        delete bmp;
    }
    if (!ok) { g_map_failed[i] = true; if (g_tex_map) g_tex_map->Release(); g_tex_map = nullptr; }
    gp_log("minimap: map %s (%s) %s in %.0f ms\n", g_maps[i].name, g_maps[i].file, ok ? "loaded" : "NOT loaded", (gp_now() - t0) * 1000.0);
    return ok;
}

// ---------------------------------------------------------------- drawing helpers
struct Vtx { float x, y, z, rhw; DWORD c; float u, v; };
DWORD hudd::argb_mul(DWORD c, float a) {
    float A = ((c >> 24) & 255) * a; int ai = (int)(A + 0.5f); ai = ai < 0 ? 0 : (ai > 255 ? 255 : ai);
    return (DWORD)ai << 24 | (c & 0xFFFFFF);
}
static DWORD rgb_mul(DWORD c, DWORD t) {
    DWORD o = 0;
    for (int s = 0; s < 32; s += 8) o |= (((c >> s) & 255) * ((t >> s) & 255) / 255) << s;
    return o;
}
// a cell of the UI texture, centred at (x, y), size s pixels, rotated by ang
void hudd::cell_quad(IDirect3DDevice9* d, int cell, float x, float y, float s, float ang, DWORD col) {
    float c = cosf(ang) * s * 0.5f, sn = sinf(ang) * s * 0.5f;
    float u0 = (cell * 64 + 0.5f) / UI_W, u1 = (cell * 64 + 63.5f) / UI_W, v0 = 0.5f / UI_H, v1 = 63.5f / UI_H;
    Vtx v[4] = {{x - c + sn - 0.5f, y - sn - c - 0.5f, 0, 1, col, u0, v0}, {x + c + sn - 0.5f, y + sn - c - 0.5f, 0, 1, col, u1, v0},
                {x - c - sn - 0.5f, y - sn + c - 0.5f, 0, 1, col, u0, v1}, {x + c - sn - 0.5f, y + sn + c - 0.5f, 0, 1, col, u1, v1}};
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}
float hudd::text_width(const char* s, float h) {
    float w = 0.0f, k = h / (g_glyph_h > 0 ? g_glyph_h : 64.0f);
    for (; *s; ++s) { const Glyph& g = g_glyph[(unsigned char)*s & 127]; w += (g.w > 0 ? g.w - 8.0f : h * 0.3f) * k; }
    return w;
}
void hudd::text(IDirect3DDevice9* d, const char* s, float x, float y, float h, DWORD col) {   // x = left, y = top
    float k = h / (g_glyph_h > 0 ? g_glyph_h : 64.0f);
    static Vtx v[64 * 6]; int n = 0;
    for (; *s && n < 64 * 6; ++s) {
        const Glyph& g = g_glyph[(unsigned char)*s & 127];
        if (g.w <= 0) { x += h * 0.3f; continue; }
        float w = g.w * k;
        float u0 = g.u0 / UI_W, u1 = g.u1 / UI_W, v0 = g.v0 / UI_H, v1 = g.v1 / UI_H;
        Vtx q[4] = {{x - 0.5f, y - 0.5f, 0, 1, col, u0, v0}, {x + w - 0.5f, y - 0.5f, 0, 1, col, u1, v0},
                    {x - 0.5f, y + h - 0.5f, 0, 1, col, u0, v1}, {x + w - 0.5f, y + h - 0.5f, 0, 1, col, u1, v1}};
        v[n++] = q[0]; v[n++] = q[1]; v[n++] = q[2]; v[n++] = q[2]; v[n++] = q[1]; v[n++] = q[3];
        x += (g.w - 8.0f) * k;
    }
    if (n) d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, v, sizeof(Vtx));
}

// ---------------------------------------------------------------- the radar background: find it, read its quad
static bool quad_of(DWORD sprite, DWORD list, float P[4][2], float UV[4][2]) {
    __try {
        DWORD buf = RD32(list + 0x1C), cnt = RD32(list + 0x20);   // command buffer, count (REF MT_QUAD)
        if (!buf || cnt > 8192) return false;
        DWORD idx = RD32(sprite + 0x9C) >> 25;                   // point index of the sprite's quad (REF MT_SPR_IDX)
        if (idx < 2 || idx + 16 > cnt) return false;
        DWORD op = RD32(buf + (idx - 2) * 4);
        if (op != 0x32 && op != 0x33) return false;
        for (int i = 0; i < 4; ++i) {
            P[i][0] = RDF(buf + (idx + 2 * i) * 4); P[i][1] = RDF(buf + (idx + 2 * i + 1) * 4);
            UV[i][0] = RDF(buf + (idx + 8 + 2 * i) * 4); UV[i][1] = RDF(buf + (idx + 9 + 2 * i) * 4);
            if (!(fabsf(P[i][0]) < 1e5f && fabsf(P[i][1]) < 1e5f)) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool write_points(DWORD sprite, DWORD list, const float P[4][2]) {
    __try {
        DWORD buf = RD32(list + 0x1C), cnt = RD32(list + 0x20);
        DWORD idx = RD32(sprite + 0x9C) >> 25;
        if (!buf || idx < 2 || idx + 16 > cnt) return false;
        DWORD op = RD32(buf + (idx - 2) * 4); if (op != 0x32 && op != 0x33) return false;
        for (int i = 0; i < 4; ++i) { WRF(buf + (idx + 2 * i) * 4, P[i][0]); WRF(buf + (idx + 2 * i + 1) * 4, P[i][1]); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static hud::Frame view_frame(DWORD d) {   // the frame the game used for its blips this frame
    hud::Frame f;
    __try { f = hud::frame_from_view(RDF(d + R_VIEW), RDF(d + R_VIEW + 8), RDF(d + R_POS), RDF(d + R_POS + 8)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return f;
}

// north up: the game's radar lists (background, blips, north blip) are turned into the north frame. A list the game did
// not rewrite this frame still holds our points: those are recognised and the original points are used again.
struct Moved { DWORD sprite; float orig[4][2], wrote[4][2]; };
static Moved g_moved[24]; static int g_nmoved = 0;
static void north_up(DWORD sprite, bool rotate_corners) {
    if (g_hc.rotate || !g_disc.ok) return;
    DWORD list = RD32(sprite + 0x94); if (!list) return;
    float P[4][2], UV[4][2];
    if (!quad_of(sprite, list, P, UV)) return;
    Moved* m = nullptr;
    for (int i = 0; i < g_nmoved; ++i) if (g_moved[i].sprite == sprite) { m = &g_moved[i]; break; }
    if (!m) { if (g_nmoved >= 24) return; m = &g_moved[g_nmoved++]; m->sprite = sprite; memset(m->wrote, 0, sizeof m->wrote); }
    if (memcmp(P, m->wrote, sizeof P)) memcpy(m->orig, P, sizeof P);   // the game wrote new points
    DWORD d = radar_data(); if (!d) return;
    hud::Frame f = view_frame(d);
    float Q[4][2];
    if (rotate_corners) {
        for (int i = 0; i < 4; ++i) {
            hud::V2 r = hud::view_to_north(f, m->orig[i][0] - g_disc.cx, m->orig[i][1] - g_disc.cy);
            Q[i][0] = g_disc.cx + r.x; Q[i][1] = g_disc.cy + r.y;
        }
    } else {
        float cx = 0, cy = 0; for (int i = 0; i < 4; ++i) { cx += m->orig[i][0] * 0.25f; cy += m->orig[i][1] * 0.25f; }
        hud::V2 r = hud::view_to_north(f, cx - g_disc.cx, cy - g_disc.cy);
        float tx = g_disc.cx + r.x - cx, ty = g_disc.cy + r.y - cy;
        for (int i = 0; i < 4; ++i) { Q[i][0] = m->orig[i][0] + tx; Q[i][1] = m->orig[i][1] + ty; }
    }
    if (write_points(sprite, list, Q)) memcpy(m->wrote, Q, sizeof Q);
}

// overlay match: the background (draw after it), its blips and the north blip (north up only)
static int match_sprite(DWORD self) {
    if (g_dead) return 0;
    if (self == g_bg) return 2;
    DWORD p = RD32(self + 0x4C); if (!p) return 0;
    char c0 = *(const char*)(uintptr_t)p;
    if (c0 != 'R') return 0;                                          // Radar_BG, RadarBlip_00.., RadarBlip_North
    DWORD parent = RD32(self + 0x40);                                 // Node +40h parent (REF PH_NODE_PARENT)
    if (name_is(self, "Radar_BG")) {
        if (!parent || !name_is(parent, "Radar") || cls_handler(parent) != H_RADAR) return 0;
        g_bg = self; g_radar = parent;
        gp_log("minimap: radar background %08X, HUD_radar node %08X (data %08X)\n", self, parent, radar_data());
        return 2;
    }
    if (g_hc.rotate) return 0;
    if (parent && name_is(parent, "BlipPool")) return 1;
    if (parent == g_radar && name_is(self, "RadarBlip_North")) return 1;
    return 0;
}
static void post_sprite(DWORD self) {
    if (g_hc.rotate) return;
    north_up(self, self == g_bg);
}

// ---------------------------------------------------------------- the minimap (right after the radar background)
static int g_map_drawn = -1;
static bool g_under = false; static double g_pre_ms = 0.0;
static double g_pf_last = 0.0, g_pf_sum = 0.0, g_pf_max = 0.0, g_pf_sum2 = 0.0, g_pf_max2 = 0.0; static int g_pf_n = 0, g_pf_n2 = 0;
static void draw_disc_map(IDirect3DDevice9* dev, const hud::Frame& f, float op) {
    g_under = false;
    if (!g_hc.map || g_nmaps == 0 || g_hc.map_opacity <= 0.0f) return;
    int mi = hud::pick_map_occ(g_maps, g_occ, g_nmaps, f.px, f.pz);
    if (mi < 0) return;
    const hud::MapInfo& m = g_maps[mi];
    g_map_drawn = mi;
    if (m.pack) {                                                     // the bake's tile pack
        char p[MAX_PATH + 80]; sprintf_s(p, "%s\\%s", g_hc.dir, m.file);
        g_under = tiles_draw(dev, f, g_units_per_m, g_disc, mi, p, argb_mul(rgb_mul(0xFFFFFFFF, g_tint), g_hc.map_opacity * op));
        return;
    }
    if (!load_map(mi)) return;                                        // the one picture of an older bake, until the next bake
    const int SEG = 56;
    static Vtx v[SEG * 6 * 2 + 8];
    int n = 0;
    float k = g_units_per_m;                                          // radar units per metre
    DWORD tint = rgb_mul(0xFFFFFFFF, g_tint);
    DWORD cin = argb_mul(tint, g_hc.map_opacity * op), cout = argb_mul(tint, 0.0f);
    auto vert = [&](float rr, float a, DWORD col) {
        float ux = cosf(a) * rr, uy = sinf(a) * rr;                    // radar units from the centre (y down)
        hud::V2 w = hud::from_radar(f, ux, uy, k);
        hud::V2 uv = hud::map_uv(m, f.px + w.x, f.pz + w.y);
        Vtx t = {g_disc.cx + ux * g_disc.unit - 0.5f, g_disc.cy + uy * g_disc.unit - 0.5f, 0, 1, col, uv.x, uv.y};
        return t;
    };
    const float R1 = hud::DISC_R * 0.86f, R2 = hud::DISC_R;
    Vtx c = vert(0.0f, 0.0f, cin);
    for (int i = 0; i < SEG; ++i) {
        float a0 = 6.2831853f * i / SEG, a1 = 6.2831853f * (i + 1) / SEG;
        Vtx p0 = vert(R1, a0, cin), p1 = vert(R1, a1, cin), q0 = vert(R2, a0, cout), q1 = vert(R2, a1, cout);
        v[n++] = c; v[n++] = p0; v[n++] = p1;
        v[n++] = p0; v[n++] = q0; v[n++] = q1; v[n++] = p0; v[n++] = q1; v[n++] = p1;
    }
    dev->SetTexture(0, g_tex_map);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
    dev->SetSamplerState(0, D3DSAMP_BORDERCOLOR, 0);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, v, sizeof(Vtx));
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}
static void draw_rim_arrows(IDirect3DDevice9* dev, const hud::Frame& f, float op) {
    if (!g_hc.objective_marker || !g_player_ok) return;
    float range = hud::BLIP_R / g_units_per_m;                       // metres to the rim
    int n = g_nblips;
    dev->SetTexture(0, g_tex_ui);
    for (int i = 0; i < n; ++i) {
        const Blip& b = g_blips[i];
        if (!b.ok || !hud::is_objective(b.type)) continue;
        float dx = b.pos.x - f.px, dz = b.pos.z - f.pz;
        if (sqrtf(dx * dx + dz * dz) <= range * 1.02f) continue;    // on the map: the game's blip says it all
        hud::V2 u = hud::to_radar(f, dx, dz, 1.0f);
        float l = sqrtf(u.x * u.x + u.y * u.y); if (l < 1e-4f) continue;
        float ax = u.x / l, ay = u.y / l, ang = atan2f(ay, ax);
        float r = (hud::DISC_R + 15.0f) * g_disc.unit, s = 12.0f * g_disc.unit;
        float x = g_disc.cx + ax * r, y = g_disc.cy + ay * r;
        cell_quad(dev, C_ARROW_O, x, y, s, ang, argb_mul(0xFF1E140C, op * 0.9f));
        cell_quad(dev, C_ARROW_F, x, y, s, ang, argb_mul(hud::objective_colour(b.type), op));
    }
}
// right before the radar background's list: the paper behind its parchment. Behind it, so the disc's rim and pointer stay
// the game's. The disc is last frame's (it only moves with the resolution); the map turns it on from its second frame.
static void pre_radar(DWORD, DWORD) {
    if (g_dead || !g_under || !g_disc.ok) return;
    IDirect3DDevice9* dev = gp_overlay_device(); if (!dev) return;
    LARGE_INTEGER q0, q1, fq; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&fq);
    GpOvState sv; if (!gp_ov_save(dev, &sv)) return;
    gp_ov_set2d(dev);
    tiles_underlay(dev, g_disc, argb_mul(rgb_mul(0xFFFFFFFF, g_tint), g_hc.map_opacity * g_radar_alpha));
    gp_ov_restore(dev, &sv);
    QueryPerformanceCounter(&q1);
    g_pre_ms = (q1.QuadPart - q0.QuadPart) * 1000.0 / fq.QuadPart;
}

static void draw_radar(DWORD sprite, DWORD list) {
    if (g_dead) return;
    LARGE_INTEGER q0, q1, fq; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&fq);
    float P[4][2], UV[4][2];
    if (!quad_of(sprite, list, P, UV)) return;
    if (!g_hc.rotate) {   // north up: the quad holds our turned corners; the disc is where the game put the sprite
        for (int i = 0; i < g_nmoved; ++i) if (g_moved[i].sprite == sprite && !memcmp(P, g_moved[i].wrote, sizeof P)) { memcpy(P, g_moved[i].orig, sizeof P); break; }
    }
    hud::Disc dc = hud::disc_from_quad(P, UV);
    if (!dc.ok) return;
    g_disc = dc; g_radar_frame = g_frame_no;
    if (!g_logged_disc) {
        gp_log("minimap: radar disc centre (%.1f, %.1f) radius %.1f px, %.3f px per unit, quad (%.0f,%.0f)-(%.0f,%.0f)\n", dc.cx, dc.cy, dc.r, dc.unit, P[0][0], P[0][1], P[2][0], P[2][1]);
        g_logged_disc = true;
    }
    float op = 1.0f; __try { op = (RD32(sprite + 0x98) & 255) / 255.0f; } __except (EXCEPTION_EXECUTE_HANDLER) {}   // the radar's own fade
    g_radar_alpha = op;
    if (op <= 0.01f) return;
    DWORD d = radar_data(); if (!d) return;
    IDirect3DDevice9* dev = gp_overlay_device(); if (!dev || !ensure_ui(dev)) return;
    hud::Frame vf = view_frame(d);
    hud::Frame f = g_hc.rotate ? vf : hud::frame_north(vf.px, vf.pz);
    GpOvState sv; if (!gp_ov_save(dev, &sv)) { gp_log("minimap: pure device (no state read-back): drawing OFF\n"); g_dead = true; return; }
    gp_ov_set2d(dev);
    draw_disc_map(dev, f, op);
    draw_rim_arrows(dev, f, op);
    gp_ov_restore(dev, &sv);
    QueryPerformanceCounter(&q1);
    double ms = (q1.QuadPart - q0.QuadPart) * 1000.0 / fq.QuadPart + g_pre_ms;
    g_pre_ms = 0.0;
    if (!g_setup_work) { g_pf_sum += ms; if (ms > g_pf_max) g_pf_max = ms; ++g_pf_n; }
    else gp_log("minimap: first frame with setup %.1f ms (textures / map picture)\n", ms);
    g_setup_work = false;
}

// ---------------------------------------------------------------- objective markers in the world (end of the frame)
static bool read_cam(hud::Cam* c) {
    __try {
        DWORD views = RD32(A_VIEWS); DWORD arr = views ? RD32(views + 0x24) : 0;   // views [0xA35E04] +24h: 0x114 per view (REF HR_VIEWS)
        DWORD ren = RD32(A_RENDERER);
        if (!arr || !ren) return false;
        for (int i = 0; i < 16; ++i) c->m[i] = RDF(arr + 0x18 + 4 * i);           // view 0 +18h: the camera matrix (REF HR_VIEWMTX)
        c->fov_deg = RDF(arr + 0xE0); c->ratioxy = RDF(arr + 0xE4);               // +E0h fov, +E4h ratioxy (REF HR_VIEWFOV)
        c->W = (float)(int)RD32(ren + 0x320); c->H = (float)(int)RD32(ren + 0x324);  // the projection's size (REF HR_PROJ)
        c->wide_flag = *(volatile BYTE*)(uintptr_t)A_WIDE != 0;
        if (!(c->W > 0 && c->H > 0 && c->W < 20000 && c->H < 20000)) return false;
        for (int i = 0; i < 16; ++i) if (!(fabsf(c->m[i]) < 1e6f)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    // camera-to-world (translation row near the player) or world-to-camera (then the rigid inverse)
    float dx = c->m[12] - g_player.x, dz = c->m[14] - g_player.z;
    int mode = (dx * dx + dz * dz < 60.0f * 60.0f) ? 1 : 2;
    if (mode == 2) {
        float r[9] = {c->m[0], c->m[1], c->m[2], c->m[4], c->m[5], c->m[6], c->m[8], c->m[9], c->m[10]};
        float t[3] = {c->m[12], c->m[13], c->m[14]};
        // view = [R | t] (row vectors: p_view = p_world R + t) -> camera rows = R^T, position = -t R^T
        float rows[9] = {r[0], r[3], r[6], r[1], r[4], r[7], r[2], r[5], r[8]};
        float pos[3];
        for (int j = 0; j < 3; ++j) pos[j] = -(t[0] * rows[j * 3 + 0] + t[1] * rows[j * 3 + 1] + t[2] * rows[j * 3 + 2]);
        // rows[] are the camera's right / up / forward in world space
        c->m[0] = rows[0]; c->m[1] = rows[1]; c->m[2] = rows[2];
        c->m[4] = rows[3]; c->m[5] = rows[4]; c->m[6] = rows[5];
        c->m[8] = rows[6]; c->m[9] = rows[7]; c->m[10] = rows[8];
        c->m[12] = pos[0]; c->m[13] = pos[1]; c->m[14] = pos[2];
        float ex = pos[0] - g_player.x, ez = pos[2] - g_player.z;
        if (ex * ex + ez * ez > 120.0f * 120.0f) mode = 3;   // neither reading puts the camera near the player
    }
    if (mode != g_cam_mode) {
        g_cam_mode = mode;
        if (!g_logged_cam || mode == 3) {
            gp_log("minimap: camera view 0 %s (fov %.1f, ratioxy %.2f, %.0fx%.0f, position %.1f %.1f %.1f, player %.1f %.1f)\n",
                   mode == 1 ? "is camera-to-world" : (mode == 2 ? "is world-to-camera (inverted)" : "is not near the player: markers wait"),
                   c->fov_deg, c->ratioxy, c->W, c->H, c->m[12], c->m[13], c->m[14], g_player.x, g_player.z);
            g_logged_cam = true;
        }
    }
    return mode != 3;
}
static void draw_world() {
    ++g_frame_no;
    if (g_dead || !g_hc.world_marker || g_radar_frame + 1 != g_frame_no || g_radar_alpha <= 0.01f) return;
    if (gp_playmode() == 2 || !g_player_ok) return;                   // paused
    LARGE_INTEGER q0, q1, fq; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&fq);
    // the objectives, most important first, then nearest
    int idx[24], n = 0;
    float dist[24];
    for (int i = 0; i < g_nblips && n < 24; ++i) {
        const Blip& b = g_blips[i];
        if (!b.ok || !hud::is_objective(b.type)) continue;
        float dx = b.pos.x - g_player.x, dy = b.pos.y - g_player.y, dz = b.pos.z - g_player.z;
        dist[i] = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist[i] < 2.5f) continue;
        idx[n++] = i;
    }
    if (!n) return;
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0; --b) {
            int x = idx[b - 1], y = idx[b];
            int rx = hud::objective_rank(g_blips[x].type), ry = hud::objective_rank(g_blips[y].type);
            if (ry < rx || (ry == rx && dist[y] < dist[x])) { idx[b - 1] = y; idx[b] = x; } else break;
        }
    if (n > g_hc.markers) n = g_hc.markers;
    hud::Cam cam; if (!read_cam(&cam)) return;
    IDirect3DDevice9* dev = gp_overlay_device(); if (!dev || !ensure_ui(dev)) return;
    D3DVIEWPORT9 vp; if (FAILED(dev->GetViewport(&vp))) return;
    GpOvState sv; if (!gp_ov_save(dev, &sv)) return;
    gp_ov_set2d(dev);
    dev->SetTexture(0, g_tex_ui);
    float u = (g_disc.ok ? g_disc.unit : cam.H / 480.0f) * g_hc.marker_scale;   // one HUD unit in pixels
    float sx_scale = vp.Width / cam.W, sy_scale = vp.Height / cam.H;
    for (int k = 0; k < n; ++k) {
        const Blip& b = g_blips[idx[k]];
        hud::V3 p = b.pos; p.y += 1.2f;
        float sx, sy, depth;
        bool front = hud::project(cam, p, &sx, &sy, &depth);
        sx = vp.X + sx * sx_scale; sy = vp.Y + sy * sy_scale;
        bool edge; float ang;
        float margin = 26.0f * u;
        hud::V2 at = hud::clamp_to_screen(sx - vp.X, sy - vp.Y, !front, (float)vp.Width, (float)vp.Height, margin, &edge, &ang);
        at.x += vp.X; at.y += vp.Y;
        float op = g_radar_alpha * (k == 0 ? 1.0f : 0.8f);
        float near_fade = dist[idx[k]] < 8.0f ? (dist[idx[k]] - 2.5f) / 5.5f : 1.0f;
        op *= near_fade;
        DWORD col = hud::objective_colour(b.type);
        float s = (k == 0 ? 17.0f : 13.0f) * u;
        if (!edge) {
            cell_quad(dev, C_GLOW, at.x, at.y, s * 2.2f, 0.0f, argb_mul(col, op * 0.35f));
            cell_quad(dev, C_DIAMOND_O, at.x, at.y, s, 0.0f, argb_mul(0xFF1E140C, op * 0.9f));
            cell_quad(dev, C_DIAMOND_F, at.x, at.y, s, 0.0f, argb_mul(col, op));
        } else {
            cell_quad(dev, C_ARROW_O, at.x, at.y, s * 1.1f, ang, argb_mul(0xFF1E140C, op * 0.9f));
            cell_quad(dev, C_ARROW_F, at.x, at.y, s * 1.1f, ang, argb_mul(col, op));
        }
        char t[24]; hud::format_distance(dist[idx[k]], t, sizeof t);
        float th = (k == 0 ? 15.0f : 12.0f) * u, tw = text_width(t, th);
        float tx = at.x - tw * 0.5f, ty = at.y + s * 0.55f;
        if (edge) {   // keep the text inside the screen, beside the arrow
            float ox = -cosf(ang) * s * 1.2f, oy = -sinf(ang) * s * 1.2f;
            tx = at.x + ox - tw * 0.5f; ty = at.y + oy - th * 0.5f;
            if (tx < vp.X + 4) tx = vp.X + 4.0f; if (tx + tw > vp.X + vp.Width - 4) tx = vp.X + vp.Width - 4 - tw;
            if (ty < vp.Y + 4) ty = vp.Y + 4.0f; if (ty + th > vp.Y + vp.Height - 4) ty = vp.Y + vp.Height - 4 - th;
        }
        text(dev, t, tx, ty, th, argb_mul(0xFFFFF3C8, op));
    }
    gp_ov_restore(dev, &sv);
    QueryPerformanceCounter(&q1);
    double ms = (q1.QuadPart - q0.QuadPart) * 1000.0 / fq.QuadPart;
    g_pf_sum2 += ms; if (ms > g_pf_max2) g_pf_max2 = ms; ++g_pf_n2;
}
static void frame_draw() {
    draw_world();
    double now = gp_now();
    if (g_pf_last <= 0.0) g_pf_last = now;
    if (g_hc.perf_seconds > 0.0f && now - g_pf_last >= g_hc.perf_seconds && (g_pf_n || g_pf_n2)) {
        gp_log("minimap: cost per frame: map + rim %.3f ms avg / %.3f max (%d frames), world markers %.3f ms avg / %.3f max (%d frames); %d blips, map %s\n",
               g_pf_n ? g_pf_sum / g_pf_n : 0.0, g_pf_max, g_pf_n, g_pf_n2 ? g_pf_sum2 / g_pf_n2 : 0.0, g_pf_max2, g_pf_n2, (int)g_nblips,
               g_map_drawn >= 0 ? g_maps[g_map_drawn].name : "-");
        tiles_log_perf(g_hc.perf_seconds);
        g_pf_last = now; g_pf_sum = g_pf_max = g_pf_sum2 = g_pf_max2 = 0.0; g_pf_n = g_pf_n2 = 0;
    }
}

// ---------------------------------------------------------------- install
int hud_install() {
    if (!g_hc.enabled) return 0;
    static const char* refs[] = {"HR_CLASS", "HR_POS", "HR_VIEWV", "HR_BLIPS", "HR_CLAMP", "HR_PLACE", "HR_RANGE", "HR_VIEWS", "HR_VIEWMTX", "HR_VIEWFOV", "HR_PROJ",
                                 "F_GETPOS", "MT_QUAD", "MT_SPR_IDX", "MT_SPR_LIST", "PD_NODE_NAME", "PH_NODE_PARENT"};
    for (const char* r : refs) if (!gp_ref_ok(r)) { gp_log("minimap: reference %s differs: [minimap] OFF\n", r); g_hc.enabled = false; return 0; }
    g_client = gp_overlay_add_sprite(&match_sprite, &post_sprite, &draw_radar, "minimap");
    gp_overlay_set_pre(g_client, &pre_radar);
    bool fr = gp_overlay_add_frame(&frame_draw, "minimap world markers");
    gp_log("minimap: enabled (%s, zoom %.2f = %.0f m, map %s, objective arrows %d, world markers %d (max %d))\n", g_hc.rotate ? "turns with the view" : "north up",
           g_hc.zoom, hud::GAME_RADIUS_M / g_hc.zoom, g_hc.map ? (g_nmaps ? "baked" : "none baked") : "off", (int)g_hc.objective_marker, (int)g_hc.world_marker, g_hc.markers);
    return g_client >= 0 && fr ? 1 : 0;
}
void* hud_test_stub(const char* name) {
    if (!strcmp(name, "hr_match")) return (void*)&match_sprite;
    if (!strcmp(name, "hr_post_input")) return (void*)&hud_post_input;
    if (!strcmp(name, "hr_nblips")) return (void*)&g_nblips;
    if (!strcmp(name, "hr_getpos")) return (void*)&P_HR_GETPOS;
    return nullptr;
}
