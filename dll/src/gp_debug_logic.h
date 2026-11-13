// gp_debug_logic.h - pure logic of the [debug] section of TODGameplay.dll: the key script player, the heap
// tracker tables and the border-swap stress schedule. No Windows, no game memory; tests in tests/dll/test_debug.cpp.
// Everything in [debug] is OFF by default and meant for the private test copy only.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace dbg {

// ------------------------------------------------------------------------------------------------ key script
// One command per line ('#' comments): wait MS | tap KEY | hold KEY MS | mark TEXT | quit |
// waitlog MS TEXT (wait until the game's own log has TEXT, at most MS)
// Keys are names of gp::key_from_name (keyboard only). The player is stepped once per game frame (after the input
// poll); it asks for at most one change per frame.
struct KeyCmd { enum { WAIT, TAP, HOLD, MARK, QUIT, WAITLOG } op; int key; int ms; char text[48]; };
struct KeyScript {
    enum { CAP = 512 };   // the display-mode chain scripts are about 290 lines
    KeyCmd c[CAP]; int n = 0, errors = 0;
    bool parse(const char* text);
};
struct KeyOut {                     // what the glue does this frame
    int press = -1;                 // key that goes down this frame (edge 1, state 0x80)
    int release = -1;               // key that goes up this frame (edge 2, state 0)
    int held = -1;                  // key that stays down (state 0x80)
    const char* mark = nullptr;     // log this
    bool quit = false;
};
struct KeyPlayer {
    int pc = 0; double t_next = -1.0; int down = -1; double t_up = 0.0; bool done = false;
    int waiting = -1; double t_wait_end = 0.0;
    typedef bool (*LogHas)(const char* text, void* ctx);
    KeyOut step(const KeyScript& s, double now, LogHas has = nullptr, void* ctx = nullptr);
};

// ------------------------------------------------------------------------------------------------ heap tracker
// Live set: open addressing on the pointer value (0 empty, 1 tombstone), no allocation after init (the caller hands
// in the memory). Freed ring: the last N frees with their caller addresses, searched newest first on a bad free.
struct LiveSet {
    uint32_t* key = nullptr; uint32_t* val = nullptr; uint32_t cap = 0, used = 0, tombs = 0;
    void init(uint32_t* k, uint32_t* v, uint32_t capacity_pow2);   // zeroes the arrays
    bool insert(uint32_t p, uint32_t size);   // false when full
    bool erase(uint32_t p, uint32_t* size);   // false when not live
    bool find(uint32_t p, uint32_t* size) const;
    bool needs_rebuild() const { return (used + tombs) * 10 > cap * 7; }
    void rebuild_into(LiveSet& dst) const;    // copies the live entries
};
struct FreedRec { uint32_t p, size, swap, frame; double t; uint32_t stack[8]; };
struct FreedRing {
    FreedRec* r = nullptr; uint32_t cap = 0, head = 0, count = 0;
    void init(FreedRec* mem, uint32_t capacity);
    FreedRec& push();
    const FreedRec* find(uint32_t p) const;   // newest record of p, nullptr when not in the ring
};
// Is the dword before ret a call instruction of TOD.exe's code? (E8 rel32, FF 15 [mem], FF /2 reg or [reg+d8/d32])
bool looks_like_call(const uint8_t* code_before_ret_6);

// ------------------------------------------------------------------------------------------------ stress schedule
// Swap k+1 starts interval(k) seconds after swap k is back in play. The interval walks a fixed pseudo-random sequence
// between min and max so the run is repeatable.
struct Stress {
    int total = 0; float imin = 1.2f, imax = 1.2f; uint32_t seed = 12345;
    float interval(int k) const;
};

}  // namespace dbg
