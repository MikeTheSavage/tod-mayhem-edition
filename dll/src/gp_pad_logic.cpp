// gp_pad_logic.cpp - pure logic of [pad] (see gp_pad_logic.h). Unit tests: tests/dll/test_pad.cpp.
#include "gp_pad_logic.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

namespace pad {

static bool ieq(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ------------------------------------------------------------------------------------------------ names
static const char* const BTN_NAMES[B_COUNT] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START",
                                               "UP", "DOWN", "LEFT", "RIGHT"};
struct Alias { const char* n; int b; };
static const Alias BTN_ALIAS[] = {
    {"CROSS", B_A}, {"CIRCLE", B_B}, {"SQUARE", B_X}, {"TRIANGLE", B_Y}, {"L1", B_LB}, {"R1", B_RB}, {"L2", B_LT},
    {"R2", B_RT}, {"L3", B_LS}, {"R3", B_RS}, {"SHARE", B_BACK}, {"CREATE", B_BACK}, {"VIEW", B_BACK}, {"SELECT", B_BACK},
    {"OPTIONS", B_START}, {"MENU", B_START}, {"DPAD_UP", B_UP}, {"DPAD_DOWN", B_DOWN}, {"DPAD_LEFT", B_LEFT},
    {"DPAD_RIGHT", B_RIGHT}, {"DUP", B_UP}, {"DDOWN", B_DOWN}, {"DLEFT", B_LEFT}, {"DRIGHT", B_RIGHT},
    {"LSTICK", G_LSTICK}, {"RSTICK", G_RSTICK}};
int btn_from_name(const char* s) {
    if (!s) return -1;
    for (int i = 0; i < B_COUNT; ++i) if (ieq(s, BTN_NAMES[i])) return i;
    for (const Alias& a : BTN_ALIAS) if (ieq(s, a.n)) return a.b;
    return -1;
}
const char* btn_name(int b) {
    if (b >= 0 && b < B_COUNT) return BTN_NAMES[b];
    if (b == G_LSTICK) return "LSTICK";
    if (b == G_RSTICK) return "RSTICK";
    return "?";
}
static const char* const VIRT_NAMES[V_COUNT] = {"SELECT", "L3", "R3", "START", "D_UP", "D_RIGHT", "D_DOWN", "D_LEFT",
                                                "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE"};
int virt_from_name(const char* s) {
    for (int i = 0; i < V_COUNT; ++i) if (ieq(s, VIRT_NAMES[i])) return i;
    return -1;
}
const char* virt_name(int v) { return v >= 0 && v < V_COUNT ? VIRT_NAMES[v] : "?"; }

// ------------------------------------------------------------------------------------------------ XInput
State from_xinput(uint16_t w, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    static const struct { uint16_t bit; int b; } T[] = {
        {0x0001, B_UP}, {0x0002, B_DOWN}, {0x0004, B_LEFT}, {0x0008, B_RIGHT}, {0x0010, B_START}, {0x0020, B_BACK},
        {0x0040, B_LS}, {0x0080, B_RS}, {0x0100, B_LB}, {0x0200, B_RB}, {0x1000, B_A}, {0x2000, B_B}, {0x4000, B_X},
        {0x8000, B_Y}};
    State s;
    for (auto& t : T) if (w & t.bit) s.buttons |= 1u << t.b;
    s.lt = lt / 255.0f; s.rt = rt / 255.0f;
    s.lx = clampf(lx / 32767.0f, -1, 1); s.ly = clampf(ly / 32767.0f, -1, 1);
    s.rx = clampf(rx / 32767.0f, -1, 1); s.ry = clampf(ry / 32767.0f, -1, 1);
    s.kind = KIND_XBOX; s.connected = true;
    return s;
}

// ------------------------------------------------------------------------------------------------ HID
HidModel hid_model(uint16_t vid, uint16_t pid) {
    if (vid != 0x054C) return HID_NONE;
    switch (pid) {
    case 0x05C4: case 0x09CC: case 0x0BA0: return HID_DS4;
    case 0x0CE6: case 0x0DF2: return HID_DS;
    default: return HID_NONE;
    }
}
uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static bool bt_sum_ok(const uint8_t* r, size_t n) {   // CRC-32 of A1 + the report without its last 4 bytes
    if (n < 5) return false;
    uint8_t h = 0xA1;
    uint32_t c = crc32(r, n - 4, crc32(&h, 1));
    return r[n - 4] == (c & 0xFF) && r[n - 3] == ((c >> 8) & 0xFF) && r[n - 2] == ((c >> 16) & 0xFF) && r[n - 1] == (c >> 24);
}
static float stick8(uint8_t v) { float f = (v - 128.0f) / 127.0f; return clampf(f, -1, 1); }
// sticks at s, buttons at b (hat + faces, then shoulders, then PS / pad), triggers at t
static void ps_fields(const uint8_t* r, size_t s, size_t b, size_t t, State& o) {
    State n;
    n.lx = stick8(r[s]); n.ly = -stick8(r[s + 1]); n.rx = stick8(r[s + 2]); n.ry = -stick8(r[s + 3]);
    uint8_t hat = r[b] & 0x0F;
    static const uint8_t HAT[8] = {1, 1 | 8, 8, 2 | 8, 2, 2 | 4, 4, 1 | 4};   // up, up-right, right, ... (1 up 2 down 4 left 8 right)
    if (hat < 8) {
        if (HAT[hat] & 1) n.buttons |= 1u << B_UP;
        if (HAT[hat] & 2) n.buttons |= 1u << B_DOWN;
        if (HAT[hat] & 4) n.buttons |= 1u << B_LEFT;
        if (HAT[hat] & 8) n.buttons |= 1u << B_RIGHT;
    }
    if (r[b] & 0x10) n.buttons |= 1u << B_X;      // Square
    if (r[b] & 0x20) n.buttons |= 1u << B_A;      // Cross
    if (r[b] & 0x40) n.buttons |= 1u << B_B;      // Circle
    if (r[b] & 0x80) n.buttons |= 1u << B_Y;      // Triangle
    uint8_t s2 = r[b + 1];
    if (s2 & 0x01) n.buttons |= 1u << B_LB;       // L1
    if (s2 & 0x02) n.buttons |= 1u << B_RB;       // R1
    if (s2 & 0x10) n.buttons |= 1u << B_BACK;     // Share / Create
    if (s2 & 0x20) n.buttons |= 1u << B_START;    // Options
    if (s2 & 0x40) n.buttons |= 1u << B_LS;       // L3
    if (s2 & 0x80) n.buttons |= 1u << B_RS;       // R3
    n.lt = r[t] / 255.0f; n.rt = r[t + 1] / 255.0f;   // L2 / R2 analog (the digital bits 4 / 8 follow them)
    n.kind = KIND_PS; n.connected = true;
    o = n;
}
bool hid_decode(HidModel m, bool bt, const uint8_t* r, size_t n, State& out) {
    if (!r || n < 1) return false;
    uint8_t id = r[0];
    if (m == HID_DS4) {
        if (id == 0x01 && n >= 10) { ps_fields(r, 1, 5, 8, out); return true; }
        if (id == 0x11 && n >= 78 && bt_sum_ok(r, 78)) { ps_fields(r, 3, 7, 10, out); return true; }
    } else if (m == HID_DS) {
        if (id == 0x01 && !bt && n >= 11) { ps_fields(r, 1, 8, 5, out); return true; }
        if (id == 0x01 && bt && n >= 10) { ps_fields(r, 1, 5, 8, out); return true; }
        if (id == 0x31 && n >= 78 && bt_sum_ok(r, 78)) { ps_fields(r, 2, 9, 6, out); return true; }
    }
    return false;
}
size_t hid_rumble_report(HidModel m, bool bt, uint8_t big, uint8_t little, uint8_t* o, size_t cap) {
    if (bt) return 0;
    if (m == HID_DS4 && cap >= 32) { memset(o, 0, 32); o[0] = 0x05; o[1] = 0x01; o[4] = little; o[5] = big; return 32; }
    if (m == HID_DS && cap >= 48) { memset(o, 0, 48); o[0] = 0x02; o[1] = 0x03; o[3] = little; o[4] = big; return 48; }
    return 0;
}

// ------------------------------------------------------------------------------------------------ shaping
void radial(float& x, float& y, float dz) {
    float m = sqrtf(x * x + y * y);
    if (m <= dz || m <= 1e-6f) { x = y = 0; return; }
    float k = (m - dz) / (1.0f - dz);
    if (k > 1.0f) k = 1.0f;
    x = x / m * k; y = y / m * k;
}
float lift(float v) {
    if (v == 0.0f) return 0.0f;
    float a = fabsf(v); if (a > 1.0f) a = 1.0f;
    float r = 0.3f + 0.7f * a;
    return v < 0 ? -r : r;
}
float look_axis(float v, float exp, float sens) {
    if (v == 0.0f) return 0.0f;
    float a = fabsf(v); if (a > 1.0f) a = 1.0f;
    if (exp < 0.5f) exp = 0.5f;
    float r = powf(a, exp / 3.0f) * cbrtf(sens > 0 ? sens : 0.0f);
    if (r > 1.0f) r = 1.0f;
    return v < 0 ? -r : r;
}

// ------------------------------------------------------------------------------------------------ the game's pad
Map default_map() {
    Map m;
    m.src[V_SELECT] = B_BACK; m.src[V_L3] = B_LS; m.src[V_R3] = B_RS; m.src[V_START] = B_START;
    m.src[V_DUP] = B_UP; m.src[V_DRIGHT] = B_RIGHT; m.src[V_DDOWN] = B_DOWN; m.src[V_DLEFT] = B_LEFT;
    m.src[V_L2] = B_LB; m.src[V_R2] = B_RB; m.src[V_L1] = B_LT; m.src[V_R1] = B_RT;
    m.src[V_TRIANGLE] = B_Y; m.src[V_CIRCLE] = B_B; m.src[V_CROSS] = B_A; m.src[V_SQUARE] = B_X;
    return m;
}
bool map_set(Map& m, const char* virt, const char* btn) {
    int v = virt_from_name(virt);
    if (v < 0) return false;
    if (ieq(btn, "NONE")) { m.src[v] = -1; return true; }
    int b = btn_from_name(btn);
    if (b < 0 || b >= B_COUNT) return false;
    m.src[v] = (int8_t)b;
    return true;
}
int map_virt_of(const Map& m, int btn) {
    for (int v = 0; v < V_COUNT; ++v) if (m.src[v] == btn) return v;
    return -1;
}
int32_t axis_to_di(float v) {
    v = clampf(v, -1, 1);
    int32_t r = (int32_t)lroundf((v + 1.0f) * 32768.0f);
    return r < 0 ? 0 : (r > 65535 ? 65535 : r);
}
void to_dijoystate(const State& s, const Map& m, const Shape& sh, DiState& o) {
    memset(&o, 0, sizeof o);
    o.pov[0] = o.pov[1] = o.pov[2] = o.pov[3] = 0xFFFFFFFFu;
    float lx = s.lx, ly = s.ly, rx = s.rx, ry = s.ry;
    if (s.connected) {
        radial(lx, ly, sh.dz_left);
        radial(rx, ry, sh.dz_right);
        if (sh.compensate) { lx = lift(lx); ly = lift(ly); }
        rx = look_axis(rx, sh.look_exp, sh.look_sens_x);
        ry = look_axis(ry, sh.look_exp, sh.look_sens_y);
        if (sh.compensate) { rx = lift(rx); ry = lift(ry); }
        if (sh.invert_x) rx = -rx;
        if (sh.invert_y) ry = -ry;
    } else lx = ly = rx = ry = 0;
    o.lX = axis_to_di(lx); o.lY = axis_to_di(-ly);
    o.lZ = axis_to_di(rx); o.lRz = axis_to_di(-ry);
    o.lRx = o.lRy = 32768;
    if (!s.connected) return;
    uint32_t b = s.buttons;
    if (s.lt > sh.trigger) b |= 1u << B_LT;
    if (s.rt > sh.trigger) b |= 1u << B_RT;
    for (int v = 0; v < V_COUNT; ++v) if (m.src[v] >= 0 && ((b >> m.src[v]) & 1u)) o.btn[v] = 0x80;
}

// ------------------------------------------------------------------------------------------------ device used last
bool pad_activity(const State& s, uint32_t prev) {
    if (!s.connected) return false;
    if (s.buttons & ~prev) return true;
    if (s.lx * s.lx + s.ly * s.ly > 0.25f || s.rx * s.rx + s.ry * s.ry > 0.25f) return true;
    return s.lt > 0.5f || s.rt > 0.5f;
}
bool Activity::step(const State& s, bool kbm, double now) {
    bool p = pad_activity(s, prev_buttons);
    prev_buttons = s.connected ? s.buttons : 0;
    if (p && s.kind != KIND_NONE) last_kind = s.kind;
    int want = active;
    if (p && !kbm) want = 1;
    else if (kbm && !p) want = 0;
    if (want != active) { active = want; t_switch = now; return true; }
    return false;
}

// ------------------------------------------------------------------------------------------------ prompts
int mode_from_name(const char* s) {
    if (!s || !*s || ieq(s, "auto")) return MODE_AUTO;
    if (ieq(s, "keyboard") || ieq(s, "pc") || ieq(s, "off")) return MODE_KEYBOARD;
    if (ieq(s, "xbox")) return MODE_XBOX;
    if (ieq(s, "playstation") || ieq(s, "ps") || ieq(s, "sony")) return MODE_PS;
    return -1;
}
Style resolve_style(int mode, int pad_active, int pad_kind) {
    switch (mode) {
    case MODE_KEYBOARD: return STYLE_KEYBOARD;
    case MODE_XBOX: return STYLE_XBOX;
    case MODE_PS: return STYLE_PS;
    default: return !pad_active ? STYLE_KEYBOARD : (pad_kind == KIND_PS ? STYLE_PS : STYLE_XBOX);
    }
}
static const uint16_t CODES[G_COUNT] = {
    0x250C, 0x2510, 0x2518, 0x2514,   // A B X Y          (console: cross circle square triangle)
    0x215B, 0x215C, 0x215E, 0x215D,   // LB RB LT RT      (L1 R1 L2 R2 labels)
    0x25D8, 0x25D9,                   // LS RS (clicks)   (console sticks)
    0x2154, 0x2153,                   // BACK START
    0x2191, 0x2193, 0x2190, 0x2192,   // d-pad up down left right
    0x2020, 0x2021};                  // left stick, right stick (move / look)
uint16_t glyph_code(int g) { return g >= 0 && g < G_COUNT ? CODES[g] : 0; }
int glyph_of_code(uint16_t c) {
    for (int g = 0; g < G_COUNT; ++g) if (CODES[g] == c) return g;
    return -1;
}
static size_t u16len(const uint16_t* s, size_t cap) { size_t n = 0; while (n < cap && s[n]) ++n; return n; }
static bool match_lit(const uint16_t* s, size_t n, const char* lit, size_t* len) {
    size_t k = 0;
    for (; lit[k]; ++k) {
        if (k >= n) return false;
        uint16_t a = s[k], b = (uint8_t)lit[k];
        if (a == b) continue;
        // case-insensitive for ASCII and Latin-1 letters
        uint16_t la = (a >= 'A' && a <= 'Z') || (a >= 0xC0 && a <= 0xDE && a != 0xD7) ? a + 32 : a;
        uint16_t lb = (b >= 'A' && b <= 'Z') || (b >= 0xC0 && b <= 0xDE && b != 0xD7) ? b + 32 : b;
        if (la != lb) return false;
    }
    *len = k;
    return true;
}
size_t post_process(uint16_t* s, size_t cap, const Literal* lit, int nlit) {
    if (!s || !cap) return 0;
    size_t n = u16len(s, cap);
    if (n >= cap) { s[cap - 1] = 0; n = cap - 1; }
    // 1. literal bracketed key names -> one glyph
    for (size_t i = 0; i < n; ++i) {
        if (s[i] != '[') continue;
        for (int k = 0; k < nlit; ++k) {
            size_t len = 0;
            if (match_lit(s + i, n - i, lit[k].text, &len) && len >= 2) {
                s[i] = glyph_code(lit[k].glyph);
                memmove(s + i + 1, s + i + len, (n - (i + len) + 1) * sizeof(uint16_t));
                n -= len - 1;
                break;
            }
        }
    }
    // 2. brackets around glyph-only groups
    for (size_t i = 0; i < n; ++i) {
        if (s[i] != '[') continue;
        size_t j = i + 1; int glyphs = 0; bool other = false;
        for (; j < n && s[j] != ']'; ++j) {
            if (glyph_of_code(s[j]) >= 0) ++glyphs;
            else if (s[j] != ' ') { other = true; break; }
        }
        if (other || j >= n || glyphs == 0) continue;
        // drop s[j] (']') then s[i] ('['); trim the inner spaces next to the brackets
        memmove(s + j, s + j + 1, (n - j) * sizeof(uint16_t)); --n;
        memmove(s + i, s + i + 1, (n - i) * sizeof(uint16_t)); --n;
    }
    return n;
}

// ------------------------------------------------------------------------------------------------ rumble
void Rumble::pulse(float big, float little, float seconds) {
    float left = ev_len > 0 ? (1.0f - ev_t / ev_len) : 0.0f;
    float cur = (ev_big > ev_small ? ev_big : ev_small) * (left > 0 ? left : 0);
    float neu = big > little ? big : little;
    if (neu >= cur || ev_t >= ev_len) { ev_big = clampf(big, 0, 1); ev_small = clampf(little, 0, 1); ev_t = 0; ev_len = seconds > 0.01f ? seconds : 0.01f; }
}
void Rumble::step(float dt) {
    if (ev_len <= 0) return;
    ev_t += dt;
    if (ev_t >= ev_len) { ev_big = ev_small = 0; ev_t = ev_len = 0; }
}
void Rumble::out(float strength, bool enabled, float& big, float& little) const {
    big = little = 0;
    if (!enabled) return;
    float k = ev_len > 0 ? 1.0f - 0.5f * (ev_t / ev_len) : 0.0f;   // events fade to half, then stop
    float eb = ev_big * k, es = ev_small * k;
    big = clampf((game[0] > eb ? game[0] : eb) * strength, 0, 1);
    little = clampf((game[1] > es ? game[1] : es) * strength, 0, 1);
}

// ------------------------------------------------------------------------------------------------ pad script
static const char* word(const char*& p, char* out, size_t cap) {
    while (*p == ' ' || *p == '\t') ++p;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') { if (n + 1 < cap) out[n++] = *p; ++p; }
    out[n] = 0;
    return out;
}
bool PadScript::parse(const char* text) {
    n = 0; errors = 0;
    const char* p = text;
    while (p && *p) {
        const char* eol = p; while (*eol && *eol != '\n') ++eol;
        char line[160]; size_t L = (size_t)(eol - p); if (L >= sizeof line) L = sizeof line - 1;
        memcpy(line, p, L); line[L] = 0;
        p = *eol ? eol + 1 : eol;
        for (char* c = line; *c; ++c) if (*c == '\r' || *c == '#') { *c = 0; break; }
        const char* q = line; char w[48];
        word(q, w, sizeof w);
        if (!w[0]) continue;
        if (n >= CAP) { ++errors; break; }
        PadCmd c; memset(&c, 0, sizeof c);
        char a1[48], a2[48], a3[48], a4[48];
        if (ieq(w, "wait")) { c.op = PadCmd::WAIT; c.ms = atoi(word(q, a1, sizeof a1)); }
        else if (ieq(w, "waitlog")) {
            c.op = PadCmd::WAITLOG; c.ms = atoi(word(q, a1, sizeof a1));
            while (*q == ' ') ++q;
            strncpy_s(c.text, q, _TRUNCATE);
        } else if (ieq(w, "mark")) { c.op = PadCmd::MARK; while (*q == ' ') ++q; strncpy_s(c.text, q, _TRUNCATE); }
        else if (ieq(w, "press") || ieq(w, "hold") || ieq(w, "release")) {
            c.op = ieq(w, "press") ? PadCmd::PRESS : (ieq(w, "hold") ? PadCmd::HOLD : PadCmd::RELEASE);
            c.a = btn_from_name(word(q, a1, sizeof a1));
            word(q, a2, sizeof a2); c.ms = a2[0] ? atoi(a2) : 120;
            if (c.a < 0 || c.a >= B_COUNT) { ++errors; continue; }
        } else if (ieq(w, "stick")) {
            c.op = PadCmd::STICK; word(q, a1, sizeof a1); c.a = ieq(a1, "R") ? 1 : (ieq(a1, "L") ? 0 : -1);
            c.x = (float)atof(word(q, a2, sizeof a2)); c.y = (float)atof(word(q, a3, sizeof a3));
            word(q, a4, sizeof a4); c.ms = a4[0] ? atoi(a4) : 0;
            if (c.a < 0) { ++errors; continue; }
        } else if (ieq(w, "trigger")) {
            c.op = PadCmd::TRIGGER; word(q, a1, sizeof a1); c.a = ieq(a1, "R") ? 1 : (ieq(a1, "L") ? 0 : -1);
            c.x = (float)atof(word(q, a2, sizeof a2)); word(q, a3, sizeof a3); c.ms = a3[0] ? atoi(a3) : 0;
            if (c.a < 0) { ++errors; continue; }
        } else if (ieq(w, "kind")) {
            c.op = PadCmd::KIND; word(q, a1, sizeof a1);
            c.a = (ieq(a1, "playstation") || ieq(a1, "ps")) ? KIND_PS : (ieq(a1, "xbox") ? KIND_XBOX : -1);
            if (c.a < 0) { ++errors; continue; }
        } else if (ieq(w, "connect")) c.op = PadCmd::CONNECT;
        else if (ieq(w, "disconnect")) c.op = PadCmd::DISCONNECT;
        else if (ieq(w, "rumble_test")) {
            c.op = PadCmd::RUMBLE; c.x = (float)atof(word(q, a1, sizeof a1)); c.y = (float)atof(word(q, a2, sizeof a2));
            c.ms = atoi(word(q, a3, sizeof a3));
        } else { ++errors; continue; }
        this->c[n++] = c;
    }
    return errors == 0;
}
// timed returns: t_rel[0..B_COUNT-1] buttons, [B_COUNT] left stick, +1 right stick, +2 left trigger, +3 right trigger
void PadPlayer::step(const PadScript& s, double now, LogHas has, void* ctx) {
    mark = nullptr;
    for (int i = 0; i < B_COUNT + 4; ++i) {
        if (t_rel[i] <= 0.0 || now < t_rel[i]) continue;
        t_rel[i] = 0.0;
        if (i < B_COUNT) st.buttons &= ~(1u << i);
        else if (i == B_COUNT) st.lx = st.ly = 0;
        else if (i == B_COUNT + 1) st.rx = st.ry = 0;
        else if (i == B_COUNT + 2) st.lt = 0;
        else st.rt = 0;
    }
    if (done) return;
    if (waiting >= 0) {
        if (now >= t_wait_end || (has && has(s.c[waiting].text, ctx))) { waiting = -1; t_next = now; }
        else return;
    }
    if (t_next < 0) t_next = now;
    // run every command that is due (at most 16 per frame, a mark ends the frame so each one is seen)
    for (int k = 0; k < 16 && !done && now >= t_next; ++k) {
        if (pc >= s.n) { done = true; break; }
        const PadCmd& c = s.c[pc++];
        switch (c.op) {
        case PadCmd::WAIT: t_next = now + c.ms / 1000.0; break;
        case PadCmd::WAITLOG: waiting = pc - 1; t_wait_end = now + c.ms / 1000.0; return;
        case PadCmd::MARK: mark = c.text; return;
        case PadCmd::PRESS: st.connected = true; st.buttons |= 1u << c.a; t_rel[c.a] = now + c.ms / 1000.0; break;
        case PadCmd::HOLD: st.connected = true; st.buttons |= 1u << c.a; t_rel[c.a] = 0.0; break;
        case PadCmd::RELEASE: st.buttons &= ~(1u << c.a); t_rel[c.a] = 0.0; break;
        case PadCmd::STICK:
            st.connected = true;
            if (c.a == 0) { st.lx = c.x; st.ly = c.y; } else { st.rx = c.x; st.ry = c.y; }
            t_rel[B_COUNT + c.a] = c.ms > 0 ? now + c.ms / 1000.0 : 0.0;
            break;
        case PadCmd::TRIGGER:
            st.connected = true;
            if (c.a == 0) st.lt = c.x; else st.rt = c.x;
            t_rel[B_COUNT + 2 + c.a] = c.ms > 0 ? now + c.ms / 1000.0 : 0.0;
            break;
        case PadCmd::KIND: st.kind = c.a; break;
        case PadCmd::CONNECT: st.connected = true; break;
        case PadCmd::DISCONNECT: st = State(); st.kind = KIND_XBOX; for (double& t : t_rel) t = 0.0; break;
        case PadCmd::RUMBLE: rumble[0] = c.x; rumble[1] = c.y; rumble[2] = c.ms / 1000.0f; break;
        }
    }
}

}  // namespace pad
