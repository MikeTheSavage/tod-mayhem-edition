// gp_debug_logic.cpp - see gp_debug_logic.h.
#include "gp_debug_logic.h"
#include "gp_logic.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

namespace dbg {

static const char* skip_ws(const char* s) { while (*s == ' ' || *s == '\t') ++s; return s; }

bool KeyScript::parse(const char* text) {
    n = 0; errors = 0;
    const char* p = text;
    while (p && *p) {
        const char* e = p; while (*e && *e != '\n' && *e != ';') ++e;
        char line[96]; size_t L = (size_t)(e - p); if (L >= sizeof line) L = sizeof line - 1;
        memcpy(line, p, L); line[L] = 0;
        p = *e ? e + 1 : e;
        char* h = strchr(line, '#'); if (h) *h = 0;
        size_t m = strlen(line); while (m && (line[m - 1] == ' ' || line[m - 1] == '\r' || line[m - 1] == '\t')) line[--m] = 0;
        const char* s = skip_ws(line);
        if (!*s) continue;
        if (n >= CAP) { ++errors; continue; }
        char w[16]; size_t i = 0; while (s[i] && s[i] != ' ' && s[i] != '\t' && i < sizeof w - 1) { w[i] = (char)tolower((unsigned char)s[i]); ++i; }
        w[i] = 0; const char* a = skip_ws(s + i);
        KeyCmd k; memset(&k, 0, sizeof k); k.key = -1;
        if (!strcmp(w, "wait")) { k.op = KeyCmd::WAIT; k.ms = atoi(a); if (k.ms < 0) { ++errors; continue; } }
        else if (!strcmp(w, "tap") || !strcmp(w, "hold")) {
            char kn[32]; size_t j = 0; while (a[j] && a[j] != ' ' && j < sizeof kn - 1) { kn[j] = a[j]; ++j; }
            kn[j] = 0;
            int code = gp::key_from_name(kn);
            if (code < 0 || code > 255) { ++errors; continue; }
            k.key = code;
            if (w[0] == 't') { k.op = KeyCmd::TAP; k.ms = 80; }
            else { k.op = KeyCmd::HOLD; k.ms = atoi(skip_ws(a + j)); if (k.ms <= 0) { ++errors; continue; } }
        }
        else if (!strcmp(w, "mark")) { k.op = KeyCmd::MARK; strncpy_s(k.text, a, sizeof k.text - 1); }
        else if (!strcmp(w, "waitlog")) {
            k.op = KeyCmd::WAITLOG; k.ms = atoi(a);
            const char* t = a; while (*t && *t != ' ' && *t != '\t') ++t; t = skip_ws(t);
            if (k.ms <= 0 || !*t) { ++errors; continue; }
            strncpy_s(k.text, t, sizeof k.text - 1);
        }
        else if (!strcmp(w, "quit")) k.op = KeyCmd::QUIT;
        else { ++errors; continue; }
        c[n++] = k;
    }
    return n > 0;
}

KeyOut KeyPlayer::step(const KeyScript& s, double now, LogHas has, void* ctx) {
    KeyOut o;
    if (waiting >= 0) {                                // waitlog: until the text shows up or the time is up
        bool hit = has && has(s.c[waiting].text, ctx);
        if (!hit && now < t_wait_end) return o;
        o.mark = hit ? "waitlog: found" : "waitlog: TIMEOUT";
        waiting = -1; t_next = now;
        if (pc >= s.n) done = true;
        return o;
    }
    if (down >= 0) {                                   // a key is down: release it when its time is up
        if (now >= t_up) { o.release = down; down = -1; t_next = now; }
        else o.held = down;
        return o;
    }
    if (done) return o;
    if (t_next < 0.0) t_next = now;
    if (now < t_next) return o;
    if (pc >= s.n) { done = true; return o; }
    const KeyCmd& k = s.c[pc++];
    switch (k.op) {
    case KeyCmd::WAIT: t_next = now + k.ms / 1000.0; break;
    case KeyCmd::TAP: case KeyCmd::HOLD: o.press = k.key; down = k.key; t_up = now + k.ms / 1000.0; break;
    case KeyCmd::MARK: o.mark = k.text; break;
    case KeyCmd::QUIT: o.quit = true; done = true; break;
    case KeyCmd::WAITLOG: waiting = pc - 1; t_wait_end = now + k.ms / 1000.0; return o;
    }
    if (pc >= s.n && down < 0 && k.op != KeyCmd::WAIT) done = true;
    return o;
}

// ------------------------------------------------------------------------------------------------ heap tracker
static inline uint32_t hsh(uint32_t p) { p ^= p >> 16; p *= 0x7feb352dU; p ^= p >> 15; p *= 0x846ca68bU; p ^= p >> 16; return p; }

void LiveSet::init(uint32_t* k, uint32_t* v, uint32_t capacity_pow2) {
    key = k; val = v; cap = capacity_pow2; used = tombs = 0;
    memset(key, 0, sizeof(uint32_t) * cap); memset(val, 0, sizeof(uint32_t) * cap);
}
bool LiveSet::insert(uint32_t p, uint32_t size) {
    if (p <= 1 || !cap) return false;
    uint32_t m = cap - 1, i = hsh(p) & m, tomb = 0xFFFFFFFF;
    for (uint32_t probe = 0; probe < cap; ++probe, i = (i + 1) & m) {
        uint32_t k = key[i];
        if (k == p) { val[i] = size; return true; }      // re-allocation of a live address (should not happen): keep
        if (k == 1) { if (tomb == 0xFFFFFFFF) tomb = i; continue; }
        if (k == 0) {
            if (tomb != 0xFFFFFFFF) { i = tomb; --tombs; }
            key[i] = p; val[i] = size; ++used; return true;
        }
    }
    if (tomb != 0xFFFFFFFF) { key[tomb] = p; val[tomb] = size; --tombs; ++used; return true; }
    return false;
}
bool LiveSet::find(uint32_t p, uint32_t* size) const {
    if (p <= 1 || !cap) return false;
    uint32_t m = cap - 1, i = hsh(p) & m;
    for (uint32_t probe = 0; probe < cap; ++probe, i = (i + 1) & m) {
        uint32_t k = key[i];
        if (k == p) { if (size) *size = val[i]; return true; }
        if (k == 0) return false;
    }
    return false;
}
bool LiveSet::erase(uint32_t p, uint32_t* size) {
    if (p <= 1 || !cap) return false;
    uint32_t m = cap - 1, i = hsh(p) & m;
    for (uint32_t probe = 0; probe < cap; ++probe, i = (i + 1) & m) {
        uint32_t k = key[i];
        if (k == p) { if (size) *size = val[i]; key[i] = 1; val[i] = 0; --used; ++tombs; return true; }
        if (k == 0) return false;
    }
    return false;
}
void LiveSet::rebuild_into(LiveSet& dst) const {
    dst.init(dst.key, dst.val, dst.cap);
    for (uint32_t i = 0; i < cap; ++i) if (key[i] > 1) dst.insert(key[i], val[i]);
}

void FreedRing::init(FreedRec* mem, uint32_t capacity) { r = mem; cap = capacity; head = count = 0; memset(r, 0, sizeof(FreedRec) * cap); }
FreedRec& FreedRing::push() {
    FreedRec& x = r[head]; head = (head + 1) % cap; if (count < cap) ++count;
    memset(&x, 0, sizeof x);
    return x;
}
const FreedRec* FreedRing::find(uint32_t p) const {
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t i = (head + cap - 1 - k) % cap;
        if (r[i].p == p) return &r[i];
    }
    return nullptr;
}

bool looks_like_call(const uint8_t* b) {   // b[0..5] = the 6 bytes before the return address
    if (b[1] == 0xE8) return true;                                     // call rel32
    if (b[0] == 0xFF && b[1] == 0x15) return true;                     // call [disp32]
    if (b[0] == 0xFF && (b[1] & 0xF8) == 0x90) return true;            // call [reg+disp32]
    if (b[3] == 0xFF && (b[4] & 0xF8) == 0x50) return true;            // call [reg+disp8]
    if (b[4] == 0xFF && ((b[5] & 0xF8) == 0xD0 || (b[5] & 0xF8) == 0x10)) return true;   // call reg / call [reg]
    return false;
}

// ------------------------------------------------------------------------------------------------ stress
float Stress::interval(int k) const {
    if (imax <= imin) return imin;
    uint32_t x = seed ^ (uint32_t)(k * 2654435761u);
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    return imin + (imax - imin) * (float)(x % 10001) / 10000.0f;
}

}  // namespace dbg
