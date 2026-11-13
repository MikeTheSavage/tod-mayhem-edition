// todgameplay_shared.h - the shared-memory block TODGameplay.dll publishes once per frame, so another process (the
// renderer bridge's Unreal side) or another DLL in the game process can draw the bullet-time meter.
//
// Name:   "Local\\TODGameplay.Shared.v1" (CreateFileMappingA on the page file, read with OpenFileMappingA +
//         MapViewOfFile(FILE_MAP_READ)). Size: sizeof(TodgpShared) = 128 bytes in version 2 (64 in version 1; the
//         mapping is 4096). The name keeps "v1": version 2 only appends fields after the first 64 bytes, so a version-1
//         reader that checks `size >= 64` keeps working; read the Suave fields only when version >= 2.
// Layout: little-endian, 4-byte fields, packed, no pointers. Version 2. Fields are only ever appended.
// Consistency: `seq` is odd while the writer updates the block and even when it is complete (seqlock). A reader copies
//         the block, then re-reads seq: if seq is odd or changed, it copies again.
// Writer: the game's main thread, once per game-loop pass (after the input poll), also while paused (values freeze).
#pragma once
#include <stdint.h>
#include <stddef.h>

#define TODGP_SHARED_NAME    "Local\\TODGameplay.Shared.v1"
#define TODGP_SHARED_MAGIC   0x53504754u   /* 'TGPS' */
#define TODGP_SHARED_VERSION 2u

// flags
#define TODGP_F_HOOKS_OK      0x0001u  /* the exe matched and the enabled features were installed */
#define TODGP_F_REFUSED       0x0002u  /* the exe did not match: nothing was patched */
#define TODGP_F_BT_ENABLED    0x0004u  /* bullet time is switched on in the ini */
#define TODGP_F_BT_RUNNING    0x0008u  /* bullet time is ramping in or active */
#define TODGP_F_CUTSCENE      0x0010u  /* the engine's cut-scene flags are set */
#define TODGP_F_PAUSED        0x0020u  /* Scene PlayMode 2 */
#define TODGP_F_IN_CONTROL    0x0040u  /* the player input procedure ran during the last 0.25 s (on foot, in play) */
#define TODGP_F_DIVE_SLOWMO_OFF 0x0080u /* automatic dive slow motion is switched off */
#define TODGP_F_SUAVE_ENABLED 0x0100u  /* the Suave move (power-up slot 11) is installed */
#define TODGP_F_SUAVE_RUNNING 0x0200u  /* a Suave move is running (suave_phase != 0) */

// suave_phase (version 2): 0 off, 1 freeze, 2 intro, 3 gather, 4 dance, 5 plant, 6 dive, 7 after (the explosion happened)
// suave_flags bits
#define TODGP_SV_ARMOR        0x01u    /* Ram has armor (the game's god mode, as El Mariachi); no visual is implied */
#define TODGP_SV_MUSIC        0x02u    /* a Suave music segment was started */
#define TODGP_SV_DYNAMITE     0x04u    /* the dynamite shell is attached to the partner */
#define TODGP_SV_BOOM         0x08u    /* the explosion happened (phase 6) */
#define TODGP_SV_INPUT_LOCKED 0x10u    /* the player's keys are held off (dance) */

#pragma pack(push, 4)
typedef struct TodgpShared {
    uint32_t magic;          // TODGP_SHARED_MAGIC
    uint16_t version;        // TODGP_SHARED_VERSION
    uint16_t size;           // sizeof(TodgpShared)
    uint32_t seq;            // seqlock counter (odd = being written)
    uint32_t frame;          // game-loop passes since the DLL loaded
    uint32_t flags;          // TODGP_F_*
    float    meter;          // bullet-time meter 0..1 (draw this)
    float    meter_seconds;  // real seconds of bullet time left at the current fill
    uint32_t bt_state;       // 0 off, 1 ramping in, 2 active
    float    bt_scale;       // configured bullet-time world scale
    float    time_mult;      // Scene.TimeMultiplier now (1 = normal speed)
    float    world_time;     // TIME_CTRL WorldTime (the base the game returns to)
    uint32_t kills;          // enemy kills seen since the DLL loaded (meter refills)
    uint32_t bt_uses;        // times bullet time was started
    uint32_t bt_game_ends;   // times the game ended it (death, cut-scene, vehicle, ...)
    uint32_t reserved[2];
    // ---- version 2: the Suave move (for a ring effect / golden shield drawn by the bridge or Unreal)
    uint32_t suave_phase;    // see above; 0 = not running
    float    suave_clock;    // game seconds since the music start (the move's clock); -1 during the freeze
    float    suave_phase_t;  // seconds in the current phase (real in the freeze, game afterwards)
    float    suave_phase_len;// planned length of the current phase (s)
    float    suave_dive_at;  // suave_clock value at which Ram is released for the dive (s); the explosion follows in the air
    float    suave_beat;     // suave_clock in beats of the song (124.17 BPM by default)
    float    suave_centre[3];// ring centre (Ram's position at the start), world, game units (1 = 1 m)
    float    suave_ring_radius;
    uint32_t suave_members;  // ring members taken (0..8)
    uint32_t suave_flags;    // TODGP_SV_*
    uint32_t suave_uses;     // moves started since the DLL loaded
    uint32_t suave_aborts;   // moves that ended early (partner died, cut-scene, vehicle, ...)
    uint32_t suave_shot;     // camera shot (sv::Shot: 1 push-in ... 12 front dive), 0 = gameplay camera
    uint32_t reserved2;
} TodgpShared;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(TodgpShared) == 128, "TodgpShared is 128 bytes in version 2 (64 in version 1)");
static_assert(offsetof(TodgpShared, suave_phase) == 64, "version-2 fields start after the version-1 block");
#endif
