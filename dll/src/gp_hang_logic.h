// gp_hang_logic.h - pure logic of the hang watchdog of TODGameplay.dll ([debug] hang_watchdog, dll/src/gp_hang.cpp):
// when the game thread's per-frame heartbeat stops, the watchdog thread decides whether that is a hang (and not a
// minimised window, an alt-tab or a long load), names the code addresses it finds on the stuck thread's stack from the
// DLL's own map file, and says when the heartbeat came back. No Windows, no game memory; tests in tests/dll/test_hang.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace hang {

// ------------------------------------------------------------------------------------------------ the stall decision
struct Cfg {
    float seconds = 5.0f;          // heartbeat silence that counts as a hang (in play / menus)
    float load_seconds = 60.0f;    // the same while a load runs (UpdateLoadedBlocks: one long frame by design)
    float commit_seconds = 1.0f;   // silence while usable this long = no minimise / alt-tab; later focus changes (the
                                   // "not responding" ghost window takes the foreground after ~5 s) do not cancel it
};
// What the watchdog sees at one poll (every ~250 ms).
struct Sample {
    uint32_t beat = 0;             // the game thread's frame counter
    double now = 0.0;              // seconds (any monotonic clock)
    bool usable = true;            // the window is up, not minimised, and the game owns the foreground (or focus ignored)
    bool loading = false;          // a load is running on the game thread
    bool debugger = false;         // a debugger is attached (breakpoints stop the thread)
};
enum Act { NONE = 0, STALL = 1, RECOVERED = 2 };
// One stall is reported once; RECOVERED follows when the heartbeat returns after a reported stall.
// The silence is counted from the last beat, or from the moment the window became usable again (a hang that started
// while the game was minimised or in the background is caught a few seconds after the player comes back).
struct Watch {
    bool armed = false;            // a first beat was seen
    uint32_t last_beat = 0;
    double t_last = 0.0;           // when the silence started counting
    bool usable_then = false;      // the window was usable at that moment (and at every poll since)
    bool committed = false;        // the silence lasted commit_seconds while usable
    bool reported = false;
    double t_reported = 0.0, stall_s = 0.0;
    Act poll(const Cfg& c, const Sample& s);
    double silence(double now) const { return armed ? now - t_last : 0.0; }
};

// ------------------------------------------------------------------------------------------------ return addresses
// Is the code before a stack dword a call instruction? (the 7 bytes before the candidate return address; checks
// E8 rel32, FF 15 [disp32], FF /2 with every ModRM / SIB / displacement form, call reg)
bool looks_like_call(const uint8_t* before_ret_7);

// ------------------------------------------------------------------------------------------------ the map file
// MSVC /MAP text: "Preferred load address is X", the "Publics by Value" and "Static symbols" tables
// ( 0001:000116c0  ?suave_post_input@@YAXPBEPAE1PAKN_N@Z  100126c0 f  suave_game.obj ). Only code ('f') rows are kept.
// The caller hands in the memory: names are copied into `text`, the table into `sym`; both are sorted by address.
struct Sym { uint32_t rva; uint32_t name; uint32_t obj; };   // name / obj: offsets into text
struct MapSyms {
    Sym* sym = nullptr; int n = 0, cap = 0;
    char* text = nullptr; uint32_t used = 0, text_cap = 0;
    uint32_t pref_base = 0;
    int parse(const char* map, size_t len, Sym* sym_mem, int sym_cap, char* text_mem, uint32_t text_cap);   // rows kept
    // the code symbol at or below rva (nullptr: none); *off = rva - its start
    const Sym* find(uint32_t rva, uint32_t* off) const;
    const char* name_of(const Sym* s) const { return text + s->name; }
    const char* obj_of(const Sym* s) const { return text + s->obj; }
};
// the " Timestamp is 6af089ab (...)" line near the top of an MSVC /MAP file: the linker's time stamp, the same value as
// the image's IMAGE_FILE_HEADER.TimeDateStamp (a map of another build does not match). 0 = no such line in the first 4 KB
uint32_t map_timestamp(const char* map, size_t len);
// "?suave_post_input@@YAXPBEPAE1PAKN_N@Z" -> "suave_post_input", "?tick@Sv@sv@@QAE..." -> "sv::Sv::tick",
// "_main" -> "main", "_f@8" -> "f"; anything else is copied as it is
void short_name(const char* decorated, char* out, size_t cap);

}  // namespace hang
