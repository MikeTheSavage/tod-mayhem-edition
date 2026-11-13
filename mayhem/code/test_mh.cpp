// test_mh.cpp - unit tests of the [mayhem] props logic (mayhem/code/mh_logic.cpp): config clamps, chain delays, the
// detonation schedule (dedupe, depth, queue, rate, per-frame cap, clock jumps), the fired log, name patterns, debris
// generation and the debris pool. Run by dll/build.bat.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "mh_logic.h"

static int g_ok = 0, g_bad = 0;
#define CHECK(c) do { if (c) ++g_ok; else { ++g_bad; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool near(float a, float b, float e = 1e-4f) { return fabsf(a - b) <= e; }

static mh::Pending pend(uint32_t node, float due, int depth) { mh::Pending p; p.node = node; p.due = due; p.depth = depth; p.kind = mh::K_DESTROYABLE; return p; }

int main() {
    // ---- config
    {
        mh::Cfg c; c.chain_radius = 0; c.delay_min = 3; c.delay_max = 1; c.max_per_second = 0; c.max_per_frame = 0; c.max_depth = 0;
        c.cover_health_scale = 0; c.debris_max = -5; c.debris_pieces = 99; c.debris_life = 1e9f; c.blast_debris = 99;
        c.clamp();
        CHECK(c.chain_radius == 1.0f); CHECK(c.delay_min == 3.0f && c.delay_max == 3.0f); CHECK(c.max_per_second == 0.5f);
        CHECK(c.max_per_frame == 1); CHECK(c.max_depth == 1); CHECK(c.cover_health_scale == 0.02f); CHECK(c.debris_max == 0);
        CHECK(c.debris_pieces == 24); CHECK(c.debris_life == 120.0f); CHECK(c.blast_debris == 24);
        mh::Cfg d; float nanv = NAN; d.chain_radius = nanv; d.clamp(); CHECK(d.chain_radius == 1.0f);
        mh::Cfg e; e.clamp();   // defaults survive the clamp unchanged
        CHECK(e.chain_radius == 9.0f && e.delay_min == 0.15f && e.delay_max == 0.6f && e.max_per_second == 6.0f && e.cover_health_scale == 0.35f);
    }
    // ---- delays: within [min, max], nearer = sooner for the same random part
    {
        mh::Cfg c; c.clamp();
        for (int i = 0; i <= 10; ++i) {
            float d = c.chain_radius * i / 10.0f;
            for (int k = 0; k <= 4; ++k) { float t = mh::chain_delay(c, d, k / 4.0f); CHECK(t >= c.delay_min - 1e-6f && t <= c.delay_max + 1e-6f); }
        }
        CHECK(mh::chain_delay(c, 1.0f, 0.5f) < mh::chain_delay(c, 8.0f, 0.5f));
        CHECK(near(mh::chain_delay(c, 0.0f, 0.0f), c.delay_min));
        CHECK(near(mh::chain_delay(c, 100.0f, 1.0f), c.delay_max));
        CHECK(near(mh::chain_delay(c, -5.0f, -1.0f), c.delay_min));
    }
    // ---- rng
    {
        mh::Rng r; r.s = 1234; float lo = 1, hi = 0;
        for (int i = 0; i < 10000; ++i) { float f = r.f01(); CHECK(f >= 0.0f && f < 1.0f); if (f < lo) lo = f; if (f > hi) hi = f; }
        CHECK(lo < 0.01f && hi > 0.99f);
        mh::Rng a, b; a.s = b.s = 77; for (int i = 0; i < 10; ++i) CHECK(a.next() == b.next());
        mh::Rng z; z.s = 0; CHECK(z.next() != 0);
    }
    // ---- schedule: dedupe, depth, queue size
    {
        mh::Cfg c; c.max_pending = 3; c.max_depth = 4; c.clamp();
        mh::Sched s;
        CHECK(s.add(c, pend(10, 1.0f, 1))); CHECK(!s.add(c, pend(10, 0.5f, 1))); CHECK(s.dropped_dup == 1);
        CHECK(!s.add(c, pend(11, 1.0f, 5))); CHECK(s.dropped_depth == 1);
        CHECK(s.add(c, pend(11, 1.0f, 4))); CHECK(s.add(c, pend(12, 1.0f, 2)));
        CHECK(!s.add(c, pend(13, 1.0f, 2))); CHECK(s.dropped_full == 1);
        CHECK(!s.add(c, pend(0, 1.0f, 1)));
        CHECK(s.pending(11) && !s.pending(13));
    }
    // ---- schedule: due order, per-frame cap, rate limit, refill with game time
    {
        mh::Cfg c; c.max_per_second = 2.0f; c.max_per_frame = 2; c.max_pending = 32; c.clamp();
        mh::Sched s;
        for (uint32_t i = 1; i <= 8; ++i) s.add(c, pend(i, 0.1f * (9 - i), 1));   // node 8 is due first (0.1)
        mh::Pending out[16];
        int n = s.pop_due(c, 0.05f, out, 16); CHECK(n == 0);               // nothing due yet (tokens filled to 2)
        n = s.pop_due(c, 0.35f, out, 16);                                  // due: 8 (0.1), 7 (0.2), 6 (0.3)
        CHECK(n == 2 && out[0].node == 8 && out[1].node == 7);             // per-frame cap 2, earliest first
        n = s.pop_due(c, 0.36f, out, 16); CHECK(n == 0);                   // tokens used up
        n = s.pop_due(c, 0.86f, out, 16); CHECK(n == 1 && out[0].node == 6);   // 0.5 s -> 1 token
        n = s.pop_due(c, 5.0f, out, 16); CHECK(n == 2);                    // burst cap = 2 even after a long wait
        CHECK(s.q.size() == 3);
        n = s.pop_due(c, 1.0f, out, 16); CHECK(n == 2);                    // the clock went back: fresh bucket
        int total = 0; for (int k = 0; k < 20; ++k) total += s.pop_due(c, 10.0f + k, out, 16);
        CHECK(total == 1 && s.q.empty());
        s.clear(); CHECK(s.q.empty() && s.last_t < 0.0f);
    }
    // ---- a burst of 30 neighbours at once: never more than max_per_frame per call, max_per_second over time
    {
        mh::Cfg c; c.max_per_second = 6.0f; c.max_per_frame = 2; c.max_pending = 64; c.clamp();
        mh::Sched s; for (uint32_t i = 1; i <= 30; ++i) s.add(c, pend(i, 0.0f, 1));
        mh::Pending out[16]; int fired = 0, worst = 0;
        for (int f = 0; f <= 60; ++f) { int n = s.pop_due(c, f / 60.0f, out, 16); fired += n; if (n > worst) worst = n; }   // one second at 60 fps
        CHECK(worst <= 2); CHECK(fired >= 6 && fired <= 13);   // burst 6 + 6 per second
    }
    // ---- fired log
    {
        mh::FiredLog L; L.add(5, 2, 1.0f, 99);
        int d = 0; uint32_t o = 0;
        CHECK(L.find(5, 2.0f, &d, &o) && d == 2 && o == 99);
        CHECK(!L.find(6, 2.0f, &d, &o));
        CHECK(!L.find(5, 20.0f, &d, &o));   // older than 10 s
        L.add(5, 3, 3.0f, 98); CHECK(L.v.size() == 1 && L.find(5, 3.5f, &d, &o) && d == 3);
        L.expire(30.0f); CHECK(L.v.empty());
        for (uint32_t i = 0; i < 300; ++i) L.add(i + 1, 1, 0.0f, 0);
        CHECK(L.v.size() == 128);
    }
    // ---- names
    {
        CHECK(mh::name_match("data/Models/Addons/Crates/CrateSmall", "table,crate"));
        CHECK(mh::name_match("FRUITSTALL_02", " stall , cart"));
        CHECK(!mh::name_match("RedBarrel", "crate,table"));
        CHECK(!mh::name_match("", "crate")); CHECK(!mh::name_match("crate", "")); CHECK(!mh::name_match(nullptr, "x"));
        CHECK(!mh::name_match("TelePole", ",,,"));
    }
    // ---- debris pieces: inside the prop's box (roughly), moving outward and up, deterministic
    {
        mh::Box b = {{0.0f, 0.5f, 0.0f}, {0.6f, 0.5f, 0.4f}};
        float dir[3] = {1, 0, 0};
        mh::Piece p[24], q[24];
        int n = mh::make_debris(b, 8, 4.0f, dir, 2.0f, 42, p, 24);
        int m = mh::make_debris(b, 8, 4.0f, dir, 2.0f, 42, q, 24);
        CHECK(n == 8 && m == 8); CHECK(!memcmp(p, q, sizeof(mh::Piece) * 8));
        float sumx = 0;
        for (int i = 0; i < n; ++i) {
            for (int a = 0; a < 3; ++a) {
                CHECK(p[i].box.h[a] > 0.0f && p[i].box.h[a] <= b.h[a] + 1e-4f);
                CHECK(fabsf(p[i].box.c[a] - b.c[a]) <= b.h[a] + 1e-4f);
            }
            CHECK(p[i].vel[1] > 0.0f);   // every piece goes up
            sumx += p[i].vel[0];
        }
        CHECK(sumx > 0.0f);   // the push along +x shows in the sum
        CHECK(mh::make_debris(b, 30, 1, dir, 0, 1, p, 24) == 24);
        CHECK(mh::make_debris(b, 0, 1, dir, 0, 1, p, 24) == 0);
        CHECK(mh::make_debris(b, 3, 0.0f, nullptr, 0, 1, p, 24) == 3);
        mh::Box thin = {{0, 0, 0}, {0.0f, 0.0f, 2.0f}};
        n = mh::make_debris(thin, 5, 1, dir, 0, 3, p, 24);
        for (int i = 0; i < n; ++i) for (int a = 0; a < 3; ++a) CHECK(p[i].box.h[a] >= 0.02f);
    }
    // ---- debris style 1: small chunks inside the prop, sized min..max, never longer than the prop, deterministic
    {
        mh::Box b = {{0.0f, 0.4f, 0.0f}, {0.45f, 0.4f, 0.45f}};    // a crate
        float dir[3] = {0, 0, 1};
        mh::Piece p[24], q[24];
        for (int shape = 0; shape < 3; ++shape) {
            int n = mh::make_chunks(b, 8, 0.06f, 0.24f, shape, 3.5f, dir, 3.0f, 9, p, 24);
            int m = mh::make_chunks(b, 8, 0.06f, 0.24f, shape, 3.5f, dir, 3.0f, 9, q, 24);
            CHECK(n == 8 && m == 8 && !memcmp(p, q, sizeof(mh::Piece) * 8));
            float sumz = 0;
            for (int i = 0; i < n; ++i) {
                float L = 2.0f * fmaxf(p[i].box.h[0], fmaxf(p[i].box.h[1], p[i].box.h[2]));
                CHECK(L >= 0.06f - 1e-4f && L <= 0.24f + 1e-4f);                       // small: never the big slats
                for (int a = 0; a < 3; ++a) CHECK(fabsf(p[i].box.c[a] - b.c[a]) <= b.h[a] + 1e-4f && p[i].box.h[a] > 0.0f);
                CHECK(p[i].vel[1] > 0.0f);
                sumz += p[i].vel[2];
            }
            CHECK(sumz > 0.0f);
        }
        int n = mh::make_chunks(b, 6, 0.06f, 0.24f, mh::SH_SPLINTER, 1, dir, 0, 3, p, 24);   // splinters: long and thin
        for (int i = 0; i < n; ++i) {
            float e[3] = {p[i].box.h[0], p[i].box.h[1], p[i].box.h[2]};
            float mx = fmaxf(e[0], fmaxf(e[1], e[2])), mn = fminf(e[0], fminf(e[1], e[2]));
            CHECK(mn <= mx * 0.25f);
        }
        mh::Box tiny = {{0, 0.05f, 0}, {0.05f, 0.05f, 0.05f}};       // a bottle: pieces never longer than the prop
        n = mh::make_chunks(tiny, 5, 0.06f, 0.24f, mh::SH_LUMP, 1, nullptr, 0, 4, p, 24);
        for (int i = 0; i < n; ++i) for (int a = 0; a < 3; ++a) CHECK(p[i].box.h[a] <= 0.05f + 1e-4f);
        CHECK(mh::make_chunks(b, 30, 0.03f, 0.1f, 2, 1, dir, 0, 1, p, 24) == 24);
        CHECK(mh::make_chunks(b, 0, 0.03f, 0.1f, 2, 1, dir, 0, 1, p, 24) == 0);
    }
    // ---- grit: falls, bounces on its ground, settles; dust grows and fades; texture patches stay inside
    {
        mh::Bit g; memset(&g, 0, sizeof g);
        g.p[1] = 1.5f; g.v[0] = 2.0f; g.v[1] = 3.0f; g.half = 0.02f; g.ground = 0.25f; g.spin = 10.0f;
        float maxy = 0; int steps = 0;
        while (!g.rest && steps < 2000) { mh::bit_step(g, 1.0f / 60.0f, 9.81f); if (g.p[1] > maxy) maxy = g.p[1]; ++steps; CHECK(g.p[1] >= g.ground + g.half - 1e-5f); }
        CHECK(g.rest == 1 && steps < 600); CHECK(maxy > 1.8f); CHECK(near(g.p[1], 0.27f)); CHECK(g.p[0] > 0.5f);
        float x = g.p[0]; mh::bit_step(g, 0.1f, 9.81f); CHECK(g.p[0] == x);           // settled = still
        mh::Puff f; memset(&f, 0, sizeof f); f.r0 = 0.2f; f.r1 = 1.2f; f.life = 1.6f; f.alpha = 0.5f; f.v[0] = 2.0f;
        float r, a, lr = 0, la = 1;
        CHECK(mh::puff_at(f, &r, &a) && near(r, 0.2f) && a == 0.0f);
        int k = 0;
        while (mh::puff_at(f, &r, &a)) {
            CHECK(r >= lr - 1e-6f && r <= 1.2f + 1e-5f && a <= 0.5f + 1e-6f);
            if (f.age > 0.1f) { CHECK(a <= la + 1e-6f); }
            lr = r; la = a; mh::puff_step(f, 1.0f / 60.0f); ++k;
        }
        CHECK(k >= 95 && k <= 97); CHECK(f.p[0] > 0.3f && f.p[0] < 2.0f);            // the burst slows down
        float u0, v0;
        mh::uv_patch(0.5f, 0.5f, 0.2f, &u0, &v0); CHECK(near(u0, 0.4f) && near(v0, 0.4f));
        mh::uv_patch(0.98f, 0.01f, 0.2f, &u0, &v0); CHECK(near(u0, 0.8f) && near(v0, 0.0f));
        mh::uv_patch(0.5f, 0.5f, 3.0f, &u0, &v0); CHECK(near(u0, 0.0f) && near(v0, 0.0f));
    }
    // ---- debris pool: capped, oldest evicted, expiry frees slots
    {
        mh::DebrisPool P; int ev = 0;
        int a = P.take(3, 0.0f, &ev); P.s[a].id = 100; CHECK(ev == -1);
        int b = P.take(3, 1.0f, &ev); P.s[b].id = 101;
        int c = P.take(3, 2.0f, &ev); P.s[c].id = 102;
        int d = P.take(3, 3.0f, &ev); CHECK(ev == 100 && d == a); P.s[d].id = 103;
        CHECK(P.take(0, 3.0f, &ev) == -1);
        int ids[8]; int n = P.expired(12.5f, 10.0f, ids, 8);   // born 1 and 2 are older than 10 s at 12.5
        CHECK(n == 2 && ((ids[0] == 101 && ids[1] == 102) || (ids[0] == 102 && ids[1] == 101)));
        int e = P.take(3, 13.0f, &ev); CHECK(ev == -1 && (e == b || e == c));
    }
    // ---- cover damage scale
    {
        CHECK(near(mh::cover_extra_damage(10.0f, 0.5f), 10.0f));
        CHECK(near(mh::cover_extra_damage(10.0f, 0.25f), 30.0f));
        CHECK(mh::cover_extra_damage(10.0f, 1.0f) == 0.0f && mh::cover_extra_damage(-1.0f, 0.5f) == 0.0f && mh::cover_extra_damage(5.0f, 0.0f) == 0.0f);
    }
    printf("mayhem props logic: %d checks passed, %d failed\n", g_ok, g_bad);
    return g_bad ? 1 : 0;
}
