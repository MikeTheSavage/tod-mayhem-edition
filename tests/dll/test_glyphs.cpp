// test_glyphs.cpp - draws every [pad] prompt picture (dll/src/gp_glyphs.cpp) for both styles, checks the sizes and
// that every picture has visible pixels, and writes preview sheets: <out>/glyphs_sheet.png (x1 and x3 on three
// backgrounds) and <out>/glyph_atlas_cells.png (the cells exactly as they go into the font atlas).
// usage: test_glyphs.exe <out folder>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "../../dll/src/gp_glyphs.h"
#include "../../dll/src/gp_pad_logic.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint32_t blend(uint32_t dst, uint32_t src) {
    unsigned a = src >> 24;
    unsigned r = ((src >> 16) & 255) * a + ((dst >> 16) & 255) * (255 - a);
    unsigned g = ((src >> 8) & 255) * a + ((dst >> 8) & 255) * (255 - a);
    unsigned b = (src & 255) * a + (dst & 255) * (255 - a);
    return 0xFF000000u | ((r / 255) << 16) | ((g / 255) << 8) | (b / 255);
}

int main(int argc, char** argv) {
    char outdir[MAX_PATH]; strcpy_s(outdir, argc > 1 ? argv[1] : ".");
    { size_t L = strlen(outdir); while (L > 1 && (outdir[L - 1] == 92 || outdir[L - 1] == '/' || outdir[L - 1] == '"')) outdir[--L] = 0; }
    if (!glyphs::init()) { printf("FAIL GdiplusStartup\n"); return 1; }
    const int H = glyphs::CELL_H, SCALE = 3;
    // sheet: 3 backgrounds (dark, game-orange, white) x 2 styles rows; each glyph x1 then x3
    int rowW = 0;
    for (int g = 0; g < pad::G_COUNT; ++g) rowW += (glyphs::width(2, g) > glyphs::width(1, g) ? glyphs::width(2, g) : glyphs::width(1, g)) * (1 + SCALE) + 12;
    const uint32_t BG[3] = {0xFF202020u, 0xFFE07818u, 0xFFF2F2F2u};
    int W = rowW + 16, Hs = 3 * 2 * (H * SCALE + 12) + 16;
    std::vector<uint32_t> sheet((size_t)W * Hs, 0xFF808080u);
    std::vector<uint32_t> atlas((size_t)512 * 128, 0);
    int ax = 0, ay = 0;
    for (int bgi = 0; bgi < 3; ++bgi)
        for (int st = 1; st <= 2; ++st) {
            int oy = 8 + (bgi * 2 + st - 1) * (H * SCALE + 12);
            for (int y = oy - 4; y < oy + H * SCALE + 4; ++y) for (int x = 4; x < W - 4; ++x) sheet[(size_t)y * W + x] = BG[bgi];
            int ox = 8;
            for (int g = 0; g < pad::G_COUNT; ++g) {
                int w = glyphs::width(st, g);
                CHECK(w > 8 && w <= glyphs::MAX_W);
                std::vector<uint32_t> cell((size_t)w * H, 0);
                CHECK(glyphs::paint(st, g, cell.data(), w));
                int opaque = 0;
                for (uint32_t p : cell) if ((p >> 24) > 128) ++opaque;
                CHECK(opaque > 60);                       // a visible picture
                int edge = 0; for (int y = 0; y < H; ++y) edge += (cell[(size_t)y * w] >> 24) > 200;
                if (edge > 2) { printf("glyph %d style %d: %d opaque pixels in the left column\n", g, st, edge); CHECK(edge <= 2); }
                for (int y = 0; y < H; ++y) for (int x = 0; x < w; ++x) {
                    uint32_t& d = sheet[(size_t)(oy + y) * W + ox + x]; d = blend(d, cell[(size_t)y * w + x]);
                    for (int yy = 0; yy < SCALE; ++yy) for (int xx = 0; xx < SCALE; ++xx) {
                        uint32_t& e = sheet[(size_t)(oy + y * SCALE + yy) * W + ox + w + 4 + x * SCALE + xx]; e = blend(e, cell[(size_t)y * w + x]);
                    }
                }
                ox += w * (1 + SCALE) + 12;
                if (bgi == 0) {   // atlas layout as the DLL does it (rows of 32, wrap at 510)
                    if (ax + w > 510) { ax = 0; ay += H; }
                    for (int y = 0; y < H; ++y) memcpy(&atlas[(size_t)(ay + y) * 512 + ax], &cell[(size_t)y * w], w * 4);
                    ax += w + 1;
                }
            }
            if (bgi == 0 && st == 1) { ax = 0; ay += H; }
        }
    wchar_t p1[MAX_PATH], p2[MAX_PATH];
    swprintf(p1, MAX_PATH, L"%hs\\glyphs_sheet.png", outdir);
    swprintf(p2, MAX_PATH, L"%hs\\glyph_atlas_cells.png", outdir);
    CHECK(glyphs::save_png(p1, sheet.data(), W, Hs));
    CHECK(glyphs::save_png(p2, atlas.data(), 512, 128));
    CHECK(ay + H <= 128);   // both styles fit in 4 rows (128 px) of the atlas' free half
    glyphs::shutdown();
    printf("test_glyphs: %d glyphs x 2 styles, sheet %dx%d, %s\n", (int)pad::G_COUNT, W, Hs, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
