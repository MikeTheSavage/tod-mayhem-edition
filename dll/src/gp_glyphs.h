// gp_glyphs.h - the controller button pictures of [pad] prompts, drawn by the DLL itself (GDI+ vector shapes and
// Windows' own Arial; no downloaded or shipped glyph art). One picture per pad::G_* index and style, 32 pixels high
// (the cell height of the game's ODPC / ODPS2 fonts), BGRA with straight alpha like the font atlas (A8R8G8B8).
// Used by gp_pad.cpp (written into the font atlas at run time) and tests/dll/test_glyphs.cpp (PNG previews).
#pragma once
#include <stdint.h>

namespace glyphs {
enum { CELL_H = 32, MAX_W = 48 };
bool init();                                   // GdiplusStartup (lazy; never from DllMain)
void shutdown();
int width(int style, int g);                   // picture width = advance in pixels (style 1 Xbox, 2 PlayStation)
// draws picture g of the style into out (width(style, g) x CELL_H, row stride `stride` pixels); false on failure
bool paint(int style, int g, uint32_t* out, int stride);
// test helper: writes a PNG of a BGRA buffer (GDI+ encoder)
bool save_png(const wchar_t* path, const uint32_t* bgra, int w, int h);
}
