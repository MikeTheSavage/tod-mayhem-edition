// gp_display_logic.h - the pure math of [display]: wide-screen HUD canvas, the pillarboxed 800x600 canvas of
// menus / subtitles / load screens, the 16:9 resolution table. No game memory, no Windows calls: unit tested by
// tests/dll/test_display.cpp. The glue that patches the game is dll/src/gp_display.cpp.
#pragma once
#include <stdint.h>

namespace gpd {

// ---- the 800x600 screen-relative canvas fitted into W x H with square pixels: x_px = ox + s x, y_px = oy + s y
struct Fit { float s, ox, oy; };
Fit fit(float W, float H);                       // s = min(W/800, H/600), centred

// what kind of screen-relative sprite a rectangle (800x600 units; centre and half size) is
enum Cls { CLS_BOX = 0, CLS_BAND = 1, CLS_FULL = 2 };   // inside / spans the full width / covers the whole canvas
Cls classify(float cx, float cy, float hw, float hh);
// how full-width bands and full-screen sprites are mapped
enum Mode { MODE_FIT = 0, MODE_STRETCH = 1, MODE_COVER = 2 };
int mode_from_name(const char* v, int def);      // "fit" / "stretch" / "cover" (anything else: def)

// the 4x4 row-vector matrix the engine's 0x4671A0 builds (scale), plus a translation row (row 3)
struct Map { float ax, ay, tx, ty; };            // x_px = tx + ax x, y_px = ty + ay y
Map sprite_map(float sx, float sy, Cls c, int band_mode, int full_mode);   // sx = W/800, sy = H/600 (the engine's args)
void write_matrix(float m[16], const Map& p, float sz);

// TextBox glyph scale: s with the lowest mantissa bit set, so the DrawString hook can tell a screen-relative box
// (font scale written by the patched calls) from any other text (scale 1.0 or something else, never tagged)
float tag_scale(float s);
bool  is_tagged(uint32_t xbits, uint32_t ybits, float tag);

// mouse: pixel -> canvas units (inverse of the fit)
void mouse_to_canvas(const Fit& f, float mx, float my, float* u, float* v);

// load bar: a vanilla pixel point (unit * W/800, unit * H/600) -> the fitted canvas
void loadbar_point(float sx, float sy, const Fit& f, float* x, float* y);

// ---- the resolution table (code 0x4AB940, five entries behind the SCREEN RESOLUTION option)
struct Res { int w, h; };
// "auto" -> 1280x720, 1600x900, 1920x1080, 2560x1440 and index 4 = the configured size (cfg_w x cfg_h, or 1920x1080);
// "WxH, WxH, WxH, WxH, WxH" -> those; returns false (out untouched) for anything else
bool res_table(const char* spec, int cfg_w, int cfg_h, Res out[5]);
// configpc.txt: "screensize = W,H" (last one wins); false when missing / not sane
bool parse_screensize(const char* text, int* w, int* h);

// ---- wide HUD (research proxy hudwide): virtual HUD 480/hud_scale high, width follows the screen aspect
struct Hud { int vw, vh; float dot_x, dot_y; };
int hud_height(float hud_scale);                 // round(480 / clamp(hud_scale, 0.25, 4))
Hud hud(int W, int H, int vh);

// ---- display modes in the game's own SETTINGS > SCREEN RESOLUTION row (mode_menu; glue gp_display_modes.cpp)
enum Kind { K_WINDOWED = 0, K_BORDERLESS = 1, K_FULLSCREEN = 2 };
struct DMode { int w, h, kind; };
inline bool same_mode(const DMode& a, const DMode& b) { return a.w == b.w && a.h == b.h && a.kind == b.kind; }
// the monitor the window is on, in physical pixels (the DLL makes the game DPI aware): full size, work area (without the
// task bar) and the extra size a framed window needs around its client area (AdjustWindowRect of the game's style)
struct Mon { int mon_w, mon_h, work_w, work_h, frame_w, frame_h; };
// the standard sizes the row may offer (16:9, 16:10, 3:2, 4:3, 21:9; ascending area); returns the count
int standard_sizes(Res* out, int max);
// the row's values: windowed = standard sizes whose framed window fits the work area (21:9 only on a monitor at least
// 2:1 wide); borderless = the monitor's own size; fullscreen = the adapter's sizes (fullscreen_ok: exclusive mode is
// possible on this monitor) that are standard or the monitor's size, at least 1024x720; `cur` (the mode in use) is
// added when it is not one of them. Sorted windowed / borderless / fullscreen, then by area, deduplicated.
int build_modes(const Mon& m, const Res* adapter, int nadapter, bool fullscreen_ok, const DMode* cur, DMode* out, int max);
int find_mode(const DMode* list, int n, const DMode& m);   // index or -1
// "1920X1080 WINDOW", "3024X1964 NATIVE" (borderless, always the monitor's size), "1920X1080 FULL" (exclusive)
int label(const DMode& m, int native_w, int native_h, char* out, int cap);
const char* kind_name(int kind);                 // "windowed" / "borderless" / "fullscreen"
int kind_from_name(const char* v);               // -1 when unknown
// the mod's side file (TODGameplay_display.txt): "mode = borderless", "width = 3024", "height = 1964", optional
// "monitor = x,y" (top-left of the monitor it was chosen on)
bool parse_mode_file(const char* text, DMode* m, int* mon_x, int* mon_y, bool* has_mon);
int format_mode_file(const DMode& m, int mon_x, int mon_y, char* out, int cap);
// configpc.txt "fullscreen = true|false" (last one wins); false when missing
bool parse_fullscreen(const char* text, bool* fs);
// the first start without a side file: configpc fullscreen / screensize and the old [display] borderless key
DMode startup_default(bool cfg_fs, int cfg_w, int cfg_h, bool borderless_ini, const Mon& mon, const Res* adapter, int nadapter, bool fullscreen_ok);
// a mode made valid for this monitor: windowed shrinks to the largest standard size that fits (same aspect first),
// borderless takes the monitor's size, fullscreen at a size the adapter does not offer becomes borderless
DMode validate(const DMode& d, const Mon& mon, const Res* adapter, int nadapter, bool fullscreen_ok);
// the game WITHOUT the DLL (DPI unaware: sizes are logical = physical / dpi_scale) at its next start: the windowed size
// to put into configpc.txt / the registry (the chosen size when it is windowed and fits, else the largest 16:9 size
// that fits), and the table index (0..4: 640x480 .. 1600x1200) the saved SCREEN RESOLUTION keeps: the largest 4:3
// size not taller than that window. The saves never get an index above 4 (vanilla menus index the value list with it).
Res vanilla_window(const DMode& d, const Mon& mon, float dpi_scale);
int vanilla_index(const Res& win, const Mon& mon, float dpi_scale);
// configpc.txt with "fullscreen = ..." and "screensize = W,H" set (in place, or added at the top when missing); every
// other byte stays (line ends kept). Returns the new length, -1 when it does not fit `cap`.
int configpc_set(const char* in, bool fullscreen, int w, int h, char* out, int cap);
// the label text box: font scale so a label of `len` characters takes at most `max_chars` vanilla characters
float label_scale(int len, int max_chars);

}  // namespace gpd
