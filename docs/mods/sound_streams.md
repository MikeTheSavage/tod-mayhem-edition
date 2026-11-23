# Streamed sound quotas (`[fixes] stream_limits`)

## What it does

Fixes the console-era limits on streamed sounds that can leave music, ambience or speech silent for good ("sometimes the
music is missing unless you load another level"; the same happens in Chili Con Carnage, so it is the engine, not a
Mayhem module). Every streamed sound (music, ambient loops, speech, the power-move voices) is a
`StreamedSoundBuffer`. The engine refuses to create one when a quota is hit, and the sound that asked simply stays silent:
nothing ever asks again, and every stream is only freed when a level unloads. The fix stops counting streams that are stopped; streams that play keep the vanilla caps (1 stereo, 5 mono, 2 aux), so nothing overlaps. On by default, installed with any other feature.

## The quotas (vanilla, GOG `TOD.exe`)

| Refusal | Rule | Log line of the game (`tod_test_log.txt`) |
|---|---|---|
| stereo | a stereo stream (every music cue) is refused while ANY other stereo stream or any aux mono stream exists | "Cannot allocate stereo stream ('...'), since there is already a stereo or one or more aux mono streams allocated." |
| mono | a 6th normal mono stream (ambient loops, speech) | "Cannot allocate normal mono stream ('...'), since 5 normal mono streams are already allocated" |
| aux | a 3rd aux mono stream, or one while a stereo stream exists | "Cannot allocate aux mono stream ('...') ..." |
| pool | an 8th stream of any kind (7 pool entries) | " ++ StreamedSound: This stream allocation has been TERMINATED. (Sorry)" |
| same file | a second stream on a SoundFile whose stream data another stream uses | none (silent) |
| movie | a fullscreen movie is allocated (`[0xA35ED4]`) and another stream exists | "Cannot allocate another Streamed Sound ... MoviePlayer ..." |

Details and addresses: `dll/HOOKS.md` "Streamed-sound quotas". The line "Play called on (...), but has not yet
finished pre-loading." in the engine log is harmless: the play request is kept and starts when the preload thread is done.
The movie rule stays (a fullscreen movie wants the sound device to itself); the "same file" rule stays (two streams cannot
share one file reader).

## How it is implemented

`dll/src/gp_music.cpp` (+ `gp_music_logic.cpp`, unit tests `tests/dll/test_music.cpp`, host test 12c):

- MU_REFUSE (0x445AE2): the constructor builds a refusal message and tests its length; the stub recomputes the four rules
  from the manager's counters (`*(0xA35EC0)` +38h stereo, +3Ch normal mono, +40h aux mono, +70h list size) and, when the engine
  refused only because of streams that no longer play (stopped, not playing, no play request pending: recounted from the manager's list), clears the length: the stream is created.
- MU_POOL (0x445EDE): the pool of 7 tokens grows (up to `stream_pool`) instead of "TERMINATED".
- MU_WARN (0x43E717): the per-frame "XBOX WARNING: More than 7 StreamedSoundBuffers" log line is silenced.

## Ini keys (`[fixes]`, `[debug]`)

| Key | Default | Meaning |
|---|---|---|
| `[fixes] stream_limits` | 1 | the fix (0 = vanilla) |
| `stream_pool` | 16 | streams alive at once (vanilla 7) |
| `[debug] music_trace` | 0 | test copy / bug reports: `TODGameplay.log` gets a line for every change of the stream picture (every stream, its channels, state flags, the quota counts, the pool) and one for every stream the engine refuses (with the rule) |
| `[debug] music_stress` | 0 | test copy only: after the key script's `mark mu_stress`, N short non-looped sounds are played one after the other through the game's global stream, each checked for a lost start and a missed end |

`mu_overlap` requests the same cue while the music really plays: refused as in vanilla, nothing overlaps.

Test-copy key script marks (`dll/src/gp_debug.cpp` -> `music_debug_mark`): `mu_quota [tag]` reproduces the quota refusal on
demand with the game's own functions (see below), `mu_hog IDX [aux]` / `mu_free IDX` allocate / destroy the stream of SoundDict
slot IDX (a stream held on purpose), `mu_stress`.

Installer: `stream_limits` is switched on with the other Essentials fixes; `stream_pool` is for testers. `music_trace` is an
ini-only key for bug reports: if you report missing music, switch it on and send `TODGameplay.log` and the game's own log.

## Reproduction (`mu_quota`)

(1) The cue that played ends: the global stream (`*(0xA3DD8C)`) is destroyed, as the next `AllocateGlobalStreamedSound`
0x89CF90 does first. (2) A SoundSlot (SoundDict 110) allocates its stream with the aux flag, as the script command
`allocate_stream(entity, true)` does (`SoundSlot::AllocateStream` 0x89D450). (3) The next music cue (a stereo ogg, looped)
asks for the global stream: vanilla refuses it ("Cannot allocate stereo stream ... aux mono ...") and the cue stays silent;
with `stream_limits` it is created and plays. This was checked on a copy of the game with an in-game test harness that
records the game's own sound (not included).

## Testing and limits

The refusal is reproduced on demand and fixed. No
natural trigger (a real mission hitting a quota) was caught in the runs made, so which script combination players meet is
inferred (a stereo music cue meeting an aux mono stream, five normal mono streams, an earlier stream that was never freed).
