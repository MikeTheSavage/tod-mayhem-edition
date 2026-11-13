// gp_display_logic.cpp - see gp_display_logic.h. Pure functions, unit tested.
#include "gp_display_logic.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

namespace gpd {

static const float CW = 800.0f, CH = 600.0f;
static const float EDGE = 1.0f;   // a rectangle within 1 unit of both edges spans them

Fit fit(float W, float H) {
    Fit f;
    if (W <= 0.0f || H <= 0.0f) { f.s = 1.0f; f.ox = f.oy = 0.0f; return f; }
    float sx = W / CW, sy = H / CH;
    f.s = sx < sy ? sx : sy;
    f.ox = (W - CW * f.s) * 0.5f;
    f.oy = (H - CH * f.s) * 0.5f;
    return f;
}

Cls classify(float cx, float cy, float hw, float hh) {
    if (hw < 0.0f) hw = -hw;
    if (hh < 0.0f) hh = -hh;
    bool fw = cx - hw <= EDGE && cx + hw >= CW - EDGE;
    bool fh = cy - hh <= EDGE && cy + hh >= CH - EDGE;
    if (fw && fh) return CLS_FULL;
    if (fw) return CLS_BAND;
    return CLS_BOX;
}

int mode_from_name(const char* v, int def) {
    if (!v) return def;
    if (!_stricmp(v, "fit")) return MODE_FIT;
    if (!_stricmp(v, "stretch")) return MODE_STRETCH;
    if (!_stricmp(v, "cover")) return MODE_COVER;
    return def;
}

Map sprite_map(float sx, float sy, Cls c, int band_mode, int full_mode) {
    Fit f = fit(sx * CW, sy * CH);
    Map m = {f.s, f.s, f.ox, f.oy};
    int mode = c == CLS_FULL ? full_mode : c == CLS_BAND ? band_mode : MODE_FIT;
    if (mode == MODE_STRETCH) {
        m.ax = sx; m.tx = 0.0f;                                  // the full width, as vanilla
        if (c == CLS_FULL) { m.ay = sy; m.ty = 0.0f; }           // the full height too (vanilla)
    } else if (mode == MODE_COVER) {
        float k = sx > sy ? sx : sy;                             // uniform, covers the screen, crops the overhang
        m.ax = m.ay = k;
        m.tx = (sx * CW - CW * k) * 0.5f;
        m.ty = (sy * CH - CH * k) * 0.5f;
    }
    return m;
}

void write_matrix(float m[16], const Map& p, float sz) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = p.ax; m[5] = p.ay; m[10] = sz;
    m[12] = p.tx; m[13] = p.ty; m[15] = 1.0f;
}

float tag_scale(float s) {
    uint32_t b; memcpy(&b, &s, 4);
    b |= 1u;
    float r; memcpy(&r, &b, 4);
    return r;
}
bool is_tagged(uint32_t xbits, uint32_t ybits, float tag) {
    uint32_t t; memcpy(&t, &tag, 4);
    return (t & 1u) && xbits == t && ybits == t;
}

void mouse_to_canvas(const Fit& f, float mx, float my, float* u, float* v) {
    float s = f.s > 0.0f ? f.s : 1.0f;
    *u = (mx - f.ox) / s;
    *v = (my - f.oy) / s;
}

void loadbar_point(float sx, float sy, const Fit& f, float* x, float* y) {
    if (sx > 0.0f) *x = *x / sx * f.s + f.ox;
    if (sy > 0.0f) *y = *y / sy * f.s + f.oy;
}

static bool sane(int w, int h) { return w >= 320 && h >= 240 && w <= 16384 && h <= 16384; }

bool res_table(const char* spec, int cfg_w, int cfg_h, Res out[5]) {
    if (!spec) return false;
    while (*spec == ' ' || *spec == '\t') ++spec;
    if (!_strnicmp(spec, "auto", 4)) {
        static const Res base[4] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
        for (int i = 0; i < 4; ++i) out[i] = base[i];
        out[4].w = sane(cfg_w, cfg_h) ? cfg_w : 1920;
        out[4].h = sane(cfg_w, cfg_h) ? cfg_h : 1080;
        return true;
    }
    Res r[5]; int n = 0; const char* p = spec;
    while (n < 5) {
        int w = 0, h = 0, used = 0;
        if (sscanf_s(p, " %d x %d%n", &w, &h, &used) != 2) break;   // "1280x720" and "1280 x 720"
        if (!sane(w, h)) return false;
        r[n].w = w; r[n].h = h; ++n;
        p += used;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != ',') break;
        ++p;
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (n != 5 || *p) return false;
    for (int i = 0; i < 5; ++i) out[i] = r[i];
    return true;
}

bool parse_screensize(const char* text, int* w, int* h) {
    bool found = false;
    const char* p = text;
    while (p && *p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256]; size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
        memcpy(line, p, n); line[n] = 0;
        p = eol ? eol + 1 : nullptr;
        char* c = line; while (*c == ' ' || *c == '\t') ++c;
        if (*c == '#' || *c == ';' || _strnicmp(c, "screensize", 10)) continue;
        c += 10; while (*c == ' ' || *c == '\t') ++c;
        if (*c != '=') continue;
        int a = 0, b = 0;
        if (sscanf_s(c + 1, " %d , %d", &a, &b) == 2 && sane(a, b)) { *w = a; *h = b; found = true; }
    }
    return found;
}

int hud_height(float hud_scale) {
    if (!(hud_scale >= 0.25f)) hud_scale = hud_scale < 0.25f ? 0.25f : 1.0f;   // NaN -> 1
    if (hud_scale > 4.0f) hud_scale = 4.0f;
    return (int)floor(480.0 / hud_scale + 0.5);
}

Hud hud(int W, int H, int vh) {
    Hud r;
    if (W <= 0 || H <= 0) { W = 640; H = 480; }
    if (vh <= 0) vh = 480;
    r.vh = vh;
    r.vw = (int)floor(vh * (double)W / (double)H + 0.5);
    r.dot_x = r.vw * 0.5f + 10.0f * vh / 480.0f;   // vanilla 640 -> 330, console 853 -> 437 (436.5)
    r.dot_y = 176.0f * vh / 480.0f;
    return r;
}

// ================================================================================================ display modes
static const Res STD[] = {
    {1024, 768}, {1280, 720}, {1280, 800}, {1280, 960}, {1440, 900}, {1600, 900}, {1440, 960}, {1680, 1050},
    {1600, 1200}, {1920, 1080}, {1920, 1200}, {1920, 1280}, {2560, 1080}, {2160, 1440}, {2560, 1440}, {2560, 1600},
    {3440, 1440}, {3000, 2000}, {3200, 1800}, {3840, 1600}, {3840, 2160}, {5120, 2880}};
// sizes the windowed list offers (a shorter list: one per useful step); fullscreen takes every STD size the adapter has
static bool windowed_std(int w, int h) {
    static const Res W[] = {{1024, 768}, {1280, 720}, {1280, 800}, {1600, 900}, {1680, 1050}, {1600, 1200}, {1920, 1080},
                            {1920, 1200}, {1920, 1280}, {2560, 1080}, {2160, 1440}, {2560, 1440}, {2560, 1600}, {3440, 1440},
                            {3200, 1800}, {3840, 2160}};
    for (const Res& r : W) if (r.w == w && r.h == h) return true;
    return false;
}
static bool is_std(int w, int h) { for (const Res& r : STD) if (r.w == w && r.h == h) return true; return false; }
static bool ultrawide(int w, int h) { return h > 0 && w * 100 >= h * 200; }   // 21:9 sizes (2.33) only on >= 2:1 monitors

int standard_sizes(Res* out, int max) {
    int n = 0;
    for (const Res& r : STD) if (n < max) out[n++] = r;
    return n;
}

static bool fits_window(const Mon& m, int w, int h) { return w + m.frame_w <= m.work_w && h + m.frame_h <= m.work_h; }
static bool adapter_has(const Res* a, int n, int w, int h) { for (int i = 0; i < n; ++i) if (a[i].w == w && a[i].h == h) return true; return false; }
static long long area(const DMode& d) { return (long long)d.w * d.h; }
static bool before(const DMode& a, const DMode& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (area(a) != area(b)) return area(a) < area(b);
    return a.w < b.w;
}
static int add_mode(DMode* out, int n, int max, const DMode& d) {
    for (int i = 0; i < n; ++i) if (same_mode(out[i], d)) return n;
    if (n < max) out[n++] = d;
    return n;
}

int build_modes(const Mon& m, const Res* adapter, int nadapter, bool fullscreen_ok, const DMode* cur, DMode* out, int max) {
    int n = 0;
    if (max <= 0) return 0;
    bool uw = ultrawide(m.mon_w, m.mon_h);
    for (const Res& r : STD) {
        if (!windowed_std(r.w, r.h) || (!uw && ultrawide(r.w, r.h))) continue;
        if (fits_window(m, r.w, r.h)) n = add_mode(out, n, max, DMode{r.w, r.h, K_WINDOWED});
    }
    if (m.mon_w > 0 && m.mon_h > 0) n = add_mode(out, n, max, DMode{m.mon_w, m.mon_h, K_BORDERLESS});
    if (fullscreen_ok) {
        for (int i = 0; i < nadapter; ++i) {
            const Res& r = adapter[i];
            if (r.w < 1024 || r.h < 720) continue;
            bool native = r.w == m.mon_w && r.h == m.mon_h;
            if (!native && !is_std(r.w, r.h)) continue;
            if (!native && !uw && ultrawide(r.w, r.h)) continue;
            if (r.w > m.mon_w || r.h > m.mon_h) continue;     // no mode above the monitor's own size
            n = add_mode(out, n, max, DMode{r.w, r.h, K_FULLSCREEN});
        }
    }
    if (cur && cur->w > 0 && cur->h > 0) n = add_mode(out, n, max, *cur);
    for (int i = 1; i < n; ++i) {                          // insertion sort (n is small)
        DMode k = out[i]; int j = i - 1;
        while (j >= 0 && before(k, out[j])) { out[j + 1] = out[j]; --j; }
        out[j + 1] = k;
    }
    return n;
}

int find_mode(const DMode* list, int n, const DMode& m) {
    for (int i = 0; i < n; ++i) if (same_mode(list[i], m)) return i;
    return -1;
}

const char* kind_name(int kind) { return kind == K_BORDERLESS ? "borderless" : kind == K_FULLSCREEN ? "fullscreen" : "windowed"; }
int kind_from_name(const char* v) {
    if (!v) return -1;
    while (*v == ' ' || *v == '\t') ++v;
    if (!_strnicmp(v, "windowed", 8) || !_strnicmp(v, "window", 6)) return K_WINDOWED;
    if (!_strnicmp(v, "borderless", 10)) return K_BORDERLESS;
    if (!_strnicmp(v, "fullscreen", 10) || !_strnicmp(v, "exclusive", 9)) return K_FULLSCREEN;
    return -1;
}

int label(const DMode& m, int native_w, int native_h, char* out, int cap) {
    // the SETTINGS value box is 220 menu units wide = about 16 characters of the menu font; a longer text wraps onto the
    // next row (in game, also in the pause menu, whose animation resets any text scale). So: "1920X1080 WINDOW",
    // "3024X1964 NATIVE" (borderless at the monitor's own size), "1920X1080 FULL" (exclusive fullscreen)
    static const char* const word[3] = {"WINDOW", "NATIVE", "FULL"};
    int k = m.kind >= 0 && m.kind <= 2 ? m.kind : 0;
    (void)native_w; (void)native_h;
    int n = _snprintf_s(out, cap, _TRUNCATE, "%dX%d %s", m.w, m.h, word[k]);
    return n < 0 ? (int)strlen(out) : n;
}

// one "key = value" per line; '#' / ';' comments
static bool kv(const char* line, const char* key, const char** val) {
    while (*line == ' ' || *line == '\t') ++line;
    size_t k = strlen(key);
    if (_strnicmp(line, key, k)) return false;
    line += k;
    while (*line == ' ' || *line == '\t') ++line;
    if (*line != '=') return false;
    ++line;
    while (*line == ' ' || *line == '\t') ++line;
    *val = line;
    return true;
}
template <class F> static void each_line(const char* text, F f) {
    const char* p = text;
    while (p && *p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256]; size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
        memcpy(line, p, n); line[n] = 0;
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        const char* c = line; while (*c == ' ' || *c == '\t') ++c;
        if (*c && *c != '#' && *c != ';') f(c);
        p = eol ? eol + 1 : nullptr;
    }
}

bool parse_mode_file(const char* text, DMode* m, int* mon_x, int* mon_y, bool* has_mon) {
    if (!text) return false;
    DMode d = {0, 0, -1}; bool hm = false; int mx = 0, my = 0;
    each_line(text, [&](const char* c) {
        const char* v;
        if (kv(c, "mode", &v)) d.kind = kind_from_name(v);
        else if (kv(c, "width", &v)) d.w = atoi(v);
        else if (kv(c, "height", &v)) d.h = atoi(v);
        else if (kv(c, "monitor", &v)) { int a = 0, b = 0; if (sscanf_s(v, " %d , %d", &a, &b) == 2) { mx = a; my = b; hm = true; } }
    });
    if (d.kind < 0 || !sane(d.w, d.h)) return false;
    *m = d;
    if (mon_x) *mon_x = mx;
    if (mon_y) *mon_y = my;
    if (has_mon) *has_mon = hm;
    return true;
}
int format_mode_file(const DMode& m, int mon_x, int mon_y, char* out, int cap) {
    int n = _snprintf_s(out, cap, _TRUNCATE,
                        "# TOD Gameplay+ display mode (chosen in SETTINGS > SCREEN RESOLUTION; written by TODGameplay.dll)\r\n"
                        "mode = %s\r\nwidth = %d\r\nheight = %d\r\nmonitor = %d,%d\r\n", kind_name(m.kind), m.w, m.h, mon_x, mon_y);
    return n < 0 ? (int)strlen(out) : n;
}

bool parse_fullscreen(const char* text, bool* fs) {
    bool found = false;
    if (!text) return false;
    each_line(text, [&](const char* c) {
        const char* v;
        if (kv(c, "fullscreen", &v)) { *fs = !_strnicmp(v, "true", 4) || !_strnicmp(v, "1", 1) || !_strnicmp(v, "yes", 3); found = true; }
    });
    return found;
}

// the largest standard windowed size that fits; the same aspect as (w, h) first, any aspect second
static Res largest_window(const Mon& m, int w, int h) {
    Res best = {0, 0}, any = {0, 0};
    for (const Res& r : STD) {
        if (!windowed_std(r.w, r.h) || !fits_window(m, r.w, r.h)) continue;
        if ((long long)r.w * r.h > (long long)any.w * any.h) any = r;
        if (h > 0 && (long long)r.w * h == (long long)r.h * w && (long long)r.w * r.h > (long long)best.w * best.h) best = r;
    }
    if (best.w) return best;
    if (any.w) return any;
    return Res{1024, 768};
}

DMode validate(const DMode& d, const Mon& mon, const Res* adapter, int nadapter, bool fullscreen_ok) {
    DMode r = d;
    if (r.kind < 0 || r.kind > 2) r.kind = K_WINDOWED;
    if (r.kind == K_FULLSCREEN && !(fullscreen_ok && adapter_has(adapter, nadapter, r.w, r.h))) r.kind = K_BORDERLESS;
    if (r.kind == K_BORDERLESS && mon.mon_w > 0) { r.w = mon.mon_w; r.h = mon.mon_h; }
    if (r.kind == K_WINDOWED && (!sane(r.w, r.h) || !fits_window(mon, r.w, r.h))) { Res b = largest_window(mon, r.w, r.h); r.w = b.w; r.h = b.h; }
    return r;
}

DMode startup_default(bool cfg_fs, int cfg_w, int cfg_h, bool borderless_ini, const Mon& mon, const Res* adapter, int nadapter, bool fullscreen_ok) {
    DMode d;
    if (!sane(cfg_w, cfg_h)) { cfg_w = 1920; cfg_h = 1080; }
    if (!cfg_fs) d = DMode{cfg_w, cfg_h, K_WINDOWED};
    else if (borderless_ini) d = DMode{mon.mon_w, mon.mon_h, K_BORDERLESS};
    else d = DMode{cfg_w, cfg_h, K_FULLSCREEN};
    return validate(d, mon, adapter, nadapter, fullscreen_ok);
}

Res vanilla_window(const DMode& d, const Mon& mon, float dpi_scale) {
    if (!(dpi_scale >= 1.0f)) dpi_scale = 1.0f;
    Mon l = mon;   // the vanilla game is DPI unaware: everything in logical pixels
    l.mon_w = (int)(mon.mon_w / dpi_scale); l.mon_h = (int)(mon.mon_h / dpi_scale);
    l.work_w = (int)(mon.work_w / dpi_scale); l.work_h = (int)(mon.work_h / dpi_scale);
    l.frame_w = (int)(mon.frame_w / dpi_scale + 0.5f); l.frame_h = (int)(mon.frame_h / dpi_scale + 0.5f);
    if (d.kind == K_WINDOWED && fits_window(l, d.w, d.h)) return Res{d.w, d.h};
    Res best = {0, 0};
    for (const Res& r : STD)
        if (r.w * 9 == r.h * 16 && fits_window(l, r.w, r.h) && r.w > best.w) best = r;
    if (best.w) return best;
    return Res{800, 600};
}
int vanilla_index(const Res& win, const Mon& mon, float dpi_scale) {
    static const Res T[5] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}, {1600, 1200}};
    if (!(dpi_scale >= 1.0f)) dpi_scale = 1.0f;
    int lw = (int)(mon.work_w / dpi_scale), lh = (int)(mon.work_h / dpi_scale);
    int fw = (int)(mon.frame_w / dpi_scale + 0.5f), fh = (int)(mon.frame_h / dpi_scale + 0.5f);
    int best = 0;
    for (int i = 0; i < 5; ++i)
        if (T[i].h <= win.h && T[i].w + fw <= lw && T[i].h + fh <= lh) best = i;
    return best;
}

// replaces the value of "key = ..." lines (all of them) or inserts "key = value" + the file's line end at the top
static int set_key(const char* in, const char* key, const char* value, char* out, int cap) {
    const char* eolstr = strstr(in, "\r\n") ? "\r\n" : "\n";
    int o = 0; bool found = false;
    auto put = [&](const char* s, size_t n) { if (o + (int)n >= cap) { o = -1; return; } memcpy(out + o, s, n); o += (int)n; };
    const char* p = in;
    while (*p && o >= 0) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p + 1) : strlen(p);
        const char* c = p; while (c < p + len && (*c == ' ' || *c == '\t')) ++c;
        size_t k = strlen(key);
        const char* e = c + k;
        bool match = (size_t)(p + len - c) > k && !_strnicmp(c, key, k);
        if (match) { while (*e == ' ' || *e == '\t') ++e; match = *e == '='; }
        if (match) {
            found = true;
            put(p, (size_t)(c - p)); if (o < 0) break;
            char line[128]; int n = _snprintf_s(line, sizeof line, _TRUNCATE, "%s = %s", key, value);
            put(line, (size_t)n); if (o < 0) break;
            const char* end = p + len;                       // keep this line's own line end
            const char* le = end; while (le > c && (le[-1] == '\n' || le[-1] == '\r')) --le;
            put(le, (size_t)(end - le));
        } else put(p, len);
        p += len;
    }
    if (o < 0) return -1;
    if (!found) {   // at the top, like the game's own file
        char line[128]; int n = _snprintf_s(line, sizeof line, _TRUNCATE, "%s = %s%s", key, value, eolstr);
        if (o + n >= cap) return -1;
        memmove(out + n, out, (size_t)o); memcpy(out, line, (size_t)n); o += n;
    }
    out[o] = 0;
    return o;
}
int configpc_set(const char* in, bool fullscreen, int w, int h, char* out, int cap) {
    if (!in || !out || cap < 2) return -1;
    char* tmp = (char*)malloc((size_t)cap);
    if (!tmp) return -1;
    char sz[32]; _snprintf_s(sz, sizeof sz, _TRUNCATE, "%d,%d", w, h);
    int n = set_key(in, "screensize", sz, tmp, cap);
    if (n >= 0) n = set_key(tmp, "fullscreen", fullscreen ? "true" : "false", out, cap);
    free(tmp);
    return n;
}

float label_scale(int len, int max_chars) {
    if (len <= max_chars || len <= 0 || max_chars <= 0) return 1.0f;
    float s = (float)max_chars / (float)len;
    return s < 0.6f ? 0.6f : s;
}

}  // namespace gpd
