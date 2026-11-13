// gp_logic.cpp - pure logic of TODGameplay.dll (see gp_logic.h). No game memory, no Windows calls.
#include "gp_logic.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

namespace gp {

// ================================================================================================ ini
static void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') ++i;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}
static void copyz(char* d, size_t cap, const char* s, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(d, s, n); d[n] = 0;
}
void Ini::parse(const char* text) {
    n = 0;
    char section[32] = "";
    const char* p = text;
    while (p && *p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512]; copyz(line, sizeof line, p, len);
        p = eol ? eol + 1 : nullptr;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            char* e = strchr(line, ']');
            if (e) { *e = 0; copyz(section, sizeof section, line + 1, strlen(line + 1)); trim(section); }
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq || n >= CAP) continue;
        *eq = 0;
        char* val = eq + 1;
        for (char* c = val; *c; ++c) if (*c == ';' || *c == '#') { *c = 0; break; }   // trailing comment
        IniEntry& en = e[n];
        copyz(en.section, sizeof en.section, section, strlen(section));
        copyz(en.key, sizeof en.key, line, strlen(line)); trim(en.key);
        copyz(en.value, sizeof en.value, val, strlen(val)); trim(en.value);
        if (en.key[0]) ++n;
    }
}
const char* Ini::get(const char* section, const char* key) const {
    for (int i = n - 1; i >= 0; --i)   // last one wins
        if (!_stricmp(e[i].section, section) && !_stricmp(e[i].key, key)) return e[i].value;
    return nullptr;
}
int Ini::geti(const char* section, const char* key, int def) const {
    const char* v = get(section, key);
    if (!v || !*v) return def;
    if (!_stricmp(v, "true") || !_stricmp(v, "on") || !_stricmp(v, "yes")) return 1;
    if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return 0;
    char* end = nullptr; long r = strtol(v, &end, 0);
    return end == v ? def : (int)r;
}
float Ini::getf(const char* section, const char* key, float def) const {
    const char* v = get(section, key);
    if (!v || !*v) return def;
    char* end = nullptr; double r = strtod(v, &end);
    return end == v ? def : (float)r;
}

// ================================================================================================ keys
struct KeyName { const char* n; int k; };
static const KeyName kKeys[] = {
    {"ESCAPE", 0x01}, {"ESC", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07},
    {"7", 0x08}, {"8", 0x09}, {"9", 0x0A}, {"0", 0x0B}, {"MINUS", 0x0C}, {"EQUALS", 0x0D}, {"BACK", 0x0E},
    {"BACKSPACE", 0x0E}, {"TAB", 0x0F}, {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14}, {"Y", 0x15},
    {"U", 0x16}, {"I", 0x17}, {"O", 0x18}, {"P", 0x19}, {"LBRACKET", 0x1A}, {"RBRACKET", 0x1B}, {"RETURN", 0x1C},
    {"ENTER", 0x1C}, {"LCONTROL", 0x1D}, {"LCTRL", 0x1D}, {"A", 0x1E}, {"S", 0x1F}, {"D", 0x20}, {"F", 0x21},
    {"G", 0x22}, {"H", 0x23}, {"J", 0x24}, {"K", 0x25}, {"L", 0x26}, {"SEMICOLON", 0x27}, {"APOSTROPHE", 0x28},
    {"GRAVE", 0x29}, {"LSHIFT", 0x2A}, {"BACKSLASH", 0x2B}, {"Z", 0x2C}, {"X", 0x2D}, {"C", 0x2E}, {"V", 0x2F},
    {"B", 0x30}, {"N", 0x31}, {"M", 0x32}, {"COMMA", 0x33}, {"PERIOD", 0x34}, {"SLASH", 0x35}, {"RSHIFT", 0x36},
    {"MULTIPLY", 0x37}, {"LMENU", 0x38}, {"LALT", 0x38}, {"SPACE", 0x39}, {"CAPITAL", 0x3A}, {"CAPSLOCK", 0x3A},
    {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42},
    {"F9", 0x43}, {"F10", 0x44}, {"NUMLOCK", 0x45}, {"SCROLL", 0x46}, {"NUMPAD7", 0x47}, {"NUMPAD8", 0x48},
    {"NUMPAD9", 0x49}, {"SUBTRACT", 0x4A}, {"NUMPAD4", 0x4B}, {"NUMPAD5", 0x4C}, {"NUMPAD6", 0x4D}, {"ADD", 0x4E},
    {"NUMPAD1", 0x4F}, {"NUMPAD2", 0x50}, {"NUMPAD3", 0x51}, {"NUMPAD0", 0x52}, {"DECIMAL", 0x53}, {"F11", 0x57},
    {"F12", 0x58}, {"NUMPADENTER", 0x9C}, {"RCONTROL", 0x9D}, {"RCTRL", 0x9D}, {"DIVIDE", 0xB5}, {"RMENU", 0xB8},
    {"RALT", 0xB8}, {"PAUSE", 0xC5}, {"HOME", 0xC7}, {"UP", 0xC8}, {"PRIOR", 0xC9}, {"PGUP", 0xC9}, {"LEFT", 0xCB},
    {"RIGHT", 0xCD}, {"END", 0xCF}, {"DOWN", 0xD0}, {"NEXT", 0xD1}, {"PGDN", 0xD1}, {"INSERT", 0xD2},
    {"DELETE", 0xD3},
    {"MOUSE1", KEY_MOUSE + 0}, {"MOUSE2", KEY_MOUSE + 1}, {"MOUSE3", KEY_MOUSE + 2}, {"MOUSE4", KEY_MOUSE + 3},
    {"MOUSE5", KEY_MOUSE + 4},
    // XInput wButtons bits
    {"PAD_UP", KEY_PAD + 0}, {"PAD_DOWN", KEY_PAD + 1}, {"PAD_LEFT", KEY_PAD + 2}, {"PAD_RIGHT", KEY_PAD + 3},
    {"PAD_START", KEY_PAD + 4}, {"PAD_BACK", KEY_PAD + 5}, {"PAD_LS", KEY_PAD + 6}, {"PAD_RS", KEY_PAD + 7},
    {"PAD_LB", KEY_PAD + 8}, {"PAD_RB", KEY_PAD + 9}, {"PAD_A", KEY_PAD + 12}, {"PAD_B", KEY_PAD + 13},
    {"PAD_X", KEY_PAD + 14}, {"PAD_Y", KEY_PAD + 15},
};
int key_from_name(const char* name) {
    if (!name) return KEY_NONE;
    char s[32]; size_t i = 0;
    while (name[i] == ' ') ++name;
    for (; name[i] && i < sizeof s - 1; ++i) s[i] = (char)toupper((unsigned char)name[i]);
    s[i] = 0;
    while (i && s[i - 1] == ' ') s[--i] = 0;
    if (!s[0] || !strcmp(s, "NONE") || !strcmp(s, "-1")) return KEY_NONE;
    if (!strncmp(s, "DIK_", 4)) memmove(s, s + 4, strlen(s + 4) + 1);
    for (const KeyName& k : kKeys) if (!strcmp(s, k.n)) return k.k;
    if (s[0] == '0' && s[1] == 'X') { long v = strtol(s, nullptr, 16); if (v > 0 && v < 256) return (int)v; }
    return -2;   // unknown name
}
const char* key_name(int code) {
    if (code == KEY_NONE) return "NONE";
    for (const KeyName& k : kKeys) if (k.k == code) return k.n;
    static char buf[16]; sprintf_s(buf, "0x%X", code); return buf;
}
struct ActionKey { const char* a; int dik; };
static const ActionKey kActions[] = {   // PC keyboard defaults (ControlDict PC/Keyboard)
    {"run", 0x11}, {"run_backwards", 0x1F}, {"strafe_left", 0x1E}, {"strafe_right", 0x20}, {"jump", 0x2A},
    {"crouch", 0x2E}, {"shootdodge", 0x39}, {"use", 0x21}, {"reload", 0x13}, {"use_powerup", 0x1D},
    {"rewind", 0x0F}, {"powerup_left", 0x10}, {"powerup_right", 0x12}, {"powerup_menu", 0x19},
    {"vehicle_brake", 0x2F}, {"vehicle_siren", 0x23}, {"vehicle_exit_stunt_prepare", 0x2E},
    {"vehicle_exit_stunt", 0x2D},
};
int action_default_key(const char* action) {
    for (const ActionKey& a : kActions) if (!_stricmp(a.a, action)) return a.dik;
    return -1;
}

void KeyRemap::clear() { for (int i = 0; i < 256; ++i) src[i] = -2; any = false; }
bool KeyRemap::bind(int g, int s) {
    if (g < 0 || g > 255) return false;
    if (s >= KEY_PAD || s < KEY_NONE || (s >= 256 && s < KEY_MOUSE) || s >= KEY_MOUSE + 8) return false;
    src[g] = (int16_t)s; any = true; return true;
}
void KeyRemap::apply(uint8_t edges[256], uint8_t state[256], const bool md[8], const bool mp[8]) const {
    if (!any) return;
    uint8_t ie[256], is[256];
    memcpy(ie, edges, 256); memcpy(is, state, 256);
    // physical keys moved away from their own game key (and not rebound themselves) lose their old function
    for (int g = 0; g < 256; ++g) {
        int s = src[g];
        if (s >= 0 && s < 256 && s != g && src[s] == -2) { edges[s] = 0; state[s] = 0; }
    }
    for (int g = 0; g < 256; ++g) {
        int s = src[g];
        if (s == -2) continue;
        if (s == KEY_NONE) { edges[g] = 0; state[g] = 0; }
        else if (s < 256) { edges[g] = ie[s]; state[g] = is[s]; }
        else {
            int b = s - KEY_MOUSE;
            bool d = md[b], p = mp[b];
            state[g] = d ? 0x80 : 0;
            edges[g] = (uint8_t)((d && !p ? 1 : 0) | (!d && p ? 2 : 0));
        }
    }
}

// ================================================================================================ meter
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
void Meter::drain(const MeterCfg& c, float dt) {
    if (c.capacity_s > 0.0f) value = clamp01(value - dt / c.capacity_s);
    since_use = 0.0f;
}
void Meter::idle(const MeterCfg& c, float dt) {
    since_use += dt;
    if (since_use >= c.regen_delay_s) value = clamp01(value + c.regen_per_s * dt);
}
void Meter::kills(const MeterCfg& c, int n) { if (n > 0) value = clamp01(value + c.per_kill * (float)n); }
void Meter::points(const MeterCfg& c, int p) { if (p > 0) value = clamp01(value + c.per_point * (float)p); }

// ================================================================================================ bullet time
static bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

BtActions BtLogic::tick(const BtCfg& c, const MeterCfg& mc, Meter& m, const BtGame& g, bool want, float dt) {
    BtActions a;
    if (!c.enabled || !g.ok) return a;
    t += dt;
    switch (state) {
    case BT_OFF:
        if (want && m.can_start(mc) && !g.exclusive && !g.cutscene && near(g.world_time, 1.0f, 1e-4f)) {
            a.send_slowmo = true; a.slowmo_target = c.scale;
            a.slowmo_seconds = c.ramp_in > 0.01f ? c.ramp_in : 0.01f; a.slowmo_no_blur = c.blur ? 0 : 1;
            state = BT_RAMP_IN; t = 0.0f; retried = false;
        }
        break;
    case BT_RAMP_IN:
        m.drain(mc, dt);
        if (!near(g.world_time, 1.0f, 1e-4f)) { state = BT_OFF; t = 0.0f; ++ended_by_game; break; }   // the game took over
        // the ramp is never cut short (a trigger sent during the controller's interpolation would be ignored);
        // latch WorldTime when the controller has arrived, or after a timeout when it was busy (dive, explosion)
        if (g.time_to_use <= 0.0f && near(g.time_mult, c.scale, 0.02f)) {
            a.set_world_time = true; a.world_time = c.scale;
            state = BT_ACTIVE; t = 0.0f;
        } else if (t >= c.ramp_in + 0.3f) {
            if (!retried && g.time_to_use <= 0.0f && !near(g.time_mult, c.scale, 0.02f)) {
                // the trigger was ignored (the controller was still ramping, e.g. a quick re-toggle): send it once more
                a.send_slowmo = true; a.slowmo_target = c.scale;
                a.slowmo_seconds = c.ramp_in > 0.01f ? c.ramp_in : 0.01f; a.slowmo_no_blur = c.blur ? 0 : 1;
                retried = true; t = 0.0f;
            } else {   // busy (dive slow motion, explosion): latch anyway, the game's own "back to normal" returns to it
                a.set_world_time = true; a.world_time = c.scale;
                state = BT_ACTIVE; t = 0.0f;
            }
        }
        break;
    case BT_ACTIVE:
        if (!near(g.world_time, c.scale, 1e-4f)) { state = BT_OFF; t = 0.0f; ++ended_by_game; break; }  // death, vehicle, mission start...
        m.drain(mc, dt);
        if (!want || m.empty()) {
            a.set_world_time = true; a.world_time = 1.0f;
            a.send_slowmo = true; a.slowmo_target = 1.0f;
            a.slowmo_seconds = c.ramp_out > 0.01f ? c.ramp_out : 0.01f; a.slowmo_no_blur = 1;
            state = BT_OFF; t = 0.0f;
        }
        break;
    }
    return a;
}
BtActions BtLogic::abort_check(const BtGame& g, bool cutscene, bool stale, bool paused) {
    BtActions a;
    if (state == BT_OFF) return a;
    if (cutscene || (stale && !paused && !g.exclusive)) {
        if (state == BT_ACTIVE) { a.set_world_time = true; a.world_time = 1.0f; }
        a.send_force_normal = true;
        state = BT_OFF; t = 0.0f; ++ended_by_game;
    }
    return a;
}

// ================================================================================================ mouse
void YawAcc::on_write(float turn, uint32_t now, uint32_t drop_after) {
    if (now - last_consume_ms > drop_after) acc = 0.0f;   // not consumed for too long (cut-scene, auto turn...)
    mouse_mode = true;
    acc += turn * seconds_per_frame;
    if (acc > 180.0f) acc = 180.0f; else if (acc < -180.0f) acc = -180.0f;
}
void YawAcc::on_pad() { mouse_mode = false; acc = 0.0f; }
float YawAcc::on_consume(float timestep, uint32_t now) {
    if (timestep <= 0.0f) return 0.0f;
    float r = acc / timestep;
    acc = 0.0f; last_consume_ms = now;
    return r;
}
uint32_t yaw_drop_ms(float tm) {
    if (tm < 0.01f) tm = 0.01f;
    double interval_ms = 1000.0 * 0.04 / (tm * 0.95);
    double d = 2.5 * interval_ms;
    if (d < 250.0) d = 250.0;
    if (d > 5000.0) d = 5000.0;
    return (uint32_t)d;
}
float mouse_y_factor(bool invert, float scale, bool fps_normalize, float dt) {
    float f = scale * (invert ? -1.0f : 1.0f);
    if (fps_normalize && dt > 0.0f) {
        float k = (1.0f / 60.0f) / dt;
        if (k < 0.1f) k = 0.1f; else if (k > 10.0f) k = 10.0f;
        f *= k;
    }
    return f;
}

// ================================================================================================ clocks
int32_t MsCarry::credit(double v) {
    double t = frac + v;
    if (t > 2.0e9 || t < -2.0e9) { frac = 0.0; return 0; }
    int32_t n = (int32_t)t;            // truncation toward zero (cvttsd2si)
    if ((double)n > t) --n;            // floor for negative values
    frac = t - (double)n;
    return n;
}

// ================================================================================================ limiter
double Limiter::wait_for(double now, double period) {
    if (period <= 0.0) { next = 0.0; return 0.0; }
    if (next == 0.0 || now - next > period) next = now;   // first frame, or late by more than a frame: resync
    double w = next - now;
    if (w < 0.0) w = 0.0;
    next += period;
    return w;
}

// ================================================================================================ misc
void diag_clamp(float* v) {
    float l2 = v[0] * v[0] + v[2] * v[2];
    if (l2 > 1.0f) { float s = 1.0f / sqrtf(l2); v[0] *= s; v[2] *= s; }
}
float fov_remap(float fov, float target) {
    if (target <= 0.0f || fov <= 1.0f || fov >= 179.0f) return fov;
    const double PI = 3.14159265358979323846;
    double k = tan(target * PI / 360.0) / tan(64.0 * PI / 360.0);
    return (float)(2.0 * atan(k * tan(fov * PI / 360.0)) * 180.0 / PI);
}
float steer_return(float v, float r) {
    float a = v - r; if (a < 0.0f) a = 0.0f;   // maxss
    float b = v + r; if (b > 0.0f) b = 0.0f;   // minss
    return a + b;
}
bool restart_allowed(int m) {
    // story missions with a result screen (0 and 1 load their follow-up directly: NoEndMissionScreen), sub-missions
    static const int story[] = {3, 5, 6, 8, 10, 11, 12, 14, 16, 18, 20, 21, 23, 24, 26, 27, 28};
    for (int s : story) if (m == s) return true;
    return m >= 30 && m <= 69;
}

// ================================================================================================ md5 (RFC 1321)
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8,
    0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87,
    0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
    0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
    0xeb86d391};
static const uint8_t R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20,
                              5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
static void md5_block(uint32_t s[4], const uint8_t* p) {
    uint32_t w[16];
    for (int i = 0; i < 16; ++i) w[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d; d = c; c = b;
        uint32_t x = a + f + K[i] + w[g];
        b = b + ((x << R[i]) | (x >> (32 - R[i])));
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}
void Md5::init() { s[0] = 0x67452301; s[1] = 0xefcdab89; s[2] = 0x98badcfe; s[3] = 0x10325476; len = 0; blen = 0; }
void Md5::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    len += n;
    while (n) {
        size_t k = 64 - blen; if (k > n) k = n;
        memcpy(buf + blen, p, k); blen += k; p += k; n -= k;
        if (blen == 64) { md5_block(s, buf); blen = 0; }
    }
}
void Md5::final(uint8_t out[16]) {
    uint64_t bits = len * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t z = 0; while (blen != 56) update(&z, 1);
    uint8_t L[8]; for (int i = 0; i < 8; ++i) L[i] = (uint8_t)(bits >> (8 * i));
    update(L, 8);
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(s[i] >> (8 * j));
}
void md5(const void* data, size_t n, uint8_t out[16]) { Md5 m; m.init(); m.update(data, n); m.final(out); }
void md5_hex(const uint8_t d[16], char out[33]) {
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) { out[i * 2] = hx[d[i] >> 4]; out[i * 2 + 1] = hx[d[i] & 15]; }
    out[32] = 0;
}

} // namespace gp
