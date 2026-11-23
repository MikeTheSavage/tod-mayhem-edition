// gp_music_logic.cpp - see gp_music_logic.h.
#include "gp_music_logic.h"
#include <stdio.h>
#include <string.h>

namespace music {

Reason vanilla_refusal(const Counts& c, const Request& r) {
    // 0x445954: [0xA35ED4] (fullscreen movie allocated) and the manager list has 1+ streams
    if (r.movie && c.total >= 1) return MOVIE;
    // 0x4459A1: channels > 1 and (stereo count > 0 or aux count > 0)
    if (r.channels > 1 && (c.stereo > 0 || c.aux > 0)) return STEREO;
    // 0x445A1F: exactly mono, not aux, 5+ normal mono
    if (r.channels == 1 && !r.aux && c.mono >= 5) return MONO;
    // 0x445A78: exactly mono, aux, (2+ aux or any stereo)
    if (r.channels == 1 && r.aux && (c.aux >= 2 || c.stereo > 0)) return AUX;
    return OK;
}

bool stream_active(unsigned flags) {
    if (flags & (0x80u | 0x4u)) return true;      // playing, or a play request is pending
    return !(flags & 0x10u);                       // not stopped: fresh / pre-allocated, about to be played
}

Counts count_active(const Live* s, int n, int total) {
    Counts c; c.total = total;
    for (int i = 0; i < n; ++i) {
        if (!stream_active(s[i].flags)) continue;
        if (s[i].channels > 1) ++c.stereo;
        else if (s[i].channels == 1) { if (s[i].aux) ++c.aux; else ++c.mono; }
    }
    return c;
}

Reason active_refusal(const Counts& active, const Request& r) { return vanilla_refusal(active, r); }

const char* reason_text(Reason r) {
    switch (r) {
        case MOVIE: return "fullscreen movie";
        case STEREO: return "stereo quota";
        case MONO: return "mono quota";
        case AUX: return "aux mono quota";
        default: return "none";
    }
}

Caps clamp_caps(Caps c) {
    c.pool = c.pool < 7 ? 7 : (c.pool > 100 ? 100 : c.pool);   // the manager warns above 7 (patched to 127), the manager caps nothing
    return c;
}

bool snapshot_changed(const Snapshot& a, const Snapshot& b) {
    if (a.n != b.n || a.pool_entries != b.pool_entries || a.pool_in_use != b.pool_in_use) return true;
    if (a.counts.total != b.counts.total || a.counts.stereo != b.counts.stereo || a.counts.mono != b.counts.mono ||
        a.counts.aux != b.counts.aux) return true;
    for (int i = 0; i < a.n; ++i) {
        const StreamRow& x = a.row[i]; const StreamRow& y = b.row[i];
        if (x.ptr != y.ptr || x.ready != y.ready) return true;
        // the flags that matter: play request (4), stop request (8), stopped (0x10), pre-loaded (0x20), playing (0x80)
        const unsigned M = 0x4 | 0x8 | 0x10 | 0x20 | 0x80;
        if ((x.flags & M) != (y.flags & M)) return true;
    }
    return false;
}

int snapshot_text(const Snapshot& s, char* out, int cap) {
    int n = _snprintf_s(out, cap, _TRUNCATE, "streams %d (stereo %d, mono %d, aux %d) pool %d/%d in use, renderer %d:",
                        s.counts.total, s.counts.stereo, s.counts.mono, s.counts.aux, s.pool_in_use, s.pool_entries, s.renderer);
    if (n < 0) return (int)strlen(out);
    for (int i = 0; i < s.n && n < cap - 8; ++i) {
        const StreamRow& r = s.row[i];
        int m = _snprintf_s(out + n, cap - n, _TRUNCATE, " [%08X ch%d%s %s%s%s%s%s fl=%08X st=%u t0=%u \"%s\"]", r.ptr, r.channels, r.aux ? " aux" : "",
                            (r.flags & 0x80) ? "PLAYING" : ((r.flags & 4) ? "REQUESTED" : "idle"),
                            (r.flags & 0x20) ? " loaded" : " unloaded", (r.flags & 0x10) ? " stopped" : "",
                            (r.flags & 8) ? " stop-req" : "", r.ready ? "" : " busy", r.flags, r.state, r.t0, r.name);
        if (m < 0) break;
        n += m;
    }
    return n;
}

}   // namespace music
