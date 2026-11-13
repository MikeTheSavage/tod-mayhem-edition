// suave_logic.cpp - see suave_logic.h. Pure logic, unit tested (suave/code/test_suave.cpp).
#include "suave_logic.h"
#include <math.h>
#include <string.h>
#include <ctype.h>

namespace sv {

static const float PI = 3.14159265358979f;

Vec3 v3(float x, float y, float z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }
Vec3 add(const Vec3& a, const Vec3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 sub(const Vec3& a, const Vec3& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 mul(const Vec3& a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
Vec3 flat_dir(const Vec3& from, const Vec3& to, const Vec3& fb) {
    float dx = to.x - from.x, dz = to.z - from.z, l = sqrtf(dx * dx + dz * dz);
    if (l > 1e-4f) return v3(dx / l, 0, dz / l);
    float fl = sqrtf(fb.x * fb.x + fb.z * fb.z);
    return fl > 1e-4f ? v3(fb.x / fl, 0, fb.z / fl) : v3(0, 0, 1);
}
float dist(const Vec3& a, const Vec3& b) { float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return sqrtf(dx * dx + dy * dy + dz * dz); }
float dist_xz(const Vec3& a, const Vec3& b) { float dx = a.x - b.x, dz = a.z - b.z; return sqrtf(dx * dx + dz * dz); }
float yaw_to(const Vec3& from, const Vec3& to) { return atan2f(to.x - from.x, to.z - from.z); }
float yaw_of(const Vec3& d) { return atan2f(d.x, d.z); }
static Vec3 right_of(const Vec3& fwd) { return v3(fwd.z, 0, -fwd.x); }   // local +x for a +z forward (yaw rotation)
static float wrap_pi(float a) { while (a > PI) a -= 2.0f * PI; while (a < -PI) a += 2.0f * PI; return a; }
float signed_yaw(const Vec3& from, const Vec3& to) { return wrap_pi(yaw_of(to) - yaw_of(from)); }

int dive_key_rel(const Vec3& f0, const Vec3& d0, float side_sign, float cone_deg, Vec3* world, Vec3* cam) {
    Vec3 f = flat_dir(v3(0, 0, 0), f0, v3(0, 0, 1)), d = flat_dir(v3(0, 0, 0), d0, f);
    Vec3 r = mul(right_of(f), side_sign < 0.0f ? -1.0f : 1.0f);
    float fw = d.x * f.x + d.z * f.z, rt = d.x * r.x + d.z * r.z, c = cosf(cone_deg * PI / 180.0f);
    int k; Vec3 w, e;
    if (fw >= c) { k = DIVE_W; w = f; e = f; }
    else if (fw <= -c) { k = DIVE_S; w = mul(f, -1.0f); e = r; }        // a back dive: seen from the side
    else if (rt >= 0.0f) { k = DIVE_D; w = r; e = f; }                    // a side dive: seen from his face
    else { k = DIVE_A; w = mul(r, -1.0f); e = f; }
    if (world) *world = w; if (cam) *cam = e;
    return k;
}

void TurnCtl::begin(const Vec3& w, float tol_rad, float max_seconds) {
    on = true; want = flat_dir(v3(0, 0, 0), w, v3(0, 0, 1)); tol = tol_rad; max_t = max_seconds; t = 0.0f;
    kicked = false; err_kick = yaw_kick = yaw_prev = 0.0f; wait = still = kicks = flips = 0;
}
int TurnCtl::step(const Vec3& f, float dt, float* deg) {
    *deg = 0.0f;
    if (!on) return 2;
    float e = err(f), y = yaw_of(f);
    if (fabsf(e) <= tol) { on = false; return 1; }
    t += dt;
    if (t >= max_t) { on = false; return 2; }
    if (kicked) {
        ++wait;
        still = fabsf(wrap_pi(y - yaw_prev)) < 0.009f ? still + 1 : 0;
        float total = wrap_pi(y - yaw_kick);
        if ((fabsf(total) > 0.02f && still >= 6) || wait > 30) {   // settled (or lost): judge it, then ask for the rest
            // the physics turned the other way: flip (judged only when the request was not a near half turn)
            if (fabsf(total) > 0.08f && fabsf(err_kick) < 2.6f && total * err_kick < 0.0f) { sign = -sign; ++flips; }
            kicked = false;
        }
    }
    yaw_prev = y;
    if (!kicked) { *deg = sign * e * 180.0f / PI; kicked = true; err_kick = e; yaw_kick = y; wait = 0; still = 0; ++kicks; }
    return 0;
}

static bool contains_ci(const char* s, const char* sub) {
    if (!s || !sub) return false;
    size_t n = strlen(sub);
    for (; *s; ++s) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)sub[i])) ++i;
        if (i == n) return true;
    }
    return false;
}
Pair pair_from_model(const char* m, bool* matched) {
    Pair p = PAIR_SOLDIER; bool ok = true;
    if (contains_ci(m, "dea_taskforce")) p = PAIR_DEA;
    else if (contains_ci(m, "civil")) p = PAIR_CIVIL;
    else if (!contains_ci(m, "soldier")) ok = false;
    if (matched) *matched = ok;
    return p;
}
Pair pair_by_limbs(float arm, float leg) {
    static const float ref[3][2] = {{0.4790f, 0.8936f}, {0.5317f, 0.9218f}, {0.5238f, 0.9727f}};   // soldier, civil, dea
    int best = 0; float bd = 1e30f;
    for (int i = 0; i < 3; ++i) { float da = arm - ref[i][0], dl = leg - ref[i][1], d = da * da + dl * dl; if (d < bd) { bd = d; best = i; } }
    return (Pair)best;
}
const char* pair_name(Pair p) { return p == PAIR_DEA ? "dea" : p == PAIR_CIVIL ? "civil" : "soldier"; }

const char* phase_name(int p) {
    static const char* n[] = {"off", "freeze", "intro", "gather", "dance", "plant", "dive", "after"};
    return (p >= 0 && p <= P_AFTER) ? n[p] : "?";
}
const char* shot_name(int s) {
    static const char* n[] = {"none", "push-in", "over Ram's shoulder", "two-shot", "high orbit", "low hero",
                              "over the partner's shoulder", "ring member's view", "wide", "plant close-up",
                              "low front", "partner looks down", "front dive"};
    return (s >= 0 && s < SH_COUNT) ? n[s] : "?";
}

// ------------------------------------------------------------------------------------------------ config
static void clampf(float& v, float lo, float hi) { if (!(v >= lo)) v = lo; if (v > hi) v = hi; }   // NaN -> lo
static void clampi(int& v, int lo, int hi) { if (v < lo) v = lo; if (v > hi) v = hi; }
int Cfg::segment_for(Pair p, bool matched) const {
    if (!music || music_segments <= 0) return 0;
    int s = !matched ? music_other : p == PAIR_DEA ? music_dea : p == PAIR_CIVIL ? music_civil : music_soldier;
    return (s >= 1 && s <= music_segments) ? s : 0;   // 0 = random (Suave::start picks one)
}
void Cfg::clamp() {
    clampf(grab_radius, 0.5f, 20.0f);
    clampf(pull_radius, 0.0f, 60.0f);
    clampi(ring_max, 0, MAX_RING);
    clampf(ring_radius, 1.0f, 20.0f);
    clampf(max_height, 0.5f, 50.0f);
    clampf(bpm, 40.0f, 240.0f);
    clampf(freeze_seconds, 0.0f, 3.0f);
    clampf(freeze_scale, 0.01f, 1.0f);
    clampf(freeze_ramp_in, 0.01f, 1.0f);
    clampf(freeze_ramp_out, 0.01f, 1.0f);
    clampf(intro_beats, 0.0f, 16.0f);
    clampf(gather_lead_beats, 0.0f, intro_beats);
    clampf(gather_beats, 0.0f, 16.0f);
    clampf(orbit_beats, 0.0f, 16.0f);
    clampf(cut_beats, 1.0f, 16.0f);
    clampf(dance_beats, 4.0f, 96.0f);
    clampi(dance_cycles, 1, 4);
    clampf(release_beat, 0.0f, dance_beats);
    clampf(dive_at_seconds, 0.2f, 6.0f);
    clampf(attach_seconds, 0.0f, dive_at_seconds); clampf(dynamite_show, 0.0f, 4.0f);
    clampf(push_seconds, 0.2f, 6.0f);
    clampf(intro_x, -3.0f, 3.0f); clampf(intro_z, -3.0f, 3.0f); clampf(face_offset_deg, -180.0f, 180.0f);
    clampf(closeup_seconds, 0.0f, dive_at_seconds);
    clampf(ending_scale, 0.0f, 1.0f);
    clampf(ending_ramp, 0.01f, 2.0f);
    clampf(front_distance, 1.5f, 10.0f); clampf(dive_cam_distance, 1.5f, 10.0f);
    clampf(plant_scale, 0.0f, 1.0f); clampf(dive_scale, 0.0f, 1.0f); clampf(takeoff_fallback, 0.05f, 3.0f); clampf(front_height, 0.3f, 3.0f);
    clampf(dive_wait, 0.2f, 10.0f);
    clampf(lookdown_real, 0.0f, 10.0f); clampf(lookdown_game, 0.0f, 5.0f);
    clampf(panic_min, 0.2f, 5.0f); clampf(panic_max, panic_min, 8.0f); clampf(panic_lift, 0.0f, 3.0f);
    clampf(panic_ram_speed, 0.05f, 1.0f); clampf(blast_after, 0.0f, 3.0f);
    clampf(boom_real, lookdown_real + panic_max + blast_after, 15.0f); clampf(boom_game, lookdown_game, 10.0f);
    clampf(land_hold, 0.0f, 5.0f);
    clampf(after_max, 0.5f, 10.0f);
    clampf(god_margin, 0.0f, 10.0f);
    clampf(arrive_distance, 0.1f, 3.0f);
    clampf(ring_phase_beats, 0.0f, 24.0f);
    clampf(camera_blend_back, 0.0f, 5.0f);
    clampf(face_stay, 0.5f, 30.0f);
    clampf(boom_radius, 1.0f, 30.0f);
    clampf(boom_damage, 0.0f, 100000.0f);
    clampf(boom_impulse, 0.0f, 50.0f);
    clampf(boom_slowmo, 0.0f, 1.0f);
    clampi(music_segments, 0, 9);
    clampi(music_soldier, 0, 9); clampi(music_civil, 0, 9); clampi(music_dea, 0, 9); clampi(music_other, 0, 9);
    clampf(partner_drift, 0.01f, 2.0f);
    clampf(partner_yaw_drift, 0.001f, 1.0f);
}

// ------------------------------------------------------------------------------------------------ selection
static bool eligible(const Cfg& c, const Vec3& ram, const Cand& k) {
    return k.hostile && k.alive && k.killable && k.has_ai && fabsf(k.pos.y - ram.y) <= c.max_height;
}
Choice choose(const Cfg& c, const Vec3& ram, const Cand* cand, int ncand) {
    Choice ch;
    if (ncand > MAX_CAND) ncand = MAX_CAND;
    float best = 1e30f;
    for (int i = 0; i < ncand; ++i) {
        if (!eligible(c, ram, cand[i])) continue;
        float d = dist(ram, cand[i].pos);
        if (d <= c.grab_radius && d < best) { best = d; ch.partner = i; }
    }
    if (ch.partner < 0) return ch;
    int idx[MAX_CAND]; float dd[MAX_CAND]; int m = 0;
    for (int i = 0; i < ncand; ++i) {
        if (i == ch.partner || !eligible(c, ram, cand[i])) continue;
        float d = dist(ram, cand[i].pos);
        if (d > c.pull_radius) continue;
        int j = m++;                                    // insertion sort by distance (ties keep input order)
        while (j > 0 && dd[j - 1] > d) { dd[j] = dd[j - 1]; idx[j] = idx[j - 1]; --j; }
        dd[j] = d; idx[j] = i;
    }
    for (int j = 0; j < m && ch.n < c.ring_max; ++j) ch.members[ch.n++] = idx[j];
    return ch;
}

void ring_slots(const Cfg& c, const Vec3& centre, const Vec3* p, int n, Vec3* slot, float* face) {
    if (n <= 0) return;
    if (n > MAX_RING) n = MAX_RING;
    float a[MAX_RING]; int order[MAX_RING];
    for (int i = 0; i < n; ++i) {
        float dx = p[i].x - centre.x, dz = p[i].z - centre.z;
        a[i] = (dx * dx + dz * dz > 1e-8f) ? atan2f(dx, dz) : 0.0f;
        order[i] = i;
    }
    for (int i = 1; i < n; ++i) {                       // sort member indices by angle (stable)
        int k = order[i], j = i;
        while (j > 0 && a[order[j - 1]] > a[k]) { order[j] = order[j - 1]; --j; }
        order[j] = k;
    }
    float step = 2.0f * PI / n, s = 0.0f, co = 0.0f;
    for (int j = 0; j < n; ++j) { float d = a[order[j]] - j * step; s += sinf(d); co += cosf(d); }
    float a0 = (fabsf(s) + fabsf(co) > 1e-6f) ? atan2f(s, co) : a[order[0]];
    for (int j = 0; j < n; ++j) {
        int i = order[j];
        float ang = a0 + j * step;
        slot[i].x = centre.x + c.ring_radius * sinf(ang);
        slot[i].z = centre.z + c.ring_radius * cosf(ang);
        slot[i].y = p[i].y;                             // keep the member's own height (the walk follows the ground)
        face[i] = yaw_to(slot[i], centre);
    }
}

// ------------------------------------------------------------------------------------------------ the move
int Suave::pick_segment(int nseg) {
    if (nseg <= 0) return 0;
    rng = rng * 1664525u + 1013904223u;
    int r = (int)((rng >> 8) % (uint32_t)nseg) + 1;
    if (nseg > 1 && r == last_segment) r = r % nseg + 1;   // never the same random segment twice in a row
    last_segment = r;
    return r;
}

float Suave::phase_len(const Cfg& c) const {
    switch (phase) {
    case P_FREEZE: return c.freeze_seconds;
    case P_INTRO:  return c.intro_end();
    case P_GATHER: return c.orbit_end() - c.intro_end();
    case P_DANCE:  return c.release_t() - c.orbit_end();
    case P_PLANT:  return c.dive_t() - c.release_t();
    case P_DIVE:   return c.dive_wait;
    case P_AFTER:  return c.after_max;
    }
    return 0.0f;
}

static float circle_pos(const Cfg& c, float clock, int j) {
    float b = clock / c.beat() + c.ring_phase_beats * j;
    float x = fmodf(b, c.dance_beats) / c.dance_beats;
    return x < 0.0f ? x + 1.0f : x;
}

// ---- camera director
bool Suave::shot_eye(const Cfg& c, int sh, const Snap& s, Vec3* eye, int* tgt, float* fov) const {
    Vec3 f = s.ram_fwd; { float l = sqrtf(f.x * f.x + f.z * f.z); f = l > 1e-4f ? v3(f.x / l, 0, f.z / l) : v3(0, 0, 1); }
    Vec3 r = right_of(f), up = v3(0, 1, 0);
    Vec3 body = s.partner_body;
    Vec3 mid = mul(add(s.ram, body), 0.5f);
    switch (sh) {
    case SH_PUSHIN: {                                   // 3.2 m -> 1.7 m in front of Ram's face over the push-in
        float span = c.freeze_seconds + 2.0f * c.beat();
        float k = span > 0 ? shot_t / span : 1.0f; if (k > 1) k = 1; if (k < 0) k = 0;
        k = k * k * (3 - 2 * k);
        *eye = add(add(s.ram, mul(f, 3.2f - 1.5f * k)), mul(up, 1.55f)); *tgt = TGT_RAM_HEAD; *fov = 45.0f; return true; }
    case SH_OTS_PARTNER:
        *eye = add(add(add(s.ram, mul(f, -1.1f)), mul(r, 0.45f)), mul(up, 1.75f)); *tgt = TGT_PARTNER_HEAD; *fov = 50.0f; return true;
    case SH_TWO:
        *eye = add(add(mid, mul(r, 3.2f)), mul(up, 1.45f)); *tgt = TGT_RAM_CHEST; *fov = 55.0f; return true;
    case SH_ORBIT: {
        float R = ring_r * 2.4f; if (R < 7.0f) R = 7.0f;
        float a = orbit_a0 + 0.22f * shot_t;            // ~13 deg per second
        *eye = add(add(centre, v3(R * sinf(a), 0, R * cosf(a))), mul(up, 5.5f)); *tgt = TGT_RAM_PELVIS; *fov = 60.0f; return true; }
    case SH_HERO_LOW:
        *eye = add(add(add(mid, mul(r, 2.4f)), mul(f, 1.0f)), mul(up, 0.35f)); *tgt = TGT_RAM_HEAD; *fov = 55.0f; return true;
    case SH_OTS_RAM: {
        Vec3 d = flat_dir(s.ram, body, f);
        *eye = add(add(add(body, mul(d, 0.9f)), mul(right_of(d), 0.35f)), mul(up, 1.7f)); *tgt = TGT_RAM_HEAD; *fov = 50.0f; return true; }
    case SH_MEMBER_POV: {
        if (pov_member < 0 || pov_member >= n) return false;
        Vec3 m = s.member[pov_member];
        *eye = add(add(m, mul(flat_dir(m, centre, f), 0.4f)), mul(up, 1.6f)); *tgt = TGT_RAM_CHEST; *fov = 55.0f; return true; }
    case SH_WIDE: {
        float R = ring_r + 5.0f;
        *eye = add(add(centre, mul(f, -R)), mul(up, 3.0f)); *tgt = TGT_RAM_PELVIS; *fov = 60.0f; return true; }
    case SH_PLANT_CLOSE: {   // fifth test: the bundle is strapped to the partner's CHEST - a three-quarter view at chest
        Vec3 chest = (s.partner_chest.x != 0 || s.partner_chest.z != 0) ? s.partner_chest : add(body, v3(0, 0.35f, 0));
        Vec3 d = flat_dir(s.ram, chest, f);
        Vec3 toward = mul(d, -1.0f);                     // from the partner toward Ram (his front during the push)
        // 1.25 m from the chest, mostly to the side so the eye is clear of Ram (who stands in front of him)
        *eye = add(add(add(chest, mul(toward, 1.3f)), mul(right_of(d), 1.1f)), mul(up, 0.15f)); *tgt = TGT_PARTNER_CHEST; *fov = 42.0f; return true; }
    case SH_FRONT_LOW:
        *eye = front_eye; *tgt = TGT_RAM_CHEST; *fov = 60.0f; return true;
    case SH_LOOKDOWN:
        *eye = lookdown_eye; *tgt = TGT_PARTNER_HEAD; *fov = 40.0f; return true;
    case SH_FRONT_DIVE:
        *eye = add(add(s.ram, mul(cam_dir.x != 0 || cam_dir.z != 0 ? cam_dir : dive_dir, c.dive_cam_distance)), mul(up, c.front_height)); *tgt = TGT_RAM_CHEST; *fov = 62.0f; return true;
    }
    return false;
}
void Suave::camera(const Cfg& c, const Snap& s, Acts& a, int want, float blend) {
    if (!c.camera || !cam_taken) return;
    if (want == SH_MEMBER_POV) {                        // the first clapping member; else the wide shot
        pov_member = -1;
        for (int j = 0; j < n; ++j) { int k = (cut_index + j) % (n > 0 ? n : 1); if (mstate[k] == M_CLAP && s.member_alive[k]) { pov_member = k; break; } }
        if (pov_member < 0) want = SH_WIDE;
    }
    bool sw = want != shot;
    if (sw) { shot = want; shot_t = 0.0f; }
    if (shot == SH_NONE) return;
    bool moving = shot == SH_PUSHIN || shot == SH_ORBIT || shot == SH_FRONT_DIVE || shot == SH_PLANT_CLOSE;
    if (!sw && !moving) return;
    Vec3 e; int t = 0; float fov = 60.0f;
    if (!shot_eye(c, shot, s, &e, &t, &fov)) return;
    Act& x = a.add(A_CAM, shot, t);
    x.f[0] = e.x; x.f[1] = e.y; x.f[2] = e.z; x.f[3] = sw ? blend : -1.0f; x.f[4] = fov;
}
void Suave::face(const Cfg& c, Acts& a, int who, int f) {
    if (!c.faces) return;
    a.add(A_FACE, who, f).f[0] = c.face_stay;
}

Acts Suave::start(const Cfg& c, const Snap& s, Pair p, bool matched, const Vec3* members, int nm, uint32_t seed) {
    Acts a;
    rng ^= seed;
    phase = P_FREEZE; real_t = 0.0f; this->c = 0.0f; phase_t = 0.0f;
    pair = p; reason = ""; dive_dir = cam_dir = v3(0, 0, 0); armed = dive_final = ending_on = false;
    // the shared node: Ram turned toward the partner (minus the intro's start angle), moved so his intro Bip starts on
    // his own spot; the ring and the pair centre on it
    float yaw = yaw_to(s.ram, s.partner) - c.face_offset_deg * PI / 180.0f;
    Vec3 fy = v3(sinf(yaw), 0, cosf(yaw)), ry = right_of(fy);
    centre = c.intro_place ? sub(sub(s.ram, mul(ry, c.intro_x)), mul(fy, c.intro_z)) : s.ram;
    attached = boomed = partner_frozen = dance_started = ring_started = slowmo_on = false;
    ram_released = lookdown_done = back_done = landed = lookdown_played = false;
    dive_real = dive_game = air_real = air_game = boom_real = land_real = -1.0f;
    ram_slowed = false; panic_t = back_t = -1.0f;
    shot = SH_NONE; shot_t = 0.0f; cut_index = 0; pov_member = -1; face_step = 0;
    orbit_a0 = yaw_of(s.ram_fwd) + 0.75f * PI;
    n = nm < 0 ? 0 : (nm > MAX_RING ? MAX_RING : nm);
    // a tight space (fifth test, rooms): the ring pulls in to stay clear of the nearest wall, fewer members on a small one
    ring_r = c.ring_radius;
    if (s.free_min < ring_r + 0.7f) ring_r = s.free_min - 0.7f;
    if (ring_r < 1.6f) ring_r = 1.6f;
    if (ring_r < 2.4f && n > 4) n = 4;
    { Cfg cc = c; cc.ring_radius = ring_r; ring_slots(cc, centre, members, n, slot, slot_yaw); }
    segment = c.segment_for(p, matched);
    if (c.music && c.music_segments > 0 && !segment) segment = pick_segment(c.music_segments);
    ++uses;
    a.add(A_GOD_ON).f[0] = c.god_seconds();
    a.add(A_HUD_START);
    if (segment) a.add(A_STOP_OTHER_MUSIC);
    Act& r = a.add(A_RAM_TAKE); r.f[0] = centre.x; r.f[1] = centre.y; r.f[2] = centre.z; r.f[3] = yaw;
    a.add(A_PARTNER_TAKE);
    ram_taken = partner_taken = true;
    for (int j = 0; j < n; ++j) { a.add(A_MEMBER_TAKE, j + 1); mstate[j] = M_WAIT; }
    for (int j = n; j < MAX_RING; ++j) mstate[j] = M_NONE;
    a.add(A_PLAY, -1, CLIP_INTRO);                      // the intro's first pose shows during the freeze
    a.add(A_PLAY, 0, CLIP_GRABBED);
    face(c, a, -1, FACE_SLICKSMILE);
    face(c, a, 0, FACE_SURPRISE);
    Act& f = a.add(A_FREEZE_ON); f.f[0] = c.freeze_scale; f.f[1] = c.freeze_ramp_in;
    if (c.lock_input) { a.add(A_INPUT_LOCK, 0, 1); input_locked = true; }
    if (c.camera) { a.add(A_CAM_TAKE); cam_taken = true; camera(c, s, a, SH_PUSHIN, 0.0f); }
    else cam_taken = false;
    return a;
}

Acts Suave::abort(const Cfg& c, const char* why, bool hard) {
    Acts a;
    if (phase == P_OFF) return a;
    reason = why;
    if (phase == P_FREEZE) a.add(A_FREEZE_OFF).f[1] = c.freeze_ramp_out;
    if (slowmo_on) { Act& x = a.add(A_SLOWMO); x.f[0] = 1.0f; x.f[1] = 0.2f; slowmo_on = false; }
    if (segment) a.add(A_MUSIC_STOP);
    float used = (phase == P_FREEZE) ? real_t : c.freeze_seconds + this->c;
    float left = c.god_seconds() - used;
    if (left > 0.0f) a.add(A_GOD_CUT).f[0] = left;
    a.add(A_HUD_END);
    if (cam_taken) { a.add(A_CAM_RELEASE).f[0] = 0.3f; cam_taken = false; }
    if (!hard) {
        if (ram_taken && !ram_released) a.add(A_RAM_RELEASE);
        if (phase >= P_PLANT && !boomed) a.add(A_DYNAMITE_DROP);
        if (partner_taken && !boomed) a.add(A_PARTNER_RELEASE);
        for (int j = 0; j < n; ++j) if (mstate[j] == M_WAIT || mstate[j] == M_WALK || mstate[j] == M_CLAP) { a.add(A_MEMBER_RELEASE, j + 1); mstate[j] = M_RELEASED; }
    }
    if (input_locked) { a.add(A_INPUT_LOCK, 0, 0); input_locked = false; }
    a.add(A_END, 0, 1);
    ++aborts;
    phase = P_OFF; ram_taken = partner_taken = false; shot = SH_NONE;
    return a;
}

void Suave::end_common(const Cfg& c, const Snap& s, Acts& a) {
    if (ram_slowed) { Act& x = a.add(A_RAM_SPEED); x.f[0] = -1.0f; x.f[1] = 0.2f; ram_slowed = false; }
    if (cam_taken) { a.add(A_CAM_RELEASE).f[0] = c.camera_blend_back; cam_taken = false; }
    if (slowmo_on) { Act& x = a.add(A_SLOWMO); x.f[0] = 1.0f; x.f[1] = 0.4f; slowmo_on = false; }
    for (int j = 0; j < n; ++j)          // survivors of the boom (immune, out of reach) get their AI back
        if ((mstate[j] == M_WAIT || mstate[j] == M_WALK || mstate[j] == M_CLAP) && s.member_alive[j]) { a.add(A_MEMBER_RELEASE, j + 1); mstate[j] = M_RELEASED; }
    if (s.partner_alive) a.add(A_PARTNER_RELEASE);
    float left = c.god_seconds() - (c.freeze_seconds + this->c);
    if (left > 0.0f) a.add(A_GOD_CUT).f[0] = left;     // the armor ends now (after the blast)
    a.add(A_HUD_END);
    if (input_locked) { a.add(A_INPUT_LOCK, 0, 0); input_locked = false; }
    a.add(A_END, 0, 0);
    phase = P_OFF; partner_taken = false; shot = SH_NONE;
}

Acts Suave::tick(const Cfg& c, const Snap& s, float dt) {
    Acts a;
    if (phase == P_OFF) return a;
    if (s.level_changed) return abort(c, "level changed", true);
    if (s.rewind) return abort(c, "rewind started", true);
    if (!s.ok) return abort(c, "player not in control", false);
    if (s.cutscene) return abort(c, "cut-scene", false);
    if (s.vehicle) return abort(c, "player in a vehicle", false);
    if (s.player_dead) return abort(c, "player dead", false);
    if (c.abort_on_partner_death && !s.partner_alive && phase < P_PLANT) return abort(c, "partner died before the plant", false);
    if (s.camera_lost && cam_taken) { cam_taken = false; shot = SH_NONE; }   // another system took it: leave it
    if (s.paused) return a;
    if (!(dt >= 0.0f)) dt = 0.0f; if (dt > 0.1f) dt = 0.1f;
    float tm = s.time_mult; if (!(tm >= 0.0f)) tm = 1.0f; if (tm > 2.0f) tm = 2.0f;
    real_t += dt;
    shot_t += dt;                                       // camera moves on real time (the camera runs in real time)
    for (int j = 0; j < n; ++j)
        if ((mstate[j] == M_WAIT || mstate[j] == M_WALK || mstate[j] == M_CLAP) && !s.member_alive[j]) mstate[j] = M_DEAD;

    if (phase == P_FREEZE) {
        phase_t += dt;
        camera(c, s, a, SH_PUSHIN, 0.0f);
        if (phase_t + 1e-6f < c.freeze_seconds) return a;
        a.add(A_FREEZE_OFF).f[1] = c.freeze_ramp_out;
        if (segment) a.add(A_MUSIC_START, 0, segment);
        phase = P_INTRO; phase_t = 0.0f; this->c = 0.0f;
        return a;
    }
    float gdt = dt * tm;
    this->c += gdt; phase_t += gdt;
    const float clock = this->c, B = c.beat();

    // partner stays on Ram until the release, then where the push leaves him
    if (!partner_frozen && s.partner_alive && (dist(s.partner, s.ram) > c.partner_drift || s.partner_yaw_err > c.partner_yaw_drift))
        a.add(A_PARTNER_PLACE);

    // ring: walk in from gather_start, arrive or get placed at the deadline, then clap
    if (!ring_started && clock >= c.gather_start()) {
        ring_started = true;
        for (int j = 0; j < n; ++j) if (mstate[j] == M_WAIT) {
            Act& w = a.add(A_MEMBER_WALK, j + 1); w.f[0] = slot[j].x; w.f[1] = slot[j].y; w.f[2] = slot[j].z; mstate[j] = M_WALK;
        }
    }
    for (int j = 0; j < n; ++j) {
        if (mstate[j] != M_WALK) continue;
        bool arrived = dist_xz(s.member[j], slot[j]) <= c.arrive_distance;
        bool late = clock >= c.gather_end() && c.place_late_members;
        if (!arrived && !late) continue;
        Act& m = a.add(A_MEMBER_ARRIVE, j + 1, arrived ? 0 : 1);
        m.f[0] = slot[j].x; m.f[1] = slot[j].y; m.f[2] = slot[j].z; m.f[3] = slot_yaw[j];
        a.add(A_PLAY, j + 1, CLIP_CIRCLE).f[0] = circle_pos(c, clock, j);
        face(c, a, j + 1, FACE_JOY);
        mstate[j] = M_CLAP;
    }
    // the dance takes over from the intro's last frame
    if (!dance_started && clock >= c.intro_end()) {
        dance_started = true;
        a.add(A_PLAY, -1, CLIP_LEAD);
        a.add(A_PLAY, 0, CLIP_FOLLOW);
    }
    // faces until the release: intro steps every 2 beats, then every cut
    if (phase <= P_DANCE && clock < c.release_t()) {
        int stepi = clock < c.intro_end() ? (int)(clock / (2.0f * B)) : 3 + (int)((clock - c.intro_end()) / (c.cut_beats * B));
        if (stepi != face_step) {
            face_step = stepi;
            if (stepi == 1) face(c, a, 0, FACE_DISBELIEF);
            else if (stepi == 2) face(c, a, 0, FACE_JOY);
            else if (stepi >= 3) {
                face(c, a, -1, (stepi & 1) ? FACE_JOY : FACE_SLICKSMILE);
                face(c, a, 0, stepi < 5 ? FACE_DISBELIEF : FACE_JOY);
            }
        }
    }

    // the partner looks down at the dynamite right after the stumble's last frame (lookdown frame 0 = stumble end)
    if (!lookdown_played && phase >= P_PLANT && clock >= c.release_t() + c.push_seconds) {
        lookdown_played = true;
        if (s.partner_alive) a.add(A_PLAY, 0, CLIP_LOOKDOWN);
        face(c, a, 0, FACE_SURPRISE);
    }

    switch (phase) {
    case P_INTRO:
        if (clock < 2.0f * B) camera(c, s, a, SH_PUSHIN, 0.0f);
        else if (clock < 4.0f * B) camera(c, s, a, SH_OTS_PARTNER, 0.0f);
        else camera(c, s, a, SH_TWO, 0.0f);
        if (clock >= c.intro_end()) { phase = P_GATHER; phase_t = 0.0f; camera(c, s, a, SH_ORBIT, 0.6f); }
        break;
    case P_GATHER:
        camera(c, s, a, SH_ORBIT, 0.6f);
        if (clock >= c.orbit_end()) { phase = P_DANCE; phase_t = 0.0f; cut_index = -1; }
        break;
    case P_DANCE: {
        int k = (int)((clock - c.orbit_end()) / (c.cut_beats * B));
        static const int cycle[4] = {SH_HERO_LOW, SH_OTS_RAM, SH_MEMBER_POV, SH_WIDE};
        if (k != cut_index) { cut_index = k; camera(c, s, a, cycle[(k < 0 ? 0 : k) % 4], 0.0f); }
        if (c.dynamite && !armed && clock >= c.release_t() - c.dynamite_show) { a.add(A_DYNAMITE_ARM); armed = true; }   // Ram shows it
        if (clock >= c.release_t()) {
            a.add(A_PLAY, -1, CLIP_PUSH);
            a.add(A_PLAY, 0, CLIP_STUMBLE);
            a.add(A_PARTNER_FREEZE); partner_frozen = true;
            if (c.dynamite && !armed) { a.add(A_DYNAMITE_ARM); armed = true; }
            face(c, a, -1, FACE_SLICKSMILE);
            face(c, a, 0, FACE_JOY);
            phase = P_PLANT; phase_t = 0.0f;
            camera(c, s, a, SH_PLANT_CLOSE, 0.0f);
        }
        break; }
    case P_PLANT:
        if (c.dynamite && !attached && clock >= c.attach_t()) {
            a.add(A_DYNAMITE_ATTACH); attached = true;
            if (c.plant_scale > 0.0f && c.plant_scale < 1.0f && !slowmo_on) {   // the plant reads: slow motion on the close-up
                Act& x = a.add(A_SLOWMO); x.f[0] = c.plant_scale; x.f[1] = 0.15f; slowmo_on = true;
            }
        }
        if (clock >= c.closeup_end() && !ending_on) {   // the ending slow motion; the close-up stays until the dive is final
            ending_on = true;
            dive_dir = cam_dir = flat_dir(v3(0, 0, 0), mul(s.ram_fwd, -1.0f), mul(s.ram_fwd, -1.0f));   // behind Ram = away from the partner (planned)
            if (c.ending_scale > 0.0f && c.ending_scale < 1.0f) {
                Act& x = a.add(A_SLOWMO); x.f[0] = c.ending_scale; x.f[1] = c.ending_ramp; slowmo_on = true;
            }
        }
        if (shot == SH_PLANT_CLOSE) camera(c, s, a, SH_PLANT_CLOSE, 0.0f);
        if (clock >= c.dive_t()) {
            if (c.dynamite && !attached) { a.add(A_DYNAMITE_ATTACH); attached = true; }
            if (dive_dir.x == 0 && dive_dir.z == 0) dive_dir = cam_dir = flat_dir(v3(0, 0, 0), mul(s.ram_fwd, -1.0f), mul(s.ram_fwd, -1.0f));   // behind Ram = away from the partner
            a.add(A_RAM_RELEASE); ram_taken = false; ram_released = true;
            if (c.auto_dive) {
                Act& d = a.add(A_AUTO_DIVE); d.f[0] = dive_dir.x; d.f[1] = 0; d.f[2] = dive_dir.z;
                face(c, a, -1, FACE_SLICKSMILE);
            } else {
                if (input_locked) { a.add(A_INPUT_LOCK, 0, 0); input_locked = false; }
                if (cam_taken) { a.add(A_CAM_RELEASE).f[0] = 0.3f; cam_taken = false; shot = SH_NONE; }   // the player's dive
            }
            dive_real = dive_game = 0.0f;
            phase = P_DIVE; phase_t = 0.0f;
        }
        break;
    case P_DIVE: {
        if (s.dive_turning && air_real < 0.0f) {         // Ram still turning: the dive clock (and its fallbacks) wait
            if (shot == SH_PLANT_CLOSE) camera(c, s, a, SH_PLANT_CLOSE, 0.0f);
            break;
        }
        if (s.dive_known && !dive_final) {               // the key the glue chose: the shots follow the real dive
            dive_final = true;
            float l = sqrtf(s.dive_world.x * s.dive_world.x + s.dive_world.z * s.dive_world.z);
            float m = sqrtf(s.dive_cam.x * s.dive_cam.x + s.dive_cam.z * s.dive_cam.z);
            if (l > 1e-3f) dive_dir = v3(s.dive_world.x / l, 0, s.dive_world.z / l);
            if (m > 1e-3f) cam_dir = v3(s.dive_cam.x / m, 0, s.dive_cam.z / m);
            // the camera goes where the dive goes, AFTER it is final: ahead of Ram, looking back at him and the partner
            front_eye = add(add(s.ram, mul(cam_dir, c.front_distance)), v3(0, c.front_height, 0));
            if (shot != SH_LOOKDOWN && shot != SH_FRONT_DIVE) { shot = SH_NONE; camera(c, s, a, SH_FRONT_LOW, 0.0f); }
        }
        dive_real += dt; dive_game += gdt;
        // takeoff: the dive's controller seen, or (detection failed) a short fixed time after the dive keys
        bool takeoff = s.ram_diving || (c.auto_dive && dive_real >= c.takeoff_fallback);
        if (air_real < 0.0f && takeoff) {
            if (c.dive_scale > 0.0f && c.dive_scale < 1.0f) { Act& x = a.add(A_SLOWMO); x.f[0] = c.dive_scale; x.f[1] = 0.1f; slowmo_on = true; }
            air_real = air_game = 0.0f;
            if (input_locked) { a.add(A_INPUT_LOCK, 0, 0); input_locked = false; }   // shoot while airborne
        } else if (air_real >= 0.0f) { air_real += dt; air_game += gdt; }
        bool air = air_real >= 0.0f;
        if (air && !lookdown_done && (air_real >= c.lookdown_real || air_game >= c.lookdown_game)) {
            lookdown_done = true; panic_t = 0.0f;
            if (!lookdown_played) { lookdown_played = true; if (s.partner_alive) a.add(A_PLAY, 0, CLIP_LOOKDOWN); face(c, a, 0, FACE_SURPRISE); }
            for (int j = 0; j < n; ++j) if (mstate[j] == M_CLAP) face(c, a, j + 1, FACE_SURPRISE);
            lookdown_eye = add(add(s.partner_body, mul(flat_dir(s.partner_body, s.ram, s.ram_fwd), 1.5f)), v3(0, 1.5f, 0));
            camera(c, s, a, SH_LOOKDOWN, 0.0f);
        }
        bool up = s.ram_height >= c.panic_lift;
        if (lookdown_done && !back_done) {
            panic_t += dt;
            // Ram airborne during the panic cut: his own animation slows so he is still in the air after it
            if (up && !ram_slowed && c.panic_ram_speed < 1.0f) { Act& x = a.add(A_RAM_SPEED); x.f[0] = c.panic_ram_speed; x.f[1] = 0.15f; ram_slowed = true; }
            if (panic_t >= c.panic_max || (panic_t >= c.panic_min && up)) {
                back_done = true; back_t = 0.0f;
                face(c, a, 0, FACE_PAIN);
                camera(c, s, a, SH_FRONT_DIVE, 0.0f);
                if (ram_slowed) { Act& x = a.add(A_RAM_SPEED); x.f[0] = -1.0f; x.f[1] = 0.5f; ram_slowed = false; }
            }
        } else if (back_done) {
            back_t += dt;
            if (shot == SH_FRONT_DIVE) camera(c, s, a, SH_FRONT_DIVE, 0.0f);
        }
        bool boom = (air && back_done && back_t >= c.blast_after) ||
                    (air && (air_real >= c.boom_real || air_game >= c.boom_game)) ||
                    (!air && (dive_game >= c.dive_wait || dive_real >= 4.0f));
        if (boom) {
            if (!air && input_locked) { a.add(A_INPUT_LOCK, 0, 0); input_locked = false; }   // no takeoff: keys back
            if (shot != SH_FRONT_DIVE) camera(c, s, a, SH_FRONT_DIVE, 0.0f);
            int mask = s.partner_alive ? 1 : 0;
            for (int j = 0; j < n; ++j) if (mstate[j] == M_WAIT || mstate[j] == M_WALK || mstate[j] == M_CLAP) mask |= 1 << (j + 1);
            a.add(A_BOOM, 0, mask);
            boomed = true; boom_real = 0.0f;
            phase = P_AFTER; phase_t = 0.0f;
        }
        break; }
    case P_AFTER:
        boom_real += dt;
        if (shot == SH_FRONT_DIVE) camera(c, s, a, SH_FRONT_DIVE, 0.0f);
        if (!landed && air_real >= 0.0f && !s.ram_diving) { landed = true; land_real = 0.0f; face(c, a, -1, FACE_JOY); }
        else if (landed) land_real += dt;
        if ((landed && land_real >= c.land_hold) || boom_real >= c.after_max || (air_real < 0.0f && boom_real >= c.land_hold + 0.5f))
            end_common(c, s, a);
        break;
    }
    return a;
}

} // namespace sv
