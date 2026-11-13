// gp_pad_prompts.cpp - which action a PC prompt means (see gp_pad_prompts.h). Unit tests: tests/dll/test_pad_prompts.cpp.
#include "gp_pad_prompts.h"
#include "gp_pad_logic.h"
#include <string.h>
#include <ctype.h>

namespace padp {

static bool ieq_n(const char* a, const char* b, size_t n) {   // a (n chars) == b (NUL-terminated), case-insensitive
    size_t i = 0;
    for (; i < n && b[i]; ++i) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return i == n && !b[i];
}

int slot_from_name(const char* n) {
    if (!n) return SLOT_OTHER;
    static const struct { const char* name; int slot; } T[] = {
        {"Tutorial_PC_TextSlot", SLOT_TUTORIAL_PC}, {"Misc_PC_TextSlot", SLOT_MISC_PC}, {"Menu_TextSlot", SLOT_MENU}};
    for (auto& t : T) if (ieq_n(n, t.name, strlen(n))) return t.slot;
    return SLOT_OTHER;
}
const char* slot_name(int s) {
    switch (s) {
    case SLOT_TUTORIAL_PC: return "Tutorial_PC";
    case SLOT_MISC_PC: return "Misc_PC";
    case SLOT_MENU: return "Menu";
    default: return "?";
    }
}

// Tutorial_PC: the parameters of every uk_Tutorial_PC.txt text with [%N]. Where the data has Tutorial_pc_index
// children (SignIndex = PC Specific_ID) the actions are those (marked D); the others follow the text and the console
// version of the same ordinal in uk_Tutorial.txt, which names the console button (marked C).
#define LOCO "change_power_up_left", "change_power_up_right", "use_powerup"
static const Hint HINTS[] = {
    {SLOT_TUTORIAL_PC, 1, {"shootdodge"}, nullptr},                                      // C L1
    {SLOT_TUTORIAL_PC, 2, {"shoot"}, nullptr},                                           // D 41
    {SLOT_TUTORIAL_PC, 4, {"shoot", "shootdodge"}, nullptr},                             // C R1, L1
    {SLOT_TUTORIAL_PC, 5, {"use_powerup"}, nullptr},                                     // MED KIT (LCONTROL)
    {SLOT_TUTORIAL_PC, 6, {"change_down", "change_up", "reload"}, nullptr},              // D 43 42 6
    {SLOT_TUTORIAL_PC, 7, {"rewind"}, nullptr},                                          // D 14 (REWIND pickup)
    {SLOT_TUTORIAL_PC, 8, {"shoot"}, nullptr},                                           // D 41
    {SLOT_TUTORIAL_PC, 11, {"use_vehicle"}, nullptr},                                    // C triangle (enter vehicles)
    {SLOT_TUTORIAL_PC, 12, {"Accelerate", "Decelerate|brake", "use_vehicle"}, nullptr},  // D 22 23 5
    {SLOT_TUTORIAL_PC, 13, {"prepare_exit_stunt", "do_exit_stunt"}, nullptr},            // D 12 11
    {SLOT_TUTORIAL_PC, 14, {"run"}, nullptr},                                            // D 0 (sprint: the left stick)
    {SLOT_TUTORIAL_PC, 15, {LOCO}, nullptr},                                             // D 29 30 13
    {SLOT_TUTORIAL_PC, 19, {"use"}, nullptr},                                            // D 4 (save point)
    {SLOT_TUTORIAL_PC, 20, {"operate_up", "operate_down"}, nullptr},                     // D 44 49 (forklift)
    {SLOT_TUTORIAL_PC, 21, {"headshot"}, nullptr},                                       // D 48
    {SLOT_TUTORIAL_PC, 22, {"reload"}, nullptr},                                         // C triangle
    {SLOT_TUTORIAL_PC, 23, {"shootdodge"}, nullptr},                                     // D 40
    {SLOT_TUTORIAL_PC, 24, {"shootdodge"}, nullptr},                                     // D 40
    {SLOT_TUTORIAL_PC, 25, {"shootdodge"}, nullptr},                                     // D 40 (wall walk)
    {SLOT_TUTORIAL_PC, 26, {"shootdodge", "shoot"}, nullptr},                            // D 40 41 (wall bounce)
    {SLOT_TUTORIAL_PC, 29, {"headshot"}, nullptr},                                       // D 48
    {SLOT_TUTORIAL_PC, 34, {LOCO}, nullptr},                                             // D
    {SLOT_TUTORIAL_PC, 35, {LOCO}, nullptr},                                             // D
    {SLOT_TUTORIAL_PC, 36, {LOCO}, nullptr},                                             // D
    {SLOT_TUTORIAL_PC, 37, {LOCO}, nullptr},                                             // D
    {SLOT_TUTORIAL_PC, 39, {"shoot", "shoot"}, nullptr},                                 // C R1 R1 (hand grenade)
    {SLOT_TUTORIAL_PC, 43, {"shootdodge"}, nullptr},                                     // C L1
    {SLOT_TUTORIAL_PC, 44, {"shootdodge"}, nullptr},                                     // C L1
    {SLOT_TUTORIAL_PC, 46, {"rewind", "rewind"}, nullptr},                               // C d-pad down
    {SLOT_TUTORIAL_PC, 52, {"shoot", "shoot"}, nullptr},                                 // D 41 41
    {SLOT_TUTORIAL_PC, 54, {"change_down", "change_up"}, nullptr},                       // D 43 42
    {SLOT_TUTORIAL_PC, 55, {"target_object", "target_object", "shoot"}, nullptr},        // C circle circle R1
    {SLOT_TUTORIAL_PC, 62, {"use_vehicle|use"}, nullptr},                                // C triangle (mounted gun)
    {SLOT_TUTORIAL_PC, 64, {LOCO}, nullptr},                                             // C
    {SLOT_TUTORIAL_PC, 65, {"use_powerup"}, nullptr},                                    // HOT CHILI (LCONTROL)
    {SLOT_TUTORIAL_PC, 71, {nullptr}, "menu_show"},                                      // "press [Esc] to enter the menu"
    {SLOT_TUTORIAL_PC, 72, {nullptr}, "menu_show"},                                      // same
    {SLOT_TUTORIAL_PC, 82, {"shootdodge"}, nullptr},                                     // C L1
    {SLOT_TUTORIAL_PC, 83, {LOCO}, nullptr},                                             // D
    {SLOT_TUTORIAL_PC, 85, {"headshot"}, nullptr},                                       // C square
    // Misc_PC: the death screen and the use prompt. "You are dead! Press [Esc]" waits for menu_show (on PC Esc opens
    // the menu; on the console the same text shows the SELECT button = menu_show there), not menu_back / cancel.
    {SLOT_MISC_PC, 13, {"rewind"}, nullptr},                                             // You are dead! Press [%0] to Rewind
    {SLOT_MISC_PC, 14, {nullptr}, "menu_show"},                                          // You are dead! Press [Esc]
    {SLOT_MISC_PC, 75, {"rewind"}, nullptr},                                             // (Xbox copy of 13)
    {SLOT_MISC_PC, 76, {nullptr}, "menu_show"},                                          // (Xbox copy of 14)
    {SLOT_MISC_PC, 86, {"use|use_vehicle"}, nullptr},                                    // Press [%0] to use (see use_prompt_action)
};
#undef LOCO
const Hint* find_hint(int slot, int ordinal) {
    for (const Hint& h : HINTS) if (h.slot == slot && h.ordinal == ordinal) return &h;
    return nullptr;
}
int hint_count() { return (int)(sizeof HINTS / sizeof HINTS[0]); }
const Hint& hint_at(int i) { return HINTS[i]; }

int list_pos(const char* list, const char* name) {
    if (!list || !name || !name[0]) return -1;
    int pos = 0;
    for (const char* p = list; *p; ++pos) {
        const char* e = strchr(p, '|'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && ieq_n(p, name, n)) return pos;
        if (!e) break;
        p = e + 1;
    }
    return -1;
}
void list_first(const char* list, char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!list) return;
    int i = 0;
    for (; list[i] && list[i] != '|' && i < cap - 1; ++i) out[i] = list[i];
    out[i] = 0;
}
int pick(const char* const* cand, int n, const char* want) {
    if (!want || !want[0]) return n > 0 ? 0 : -1;
    int best = -1, best_pos = 1 << 30;
    for (int i = 0; i < n; ++i) {
        int p = list_pos(want, cand[i]);
        if (p >= 0 && p < best_pos) { best = i; best_pos = p; }
    }
    return best;
}

bool is_use_prompt(int slot, int ordinal) { return slot == SLOT_MISC_PC && ordinal == 86; }
const char* use_prompt_action(int vehicle_flag, int use_flag) {
    if (vehicle_flag) return "use_vehicle";   // both set: the vehicle prompt is the one the HUD shows most (cars everywhere)
    if (use_flag) return "use";
    return nullptr;
}

const char* literal_action(int lit, const Hint* h) {
    switch (lit) {
    case LIT_ENTER: return "menu_select";
    case LIT_ESC: return h && h->esc ? h->esc : "menu_back";
    case LIT_DELETE: return "menu_delete";
    case LIT_SPACE: return "tutorial_skip";
    default: return nullptr;
    }
}
int literal_fallback_button(int lit) {
    switch (lit) {
    case LIT_ENTER: return pad::B_A;
    case LIT_ESC: return pad::B_B;
    case LIT_DELETE: return pad::B_Y;
    case LIT_SPACE: return pad::B_A;
    default: return -1;
    }
}

}  // namespace padp
