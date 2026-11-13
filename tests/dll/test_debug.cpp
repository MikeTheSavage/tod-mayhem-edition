// test_debug.cpp - unit tests of dll/src/gp_debug_logic.cpp: key script, heap tracker tables, stress
// schedule. Built and run by build.bat. Exit code 0 = all passed.
#include "../../dll/src/gp_debug_logic.h"
#include "../../dll/src/gp_logic.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
using namespace dbg;

static void test_keys() {
    KeyScript s;
    CHECK(s.parse("# load the first save\nwait 1000\ntap ESC\r\nhold W 500\nmark title screen\nbogus 3\ntap NOPE\nquit\n"));
    CHECK(s.n == 5 && s.errors == 2);
    CHECK(s.c[0].op == KeyCmd::WAIT && s.c[0].ms == 1000);
    CHECK(s.c[1].op == KeyCmd::TAP && s.c[1].key == 0x01);
    CHECK(s.c[2].op == KeyCmd::HOLD && s.c[2].key == 0x11 && s.c[2].ms == 500);
    CHECK(s.c[3].op == KeyCmd::MARK && !strcmp(s.c[3].text, "title screen"));
    CHECK(s.c[4].op == KeyCmd::QUIT);
    KeyScript t; CHECK(t.parse("wait 100; tap ENTER; wait 50; tap DOWN"));
    CHECK(t.n == 4);
    // play at 60 fps: ENTER goes down once at >= 0.1 s, up 80 ms later, DOWN after a further 50 ms wait
    KeyPlayer p; int downs = 0, ups = 0; double t_enter = -1, t_enter_up = -1, t_down = -1;
    for (int f = 0; f < 60; ++f) {
        double now = f / 60.0;
        KeyOut o = p.step(t, now);
        if (o.press == 0x1C) { ++downs; t_enter = now; }
        if (o.release == 0x1C) { ++ups; t_enter_up = now; }
        if (o.press == 0xD0) t_down = now;
        CHECK(!(o.press >= 0 && o.release >= 0));
    }
    CHECK(downs == 1 && ups == 1);
    CHECK(t_enter >= 0.1 && t_enter < 0.14);
    CHECK(t_enter_up - t_enter >= 0.08 && t_enter_up - t_enter < 0.12);
    CHECK(t_down - t_enter_up >= 0.05 && t_down - t_enter_up < 0.09);
    // waitlog: holds the script until the text is there (or the timeout), then goes on
    KeyScript w; CHECK(w.parse("waitlog 5000 mainmenu_music_loop\ntap ENTER\nwaitlog 0 x\nwaitlog 100\n"));
    CHECK(w.n == 2 && w.errors == 2 && w.c[0].op == KeyCmd::WAITLOG && w.c[0].ms == 5000 && !strcmp(w.c[0].text, "mainmenu_music_loop"));
    struct Ctx { bool have; static bool has(const char* t, void* c) { return ((Ctx*)c)->have && !strcmp(t, "mainmenu_music_loop"); } } cx = {false};
    KeyPlayer pw; int enter_at = -1; const char* m = nullptr;
    for (int f = 0; f < 120; ++f) {
        if (f == 60) cx.have = true;
        KeyOut o = pw.step(w, f / 60.0, &Ctx::has, &cx);
        if (o.mark && !m) m = o.mark;
        if (o.press == 0x1C && enter_at < 0) enter_at = f;
    }
    CHECK(enter_at >= 60 && enter_at <= 62 && m && !strcmp(m, "waitlog: found"));
    KeyScript wt; wt.parse("waitlog 500 never\ntap ENTER"); KeyPlayer pt; const char* mt = nullptr; int et = -1;
    for (int f = 0; f < 60; ++f) { KeyOut o = pt.step(wt, f / 60.0, &Ctx::has, &cx); if (o.mark && !mt) mt = o.mark; if (o.press == 0x1C && et < 0) et = f; }
    CHECK(mt && !strcmp(mt, "waitlog: TIMEOUT") && et >= 30 && et <= 32);
    // quit
    KeyScript q; q.parse("quit"); KeyPlayer pq; KeyOut oq = pq.step(q, 0.0); CHECK(oq.quit); CHECK(pq.done);
}

static void test_live() {
    const uint32_t CAP = 1u << 12;
    std::vector<uint32_t> k(CAP), v(CAP), k2(CAP), v2(CAP);
    LiveSet s; s.init(k.data(), v.data(), CAP);
    LiveSet s2; s2.key = k2.data(); s2.val = v2.data(); s2.cap = CAP;
    for (uint32_t i = 0; i < 2000; ++i) CHECK(s.insert(0x10000000 + i * 16, i));
    CHECK(s.used == 2000);
    uint32_t sz = 0;
    CHECK(s.find(0x10000000 + 5 * 16, &sz) && sz == 5);
    CHECK(!s.find(0x10000000 + 5 * 16 + 4, &sz));
    for (uint32_t i = 0; i < 2000; i += 2) CHECK(s.erase(0x10000000 + i * 16, &sz) && sz == i);
    CHECK(!s.erase(0x10000000, &sz));                       // double free detected
    CHECK(s.used == 1000 && s.tombs == 1000);
    for (uint32_t i = 1; i < 2000; i += 2) CHECK(s.find(0x10000000 + i * 16, &sz) && sz == i);
    CHECK(s.needs_rebuild() == ((s.used + s.tombs) * 10 > CAP * 7));
    s.rebuild_into(s2);
    CHECK(s2.used == 1000 && s2.tombs == 0);
    for (uint32_t i = 1; i < 2000; i += 2) CHECK(s2.find(0x10000000 + i * 16, &sz) && sz == i);
    CHECK(!s2.find(0x10000000, &sz));
    // churn with tombstones reused
    LiveSet c; std::vector<uint32_t> kc(256), vc(256); c.init(kc.data(), vc.data(), 256);
    for (int round = 0; round < 50; ++round) {
        for (uint32_t i = 0; i < 100; ++i) CHECK(c.insert(0x2000 + (round * 100 + i) * 8, i));
        for (uint32_t i = 0; i < 100; ++i) CHECK(c.erase(0x2000 + (round * 100 + i) * 8, nullptr));
    }
    CHECK(c.used == 0);
    CHECK(!c.insert(0, 1) && !c.insert(1, 1));
}

static void test_ring() {
    std::vector<FreedRec> m(8);
    FreedRing r; r.init(m.data(), 8);
    for (uint32_t i = 0; i < 20; ++i) { FreedRec& x = r.push(); x.p = 0x100 + i; x.swap = i; }
    CHECK(r.count == 8);
    CHECK(r.find(0x100 + 19) && r.find(0x100 + 19)->swap == 19);
    CHECK(r.find(0x100 + 12) && !r.find(0x100 + 11));
    FreedRec& y = r.push(); y.p = 0x100 + 15; y.swap = 99;
    CHECK(r.find(0x100 + 15)->swap == 99);                  // newest first
}

static void test_call() {
    uint8_t e8[6] = {0x90, 0xE8, 1, 2, 3, 4};
    uint8_t ff15[6] = {0xFF, 0x15, 0xD0, 0x31, 0x9B, 0x00};
    uint8_t ff50[6] = {0x00, 0x00, 0x00, 0xFF, 0x50, 0x10};
    uint8_t ffd0[6] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xD7};
    uint8_t none[6] = {0x00, 0x8B, 0x45, 0x08, 0x89, 0x01};
    CHECK(looks_like_call(e8) && looks_like_call(ff15) && looks_like_call(ff50) && looks_like_call(ffd0));
    CHECK(!looks_like_call(none));
}

static void test_stress() {
    Stress s; s.imin = 1.2f; s.imax = 1.2f;
    CHECK(s.interval(0) == 1.2f && s.interval(7) == 1.2f);
    s.imin = 0.3f; s.imax = 2.5f;
    float lo = 9, hi = 0; bool varies = false;
    for (int k = 0; k < 200; ++k) { float x = s.interval(k); if (x < lo) lo = x; if (x > hi) hi = x; if (k && x != s.interval(k - 1)) varies = true; }
    CHECK(lo >= 0.3f && hi <= 2.5f && varies && hi - lo > 1.5f);
    CHECK(s.interval(5) == s.interval(5));
}

int main() {
    test_keys(); test_live(); test_ring(); test_call(); test_stress();
    printf("test_debug: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
