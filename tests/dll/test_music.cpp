// test_music.cpp - unit tests of dll/src/gp_music_logic.cpp ([fixes] stream_limits): the engine's quota rules as the
// constructor 0x445770 applies them, the active-stream rule, the caps and the trace text. Exit code 0 = all passed.
#include "../../dll/src/gp_music_logic.h"
#include <stdio.h>
#include <string.h>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
using namespace music;

static Counts C(int total, int st, int mono, int aux) { Counts c; c.total = total; c.stereo = st; c.mono = mono; c.aux = aux; return c; }
static Request R(int ch, bool aux = false, bool movie = false) { Request r; r.channels = ch; r.aux = aux; r.movie = movie; return r; }

static void test_vanilla() {
    // nothing alive: everything is allowed
    CHECK(vanilla_refusal(C(0, 0, 0, 0), R(2)) == OK);
    CHECK(vanilla_refusal(C(0, 0, 0, 0), R(1)) == OK);
    CHECK(vanilla_refusal(C(0, 0, 0, 0), R(1, true)) == OK);
    // a second stereo stream (the missing music: a stopped stereo stream keeps its quota)
    CHECK(vanilla_refusal(C(1, 1, 0, 0), R(2)) == STEREO);
    // stereo while an aux mono exists
    CHECK(vanilla_refusal(C(1, 0, 0, 1), R(2)) == STEREO);
    // stereo next to normal monos is fine
    CHECK(vanilla_refusal(C(5, 0, 5, 0), R(2)) == OK);
    // the 6th normal mono; the 5th is fine
    CHECK(vanilla_refusal(C(4, 0, 4, 0), R(1)) == OK);
    CHECK(vanilla_refusal(C(5, 0, 5, 0), R(1)) == MONO);
    // aux mono: 2 aux or any stereo refuses, one aux does not
    CHECK(vanilla_refusal(C(1, 0, 0, 1), R(1, true)) == OK);
    CHECK(vanilla_refusal(C(2, 0, 0, 2), R(1, true)) == AUX);
    CHECK(vanilla_refusal(C(1, 1, 0, 0), R(1, true)) == AUX);
    // a normal mono ignores stereo / aux counts
    CHECK(vanilla_refusal(C(3, 1, 1, 1), R(1)) == OK);
    // the movie rule needs another stream
    CHECK(vanilla_refusal(C(0, 0, 0, 0), R(1, false, true)) == OK);
    CHECK(vanilla_refusal(C(1, 0, 1, 0), R(1, false, true)) == MOVIE);
}

static Live L(int ch, bool aux, unsigned flags) { Live l; l.channels = ch; l.aux = aux; l.flags = flags; return l; }
static const unsigned PLAYING = 0x80 | 0x20, REQ = 0x4, STOPPED = 0x10 | 0x20, FRESH = 0x20;

static void test_active() {
    // what holds a place
    CHECK(stream_active(PLAYING) && stream_active(REQ) && stream_active(FRESH));
    CHECK(!stream_active(STOPPED));
    CHECK(stream_active(STOPPED | 0x80));          // stopped flag still set but playing again: counts
    CHECK(stream_active(STOPPED | 0x4));           // a new play request on a stopped stream: counts
    // counts of the really present streams
    Live a[] = {L(2, true, STOPPED), L(1, false, PLAYING), L(1, true, STOPPED), L(1, false, STOPPED)};
    Counts c = count_active(a, 4, 4);
    CHECK(c.total == 4 && c.stereo == 0 && c.mono == 1 && c.aux == 0);
    Live b[] = {L(2, true, PLAYING), L(1, true, REQ), L(1, false, PLAYING), L(1, false, FRESH)};
    c = count_active(b, 4, 4);
    CHECK(c.stereo == 1 && c.aux == 1 && c.mono == 2);
    // THE BUG: the previous cue was stopped but still allocated: vanilla refuses the next stereo cue, the active rule lets it in
    Live zombie[] = {L(2, true, STOPPED)};
    Counts engine = count_active(zombie, 1, 1); engine.stereo = 1;      // the engine's own counter still says 1
    CHECK(vanilla_refusal(engine, R(2)) == STEREO);
    CHECK(active_refusal(count_active(zombie, 1, 1), R(2)) == OK);
    // NO OVERLAP: while a stereo stream really plays (or is about to), the second cue is refused exactly as in vanilla
    Live playing[] = {L(2, true, PLAYING)};
    CHECK(active_refusal(count_active(playing, 1, 1), R(2)) == STEREO);
    Live pending[] = {L(2, true, REQ)};
    CHECK(active_refusal(count_active(pending, 1, 1), R(2)) == STEREO);
    // a playing aux mono also keeps a stereo cue out; a stopped one does not
    Live auxp[] = {L(1, true, PLAYING)}, auxs[] = {L(1, true, STOPPED)};
    CHECK(active_refusal(count_active(auxp, 1, 1), R(2)) == STEREO);
    CHECK(active_refusal(count_active(auxs, 1, 1), R(2)) == OK);
    // mono: 5 playing normal monos refuse the 6th (vanilla), 5 stopped ones do not
    Live m5[5], m5s[5];
    for (int i = 0; i < 5; ++i) { m5[i] = L(1, false, PLAYING); m5s[i] = L(1, false, STOPPED); }
    CHECK(active_refusal(count_active(m5, 5, 5), R(1)) == MONO);
    CHECK(active_refusal(count_active(m5s, 5, 5), R(1)) == OK);
    Live m4[4]; for (int i = 0; i < 4; ++i) m4[i] = L(1, false, PLAYING);
    CHECK(active_refusal(count_active(m4, 4, 4), R(1)) == OK);
    // aux: 2 playing aux refuse the 3rd; a playing stereo refuses an aux mono
    Live x2[] = {L(1, true, PLAYING), L(1, true, PLAYING)};
    CHECK(active_refusal(count_active(x2, 2, 2), R(1, true)) == AUX);
    CHECK(active_refusal(count_active(playing, 1, 1), R(1, true)) == AUX);
    // the movie rule is on the manager's own total, stopped streams included
    CHECK(active_refusal(count_active(zombie, 1, 1), R(1, false, true)) == MOVIE);
    // with the same present streams the active rule is never stricter than vanilla on the engine's counts, and equals vanilla
    // when every stream plays
    int stricter = 0, diff = 0;
    for (int st = 0; st <= 2; ++st) for (int mono = 0; mono <= 6; ++mono) for (int ax = 0; ax <= 3; ++ax)
        for (int ch = 1; ch <= 2; ++ch) for (int a1 = 0; a1 <= 1; ++a1) for (int mv = 0; mv <= 1; ++mv) {
            std::vector<Live> v;
            for (int i = 0; i < st; ++i) v.push_back(L(2, true, PLAYING));
            for (int i = 0; i < mono; ++i) v.push_back(L(1, false, PLAYING));
            for (int i = 0; i < ax; ++i) v.push_back(L(1, true, PLAYING));
            Counts act = count_active(v.data(), (int)v.size(), (int)v.size());
            Request r = R(ch, a1 != 0, mv != 0);
            if (vanilla_refusal(act, r) != active_refusal(act, r)) ++diff;
            Counts allc = act;                                   // adding stopped streams to the engine's count only adds refusals
            allc.stereo += 1; allc.mono += 1; allc.aux += 1;
            if (vanilla_refusal(allc, r) == OK && active_refusal(act, r) != OK) ++stricter;
        }
    CHECK(diff == 0 && stricter == 0);
}

static void test_caps_and_pool() {
    Caps c; c.pool = 5;
    CHECK(clamp_caps(c).pool == 7);
    c.pool = 5000; CHECK(clamp_caps(c).pool == 100);
    c.pool = 16; CHECK(clamp_caps(c).pool == 16);
    CHECK(!pool_may_grow(7, 7));
    CHECK(pool_may_grow(7, 16));
    CHECK(pool_may_grow(15, 16));
    CHECK(!pool_may_grow(16, 16));
}

static void test_snapshot() {
    Snapshot a, b;
    a.renderer = b.renderer = 3;
    CHECK(!snapshot_changed(a, b));
    a.n = b.n = 1; a.row[0].ptr = b.row[0].ptr = 0x1000; a.row[0].channels = b.row[0].channels = 2; a.row[0].ready = b.row[0].ready = 1;
    strcpy_s(a.row[0].name, "music.ogg"); strcpy_s(b.row[0].name, "music.ogg");
    a.counts = C(1, 1, 0, 0); b.counts = a.counts;
    CHECK(!snapshot_changed(a, b));
    b.row[0].flags = 0x4;                      // play requested
    CHECK(snapshot_changed(a, b));
    a.row[0].flags = 0x4; a.row[0].flags |= 0x4000;   // a thread-internal bit alone is not news
    CHECK(!snapshot_changed(a, b));
    b.row[0].flags = 0x80;
    CHECK(snapshot_changed(a, b));
    b = a; b.pool_entries = 8;
    CHECK(snapshot_changed(a, b));
    b = a; b.n = 0;
    CHECK(snapshot_changed(a, b));
    char line[512];
    a.row[0].flags = 0x80 | 0x20; a.pool_entries = 7; a.pool_in_use = 1;
    int n = snapshot_text(a, line, sizeof line);
    CHECK(n > 0 && strstr(line, "streams 1 (stereo 1, mono 0, aux 0) pool 1/7") && strstr(line, "PLAYING") && strstr(line, "\"music.ogg\""));
    char tiny[40]; n = snapshot_text(a, tiny, sizeof tiny);
    CHECK(n > 0 && n < (int)sizeof tiny && tiny[sizeof tiny - 1] == 0);
}

int main() {
    test_vanilla(); test_active(); test_caps_and_pool(); test_snapshot();
    printf("test_music: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
