# Sources: Mayhem music ("La Llorona")

## Music
"La Llorona" is a traditional Mexican song in the public domain. The melody used
here is the plain traditional tune (E minor, 3/4). The intro, accompaniment,
counter-lines, outro lament and percussion are this project's own arrangement.
They are not taken from any recording (Chavela Vargas, Lila Downs, *Coco*, etc.).

## Downloads (2026-10-28, kept in `work\music\downloads\`, git-ignored, never shipped)

| What | URL | File | Size (bytes) | sha256 |
|---|---|---|---|---|
| GeneralUser GS v2.0.3 SoundFont (S. Christian Collins), author's official GitHub, commit 684543d5 | https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/684543d5e5efaef08d02be50dcda8d552478fa60/GeneralUser-GS.sf2 | `generaluser_gs\GeneralUser-GS.sf2` | 32,319,396 | 9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe |
| Its licence | https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/684543d5e5efaef08d02be50dcda8d552478fa60/documentation/LICENSE.txt | `generaluser_gs\LICENSE.txt` (copy: `licenses\GeneralUser-GS_LICENSE.txt`) | 2,317 | 7b32efefdf95ce38a043799f0659853ddc00fbaa14d8c50f0aca16b9b8b405be |
| Its README (preset list, recommended FluidSynth settings) | https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/684543d5e5efaef08d02be50dcda8d552478fa60/documentation/README.md | `generaluser_gs\README.md` | 67,719 | fb23217ca2a9cf199e04aed1b04a7e81e024330411941c3638302e081584c151 |
| FluidSynth 2.6.1 (LGPL), official Windows x64 release | https://github.com/FluidSynth/fluidsynth/releases/download/v2.6.1/fluidsynth-v2.6.1-win10-x64-cpp11.zip | `fluidsynth\fluidsynth-v2.6.1-win10-x64-cpp11.zip` (extracted to `fluidsynth\fs\`, then the zip was deleted) | 2,727,529 | fab7a2e4b85675b66970f97a39bbc239729c5e0f237198b5922a6a73cbc8677c |

- The SoundFont page https://schristiancollins.com/generaluser.php renders its
  download links with script. The author's GitHub repository was used instead.
- Extracted `fluidsynth.exe` sha256: 557b9e98d860a6df6a927e58f6cc274d6451a046da780a1011810d9d00db133d.
  It is used only as a local offline renderer. Nothing else from the downloads
  was executed.
- FluidR3_GM (the fallback) was not needed. GeneralUser GS has a usable Nylon
  Guitar (0:24) and Trumpet (0:56).

## Licence notes
- GeneralUser GS licence v2.0 lets you use the SoundFont without restriction for
  your own music, private or commercial. Its samples allow full use in music
  production. No attribution is required, so the credit line in
  `web\music\music.json` names only the song and the arrangement. The SoundFont
  is acknowledged in the separate `instruments_source` field. The licence also
  asks that websites do not hotlink the author's download files.
- FluidSynth (LGPL-2.1) is not distributed. Only its rendered audio output is
  used.
