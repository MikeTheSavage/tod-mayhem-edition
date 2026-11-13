// gp_pad_prompts.h - which action a PC prompt means ([pad] prompts). Pure logic, no Windows, no game memory; unit tests
// in tests/dll/test_pad_prompts.cpp, glue in dll/src/gp_pad.cpp (ExpandWrap).
//
// The PC texts name keys: "Press [%0] to ACCELERATE" gets the KEY of the action (W) through a Keyboard text-slot
// parameter, and literal "[Esc]" / "[Enter]" / "[A]" / "[Space]" stand for menu keys. A key can drive several actions
// (W: run, aim_up, Accelerate; S: run_backwards, Decelerate; F: use, use_vehicle; Space: shootdodge, slide, the skips;
// Esc: menu_back, menu_cancel, menu_show), and on the pad those actions sit on different buttons. So the picture a pad
// prompt shows must come from the ACTION the text means, not from the first action bound to the key.
//
// The game data says which action each tutorial parameter is: every Trigger_Activate_Tutorial has Tutorial_pc_index
// children whose SignIndex is the Specific_ID of a PC control, in parameter order (MM013 Tut02_UseCar: ACCELERATE 22,
// DECELERATE 23, USE_VEHICLE 5). The table below carries those, plus the texts shown without such children, keyed by
// the text slot and the ordinal (both the same in every language).
#pragma once

namespace padp {

enum Slot { SLOT_OTHER, SLOT_TUTORIAL_PC, SLOT_MISC_PC, SLOT_MENU, SLOT_COUNT };
int slot_from_name(const char* node_name);   // "Tutorial_PC_TextSlot" ... (TextSlots.fragment names), SLOT_OTHER else
const char* slot_name(int slot);             // "Tutorial_PC" / "Misc_PC" / "Menu" / "?"

enum { MAX_PARAMS = 4 };
struct Hint {
    int slot, ordinal;
    const char* param[MAX_PARAMS];   // [%N] -> the action(s) it means, "a|b" in preference order; nullptr = no hint
    const char* esc;                 // the action a literal [Esc] means in this text (nullptr = the menu default)
};
const Hint* find_hint(int slot, int ordinal);
int hint_count();
const Hint& hint_at(int i);

// Is `name` one of the '|'-separated actions of `list` (case-insensitive)? Its position (0..) or -1.
int list_pos(const char* list, const char* name);
// Copies the first action of `list` into out ("" for none).
void list_first(const char* list, char* out, int cap);

// The action for one key parameter. `cand` are the actions bound to the key the script put in, each with a pad
// picture, in ControlSetup table order (what the old rule picked from: always cand[0]). With a hint the candidate the
// hint prefers wins; when the hint names none of them (the key is shared with nothing the text means, or the text
// names a console-only action like target_object), the result is -1 and the caller shows the hint's first action.
// Without a hint: cand[0] (the old rule). -1 also for n == 0 without a hint.
int pick(const char* const* cand, int n, const char* want);

// "Press [%0] to use" (Misc_PC #86, PC only) is the HUD use prompt (HUDAuxCtrl Mission_HUD/PC_Use_TB). The HUD script
// shows it for a vehicle (0x60D7D0, data +38h = 1: on the pad TRIANGLE, use_vehicle) and for other usable things such as
// save points (0x60D630, data +24h = 1: CROSS, use); the key parameter is F either way. The action from those flags,
// nullptr when neither is set (the table's hint then).
bool is_use_prompt(int slot, int ordinal);
const char* use_prompt_action(int vehicle_flag, int use_flag);

// The literal key names of the PC texts and what they stand for in a menu.
enum Lit { LIT_ENTER, LIT_ESC, LIT_DELETE, LIT_SPACE, LIT_COUNT };
const char* literal_action(int lit, const Hint* h);   // menu_select / menu_back (or h->esc) / menu_delete / tutorial_skip
int literal_fallback_button(int lit);                 // pad::B_A / B_B / B_Y / B_A when the action has no pad binding

}  // namespace padp
