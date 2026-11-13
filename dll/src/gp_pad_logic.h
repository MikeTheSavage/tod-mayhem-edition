// gp_pad_logic.h - pure logic of [pad] (native controller support, prompts, rumble) of TODGameplay.dll. No Windows, no
// game memory; unit tests in tests/dll/test_pad.cpp. The glue is dll/src/gp_pad.cpp.
//
// The model: every device (XInput pad, PlayStation pad over HID, the [debug] scripted pad) produces one State in
// LOGICAL buttons (Xbox positions: A = bottom face button = Cross ...). The game's own gamepad code reads a PS2-style
// pad with 16 VIRTUAL buttons (Gamepad::GamepadButtonToString 0x439A50: SELECT L3 R3 START D_UP D_RIGHT D_DOWN D_LEFT
// L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE) and two sticks; the console control setups in ControlDict bind actions to
// those. A Map (ini [pad] map_*) says which logical button feeds each virtual one; the fake DirectInput device hands
// the result to the game as a DIJOYSTATE.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pad {

// ------------------------------------------------------------------------------------------------ buttons
enum Btn { B_A, B_B, B_X, B_Y, B_LB, B_RB, B_LT, B_RT, B_LS, B_RS, B_BACK, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT,
           B_COUNT,
           // prompt-only pictures (not buttons): the sticks as a whole
           G_LSTICK = B_COUNT, G_RSTICK, G_COUNT };
enum Virt { V_SELECT, V_L3, V_R3, V_START, V_DUP, V_DRIGHT, V_DDOWN, V_DLEFT, V_L2, V_R2, V_L1, V_R1, V_TRIANGLE,
            V_CIRCLE, V_CROSS, V_SQUARE, V_COUNT };
enum Kind { KIND_NONE, KIND_XBOX, KIND_PS };

int btn_from_name(const char* s);          // A B X Y LB RB LT RT LS RS BACK START UP DOWN LEFT RIGHT and the PlayStation
                                           // names CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 SHARE CREATE OPTIONS,
                                           // VIEW MENU SELECT, DPAD_UP.. ; -1 unknown
const char* btn_name(int b);               // "A" .. "RIGHT"
int virt_from_name(const char* s);         // the game's names (SELECT .. SQUARE), -1 unknown
const char* virt_name(int v);

struct State {
    uint32_t buttons = 0;                  // bit Btn
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, up = +y
    float lt = 0, rt = 0;                  // 0..1
    int kind = KIND_NONE;
    bool connected = false;
    bool down(int b) const { return (buttons >> b) & 1u; }
};

// XInput XINPUT_GAMEPAD -> State (wButtons bits: up 1 down 2 left 4 right 8 start 10 back 20 LS 40 RS 80 LB 100 RB 200
// A 1000 B 2000 X 4000 Y 8000)
State from_xinput(uint16_t w, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry);

// ------------------------------------------------------------------------------------------------ PlayStation over HID
enum HidModel { HID_NONE, HID_DS4, HID_DS };
HidModel hid_model(uint16_t vid, uint16_t pid);   // 054C: DS4 05C4 / 09CC / 0BA0, DualSense 0CE6 / Edge 0DF2
uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0);
// One input report (report id first, as ReadFile returns it). Layouts (public knowledge, no pad here to test):
//   DS4 01 (USB 64 B / BT short): 1..4 LX LY RX RY, 5 hat (low nibble) + Square Cross Circle Triangle, 6 L1 R1 L2 R2
//   Share Options L3 R3, 8..9 L2 R2 analog;  DS4 BT 11 (78 B): the same from byte 3, CRC-32 (seed A1) in the last 4;
//   DualSense USB 01: 1..4 sticks, 5..6 triggers, 8 hat + faces, 9 L1 R1 L2 R2 Create Options L3 R3;
//   DualSense BT 31 (78 B): the USB fields from byte 2, CRC-32 last.
// Returns false (out untouched) for a report it does not know or a bad checksum.
bool hid_decode(HidModel m, bool bluetooth, const uint8_t* r, size_t n, State& out);
// Output report that sets the motors (big = left / low frequency, little = right), USB only; returns its length (0 =
// none known: Bluetooth needs the long CRC report, not sent).
size_t hid_rumble_report(HidModel m, bool bluetooth, uint8_t big, uint8_t little, uint8_t* out, size_t cap);

// ------------------------------------------------------------------------------------------------ shaping
struct Shape {
    float dz_left = 0.20f, dz_right = 0.15f;   // radial dead zones (fraction of full deflection)
    float trigger = 0.25f;                     // a trigger above this is a pressed button
    float look_exp = 2.0f;                     // wanted look-rate curve |x|^look_exp (the game cubes the axis: we
                                               // send |x|^(look_exp/3) so the game's cube gives |x|^look_exp)
    float look_sens_x = 1.0f, look_sens_y = 1.0f;
    bool invert_x = false, invert_y = false;
    bool compensate = true;                    // lift past the game's 0.3 axial dead zone (binding type 5)
};
void radial(float& x, float& y, float dz);     // dead zone, rescaled so the edge of the zone is 0, clamped to 1
float lift(float v);                           // v in -1..1 -> sign(v) (0.3 + 0.7 |v|) (0 stays 0)
float look_axis(float v, float exp, float sens);   // |v|^(exp/3) * sens^(1/3), sign kept, clamped to 1

// ------------------------------------------------------------------------------------------------ the game's pad
struct Map { int8_t src[V_COUNT]; };          // virtual -> logical button (-1 = never pressed)
Map default_map();                             // console layout with RT shoot / LT dodge: L1<-LT R1<-RT L2<-LB R2<-RB ...
bool map_set(Map& m, const char* virt_name, const char* btn);   // ini [pad] map_<virt> = <btn>
int map_virt_of(const Map& m, int btn);        // first virtual fed by btn, -1 none
// DIJOYSTATE (80 bytes, c_dfDIJoystick): what GetDeviceState hands to Gamepad::ProcessController 0x439E00. The game
// object is type 2 (no button remap): rgbButtons[virt]; stick 0 = lX / lY, stick 1 = lZ / lRz; axes 0..65535,
// centre 32768 (the game computes v * 1/32768 - 1), y down positive.
#pragma pack(push, 4)
struct DiState { int32_t lX, lY, lZ, lRx, lRy, lRz, slider[2]; uint32_t pov[4]; uint8_t btn[32]; };
#pragma pack(pop)
static_assert(sizeof(DiState) == 80, "DIJOYSTATE is 80 bytes");
void to_dijoystate(const State& s, const Map& m, const Shape& sh, DiState& out);
int32_t axis_to_di(float v);                   // -1..1 -> 0..65535

// ------------------------------------------------------------------------------------------------ device used last
struct Activity {
    int active = 0;                            // 0 keyboard / mouse, 1 pad
    int last_kind = KIND_XBOX;                 // kind of the pad that was used last
    double t_switch = -1e9;
    uint32_t prev_buttons = 0;
    // pad_moved: a button went down, a stick / trigger past 0.5 (the caller passes the State); kbm: a key or mouse
    // button went down or the mouse moved past its threshold this frame. Returns true when `active` changed.
    bool step(const State& s, bool kbm, double now);
};
bool pad_activity(const State& s, uint32_t prev_buttons);

// ------------------------------------------------------------------------------------------------ prompts
enum Mode { MODE_AUTO, MODE_KEYBOARD, MODE_XBOX, MODE_PS };
enum Style { STYLE_KEYBOARD, STYLE_XBOX, STYLE_PS };
int mode_from_name(const char* s);             // auto keyboard xbox playstation (ps), -1 unknown
Style resolve_style(int mode, int pad_active, int pad_kind);
// The 18 font code points that carry the pad pictures (console glyphs of ODPC / ODPS2 that no PC text uses); the glue
// repoints them at the style's cells. glyph_code(G) for G in 0..G_COUNT-1.
uint16_t glyph_code(int g);
int glyph_of_code(uint16_t c);                 // -1 when c is not one of ours
// Text post-processing on the expanded UTF-16 text (in place, NUL-terminated, cap units):
//  * "[" + one or more of our glyphs (and spaces) + "]" -> the glyphs ("Press [A] to use" reads "Press A to use");
//  * the literal bracketed key names of the PC texts (Enter / Esc / A / Space in the five languages) -> the glyph of
//    the matching menu action. Returns the new length.
struct Literal { const char* text; int glyph; };   // text is ASCII/Latin-1 incl. the brackets
size_t post_process(uint16_t* s, size_t cap, const Literal* lit, int nlit);

// ------------------------------------------------------------------------------------------------ rumble
struct Rumble {
    float game[2] = {0, 0};                    // the game's motors (Gamepad +0xD8 motor 0, +0xDC motor 1), 0..1
    float ev_big = 0, ev_small = 0;            // DLL events, decaying
    float ev_t = 0, ev_len = 0;
    void pulse(float big, float little, float seconds);   // a stronger or longer pulse replaces a weaker one
    void step(float dt);
    // final motor values 0..1: max(game, events) x strength, 0 when !enabled; motor 0 -> big (left)
    void out(float strength, bool enabled, float& big, float& little) const;
};

// ------------------------------------------------------------------------------------------------ [debug] pad script
// One command per line ('#' comments). Times in ms. Buttons are btn names, sticks L / R.
//   wait MS | waitlog MS TEXT | mark TEXT | quit-not-supported (use the key script's quit)
//   press BTN [MS=120] | hold BTN | release BTN | stick L|R X Y [MS] | trigger L|R V [MS] | kind xbox|playstation
//   connect | disconnect | rumble_test BIG SMALL MS (a DLL pulse, logged like a game event)
struct PadCmd { enum { WAIT, WAITLOG, MARK, PRESS, HOLD, RELEASE, STICK, TRIGGER, KIND, CONNECT, DISCONNECT, RUMBLE } op;
                int a; int ms; float x, y; char text[48]; };
struct PadScript {
    enum { CAP = 256 };
    PadCmd c[CAP]; int n = 0, errors = 0;
    bool parse(const char* text);
};
struct PadPlayer {
    int pc = 0; double t_next = -1.0; bool done = false; int waiting = -1; double t_wait_end = 0.0;
    State st;                                  // the scripted pad (connected after `connect` or the first input)
    double t_rel[B_COUNT + 4];                 // when a timed press / stick / trigger returns to rest (0 = not timed)
    const char* mark = nullptr;                // set for one step when a mark is hit
    float rumble[3] = {0, 0, 0};               // rumble_test: big, little, seconds (consumed by the glue)
    PadPlayer() { for (double& t : t_rel) t = 0.0; st.kind = KIND_XBOX; }
    typedef bool (*LogHas)(const char* text, void* ctx);
    void step(const PadScript& s, double now, LogHas has = nullptr, void* ctx = nullptr);
};

}  // namespace pad
