// gw_logic.cpp - pure logic of gang wars (see gw_logic.h). No Windows, no game memory.
#include "gw_logic.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

namespace gw {

int parse_ints(const char* s, int* out, int cap) {
    int n = 0;
    if (!s) return 0;
    while (*s && n < cap) {
        while (*s && !(isdigit((unsigned char)*s) || *s == '-')) ++s;
        if (!*s) break;
        char* e = nullptr;
        long v = strtol(s, &e, 10);
        if (e == s) { ++s; continue; }
        out[n++] = (int)v;
        s = e;
    }
    return n;
}

static void lower_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; ++i) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

int find_group(const Cfg& c, const char* name) {
    char key[NAME_LEN]; lower_copy(key, name, NAME_LEN);
    for (int g = 0; g < c.ngroups; ++g) if (!strcmp(c.groups[g].name, key)) return g;
    return -1;
}

int add_group(Cfg& c, const char* name, const char* models) {
    if (!name || !*name || strlen(name) >= NAME_LEN) return -1;
    int g = find_group(c, name);
    if (g < 0) {
        if (c.ngroups >= MAX_GROUPS) return -1;
        g = c.ngroups;
    }
    Group gr; memset(&gr, 0, sizeof gr);
    lower_copy(gr.name, name, NAME_LEN);
    gr.nmodels = parse_ints(models, gr.models, MAX_MODELS);
    if (gr.nmodels <= 0) return -1;
    c.groups[g] = gr;
    if (g == c.ngroups) ++c.ngroups;
    return g;
}

int parse_pairs(Cfg& c, const char* s, char* bad, int bad_len) {
    int n = 0;
    if (bad && bad_len) bad[0] = 0;
    for (int a = 0; a < MAX_GROUPS; ++a) for (int b = 0; b < MAX_GROUPS; ++b) c.hostile[a][b] = false;
    if (!s) return 0;
    char tok[64];
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\t' || *s == ';') ++s;
        if (!*s) break;
        int k = 0;
        while (*s && *s != ',' && *s != ';' && k < (int)sizeof tok - 1) tok[k++] = *s++;
        tok[k] = 0;
        while (k && (tok[k - 1] == ' ' || tok[k - 1] == '\t')) tok[--k] = 0;
        char* dash = strchr(tok, '-');
        if (!dash) { if (bad && (int)(strlen(bad) + strlen(tok) + 2) < bad_len) { strcat(bad, tok); strcat(bad, " "); } continue; }
        *dash = 0;
        char* a = tok; char* b = dash + 1;
        while (*a == ' ') ++a;
        while (*b == ' ') ++b;
        for (char* e = a + strlen(a); e > a && e[-1] == ' '; ) *--e = 0;
        int ga = find_group(c, a), gb = find_group(c, b);
        if (ga < 0 || gb < 0 || ga == gb) {
            if (bad && (int)(strlen(bad) + strlen(a) + strlen(b) + 3) < bad_len) { strcat(bad, a); strcat(bad, "-"); strcat(bad, b); strcat(bad, " "); }
            continue;
        }
        c.hostile[ga][gb] = c.hostile[gb][ga] = true;
        ++n;
    }
    return n;
}

void default_groups(Cfg& c) {
    c.ngroups = 0;
    // Model_type numbers of CharacterDict's ModelStat rows (gang_wars/README.md): the shipped hostile models
    add_group(c, "cartel", "0, 1");          // CMLong, CMShort (Cezar Morales' men; Cezar 6 is a boss)
    add_group(c, "virgilio", "2, 3");        // VB_Small, VB_Tall (the Virgilio brothers' gang)
    add_group(c, "elvez", "22");             // ElvezGuard (the hacienda guards)
    add_group(c, "gangster", "27");          // Gangster (Los Toros street gangs)
    add_group(c, "army", "15");              // Soldier
    add_group(c, "mexpolice", "24");         // MexicanPolice
    add_group(c, "border", "5");             // BorderPolice
    add_group(c, "dea", "9");                // DEA_Taskforce (enemies in the last missions)
    add_group(c, "outlaw", "30");            // DEA_Outlaw
}

int group_of_model(const Cfg& c, int model) {
    for (int g = 0; g < c.ngroups; ++g)
        for (int k = 0; k < c.groups[g].nmodels; ++k) if (c.groups[g].models[k] == model) return g;
    return -1;
}

bool any_rival(const Cfg& c, int g) {
    if (g < 0 || g >= c.ngroups) return false;
    for (int h = 0; h < c.ngroups; ++h) if (c.hostile[g][h]) return true;
    return false;
}

// ------------------------------------------------------------------------------------------------ matrix
int owner(const Cfg& c, const Design& d, int f, bool* provoked) {
    (void)c;
    for (int g = 0; g < MAX_GROUPS; ++g) {
        if (d.alloc[g] < 0) continue;
        if (f == d.alloc[g]) { if (provoked) *provoked = false; return g; }
        if (f == d.alloc[g] + d.base) { if (provoked) *provoked = true; return g; }
    }
    return -1;
}

bool is_spare(const Cfg& c, const Design& d, int f) {
    for (int i = 0; i < c.nspares; ++i) if (f == c.spares[i] || f == c.spares[i] + d.base) return true;
    return false;
}

int desired_cell(const Cfg& cfg, const Design& d, int r, int c, VanillaFn vanilla, void* ctx) {
    bool pr = false, pc = false;
    int gr = owner(cfg, d, r, &pr), gc = owner(cfg, d, c, &pc);
    int player_m = FACTION_PLAYER + d.base, enemy_m = d.enemy + d.base;
    if (gr >= 0 && gc >= 0) {
        if (gr == gc) return REL_SELF;
        return cfg.hostile[gr][gc] ? REL_HOSTILE : REL_FRIEND;
    }
    if (gr >= 0) {
        if (c == FACTION_PLAYER || c == player_m) return (pr || !d.provoke) ? REL_HOSTILE : REL_NEUTRAL;
        if (c == d.enemy || c == enemy_m) return REL_FRIEND;
        if (is_spare(cfg, d, c)) return REL_FRIEND;            // an unused spare: nobody holds it
        return vanilla(ctx, d.enemy, c);                        // everyone else: as faction 9 sees them
    }
    if (gc >= 0) {
        if (r == FACTION_PLAYER || r == player_m) return REL_HOSTILE;   // Ram: kills count, Loco / auto-aim pick them
        if (r == d.enemy || r == enemy_m) return REL_FRIEND;
        if (is_spare(cfg, d, r)) return REL_FRIEND;
        return vanilla(ctx, r, d.enemy);                        // everyone else sees them as faction 9
    }
    if (is_spare(cfg, d, r) || is_spare(cfg, d, c)) return vanilla(ctx, r, c);   // a spare nobody holds: as shipped
    return -1;
}

int alloc_spare(const Cfg& c, Design& d, int g) {
    if (g < 0 || g >= MAX_GROUPS) return -1;
    if (d.alloc[g] >= 0) return d.alloc[g];
    for (int i = 0; i < c.nspares; ++i) {
        int f = c.spares[i];
        if (f < 0 || f >= d.base || f == FACTION_PLAYER || f == d.enemy) continue;
        bool used = false;
        for (int h = 0; h < MAX_GROUPS; ++h) if (d.alloc[h] == f) used = true;
        if (!used) { d.alloc[g] = f; return f; }
    }
    return -1;
}

void release_spare(Design& d, int g) { if (g >= 0 && g < MAX_GROUPS) d.alloc[g] = -1; }

// ------------------------------------------------------------------------------------------------ war
float dist(const float* a, const float* b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

int nearest_rival(const Cfg& c, const Member* m, int n, int i, float r, float* d) {
    const Member& a = m[i];
    if (a.group < 0 || !a.has_pos) return -1;
    int best = -1; float bd = r;
    for (int j = 0; j < n; ++j) {
        const Member& b = m[j];
        if (j == i || !b.alive || !b.eligible || !b.has_pos || b.group < 0 || b.state == ST_RELEASED) continue;
        if (!c.hostile[a.group][b.group]) continue;
        float dd = dist(a.pos, b.pos);
        if (dd <= bd) { bd = dd; best = j; }
    }
    if (d) *d = bd;
    return best;
}

static int faction_for(const Design& d, const Member& m) {
    if (m.state == ST_WAR) return d.alloc[m.group];
    if (m.state == ST_PROVOKED) return d.alloc[m.group] >= 0 ? d.alloc[m.group] + d.base : -1;
    return m.orig;
}

void step(const Cfg& c, Design& d, Member* m, int n, Spark& sp, float dt, Out* out) {
    d.provoke = c.provoke; d.enemy = c.enemy_faction;
    for (int i = 0; i < n; ++i) out[i] = Out();
    // 1. reconcile with what the roots hold (the game may have provoked, reset or taken a member)
    for (int i = 0; i < n; ++i) {
        Member& a = m[i];
        if (a.state == ST_RELEASED || !a.alive) continue;
        if (a.state == ST_CALM) {
            if (a.faction != a.orig) { a.state = ST_RELEASED; out[i].why = "the game changed its faction"; out[i].changed = true; }
            continue;
        }
        int g = a.group, f = g >= 0 ? d.alloc[g] : -1;
        if (f >= 0 && a.faction == f + d.base) { if (a.state != ST_PROVOKED) { a.state = ST_PROVOKED; out[i].changed = true; out[i].why = "provoked (the game's own rule: Ram shot it)"; } }
        else if (f >= 0 && a.faction == f) { /* ours */ }
        else if (a.faction == a.orig) { a.state = ST_CALM; a.calm_t = 0; out[i].changed = true; out[i].why = "the game put its faction back"; }
        else { a.state = ST_RELEASED; out[i].changed = true; out[i].why = "the game changed its faction"; }
    }
    // 2. the spark: Ram's hit on a calm gang member with a rival within range
    if (c.spark) {
        for (int i = 0; i < n; ++i) {
            Member& a = m[i];
            if (!a.ram_hit || !a.alive || !a.eligible || a.group < 0 || a.state != ST_CALM) continue;
            float rd = 0; int r = nearest_rival(c, m, n, i, c.range, &rd);
            if (r < 0) continue;
            if (alloc_spare(c, d, a.group) < 0 || alloc_spare(c, d, m[r].group) < 0) continue;
            sp.active = true; sp.quiet_t = 0; memcpy(sp.pos, a.pos, sizeof sp.pos);
            a.state = c.provoke ? ST_PROVOKED : ST_WAR; a.calm_t = 0;
            out[i].changed = true; out[i].why = c.provoke ? "spark: Ram shot it with a rival near (it goes after Ram)" : "spark: Ram shot it with a rival near";
            out[i].rival = r; out[i].rival_d = rd;
        }
    }
    // 3. war and calm
    float stay = c.range + c.hysteresis;
    for (int i = 0; i < n; ++i) {
        Member& a = m[i];
        if (a.state == ST_RELEASED || a.group < 0) continue;
        if (!a.alive) { if (a.state != ST_CALM) { a.state = ST_CALM; a.calm_t = 0; } continue; }   // the dead keep their faction
        if (!a.eligible) {
            if (a.state != ST_CALM) { a.state = ST_CALM; a.calm_t = 0; out[i].changed = true; out[i].why = "not eligible any more (scripted / mounted gun / priority target)"; }
            continue;
        }
        if (a.state == ST_CALM) {
            if (a.cooldown_t > 0.0f) { a.cooldown_t -= dt; if (a.cooldown_t > 0.0f) continue; a.cooldown_t = 0.0f; }
            if (c.spark && (!sp.active || !a.has_pos || dist(a.pos, sp.pos) > c.spark_radius)) continue;
            float rd = 0; int r = nearest_rival(c, m, n, i, c.range, &rd);
            if (r < 0) continue;
            if (alloc_spare(c, d, a.group) < 0) { out[i].why = "no spare faction left"; continue; }
            a.state = ST_WAR; a.calm_t = 0; a.idle_t = 0;
            out[i].changed = true; out[i].why = "a rival gang member is near"; out[i].rival = r; out[i].rival_d = rd;
        } else {
            float rd = 0; int r = nearest_rival(c, m, n, i, stay, &rd);
            if (r >= 0) a.calm_t = 0;
            else {
                a.calm_t += dt;
                if (a.calm_t >= c.calm_s) { a.state = ST_CALM; a.calm_t = 0; a.idle_t = 0; out[i].changed = true; out[i].why = "no rival near any more"; continue; }
            }
            // near a rival but not fighting one (a wall between them, out of sight): a gang at war ignores Ram, so it
            // must not idle there; back to the game's faction for a while (provoked members hunt Ram anyway)
            if (a.state == ST_WAR) {
                if (a.target_rival) a.idle_t = 0;
                else if ((a.idle_t += dt) >= c.idle_s) {
                    a.state = ST_CALM; a.calm_t = 0; a.idle_t = 0; a.cooldown_t = c.retry_s;
                    out[i].changed = true; out[i].why = "no rival in sight (not fighting one)";
                }
            }
        }
    }
    // 4. spark area quiet -> over
    if (sp.active) {
        bool busy = false;
        for (int i = 0; i < n; ++i)
            if (m[i].alive && (m[i].state == ST_WAR || m[i].state == ST_PROVOKED) && m[i].has_pos && dist(m[i].pos, sp.pos) <= c.spark_radius) busy = true;
        if (busy) sp.quiet_t = 0; else if ((sp.quiet_t += dt) >= c.calm_s) sp.active = false;
    }
    // 5. what each live root must hold; spares nobody alive uses go back
    for (int i = 0; i < n; ++i) {
        Member& a = m[i];
        if (!a.alive || a.state == ST_RELEASED || a.group < 0) continue;
        int f = faction_for(d, a);
        if (f >= 0 && f != a.faction) out[i].want_faction = f;
    }
    for (int g = 0; g < MAX_GROUPS; ++g) {
        if (d.alloc[g] < 0) continue;
        bool used = false;
        for (int i = 0; i < n; ++i) {
            const Member& a = m[i];
            if (!a.alive) continue;
            bool pv = false;
            if ((a.group == g && (a.state == ST_WAR || a.state == ST_PROVOKED)) || owner(c, d, a.faction, &pv) == g) used = true;
        }
        if (!used) release_spare(d, g);
    }
}

const char* state_name(int s) {
    switch (s) { case ST_CALM: return "calm"; case ST_WAR: return "war"; case ST_PROVOKED: return "provoked"; case ST_RELEASED: return "released"; }
    return "?";
}

}  // namespace gw
