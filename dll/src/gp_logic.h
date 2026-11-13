// gp_logic.h - the pure logic of TODGameplay.dll: no game memory, no Windows calls. Everything here is unit tested
// by tests/dll/test_logic.cpp (built and run by build.bat). The game glue (gp_game.cpp) only reads/writes game memory
// and turns the actions returned here into the game's own calls.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace gp {

// ------------------------------------------------------------------------------------------------ ini
// Minimal ini: [section] lines, key = value, ';' or '#' comments (full line or after the value), case-insensitive.
struct IniEntry { char section[32]; char key[48]; char value[128]; };
struct Ini {
    static const int CAP = 512;   // entries kept (the merged sample ini is near 250; a full one must not drop the last sections)
    IniEntry e[CAP];
    int n = 0;
    void parse(const char* text);
    const char* get(const char* section, const char* key) const;   // nullptr when missing
    int   geti(const char* section, const char* key, int def) const;
    float getf(const char* section, const char* key, float def) const;
};

// ------------------------------------------------------------------------------------------------ keys
// Input "codes": 0..255 = DirectInput DIK codes (the game's keyboard array index), 0x100 + b = mouse button b (0..7),
// 0x200 + bit = XInput gamepad button bit (0..15), -1 = none.
enum { KEY_NONE = -1, KEY_MOUSE = 0x100, KEY_PAD = 0x200 };
int  key_from_name(const char* name);            // "SPACE", "LSHIFT", "F10", "MOUSE3", "PAD_RB", "0x39", "NONE" -> code
const char* key_name(int code);                  // inverse for logs (static buffer for unknown codes)

// Game action names (PC keyboard defaults of ControlDict) -> the game DIK the action is read from.
int  action_default_key(const char* action);     // "jump" -> DIK_LSHIFT, unknown -> -1

// Key rebinding: out[g] = in[src[g]] for bound game keys g; a physical keyboard key that was moved to another game key
// and is not itself bound keeps no function of its own (out = 0). Mouse sources are allowed for keyboard game keys.
struct KeyRemap {
    int16_t src[256];   // -2 = not rebound (identity); otherwise a code (KEY_NONE = unbound)
    bool any = false;
    void clear();
    bool bind(int game_dik, int source_code);   // false if the game key is not a keyboard DIK or the source is a pad code
    // edges: bit0 pressed this frame, bit1 released this frame (the game's keyboard object +0x000..+0x0FF)
    // state: bit7 held (keyboard object +0x100..+0x1FF); mouse_down[b]: held; mouse_prev[b]: held last frame
    void apply(uint8_t edges[256], uint8_t state[256], const bool mouse_down[8], const bool mouse_prev[8]) const;
};

// ------------------------------------------------------------------------------------------------ bullet-time meter
struct MeterCfg {
    float capacity_s   = 8.0f;    // seconds of real time a full meter lasts
    float min_start    = 0.10f;   // fraction needed to start
    float regen_per_s  = 0.02f;   // passive refill per real second (fraction), after regen_delay_s without use
    float regen_delay_s = 2.0f;
    float per_kill     = 0.12f;   // fraction per enemy kill
    float per_point    = 0.0005f; // fraction per combo action point
};
struct Meter {
    float value = 1.0f;           // 0..1
    float since_use = 1e9f;
    void drain(const MeterCfg& c, float dt);                // while bullet time runs
    void idle(const MeterCfg& c, float dt);                 // while it does not
    void kills(const MeterCfg& c, int n);
    void points(const MeterCfg& c, int p);
    bool can_start(const MeterCfg& c) const { return value >= c.min_start && value > 0.0f; }
    bool empty() const { return value <= 0.0f; }
};

// ------------------------------------------------------------------------------------------------ bullet-time controller
// States: OFF -> RAMP_IN (trigger_slowmotion sent, WorldTime still 1) -> ACTIVE (WorldTime latched to the scale, so every
// "back to normal" of the game returns to bullet time) -> OFF (WorldTime 1 + trigger_slowmotion back to it).
enum BtState { BT_OFF = 0, BT_RAMP_IN = 1, BT_ACTIVE = 2 };
struct BtCfg {
    bool  enabled = false;
    bool  hold_mode = false;      // false: press toggles; true: active while held
    float scale = 0.30f;          // world time scale in bullet time
    float ramp_in = 0.25f;        // s (real) for the ramp into bullet time
    float ramp_out = 0.25f;       // s for the ramp back
    bool  blur = true;            // the game's slow-motion motion blur
};
struct BtGame {                   // snapshot of the game the decision needs (read by the glue)
    bool  ok = false;             // all pointers valid, player in control (an input tick is running)
    bool  exclusive = false;      // TIME_CTRL ExclusiveMode (Tornado, power-up menu)
    bool  cutscene = false;
    float world_time = 1.0f;      // cache +0x850
    float desired = 1.0f;         // cache +0x854
    float time_to_use = 0.0f;     // cache +0x85C
    float time_mult = 1.0f;       // Scene +0x114
};
struct BtActions {                // what the glue must do, in this order
    bool  set_world_time = false; float world_time = 1.0f;
    bool  send_slowmo = false;    float slowmo_target = 1.0f; float slowmo_seconds = 0.1f; int slowmo_no_blur = 0;
    bool  send_force_normal = false;
};
struct BtLogic {
    BtState state = BT_OFF;
    float   t = 0.0f;             // time in the current state (real s)
    int     ended_by_game = 0;    // counter for logs/shared memory
    bool    retried = false;      // RAMP_IN: the trigger was sent a second time
    // input tick (script context): want = toggled/held request; returns the actions to perform
    BtActions tick(const BtCfg& c, const MeterCfg& mc, Meter& m, const BtGame& g, bool want, float dt);
    // main-loop check (no script context): abort when a cut-scene starts or the input ticks stopped (not paused)
    BtActions abort_check(const BtGame& g, bool cutscene, bool stale, bool paused);
};

// ------------------------------------------------------------------------------------------------ mouse
// Frame-rate independent yaw: every frame's turn rate (deg/s) is turned into degrees with a fixed per-frame factor and
// accumulated; the next physics integrate turns by exactly the accumulated angle (turn_speed = acc / TimeStep).
struct YawAcc {
    float acc = 0.0f;
    uint32_t last_consume_ms = 0;
    bool mouse_mode = false;
    float seconds_per_frame = 0.95f / 60.0f;   // vanilla average at 60 fps
    void on_write(float turn_deg_per_s, uint32_t now_ms, uint32_t drop_after_ms);
    void on_pad();
    float on_consume(float timestep, uint32_t now_ms);   // returns the turn rate the integrate should use
};
uint32_t yaw_drop_ms(float time_mult);   // how long an unconsumed accumulation may wait (longer in slow motion)

// Vertical: invert and scale; optional normalisation of a per-frame count to a 60 fps-equivalent rate (experimental).
float mouse_y_factor(bool invert, float scale, bool fps_normalize, float real_dt);

// ------------------------------------------------------------------------------------------------ clocks
// Fractional carry for the integer-ms clocks: the game credits trunc(dt*1000) every frame; this credits
// floor(total) - floor(previous total) so no fraction is lost (identical at integer-ms deltas).
struct MsCarry {
    double frac = 0.0;
    int32_t credit(double v);
};

// ------------------------------------------------------------------------------------------------ frame limiter
struct Limiter {
    double next = 0.0;       // deadline (s) of the next frame start
    // returns how long to wait (s) for a frame starting at now; updates the deadline (no catch-up bursts)
    double wait_for(double now, double period);
};

// ------------------------------------------------------------------------------------------------ misc
void  diag_clamp(float* v3);                    // clamp (x, _, z) to length 1
float fov_remap(float fov, float target);       // keep tan ratio to 64: 64 -> target
bool  restart_allowed(int mission_index);       // story missions with a result screen and sub-missions
float steer_return(float wheel_angle, float rate); // what SteerReturnStub computes: toward 0 by rate, never past it

// ------------------------------------------------------------------------------------------------ md5
void md5(const void* data, size_t n, uint8_t out[16]);
void md5_hex(const uint8_t d[16], char out[33]);
struct Md5 {                                     // streaming
    uint32_t s[4]; uint64_t len; uint8_t buf[64]; size_t blen;
    void init(); void update(const void* p, size_t n); void final(uint8_t out[16]);
};

} // namespace gp
