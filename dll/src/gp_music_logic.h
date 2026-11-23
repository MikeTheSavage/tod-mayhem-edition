// gp_music_logic.h - pure logic of the streamed-sound fix of TODGameplay.dll ([fixes] stream_limits, dll/src/gp_music.cpp).
// No Windows, no game memory; tests in tests/dll/test_music.cpp.
//
// The engine's streamed-sound class (StreamedSoundBuffer, ctor 0x445770) refuses to create a stream when one of these
// console-era quotas is hit, and the SoundSlot that asked just stays silent (a looped music cue never starts again until
// a level load frees every slot's stream):
//   movie   a fullscreen movie plays and another stream exists                        (movie flag 0xA35ED4, list size)
//   stereo  the new stream has 2+ channels and a stereo or an "aux mono" stream exists    (counts +38h / +40h)
//   mono    the new stream is a normal mono one and 5 normal mono streams exist           (count +3Ch)
//   aux     the new stream is an aux mono one and 2 aux monos or any stereo stream exist  (counts +40h / +38h)
// plus a pool of 7 token entries (0xA09314): the 8th stream finds none and its creation is "TERMINATED".
// The fix relaxes the last four to caps that the PC mixer (DieselPower / DirectSound software mixing) does not need.
#pragma once

namespace music {

struct Counts { int total = 0, stereo = 0, mono = 0, aux = 0; };    // list size, +38h, +3Ch, +40h of the manager
struct Request { int channels = 1; bool aux = false; bool movie = false; };   // the stream being created
struct Caps {
    int pool = 16;      // pool entries = streams alive (vanilla 7); the only raised number: it only stops the 8th-stream refusal
};

enum Reason { OK = 0, MOVIE = 1, STEREO = 2, MONO = 3, AUX = 4 };

// the engine's own rule (vanilla): which quota refuses this stream (OK = none), for the given counts
Reason vanilla_refusal(const Counts& c, const Request& r);

// A stream that is stopped (flag 10h) and neither playing (80h) nor asked to play (4h) holds no place any more: the engine keeps
// it allocated (its slot still points at it) and counts it, which is what silences the next cue. Everything else counts.
bool stream_active(unsigned flags);
struct Live { int channels; bool aux; unsigned flags; };
// the quota counts of the streams that are really there: stopped ones left out; total = the manager's own list size
Counts count_active(const Live* s, int n, int total);
// the fix's rule: the vanilla rule on the active counts (the vanilla caps exactly: 1 stereo, 5 mono, 2 aux, aux next to stereo,
// the movie rule on the manager's own total), so playing music is never overlapped. OK when the engine's refusal came only
// from streams that no longer play.
Reason active_refusal(const Counts& active, const Request& r);
const char* reason_text(Reason r);

// the pool-growth decision of the ctor's "no free entry" path: entries = vector size (0xA09318)
inline bool pool_may_grow(int entries, int cap) { return cap > entries; }

// keeps the pool cap in the range the code patches can hold
Caps clamp_caps(Caps c);

// ---- the trace (what the debug thread logs): one line per change of the stream picture
struct StreamRow { unsigned ptr = 0; int channels = 0; unsigned flags = 0; int ready = 0; bool aux = false; unsigned state = 0, t0 = 0; char name[96] = {0}; };   // state = fill state (+7Ch), t0 = the EOF chunk's time stamp (+5Ch)
struct Snapshot {
    int renderer = 0; Counts counts; int pool_entries = 0, pool_in_use = 0;
    int n = 0; StreamRow row[24];
};
// true when b differs from a in anything worth a log line (stream set, counts, a playing / requested change)
bool snapshot_changed(const Snapshot& a, const Snapshot& b);
// one line (no newline) into out
int snapshot_text(const Snapshot& s, char* out, int cap);

}   // namespace music
