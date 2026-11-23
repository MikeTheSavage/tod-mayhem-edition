# Third-party code

## box3d

Box3D, the 3D rigid-body physics engine by Erin Catto. MIT licence (`box3d/LICENSE`, kept unchanged).

| | |
|---|---|
| Source | https://github.com/erincatto/box3d (the official repository, cloned 2026-10-28) |
| Pinned commit | `336263320469e5e9febfe18697ab562488f7e97a` (2026-10-27, "Dynamic Mover (#202)") |
| Kept | `src/`, `include/`, `test/`, `shared/` (the engine, its headers, its own unit tests and their helpers), `LICENSE`, `README.md`, the CMake / build files |
| Left out | `samples/`, `extern/` (sokol, imgui, ... for the samples app), `data/`, `docs/`, `benchmark/`, `.github/`, the `.git` folder |
| Used by | `TODPhysics.dll` (`physics/code/phys_sim.cpp`), built by `dll/build.bat` as a static library: C17, `/MT`, x86, `BOX3D_DISABLE_AVX2` (the SSE2 path; the game process may run on CPUs without AVX2 and the 8-wide AVX2 kernel is not needed for a few dozen props), single-threaded (`workerCount = 1`, no Box3D threads inside the game) |

### Local changes (32-bit x86 MSVC)

Box3D builds for x64 / ARM64 upstream. Two intrinsics it uses do not exist for 32-bit x86 in MSVC; both are replaced
under `_M_IX86` / `!_WIN64` only (x64 builds are unchanged). The diff is `box3d_x86.patch`.

* `src/platform.h` `b3AtomicFetchAddI64`: `_InterlockedExchangeAdd64` -> a `_InterlockedCompareExchange64` (cmpxchg8b) loop.
* `src/ctz.h` `b3PopCount64`: `__popcnt64` -> two `__popcnt` of the halves.

With those two changes the whole upstream test suite (`test/`, 28 test groups incl. determinism, recording, large
world) passes as a 32-bit x86 executable: `physics/tools/box3d_selftest.bat`.

* `src/dynamic_tree.c` `b3FindBestSibling` (all platforms, `box3d_tree_hang.patch`): the greedy descent never steps into a
  leaf, and it stops after `nodeEnd + 2` steps. Upstream, a NaN / inf box made every cost compare false, the else-branch
  descended into child 2 even when it was a leaf, and the leaf's proxy id was then read as a child index: the walk went
  through unrelated nodes and, in 17 of 200 random trees, never returned (the game thread hung, frame frozen, sound on).
  `physics/code/test_phys.cpp` `test_nan_no_hang` runs 400 such trees on a worker thread with a time limit. TODPhysics.dll
  also never passes a non-finite float in (`phys_sim_internal.h` `phsim_v` / `phsim_q`).

* `src/hull.c` (all platforms, `box3d_hull_hang.patch`, marked `TOD Gameplay+ patch (hull_hang)`): a `failed` flag, a step counter
  (`work` / `workCap`) and bounds on every pointer-chasing walk of the hull builder. Upstream, `b3HullBuilder_ConnectFaces` follows
  the run of edges shared by two faces with no bound; when the two faces share EVERY edge (a degenerate cloud: nearly collinear
  points, a flat patch far from the origin, duplicated points) the walk goes round the face ring forever. A player's game froze
  there (mission MM030 Smash the Stash, Map06: EIP `ConnectFaces+0x60`, stack `MergeFaces <- AddVertexToHull <- Construct <-
  b3CreateHull`); the offline fuzz (`physics/tools/hull_fuzz.bat`) hangs the original at exactly that address on about 2 % of its
  degenerate inputs, and also found 4 of 12,000 that crash it (NULL dereference in `b3NewellPlane` on a ring that runs into a
  retired half-edge). Now: the walks stop before changing anything when they come back to their start or run past the edge pool,
  the pools never write past their end, `b3NewellPlane` and the merge loops are bounded, `Construct` returns false and
  `b3CreateHull` returns NULL (`make_hull` in `phys_sim.cpp` already retries on a 5 mm grid and then falls back to a box).
  `b3HullBuilderGaveUp` counts the abandoned builds (logged in the `physics: stats` line). On every input where the original
  returns, the patched builder returns the identical hull (hash compared, 12,000 cases). Regression: `physics/code/test_phys.cpp`
  `test_hull_hang` (three real hanging inputs, 6000 random degenerate clouds, the game's function table) and
  `physics/tools/hull_fuzz.bat` (original vs patched; replays `physics/tools/hull_hang_cases/*.pts`).

To update: clone the official repository at the new commit, apply `box3d_x86.patch`, `box3d_tree_hang.patch` and `box3d_hull_hang.patch` (check that the functions still
need it), copy the kept folders over, run `dll\build.bat` and `physics\tools\box3d_selftest.bat`, update the commit here.
