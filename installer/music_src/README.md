# Mayhem music: "La Llorona" (traditional), arranged for Gameplay+

A 48-bar loop of 3/4 at 80 BPM in E minor (108.000 s, 5,184,000 samples at 48 kHz).
It comes as four sample-aligned stems. Each Mayhem toggle adds the next stem:

| # | File (`installer\web\music\`) | Instruments (GeneralUser GS, bank:program) |
|---|---|---|
| 1 | `stem1_guitar.ogg` | Nylon Guitar 0:24: tremolo-picked melody, arpeggios, strums, fills |
| 2 | `stem2_bass_perc.ogg` | Acoustic Bass 0:32 as guitarrón (octave pairs), Timpani 0:47 heartbeat and roll, conga "cajón" and maracas (Standard kit, ch 10), concert bass drum (Orchestral kit 120:48) |
| 3 | `stem3_trumpet.ogg` | Trumpet 0:56: the melody, with pitch-bend vibrato, falls, CC11 phrasing and a filtered canyon echo |
| 4 | `stem4_strings_choir.ogg` | Slow Strings 0:49, Tremolo Strings 0:44, Voice Oohs 0:53 |

Play every stem from the same start sample, summed at unity gain. The full mix
peaks at about -1 dBFS. `music.json` holds the metadata.

## Form
| Bars | Section | Chords |
|---|---|---|
| 1-8 | Intro: the bar 1 landing hit, then a ghostly guitar tremolo of the opening phrase over an E pedal, then the bII colour | Em Em Em Em Am F B7 B7 |
| 9-24 | Couplet 1 twice ("Salías del templo..."). The trumpet enters on the bar 8 pickup. On the 2nd pass the guitar adds the tune in tremolo an octave up | Em Em Em Em B7 B7 Em Em |
| 25-40 | Couplet 2 twice ("Hermoso huipil..."). The 2nd pass climbs to C5, and tremolo strings, shaker and a fuller cajón come in | Em Em E7 Am Am B7 Em Em |
| 41-48 | Outro: the trumpet lament, F (bII) menace, a timpani roll and a string crescendo on B7. It resolves across the seam onto bar 1 | Em C Am F Em F B7 B7 |

## Re-render
Run from any folder. Python 3 with numpy and scipy, nothing to pip install:

    python -I <repo>\installer\music_src\compose.py         # writes midi\*.mid
    python -I <repo>\installer\music_src\render.py          # FluidSynth -> loop -> ogg, previews, report (~5 min)
    python -I <repo>\installer\music_src\render.py --reuse  # skip FluidSynth for stems with a cached loop
    python -I <repo>\installer\music_src\verify_melody.py   # pitch-tracks the trumpet ogg against the written tune

After an edit in `compose.py`, delete `work\music\render\<stem>_loop_cache.npy`
for each changed stem, or run without `--reuse`.

Requirements: the SoundFont and FluidSynth in `work\music\downloads\` (see
`SOURCES.md`), and an ffmpeg build with libvorbis (on PATH, or set the `FFMPEG` environment variable to its path).

## How the seamless loop is made
`compose.py` writes the loop body three times in a row into each MIDI file. The
humanisation is drawn once from a fixed seed, so the three passes are identical.
`render.py` keeps the middle pass, so it starts with the real reverb tails and
ringing notes of the previous pass. The first 50 ms of that pass are crossfaded
with the first 50 ms of pass 3. Sample L-1 therefore runs into sample 0 exactly
as it does in a continuous performance. The loop is 81,000 FluidSynth 64-sample
blocks long, so event timing is identical in every pass.

## Files
- `compose.py`: the arrangement, humanisation and Standard MIDI File writer (stdlib only)
- `render.py`: rendering, loop extraction, balance (`MIX_GAIN`, `HIGH_PASS`), normalisation, Vorbis encode, previews, spectrograms, measurements
- `verify_melody.py`: checks the trumpet audio against the tune
- `sf2_presets.py`: lists the presets in a .sf2
- `midi\*.mid`: render input (3 passes); `midi\loop_once\*.mid`: one pass, for a sequencer
- `last_render_report.json`: levels, seam, spectrum and section loudness from the last render
- `SOURCES.md`, `licenses\`: downloads and licences
- Previews (written by `render.py` into the git-ignored `work\music_preview\`): `llorona_preview_{1,1-2,1-3,full}.wav` (30 s; the full one crosses the loop point at 15.0 s), `llorona_preview_full_spectrogram.png`, `llorona_full_loop_spectrogram.png`
