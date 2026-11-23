// hull_fuzz.cpp - offline hunt for point sets that make Box3D's hull builder (b3CreateHull) loop forever, and the proof
// that the patched builder (third_party/box3d/src/hull.c, "hull_hang") always returns. Built twice by hull_fuzz.bat:
//   hull_fuzz_orig.exe    the ORIGINAL hull.c (git show bc37735), may hang
//   hull_fuzz_patched.exe the vendored, patched hull.c
// Every case runs on a worker thread; if it makes no progress for the timeout it is reported as a HANG (its points are
// written to a .pts file, the thread is terminated and the run goes on). Results of every case (hull hash or NULL) go
// to a text file, so the two builds can be compared: on every input where the original returns, the patched builder
// must return the identical hull.
//
//   hull_fuzz <results.txt> <first_seed> <last_seed> <timeout_ms> [hangdir]
//   hull_fuzz <results.txt> --file <case.pts> <timeout_ms>        replay one saved input
//
// Inputs mimic what the game feeds the builder (phys_game.cpp add_static_points -> prepare_hull -> make_hull): slabs of
// near-planar level patches at world coordinates, boxes, duplicates, collinear and lattice sets, huge + tiny extents.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#define NOMINMAX
#include <windows.h>
#include "box3d/box3d.h"
#include "phys_logic.h"

using ph::V3;
static unsigned g_s;
static unsigned rnd() { g_s = g_s * 1664525u + 1013904223u; return g_s >> 8; }
static float rf() { return (float)rnd() / 16777216.0f; }                 // [0,1)
static float rs(float a) { return (rf() * 2.0f - 1.0f) * a; }              // [-a,a]

static const char* kind_name[] = {"cloud", "slab-patch", "slab-grid", "duplicates", "collinear", "huge-tiny", "boxes",
                                  "sphere", "lattice", "slab-noise", "twin-planes", "tri-strip"};
static const int KINDS = 12;

static void rot_basis(V3 n, V3* u, V3* v) {
    n = ph::norm(n);
    V3 a = fabsf(n.x) < 0.9f ? ph::v3(1, 0, 0) : ph::v3(0, 1, 0);
    *u = ph::norm(ph::cross(n, a)); *v = ph::cross(n, *u);
}

// returns the point set of case `seed`; kind in *kindOut
static std::vector<V3> make_case(unsigned seed, int* kindOut) {
    g_s = seed * 2654435761u + 12345u;
    for (int i = 0; i < 4; ++i) rnd();
    int kind = (int)(seed % KINDS);
    *kindOut = kind;
    // world position like the farm (Map06) or anywhere
    float ox, oy, oz;
    switch (rnd() % 4) {
        case 0: ox = 780 + rs(60); oy = -40 + rs(20); oz = -1750 + rs(60); break;
        case 1: ox = rs(2000); oy = rs(100); oz = rs(2000); break;
        case 2: ox = rs(3); oy = rs(3); oz = rs(3); break;
        default: ox = rs(20000); oy = rs(500); oz = rs(20000); break;
    }
    std::vector<V3> p;
    auto fin = [&](std::vector<V3>& q) { for (auto& a : q) { a.x = (float)(double)a.x; } };
    (void)fin;
    switch (kind) {
    case 0: {   // random cloud
        int n = 4 + rnd() % 60; if (seed & 32) n = 60 + rnd() % 500; float sc = powf(10.0f, rs(2.0f));
        for (int i = 0; i < n; ++i) p.push_back(ph::v3(ox + rs(sc), oy + rs(sc), oz + rs(sc)));
        break; }
    case 1: case 9: {   // a level patch: n near-planar points (any orientation, noise 0..5 mm) -> prepare_hull slab
        int n = 3 + rnd() % 22;
        V3 nrm = ph::norm(ph::v3(rs(1), (rnd() % 2) ? 1.0f : rs(1), rs(1)));
        if (ph::len(nrm) < 0.5f) nrm = ph::v3(0, 1, 0);
        static const float noises[] = {0.0f, 1e-5f, 1e-4f, 1e-3f, 3e-3f, 5e-3f, 0.02f};
        float noise = noises[rnd() % 7]; if (kind == 9) noise = 1e-3f + rf() * 4e-3f;
        V3 u, v; rot_basis(nrm, &u, &v);
        float sc = powf(10.0f, rs(1.2f) + 0.5f);
        std::vector<V3> patch;
        for (int i = 0; i < n; ++i) {
            float a = rs(sc), b = rs(sc), h = rs(noise);
            patch.push_back(ph::add(ph::v3(ox, oy, oz), ph::add(ph::add(ph::mul(u, a), ph::mul(v, b)), ph::mul(nrm, h))));
        }
        std::vector<V3> out(2 * patch.size() + 2);
        int k = ph::prepare_hull(patch.data(), (int)patch.size(), 0.02f, 0.25f, out.data());
        out.resize(k > 0 ? k : 0); p = out;
        break; }
    case 2: {   // a regular grid patch (colinear + coplanar everywhere), maybe with mm noise, as a slab
        int nx = 2 + rnd() % 5, nz = 2 + rnd() % 5; float cell = 0.25f + rf() * 4, noise = (rnd() % 2) ? 0.0f : rf() * 4e-3f;
        float tilt = (rnd() % 3) ? 0.0f : rs(0.5f);
        std::vector<V3> patch;
        for (int i = 0; i < nx; ++i) for (int j = 0; j < nz; ++j)
            patch.push_back(ph::v3(ox + i * cell, oy + tilt * i * cell + rs(noise), oz + j * cell));
        std::vector<V3> out(2 * patch.size() + 2);
        int k = ph::prepare_hull(patch.data(), (int)patch.size(), 0.02f, 0.25f, out.data());
        out.resize(k > 0 ? k : 0); p = out;
        break; }
    case 3: {   // few distinct points, repeated, +-1 ulp jitter
        int m = 4 + rnd() % 5, n = m + rnd() % 40;
        std::vector<V3> base;
        for (int i = 0; i < m; ++i) base.push_back(ph::v3(ox + rs(3), oy + rs(3), oz + rs(3)));
        for (int i = 0; i < n; ++i) {
            V3 a = base[rnd() % m];
            if (rnd() % 2) { a.x = nextafterf(a.x, 1e30f); }
            if (rnd() % 4 == 0) { a.y = nextafterf(a.y, -1e30f); }
            p.push_back(a);
        }
        break; }
    case 4: {   // collinear points plus (maybe) one slightly off the line
        int n = 4 + rnd() % 30; V3 d = ph::norm(ph::v3(rs(1), rs(1), rs(1)));
        if (ph::len(d) < 0.5f) d = ph::v3(1, 0, 0);
        for (int i = 0; i < n; ++i) p.push_back(ph::add(ph::v3(ox, oy, oz), ph::mul(d, rs(10))));
        if (rnd() % 2) { V3 u, v; rot_basis(d, &u, &v); p.push_back(ph::add(p[0], ph::mul(u, rf() * 1e-3f))); }
        if (rnd() % 2) { V3 u, v; rot_basis(d, &u, &v); p.push_back(ph::add(p[1], ph::mul(v, rf() * 1e-2f))); }
        break; }
    case 5: {   // huge offset + tiny extent, or tiny + huge together
        int n = 4 + rnd() % 30; float big = powf(10.0f, 3.0f + rf() * 3.0f), tiny = powf(10.0f, -4.0f + rf() * 3.0f);
        for (int i = 0; i < n; ++i) {
            float s = (rnd() % 5 == 0) ? big : tiny;
            p.push_back(ph::v3(big + rs(s), -big + rs(tiny), rs(s)));
        }
        break; }
    case 6: {   // boxes (collision boxes): rotated, thin, scaled, with extra duplicated corners
        float hx = powf(10.0f, rs(1.5f)), hy = (rnd() % 3) ? powf(10.0f, rs(1.5f)) : 1e-4f + rf() * 1e-3f, hz = powf(10.0f, rs(1.5f));
        ph::Q4 q = ph::qnorm({rs(1), rs(1), rs(1), rs(1) + 0.01f});
        for (int i = 0; i < 8; ++i) {
            V3 c = ph::v3((i & 1) ? hx : -hx, (i & 2) ? hy : -hy, (i & 4) ? hz : -hz);
            p.push_back(ph::add(ph::v3(ox, oy, oz), ph::qrot(q, c)));
        }
        int dup = rnd() % 8; for (int i = 0; i < dup; ++i) p.push_back(p[rnd() % 8]);
        break; }
    case 7: {   // points on a sphere / circle / ellipsoid (many cocircular, coplanar faces)
        int n = 8 + rnd() % 100; if (seed & 16) n = 100 + rnd() % 700; float r = 0.3f + rf() * 8; float sy = (rnd() % 3) ? 1.0f : 0.001f;
        for (int i = 0; i < n; ++i) {
            V3 d = ph::norm(ph::v3(rs(1), rs(1), rs(1))); if (ph::len(d) < 0.1f) d = ph::v3(1, 0, 0);
            if (rnd() % 4 == 0) d.y = 0;
            p.push_back(ph::v3(ox + d.x * r, oy + d.y * r * sy, oz + d.z * r));
        }
        break; }
    case 8: {   // subset of an integer lattice: lots of exactly coplanar / collinear / duplicate points
        int n = 4 + rnd() % 40, g = 1 + rnd() % 3; float cell = (rnd() % 2) ? 1.0f : 0.1f;
        for (int i = 0; i < n; ++i) p.push_back(ph::v3(ox + (float)(rnd() % (g + 1)) * cell, oy + (float)(rnd() % (g + 1)) * cell, oz + (float)(rnd() % (g + 1)) * cell));
        break; }
    case 10: {   // two parallel planes of the SAME polygon, thickness 0 .. 2 mm (a slab whose faces nearly coincide)
        int n = 3 + rnd() % 12; float th = (rnd() % 3) ? 0.0f : rf() * 2e-3f, sc = 0.5f + rf() * 20;
        V3 nrm = ph::norm(ph::v3(rs(0.3f), 1, rs(0.3f))); V3 u, v; rot_basis(nrm, &u, &v);
        std::vector<V3> patch;
        for (int i = 0; i < n; ++i) { float a = rs(sc), b = rs(sc); patch.push_back(ph::add(ph::v3(ox, oy, oz), ph::add(ph::mul(u, a), ph::mul(v, b)))); }
        for (int i = 0; i < n; ++i) p.push_back(patch[i]);
        for (int i = 0; i < n; ++i) p.push_back(ph::sub(patch[i], ph::mul(nrm, th)));
        break; }
    case 11: {   // a strip of terrain triangles (long, thin, nearly collinear vertices), slab
        int n = 3 + rnd() % 8; float len = 1 + rf() * 40, wid = 0.01f + rf() * 0.3f;
        std::vector<V3> patch;
        for (int i = 0; i < n; ++i) patch.push_back(ph::v3(ox + (float)i / n * len + rs(0.01f), oy + rs(1e-3f) + 0.01f * i, oz + ((i & 1) ? wid : 0.0f) + rs(1e-4f)));
        std::vector<V3> out(2 * patch.size() + 2);
        int k = ph::prepare_hull(patch.data(), (int)patch.size(), 0.02f, 0.25f, out.data());
        out.resize(k > 0 ? k : 0); p = out;
        break; }
    }
    // HULL_FUZZ_PREPARE=1: route every case through the game's prepare_hull first (what add_static_points does)
    static int prep = -1;
    if (prep < 0) { const char* e = getenv("HULL_FUZZ_PREPARE"); prep = (e && e[0] == '1') ? 1 : 0; }
    if (prep && kind != 1 && kind != 2 && kind != 9 && kind != 11 && p.size() >= 3) {
        std::vector<V3> out(2 * p.size() + 2);
        int k = ph::prepare_hull(p.data(), (int)p.size(), 0.02f, 0.25f, out.data());
        out.resize(k > 0 ? k : 0); p = out;
    }
    return p;
}

static bool load_pts(const char* path, std::vector<V3>* out) {
    FILE* f = fopen(path, "r"); if (!f) return false;
    float x, y, z; char line[256];
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%f %f %f", &x, &y, &z) == 3) out->push_back(ph::v3(x, y, z));
    fclose(f); return !out->empty();
}
static void save_pts(const char* path, const std::vector<V3>& p) {
    FILE* f = fopen(path, "w"); if (!f) return;
    for (auto& a : p) fprintf(f, "%.9g %.9g %.9g\n", a.x, a.y, a.z);
    fclose(f);
}

// ---- worker
static volatile LONG g_progress = 0;       // incremented when a case starts and when it ends
static volatile LONG g_cur = -1;           // index of the running case
static std::vector<std::vector<V3>> g_cases;
static std::vector<int> g_kinds;
static std::vector<unsigned> g_ids;
struct Res { unsigned long long hash; int vc, fc; int state; };   
static std::vector<Res> g_res;
static volatile LONG g_next = 0;

static int crash_filter(EXCEPTION_POINTERS* ep, Res* r) {
    HMODULE m = GetModuleHandleA(0);
    r->vc = (int)((DWORD_PTR)ep->ExceptionRecord->ExceptionAddress - (DWORD_PTR)m);   // image-relative address of the fault
    r->fc = (int)ep->ExceptionRecord->ExceptionCode;
    if (getenv("HULL_FUZZ_VERBOSE"))
        printf("  AV at image+0x%X %s of 0x%08lX  eax %08lX ecx %08lX edx %08lX esi %08lX edi %08lX\n", r->vc, ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
               (unsigned long)ep->ExceptionRecord->ExceptionInformation[1], ep->ContextRecord->Eax, ep->ContextRecord->Ecx, ep->ContextRecord->Edx, ep->ContextRecord->Esi, ep->ContextRecord->Edi);
    return EXCEPTION_EXECUTE_HANDLER;
}
static DWORD WINAPI worker(void*) {
    for (;;) {
        LONG i = g_next;
        if (i >= (LONG)g_cases.size()) return 0;
        g_next = i + 1;
        g_cur = i;
        InterlockedIncrement(&g_progress);
        const std::vector<V3>& p = g_cases[i];
        Res r = {0, 0, 0, 1};
        if (p.size() >= 4) {
            __try {
                b3HullData* h = b3CreateHull((const b3Vec3*)p.data(), (int)p.size(), (p.size() > 80) ? 128 : 32);
                if (h) { r.hash = h->hash; r.vc = h->vertexCount; r.fc = h->faceCount; r.state = 2; b3DestroyHull(h); }
            } __except (crash_filter(GetExceptionInformation(), &r)) { r.state = 4; }   // an access violation inside the builder
        }
        g_res[i] = r;
        InterlockedIncrement(&g_progress);
    }
}

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: hull_fuzz results.txt first last timeout_ms [hangdir] | results.txt --file x.pts timeout_ms\n"); return 2; }
    const char* resPath = argv[1];
    int timeout = 2000; const char* hangdir = ".";
    if (strcmp(argv[2], "--file") == 0) {
        std::vector<V3> p; if (!load_pts(argv[3], &p)) { printf("cannot read %s\n", argv[3]); return 2; }
        g_cases.push_back(p); g_kinds.push_back(0); g_ids.push_back(0); timeout = atoi(argv[4]);
    } else {
        unsigned a = (unsigned)strtoul(argv[2], 0, 10), b = (unsigned)strtoul(argv[3], 0, 10); timeout = atoi(argv[4]);
        if (argc > 5) hangdir = argv[5];
        for (unsigned s = a; s <= b; ++s) { int k; g_cases.push_back(make_case(s, &k)); g_kinds.push_back(k); g_ids.push_back(s); }
    }
    g_res.assign(g_cases.size(), Res{0, 0, 0, 0});
    int hangs = 0, kindHang[16] = {0};
    DWORD t0 = GetTickCount();
    HANDLE th = CreateThread(0, 0, worker, 0, 0, 0);
    LONG lastProg = -1; DWORD lastChange = GetTickCount();
    for (;;) {
        Sleep(5);
        if (WaitForSingleObject(th, 0) == WAIT_OBJECT_0) break;
        LONG pr = g_progress;
        if (pr != lastProg) { lastProg = pr; lastChange = GetTickCount(); continue; }
        if ((int)(GetTickCount() - lastChange) > timeout) {
            LONG i = g_cur;
            SuspendThread(th);
            CONTEXT cx; memset(&cx, 0, sizeof cx); cx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &cx)) {   // where it spins: EIP relative to the image (look it up in the /MAP file)
                HMODULE m = GetModuleHandleA(0);
                printf("  spinning at image+0x%lX\n", (unsigned long)((DWORD_PTR)cx.Eip - (DWORD_PTR)m));
            }
            TerminateThread(th, 1); CloseHandle(th);
            g_res[i].state = 3; ++hangs; kindHang[g_kinds[i] & 15]++;
            char path[512]; _snprintf(path, sizeof path, "%s\\hang_%u.pts", hangdir, g_ids[i]); path[sizeof path - 1] = 0;
            save_pts(path, g_cases[i]);
            printf("HANG seed %u kind %s n %d -> %s\n", g_ids[i], kind_name[g_kinds[i]], (int)g_cases[i].size(), path); fflush(stdout);
            th = CreateThread(0, 0, worker, 0, 0, 0);
            lastProg = -1; lastChange = GetTickCount();
        }
    }
    CloseHandle(th);
    int nul = 0, ok = 0, crashes = 0;
    FILE* f = fopen(resPath, "w");
    for (size_t i = 0; i < g_cases.size(); ++i) {
        const Res& r = g_res[i];
        if (r.state == 1) ++nul; else if (r.state == 2) ++ok; else if (r.state == 4) { ++crashes; char path[512]; _snprintf(path, sizeof path, "%s\\crash_%u.pts", hangdir, g_ids[i]); path[sizeof path - 1] = 0; save_pts(path, g_cases[i]); }
        if (f) fprintf(f, "%u %s %d %d %016llx\n", g_ids[i], r.state == 3 ? "HANG" : r.state == 4 ? "CRASH" : r.state == 1 ? "NULL" : "HULL", r.vc, r.fc, r.hash);
    }
    if (f) fclose(f);
    printf("cases %d: hull %d, NULL %d, CRASH %d, HANG %d in %.1f s\n", (int)g_cases.size(), ok, nul, crashes, hangs, (GetTickCount() - t0) / 1000.0);
    for (int k = 0; k < KINDS; ++k) if (kindHang[k]) printf("  hangs of kind %s: %d\n", kind_name[k], kindHang[k]);
    return (hangs || crashes) ? 1 : 0;
}
