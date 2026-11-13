// gp_hud_logic.cpp - pure logic of [minimap] (see gp_hud_logic.h)
#include "gp_hud_logic.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace hud {

static const float PI = 3.14159265358979f;

V2 to_radar(const Frame& f, float dx, float dz, float k) {
    float side = dx * f.fz - dz * f.fx;      // along right = (fz, -fx)
    float fwd = dx * f.fx + dz * f.fz;
    return V2{side * k, -fwd * k};
}
V2 from_radar(const Frame& f, float ux, float uy, float k) {
    float side = ux / k, fwd = -uy / k;
    return V2{side * f.fz + fwd * f.fx, -side * f.fx + fwd * f.fz};
}
Frame frame_from_view(float vx, float vz, float px, float pz) {
    Frame f; float l = sqrtf(vx * vx + vz * vz);
    if (l > 1e-6f) { f.fx = vx / l; f.fz = vz / l; }
    f.px = px; f.pz = pz;
    return f;
}
Frame frame_north(float px, float pz) { Frame f; f.fx = 0.0f; f.fz = 1.0f; f.px = px; f.pz = pz; return f; }
V2 view_to_north(const Frame& view, float ux, float uy) {
    V2 w = from_radar(view, ux, uy, 1.0f);
    return to_radar(frame_north(0, 0), w.x, w.y, 1.0f);
}

Disc disc_from_quad(const float P[4][2], const float UV[4][2]) {
    Disc d = {0, 0, 0, 0, false};
    int a = 0, b = -1, c = -1;
    for (int i = 1; i < 4; ++i) if (UV[i][0] + UV[i][1] < UV[a][0] + UV[a][1]) a = i;
    // b: same v as a, largest u; c: same u as a, largest v
    for (int i = 0; i < 4; ++i) {
        if (i == a) continue;
        if (fabsf(UV[i][1] - UV[a][1]) < 1e-4f && (b < 0 || UV[i][0] > UV[b][0])) b = i;
        if (fabsf(UV[i][0] - UV[a][0]) < 1e-4f && (c < 0 || UV[i][1] > UV[c][1])) c = i;
    }
    if (b < 0 || c < 0) return d;
    float du = UV[b][0] - UV[a][0], dv = UV[c][1] - UV[a][1];
    if (fabsf(du) < 1e-6f || fabsf(dv) < 1e-6f) return d;
    float ex[2] = {(P[b][0] - P[a][0]) / du, (P[b][1] - P[a][1]) / du};    // pixels per one u
    float ey[2] = {(P[c][0] - P[a][0]) / dv, (P[c][1] - P[a][1]) / dv};
    float tu = DISC_CX / RADAR_TEX - UV[a][0], tv = DISC_CY / RADAR_TEX - UV[a][1];
    d.cx = P[a][0] + ex[0] * tu + ey[0] * tv;
    d.cy = P[a][1] + ex[1] * tu + ey[1] * tv;
    float wpx = sqrtf(ex[0] * ex[0] + ex[1] * ex[1]);       // one full texture width in pixels
    d.unit = wpx / RADAR_TEX;
    d.r = DISC_R * d.unit;
    d.ok = d.unit > 0.01f && d.unit < 100.0f;
    return d;
}

bool parse_map_line(const char* line, MapInfo* m) {
    while (*line == ' ' || *line == '\t') ++line;
    if (!*line || *line == '#' || *line == '\r' || *line == '\n') return false;
    memset(m, 0, sizeof *m);
    int n = sscanf_s(line, "%47s %f %f %f %f %d %d %63s", m->name, (unsigned)sizeof m->name, &m->x0, &m->z0, &m->x1, &m->z1,
                     &m->w, &m->h, m->file, (unsigned)sizeof m->file);
    if (n != 8 || !(m->x1 > m->x0) || !(m->z1 > m->z0) || m->w <= 0 || m->h <= 0) return false;
    size_t l = strlen(m->file);
    m->pack = l > 4 && !strcmp(m->file + l - 4, ".mmp");
    return m->pack ? (m->w <= 65536 && m->h <= 65536) : (m->w <= 8192 && m->h <= 8192);
}
V2 map_uv(const MapInfo& m, float x, float z) { return V2{(x - m.x0) / (m.x1 - m.x0), (m.z1 - z) / (m.z1 - m.z0)}; }
int pick_map(const MapInfo* maps, int n, float x, float z) {
    int best = -1; float best_in = 0.0f, best_area = 0.0f;
    for (int i = 0; i < n; ++i) {
        const MapInfo& m = maps[i];
        if (x < m.x0 || x > m.x1 || z < m.z0 || z > m.z1) continue;
        float in = fminf(fminf(x - m.x0, m.x1 - x), fminf(z - m.z0, m.z1 - z));
        float area = (m.x1 - m.x0) * (m.z1 - m.z0);
        bool take = best < 0 || area < best_area * 0.8f || (area < best_area * 1.25f && in > best_in);
        if (take) { best = i; best_in = in; best_area = area; }
    }
    return best;
}

bool parse_occ(const char* t, Occ* o) {
    int used = 0;
    if (sscanf_s(t, "occ %f %d %d %f %f%n", &o->cell, &o->cols, &o->rows, &o->x0, &o->z1, &used) != 5) return false;
    if (!(o->cell > 0.5f) || o->cols <= 0 || o->rows <= 0 || o->cols > 4096 || o->rows > 4096) return false;
    o->bits = new unsigned char[(size_t)o->cols * o->rows];
    const char* p = t + used; int k = 0;
    while (*p && k < o->cols * o->rows) { if (*p == '0' || *p == '1') o->bits[k++] = (unsigned char)(*p - '0'); ++p; }
    if (k != o->cols * o->rows) { delete[] o->bits; o->bits = nullptr; return false; }
    return true;
}
bool occ_near(const Occ& o, float x, float z) {
    if (!o.bits) return false;
    int c = (int)floorf((x - o.x0) / o.cell), r = (int)floorf((o.z1 - z) / o.cell);
    for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -1; dc <= 1; ++dc) {
            int rr = r + dr, cc = c + dc;
            if (rr >= 0 && rr < o.rows && cc >= 0 && cc < o.cols && o.bits[rr * o.cols + cc]) return true;
        }
    return false;
}
int pick_map_occ(const MapInfo* maps, const Occ* occ, int n, float x, float z) {
    int best = -1; float best_area = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (!occ || !occ_near(occ[i], x, z)) continue;
        float area = (maps[i].x1 - maps[i].x0) * (maps[i].z1 - maps[i].z0);
        if (best < 0 || area < best_area) { best = i; best_area = area; }
    }
    return best >= 0 ? best : pick_map(maps, n, x, z);
}

bool is_objective(int t) { return t == 1 || t == 2 || (t >= 6 && t <= 10) || t == 12 || t == 14; }
int objective_rank(int t) {
    switch (t) {
    case 2: return 0;          // waypoint: where the mission sends you
    case 12: return 1;         // primary gate
    case 7: case 6: return 2;  // use / blow up
    case 8: case 9: return 3;  // boss / enemy objective
    case 10: return 4;         // ally objective
    case 14: return 5;         // tutorial area
    case 1: return 6;          // next story mission (free roam)
    default: return 9;
    }
}
uint32_t objective_colour(int t) {
    switch (t) {
    case 8: case 9: case 14: return 0xFFE8442E;   // red (the radar's enemy / boss / tutorial target)
    case 10: return 0xFF86C83F;                   // green (friendly object)
    case 2: return 0xFFFFF1C4;                    // cream white (the waypoint glow)
    default: return 0xFFFFC62E;                   // gold (primary objectives, story mission)
    }
}
int format_distance(float m, char* out, int cap) {
    if (!(m >= 0.0f)) m = 0.0f;
    if (m < 999.5f) return snprintf(out, cap, "%d m", (int)(m + 0.5f));
    if (m < 9950.0f) return snprintf(out, cap, "%.1f km", m / 1000.0f);
    return snprintf(out, cap, "%d km", (int)(m / 1000.0f + 0.5f));
}

bool project(const Cam& c, V3 p, float* sx, float* sy, float* depth) {
    const float* r = c.m; const float* u = c.m + 4; const float* f = c.m + 8; const float* o = c.m + 12;
    float dx = p.x - o[0], dy = p.y - o[1], dz = p.z - o[2];
    auto len = [](const float* v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); return l > 1e-6f ? l : 1.0f; };
    float vx = (dx * r[0] + dy * r[1] + dz * r[2]) / len(r);
    float vy = (dx * u[0] + dy * u[1] + dz * u[2]) / len(u);
    float vz = (dx * f[0] + dy * f[1] + dz * f[2]) / len(f);
    float fov = c.fov_deg > 1.0f && c.fov_deg < 179.0f ? c.fov_deg : 64.0f;
    float ty = 0.75f * tanf(fov * 0.5f * PI / 180.0f);            // tan(vfov / 2), vfov = 2 atan(0.75 tan(fov / 2))
    float A = c.wide_flag ? 16.0f / 9.0f : (c.H > 0 && c.ratioxy > 0.01f ? c.W / c.H / c.ratioxy : 4.0f / 3.0f);
    *depth = vz;
    const float NEAR_Z = 0.05f;
    if (vz <= NEAR_Z) {
        // behind: a direction only (left / right of the camera, pointing down the screen)
        float hx = vx, hy = fabsf(vz) * 0.25f + fabsf(vy) * 0.1f + 0.01f;
        float l = sqrtf(hx * hx + hy * hy);
        *sx = c.W * 0.5f + hx / l * c.W; *sy = c.H * 0.5f + hy / l * c.H;
        return false;
    }
    float nx = vx / (vz * A * ty), ny = vy / (vz * ty);
    *sx = (nx + 1.0f) * 0.5f * c.W;
    *sy = (1.0f - ny) * 0.5f * c.H;
    return true;
}
V2 clamp_to_screen(float sx, float sy, bool behind, float W, float H, float margin, bool* edge, float* angle) {
    float cx = W * 0.5f, cy = H * 0.5f;
    float dx = sx - cx, dy = sy - cy;
    *angle = atan2f(dy, dx);
    if (!behind && sx >= margin && sx <= W - margin && sy >= margin && sy <= H - margin) { *edge = false; return V2{sx, sy}; }
    *edge = true;
    if (fabsf(dx) < 1e-3f && fabsf(dy) < 1e-3f) { dx = 0.0f; dy = 1.0f; *angle = PI * 0.5f; }
    float hx = cx - margin, hy = cy - margin;
    float t = 1e30f;
    if (fabsf(dx) > 1e-6f) t = fminf(t, hx / fabsf(dx));
    if (fabsf(dy) > 1e-6f) t = fminf(t, hy / fabsf(dy));
    return V2{cx + dx * t, cy + dy * t};
}

}  // namespace hud
