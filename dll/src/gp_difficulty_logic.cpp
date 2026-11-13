// gp_difficulty_logic.cpp - see gp_difficulty_logic.h
#include "gp_difficulty_logic.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

namespace df {

const char* level_name(int lv) {
    switch (lv) { case EASY: return "easy"; case NORMAL: return "normal"; case HARD: return "hard"; case BRUTAL: return "brutal";
                  case PSYCHO: return "psycho"; }
    return "?";
}
static bool ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}
int level_from_name(const char* s) {
    if (!s) return -1;
    for (int i = EASY; i <= LEVEL_MAX; ++i) if (ieq(s, level_name(i))) return i;
    return -1;
}

int level_from_setting(int setting, int extra) {
    if (setting <= 0) return EASY;
    if (setting == 1) return NORMAL;
    if (setting == 2) { if (extra < 0) extra = 0; if (extra > EXTRA_MAX) extra = EXTRA_MAX; return HARD + extra; }
    return NORMAL;   // the game reads anything else as Normal (0x5B73F0: only 0 and 2 are special)
}

// options.py DIFFICULTY: hard  dmg_taken 1.4, dmg_dealt 0.8, spread x0.6, react 0.7, pause 0.75, ammo 0.75
//                        brutal dmg_taken 2.0, dmg_dealt 0.6, spread x0.25, react 0.5, pause 0.5, ammo 0.5
Factors factors(int level) {
    Factors f;
    for (int i = 0; i < T_COUNT; ++i) f.k[i] = 1.0f;
    if (level == HARD) {
        f.k[T_DAMAGE_TAKEN] = 1.4f; f.k[T_DAMAGE_DEALT] = 0.8f; f.k[T_SPREAD] = 0.6f;
        f.k[T_PAUSE] = 0.75f; f.k[T_REACT] = 0.7f; f.k[T_AMMO] = 0.75f;
    } else if (level >= BRUTAL) {   // PSYCHO: BRUTAL's tuning until its own feature exists
        f.k[T_DAMAGE_TAKEN] = 2.0f; f.k[T_DAMAGE_DEALT] = 0.6f; f.k[T_SPREAD] = 0.25f;
        f.k[T_PAUSE] = 0.5f; f.k[T_REACT] = 0.5f; f.k[T_AMMO] = 0.5f;
    }
    return f;
}
float tuned(int target, float base, int level) {
    if (target < 0 || target >= T_COUNT) return base;
    return base * factors(level).k[target];
}

bool weapon_row_tuned(int id) { return id >= 0 && id <= 9; }
bool chartype_row_tuned(int id) { return id != 7; }
bool modelstat_row_tuned(int t) { return t != 20 && t != 10 && t != 18; }

static bool near_(float a, float b) { return fabsf(a - b) <= 1e-4f * (fabsf(b) > 1.0f ? fabsf(b) : 1.0f); }
int static_level(float dh, float pm) {
    // vanilla PlayerStatusCtrl: DamageHard 1.4, PlayerDamageMod 1.0; the data option multiplied them
    const float VH = 1.4f, VM = 1.0f;
    if (near_(dh, VH) && near_(pm, VM)) return NORMAL;
    if (near_(dh, VH * 0.6f) && near_(pm, 1.5f)) return EASY;
    if (near_(dh, VH * 1.4f) && near_(pm, 0.8f)) return HARD;
    if (near_(dh, VH * 2.0f) && near_(pm, 0.6f)) return BRUTAL;
    return -1;
}

int menu_shown_value(int current, int extra, int value_count) {
    if (current != 2 || extra <= 0) return current;
    int v = 2 + extra;
    if (v > value_count - 1) v = value_count - 1;
    return v < 2 ? 2 : v;
}
SetResult menu_chosen(int index) {
    SetResult r;
    if (index >= 3) { r.setting = 2; r.extra = index - 2; if (r.extra > EXTRA_MAX) r.extra = EXTRA_MAX; }
    else { r.setting = index < 0 ? 0 : index; r.extra = 0; }
    return r;
}

bool parse_sidecar(const char* text, int* extra) {
    if (!text) return false;
    const char* p = strstr(text, "\nlevel");
    if (!p && !strncmp(text, "level", 5)) p = text - 1;
    if (p) {
        p += 6;
        while (*p == ' ' || *p == '\t' || *p == '=') ++p;
        char w[16]; int n = 0;
        while (n < 15 && isalpha((unsigned char)p[n])) { w[n] = p[n]; ++n; }
        w[n] = 0;
        if (!strcmp(w, "none")) { *extra = 0; return true; }
        int lv = level_from_name(w);
        if (lv < 0) return false;
        *extra = lv > HARD ? lv - HARD : 0;
        return true;
    }
    p = strstr(text, "brutal");   // the first build wrote "brutal = 0 / 1"
    if (!p) return false;
    p += 6;
    while (*p == ' ' || *p == '\t' || *p == '=') ++p;
    if (*p == '1') { *extra = 1; return true; }
    if (*p == '0') { *extra = 0; return true; }
    return false;
}
int format_sidecar(int extra, char* out, int cap) {
    if (extra < 0) extra = 0; if (extra > EXTRA_MAX) extra = EXTRA_MAX;
    return snprintf(out, cap, "; TOD Gameplay+: levels above HARD are saved as the game's HARD (Slot11) plus this line\r\n"
                              "level = %s\r\n", extra ? level_name(HARD + extra) : "none");
}

void ReinfState::on_death(const ReinfCfg& c) {
    ++deaths;
    acc += c.share;
    if (acc > 8.0f) acc = 8.0f;   // a quiet backlog never turns into a wave
}
bool ReinfState::take() {
    if (acc < 1.0f - 1e-4f) return false;
    acc -= 1.0f;
    if (acc < 0.0f) acc = 0.0f;
    ++rearmed;
    return true;
}
bool faction_skipped(int f, const int* skip, int n) {
    for (int i = 0; i < n; ++i) if (skip[i] == f || (f >= 19 && skip[i] == f - 19)) return true;
    return false;
}
bool reinforcement_candidate(const Seen& s, const ReinfCfg& c, const int* skip, int n) {
    if (s.faction < 0 || s.buddy) return false;
    if (s.sub_type == 4 || s.sub_type == 7 || s.sub_type == 8 || s.sub_type == 9) return false;   // escort, Ram, never aggressive, civilians
    if (s.faction == 0 || s.faction == 18 || s.faction == 19 || s.faction == 37) return false;      // the player's / the Mexican buddies' factions (and copies)
    if (s.health <= 0.0f || s.health >= c.boss_health) return false;
    if (faction_skipped(s.faction, skip, n)) return false;
    return true;
}
int live_cap(int requested, int game_value, int pool_shells, bool friend_mission, bool cap_with_friends) {
    if (requested <= 0 || (friend_mission && !cap_with_friends)) return game_value;
    int top = pool_shells - 1;
    int t = requested > top ? top : requested;
    return t > game_value ? t : game_value;
}
float level_share(int level, const ReinfCfg& c) { return level == PSYCHO ? c.share_psycho : (level == BRUTAL ? c.share : 0.0f); }
bool born_dead(float health, float damage_taken) { return health > 0.0f && damage_taken >= health; }
bool distance_ok(float d, const ReinfCfg& c) { return d >= c.min_distance && d <= c.max_distance; }

}  // namespace df
