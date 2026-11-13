// test_seam.cpp - unit tests of the [seamless] pure logic (seam_logic.cpp). Built and run by dll\build.bat.
#include <stdio.h>
#include <string.h>
#include "seam_logic.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static sl::Pending P(uint32_t e, uint32_t f, bool tp, int kind) { sl::Pending p; p.entry = e; p.folder = f; p.teleport = tp; p.kind = kind; return p; }

static void test_classify() {
    sl::Cfg c; c.enabled = true;
    sl::Tags t;
    double now = 100.0;
    // a map-border trigger: two actions (MAINMAP + SUBMAP), both entries appended under walk tags
    t.add_entry(0x1000, now - 0.02); t.add_entry(0x2000, now - 0.02);
    t.add_folder(0xA000, now - 0.02); t.add_folder(0xB000, now - 0.02);
    sl::Pending b[2] = {P(0x1000, 0xA000, false, 1), P(0x2000, 0xB000, false, 2)};
    sl::Decision d = sl::classify(c, b, 2, t, now);
    CHECK(d.v == sl::V_SEAMLESS && d.walk_by_entry == 2 && d.walk_by_folder == 0);
    // off
    sl::Cfg off; CHECK(sl::classify(off, b, 2, t, now).v == sl::V_OFF);
    // a teleport in the batch (corridor teleport in vanilla data, taxi)
    sl::Pending tp[2] = {P(0x1000, 0xA000, false, 1), P(0x2000, 0xB000, true, 2)};
    CHECK(sl::classify(c, tp, 2, t, now).v == sl::V_NORMAL);
    // a mission folder in the batch (mission start: map + submap + mission)
    t.add_entry(0x3000, now);
    sl::Pending mi[3] = {P(0x1000, 0xA000, false, 1), P(0x2000, 0xB000, false, 2), P(0x3000, 0xC000, false, 3)};
    CHECK(sl::classify(c, mi, 3, t, now).v == sl::V_NORMAL);
    // an entry nobody walk-tagged (a mission or script asked for the same submap)
    sl::Pending un[2] = {P(0x1000, 0xA000, false, 1), P(0x4000, 0xD000, false, 2)};
    CHECK(sl::classify(c, un, 2, t, now).v == sl::V_NORMAL);
    // tagged by folder only (the entry append was not seen) still counts
    sl::Pending fo[1] = {P(0x5000, 0xB000, false, 2)};
    d = sl::classify(c, fo, 1, t, now);
    CHECK(d.v == sl::V_SEAMLESS && d.walk_by_folder == 1);
    // stale tags (older than the window) do not count
    CHECK(sl::classify(c, b, 2, t, now + 10.0).v == sl::V_NORMAL);
    // tags from the future (clock confusion) do not count
    CHECK(sl::classify(c, b, 2, t, now - 1.0).v == sl::V_NORMAL);
    // empty / too many / unreadable / unknown kind
    CHECK(sl::classify(c, b, 0, t, now).v == sl::V_NORMAL);
    sl::Pending many[5] = {P(1, 1, false, 2), P(2, 2, false, 2), P(3, 3, false, 2), P(4, 4, false, 2), P(5, 5, false, 2)};
    sl::Tags t5; for (int i = 1; i <= 5; ++i) t5.add_entry(i, now);
    CHECK(sl::classify(c, many, 5, t5, now).v == sl::V_NORMAL);
    CHECK(sl::classify(c, many, 4, t5, now).v == sl::V_SEAMLESS);
    sl::Pending z[1] = {P(0, 0xA000, false, 1)};
    CHECK(sl::classify(c, z, 1, t, now).v == sl::V_NORMAL);
    sl::Pending k[1] = {P(0x1000, 0xA000, false, -1)};
    CHECK(sl::classify(c, k, 1, t, now).v == sl::V_NORMAL);
    // first_time and clear
    CHECK(t.first_time() == now - 0.02);
    t.clear(); CHECK(t.n_entry == 0 && t.n_folder == 0 && t.first_time() == 0.0);
    CHECK(sl::classify(c, b, 2, t, now).v == sl::V_NORMAL);
    // the tag ring keeps the newest CAP entries; a re-added entry refreshes its time
    sl::Tags r;
    for (uint32_t i = 1; i <= 20; ++i) r.add_entry(i, now);
    CHECK(r.n_entry == sl::Tags::CAP && !r.has_entry(1, now, 3.0f) && r.has_entry(20, now, 3.0f) && r.has_entry(5, now, 3.0f));
    r.add_entry(20, now + 2.5); CHECK(r.has_entry(20, now + 5.0, 3.0f) && !r.has_entry(19, now + 5.0, 3.0f));
    CHECK(!r.has_entry(0, now, 3.0f));
}

static void test_swap_stats() {
    sl::Swap s;
    CHECK(!s.active && !s.expired(5.0, 15.0f));
    s.begin(0.0, 10.0, 2);
    CHECK(s.active && s.t_trigger == 10.0 && s.entries == 2);
    s.begin(9.9, 10.0, 2); CHECK(s.t_trigger == 9.9);
    CHECK(!s.expired(20.0, 15.0f) && s.expired(25.5, 15.0f));
    sl::Stats st;
    st.add(1, 400.0); st.add(1, 200.0); st.add(1, 300.0); st.add(0, 700.0); st.add(5, 1.0); st.add(-1, 1.0);
    CHECK(st.n[1] == 3 && st.min_ms[1] == 200.0 && st.max_ms[1] == 400.0 && st.avg(1) == 300.0);
    CHECK(st.n[0] == 1 && st.avg(0) == 700.0 && st.n[2] == 0 && st.avg(2) == 0.0);
}

static const char* TABLE =
    "# TODGameplay seamless prefetch table\r\n"
    "archive todgameplay_data.zip 83897886\r\n"
    "archive blocks.naz 474229592\n"
    "block data_pc/overdose_the_game/overdose/shared/submap01_map04_lostoros_train_harbour.submap todgameplay_data.zip 1000 14509236\n"
    "block data_pc/overdose_the_game/overdose/shared/mainmap_map04_lostoros_train_harbour.map blocks.naz 5000 576716\n"
    "block data_pc/overdose_the_game/overdose/shared/submap01_map02_lostoros_downtown.submap todgameplay_data.zip 20000 18640672\n"
    "loader -306.41 -539.99 16.3 data_pc/overdose_the_game/overdose/shared/submap01_map04_lostoros_train_harbour.submap|data_pc/overdose_the_game/overdose/shared/mainmap_map04_lostoros_train_harbour.map\n"
    "loader -306.41 -495.89 17.8 data_pc/overdose_the_game/overdose/shared/submap01_map02_lostoros_downtown.submap\n"
    "loader 1 2 3 data_pc/unknown.submap\n"
    "block x unknown.zip 1 2\n"
    "nonsense line\n";

static void test_table() {
    sl::Table t;
    CHECK(t.parse(TABLE));
    CHECK(t.na == 2 && t.nb == 3 && t.nl == 2);
    CHECK(t.errors == 4);                    // 3rd loader: unknown block + no block left; the unknown archive; the nonsense
    CHECK(t.a[0].bytes == 83897886ull && !strcmp(t.a[1].name, "blocks.naz"));
    CHECK(t.b[1].archive == 1 && t.b[1].offset == 5000 && t.b[1].size == 576716);
    CHECK(t.l[0].nblock == 2 && t.l[0].block[0] == 0 && t.l[0].block[1] == 1 && t.l[1].nblock == 1);
    CHECK(t.find_block("DATA_PC/overdose_the_game/overdose/shared/submap01_map02_lostoros_downtown.submap") == 2);
    sl::Table e; CHECK(!e.parse("")); CHECK(!e.parse("# only a comment\n"));
    // scheduler: far away -> nothing; near the bridge's south box -> Harbour + Map04 once; again only after the repeat
    sl::Scheduler s;
    int out[8];
    CHECK(s.due(t, 0.0f, 0.0f, 10.0, 60.0f, 180.0f, out, 8) == 0);
    int n = s.due(t, -306.0f, -470.0f, 10.0, 60.0f, 180.0f, out, 8);   // 70 m from the 1st box, 26 m from the 2nd
    CHECK(n == 3);
    n = s.due(t, -306.0f, -470.0f, 11.0, 60.0f, 180.0f, out, 8);
    CHECK(n == 0);
    n = s.due(t, -306.0f, -540.0f, 200.0, 60.0f, 180.0f, out, 8);
    CHECK(n == 3);
    sl::Scheduler s2;
    n = s2.due(t, -306.0f, -620.0f, 10.0, 60.0f, 180.0f, out, 8);       // 80 m south of the 1st box: within 16.3 + 60? no
    CHECK(n == 0);
    n = s2.due(t, -306.0f, -600.0f, 10.0, 60.0f, 180.0f, out, 8);       // 60 m: yes (only the 1st loader's two blocks)
    CHECK(n == 2 && out[0] == 0 && out[1] == 1);
    n = s2.due(t, -306.0f, -470.0f, 10.0, 60.0f, 180.0f, out, 1);       // the 1st loader's blocks were just read
    CHECK(n == 1 && out[0] == 2);
}

static void test_names() {
    CHECK(!strcmp(sl::submap_name(3), "Downtown 02") && !strcmp(sl::submap_name(17), "Industrial 04"));
    CHECK(!strcmp(sl::submap_name(6), "Harbour 01") && !strcmp(sl::submap_name(19), "Jungle 02"));
    CHECK(!strcmp(sl::submap_name(99), "?") && !strcmp(sl::submap_name(-1), "?"));
    CHECK(!strcmp(sl::map_name(3), "Map04 Harbour") && !strcmp(sl::map_name(9), "?"));
}

int main() {
    test_classify();
    test_swap_stats();
    test_table();
    test_names();
    printf("test_seam: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
