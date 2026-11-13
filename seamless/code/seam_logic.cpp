// seam_logic.cpp - pure logic of the [seamless] feature (see seam_logic.h).
#include "seam_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace sl {

void Tags::add_entry(uint32_t p, double t) {
    for (int i = 0; i < n_entry; ++i) if (entry[i] == p) { entry_t[i] = t; return; }
    if (n_entry == CAP) { memmove(entry, entry + 1, sizeof(uint32_t) * (CAP - 1)); memmove(entry_t, entry_t + 1, sizeof(double) * (CAP - 1)); --n_entry; }
    entry[n_entry] = p; entry_t[n_entry] = t; ++n_entry;
}
void Tags::add_folder(uint32_t f, double t) {
    for (int i = 0; i < n_folder; ++i) if (folder[i] == f) { folder_t[i] = t; return; }
    if (n_folder == CAP) { memmove(folder, folder + 1, sizeof(uint32_t) * (CAP - 1)); memmove(folder_t, folder_t + 1, sizeof(double) * (CAP - 1)); --n_folder; }
    folder[n_folder] = f; folder_t[n_folder] = t; ++n_folder;
}
bool Tags::has_entry(uint32_t p, double now, float window) const {
    if (!p) return false;
    for (int i = 0; i < n_entry; ++i) if (entry[i] == p && now - entry_t[i] <= window && now >= entry_t[i]) return true;
    return false;
}
bool Tags::has_folder(uint32_t f, double now, float window) const {
    if (!f) return false;
    for (int i = 0; i < n_folder; ++i) if (folder[i] == f && now - folder_t[i] <= window && now >= folder_t[i]) return true;
    return false;
}
double Tags::first_time() const {
    double t = 0;
    for (int i = 0; i < n_entry; ++i) if (t == 0 || entry_t[i] < t) t = entry_t[i];
    for (int i = 0; i < n_folder; ++i) if (t == 0 || folder_t[i] < t) t = folder_t[i];
    return t;
}

Decision classify(const Cfg& c, const Pending* p, int n, const Tags& t, double now) {
    Decision d;
    if (!c.enabled) { d.v = V_OFF; d.why = "feature off"; return d; }
    d.v = V_NORMAL;
    if (n <= 0) { d.why = "nothing pending"; return d; }
    if (n > c.max_entries) { d.why = "more blocks than a border swap loads"; return d; }
    for (int i = 0; i < n; ++i) {
        if (!p[i].entry || !p[i].folder) { d.why = "unreadable entry"; return d; }
        if (p[i].teleport) { d.why = "a teleport (taxi, mission start, corridor teleport)"; return d; }
        if (p[i].kind != 1 && p[i].kind != 2) { d.why = "not a map / submap block (mission, cutscene, playerdata)"; return d; }
        bool by_e = t.has_entry(p[i].entry, now, c.tag_window), by_f = t.has_folder(p[i].folder, now, c.tag_window);
        if (!by_e && !by_f) { d.why = "not requested by a walk loader (mission, taxi, death, menu, script)"; return d; }
        if (by_e) ++d.walk_by_entry; else ++d.walk_by_folder;
    }
    d.v = V_SEAMLESS; d.why = "walk border";
    return d;
}

void Stats::add(int k, double ms) {
    if (k < 0 || k > 2) return;
    if (!n[k] || ms < min_ms[k]) min_ms[k] = ms;
    if (!n[k] || ms > max_ms[k]) max_ms[k] = ms;
    ++n[k]; sum_ms[k] += ms;
}

// ---------------------------------------------------------------------------------------------- prefetch table
static const char* skip_ws(const char* s) { while (*s == ' ' || *s == '\t') ++s; return s; }
static int word(const char*& s, char* out, int cap) {
    s = skip_ws(s);
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') { if (n < cap - 1) out[n++] = *s; ++s; }
    out[n] = 0;
    return n;
}
int Table::find_archive(const char* name) const {
    for (int i = 0; i < na; ++i) if (!_stricmp(a[i].name, name)) return i;
    return -1;
}
int Table::find_block(const char* entry) const {
    for (int i = 0; i < nb; ++i) if (!_stricmp(b[i].entry, entry)) return i;
    return -1;
}
bool Table::parse(const char* text) {
    na = nb = nl = errors = 0;
    const char* s = text;
    char w[160], x[160], y[160], z[160], e[400];
    while (s && *s) {
        const char* line_end = strchr(s, '\n');
        const char* ln = skip_ws(s);
        if (*ln && *ln != '#' && *ln != '\r' && *ln != '\n') {
            const char* q = ln;
            word(q, w, sizeof w);
            if (!strcmp(w, "archive")) {
                if (word(q, x, sizeof x) && word(q, y, sizeof y) && na < MAX_A && strlen(x) < sizeof a[0].name) {
                    strcpy_s(a[na].name, x); a[na].bytes = _strtoui64(y, nullptr, 10); ++na;
                } else ++errors;
            } else if (!strcmp(w, "block")) {
                if (word(q, e, sizeof e) && word(q, x, sizeof x) && word(q, y, sizeof y) && word(q, z, sizeof z) &&
                    nb < MAX_B && strlen(e) < sizeof b[0].entry && find_archive(x) >= 0) {
                    strcpy_s(b[nb].entry, e); b[nb].archive = find_archive(x);
                    b[nb].offset = (uint32_t)strtoul(y, nullptr, 10); b[nb].size = (uint32_t)strtoul(z, nullptr, 10);
                    if (b[nb].size) ++nb; else ++errors;
                } else ++errors;
            } else if (!strcmp(w, "loader")) {
                if (word(q, x, sizeof x) && word(q, y, sizeof y) && word(q, z, sizeof z) && word(q, e, sizeof e) && nl < MAX_L) {
                    Loader& L = l[nl];
                    L.x = (float)atof(x); L.z = (float)atof(y); L.r = (float)atof(z); L.nblock = 0;
                    char* ctx = nullptr;
                    for (char* tok = strtok_s(e, "|", &ctx); tok; tok = strtok_s(nullptr, "|", &ctx)) {
                        int bi = find_block(tok);
                        if (bi >= 0 && L.nblock < 4) L.block[L.nblock++] = bi; else ++errors;
                    }
                    if (L.nblock && L.r > 0 && L.r < 2000 && fabsf(L.x) < 100000 && fabsf(L.z) < 100000) ++nl; else ++errors;
                } else ++errors;
            } else ++errors;
        }
        s = line_end ? line_end + 1 : nullptr;
    }
    return nl > 0;
}

int Scheduler::due(const Table& t, float x, float z, double now, float distance, float repeat, int* out, int cap) {
    int n = 0;
    for (int i = 0; i < t.nl; ++i) {
        const Loader& L = t.l[i];
        float dx = x - L.x, dz = z - L.z;
        float lim = L.r + distance;
        if (dx * dx + dz * dz > lim * lim) continue;
        for (int k = 0; k < L.nblock; ++k) {
            int b = L.block[k];
            if (b < 0 || b >= t.nb || now - last[b] < repeat) continue;
            bool dup = false;
            for (int j = 0; j < n; ++j) if (out[j] == b) dup = true;
            if (dup) continue;
            if (n < cap) { out[n++] = b; last[b] = now; }
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------- names
const char* submap_name(int id) {
    static const char* names[] = {"Border 01", "Border 02", "Downtown 01", "Downtown 02", "Industrial 01", "Industrial 02",
                                  "Harbour 01", "Farm 01", "Meat factory 01", "Hacienda", "Jungle 01", "Runaway train",
                                  "Farm 02", "Meat factory 02", "Harbour 02", "Downtown 03", "Industrial 03",
                                  "Industrial 04", "Downtown 04", "Jungle 02", "Downtown 05 (training)"};
    return id >= 0 && id < (int)(sizeof names / sizeof names[0]) ? names[id] : "?";
}
const char* map_name(int id) {
    static const char* names[] = {"Map01 Border", "Map02 Downtown", "Map03 Industrial", "Map04 Harbour", "Map06 Farm",
                                  "Map07 Meat factory", "Map08 Hacienda", "Map09 Jungle", "Map10 Runaway train"};
    return id >= 0 && id < (int)(sizeof names / sizeof names[0]) ? names[id] : "?";
}

}  // namespace sl
