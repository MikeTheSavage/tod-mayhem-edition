// seam_logic.h - pure logic of the [seamless] feature of TODGameplay.dll: which load batches are seamless
// border swaps, the swap timing record, and the prefetch table / scheduler. No Windows, no game memory: the game glue
// (seam_game.cpp) reads the engine's lists and calls these; tests/ in seamless/code/test_seam.cpp.
#pragma once
#include <stdint.h>

namespace sl {

struct Cfg {
    bool enabled = false;          // [seamless] enabled
    bool prefetch = true;          // [seamless] prefetch: read the neighbour blocks into the OS cache near a loader
    float prefetch_distance = 60;  // metres from a loader volume's edge
    float prefetch_repeat = 180;   // seconds before the same block is read again
    float tag_window = 3.0f;       // a walk tag older than this no longer counts (seconds)
    float active_timeout = 15.0f;  // a seamless swap that never reaches its post-load end is dropped after this
    int max_entries = 4;           // more pending blocks than this = not a border swap (missions add mission blocks)
    bool log_swaps = true;
};

// ---- walk tags: the LoadCtrl list entries a walk loader (ActionType 21, no Teleport_to needed here) appended, and
// the target folders it named. Both are recorded by the hook around the action's command_load_block call.
struct Tags {
    enum { CAP = 16 };
    uint32_t entry[CAP]; double entry_t[CAP]; int n_entry = 0;
    uint32_t folder[CAP]; double folder_t[CAP]; int n_folder = 0;
    void clear() { n_entry = n_folder = 0; }
    void add_entry(uint32_t p, double t);
    void add_folder(uint32_t f, double t);
    bool has_entry(uint32_t p, double now, float window) const;
    bool has_folder(uint32_t f, double now, float window) const;
    double first_time() const;      // earliest tag time (the trigger moment), 0 when none
};

struct Pending {                    // one s_BlockLoad of the LoadCtrl's Load_pending_list
    uint32_t entry = 0;             // the s_BlockLoad pointer (list element)
    uint32_t folder = 0;            // pBlock
    bool teleport = false;          // sTeleportName not empty
    int kind = -1;                  // Folder kind: 1 map, 2 submap, 3 mission ... (-1 unknown)
};

enum Verdict { V_OFF = 0, V_NORMAL, V_SEAMLESS };
struct Decision {
    Verdict v = V_OFF;
    const char* why = "";
    int walk_by_entry = 0, walk_by_folder = 0;
};
// A batch is a seamless border swap when the feature is on, it has 1..max_entries entries, none teleports, every one
// is a map or submap folder (kind 1/2; unknown kinds refuse) and every one was appended by a walk loader (entry tag)
// or names a folder a walk loader asked for (folder tag), within the tag window.
Decision classify(const Cfg& c, const Pending* p, int n, const Tags& t, double now);

// ---- the state of one seamless swap (begin at the batch decision, end at the post-load fade site or a timeout)
struct Swap {
    bool active = false;
    double t_trigger = 0, t_decide = 0, t_swap0 = 0, t_swap1 = 0, t_end = 0;
    int entries = 0;
    void begin(double trigger, double now, int n) { active = true; t_trigger = trigger > 0 ? trigger : now; t_decide = now; t_swap0 = t_swap1 = t_end = 0; entries = n; }
    bool expired(double now, float timeout) const { return active && now - t_decide > timeout; }
};

struct Stats {                      // every timed UpdateLoadedBlocks call (all loads), by kind
    int n[3] = {0, 0, 0};           // 0 normal load, 1 seamless border swap, 2 save load
    double sum_ms[3] = {0, 0, 0}, min_ms[3] = {0, 0, 0}, max_ms[3] = {0, 0, 0};
    void add(int kind, double ms);
    double avg(int kind) const { return n[kind] ? sum_ms[kind] / n[kind] : 0.0; }
};

// ---- prefetch table (TODGameplay_seamless.txt, written by seamless/tools/make_prefetch_table.py)
//   archive <file name> <file bytes>
//   block <entry> <archive file name> <offset> <size>
//   loader <x> <z> <radius> <entry>[|<entry>...]
struct Archive { char name[64]; uint64_t bytes; };
struct Block { char entry[128]; int archive; uint32_t offset, size; };
struct Loader { float x, z, r; int block[4]; int nblock; };
struct Table {
    enum { MAX_A = 8, MAX_B = 64, MAX_L = 256 };
    Archive a[MAX_A]; int na = 0;
    Block b[MAX_B]; int nb = 0;
    Loader l[MAX_L]; int nl = 0;
    int errors = 0;
    bool parse(const char* text);   // false when nothing usable
    int find_archive(const char* name) const;
    int find_block(const char* entry) const;
};

struct Scheduler {
    double last[Table::MAX_B];
    Scheduler() { for (int i = 0; i < Table::MAX_B; ++i) last[i] = -1e9; }
    // blocks to read now: loaders within distance of (x, z) whose blocks were not read for `repeat` seconds.
    // Returns the count written to out (block indices), marks them as read at `now`.
    int due(const Table& t, float x, float z, double now, float distance, float repeat, int* out, int cap);
};

const char* submap_name(int specific_id);     // Specific_ID -> "Downtown 02" (level streaming doc 2.3)
const char* map_name(int specific_id);

}  // namespace sl
