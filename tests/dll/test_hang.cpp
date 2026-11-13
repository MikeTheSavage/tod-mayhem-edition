// test_hang.cpp - unit tests of dll/src/gp_hang_logic.cpp (the [debug] hang_watchdog): the stall decision, the call
// detector of the stack scan, the map file reader and the short names. Built and run by build.bat. Exit code 0 = all
// passed. With an argument (the build's TODGameplay.map) the real map is read too.
#include "../../dll/src/gp_hang_logic.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
using namespace hang;

// the watchdog polls every 0.25 s; frames come at 60 fps while `running`
struct Sim {
    Cfg c; Watch w; uint32_t beat = 0; double t = 0.0;
    int stalls = 0, recovered = 0; double last_stall = 0.0, last_back = 0.0;
    Act step(bool running, bool usable = true, bool loading = false, bool dbg = false) {
        t += 0.25;
        if (running) beat += 15;
        Sample s; s.beat = beat; s.now = t; s.usable = usable; s.loading = loading; s.debugger = dbg;
        Act a = w.poll(c, s);
        if (a == STALL) { ++stalls; last_stall = w.stall_s; }
        if (a == RECOVERED) { ++recovered; last_back = w.stall_s; }
        return a;
    }
    void run(double secs, bool running, bool usable = true, bool loading = false, bool dbg = false) {
        for (int i = 0; i < (int)(secs / 0.25 + 0.5); ++i) step(running, usable, loading, dbg);
    }
};

static void test_watch() {
    {   // boot: no frame yet -> nothing, however long
        Sim s; s.run(30, false);
        CHECK(s.stalls == 0 && !s.w.armed);
    }
    {   // a hang in play: one report after 5 s, once; back -> RECOVERED with the whole silence
        Sim s; s.run(10, true);
        s.run(4.5, false); CHECK(s.stalls == 0);
        s.run(1.0, false); CHECK(s.stalls == 1 && s.last_stall >= 5.0 && s.last_stall < 5.3);
        s.run(30, false); CHECK(s.stalls == 1);
        s.run(1, true); CHECK(s.recovered == 1 && s.last_back > 35.0 && s.last_back < 36.5);
        s.run(10, true); CHECK(s.recovered == 1 && s.stalls == 1);
        s.run(6, false); CHECK(s.stalls == 2);   // a second stall: a second report
    }
    {   // frames that keep coming (paused menus run the main loop too): never
        Sim s; s.run(120, true); CHECK(s.stalls == 0);
    }
    {   // minimised / alt-tab right as the frames stop: no report while away, none when back and running
        Sim s; s.run(5, true);
        s.step(false, true);                     // the last frame before the window went away
        s.run(60, false, false); CHECK(s.stalls == 0);
        s.run(2, true, true); CHECK(s.stalls == 0 && s.recovered == 0);
    }
    {   // a hang that started in the background: counted from the moment the window is back
        Sim s; s.run(5, true);
        s.run(20, false, false); CHECK(s.stalls == 0);
        s.run(4.5, false, true); CHECK(s.stalls == 0);
        s.run(1.0, false, true); CHECK(s.stalls == 1);
    }
    {   // the "not responding" ghost window takes the foreground after ~5 s of a real hang: still reported
        Sim s; s.run(5, true);
        s.run(2.0, false, true);                 // committed (1 s usable)
        s.run(10, false, false); CHECK(s.stalls == 1);
    }
    {   // a long load: 60 s allowed, then reported
        Sim s; s.run(5, true);
        s.run(30, false, true, true); CHECK(s.stalls == 0);
        s.run(31, false, true, true); CHECK(s.stalls == 1);
    }
    {   // a debugger at a breakpoint: never
        Sim s; s.run(5, true);
        s.run(60, false, true, false, true); CHECK(s.stalls == 0);
        s.run(4.5, false, true); CHECK(s.stalls == 0);   // detached: counted from then
        s.run(1.0, false, true); CHECK(s.stalls == 1);
    }
    {   // the 8 s test stall of debug_stall_key: reported once after 5 s, back after ~8 s
        Sim s; s.run(10, true);
        s.run(8, false); s.run(1, true);
        CHECK(s.stalls == 1 && s.recovered == 1 && s.last_back >= 7.9 && s.last_back <= 8.6);
    }
    {   // configurable limit
        Sim s; s.c.seconds = 10; s.run(5, true);
        s.run(9.5, false); CHECK(s.stalls == 0);
        s.run(1, false); CHECK(s.stalls == 1);
    }
}

static void test_call() {
    auto chk = [](std::vector<unsigned char> code) {   // code = the instruction(s) that end right at the return address
        unsigned char b[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        size_t n = code.size(); for (size_t i = 0; i < n; ++i) b[7 - n + i] = code[i];
        return looks_like_call(b);
    };
    CHECK(chk({0xE8, 0x10, 0x20, 0x30, 0x40}));                 // call rel32
    CHECK(chk({0xFF, 0x15, 0xC8, 0x32, 0x9B, 0x00}));           // call [0x9B32C8]
    CHECK(chk({0xFF, 0xD3}));                                   // call ebx (the frame call at 0x43C51B)
    CHECK(chk({0xFF, 0xD5}));                                   // call ebp
    CHECK(chk({0xFF, 0x10}));                                   // call [eax]
    CHECK(chk({0xFF, 0x50, 0x18}));                             // call [eax+18h] (a vtable slot)
    CHECK(chk({0xFF, 0x54, 0x24, 0x10}));                       // call [esp+10h]
    CHECK(chk({0xFF, 0x14, 0x85, 0x00, 0x10, 0x40, 0x00}));     // call [eax*4+401000h]
    CHECK(chk({0xFF, 0x92, 0x00, 0x01, 0x00, 0x00}));           // call [edx+100h]
    CHECK(chk({0xFF, 0x94, 0x24, 0x00, 0x01, 0x00, 0x00}));     // call [esp+100h]
    CHECK(!chk({0x90, 0x90, 0x90}));
    CHECK(!chk({0x8B, 0x45, 0x08}));                            // mov eax,[ebp+8]
    CHECK(!chk({0xFF, 0x75, 0x08}));                            // push [ebp+8] (FF /6)
    CHECK(!chk({0xFF, 0xE0}));                                  // jmp eax (FF /4)
}

static const char* MAP =
    " TODGameplay\r\n\r\n"
    " Timestamp is 6a0b1c2d (Sun Nov  8 06:00:00 2026)\r\n\r\n"
    " Preferred load address is 10000000\r\n\r\n"
    " Start         Length     Name                   Class\r\n"
    " 0001:00000000 0009f000H .text$mn                CODE\r\n\r\n"
    "  Address         Publics by Value              Rva+Base       Lib:Object\r\n\r\n"
    " 0000:00000000       ___safe_se_handler_count   00000000     <absolute>\r\n"
    " 0001:00000d10       ?PostInputC@@YAXXZ         10001d10 f   gp_game.obj\r\n"
    " 0001:000116c0       ?suave_post_input@@YAXPBEPAE1PAKN_N@Z 100126c0 f   suave_game.obj\r\n"
    " 0001:00012000       ?tick@Sv@sv@@QAE?AUActs@2@ABUCfg@2@ABUSnap@2@M@Z 10013000 f i suave_logic.obj\r\n"
    " 0001:00020000       _hang_debug_stall          10021000 f   gp_hang.obj\r\n"
    " 0002:00000010       ?g_table@@3PAHA            100a0010     gp_hud_tiles.obj\r\n"
    " 0003:00000100       _g_beat                    100b1100     gp_hang.obj\r\n\r\n"
    "  entry point at        0001:0009e5d0\r\n\r\n"
    " Static symbols\r\n\r\n"
    " 0001:00000960       ?LimiterC@@YAXXZ           10001960 f   gp_game.obj\r\n"
    " 0001:00030000       _b3FindBestSibling         10031000 f   dynamic_tree.obj\r\n";

static void test_map() {
    static Sym sym[64]; static char text[4096];
    MapSyms m;
    int n = m.parse(MAP, strlen(MAP), sym, 64, text, sizeof text);
    CHECK(n == 6);
    CHECK(m.pref_base == 0x10000000);
    uint32_t off = 0;
    const Sym* s = m.find(0x1d10 + 0x1000, &off);   // rva of PostInputC = 0x1d10 (Rva+Base - base); next is 0x126c0
    CHECK(s && !strcmp(m.name_of(s), "?PostInputC@@YAXXZ") && off == 0x1000);
    s = m.find(0x1d10, &off); CHECK(s && !strcmp(m.name_of(s), "?PostInputC@@YAXXZ") && off == 0 && !strcmp(m.obj_of(s), "gp_game.obj"));
    s = m.find(0x1d10 + 0x20, &off); CHECK(s && !strcmp(m.name_of(s), "?PostInputC@@YAXXZ") && off == 0x20);
    s = m.find(0x1960 + 4, &off); CHECK(s && !strcmp(m.name_of(s), "?LimiterC@@YAXXZ") && off == 4);   // a static symbol, sorted in
    s = m.find(0x21000 + 0x45, &off); CHECK(s && !strcmp(m.name_of(s), "_hang_debug_stall") && off == 0x45 && !strcmp(m.obj_of(s), "gp_hang.obj"));
    s = m.find(0x13000 + 1, &off); CHECK(s && !strcmp(m.obj_of(s), "suave_logic.obj"));   // "f i" rows
    CHECK(m.find(0x100, &off) == nullptr);                    // below the first function
    s = m.find(0x31000 + 0x99999, &off); CHECK(s && !strcmp(m.name_of(s), "_b3FindBestSibling"));
    // no data rows, no absolute rows
    for (int i = 0; i < m.n; ++i) CHECK(strcmp(m.name_of(&m.sym[i]), "_g_beat") && strcmp(m.name_of(&m.sym[i]), "?g_table@@3PAHA"));
    // a tiny table: rows beyond the capacity are dropped, nothing overflows
    static Sym s2[2]; static char t2[64]; MapSyms m2;
    CHECK(m2.parse(MAP, strlen(MAP), s2, 2, t2, sizeof t2) <= 2);
    CHECK(m2.used <= sizeof t2);
    MapSyms m3; static Sym s3[4]; static char t3[256];
    CHECK(m3.parse("no map here\n", 12, s3, 4, t3, sizeof t3) == 0 && m3.find(5, &off) == nullptr);
}

static void test_names() {
    char b[128];
    short_name("?suave_post_input@@YAXPBEPAE1PAKN_N@Z", b, sizeof b); CHECK(!strcmp(b, "suave_post_input"));
    short_name("?tick@Sv@sv@@QAE?AUActs@2@ABUCfg@2@ABUSnap@2@M@Z", b, sizeof b); CHECK(!strcmp(b, "sv::Sv::tick"));
    short_name("_hang_debug_stall", b, sizeof b); CHECK(!strcmp(b, "hang_debug_stall"));
    short_name("_DllMain@12", b, sizeof b); CHECK(!strcmp(b, "DllMain"));
    short_name("??__Eg_e@@YAXXZ", b, sizeof b); CHECK(!strcmp(b, "??__Eg_e@@YAXXZ"));
    short_name("plain", b, sizeof b); CHECK(!strcmp(b, "plain"));
    short_name("?a_very_long_function_name@@YAXXZ", b, 8); CHECK(strlen(b) == 7);
    short_name(nullptr, b, sizeof b); CHECK(b[0] == 0);
    // the map's linker time stamp (compared with the module's PE time stamp: a map of another build gives no names)
    const char* M = " TODGameplay\r\n\r\n Timestamp is 6af089ab (Sun Nov  8 06:39:39 2026)\r\n\r\n Preferred load address is 10000000\r\n";
    CHECK(map_timestamp(M, strlen(M)) == 0x6af089abu);
    CHECK(map_timestamp(" Timestamp is 0000BEEF (x)", 26) == 0xBEEFu);
    CHECK(map_timestamp(" Timestamp is zz", 16) == 0);
    CHECK(map_timestamp("no stamp here", 13) == 0 && map_timestamp(nullptr, 5) == 0);
    std::vector<char> late(5000, ' '); memcpy(late.data() + 4500, "Timestamp is 12345678", 21);
    CHECK(map_timestamp(late.data(), late.size()) == 0);   // only the head of the file is looked at
}

static void test_real_map(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) { printf("test_hang: no map %s\n", path); ++g_fail; return; }
    std::vector<char> t(1 << 24); size_t n = fread(t.data(), 1, t.size(), f); fclose(f);
    std::vector<Sym> sym(32768); std::vector<char> names(2u << 20);
    MapSyms m; int k = m.parse(t.data(), n, sym.data(), (int)sym.size(), names.data(), (uint32_t)names.size());
    CHECK(k > 1000 && m.pref_base == 0x10000000);
    bool stall = false, post = false;
    for (int i = 0; i < m.n; ++i) {
        if (!strcmp(m.name_of(&m.sym[i]), "_hang_debug_stall")) { stall = true; uint32_t o; CHECK(m.find(m.sym[i].rva + 3, &o) == &m.sym[i] && o == 3); }
        if (!strcmp(m.name_of(&m.sym[i]), "?PostInputC@@YAXXZ")) post = true;
        if (i) CHECK(m.sym[i - 1].rva <= m.sym[i].rva);
    }
    CHECK(stall && post);
    // the build's own map matches the build's own DLL (TODGameplay.dll next to the map)
    std::string dll(path); size_t dot = dll.rfind('.'); dll = dll.substr(0, dot) + ".dll";
    FILE* d = fopen(dll.c_str(), "rb"); uint32_t pe_ts = 0;
    if (d) {
        unsigned char h[4096]; size_t hn = fread(h, 1, sizeof h, d); fclose(d);
        uint32_t lf = hn >= 64 ? *(uint32_t*)(h + 0x3C) : 0;
        if (lf + 12 <= hn && !memcmp(h + lf, "PE\0\0", 4)) pe_ts = *(uint32_t*)(h + lf + 8);
    }
    CHECK(pe_ts != 0 && map_timestamp(t.data(), n) == pe_ts);
    printf("test_hang: %s: %d code symbols, time stamp %08X = %s\n", path, k, map_timestamp(t.data(), n), dll.c_str());
}

int main(int argc, char** argv) {
    test_watch(); test_call(); test_map(); test_names();
    if (argc > 1) test_real_map(argv[1]);
    printf("test_hang: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
