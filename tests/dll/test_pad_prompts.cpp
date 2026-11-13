// test_pad_prompts.cpp - unit tests of the [pad] prompt hints (dll/src/gp_pad_prompts.cpp): which action a PC prompt
// parameter or literal key name means, so the pad picture follows the action that works in that place.
#include <stdio.h>
#include <string.h>
#include "../../dll/src/gp_pad_prompts.h"
#include "../../dll/src/gp_pad_logic.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
using namespace padp;

static void test_slots() {
    CHECK(slot_from_name("Tutorial_PC_TextSlot") == SLOT_TUTORIAL_PC);
    CHECK(slot_from_name("misc_pc_textslot") == SLOT_MISC_PC);
    CHECK(slot_from_name("Menu_TextSlot") == SLOT_MENU);
    CHECK(slot_from_name("Tutorial_TextSlot") == SLOT_OTHER);      // the console texts: no hints
    CHECK(slot_from_name("Keyboard_TextSlot") == SLOT_OTHER);
    CHECK(slot_from_name("Tutorial_PC_TextSlotX") == SLOT_OTHER);
    CHECK(slot_from_name("") == SLOT_OTHER);
    CHECK(slot_from_name(nullptr) == SLOT_OTHER);
    CHECK(!strcmp(slot_name(SLOT_TUTORIAL_PC), "Tutorial_PC"));
    CHECK(!strcmp(slot_name(SLOT_MISC_PC), "Misc_PC"));
    CHECK(!strcmp(slot_name(SLOT_OTHER), "?"));
}

static void test_lists() {
    CHECK(list_pos("Decelerate|brake", "Decelerate") == 0);
    CHECK(list_pos("Decelerate|brake", "BRAKE") == 1);
    CHECK(list_pos("Decelerate|brake", "brak") == -1);
    CHECK(list_pos("Decelerate|brake", "brakes") == -1);
    CHECK(list_pos("use", "use_vehicle") == -1);
    CHECK(list_pos("use_vehicle|use", "use") == 1);
    CHECK(list_pos(nullptr, "use") == -1);
    CHECK(list_pos("use", "") == -1);
    char b[16];
    list_first("use_vehicle|use", b, sizeof b); CHECK(!strcmp(b, "use_vehicle"));
    list_first("rewind", b, sizeof b); CHECK(!strcmp(b, "rewind"));
    list_first("change_power_up_left", b, 8); CHECK(!strcmp(b, "change_"));   // truncated, terminated
    list_first(nullptr, b, sizeof b); CHECK(b[0] == 0);
}

static void test_hints() {
    // every hint is reachable and the table has no duplicate key
    for (int i = 0; i < hint_count(); ++i) {
        const Hint& h = hint_at(i);
        CHECK(find_hint(h.slot, h.ordinal) == &h);
        CHECK(h.slot == SLOT_TUTORIAL_PC || h.slot == SLOT_MISC_PC);
        CHECK(h.param[0] || h.esc);
    }
    // MM013 Tut02_UseCar: "Press [%0] to ACCELERATE, [%1] to DECELERATE, [%2] to EXIT" (Tutorial_pc_index 22 23 5)
    const Hint* car = find_hint(SLOT_TUTORIAL_PC, 12);
    CHECK(car && !strcmp(car->param[0], "Accelerate") && list_pos(car->param[1], "Decelerate") == 0
          && !strcmp(car->param[2], "use_vehicle"));
    const Hint* stunt = find_hint(SLOT_TUTORIAL_PC, 13);
    CHECK(stunt && !strcmp(stunt->param[0], "prepare_exit_stunt") && !strcmp(stunt->param[1], "do_exit_stunt"));
    // the death screen: [Esc] there is menu_show, the rewind text's [%0] is rewind
    const Hint* dead = find_hint(SLOT_MISC_PC, 14);
    CHECK(dead && !dead->param[0] && !strcmp(dead->esc, "menu_show"));
    const Hint* rw = find_hint(SLOT_MISC_PC, 13);
    CHECK(rw && !strcmp(rw->param[0], "rewind"));
    CHECK(find_hint(SLOT_MISC_PC, 76) && !strcmp(find_hint(SLOT_MISC_PC, 76)->esc, "menu_show"));
    CHECK(find_hint(SLOT_TUTORIAL_PC, 999) == nullptr);
    CHECK(find_hint(SLOT_MENU, 12) == nullptr);       // ordinals are per slot
    CHECK(find_hint(SLOT_OTHER, 12) == nullptr);
}

static void test_pick() {
    // S drives run_backwards, aim_down and Decelerate (ControlSetup order): the old rule showed run_backwards (stick)
    const char* s_key[] = {"run_backwards", "aim_down", "Decelerate"};
    const Hint* car = find_hint(SLOT_TUTORIAL_PC, 12);
    CHECK(pick(s_key, 3, car->param[1]) == 2);
    CHECK(pick(s_key, 3, nullptr) == 0);              // no hint: the old rule
    // W: run, aim_up, Accelerate
    const char* w_key[] = {"run", "aim_up", "Accelerate"};
    CHECK(pick(w_key, 3, car->param[0]) == 2);
    // F: use, use_vehicle -> use_vehicle (TRIANGLE), not use
    const char* f_key[] = {"use", "use_vehicle"};
    CHECK(pick(f_key, 2, car->param[2]) == 1);
    CHECK(pick(f_key, 2, "use_vehicle|use") == 1);    // hint order wins over table order
    CHECK(pick(f_key, 2, "use|use_vehicle") == 0);
    // the hint names none of the key's actions: -1, the caller shows the hint's first action
    const char* space[] = {"shootdodge", "slide"};
    CHECK(pick(space, 2, "target_object") == -1);
    CHECK(pick(space, 2, "slide") == 1);
    CHECK(pick(space, 0, "slide") == -1);
    CHECK(pick(space, 0, nullptr) == -1);
    CHECK(pick(space, 2, "") == 0);
}

static void test_use_prompt() {
    CHECK(is_use_prompt(SLOT_MISC_PC, 86));
    CHECK(!is_use_prompt(SLOT_TUTORIAL_PC, 86));
    CHECK(!is_use_prompt(SLOT_MISC_PC, 85));
    CHECK(!strcmp(use_prompt_action(1, 0), "use_vehicle"));   // next to a car: TRIANGLE
    CHECK(!strcmp(use_prompt_action(0, 1), "use"));           // save point and other usables: CROSS
    CHECK(!strcmp(use_prompt_action(1, 1), "use_vehicle"));
    CHECK(use_prompt_action(0, 0) == nullptr);                // neither: the table's hint
    const char* f_key[] = {"use", "use_vehicle"};
    CHECK(pick(f_key, 2, use_prompt_action(1, 0)) == 1);
    CHECK(pick(f_key, 2, use_prompt_action(0, 1)) == 0);
    CHECK(pick(f_key, 2, find_hint(SLOT_MISC_PC, 86)->param[0]) == 0);
}

static void test_literals() {
    const Hint* dead = find_hint(SLOT_MISC_PC, 14);
    CHECK(!strcmp(literal_action(LIT_ESC, dead), "menu_show"));
    CHECK(!strcmp(literal_action(LIT_ESC, nullptr), "menu_back"));
    CHECK(!strcmp(literal_action(LIT_ESC, find_hint(SLOT_TUTORIAL_PC, 12)), "menu_back"));   // hint without esc
    CHECK(!strcmp(literal_action(LIT_ENTER, dead), "menu_select"));
    CHECK(!strcmp(literal_action(LIT_DELETE, nullptr), "menu_delete"));
    CHECK(!strcmp(literal_action(LIT_SPACE, nullptr), "tutorial_skip"));
    CHECK(literal_action(LIT_COUNT, nullptr) == nullptr);
    CHECK(literal_fallback_button(LIT_ENTER) == pad::B_A);
    CHECK(literal_fallback_button(LIT_ESC) == pad::B_B);
    CHECK(literal_fallback_button(LIT_DELETE) == pad::B_Y);
    CHECK(literal_fallback_button(LIT_SPACE) == pad::B_A);
    CHECK(literal_fallback_button(-1) == -1);
}

int main() {
    test_slots();
    test_lists();
    test_hints();
    test_pick();
    test_use_prompt();
    test_literals();
    printf("pad prompt tests: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
