// gp_hang_logic.cpp - pure logic of the hang watchdog (see gp_hang_logic.h). Unit tests: tests/dll/test_hang.cpp.
#include "gp_hang_logic.h"
#include <stdlib.h>
#include <string.h>

namespace hang {

// ------------------------------------------------------------------------------------------------ the stall decision
Act Watch::poll(const Cfg& c, const Sample& s) {
    if (!armed) {
        if (s.beat == 0) return NONE;   // the game has not run a frame yet (boot)
        armed = true; last_beat = s.beat; t_last = s.now; usable_then = s.usable; committed = false; reported = false;
        return NONE;
    }
    if (s.beat != last_beat) {
        double silent = s.now - t_last;
        last_beat = s.beat; t_last = s.now; usable_then = s.usable; committed = false;
        if (reported) { reported = false; stall_s = silent; return RECOVERED; }
        return NONE;
    }
    if (s.debugger) { t_last = s.now; committed = false; return NONE; }   // a breakpoint, not a hang
    if (!usable_then) {                 // silent since the window went away: count from the moment it is back
        if (s.usable) { usable_then = true; t_last = s.now; committed = false; }
        return NONE;
    }
    if (!committed) {
        if (!s.usable) { usable_then = false; return NONE; }   // minimised / alt-tab right as the frames stopped
        if (s.now - t_last >= c.commit_seconds) committed = true;
    }
    if (reported) return NONE;
    double lim = s.loading ? c.load_seconds : c.seconds;
    if (s.now - t_last >= lim) { reported = true; t_reported = s.now; stall_s = s.now - t_last; return STALL; }
    return NONE;
}

// ------------------------------------------------------------------------------------------------ return addresses
bool looks_like_call(const uint8_t* b) {   // b[0..6] = the 7 bytes before the return address
    if (b[2] == 0xE8) return true;                                                   // call rel32
    if (b[1] == 0xFF && b[2] == 0x15) return true;                                   // call [disp32]
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0xD0) return true;                          // call reg
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0x10 && (b[6] & 7) != 4 && (b[6] & 7) != 5) return true;   // call [reg]
    if (b[4] == 0xFF && b[5] == 0x14 && (b[6] & 7) != 5) return true;                // call [sib]
    if (b[4] == 0xFF && (b[5] & 0xF8) == 0x50 && (b[5] & 7) != 4) return true;       // call [reg+d8]
    if (b[3] == 0xFF && b[4] == 0x54) return true;                                   // call [sib+d8]
    if (b[1] == 0xFF && (b[2] & 0xF8) == 0x90 && (b[2] & 7) != 4) return true;       // call [reg+d32]
    if (b[0] == 0xFF && (b[1] == 0x94 || b[1] == 0x14)) return true;                 // call [sib+d32] / [sib d32]
    return false;
}

// ------------------------------------------------------------------------------------------------ the map file
static bool is_hex(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }
static uint32_t hexval(const char* p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) { char ch = p[i]; v = v * 16 + (uint32_t)(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10); }
    return v;
}
static int cmp_sym(const void* a, const void* b) {
    uint32_t x = ((const Sym*)a)->rva, y = ((const Sym*)b)->rva;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int MapSyms::parse(const char* map, size_t len, Sym* sym_mem, int sym_cap, char* text_mem, uint32_t tcap) {
    sym = sym_mem; cap = sym_cap; n = 0; text = text_mem; text_cap = tcap; used = 0; pref_base = 0;
    if (!map || !sym || !text || tcap < 2) return 0;
    text[used++] = 0;   // offset 0 = ""
    size_t i = 0;
    while (i < len) {
        size_t a = i;
        while (i < len && map[i] != '\n') ++i;
        size_t b = i; ++i;
        if (b > a && map[b - 1] == '\r') --b;
        const char* L = map + a; size_t ln = b - a;
        static const char PREF[] = "Preferred load address is ";
        const char* pp = nullptr;
        for (size_t k = 0; k + sizeof PREF - 1 <= ln && k < 4; ++k) if (!memcmp(L + k, PREF, sizeof PREF - 1)) pp = L + k + sizeof PREF - 1;
        if (pp) { size_t m = 0; while (pp + m < L + ln && is_hex(pp[m]) && m < 8) ++m; if (m) pref_base = hexval(pp, (int)m); continue; }
        // tokens
        const char* tok[8]; size_t tl[8]; int nt = 0;
        for (size_t k = 0; k < ln && nt < 8;) {
            while (k < ln && (L[k] == ' ' || L[k] == '\t')) ++k;
            if (k >= ln) break;
            size_t s0 = k;
            while (k < ln && L[k] != ' ' && L[k] != '\t') ++k;
            tok[nt] = L + s0; tl[nt] = k - s0; ++nt;
        }
        if (nt < 4 || tl[0] != 13 || tok[0][4] != ':') continue;
        if (memcmp(tok[0], "0001", 4)) continue;                         // the code section only
        if (tl[2] != 8) continue;
        bool hx = true; for (int k = 0; k < 8; ++k) hx = hx && is_hex(tok[2][k]);
        if (!hx) continue;
        int fi = -1; for (int k = 3; k < nt; ++k) if (tl[k] == 1 && tok[k][0] == 'f') fi = k;
        if (fi < 0) continue;                                            // not a function
        int oi = nt - 1;                                                 // the object file, when the row has one
        uint32_t va = hexval(tok[2], 8);
        if (!pref_base || va < pref_base || n >= cap) continue;
        size_t need = tl[1] + 1 + (oi > 0 ? tl[oi] + 1 : 1);
        if (used + need > text_cap) continue;
        Sym& s = sym[n++];
        s.rva = va - pref_base;
        s.name = used; memcpy(text + used, tok[1], tl[1]); used += (uint32_t)tl[1]; text[used++] = 0;
        if (oi > 0 && oi > fi) { s.obj = used; memcpy(text + used, tok[oi], tl[oi]); used += (uint32_t)tl[oi]; text[used++] = 0; }
        else s.obj = 0;
    }
    qsort(sym, (size_t)n, sizeof(Sym), cmp_sym);
    return n;
}

const Sym* MapSyms::find(uint32_t rva, uint32_t* off) const {
    if (n <= 0 || rva < sym[0].rva) return nullptr;
    int lo = 0, hi = n - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (sym[mid].rva <= rva) lo = mid; else hi = mid - 1; }
    if (off) *off = rva - sym[lo].rva;
    return &sym[lo];
}

uint32_t map_timestamp(const char* map, size_t len) {
    static const char K[] = "Timestamp is ";
    const size_t kl = sizeof K - 1;
    if (!map) return 0;
    if (len > 4096) len = 4096;
    for (size_t i = 0; i + kl < len; ++i) {
        if (memcmp(map + i, K, kl) != 0) continue;
        uint32_t v = 0; int digits = 0;
        for (size_t j = i + kl; j < len && digits < 8; ++j, ++digits) {
            char c = map[j]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | (uint32_t)d;
        }
        return digits ? v : 0;
    }
    return 0;
}
void short_name(const char* d, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!d) return;
    size_t o = 0;
    auto put = [&](const char* s, size_t n) { for (size_t k = 0; k < n && o + 1 < cap; ++k) out[o++] = s[k]; out[o] = 0; };
    if (d[0] == '?' && d[1] != '?') {
        const char* part[8]; size_t pl[8]; int np = 0;
        const char* p = d + 1;
        while (*p && np < 8) {
            const char* e = strchr(p, '@');
            if (!e) { part[np] = p; pl[np] = strlen(p); ++np; break; }
            if (e == p) break;          // "@@": the end of the qualified name
            part[np] = p; pl[np] = (size_t)(e - p); ++np;
            p = e + 1;
        }
        if (np == 0) { put(d, strlen(d)); return; }
        for (int k = np - 1; k >= 0; --k) { put(part[k], pl[k]); if (k) put("::", 2); }
        return;
    }
    if (d[0] == '_') {
        const char* s = d + 1; const char* at = strchr(s, '@');
        put(s, at ? (size_t)(at - s) : strlen(s));
        return;
    }
    put(d, strlen(d));
}

}  // namespace hang
