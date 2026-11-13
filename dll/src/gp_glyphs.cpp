// gp_glyphs.cpp - controller button pictures for the [pad] prompts (see gp_glyphs.h). Everything is drawn here from
// shapes: discs, pills, triggers, a d-pad cross and stick tops, with Arial Bold letters. Drawn 4x larger with GDI+
// anti-aliasing, then scaled down with a bicubic filter to the 32-pixel font cell.
#include <windows.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <string.h>
#include <stdio.h>
#include "gp_glyphs.h"
#include "gp_pad_logic.h"

using namespace Gdiplus;

namespace glyphs {

static ULONG_PTR g_tok = 0;
static bool g_ok = false;
bool init() {
    if (g_ok) return true;
    GdiplusStartupInput in;
    g_ok = GdiplusStartup(&g_tok, &in, nullptr) == Ok;
    return g_ok;
}
void shutdown() { if (g_ok) { GdiplusShutdown(g_tok); g_ok = false; } }

enum Shape { SH_FACE, SH_PILL, SH_TRIGGER, SH_CLICK, SH_SYS, SH_DPAD, SH_STICK };
static Shape shape_of(int g) {
    using namespace pad;
    switch (g) {
    case B_A: case B_B: case B_X: case B_Y: return SH_FACE;
    case B_LB: case B_RB: return SH_PILL;
    case B_LT: case B_RT: return SH_TRIGGER;
    case B_LS: case B_RS: return SH_CLICK;
    case B_BACK: case B_START: return SH_SYS;
    case B_UP: case B_DOWN: case B_LEFT: case B_RIGHT: return SH_DPAD;
    default: return SH_STICK;
    }
}
int width(int style, int g) {
    switch (shape_of(g)) {
    case SH_PILL: case SH_TRIGGER: return 40;
    case SH_SYS: return style == 2 ? 46 : 30;
    default: return 30;
    }
}

static const Color DARK(255, 38, 38, 40), RIM(255, 120, 120, 126), WHITE(255, 244, 244, 244), DIM(255, 92, 92, 98);

static void rounded(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
}
static void text(Graphics& gr, const wchar_t* t, REAL cx, REAL cy, REAL px, const Color& c) {
    FontFamily fam(L"Arial");
    Font f(&fam, px, FontStyleBold, UnitPixel);
    StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(c);
    RectF r(cx - 200, cy - 100, 400, 200);
    gr.DrawString(t, -1, &f, r, &sf, &b);
}
static void disc(Graphics& gr, REAL cx, REAL cy, REAL r, const Color& fill, const Color& rim, REAL rimw) {
    SolidBrush b(fill); gr.FillEllipse(&b, cx - r, cy - r, 2 * r, 2 * r);
    if (rimw > 0) { Pen p(rim, rimw); gr.DrawEllipse(&p, cx - r + rimw / 2, cy - r + rimw / 2, 2 * r - rimw, 2 * r - rimw); }
}
static void arrow(Graphics& gr, REAL cx, REAL cy, REAL s, int dir, const Color& c) {   // little filled triangle
    PointF p[3];
    switch (dir) {
    case 0: p[0] = PointF(cx, cy - s); p[1] = PointF(cx - s, cy + s * 0.6f); p[2] = PointF(cx + s, cy + s * 0.6f); break;   // up
    case 1: p[0] = PointF(cx, cy + s); p[1] = PointF(cx - s, cy - s * 0.6f); p[2] = PointF(cx + s, cy - s * 0.6f); break;   // down
    case 2: p[0] = PointF(cx - s, cy); p[1] = PointF(cx + s * 0.6f, cy - s); p[2] = PointF(cx + s * 0.6f, cy + s); break;   // left
    default: p[0] = PointF(cx + s, cy); p[1] = PointF(cx - s * 0.6f, cy - s); p[2] = PointF(cx - s * 0.6f, cy + s); break;  // right
    }
    SolidBrush b(c); gr.FillPolygon(&b, p, 3);
}

// draws at 4x: canvas W x 128, centre line y = 64
static void draw(Graphics& gr, int style, int g, REAL W) {
    using namespace pad;
    const REAL cy = 64, cx = W / 2;
    bool ps = style == 2;
    switch (shape_of(g)) {
    case SH_FACE: {
        static const Color XB[4] = {Color(255, 108, 194, 74), Color(255, 232, 71, 58), Color(255, 61, 140, 232), Color(255, 242, 195, 24)};
        static const Color PSC[4] = {Color(255, 140, 184, 240), Color(255, 242, 103, 107), Color(255, 232, 155, 212), Color(255, 63, 209, 176)};
        int i = g - B_A;
        if (!ps) {
            disc(gr, cx, cy, 54, DARK, XB[i], 7);
            static const wchar_t* L[4] = {L"A", L"B", L"X", L"Y"};
            text(gr, L[i], cx, cy + 3, 70, XB[i]);
        } else {
            disc(gr, cx, cy, 54, DARK, RIM, 5);
            Pen p(PSC[i], 9); p.SetLineJoin(LineJoinRound); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
            if (g == B_A) { gr.DrawLine(&p, cx - 22, cy - 22, cx + 22, cy + 22); gr.DrawLine(&p, cx - 22, cy + 22, cx + 22, cy - 22); }
            else if (g == B_B) gr.DrawEllipse(&p, cx - 25, cy - 25, 50.0f, 50.0f);
            else if (g == B_X) gr.DrawRectangle(&p, cx - 22, cy - 22, 44.0f, 44.0f);
            else { PointF t[3] = {PointF(cx, cy - 28), PointF(cx - 27, cy + 20), PointF(cx + 27, cy + 20)}; gr.DrawPolygon(&p, t, 3); }
        }
        break;
    }
    case SH_PILL: case SH_TRIGGER: {
        bool trig = shape_of(g) == SH_TRIGGER;
        REAL h = trig ? 92.0f : 70.0f, top = cy - h / 2;
        GraphicsPath p;
        if (trig) {   // trigger: tall, round on top, flat-ish bottom
            p.AddArc(8.0f, top, W - 16, 60.0f, 180, 180);
            p.AddLine(W - 8, top + 30, W - 8, top + h - 14);
            p.AddArc(W - 8 - 28, top + h - 28, 28.0f, 28.0f, 0, 90);
            p.AddArc(8.0f, top + h - 28, 28.0f, 28.0f, 90, 90);
            p.CloseFigure();
        } else rounded(p, 6, top, W - 12, h, 30);
        SolidBrush b(DARK); gr.FillPath(&b, &p);
        Pen pen(RIM, 6); gr.DrawPath(&pen, &p);
        static const wchar_t* XL[4] = {L"LB", L"RB", L"LT", L"RT"};
        static const wchar_t* PL[4] = {L"L1", L"R1", L"L2", L"R2"};
        text(gr, (ps ? PL : XL)[g - B_LB], cx, cy + (trig ? 6.0f : 2.0f), 50, WHITE);
        break;
    }
    case SH_CLICK: {
        disc(gr, cx, cy, 54, DARK, WHITE, 7);
        disc(gr, cx, cy, 36, DIM, DIM, 0);
        static const wchar_t* XL[2] = {L"LS", L"RS"};
        static const wchar_t* PL[2] = {L"L3", L"R3"};
        text(gr, (ps ? PL : XL)[g - B_LS], cx, cy + 2, 42, WHITE);
        break;
    }
    case SH_SYS: {
        if (!ps) {
            disc(gr, cx, cy, 54, DARK, RIM, 6);
            Pen pen(WHITE, 7);
            if (g == B_BACK) {   // View: two overlapping windows
                gr.DrawRectangle(&pen, cx - 24, cy - 22, 30.0f, 26.0f);
                SolidBrush bw(DARK); gr.FillRectangle(&bw, cx - 8, cy - 6, 32.0f, 28.0f);
                gr.DrawRectangle(&pen, cx - 8, cy - 6, 30.0f, 26.0f);
            } else {             // Menu: three lines
                for (int k = -1; k <= 1; ++k) gr.DrawLine(&pen, cx - 22, cy + k * 15.0f, cx + 22, cy + k * 15.0f);
            }
        } else {
            GraphicsPath p; rounded(p, 6, cy - 34, W - 12, 68, 34);
            SolidBrush b(DARK); gr.FillPath(&b, &p);
            Pen pen(RIM, 6); gr.DrawPath(&pen, &p);
            text(gr, g == B_BACK ? L"SHARE" : L"OPTIONS", cx, cy + 2, g == B_BACK ? 34.0f : 28.0f, WHITE);
        }
        break;
    }
    case SH_DPAD: {
        int dir = g == B_UP ? 0 : g == B_DOWN ? 1 : g == B_LEFT ? 2 : 3;
        if (!ps) {   // one cross
            GraphicsPath p;
            rounded(p, cx - 18, cy - 56, 36, 112, 8);
            GraphicsPath q; rounded(q, cx - 56, cy - 18, 112, 36, 8);
            Region rg(&p); rg.Union(&q);
            SolidBrush b(DARK); gr.FillRegion(&b, &rg);
            Pen pen(RIM, 5); gr.DrawPath(&pen, &p); gr.DrawPath(&pen, &q);
            SolidBrush b2(DARK); gr.FillRectangle(&b2, cx - 15, cy - 15, 30.0f, 30.0f);
            static const REAL OX[4] = {0, 0, -34, 34}, OY[4] = {-34, 34, 0, 0};
            {   // the pressed arm lit
                GraphicsPath a; bool vert = dir < 2;
                rounded(a, cx + OX[dir] - (vert ? 15 : 20), cy + OY[dir] - (vert ? 20 : 15), vert ? 30.0f : 40.0f, vert ? 40.0f : 30.0f, 6);
                SolidBrush lit(WHITE); gr.FillPath(&lit, &a);
            }
            for (int k = 0; k < 4; ++k) arrow(gr, cx + OX[k], cy + OY[k], 11, k, k == dir ? DARK : DIM);
        } else {     // four separate pads
            static const REAL OX[4] = {0, 0, -33, 33}, OY[4] = {-33, 33, 0, 0};
            for (int k = 0; k < 4; ++k) {
                GraphicsPath p;
                bool vert = k < 2;
                rounded(p, cx + OX[k] - (vert ? 15 : 20), cy + OY[k] - (vert ? 20 : 15), vert ? 30.0f : 40.0f, vert ? 40.0f : 30.0f, 7);
                SolidBrush b(k == dir ? WHITE : DARK); gr.FillPath(&b, &p);
                Pen pen(RIM, 4); gr.DrawPath(&pen, &p);
                arrow(gr, cx + OX[k], cy + OY[k], 9, k, k == dir ? DARK : DIM);
            }
        }
        break;
    }
    default: {       // whole stick (move / look): stick top with four arrows
        bool right = g == G_RSTICK;
        disc(gr, cx, cy, 56, Color(255, 64, 64, 70), RIM, 4);
        disc(gr, cx, cy, 38, DARK, DIM, 4);
        static const REAL OX[4] = {0, 0, -47, 47}, OY[4] = {-47, 47, 0, 0};
        for (int k = 0; k < 4; ++k) arrow(gr, cx + OX[k], cy + OY[k], 8, k, WHITE);
        text(gr, right ? L"R" : L"L", cx, cy + 2, 44, WHITE);
        break;
    }
    }
}

bool paint(int style, int g, uint32_t* out, int stride) {
    if (!init() || g < 0 || g >= pad::G_COUNT || (style != 1 && style != 2)) return false;
    int w = width(style, g), W = w * 4, H = CELL_H * 4;
    Bitmap big(W, H, PixelFormat32bppARGB);
    {
        Graphics gr(&big);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintAntiAlias);
        gr.Clear(Color(0, 0, 0, 0));
        draw(gr, style, g, (REAL)W);
    }
    Bitmap little(w, CELL_H, PixelFormat32bppARGB);
    {
        Graphics gs(&little);
        gs.Clear(Color(0, 0, 0, 0));
        gs.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gs.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        gs.SetCompositingMode(CompositingModeSourceCopy);
        gs.DrawImage(&big, Rect(0, 0, w, CELL_H), 0, 0, W, H, UnitPixel);
    }
    BitmapData bd; Rect r(0, 0, w, CELL_H);
    if (little.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return false;
    for (int y = 0; y < CELL_H; ++y) memcpy(out + y * stride, (BYTE*)bd.Scan0 + y * bd.Stride, w * 4);
    little.UnlockBits(&bd);
    return true;
}

static int png_clsid(CLSID* id) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return -1;
    ImageCodecInfo* info = (ImageCodecInfo*)malloc(size);
    GetImageEncoders(n, size, info);
    int r = -1;
    for (UINT i = 0; i < n; ++i) if (!wcscmp(info[i].MimeType, L"image/png")) { *id = info[i].Clsid; r = (int)i; break; }
    free(info);
    return r;
}
bool save_png(const wchar_t* path, const uint32_t* bgra, int w, int h) {
    if (!init()) return false;
    Bitmap bmp(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)bgra);
    CLSID id;
    if (png_clsid(&id) < 0) return false;
    Status st = bmp.Save(path, &id, nullptr);
    if (st != Ok) { char m[96]; sprintf_s(m, "gp_glyphs: PNG save status %d\n", (int)st); OutputDebugStringA(m); fputs(m, stdout); }
    return st == Ok;
}

}  // namespace glyphs
