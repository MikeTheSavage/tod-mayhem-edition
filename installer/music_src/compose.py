"""La Llorona (traditional), arranged for Gameplay+ -- MIDI generator.

Writes one Standard MIDI File per stem into ./midi/ (next to this script):
  stem1_guitar.mid, stem2_bass_perc.mid, stem3_trumpet.mid, stem4_strings_choir.mid
plus loop_once/*.mid (one pass of the loop, for reading in a sequencer).

The loop is 48 bars of 3/4 at 80 BPM in E minor (108.0 s). The render files hold
the same loop body N_LOOPS times in a row; render.py keeps the middle pass so
every reverb tail and ringing note crosses the seam exactly as it would in a
continuous performance. The humanisation is drawn once from a fixed seed, so
every pass is identical and the render is periodic.

Python standard library only.  Usage: python -I compose.py
"""
import math
import os
import random
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
PPQ = 960
BPM = 80
SEC_PER_BEAT = 60.0 / BPM
BAR = 3                      # beats per bar (3/4)
BARS = 48
LOOP_BEATS = BARS * BAR      # 144 beats = 108 s
N_LOOPS = 3
SEED = 1504

NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']


def nn(name):
    """'F#4' -> MIDI number (C4 = 60)."""
    pc, octv = name[:-1], int(name[-1])
    return NOTE_NAMES.index(pc) + 12 * (octv + 1)


def name_of(n):
    return f'{NOTE_NAMES[n % 12]}{n // 12 - 1}'


def bar(b, beat=0.0):
    """1-based bar number + beat offset (0..3) -> absolute beat in the loop."""
    return (b - 1) * BAR + beat


def ms(x):
    return x / 1000.0 / SEC_PER_BEAT    # milliseconds -> beats


# --------------------------------------------------------------------------- harmony
# One chord per bar (1-based).
#  1-8   intro: Em pedal (ghost tremolo), then Am - F - B7 - B7 (the bII colour)
#  9-24  couplet 1 twice ("Salias del templo un dia, Llorona / cuando al pasar yo te vi")
# 25-40  couplet 2 twice ("Hermoso huipil llevabas, Llorona / que la Virgen te crei")
# 41-48  outro: Em C Am F Em F B7 B7 -> B7 resolves across the seam onto bar 1 Em
C1 = ['Em', 'Em', 'Em', 'Em', 'B7', 'B7', 'Em', 'Em']
C2 = ['Em', 'Em', 'E7', 'Am', 'Am', 'B7', 'Em', 'Em']
CHORDS = (['Em', 'Em', 'Em', 'Em', 'Am', 'F', 'B7', 'B7'] + C1 + C1 + C2 + C2 +
          ['Em', 'C', 'Am', 'F', 'Em', 'F', 'B7', 'B7'])
assert len(CHORDS) == BARS

# guitar voicings (sounding pitch): bass, alternate bass, three upper notes
GUIT = {
    'Em': (40, 47, [55, 59, 64]),   # E2 B2 | G3 B3 E4
    'B7': (47, 42, [57, 59, 63]),   # B2 F#2 | A3 B3 D#4
    'Am': (45, 40, [57, 60, 64]),   # A2 E2 | A3 C4 E4
    'C':  (48, 43, [55, 60, 64]),   # C3 G2 | G3 C4 E4
    'F':  (41, 48, [53, 57, 60]),   # F2 C3 | F3 A3 C4
    'E7': (40, 47, [56, 62, 64]),   # E2 B2 | G#3 D4 E4
}
# guitarron roots (low, high) and fifth
# (the real guitarron's lowest string is A1, so nothing goes below A1 except B7's B1)
GTRN = {'Em': (40, 52, 35), 'B7': (35, 47, 42), 'Am': (33, 45, 40), 'C': (36, 48, 43),
        'F': (41, 53, 36), 'E7': (40, 52, 35)}
# timpani pitch per chord
TIMP = {'Em': 40, 'B7': 42, 'Am': 45, 'C': 43, 'F': 41, 'E7': 40}
# low strings (dark, close) and upper string notes
STR_LO = {'Em': [40, 47, 55], 'B7': [35, 47, 51, 57], 'Am': [33, 45, 52, 60],
          'C': [36, 48, 52, 55], 'F': [41, 48, 53, 57], 'E7': [40, 47, 50, 56]}
STR_HI = {'Em': [59, 64], 'B7': [63, 66], 'Am': [60, 64], 'C': [60, 64], 'F': [60, 65],
          'E7': [62, 68]}
CHOIR = {'Em': [52, 59], 'B7': [51, 57], 'Am': [52, 57, 60], 'C': [52, 55, 60],
         'F': [53, 57, 60], 'E7': [50, 56, 59]}

# --------------------------------------------------------------------------- melody
# Traditional tune, E minor, (note, beat-in-bar, length-in-beats) per song bar.
# Song bar 0 = the pickup bar (only its beat-3 pickup is used).
MEL_C1 = {
    0: [('B3', 2, 1)],
    1: [('E4', 0, 1), ('E4', 1, 1), ('F#4', 2, 1)],     # sa-LI-as del
    2: [('G4', 0, 2), ('F#4', 2, 1)],                   # TEM-plo un
    3: [('F#4', 0, 1), ('E4', 1, 1), ('F#4', 2, 1)],    # DI-a, Llo-
    4: [('G4', 0, 2), ('F#4', 2, 1)],                   # RO-na, cuan-
    5: [('F#4', 0, 1), ('F#4', 1, 1), ('G4', 2, 1)],    # -do al pa-
    6: [('A4', 0, 1), ('G4', 1, 1), ('F#4', 2, 1)],     # SAR yo te
    7: [('E4', 0, 3)],                                  # VI
}
MEL_C1_B = dict(MEL_C1)
MEL_C1_B[6] = [('A4', 0, 1), ('F#4', 1, 1), ('D#4', 2, 1)]   # 2nd pass: leading tone into "vi"
MEL_C2 = {
    0: [('B3', 2, 1)],
    1: [('E4', 0, 1), ('E4', 1, 1), ('F#4', 2, 1)],     # her-MO-so hui-
    2: [('G4', 0, 2), ('A4', 2, 1)],                    # PIL lle-
    3: [('B4', 0, 1), ('A4', 1, 1), ('G#4', 2, 1)],     # VA-bas, Llo-   (E7)
    4: [('A4', 0, 2), ('A4', 2, 1)],                    # RO-na          (Am)
    5: [('A4', 0, 1), ('A4', 1, 1), ('G4', 2, 1)],      # que la Vir-
    6: [('F#4', 0, 1), ('E4', 1, 1), ('D#4', 2, 1)],    # -gen te cre-   (B7)
    7: [('E4', 0, 3)],                                  # I
}
MEL_C2_B = dict(MEL_C2)
MEL_C2_B[5] = [('C5', 0, 1), ('B4', 1, 1), ('A4', 2, 1)]     # 2nd pass climbs a third higher
# trumpet outro lament (own line, not from any recording)
MEL_OUTRO = [
    (43, [('C5', 0, 2), ('B4', 2, 1)]),
    (44, [('A4', 0, 3)]),
    (45, [('G4', 0, 1), ('F#4', 1, 1), ('E4', 2, 1)]),
    (46, [('F4', 0, 3)]),                               # bII over F: the threat
    (47, [('D#4', 0, 2), ('F#4', 2, 1)]),
    (48, [('B3', 0, 3)]),                               # falls away into the seam
]


def place(mel, first_bar):
    """song-bar dict -> list of (abs_beat, midi, length).  Song bar 1 = first_bar."""
    out = []
    for sb in sorted(mel):
        for note, beat, length in mel[sb]:
            out.append((bar(first_bar + sb - 1, beat), nn(note), length))
    return out


# --------------------------------------------------------------------------- event store
class Song:
    def __init__(self):
        self.ev = []          # (beat, order, bytes)
        self.setup = []       # (order, bytes) at tick 0 of the file only

    def raw(self, beat, data, order=1):
        self.ev.append((beat, order, bytes(data)))

    def cc(self, ch, num, val, beat):
        self.raw(beat, [0xB0 | ch, num, max(0, min(127, int(round(val))))], 1)

    def bend(self, ch, val, beat):
        v = max(0, min(16383, int(round(8192 + val))))
        self.raw(beat, [0xE0 | ch, v & 0x7F, v >> 7], 1)

    def note(self, ch, pitch, beat, length, vel):
        vel = max(1, min(127, int(round(vel))))
        self.raw(beat, [0x90 | ch, pitch, vel], 2)
        self.raw(beat + max(length, 0.02), [0x80 | ch, pitch, 0], 0)

    def ramp(self, ch, num, b0, b1, v0, v1, step=0.125, shape='lin'):
        n = max(1, int(round((b1 - b0) / step)))
        last = None
        for i in range(n + 1):
            t = i / n
            if shape == 'sin':
                t = 0.5 - 0.5 * math.cos(math.pi * t)
            v = int(round(v0 + (v1 - v0) * t))
            if v != last:
                self.cc(ch, num, v, b0 + (b1 - b0) * i / n)
                last = v

    def program(self, ch, bank, prog, vol, pan, rev, cho=0):
        s = self.setup
        s += [(1, bytes([0xB0 | ch, 0, bank])), (1, bytes([0xB0 | ch, 32, 0])),
              (2, bytes([0xC0 | ch, prog])), (3, bytes([0xB0 | ch, 7, vol])),
              (3, bytes([0xB0 | ch, 10, pan])), (3, bytes([0xB0 | ch, 91, rev])),
              (3, bytes([0xB0 | ch, 93, cho])), (3, bytes([0xB0 | ch, 11, 127]))]

    def bend_range(self, ch, semis):
        self.setup += [(4, bytes([0xB0 | ch, 101, 0])), (4, bytes([0xB0 | ch, 100, 0])),
                       (4, bytes([0xB0 | ch, 6, semis])), (4, bytes([0xB0 | ch, 38, 0])),
                       (4, bytes([0xB0 | ch, 101, 127])), (4, bytes([0xB0 | ch, 100, 127]))]


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def fix_overlaps(events):
    """Same channel + pitch: a note-off must not land after the next note-on."""
    ons = {}
    for t, o, d in events:
        if d[0] & 0xF0 == 0x90:
            ons.setdefault((d[0] & 0x0F, d[1]), []).append(t)
    for k in ons:
        ons[k].sort()
    fixed = []
    for t, o, d in events:
        if d[0] & 0xF0 == 0x80:
            key = (d[0] & 0x0F, d[1])
            lst = ons.get(key, [])
            # earliest note-on strictly after the note this off belongs to
            nxt = [x for x in lst if x < t]
            if nxt:
                start = nxt[-1]
                later = [x for x in lst if x > start]
                if later and t >= later[0]:
                    t = later[0] - 1
                    if t <= start:
                        t = start + 1
        fixed.append((t, o, d))
    return fixed


def write_mid(path, song, loops, title):
    ticks = []
    for o, d in song.setup:
        ticks.append((0, o - 10, d))
    loop_ticks = LOOP_BEATS * PPQ
    for k in range(loops):
        for beat, o, d in song.ev:
            t = int(round(beat * PPQ)) + k * loop_ticks
            ticks.append((max(t, 2), o, d))      # setup first, events from tick 2
    ticks = fix_overlaps(ticks)
    ticks.sort(key=lambda e: (e[0], e[1]))
    end = loops * loop_ticks + 4 * PPQ
    trk = bytearray()
    name = title.encode('ascii')
    trk += vlq(0) + b'\xFF\x03' + vlq(len(name)) + name
    trk += vlq(0) + b'\xFF\x51\x03' + struct.pack('>I', int(round(SEC_PER_BEAT * 1e6)))[1:]
    trk += vlq(0) + b'\xFF\x58\x04' + bytes([3, 2, 24, 8])
    trk += vlq(0) + b'\xFF\x59\x02' + bytes([1, 1])      # 1 sharp, minor = E minor
    last = 0
    for t, o, d in ticks:
        trk += vlq(t - last) + d
        last = t
    trk += vlq(max(0, end - last)) + b'\xFF\x2F\x00'
    with open(path, 'wb') as f:
        f.write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, PPQ))
        f.write(b'MTrk' + struct.pack('>I', len(trk)) + trk)


# --------------------------------------------------------------------------- humanising
R = random.Random(SEED)


def jit(sd_ms, lim_ms):
    return ms(max(-lim_ms, min(lim_ms, R.gauss(0, sd_ms))))


def hv(v, sd=6):
    return v + R.gauss(0, sd)


# --------------------------------------------------------------------------- stem 1: nylon guitar
def stem_guitar():
    s = Song()
    G = 0
    s.program(G, 0, 24, 112, 52, 52)

    def tremolo(start, pitch, length, chord, vel=46, slot=0.125):
        """p-a-m-i tremolo: thumb on chord tones, three repeats of the melody note."""
        bass, alt, up = GUIT[chord]
        pcycle = [bass, up[0], alt, up[1]]
        n = int(round(length / slot))
        for i in range(n):
            b = start + i * slot + jit(5, 12)
            if i % 4 == 0:
                p = pcycle[(int(round(start / slot)) // 4 + i // 4) % 4]
                s.note(G, p, b, slot * 4 - 0.02, hv(vel + 8 if i == 0 else vel - 2, 4))
            else:
                acc = [0, 4, -3, -6][i % 4]
                s.note(G, pitch, b, slot * 1.02, hv(vel + acc, 5))

    def arp(b0, chord, vel=58, pattern='A'):
        bass, alt, up = GUIT[chord]
        seq = {'A': [bass, up[0], up[1], up[2], up[1], up[0]],
               'B': [bass, up[0], up[2], up[1], alt, up[1]],
               'C': [bass, up[1], up[2], up[0], up[2], up[1]]}[pattern]
        for i, p in enumerate(seq):
            b = b0 + i * 0.5 + jit(8, 18)
            if i == 0 or (pattern == 'B' and i == 4):
                s.note(G, p, b, 2.9 if i == 0 else 0.95, hv(vel + 8, 5))
            else:
                s.note(G, p, b, 1.6, hv(vel - 10 + (4 if p == up[2] else 0), 5))

    def strum(b0, chord, vel=70, down=True, spread_ms=None, length=2.8):
        bass, alt, up = GUIT[chord]
        notes = [bass] + ([alt] if alt > bass else []) + up
        if not down:
            notes = list(reversed(notes[1:]))
        spread = ms(spread_ms if spread_ms else R.uniform(14, 30))
        for i, p in enumerate(notes):
            s.note(G, p, b0 + jit(4, 8) + i * spread / max(1, len(notes) - 1) * 1.0,
                   length, hv(vel - i * 2, 4))

    def fill(b0, notes, vel=54):
        for i, nm in enumerate(notes):
            s.note(G, nn(nm), b0 + i * 0.5 + jit(7, 15), 0.6, hv(vel - i, 5))

    # intro 1-4: ghost tremolo of the opening phrase over an E pedal, left unresolved
    intro = [(1, 'E4', 0, 2), (1, 'F#4', 2, 1), (2, 'G4', 0, 2), (2, 'F#4', 2, 1),
             (3, 'F#4', 0, 1), (3, 'E4', 1, 1), (3, 'F#4', 2, 1), (4, 'G4', 0, 2), (4, 'F#4', 2, 1)]
    for b, nm, bt, ln in intro:
        tremolo(bar(b, bt), nn(nm), ln, 'Em', vel=44 + (6 if b == 1 and bt == 0 else 0))
    s.note(G, 40, bar(1) + ms(3), 5.5, 80)                     # low E landing on the seam
    # 5-8: Am F B7 B7 arpeggios, slowing into the verse
    arp(bar(5), 'Am', 54, 'A')
    arp(bar(6), 'F', 54, 'C')
    arp(bar(7), 'B7', 56, 'B')
    s.note(G, 47, bar(8) + jit(5, 10), 2.0, 64)
    s.note(G, 57, bar(8, 0.5) + jit(5, 10), 1.5, 44)
    s.note(G, 63, bar(8, 1.0) + jit(5, 10), 1.0, 46)
    s.note(G, 42, bar(8, 2.0) + jit(5, 10), 1.0, 52)

    # couplet 1, pass 1 (9-16): arpeggios under the trumpet
    for i, ch in enumerate(C1):
        b = 9 + i
        if i == 7:
            fill(bar(b), ['G4', 'F#4', 'E4', 'D#4', 'E4'], 50)
            s.note(G, 40, bar(b) + jit(5, 10), 2.4, 60)
        else:
            arp(bar(b), ch, 52, 'A' if i % 2 == 0 else 'B')
    # couplet 1, pass 2 (17-24): the tune in tremolo an octave up (the guitar's own voice)
    for (b, p, ln) in place(MEL_C1_B, 17):
        if b < bar(17):
            continue
        tremolo(b, p + 12, ln, CHORDS[int(b // BAR)], vel=42)
    tremolo(bar(16, 2), nn('B4'), 1, 'Em', vel=40)                # pickup in tremolo
    s.note(G, 40, bar(23) + jit(5, 10), 3.0, 60)
    arp(bar(24), 'Em', 50, 'C')
    # couplet 2 (25-40)
    for i, ch in enumerate(C2 + C2):
        b = 25 + i
        second = i >= 8
        if i % 8 == 7:
            if second:
                s.note(G, 40, bar(b) + jit(5, 10), 2.8, 66)
                s.note(G, 47, bar(b, 1.5) + jit(5, 10), 1.4, 46)
            else:
                fill(bar(b), ['B4', 'A4', 'G4', 'F#4', 'E4'], 52)
                s.note(G, 40, bar(b) + jit(5, 10), 2.4, 60)
        elif second and i % 2 == 0:
            strum(bar(b), ch, 62)
            bass, alt, up = GUIT[ch]
            for k, p in enumerate([up[1], up[2], up[1], up[0]]):
                s.note(G, p, bar(b, 1.0 + 0.5 * k) + jit(8, 15), 1.2, hv(46, 4))
        else:
            arp(bar(b), ch, 54 if second else 52, 'B' if i % 2 else 'A')
    # outro (41-48)
    for b, nm, bt, ln in [(41, 'B3', 0, 1), (41, 'E4', 1, 1), (41, 'F#4', 2, 1),
                          (42, 'G4', 0, 2), (42, 'A4', 2, 1)]:
        tremolo(bar(b, bt), nn(nm), ln, CHORDS[b - 1], vel=48)
    strum(bar(43), 'Am', 64)
    arp(bar(43, 0), 'Am', 48, 'C')
    strum(bar(44), 'F', 62)
    for k, p in enumerate([53, 57, 60, 57]):
        s.note(G, p, bar(44, 1.0 + 0.5 * k) + jit(8, 15), 1.2, hv(44, 4))
    arp(bar(45), 'Em', 52, 'A')
    strum(bar(46), 'F', 66)
    strum(bar(46, 2.0), 'F', 50, down=False, spread_ms=18, length=0.9)
    strum(bar(47), 'B7', 72)
    strum(bar(47, 1.0), 'B7', 56, down=False, spread_ms=16, length=0.9)
    strum(bar(47, 1.5), 'B7', 60, spread_ms=14, length=0.9)
    strum(bar(47, 2.0), 'B7', 66, spread_ms=20, length=0.9)
    strum(bar(48), 'B7', 78, spread_ms=34, length=2.6)       # last strum rings into the seam
    s.note(G, 42, bar(48, 2.0) + jit(5, 8), 1.0, 50)
    return s


# --------------------------------------------------------------------------- stem 2: guitarron + low percussion
def stem_bass_perc():
    s = Song()
    B, T, D, O = 0, 1, 9, 2          # guitarron, timpani, std kit (ch 10), orchestral kit
    s.program(B, 0, 32, 104, 64, 30)
    s.program(T, 0, 47, 100, 62, 50)
    s.program(D, 0, 0, 92, 72, 40)          # channel 10: Standard kit (congas, maracas)
    s.program(O, 120, 48, 100, 60, 55)      # bank 120 = drum kits on a melodic channel
    LOW_CONGA, MUTE_HI, OPEN_HI, MARACAS, CBD = 64, 62, 63, 70, 36

    def gtrn(b0, chord, vel, fifth=True, length=2.0):
        lo, hi, fi = GTRN[chord]
        t = b0 + jit(6, 14)
        s.note(B, lo, t, length, hv(vel, 4))
        s.note(B, hi, t + ms(R.uniform(10, 22)), length, hv(vel - 12, 4))
        if fifth:
            s.note(B, fi, b0 + 2 + jit(6, 14), 0.9, hv(vel - 10, 4))

    def heart(b0, chord, vel):
        p = TIMP[chord]
        s.note(T, p, b0 + jit(4, 10), 1.0, hv(vel, 3))
        s.note(T, p, b0 + 0.42 + jit(4, 10), 1.2, hv(vel * 0.68, 3))

    def cajon(b0, level):
        pat = [(0.0, LOW_CONGA, 70)]
        if level >= 1:
            pat += [(2.0, MUTE_HI, 52)]
        if level >= 2:
            pat += [(1.5, MUTE_HI, 38), (2.5, MUTE_HI, 30)]
        if level >= 3:
            pat += [(1.0, LOW_CONGA, 46), (2.5, OPEN_HI, 40)]
        for off, p, v in pat:
            s.note(D, p, b0 + off + jit(6, 14), 0.4, hv(v - (0 if level >= 2 else 8), 5))

    def shaker(b0, vel):
        for i in range(6):
            s.note(D, MARACAS, b0 + i * 0.5 + jit(7, 15), 0.2,
                   hv(vel + (8 if i % 2 == 0 else -4) + (6 if i == 0 else 0), 4))

    # intro: the landing hit, then a slow heartbeat
    s.note(O, CBD, bar(1) + ms(2), 3.0, 84)
    s.note(T, 40, bar(1) + ms(4), 2.0, 104)
    gtrn(bar(1), 'Em', 82, fifth=False, length=2.8)
    for b in (3, 5, 7):
        heart(bar(b), CHORDS[b - 1], 52 if b < 7 else 60)
    for b in (2, 3, 4):
        gtrn(bar(b), 'Em', 56, fifth=False, length=2.6)
    gtrn(bar(5), 'Am', 64, fifth=False, length=2.6)
    gtrn(bar(6), 'F', 64, fifth=False, length=2.6)
    gtrn(bar(7), 'B7', 68, fifth=True)
    gtrn(bar(8), 'B7', 64, fifth=True)
    # couplets
    for b in range(9, 41):
        ch = CHORDS[b - 1]
        sec = (b - 9) // 8               # 0,1 couplet 1; 2,3 couplet 2
        gtrn(bar(b), ch, 66 + 4 * sec, fifth=True)
        heart(bar(b), ch, 50 + 6 * sec)
        cajon(bar(b), sec)
        if sec >= 1:
            shaker(bar(b), 24 + 6 * sec)
        if b in (9, 25, 33):
            s.note(O, CBD, bar(b) + ms(3), 3.0, 60 + 4 * sec)
        if sec == 3:
            s.note(T, TIMP[ch], bar(b, 2.0) + jit(4, 8), 0.8, hv(52, 3))
    # outro
    for b in range(41, 47):
        ch = CHORDS[b - 1]
        gtrn(bar(b), ch, 76, fifth=True)
        heart(bar(b), ch, 72)
        cajon(bar(b), 3)
        shaker(bar(b), 46)
    s.note(O, CBD, bar(41) + ms(3), 3.0, 72)
    s.note(O, CBD, bar(45) + ms(3), 3.0, 68)
    # 47-48: B7 dominant; timpani roll swells into the seam (resolves on bar 1)
    gtrn(bar(47), 'B7', 80, fifth=True)
    gtrn(bar(48), 'B7', 84, fifth=False, length=2.9)
    for b0, v in ((bar(47), 84), (bar(47, 1.0), 60), (bar(47, 2.0), 66)):
        s.note(D, LOW_CONGA, b0 + jit(5, 10), 0.4, hv(v, 3))
    n = int(6 / 0.125)
    for i in range(n - 1):
        t = bar(47) + i * 0.125
        v = 26 + 84 * (i / n) ** 1.6
        s.note(T, 42 if (i // 2) % 2 == 0 else 42, t + jit(3, 6), 0.2, hv(v + (6 if i % 2 == 0 else -6), 3))
    return s


# --------------------------------------------------------------------------- stem 3: trumpet
def stem_trumpet():
    s = Song()
    TR = 0
    s.program(TR, 0, 56, 104, 72, 70)
    s.bend_range(TR, 4)
    BR = 8192 / 4.0                 # bend units per semitone

    phrases = []                    # lists of (beat, midi, length)
    c1a = place(MEL_C1, 9)          # pickup lands in bar 8
    c1b = place(MEL_C1_B, 17)
    c2a = place(MEL_C2, 25)
    c2b = place(MEL_C2_B, 33)
    outro = []
    for b, notes in MEL_OUTRO:
        for nm, bt, ln in notes:
            outro.append((bar(b, bt), nn(nm), ln))
    phrases = [c1a, c1b, c2a, c2b, outro]
    falls = {id(c1b): 0.5, id(c2a): 0.0, id(c2b): 0.55, id(outro): 0.7}   # fall length (s)

    s.cc(TR, 11, 0, bar(1))
    s.bend(TR, 0, bar(1))
    for ph in phrases:
        intensity = {id(c1a): 0, id(c1b): 6, id(c2a): 8, id(c2b): 16, id(outro): 10}[id(ph)]
        # split each couplet at the long tonic into two breaths
        for idx, (b, p, ln) in enumerate(ph):
            nxt = ph[idx + 1][0] if idx + 1 < len(ph) else b + ln
            gap = nxt - (b + ln)
            start = b + ms(max(-8, min(28, R.gauss(10, 8))))     # laid back
            end = b + ln - (0.06 if gap < 0.01 else 0.0) + ms(R.uniform(-10, 10))
            length = end - start
            hi = (p - 59) / 12.0            # brighter (louder) as the line climbs
            vel = 64 + 18 * hi + intensity + R.gauss(0, 4)
            last_of_phrase = idx == len(ph) - 1 or ln >= 3
            s.note(TR, p, start, length, vel)
            # expression: soft attack swell, long notes breathe, phrase ends sink
            base = 92 + intensity * 0.8
            s.cc(TR, 11, base - 22, start)
            s.ramp(TR, 11, start, start + min(0.3, length * 0.4), base - 22, base, step=0.05,
                   shape='sin')
            if length >= 1.5:
                peak = min(127, base + 14)
                mid = start + length * 0.45
                s.ramp(TR, 11, start + 0.3, mid, base, peak, step=0.1, shape='sin')
                tail = base - 18 if last_of_phrase else base - 6
                s.ramp(TR, 11, mid, end - 0.05, peak, tail, step=0.1, shape='sin')
            # vibrato on long notes (pitch bend), plus a fall on phrase-final notes
            fall_s = falls.get(id(ph), 0.0) if (last_of_phrase and idx == len(ph) - 1) else 0.0
            if ln >= 2 or fall_s:
                vib_start = start + ms(320)
                t = vib_start
                step = ms(24)
                fall_b = ms(fall_s * 1000)
                fall_t0 = end - fall_b
                while t < end:
                    age = (t - vib_start) * SEC_PER_BEAT
                    depth = min(1.0, age / 0.7) * 16.0          # cents
                    v = math.sin(2 * math.pi * 5.1 * age) * depth / 100.0 * BR
                    if fall_s and t >= fall_t0:
                        f = (t - fall_t0) / fall_b
                        v += -3.2 * BR * f ** 1.8
                        s.cc(TR, 11, (base - 14) * (1 - f) ** 1.3, t)
                    s.bend(TR, v, t)
                    t += step
                s.bend(TR, 0, end + ms(5))
    s.cc(TR, 11, 0, bar(48, 2.95))
    return s


# --------------------------------------------------------------------------- stem 4: strings + choir
def stem_strings_choir():
    s = Song()
    ST, TQ, CH = 0, 1, 2
    s.program(ST, 0, 49, 100, 64, 66)        # Slow Strings
    s.program(TQ, 0, 44, 92, 70, 72)         # Tremolo Strings
    s.program(CH, 0, 53, 96, 56, 84)         # Voice Oohs

    def hold(ch, pitches, b0, length, vel, j=10):
        for p in pitches:
            s.note(ch, p, b0 + jit(j, 20), length + ms(R.uniform(-15, 15)), hv(vel, 3))

    # ---- low strings: one chord per bar, tied when the chord repeats
    runs = []
    b = 1
    while b <= BARS:
        e = b
        while e + 1 <= BARS and CHORDS[e] == CHORDS[b - 1] and (e + 1) not in (9, 17, 25, 33, 41) \
                and e + 1 - b < 4:
            e += 1
        runs.append((b, e, CHORDS[b - 1]))
        b = e + 1
    for b0, b1, ch in runs:
        length = (b1 - b0 + 1) * BAR - 0.08
        vel = 60 if b0 < 25 else (68 if b0 < 41 else 80)
        hold(ST, STR_LO[ch], bar(b0), length, vel)
        if b0 >= 33:
            hold(ST, STR_HI[ch], bar(b0), length, vel - 8)
    # expression curve for the low strings (the slow burn)
    s.cc(ST, 11, 96, bar(1))
    s.ramp(ST, 11, bar(1, 0.2), bar(3), 96, 58, step=0.25, shape='sin')     # impact decays
    s.ramp(ST, 11, bar(3), bar(5), 58, 70, step=0.25, shape='sin')
    s.ramp(ST, 11, bar(5), bar(8, 2), 70, 82, step=0.25, shape='sin')
    s.ramp(ST, 11, bar(8, 2), bar(9, 1), 82, 62, step=0.25, shape='sin')
    for k in range(4):                                     # couplet 1: breathing swells
        b0 = 9 + 4 * k
        s.ramp(ST, 11, bar(b0, 0.5), bar(b0 + 2), 60, 74, step=0.25, shape='sin')
        s.ramp(ST, 11, bar(b0 + 2), bar(b0 + 4), 74, 62, step=0.25, shape='sin')
    s.ramp(ST, 11, bar(25), bar(33), 66, 86, step=0.25, shape='sin')
    s.ramp(ST, 11, bar(33), bar(41), 86, 100, step=0.25, shape='sin')
    s.ramp(ST, 11, bar(41), bar(43), 100, 88, step=0.25, shape='sin')
    s.ramp(ST, 11, bar(43), bar(48, 2.6), 88, 122, step=0.25, shape='sin')
    s.ramp(ST, 11, bar(48, 2.6), bar(48, 2.95), 122, 96, step=0.05)

    # ---- tremolo strings: enter on couplet 2 pass 2 and build to the seam
    for b in range(33, 49):
        ch = CHORDS[b - 1]
        pitches = [p + 12 for p in STR_LO[ch][-2:]]
        if b >= 45:
            pitches += [STR_HI[ch][-1] + 12] if ch == 'B7' else [STR_HI[ch][0] + 12]
        hold(TQ, pitches, bar(b), BAR - 0.04, 70 + (b - 33) * 2, j=6)
    s.cc(TQ, 11, 0, bar(1))
    s.cc(TQ, 11, 30, bar(32, 2.9))
    s.ramp(TQ, 11, bar(33), bar(41), 30, 70, step=0.25, shape='sin')
    s.ramp(TQ, 11, bar(41), bar(44), 70, 60, step=0.25, shape='sin')
    s.ramp(TQ, 11, bar(44), bar(48, 2.7), 60, 124, step=0.25, shape='sin')
    s.ramp(TQ, 11, bar(48, 2.7), bar(48, 2.98), 124, 70, step=0.05)

    # ---- choir "ooh": ghost voices
    def choir(b0, b1, ch, vel):
        hold(CH, CHOIR[ch], bar(b0), (b1 - b0 + 1) * BAR - 0.1, vel, j=14)
    choir(3, 4, 'Em', 54)
    for b in (5, 6, 7, 8):
        choir(b, b, CHORDS[b - 1], 58)
    s.cc(CH, 11, 0, bar(1))
    s.ramp(CH, 11, bar(3), bar(4, 2), 0, 70, step=0.25, shape='sin')
    s.ramp(CH, 11, bar(4, 2), bar(5), 70, 56, step=0.25, shape='sin')
    s.ramp(CH, 11, bar(5), bar(8, 2), 56, 78, step=0.25, shape='sin')
    s.ramp(CH, 11, bar(8, 2), bar(9), 78, 0, step=0.25, shape='sin')
    for b0, b1 in ((17, 18), (19, 20), (21, 22), (23, 24)):
        choir(b0, b1, CHORDS[b0 - 1], 50)
    s.ramp(CH, 11, bar(17), bar(19), 0, 52, step=0.25, shape='sin')
    s.ramp(CH, 11, bar(19), bar(24, 2), 52, 46, step=0.25, shape='sin')
    s.ramp(CH, 11, bar(24, 2), bar(25), 46, 58, step=0.25, shape='sin')
    for b in range(25, 41):
        choir(b, b, CHORDS[b - 1], 56 + (b - 25))
    s.ramp(CH, 11, bar(25), bar(41), 58, 84, step=0.25, shape='sin')
    for b in range(41, 49):
        choir(b, b, CHORDS[b - 1], 74)
        if CHORDS[b - 1] == 'F':                    # the bII: high ghost voice
            hold(CH, [65], bar(b), BAR - 0.1, 66, j=14)
        if CHORDS[b - 1] == 'B7':
            hold(CH, [63], bar(b), BAR - 0.1, 66, j=14)
    s.ramp(CH, 11, bar(41), bar(48, 2.6), 84, 110, step=0.25, shape='sin')
    s.ramp(CH, 11, bar(48, 2.6), bar(48, 2.95), 110, 0, step=0.05)
    return s


STEMS = [
    ('stem1_guitar', stem_guitar),
    ('stem2_bass_perc', stem_bass_perc),
    ('stem3_trumpet', stem_trumpet),
    ('stem4_strings_choir', stem_strings_choir),
]


def trumpet_notes():
    """(bar, beat, name, length) for the trumpet, for the melody check."""
    out = []
    for mel, first in ((MEL_C1, 9), (MEL_C1_B, 17), (MEL_C2, 25), (MEL_C2_B, 33)):
        for b, p, ln in place(mel, first):
            out.append((int(b // BAR) + 1, b % BAR, name_of(p), ln))
    for b, notes in MEL_OUTRO:
        for nm, bt, ln in notes:
            out.append((b, bt, nm, ln))
    return out


def main():
    os.makedirs(os.path.join(HERE, 'midi', 'loop_once'), exist_ok=True)
    for name, fn in STEMS:
        song = fn()
        write_mid(os.path.join(HERE, 'midi', name + '.mid'), song, N_LOOPS, name)
        write_mid(os.path.join(HERE, 'midi', 'loop_once', name + '.mid'), song, 1, name)
        notes = sum(1 for e in song.ev if e[2][0] & 0xF0 == 0x90)
        print(f'{name}: {notes} notes per loop, {len(song.ev)} events')


if __name__ == '__main__':
    main()
