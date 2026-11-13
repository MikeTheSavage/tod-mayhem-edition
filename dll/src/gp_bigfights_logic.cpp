// gp_bigfights_logic.cpp - see gp_bigfights_logic.h
#include "gp_bigfights_logic.h"
#include <math.h>

namespace bf {

bool parse_clone_folder(const char* s, int* tier, int* serial) {
    if (!s || s[0] != 'B' || s[1] != 'F' || s[2] < '1' || s[2] > '3' || s[3] != '_') return false;
    int v = 0, n = 0;
    for (const char* p = s + 4; *p; ++p, ++n) {
        if (*p < '0' || *p > '9' || n >= 3) return false;
        v = v * 10 + (*p - '0');
    }
    if (n != 3) return false;
    *tier = s[2] - '0'; *serial = v;
    return true;
}

bool tier_wakes(int tier, int level, int force_tier) {
    if (force_tier >= 0) return force_tier > 0 && (tier == TIER_FRIEND || tier <= force_tier);
    if (level == BRUTAL) return tier == TIER_MAIN || tier == TIER_FRIEND;
    return level == PSYCHO;
}

bool share_pick(int serial, float share) {
    if (share >= 1.0f) return true;
    if (share <= 0.0f || serial < 1) return false;
    long long n = (long long)floorf(share * 1000.0f + 0.5f);   // per mille: integer ceilings, no float edge at 5 x 0.6
    long long a = ((long long)serial * n + 999) / 1000, b = ((long long)(serial - 1) * n + 999) / 1000;
    return a > b;
}

bool clone_wakes(int tier, int serial, int level, const Cfg& c) {
    if (!tier_wakes(tier, level, c.force_tier)) return false;
    bool psycho = c.force_tier >= 0 ? c.force_tier == 2 : level == PSYCHO;
    return !psycho || share_pick(serial, c.wake_share_psycho);
}

int live_cap(int level, int game_value, int shells, const Cfg& c) {
    float s = level == BRUTAL ? c.cap_brutal : (level == PSYCHO ? c.cap_psycho : 0.0f);
    if (s <= 1.0f || game_value <= 0) return 0;
    int t = (int)floorf((float)game_value * s + 0.5f);
    int top = shells - 1;
    if (t > top) t = top;
    return t > game_value ? t : 0;
}

const char* slot_format(int index, const char* game_format, bool separate) {
    return separate && index != 11 ? "BFSlot%02d" : game_format;
}

bool ring_spot(int i, int seed, float* dx, float* dz) {
    static const float R[3] = {2.2f, 3.4f, 4.6f};
    if (i < 0 || i >= 24) return false;
    float a = ((float)((i % 8) + (seed & 7)) + (float)(i / 8) * 0.5f) * 0.7853982f;
    *dx = R[i / 8] * cosf(a); *dz = R[i / 8] * sinf(a);
    return true;
}

bool is_free_roam(int mission) {
    switch (mission) { case 2: case 4: case 7: case 9: case 13: case 17: case 19: case 22: case 25: return true; default: return false; }
}

int squads_for_heat(int heat, const Cfg& c) {
    if (heat >= 5) return c.squad_cars_l5;
    if (heat == 4) return c.squad_cars_l4;
    if (heat == 3) return c.squad_cars_l3;
    return 0;
}

bool squad_may_start(int heat, int alive_cars, int game_cars_busy, bool friend_mission, bool cutscene, bool in_mission, const Cfg& c) {
    if (!c.squads || cutscene || friend_mission) return false;
    if (in_mission && !c.squads_in_missions) return false;
    if (alive_cars >= squads_for_heat(heat, c)) return false;
    return game_cars_busy < 4;      // the vehicle sub-controller hands out 4 of the 5 shells
}

}  // namespace bf
