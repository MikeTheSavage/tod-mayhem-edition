# TODGameplay.dll: hooks and patches

GOG `TOD.exe` (6,860,800 B, md5 `d9beb351b3c78ee9f14a766a368f3ff9`, image base 0x400000). The DLL refuses to patch
anything unless the exe file on disk has exactly that size and md5. Each site is then patched only when **all** its
listed bytes match in memory (a site already hooked by another mod is skipped and logged; dependent sites of the same
feature stay off together). Source of truth: `src/sites.h`. Verification:

* **S** `tests/dll/check_sites.py`: the bytes in the exe file, the patch covers whole instructions (capstone),
  CALL targets / stub return addresses equal the constants in `gp_game.cpp`, operand patches decode to the intended
  instruction, no overlap with each other or with the renderer bridge's patches. Listing: `tests/dll/review/sites_listing.txt` (written by `build.bat`).
* **H** `tests/dll/host.exe`: a non-game process maps TOD.exe's sections (read from the file) at their addresses;
  the DLL refuses there (hash), then a test-only entry installs onto that image: every site's new bytes checked
  (call/jmp into the DLL, operand forms), every other byte identical, page protection restored; a pre-hooked camera
  entry is skipped while the rest installs.
* **X** executed in the host: FtolGameStub (x87 at 24-bit precision; ecx/ebx/esi/edi preserved, x87 stack balanced,
  carried total exact) and ArmDiveStub (suppressed path returns 0 and pops 8 bytes).
* **D** disassembly review of every stub in the built DLL: `tests/dll/review/stubs_disasm.txt` (written by `build.bat`).
* **U** unit tests of the logic behind it (`tests/dll/test_logic.cpp`, 325 checks).
* **G** = must be confirmed in the game ([TEST_IN_GAME.md](TEST_IN_GAME.md)).

## Patch sites

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there | Verified |
|---|---|---|---|---|---|
| LOOP_TOP | 0x43C4E8 | `e843f7ffff` (0x43C4E8) | call retarget | main loop `call 0x43BC30` (inactive check, every pass, before the input poll) -> LoopTopStub: limiter wait (regs, flags, xmm saved), tail-jump to 0x43BC30 | S H D U, G |
| POST_INPUT | 0x43C516 | `e84aeaffffe875dcffff` (0x43C511) | call retarget | `call 0x43A190` (pad poll, after the keyboard poll) -> PostInputStub: calls 0x43A190, then hotkeys, key remap on the keyboard object arrays, meter refill, bullet-time abort checks, shared memory | S H D U, G |
| KB_PROC | 0x740B70 | `5557a35cb7a300e86becffff` (0x740B69) | call retarget | play state 0x740560 `call 0x73F7E0` (kb/mouse procedure, cdecl 2 args) -> KbProcWrap: calls it, then the input tick (bullet time, quick restart) in the player's script context, skipped when the thread has a state change pending (+0x3C) | S H D U, G |
| PAD_PROC | 0x740AAD | `5557a35cb7a300e8bed1ffff` (0x740AA6) | call retarget | same for `call 0x73DC70` (gamepad procedure) | S H D U, G |
| MOUSE_DX | 0x73ED99 | `f30f59442410f30f114644` (0x73ED93) | jmp hook 5 | `movss [esi+44h],xmm0` (MouseDeltaX) -> `mulss xmm0,[g_mx_factor]`, the store, back to 0x73ED9E | S H D, G |
| MOUSE_DY | 0x73EDF0 | `f30f59442410f30f114648` (0x73EDEA) | jmp hook 5 | `movss [esi+48h],xmm0` (MouseDeltaY) -> `mulss xmm0,[g_my_factor]` (invert, scale, optional fps factor), store, back to 0x73EDF5 | S H D U, G |
| YAW_WRITE | 0x73FFD5 | `f30f59442444f30f114024` (0x73FFCF) | jmp hook 5 | `movss [eax+24h],xmm0` (PhysicCtrl.turn_speed from the mouse) -> the store, then accumulate degrees (YawAcc), back to 0x73FFDA | S H D U, G |
| YAW_PAD | 0x73E3F9 | `f30f114224` | jmp hook 5 | the pad's turn_speed store -> store, leave mouse mode, back to 0x73E3FE | S H D U, G |
| YAW_READ | 0x57D504 | `f30f104624` | jmp hook 5 | integrate `movss xmm0,[esi+24h]` -> for the player in mouse mode: accumulated angle / TimeStep, else the original value; back to 0x57D509 | S H D U, G |
| DIAG | 0x55FCEF | `894108e91d0a0000` (0x55FCEC) | jmp hook 5 | `jmp 0x560711` after the player's stick copy into [esi] -> clamp (x, z) to length 1, then jmp 0x560711 | S H D U, G |
| ARM_DIVE1..3 | 0x53A5BE, 0x53C526, 0x53CB4F | `6a006af4894c2428e89df83200`, `6a006af4e835d93200`, `6a006af4e80cd33200` | call retarget | `SendMessage(TIME_CTRL, -12 command_auto_slow_motion, 0)` of the dives -> ArmDiveStub: `ret 8` with eax 0 while suppressed (dive slow motion off, or bullet time running and `dive_slowmo_in_bullettime = 0`), else tail-jump to 0x869E60 | S H X D, G |
| ARM_WIRE | 0x5F4429 | `6a006af4e8325a2700` (0x5F4425) | call retarget | same for the slide wire (`wire_auto_slowmo`) | S H D, G |
| CLK_GAME | 0x897E3C | `d905c8dca300d80d54399b00e8ff970b00` (0x897E30) | call retarget | game-ms `fld gdt; fmul 1000; call _ftol2` -> FtolGameStub: pops st(0), credits floor(total) - floor(previous) (fraction carried), eax (edx = sign); ecx, xmm0-7 kept | S H X D U, G |
| CLK_REWIND | 0x897C31 | `d905c8dca300d80d70dd9b00e80a9a0b00` (0x897C25) | call retarget | the rewind credit (x -1000) of the same clock -> FtolGameStub (same accumulator) | S H X D U, G |
| CLK_REAL | 0x8974F8 | `d905c0dca300d80d54399b00e843a10b00` (0x8974EC) | call retarget | real-ms credit -> FtolRealStub (own accumulator) | S H X D U, G |
| PICKUP | 0x79D0A3 | `0f57c06a0a8bcff30f114644` (0x79D09C) | jmp hook 5 | prop_move_ctrl `t = 0` after a step -> `t = min(max(t - TimeStep, 0), TimeStep)` (esi+44h, esi+0A8h), back to 0x79D0A8 (xmm0 is reloaded by the game right after) | S H D, G |
| CAM_ENTRY | 0x87DE40 | `81ecf0000000` | jmp hook 6 | Camera::UpdateCameraMatrix entry -> CameraStub: effective fov (GameCamera only unless `fov_all_cameras`), culling aspect; then `sub esp,0F0h`, back to 0x87DE46 | S H D U, G |
| CAM_VIEW | 0x87DE5F | `8b86b4000000` | operand | `mov eax,[esi+0B4h]` -> `mov eax,[g_fov_eff]; nop` (fov for the render views); the stored fov +0B4h is never written, so game logic still sees 64 | S H, G |
| CAM_CULL | 0x87DFE5 | `f30f1086b4000000` (0x87DFE2) | operand | modrm+disp of `movss xmm0,[esi+0B4h]` -> `movss xmm0,[g_fov_eff]` (5 bytes `05 <addr>`) | S H, G |
| CAM_ASPECT | 0x87E054 | `f30f1005ec389b00` (0x87E050) | operand | the 4:3 culling aspect operand -> `[g_aspect]` (= width/height/ratioxy). **Shared with the renderer bridge's cull fix**: whoever patches first wins, the other sees different bytes and skips. The DLL also skips `cull_fix` when a dinput8.dll from the game folder is loaded. | S H, G |
| STEER_RET | 0x4F60C5 | `e8eb5337000f57c0f30f118678030000` (0x4F60C0) | jmp hook 11 | `xorps xmm0,xmm0; movss [esi+378h],xmm0` (wheel_angle = 0 without a steering key) -> wheel_angle toward 0 by `steering_return_rate` per 0.04 s step (`max(v-r,0)+min(v+r,0)`), xmm0 = 0 again (the code after reads it), back to 0x4F60D0 | S H D U, G |
| SBRAKE_A | 0x4F6D85 | `0f2f4424188954241c0f874a010000` (0x4F6D7C) | operand | `ja 0x4F6ED5` (S held: pedal path only when rolling backwards) -> `jmp 0x4F6ED5; nop` | S H, G |
| SBRAKE_B | 0x4F6F43 | `0f2f442418894c24148944241c771e` (0x4F6F36) | operand | `ja 0x4F6F63` (same test inside the pedal path) -> `jmp short 0x4F6F63`: S counts as a pedal, no_force_inertia_mass_factor 1.0 like V | S H, G |

## Game functions the DLL calls

| Address | Function | How | Checked bytes |
|---|---|---|---|
| 0x869E60 | `Node::SendMessage(this=node, int id, void* args)`, ret 8 -> `Scriptbaked::DispatchMessage` 0x48A7E0 (sets the script "current node" globals 0xA3B75C/60/64; the DLL saves and restores them around each send) | `__fastcall` with a dummy edx | `8b4424088b54240450` |
| 0x86B4B0 | member-write notify (rewind record) `thiscall(cache node, member)`, ret 4: what `command_change_world_time` calls before writing WorldTime (member 0x1F4) | `__fastcall` | `51578bf98b4728` |
| 0x73F7E0 / 0x73DC70 | the kb/mouse and gamepad procedures (cdecl, thread, 0), called by the wrappers exactly as the game does | cdecl | through KB_PROC / PAD_PROC |
| 0x43A190, 0x43BC30 | pad poll, inactive check (the original targets of the retargeted calls) | as the game | `a17c5ea30056`, `83ec2c53568bf1` |

Messages sent (ids from the game's class method tables):

| Receiver (cache data slot) | Id | Command | Args | When |
|---|---|---|---|---|
| TIME_CTRL (+0x280) | -8 | `command_trigger_slowmotion(number,number,truth)` (handler 0x784D70, normal state) | {target, seconds, no_blur} | bullet time on (scale, ramp_in, !blur); off (1.0, ramp_out, 1); a resend once when the first was ignored |
| TIME_CTRL | -6 / -5 | `command_force_normal_time` (normal-state method 0x784020 / automatic-slow-motion handler) | none (NULL, as the game's landings send) | abort: a cut-scene started or the player lost control for 0.4 s (not paused, not exclusive) |
| MissionCtrl (+0x25C) | -10 | `Command_End_Mission(integer)` (0x790A20, only in the running state 0x788520) | {0} | quick restart (allowed missions only) |

Memory written without a message: TIME_CTRL WorldTime (cache data +0x850) after the 0x86B4B0 notify, exactly as
`command_change_world_time` does minus its forced snap. Memory read: cache data +0x854 Desired, +0x85C TimeToUse,
+0x864 ExclusiveMode, Scene +0x114 TimeMultiplier and +0x5C PlayMode, MASTER_CTRL ->+0x30 ->+0xD4 (cut-scene: the
test the explosion slow motion uses), MissionCtrl data +0x28 EnemiesKilledOnCurrentMission and +0x38 Current_Mission,
Combo_Ctrl data +0x3C CurrentActionPoints, keyboard object [0xA35E80] (+0 edges, +0x100 state), mouse object
[0xA35EAC] (button dwords). All reads are SEH-guarded; each address is proven by a REF row of `sites.h`
(the instruction that uses it, checked in the exe and again at run time before the feature is enabled).

## Interplay with the other proxies

* **Renderer bridge (dinput8.dll)**: no shared site except CAM_ASPECT (see above). The bridge's limiter sits at
  0x93D027 (call GfxInternal::Render), the DLL's at 0x43C4E8: use one of them. The bridge feeds input through the
  DirectInput device vtables; the DLL reads the game's keyboard/mouse objects after the poll, so bridge-injected keys
  reach the hotkeys and the rebinding.
* **TOD HD (d3d9.dll)**: hooks the D3D9 device and USER32 import slots; no code site in common. Its `fps_cap` (default
  60) makes the DLL's `fps_limit` unnecessary.
* **Research proxies (movefeel / camfov / locotune dinput8.dll, not included)**: overlap on purpose (the DLL contains
  their mouse, diagonal and fov features); do not install them together. The byte checks keep the second one off.

## Notes on the research proxies

* The camfov research proxy writes only the 4-byte displacement at 0x87DFE6 and keeps modrm 0x86, which makes
  `movss xmm0,[esi + &g_fov_eff]` (a wild read) instead of `[&g_fov_eff]`. The DLL writes modrm `05` + the address;
  `check_sites.py` decodes the patched form.
* The movefeel research proxy drops the accumulated mouse turn after 250 ms without a physics step: in slow
  motion (one step every 0.14 s at 0.3, 0.38 s at 0.11) that would throw mouse input away. The DLL scales the window
  with the time scale (`gp::yaw_drop_ms`).

## Suave (power-up slot 11; code `suave/code/`, docs `suave/README.md`)

Off unless `[suave] enabled = 1`; then it also needs POST_INPUT + KB_PROC / PAD_PROC (the input tick) and every
`SV_*` / `H_*` / `F_*` REF row below to match, else it stays off (logged). Verification letters as above, plus
**C** = `suave/code/tools/check_suave.py` (message ids vs the class method tables, member offsets and touch slots vs
the class registry, class handlers, property ids, calling conventions of 20 called functions) and
**T** = `test_suave.exe` (566 checks of the pure logic).

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there | Verified |
|---|---|---|---|---|---|
| USE_GATE_A | 0x5C0CDD | `6a048bcfe8cea72a00` (0x5C0CD9) | call retarget | `command_use_power_up` slots 2/9/10: `push 4; mov ecx,edi; call 0x86B4B0` -> UseGateStub: refuses while Suave runs (drops the return address + the pushed 4, jumps to the handler's common exit 0x5C0FE9: no charge spent), notes El Mariachi uses (its stream on SoundDict 110), else tail-jumps to 0x86B4B0 | S H X D, G |
| USE_GATE_B | 0x5C0D95 | `6a048bcfe816a72a00` (0x5C0D91) | call retarget | same for slots 3/4/6/8 | S H D, G |
| USE_GATE_G | 0x5C0E8F | `6a048bcfe81ca62a00` (0x5C0E8B) | call retarget | the generic branch (slot 11): with a charge, the game's eligibility 0x5BCD90 (cdecl thread, {result, slot 11}), the partner search; refused (no partner / not eligible) -> common exit; approved -> the game spends the charge, the move starts at the next input tick | S H X D, G |

Game functions the Suave code calls (all thiscall unless noted; ret bytes and `this` use checked by `callconv.py`):
0x869E60 SendMessage, 0x86A250 CallNative (`motion` 5Eh, `setplaypos` 4Bh), 0x86A2B0 TriggerScript (`attachnode`
45h/56h, `detachnode` 46h/57h, shape from the weapon attach 0x6140D7), 0x86A6A0 GetProperty (0x2A8 FactionID, 0x69D
torso_cs), 0x869E80 GetMember (FactionTable slot 0), 0x86B4B0 touch, 0x486950 quadtree query (static, the Tornado's
call), 0x8718D0 list release, 0x88CA80 / 0x88C9C0 world position / direction, 0x88E600 SetWorldPosition, 0x891360
SetRotationY, 0x88EB60 FindChild, 0x89E270 SoundSlot `streaming_sound` setter, 0x5010A0 layer speed, 0x9159E0
Character::GetModelRes, 0x5BCD90 eligibility (cdecl), 0x500BE0 Node::SetFlags (the dynamite's show(): flag 1 clear as
0x62E110 does, REF `SV_SHOW_FN`; node flag 4 HIDES a shell's model (set by the explode handler after a blast, REF
`SV_SHELL_SHOW`, cleared by command_release_grenade with 0x62E1B0), so the DLL clears flags 1 and 4 on the dynamite),
0x91D020 Navigator::IsPointOccluded (thiscall(this, from[4], to[4]) ret 8, al = blocked; the scene probe Scene +A8h,
ignore this +70h, mask 21h, IsLineColliding; REF `F_OCCLUDED` / `SV_OCCL_IGNORE` / `SV_OCCL_MASK`: the free-space
probe, the most open dive side, the camera clamp; hit distances by bisection).

Messages (constants and receivers in `suave/code/suave_msgs.h`; every id checked as the first of its group):

| Receiver | Ids | Commands | Game precedent (REF) |
|---|---|---|---|
| TIME_CTRL (cache +280h) | -8, -13 | trigger_slowmotion {target, s, no_blur}; do_explosion {size} | bullet time (above); barrel 0x67153C |
| GoodieCtrl (root data +88h) | -8, -7 | induce_god_mode {s} (armor); activate_goodie {17, 1.0} (cheat key, refund); god time cut = touch slot 3 + CurrentGodTime -= rest | El Mariachi 0x5BF838, 0x5C0C51 |
| HUD effects (powerup data +10h) | -8, -4 | start_effect {11, 60.0}; end_effect | 0x5BF38D |
| AmbientSoundCtrl (cache +254h ->+18h ->+3Ch) | -15, -16 | Play_Speak {slot, speaker}; stop_speak {slot} | El Mariachi 0x5BF874..0x5BF8F6, handler 0x4C0480 |
| MoveCtrl (root data +48h) | -36, -81, -69, -5, -19 | Request_new_State {3,22} park / {0,0}; idle; walk; stop_moving; teleport_here | EngageBeating 0x562BE4 |
| sentry AI (root data +58h, class handler 5C90F0h) | -23, -9 | no_ai_control; regain_control (KnockedOutTimer +264h = 0 first) | handler 0x5E3400 |
| character_root | -2 | take_a_hit {dmg, owner Ram, inflictor, torso_cs, impulse, pos, 1} | Tornado 0x538148, relay 0x5C4F40 |
| WeaponCtrl (root data +6Ch) | -66, -42, -13, -37 | special-move state / ended (Ram); unarmed / best weapon (NPCs) | power-up coroutine 0x5BF2F0 |
| PhysicCtrl (root data +68h) | -17, -11, -22 | collision off / on; reset_to_parent | teleport handler 0x510BE0 |
| weapon_shell (cache +550h pool, handler 843D10h) | -11, -12, -9, -14 | morph {3 dynamite}; aim {pos}; explode {1,0,vec} (owner +24h = Ram, radius +38h = boom_radius, restored after); goto_inactive | handler 0x848AA0 |
| default_weapon (the gun in hand, weapon ctrl +20h, handler 665630h) | -12, -8, -11 | stop_firing; kill_effects; command_drop (seize fire, GOTO the dropped state 0x667D00: detach from the hand, fall), sent before force_drop_weapon, which only clears the inventory slot (0x618C60) and left the gun floating | REF `SV_GUN_DROP`, `SV_CLS_WEAPON` |
| FaceCtrl (root data +9Ch, handler 541A50h) | -3 | facial_expression {id, stay, 0} (ids: AnimDict PlayerAnims/Facials) | handler 0x542090 |
| camera AI (cache +260h ->+18h ->+14h), GameCamera | -7, -8, -9, -10, -17, -18, -6; GameCamera -4 | look_at; transition_to; cut_to; stop_look_at; real-time camera on / off; shake; change_fov | golden gun 0x83EECE / 0x83F8C2, barrel 0x671496 |

Memory written without a message: MoveCtrl data +B8h/+BCh/+C0h (auto_move, after touch slots 28-30), sentry AI
+264h (KnockedOutTimer 0 before regain), weapon_shell +24h / +38h, GoodieCtrl +10h (after touch 3), the keyboard
arrays and mouse button dwords (input lock, injected dive keys), the turn before the automatic dive (no
orientation writes): degrees added to the mouse-turn accumulator (`gp_yaw_add`, consumed by the YAW_READ hook as
turn_speed = degrees / TimeStep) or, without `mouse_turn_fix` / in pad mode, PhysicCtrl +24h turn_speed (touch slot 0,
= degrees / TimeStep +84h) right after the input procedure in the input tick, as the mouse code writes it (0x73FFD5);
Ram's MoveCtrl +A8h target_lock / +A4h target_lock_worldpos_active cleared (touch 26 / 25) when set, because the
physics turn 0x57C220 turns to a lock instead of turn_speed (REF `SV_TURN_LOCK`), the script current-node globals (only restored when
an engine call changed them), SoundSlot `streaming_sound` through its own setter. Camera eye: the free camera node
RamLogicCamTarget (PLAYER_NODE_AICTRL data +114h) through SetWorldPosition, as the golden gun does.

Build note: a crashing test host exits with a negative code, which is not `errorlevel 1`, so `build.bat` tests
`if not "%errorlevel%"=="0"`; the host's stdout is unbuffered.

## Smart AI (`[smart_ai]`; code `smart_ai/code/`, docs `smart_ai/README.md`)

Off unless `[smart_ai] enabled = 1`. Then it needs the core / input REF rows (it runs in the input tick: POST_INPUT +
KB_PROC / PAD_PROC) and every row listed in `SM_REFS` of `smart_game.cpp` (the `SM_*` rows below plus F_TOUCH, F_GETPOS,
F_QUERY, F_LISTFREE, SV_CLASSOBJ, SV_CLS_SENTRY, SV_CLS_ROOT, SV_QUERY_A, SV_QUERY_FREE, SV_CAM_RELEASE, G_MISSION,
G_MISSION_CM); one mismatch and the whole feature stays off (logged). Verification letters as above, plus
**K** = `smart_ai/code/tools/check_smart.py` (every member the glue reads or writes proven by the game's own
touch(slot) + store pair or reader in the exe, the offsets cross-checked with the class registry layout of
character_sentry_ai, the REF list complete, calling conventions of 0x5D4190 / 0x86B4B0 / 0x88CA80) and
**M** = `test_smart.exe` (pure logic: personalities over 2,000 enemies of five shipped rows, approach tokens, morale).
Host test 9 executes SmartInitWrap and the tick on fake sentry nodes (init, touch, position and the spatial query
redirected to host functions), test 10 the switches.

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there | Verified |
|---|---|---|---|---|---|
| SM_INIT | 0x5E7C28 | `5789355cb7a300e863c5feff` (0x5E7C21) | call retarget | reset handler 0x5E7BE0 (`reset(placeholder)`, a pooled character takes over a placeholder): `push edx(&out); push edi(thread); mov [0xA3B75C], esi; call 0x5D4190` -> SmartInitWrap (cdecl, same 2 args): the sentry node is read from [0xA3B75C] first, then the original init 0x5D4190, then (out != 0) the personality writes and the registry; the script receiver globals are put back if they changed | S H X D K M, G |

Members written (character_sentry_ai data, each after `touch(sentry node, slot)` exactly as the game does; slot =
member index, every pair proven by an `SM_SLOT_xx` REF = the init's own `push slot; mov ecx,edi; call 0x86B4B0; ...;
movss [esi+off]`):

| Member | Offset / slot | Who writes it | Game reader (REF) |
|---|---|---|---|
| Strafe/Crouch/Wait time from/to | +4C..+58 / 0A..0D, +60/+64 / 0F/10 | spawn (personality) | state 20 action times |
| ProCrouch .. ProStrafeShootdodgeStrafe | +88..+A4 / 19..20 | spawn, only weights the row has (non-zero); the shoot-dodge bonus only over the row's own dodge weights | state 20 sequence pick (0x5D62D8) |
| StopDistance | +FC / 36 | spawn, only when it is the default 10 m | SM_RD_COMBAT (state 20 far test), state 13 / 17 / 19 |
| Retreat_Range | +48 / 09 | spawn, only a non-zero row value (cautious x1.25, capped at 0.6 x StopDistance) | SM_RD_RETREAT22 (state 22 back-off) |
| ShootDodge_Min_dist | +BC / 26 | never written (a value here would give assault enemies a 12 m dodge where the row has 0) | state 20 dodge test |
| Wait_until_Aggressive | +CC / 2A | spawn (only if 0), cleared by the tick once the enemy is past state 14 | SM_RD_WAITAGGR (13 -> CALL 14), SM_RD_WAIT14 |
| DontHunt | +F0 / 33 | tick (squad); back to the spawn value when the enemy leaves the fight or the game pauses | SM_RD_DONTHUNT13, SM_RD_DONTHUNT_LOST, 19's shoot_target_aquired 0x5DE1A0 |
| CS_HealthLeft / CS_Change_for | +68 / 11, +6C / 12 | tick (morale); back to the row values when fear is 0 or the game pauses | SM_RD_MORALE, SM_RD_CS, SM_RD_CF (0x5C98C0) |

Read only: Sub_Type +28 (SM_SLOT_01), Health +2C / DamageTaken +30 (SM_SLOT_02/03), Is_Melee +AC (SM_SLOT_22), Is_Buddy
+B8 (SM_SLOT_25), Morale +D8 (SM_SLOT_2D), Current_state +1D4 (SM_RD_STATE), CombatCtrl +234 -> data +8 target / +10h
squared distance (SM_RD_COMBAT), node +40h = root, root data +14h Sub_Type (SM_RD_ROOT), +18h FactionID (SM_RD_FACTION),
root data +58h AICtrl (SM_ROOT_AI), game ms = cache +274h -> +18h -> +4 (SM_CLOCK), mission index (G_MISSION_CM).

Game functions called: 0x86B4B0 touch (thiscall, ret 4), 0x88CA80 Node::GetWorldPosition (thiscall, ret 4; the
morale tick and the discovery centre), 0x486950 quadtree query + 0x8718D0 list release (thiscall, ret 4; the
Tornado's call, REF SV_QUERY_A / SV_QUERY_FREE: characters within 60 m of Ram once a second, to adopt enemies that a
loaded save restored without running the init, a case seen in testing). No message is sent.

Safety rules in the code: an enemy whose DontHunt or CS values were changed by anything else after the DLL wrote them
(the mounted-gun setup 0x5CAD10 writes DontHunt, mission scripts, rewind) is never written again; authored DontHunt /
StandFast enemies are never given or denied tokens; buddies, Sub_Type 4/7/8/9, the factions of `skip_factions`
(default 6 civilians, 10 DEA = Ram's side, 13 wrestlers) and health >= `boss_health` (1000: bosses and the 1000-hp DEA
specials of the post-game Harbour) are left alone; a behaviour the enemy's row does not have (shoot-dodge, back-off, a
state 20 move with weight 0) is never given; the registry is forgotten (no writes) on a mission change or when the
game clock goes back, but a table of the personalities written survives it: an enemy found again whose members still
hold our values is re-registered without writes (no second personality on top of the first); nothing
happens while paused (after one release pass) or rewinding.

## Seamless border swaps (`[seamless]`; code `seamless/code/`, docs `seamless/README.md`)

Off unless `[seamless] enabled = 1`; then every `SL_*` SITE and REF row (and F_CMD, F_SENDMSG, F_TOUCH) must match,
else nothing of it is patched (logged). Background: the author's reverse-engineering notes on the load controller and
on hiding the swap (not included). The renderer bridge hooks 0x746C40, 0x748840, 0x748AAC, 0x8986E0,
0x7478E0 and 0x421B30; none of these sites overlaps them (check_sites.py). Verification letters as above; the test
host (section 11) installs the feature alone, with prefetch, and with Smart AI, then drives one walk-border swap
through every stub with every game address redirected to host stand-ins (both paths of each stub, stack and
callee-saved registers checked); `test_seam.exe` 45 checks of the batch rule, timing record and prefetch table.

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there | Verified |
|---|---|---|---|---|---|
| SL_WALKREQ | 0x7E69EA | `8b0d8cb5a300...897c2414e851390800` (0x7E69C7, 40 B) | call retarget | ActionType 21 action body: `SendCommand(LoadCtrl, 0x273 command_load_block, {action})` -> WalkReqStub: reads the pending list, calls 0x86A340 with the same arguments, tags every entry it appended (and the action's TargetEntity +24h) as a walk request; ret 8 | S H X D U, G |
| SL_LSPUSH | 0x74A985 | `8b15ecbda300...e8763dd4ff` (0x74A974) | call retarget | processing state 0x74A8F0 case 0: push of the load-screen state 0x748840 -> LsPushStub: classifies the pending batch (seamless only if every entry is walk-tagged, none has a sTeleportName, all are map / submap folders, at most 4), logs it, tail-jumps to 0x48E700 | S H X D U, G |
| SL_LSSKIP | 0x7489A1 | `a18cb5a3008b4818` | jmp hook 5 | load-screen state case 3/4 (after setwindpause / setpausemode 1 and one frame): `mov eax,[0xA3B58C]` -> seamless: jmp 0x748B6E, the state's own path without a fade node (no fade-out 0.25 s, no 0.3 s wait at 0x748AAC, no 2 frames); else the mov, back to 0x7489A6 | S H X D, G |
| SL_LSSKIP2 | 0x748D0E | `6a008bcde899271200` | jmp hook 9 | after the state's kill-text (1EEh to the HUD parts), HUD -5, rumble -3 and AmbientSoundCtrl -2Fh `command_Shutdown_for_loading(1)`: seamless: jmp 0x74A801 (the +84h check and exit: no load screen chosen or shown); else `push 0; mov ecx,ebp; call 0x86B4B0`, back to 0x748D17 | S H X D, G |
| SL_PROGRESS | 0x9CC4F4 | `30b88700` (vtable of the progress object [0xA3D7D0], slot +8) | data pointer | SetProgress(float, bool) 0x87B830 draws the bar and presents a frame (0x421B30) -> ProgressStub: during a seamless swap `ret 8` (nothing drawn, no present: the window keeps the last game frame through UpdateLoadedBlocks); else jmp 0x87B830 | S H X D, G |
| SL_POSTFADE | 0x747CA5 | `89a9a8000000 8b158cb5a300` (0x747C9F) | jmp hook 6 | post-load 0x7478E0, FadeFromBlackAfterLoad (+A8h) branch after the flag clear: `mov edx,[0xA3B58C]` -> seamless: jmp 0x747DBE (no show of the black fade node, no alpha 1.0, no 0.2 s wait); the HUD -7, -10h {4.0}, setpausemode 0 and 4Ch follow as vanilla; else the mov, back to 0x747CAB | S H X D, G |
| SL_FADEIN | 0x747E3A | `518b48188b49606afb...e821201200` (0x747E2B) | call retarget | `SendMessage(ObjectfadeCtrl, -5 fade from black, {fade node, 0.3})` -> FadeInStub: not sent after a seamless swap (nothing was faded; eax 0, ret 8); ends the swap and logs trigger-to-play; else 0x869E60 | S H X D, G |
| SL_SWAP | 0x89A819 | `8b4424048b10526a01e8c2deffff` (0x89A810) | call retarget | the updateloadedblocks command: `call 0x8986E0` (UpdateLoadedBlocks_Impl, thiscall ret 8) -> SwapTimeStub: same call, timed (QPC), logged with running min / avg / max per kind | S H X D, G |
| SL_SWAP_RG | 0x874ECA | `506a01e811380200` (0x874EC7) | call retarget | ResetGame (save load) reload pass, same call -> SwapTimeRgStub (kind "save load") | S H D, G |

Memory read, never written: LoadCtrl data +0Ch `Load_pending_list` (count `[list+8] / [[0xA3B8F0]+24h]`, element i
`[[[list+4]+4] + 4*es*i]` = `s_BlockLoad*` {+0 folder, +4 sTeleportName}), Folder +50h kind, the action data +24h
TargetEntity, the player holder chain `[0xA3B590]+18h+84h` (prefetch). Game functions called: GetProperty 0x86A6A0
(Specific_ID 0xA9F of a pending folder, for the log; script globals saved / restored), GetWorldPosition 0x88CA80 (Ram's
root, prefetch, input tick only). No message is sent and no member written by the DLL itself.

Proven by REF rows: SL_AT21 (ActionType 21, +24h), SL_LIST / SL_LIST_EL / SL_ENTRY_TP / SL_APPEND (the list and its
element), SL_LS_TEXT / SL_LS_SOUND / SL_LS_EXIT84 (the jump targets inside 0x748840 and their register contract),
SL_FOLDER_KIND, SL_FADE_SEND (the fade node at cache +314h), SL_POST_HUD (jump target in 0x7478E0), SL_PROG_FN /
SL_PROG_RET / SL_PROG_VT / SL_PROG_OBJ (the progress object, its vtable and SetProgress's ret 8), F_SWAP / SL_SWAP_RET,
SL_PLAYER. check_sites.py also checks that every R_SL_ jump constant equals its REF row and the DATA slot's value.

Design notes:
* The 0.3 s wait is only reached through the fade-out path; skipping that path for walk swaps removes it without
  patching the float at 0x748AAC (missions, taxis keep their wait; the bridge also patches that float).
* The load-screen state also shuts the ambient / mission sound down for the load (AmbientSoundCtrl -2Fh) and the
  post-load state resumes it (-30h `command_Resume_after_Loading`): skipping the whole state would resume sound that
  was never stopped while the outgoing block's emitters unload. The skip is therefore two points around those messages.
* The fade-after-load branch of the post-load state paints the screen black on purpose (fade node alpha 1.0, 0.2 s)
  before fading in; it runs for every walk loader (they all send `command_fade_from_black_after_load`). It also holds
  the only `setpausemode 0` of that path, so the seamless jump lands after the black part, not past the unpause.

Interplay: with `[smart_ai]` both features use the shared post-input / input-tick sites (patched once). A border swap
keeps the current mission, so Smart AI keeps its registry; the unloaded enemies drop out through its own liveness
check, the new block's enemies get their personality at spawn (the init call site). No common site or member.

**Suave on slot 10** (`[suave] slot = 10`, the Pinata's slot): USE_GATE_A (the 2/9/10 branch) gates slot 10. Approved: the gate performs the branch's touch(4) with its return address replaced by 0x5C0E94 (REF `SV_USE_GENERIC` / `SV_USE_GENERIC2`: the generic branch right after its own touch), so the game's own count check, decrement (list type vtable +44h, op 0Bh) and selection refresh 0x5C00C0 spend the charge and the handler exits; the Pinata coroutine 0x5BF2F0 never runs. Refused: the common exit, charge kept. `check_sites.py` checks R_USE_GENERIC = that REF; the test host executes the redirect path with slot 11 and slot 10 (section 8 / 8b). The cheat key grants goodie `slot + 6` (16 = Pinata).

## Engine fixes (`[fixes]`; code `dll/src/gp_fix.cpp`)

| Site | VA | What | Why |
|---|---|---|---|
| FIX_VEC_FREE | 0x40638C (in 0x406350) | `call 0x4778D0` (free the old array buffer after the allocator realloc moved it) -> `VecFreeStub`: passed on, except when the realloc returned a block (esi != 0) and the old buffer's allocator has the CRT realloc as vtable +18h (0x47ACB0) | ALLOCATOR_DEFAULT's realloc is the CRT realloc (0x47ACB0 -> 0x952F77): it already released the moved block, so the engine's own free was a double free in the CRT heap, on every block swap (asset loader table copy, 0x85DEC0 mode 3: 0x8986E0 -> 0x87E8D0 -> 0x87E790 -> 0x8759E0 -> vtable +18h -> 0x406750 -> 0x4065A0 -> 0x406350). The crashes in ntdll (+0x934D9) after a few quick border crossings are this second free. |

REF rows: FIX_ALLOC_FREE (the free dispatch: off flag 0xA3AFBC, CS 0xA3AFA0, range table 0xA3AFE0 / count 0xA3B098),
FIX_DEF_RLC (0x47ACB0, the compared method), FIX_DEF_REALLOC (it calls the CRT realloc). The stub looks the allocator up
exactly as 0x4778D0 does, under the same critical section. Pool allocators (stack pools: realloc grows in place or
fails) keep the vanilla path. Host test 12 runs the stub on four cases.

| Site | VA | What | Why |
|---|---|---|---|
| FIX_WPN_OWNER | 0x8418D4 (in 0x8412B0) | jmp hook 6: `mov ecx,[esi+94h]` (pOwner) before `get(17h)` + `command_reload` (416h) -> `WeaponOwnerStub`: pOwner 0 -> jmp 0x8418F6 (no reload request; the shell picks its next state as after the request), else the mov and back to 0x8418DA | weapon_effects_ctrl's fire shell 0x8412B0 enters state 10 with an empty clip and a character owner; the next step (case 10/11, 0x84184E) re-reads the owner through 0x834EB0 (WeaponRoot +84h .Owner 6BBh -> pOwner; 0 when the weapon lost its owner meanwhile: dropped, its character despawned / pooled) and then uses pOwner unchecked: `mov ecx,[esi+94h]; push &out; push 17h; call 0x869E80` -> `mov eax,[ecx+4]` reads address 4. The observed crash (TOD.exe +0x469E89 = 0x869E89, ecx 0, return address 0x8418E6, in the post-game Harbour) is this read. |

REF rows: FIX_WPN_SKIP (0x8418F6, the skip target: frame recomputed from edi, then case 0Ch), FIX_WPN_OWNERRD (0x834EB0
writes pOwner from the weapon root's Owner and answers 0 for none). check_sites.py: R_WO_CONT = the patch end,
R_WO_SKIP = FIX_WPN_SKIP. Every skip is logged (`fixes: weapon_owner: ...`, first 5 then every 100th). Host test 12b
runs the stub with and without an owner.

## Debug (`[debug]`; code `dll/src/gp_debug.cpp`, `gp_debug_logic.cpp`; test copy only)

No game site of its own: TOD.exe's import slots HeapAlloc 0x9B31DC / HeapFree 0x9B31D0 / HeapReAlloc 0x9B31E0 are
replaced (after checking they hold the kernel32 addresses) when `heap_check` is on; the key script and the stress run
use the existing POST_INPUT / input-tick hooks. The stress run sends `command_load_block` (0x273, {folder}) and
`command_fade_from_black_after_load` (0x274) to LoadCtrl like the ActionType 21 action body (0x7E69D5..0x7E6A1A),
bracketed by the seamless walk tagging (`seam_debug_request`). They are driven by an in-game test harness
(not included).

## Display (`[display]`; code `dll/src/gp_display.cpp`, maths `gp_display_logic.cpp`; docs `../display/README.md`)

Off by default (`enabled = 0`: nothing patched). Background: the author's reverse-engineering notes on the HUD and menus
and the hudwide research proxy (not included; wide_hud / hud_scale / border / res_table are ported from it, the menu fit
is new here). Every site and frame offset below is a SITE / REF row of `sites.h` (check_sites: 30
DS_ sites + 28 DS_ refs OK), host test 13 executes every stub.

| Site | VA | Was | Now |
|---|---|---|---|
| DS_RES_W0..H4 | 0x4AB9FF / 0x4ABA07 ... 0x4AB973 / 0x4AB97B (table 0x4AB940) | imm32 of `mov [esp+4/8], 640/480 .. 1600/1200` | the 16:9 sizes (auto: 1280x720, 1600x900, 1920x1080, 2560x1440, configpc screensize) |
| DS_RES_START | 0x769ABD (after_initial_frame 0x769A80) | `call 0x4AB940` (saved Setting_ScreenSize -> setscreenresolution) | `StartupResC`: logged, not applied (`startup_size = config`) |
| DS_WH_DIM / DS_WH_DOT | 0x72AFD6 / 0x6DB6F4 | `call 0x485640` (is_wide_screen: console flag) | `IsWideHook` (stdcall, ret 4): true; canvas 480 / hud_scale high, 480 W/H wide from the back buffer (renderer +328h / +32Ch); rewrites the 853 imm (0x72B060) when the width changes |
| DS_WH_H480 / DS_WH_W853 | 0x72AFE5 / 0x72B060 | imm 480 / 853 | canvas height / width |
| DS_WH_SX / SY / DOTX / DOTY | 0x72B083 / 0x72B0A8 / 0x6DB714 / 0x6DB72A | `movss xmm0,[853.0 / 480.0 / 437.0 / 176.0]` | `[g_ds_vwf / vhf / dotx / doty]` (aim dot = canvas width / 2 + 10) |
| DS_WH_BORDER | 0x719108 | `movss xmm0,[0.05]` (BORDER_PERCENTAGE) | `[g_ds_border]` (only with `border` set) |
| DS_SPR_MTX | 0x8F4D20 (Sprite render 0x8F4C40) | `call 0x4671A0` (scale W/800, H/600) | `SprMtxStubA`: the same 4x4 plus a translation row; the caller frame (esp+10h at entry) gives the sprite's centre [+74h/+78h] and half size [+30h/+14h]: box -> fit (s = min(W/800, H/600), centred), full-width band -> x stretched / y fitted, full-screen -> cover (or stretch / fit) |
| DS_SPR2_MTX | 0x8F6EED (list path 0x8F6D65) | same call | `SprMtxStubB`: centre [+60h/+64h], half [+2Ch/+10h] |
| DS_TXT_SCALE_A / _B | 0x8FE7DF / 0x8FEFE2 (TextBox render, screen-relative boxes only) | `call 0x8F4950` (font scale W/800, H/600) | `FontScaleC`: s with the lowest mantissa bit set (the tag), offset kept; the box resets the scale to 1.0 after the draw (REF DS_TXT_RESET_A/B), so only these glyphs carry the tag |
| DS_TXT_A_ROT / _A_AX | 0x41B5C8 (10 B) / 0x41B805 (6 B) in DrawString 0x41B200 | `test byte [esi+38h],10h; je` at the joins after the font-scale multiply | when font +5Ch and +60h hold the tag: + (ox, oy) on the corners ([esp+14h/18h] [28h/2Ch] [0Ch/10h] [1Ch/20h] rotated; [14h/18h] [0Ch/10h] axis-aligned), then the test / je again (both continuations proven by REF rows) |
| DS_TXT_B_ROT / _B_AX | 0x41BD82 / 0x41BFBF in DrawString 0x41B9C0 | same | same (rotated corner 1 at [esp+24h/28h]) |
| DS_MOUSE | 0x8F4AD4 (12 B, TriggerMouseCallbacks 0x8F4A00) | `movss [esp+14h],xmm2; movss [esp+18h],xmm0` (cursor / (W/800, H/600)) | `MouseStub`: ((x - ox) / s, (y - oy) / s); all registers and the flags of `test edx,edx` (the jle after) kept |
| DS_LOADBAR | 0x87BA5E (load bar 0x87B830) | `call 0x4322A0` (quad: tex, p0..p3, uv0..uv3, colour; ret 28h) with vanilla pixels | `LoadBarStub`: the 4 positions moved onto the fitted canvas (x / sx * s + ox), then the original |

Not touched: the native `widescreen_emulation` flag (projection, culling, `scale_if_widescreen` stay vanilla), the
world-texture copy 0x45D5E0 (its 1/800 is the motion-blur offset; the bridge hooks it), LimitCoordsToScreen 0x420DF0
(particles in the virtual HUD, already aspect-consistent), the load-bar branch without a bar sprite (unused by the
data). `cull_fix` (= `[camera] cull_fix`, CAM_ENTRY + CAM_ASPECT) logs the culling aspect it uses (1.7778 at 16:9).
Checked in game (test copy, 1920x1080): the first screen-relative sprites classified as expected (full-screen
centre (400,300), band (400,38), box), the hook runs after the first frame and on each level load, the mouse probe
outside the fitted entry does not hover (vanilla would), on LOAD GAME it does and ENTER opens the load list.

### Fullscreen sizes and borderless

Why vanilla exclusive fullscreen comes up at 640x480 on a wide desktop (for example 3024x1964 at 150 %): the renderer's mode
list (renderer +18h / count +1Ch; entry {w, h, usable +8, refresh +0Ch, format +10h}) is a fixed 4:3 list built at
0x45EA60.. (640x480 .. 1600x1200); the adapter scan 0x4508F0 only marks those entries usable (a refresh <= 85 Hz in
X8R8G8B8). Fullscreen init 0x45BE30 looks the configpc size up (0x450890, usable required) and falls back to 640x480;
setscreenresolution 0x45BF90 refuses any size not in the list in fullscreen. So vanilla never uses a wide size in
exclusive fullscreen, and the saved option cannot switch to the 16:9 table entries either.

| Site | VA | Was | Now |
|---|---|---|---|
| DS_FS_LOOKUP | 0x45BE42 (fullscreen init) | `call 0x450890` | `FsLookupC` (fastcall = thiscall, ret 0Ch): the original; on a miss an entry of its own for a size the adapter offers (IDirect3D9 renderer +40h: GetAdapterModeCount / EnumAdapterModes, X8R8G8B8; 60 Hz if offered, else the highest <= 85, else the lowest); `fullscreen_any_mode = 1` (default) |
| DS_SR_LOOKUP | 0x45BFCD (setscreenresolution, fullscreen) | same | same |
| DS_BL_INIT | 0x45EBE2 (device setup, fullscreen branch) | `call 0x45BE30` | `BorderlessInitStub` = `jmp 0x45BEF0` (windowed init, same (w, h)); `borderless = 1` |
| DS_BL_WIN1 / DS_BL_WIN2 | 0x45BF08 (windowed init) / 0x45C02C (resolution change, windowed) | `call 0x43B9F0` (AdjustWindowRect + SetWindowPos size) | `BorderlessWinC`: WS_POPUP, no edges, centred on the window's monitor (covering it at the monitor size), HWND_TOP, no activation |

`dpi_aware` (default: on with borderless) calls SetProcessDpiAwarenessContext(per-monitor v2) from the install (the
loader's DllMain, before the game creates its window): sizes are physical pixels, so `screensize = 3024,1964` is the
whole screen and nothing is scaled by Windows.

### Display modes in SETTINGS > SCREEN RESOLUTION (`mode_menu`, default 1; code `dll/src/gp_display_modes.cpp`, logic `gp_display_logic.cpp`)

How the game does it (static analysis in Ghidra): the row is
Trigger_Root_Menu option type 20. A value change runs the settings setter 0x6BA420 case 20 (HUD_intermission data +11Ch
PCScreenSize = index, +360h pending). Enter on the row with PCScreenSize != OldPcScreenSize (+120h; check 0x6CE7C5) sends
command_apply_screenres (global id 1184), thread 0x6D5800: table 0x4AB940 (index -> setscreenresolution 0x45BF90:
window resize, or mode list + device Reset 0x451020), then the countdown text uk_Menu #352 "Screen Resolution has
changed. To keep changes press [Enter]. To Revert changes press [Esc]. Changes will automatically revert in %0 seconds",
then keep (Old = PCScreenSize, 0x6D5E1E) or revert (setter + table with Old, 0x6D5E51 / 0x6D5E95), then
Setting_ScreenSize (settings member 8) = Old (0x6D5EE5). Leaving the page without Enter puts PCScreenSize back (0x6C5780).
The page open reads member 8 into Old and PCScreenSize (0x6CAF7F). The engine also has an unused windowed <-> fullscreen
toggle 0x45C0D0 (flips renderer +310h and present Windowed +198h, then Reset): a live mode switch is a device Reset the
engine was written for. Every Sprite / TextBox keeps its render list (+94h) in pixels and rebuilds it only when its node
is dirty (+14h bit 3, Sprite render 0x8F6CC0): without marking them the menus keep drawing at the old size.

| Site | VA | Was | Now |
|---|---|---|---|
| DS_BL_INIT / DS_MM_WININIT | 0x45EBE2 / 0x45EBF9 (device setup 0x45E620) | `call 0x45BE30` (fullscreen init) / `call 0x45BEF0` (windowed init) with configpc's size | `ModeInitC` (thiscall (w, h) ret 8): the mod's mode (side file, else configpc + `borderless`), validated for the window's monitor; renderer +320h..+32Ch set; the fullscreen or windowed init with that size (exclusive fullscreen that does not start -> borderless) |
| DS_BL_WIN1 / DS_BL_WIN2 | 0x45BF08 / 0x45C02C | `call 0x43B9F0` (AdjustWindowRect + size) | `ModeWinC`: frame (the game's own style, AdjustWindowRectExForDpi) or popup per the mode, centred on the window's monitor (work area / monitor rect), z order kept |
| DS_MM_APPLY / DS_MM_REVERT | 0x6D5CD3 / 0x6D5E95 (apply thread) | `call 0x4AB940` (table[PCScreenSize] / table[Old]) | `ModeTableC` (cdecl (thread, int* index)): the row's mode applied live: present parameters (W, H, format, Windowed, refresh: exclusive sizes through the fullscreen lookup with the any-size fallback), window style / size, `Reset 0x451020`, VirtualHud 0x420D90, every Sprite / TextBox marked dirty, HUD relayout (MasterControl command_get_screen_dimensions 1096, HUD command_reposition_HUD 1088). A failed Reset (+1B0h stays 1) puts the previous parameters back and resets again |
| DS_MM_OLDGET | 0x6CAF7F (settings page open) | `call 0x869E80` (member 8 -> Old / PCScreenSize) | `MGetC`: the original, then the row index of the mode in use (the row's list is rebuilt for the window's monitor) |
| DS_MM_STORE | 0x6D5EE5 (end of the apply thread) | `call 0x869EA0` (member 8 = Old) | `MSetC`: the mode in use is persisted (side file, configpc.txt, registry), Old / PCScreenSize follow it, and member 8 (Slot11) gets the vanilla index 0..4 |
| DS_MM_SETTEXT | 0x60FA50 (6 B: `sub esp,0Ch; push esi; mov esi,ecx`) | TextBox::SetTextIndex(ordinal) | `SetIdxStub`: the original (it clamps an ordinal past the text table and frees the box's own text +B8h), then for the row's value codes 0x7E00+i: the label as the box's own text (game allocator 0x478180 pool 0, freed by the game), condensed to fit the box (+80h width, +C4h/+C8h scale) |
| menu helper type 20 | MN_GET_INIT / MN_GET_RESET | - | the row's StringValueList = the codes of the modes (`gp_menu_set_values`, ValueStrings untouched), CurrentIntegerValue = the mode in use |

REF rows: DS_MM_RESET, DS_MM_TOGGLE, DS_MM_VHUD, DS_MM_ALLOC, DS_MM_THREAD, DS_MM_CONFIRM, DS_MM_TBFREE, DS_MM_TBDRAW, plus
DS_WININIT, DS_WINSIZE, DS_MODEFIND, F_CMD, F_FINDCHILD. Renderer offsets used: +40h IDirect3D9 (GetAdapterModeCount
+18h, EnumAdapterModes +1Ch, GetAdapterDisplayMode +20h, GetAdapterMonitor +3Ch), +178h D3DPRESENT_PARAMETERS, +1B0h
reset pending, +310h fullscreen, +314h format, +320h..+32Ch sizes, +5548h ratio. Exclusive fullscreen is offered only
when the window is on adapter 0's monitor (the device is created on adapter 0). Host test 13b executes SetIdxStub on a
fake text box (labels, other ordinals untouched, registers / stack kept); the live path itself is game-only (G).

## Pad (`[pad]`; code `dll/src/gp_pad.cpp`, logic `gp_pad_logic.cpp`, pictures `gp_glyphs.cpp`)

Off by default (`enabled = 0`: nothing patched, no thread, no XInput load). Every site / offset is a SITE / REF row of `sites.h` (12 PD sites + 29 PD refs; one mismatch and the
whole feature stays off, logged); host test 14 checks the patch set, the "pad present" stub and the fake device.

| Site | VA | Was | Now |
|---|---|---|---|
| PD_SEL_PROC | 0x740A93 (play state 0x740560) | `call 0x4853D0` (is_pad_present: 0 on PC) -> kb/mouse procedure 0x73F7E0 | `PadPresentStub` (thiscall, ret 4): 1 while the pad is the device used last -> the game's gamepad procedure 0x73DC70 (analog run / strafe, right-stick turn and look) |
| PD_SEL_LOOK | 0x742360 | same call (gates the right-stick look block) | same stub |
| PD_SEL_CMD | 0x73F7A7 (`command_use_controller_input:truth` 0x73F780) | same call | same stub, only with `tell_scripts = 1` (default 0: the menus keep the PC help, whose key names become pad pictures) |
| PD_CTLSTART | 0x925F17 (imm32 of the mov at 0x925F16, ControlSetup registration 0x925BA0) | `mov eax, 0x926280` (handler of event `start`: clear the table, walk the setup) | `CtlStartWrap`: the original, then for the setup named `PC` the child folders of its sibling `XBox` (except `cheats`) are walked with the game's own Start_Impl 0x925A60: each action gets the console pad binding next to its keys (new names such as look_* / target_object / menu_analog_* are appended, existing indices unchanged); `[pad_bind]` then rewrites the pad Control nodes' code / type (+50h / +58h); defaults `menu_show` = START, `pause` = none (`start_opens_menu = 1`) |
| PD_TXTPARAM | 0x8FFE6F (settextslotparamfromtextslot 0x8FFE60) | `call 0x8FD870` | `TxtParamWrap`: the original, then (TextBox, param) -> ordinal when the slot is the Keyboard slot (cache +2D8h) |
| PD_EXPAND1/2/3 | 0x8FDFB9 / 0x8FE22D / 0x8FEB8E (box size, string width, render) | `call 0x8FD410` (expander: `\n`, `%N`) | `ExpandWrap`: in a pad style the remembered key parameters are swapped for one pad glyph each for the call: the glyph of the ACTION the text means (per-text hints `gp_pad_prompts.cpp`, keyed by the box's TextSlot node name + ordinal, read through PD_TB_TEXTID / PD_TB_SLOTIDX: TextBox +B8h literal text, +B4h TextSlot, flags +D8h bit 12 list / bit 9 current_index (+54h) / bits 16..26 index), among the actions bound to that key; no hint = the key's first action with a pad binding; then `post_process` (brackets around glyphs dropped; `[Enter]` / `[Esc]` / `[A]` / `[Space]` and their 4 translations -> the pad buttons of menu_select / menu_back (menu_show in the death-screen and "enter the menu" texts) / menu_delete / tutorial_skip, A / B / Y / A when unbound); the box's font gets the style's glyph records and the box flag bit 13 (glyphs keep their colours) |
| PD_VIBRATE | 0x924F76 (SetVibration_Impl 0x924F40) | `call 0x439B10` | `VibrateWrap`: log `rumble: game set_vibration(motor, force)`, then the original (`rumble_log = 1`) |
| PD_SHOT | 0x834760 (9 B, weapon_effects spends a round) | `sub esp,10h; mov ecx,[0xA3B58C]` | `ShotStub`: counts the shot when the current node's data +94h (pOwner) is Ram, then the two instructions, back to 0x834769 (`rumble_events = 1`) |
| PD_USE_VEH / PD_USE_ON | 0x60D81B (8 B, Mission_HUD vehicle use prompt 0x60D7D0) / 0x60D646 (7 B, use prompt 0x60D630) | `mov eax,[esp+18h]; mov esi,[esp+14h]` / `mov eax,[edi+34h]; mov esi,[esp+14h]`, just before data +38h / +24h = 1 | `UseVehStub` / `UseOnStub`: note edi (the HUD script data: +54h PC_Use_TB, REF PD_USE_TB 0x60D6E9), then the two instructions, back to 0x60D823 / 0x60D64D. ExpandWrap gives the use prompt (Misc_PC #86) use_vehicle (TRIANGLE) while +38h is set, else use (CROSS); only in a pad style |

Data written at run time (only with `enabled = 1`): Gamepad 0 ([0xA35E7C][0]) +0 = the DLL's fake IDirectInputDevice8A
(GetDeviceState hands the merged pad as DIJOYSTATE; the original device pointer, if any, is kept in the log and no
longer polled), +B4h type 2 (no SmartJoy remap: rgbButtons index = the game's button), +B8h 0, +CCh 0 (no DirectInput
force feedback), [0xA08FD0] (pads found) at least 1. Font glyph records of the 18 code points of `pad::glyph_code`
(ODPC / ODPS2: the console button glyphs, used only by console texts) -> U / V / width of the DLL's cells in rows
288..415 of the 512x512 atlas (Font +3Ch & ~3 = the GfxTexture record, its +4 & ~3 the Direct3D texture, checked:
512x512 A8R8G8B8, vtable inside a loaded module) (painted once per font with LockRect; keyboard style restores the 32-byte records).
Read only: Gamepad +D8h / +DCh (motors), settings member 3 (VIBRATION), Ram's MoveCtrl +6Ch (MoveState), the
GameCamera MoveCtrl +30h..+44h (shake), the PC ControlSetup table.

## Physics (`[physics]`; code `physics/code/`, docs `physics/README.md`; Box3D in `TODPhysics.dll`)

Off unless `[physics] enabled = 1`; then it needs POST_INPUT + KB_PROC / PAD_PROC (the input tick), every `PH_*` REF
row (27 + PH_SHELLS) and `TODPhysics.dll` next to TOD.exe (API version 1), else it stays off (logged). Each of the three
call sites is optional (`bullets = 0` / `explosions = 0` leave them alone; a site with other bytes is skipped and
logged). Verification: S, H (host test 15: off = 0 patches, on = the 3 sites + input tick + fixes; the three wrappers
called with the originals redirected: `ret 0Ch`, result and ebx/esi/edi kept; cdecl arguments passed), U
(`test_phys.exe`, 176 checks incl. the Box3D world as 32-bit), G (test copy runs, `physics/README.md`).

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there | Verified |
|---|---|---|---|---|---|
| PH_BULLET | 0x8C95A4 | `8bcfe857dafeff` (0x8C95A2) | call retarget | bullet step `call 0x8B7000` (IsLineColliding thiscall(probe, -1, from, to), ret 0Ch) -> `PhLineWrap` (__fastcall, same stack): the original, then a Box3D ray along the segment against the prop bodies; a hit pushes the body when the game's hit node (probe +80h) is that prop or the game hit nothing | S H X, G (18 and 26 hits in two test runs) |
| PH_SHELL | 0x848BD2 | `893d5cb7a300e8d9d4ffff` (0x848BCC) | call retarget | weapon_shell command_explode: `call 0x8460B0` (the blast, cdecl(thread, &result), [0xA3B75C] = the shell) -> `PhShellWrap`: the shell's position, the original, then a Box3D explosion there | S H X, G |
| PH_DESTROY | 0x67145B | `6a0053893d5cb7a300e84077ffff` (0x671452) | call retarget | Destroyable_Object command_destroy_object: `call 0x668BA0` (the explosion, cdecl(thread, 0), [0xA3B75C] = the object) -> `PhDestroyWrap`: the same for red barrels, gas pumps ... | S H X (not met in game: no explosive destroyable near the test spot) |

Game functions called (all thiscall, SEH-guarded): 0x4842C0 GetWorldMatrix(node, float[16]), 0x88D6C0 SetPos(local
float[3]), 0x88DB20 SetOrient(stored quaternion = conjugate of the true rotation; REF PH_SETROTY_Q shows SetRotationY
building {0, sin(-y/2), 0, cos(-y/2)}), 0x884860 Model::GetPivotCollision(i), 0x884350 model name (a shared buffer:
copied at once), 0x88CA80 GetWorldPosition; test copy only: 0x88E600 SetWorldPosition (props of the rig, the debug
shell), Node::SendMessage 0x869E60 (weapon_shell -11 morph, -12 aim, -9 explode as the Suave dynamite).
Memory read: Scene [0xA3DCBC] tree (+44h first child, +34h next sibling), Node +28h flags (`(f | f >> 16) & 1` = no
collision), +3Ch NodeMatrix (+60h owner), +40h parent, +4Ch name, +14h entity id word; Model +50h asset, +64h / +A0h
pivot nodes; asset +34h pivot count, +54h HullList (& ~3; +0 element pointers, +4 count; element +0 shape); CollisionList
+0 shape pointers (& ~3), +4 count, +10h flag byte (Solid = 1); shape +24h surface word (bit 10 Solid); VertexCloud
(vtable 0x9D05A0) +2Dh vertex count, +30h vertices (& ~3); OrientedBox (0x9D02CC) +28h size, +34h centre, +40h rotation;
CollisionBox (0x9D02F8) +50h list; entity registry AssetManager [0xA3D7C4] +130h + k*10h {data, count}; game dt
[0xA3DCC4]; test copy only: cache +550h shell pool (REF PH_SHELLS), +260h GameCamera. Nothing is written except the
props' transforms through SetPos / SetOrient (and, on the test copy, the rig's positions, the debug shell's owner word
+24h and the mouse object's button dword 0 / active byte +48h while `debug_fire_key` is held).

**In a vehicle (`drive_step = 1`).** The game runs no on-foot input tick (KB_PROC 0x740B69 / PAD_PROC 0x740AA6)
while Ram drives, so without this the Box3D world would stand still in a car (props, wrecks and ragdolls frozen). No new patch:
POST_INPUT (0x43C511, every main-loop pass) calls `drive_frame`, which steps the world itself when the on-foot tick did not
run in the last pass and Ram is in a vehicle. Paced by the game-ms clock [0xA3DCD4] (REF PS_GAMEMS: still while paused or
loading, back on a rewind; a jump over 1000 ms rebases), with the game dt [0xA3DCC4] for the fixed 0.04 s steps; nothing
while Scene +5Ch playmode is 2 (paused), a rewind (3) is handled as on foot, and nothing in a cut-scene (`gp_cutscene`,
REF G_CUTSCENE). "In a vehicle" = Ram's character_root data +58h is the player_ai (class handler 0x5ACE10, REF
SV_CLS_PLAYERAI / SM_ROOT_AI), its data +24h holds the vehicle node (written by command_drive_vehicle 0x5B5B10 at 0x5B5B4C,
member 4, REF **PH_PAI_VEHICLE** 0x5B5B46 `8b44241c8b00894624`; the forced exit 0x5B6900 clears it), and that node (or a
parent via +40h, up to 4) is a vehicle_root (0x824A70, data +4Ch = vehicle_ai, REF CP_CLS_VROOT / CP_VROOT_AI) or a
vehicle_ai (0x803A50, REF CP_CLS_VAI) whose ControlState data +18h is 1 (the player's). Any missing REF leaves the drive off
(logged in the "Box3D loaded" line). While driving, a scan is centred `drive_lead_seconds` of the car's smoothed speed ahead
(at most 0.4 x radius). Verification: S (the REFs), U (`test_drive` in test_phys.exe: the clock gate, the decisions, the
scan point), G (driving test runs).

## Car physics (`[car_physics]`; code `physics/code/car_*.cpp`, `phys_car_sim.cpp`; docs `physics/CARS.md`)

No patch site of its own: it runs inside `[physics]` (needs `[physics] enabled = 1`), called from phys_game.cpp's scan,
tick, blast wrappers (PH_SHELL / PH_DESTROY via explode_at) and bullet wrapper (PH_BULLET). Off unless
`[car_physics] enabled = 1`, every `CP_*` REF row (26) plus the PH_ rows it shares match, and TODPhysics.dll exports
`TODPhysics_GetCarApi` (version 1). Verification: S (check_sites: the CP_ rows), U (`test_car.exe`), G (test copy runs,
`physics/CARS.md`). Host test unchanged (no new site; [physics] on = the same 8 patches).

Game functions called (thiscall, SEH-guarded): RigidBody (the car_move_ctrl node) GetLinearVelocity 0x4CEEF0(out[4]),
SetLinearVelocity 0x500C20(v[4]), GetAngularVelocity 0x932530(out[4]), SetAngularVelocity 0x932560(w[4]), SetPositionT
0x4CF040(p[4]), SetOrientationT 0x4CEFD0(stored q[4]) (all ret 4, each writes its own rewind record); script member
rewind record 0x86B4B0(node, member index) before every member write; Node::SendMessage 0x869E60 with global commands
377 `command_activate_without_first_frame` (car_move_ctrl idle state 0x4EC0B0 -> 0x4F9C30) and 382
`command_traffic_accident` (vehicle_ai track states); plus the [physics] node functions (GetWorldMatrix, SetPos,
SetOrient, GetPivotCollision, model name, GetWorldPosition; test copy: SetWorldPosition).
Memory read: script class main handler (node +4 -> +28h -> +60h: car_move_ctrl 0x4DB730, vehicle_root 0x824A70,
vehicle_ai 0x803A50); car_move_ctrl data (+18h): in_control +4Ch, max_engine_force +50h, engine_force +54h,
spinning_angular / linear_friction +80h / +84h, tumbling +D8h, Mass +168h, key_accelerate +384h, accelerate_number
+388h, l_friction / a_friction +3BCh / +3C0h, control_state +470h; RigidBody coll mass +50h, Position_t +ECh;
vehicle_root data: movectrl +48h, AICtrl +4Ch; vehicle_ai data: ControlState +18h, Health +B0h, DamageTaken +B4h,
Destroyed +C8h. Written (always after the rewind record): tumbling, a_friction, l_friction, in_control, the RigidBody
velocities and pose (blasts, wrecks), the vehicle_root transform (wrecks, through SetPos / SetOrient); test copy only:
key_accelerate, accelerate_number, engine_force, the rig's car pose.

## Menus (`dll/src/gp_menu.cpp`; shared helper for the game's option menus)

Installed only when a feature registered a handler for an option type (`[difficulty]`, type 3; `[display]` mode_menu, type 20).

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there | Verified |
|---|---|---|---|---|---|
| MN_GET_INIT | 0x7FF75C | `6a0053893d5cb7a300e8bfecffff` (0x7FF753) | call retarget | menu entry initialize_local 0x7FF5F0: `call 0x7FE420` (CurrentIntegerValue = the settings member of the entry's OptionValueType1) -> MenuGetWrap: the original, then the type's handler with the entry ([0xA3B75C] at the call) | S H X, G |
| MN_GET_RESET | 0x7FE7C8 | `6a0056e853fcffff` (0x7FE7C5) | call retarget | command_reset_data 0x7FE7C0 (every page open): same call -> MenuGetWrap | S H X, G |
| MN_SET | 0x6BA420 | `83ec28a18cb5a300` | jmp hook 8 | settings setter entry (thread, int* {OptionValueType1, index}; callers 0x6C4B80 / 0x6C5448 / 0x6C5C14 / 0x6D5E51) -> MenuSetStub: pushad, the type's handler on {type, index}, popad, `sub esp,28h; mov eax,[0xA3B58C]`, back to 0x6BA428 | S H X, G |

REF rows MN_GETTER (data +20h type, +4Ch current), MN_CURRENT, MN_LISTBUILD (+58h StringValueList, list(integer) type
[0xA3BB04] vtable +44h op 3 / 9), MN_VS4 (+34h ValueString4), MN_LISTREAD (element = [[list+4]+4] + stride x i x 4),
MN_SETPARAM, MN_SETDIFF (case 3: settings member 1 = index). Host test 16 executes MenuSetStub (registers, stack, eax =
[0xA3B58C] for the original) and MenuGetWrap on a fake entry / list type.

## Difficulty (`[difficulty]`; code `dll/src/gp_difficulty.cpp`, logic `gp_difficulty_logic.cpp`)

No patch site of its own: the three MN_* sites (menu), the input tick (KB_PROC / PAD_PROC + POST_INPUT) for the tuning
and the reinforcements. Every game read SEH-guarded; the script current-node globals 0xA3B75C/60/64 saved and restored
around every game call.

| What | Game code / data (REF rows) | How |
|---|---|---|
| BRUTAL / PSYCHO in the menu | type 3 handler (MN_*) | on_get: OptionValueType4 / 5 (+38h / +40h, never read by the code) hold the text ordinals from the data option `difficulty_menu` (77 / 101); appended once with gp_menu_append_value; shows 2 + extra while Setting_difficulty = 2. on_set: index >= 3 -> 2 + extra (index - 2), 0..2 -> extra 0 |
| Setting_difficulty | DF_SETTINGS 0x5B7379 (cache data +398h, member 1), DF_GET 0x869E80 (member get thiscall(node, slot, out), ret 8) | read every 0.5 s in the input tick |
| Extra level | `<Documents>\Total Overdose\savegames\TODGameplay_difficulty.txt` (SHGetFolderPathA CSIDL_PERSONAL, as the game finds Slot11) | written when the menu choice changes it |
| Tuning | DF_DMGHARD 0x5B73F3 (PLAYER_STATUS cache +2B0h, data +98h DamageHard; +90h/+94h Easy/Normal), DF_DMGMOD 0x5E445B (+9Ch PlayerDamageMod), DF_BULLETDROP 0x666637 (MASTER_BULLET_DROP cache +2C0h, data +0Ch..+30h), DF_CHARLIST 0x5D44AD (DB_CHARACTERDICT_LIST cache +3A8h, indexed by AISubType = Specific_ID +10h; Wait_time_from/to +2Ch/+30h), DF_CLS_WSUB 0x6587E8 (database_weapon_subtype handler 0x6580F0; DB_WEAPONDICT_LIST cache +3BCh; Specific_ID +1Ch, SpreadEnemy_m +C0h, Min/MaxPauseTime +150h/+154h), DF_CLS_MSTAT 0x649CB8 (database_model_stats 0x649BB0; DB_MODELSTAT_LIST cache +470h; Model_type +0Ch, Wait_time +4Ch/+50h) | captured once after checking known vanilla values (DamageEasy 0.65, Normal 1.1, Hard 1.4, PlayerDamageMod 1.0, VeryFewDropFrom 10, VeryManyDropTo 250, weapon 6 = 1.4 / 0.8 / 2.0, behaviour row 7 = 1 / 3); a baked static level is refused; written without the member-write notify (block 1 stays loaded across save loads; no save records them) |
| Reinforcements | character query (SV_QUERY_A: mask 40h mode 0) -> root data +24h Placeholder, its data +14h Resource == the root (DF_PH_RES); placeholder query (DF_PH_QUERY: mask 80h mode 1) for diagnostics; DF_CLS_PH 0x5AAA0B (handler 0x5A3100); SV_CLS_ROOT / SM_ROOT_AI / SV_CLS_SENTRY (root +14h Sub_Type, +18h FactionID; sentry +2Ch Health, +B8h Is_Buddy, +1D4h state); DF_SPMASK_GET / DF_SPMASK_SET 0x48F390 / 0x48F3A0 (NodeSpatial type mask, notify slot 6 = saved); DF_PH_KILLMASK 0x5A8ABB (the kill clears mask 80h); SV_SHOW_FN / F_SETFLAGS (show = SetFlags(flags & FFEh)); DF_PH_STATUS / DF_PH_LOCKED (Status / Locked setters) | measured on the test copy: a live enemy's placeholder has Status 2, mask 1000h and is not in the spatial tree; after the death Status 0, Resource 0, mask 1000h (80h cleared by the kill) and node flag 1 (hidden): the collect never picks it again. Re-arm = the game's own setters: Status 0 / Locked 0 if needed, mask |= 80h, SetFlags(flags & FFEh). The resource collect then gives it a pooled character within 0.5 s, at the placement, armed as authored. Every 0.5 s on BRUTAL/PSYCHO; a death adds share 0.5; one re-arm per whole unit, 4 real s after the death, Ram 18..45 m from the placement, never twice; candidates = seen alive, not buddy / civilian / Ram / 1000 hp / skip factions 6, 10, 13, 14. Forgets everything on a mission change, a game clock jump back (load / retry) and a change of the loaded submap (Scene +1B4h) |
| Reinforcement damage | DF_SETPROP 0x86A6C0 (Node::SetProperty thiscall(raw id, value*), ret 8), F_GETPROP 0x86A6A0, F_TOUCH 0x86B4B0 | a dead placement keeps its last enemy's DamageTaken in placeholder property 0x287; the AI init 0x5D4190 copies it into the new enemy's sentry +30h, so without this every re-armed reinforcement would be born dead. `rearm()` sets 0x287 to 0 through the property's own setter before the spawn; on arrival a reinforcement with DamageTaken >= Health is cleared with the member write (touch slot 3, sentry +30h) |
| Debug (test copy only) | DF_SENDCMD 0x86A340 (Node::SendCommand by global id) | `debug_kill_seconds`: command_goto_death (1537) to the nearest live enemy's AICtrl within 60 m; `debug_no_damage`: DamageHard 0; `debug_focus`: EntryFocus (HUD_intermission +CCh) on the DIFFICULTY row at page open (CurrentEntryList +B4h); `log_verbose`: diagnostics of characters, placeholders and node / spatial fields |
| Mission safety (reinforcements) | CharacterResourceCtrl (MasterCtrlAux/ResourceCtrl/CharacterResourceCtrl via F_FINDCHILD; MasterCtrlAux = BulletDrop's parent) data +18h MaxAllocated, +4Ch HasResourcePHList; placeholder data +24h CallbackTrigger_List (Trigger_Condition TrueOn 3 registrations), condition data +44h IsMet | a re-arm waits while HasResourcePHList + `reinforce_keep_slots` (2) >= MaxAllocated (the mission's own placements first); a placement with a registered, not-yet-met death condition is never re-armed (its count root would have to wait for a different death) |
| Live-enemy cap | DF_RC_MAX6 0x763694 / DF_RC_MAX8 0x7636A6 (FUN_00763440 writes 6 or 8 at init and every command_new_block_loaded) | `max_live_enemies` (BRUTAL / PSYCHO only, up to 9 = the 9 AICharacter shells of DefaultEnemyResources): MaxAllocated written back every 0.5 s when the game reset it, the game's value restored when the level drops; no notify (never saved) |

## Mayhem title (`[mayhem_title]`; code `dll/src/gp_mayhem.cpp`, logic `gp_mayhem_logic.cpp`, shaders `gp_mayhem_ps.hlsl` / `gp_mayhem_vs.hlsl`, art `../mayhem_title/`)

MAYHEM burns through the title screen ("PRESS ENTER TO START", `OverdoseIntro.scene` node
`DataBase/StartScreenfolder/StartScreen`) under the TOTAL OVERDOSE logo, and into the main menu's header band after the
caption "TOTAL OVERDOSE". Off unless `enabled = 1`; then 4 sites (3 with `main_menu = 0`), all-or-nothing after 15
REF rows. No data change; nothing in game memory is written except the patched places.

How the engine draws a 2D widget (static research for this feature): every Sprite / TextBox owns a retained render list
(+94h, command buffer {capacity +18h, buffer +1Ch, count +20h}); its per-frame method (vtable slot +20h: Sprite
0x8F6CC0, TextBox 0x8FE280) rebuilds or patches the list and submits it to its layer (0x436040: +14h |= 20h, linked
through +58h). GfxInternal::Render 0x421B30 -> layer renderer 0x421530 executes each submitted list (0x434290 ->
interpreter 0x4342C0; quad opcodes 32h / 33h -> 0x452260; full-screen picture opcode 0Fh -> 0x45D940). The title picture
is NOT a quad: its texture record has +24h bit 9, so the sprite emits `0F {record}` and 0x45D940 draws it at the full
back-buffer width, 600/800 of that high, centred (1920x1080: (0, -180)-(1920, 1260), UV (0, 0)-(0.625, 1); 1600x1200: the
whole window); it does not go through `[display]`, so vanilla and `[display]` users see the same place.

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there | Verified |
|---|---|---|---|---|---|
| MT_SPRITE_VT | 0x9D2F70 (.rdata) | `c06c8f00` (= 0x8F6CC0) | data pointer | Sprite vtable slot +20h -> `SpriteWrap` (thiscall, ret): the original, then the node named `StartScreen` under `StartScreenfolder` (+4Ch name, +40h parent) remembers the list it submitted this frame | S H, G |
| MT_TEXTBOX_VT | 0x9D3618 (.rdata) | `80e28f00` (= 0x8FE280) | data pointer | TextBox vtable slot +20h -> `TextBoxWrap`: the original, then `Caption_Yellow` under `GfxFolder` (the menu header caption) showing "TOTAL OVERDOSE" (+B8h text) remembers its list (`main_menu = 1` only) | S H, G |
| MT_EXEC_A | 0x421AB3 | `8b0caee8d8270100` (0x421AB0) | call retarget | layer renderer, sorted layers: `call 0x434290` -> `ExecWrap` (thiscall, ret): the original, then, right after a remembered list ran, the MAYHEM pass | S H, G |
| MT_EXEC_B | 0x421AE2 | `8bcde8a9270100` (0x421AE0) | call retarget | the same call for linked layers | S H, G |

REF rows: MT_DEVICE (renderer +0 = device), MT_EXEC_CLR / MT_SUBMIT (the submitted bit), MT_SPR_LIST (+94h list),
MT_SPR_IDX / MT_QUAD / MT_SPR_UV (a sprite quad in its list), MT_SPR_FULL / MT_OP_F / MT_FULLSCREEN / MT_FS_UV (the
full-screen picture path and its UV), DS_SCREEN (back buffer +328h / +32Ch), MT_TB_TEXT (TextBox +B8h text),
PD_NODE_NAME, PH_NODE_PARENT.

Placement: title = a rectangle in title-art pixels (`place_default` + `scale` / `offset_*`) mapped with the picture's
own place; menu = right after the caption's glyph quads (their bounding box, read from the caption's list), 2.1 x the
caption height (`menu_scale`), so the `[display]` menu fit is followed automatically.

Drawing (inside ExecWrap): one quad over the effect region E with the vs_3_0 / ps_3_0 shaders (embedded, compiled by
build.bat with fxc), premultiplied blending (ONE, INVSRCALPHA), samplers: lettering (A8R8G8B8, mips), fx (hole field,
flame band, ember cracks, burn noise), a 256x256 noise (wrap), the fire flipbook (DXT1 8x8 atlas, 48 frames at 12 fps);
then the sparks with fixed function (pre-transformed dots). Per pixel (see the shader): burn time = distance from the
ignition + fBm noise; behind the front: hot rim, its bloom, char with dying ember specks, MAYHEM dark -> glowing -> its
colours, heat shimmer on the flames, the fire loop over the flame tips, flickering cracks; ahead of it: curled-back
paper, the fire's glow, scorch; the front stops at the hole field's edge (the hole hugs the word) and settles to a
calm charred rim. Every render state, texture-stage state, sampler state 0..3, texture 0..3, FVF / declaration,
shaders, VS constant c0, PS constants c0..c6 and stream 0 it touches is read before and written back after; a pure
device or a card without shader model 3 turns it off (logged). Managed textures (they survive a Reset); no state blocks.
Any fault inside the draw turns it off for the session (logged).
Interplay: the renderer bridge hooks 0x421530's and 0x434290's entries (not these call sites; ExecWrap calls 0x434290,
so the bridge's hook still runs); TOD HD's d3d9.dll wraps the device the overlay draws through.


## PSYCHO enemy loco moves (`[psycho]`; code `psycho/code/`, docs `psycho/README.md`)

Off unless `[psycho] enemy_loco_moves = 1`, and inert on every level but PSYCHO (`difficulty_enemy_loco_moves()`).
**No code site of its own**: it runs in the input tick (KB_PROC / PAD_PROC, script context, player in control) and the
post-input hook (debug keys only) that every tick feature shares. One mismatch among the REF rows below and the feature
stays off (logged). Verification letters as above, plus **P** = `test_psycho.exe` (pure logic: pick, cooldowns, the two
timelines, interrupt, dodge / cover / range, charge placement) and host test 18 (off = no patch; on = only the shared
tick sites + fixes; an unknown `moves` list refuses).

Game calls (every one has the game's own precedent on Ram; the enemy is the receiver instead):

| What | Call | Precedent (REF) |
|---|---|---|
| clips on the enemy | CallNative(MoveCtrl, 5Eh `motion`, {slot, -1}); slots = cache +73Ch anim_ram_loco_move, +740h anim_ram_move_idle, +744h anim_ram_move_run (fallback: AnimDict PlayerAnims/specials/...) | PS_ANIM_LOCO 0x5358E2 (Tornado), PS_ANIM_TORO_IDLE 0x5268F1, PS_ANIM_TORO_RUN 0x525767 (El Toro), SV_MOTION |
| clip speed | AnimLayer speed setter 0x5010A0 on the MoveCtrl (telegraph 0.76, firing 0.4, then back to its own) | F_LAYERSPEED |
| the enemy parked / released | MoveCtrl -36 Request_new_State {3, 22} (Victim controller) / {0, 0} + -81 command_idle; sentry -23 no_ai_control / -9 regain_control (KnockedOutTimer +264h = 0 first) | SV_BEATING (EngageBeating 0x562BE4), H_NO_AI |
| burst stopped | default_weapon -12 command_stop_firing on the gun in hand (weapon ctrl data +20h) | SV_CLS_WEAPON, SV_GUN_DROP |
| blank shots | GetProperty(gun, 6C0h EffectCtrl), SendCommand(1266 command_fire_blank_shot, {0}) | PS_BLANK 0x535C85 (Tornado) |
| telegraph particles | cache +3F0h DB_PARTICLEDICT_LIST[id] (30 PickupPowerups_Trigger_Red, 180 Goodie_Glow_Orange), SendCommand(247 Trigger(entity), {chest bone}); the glow ends with StopEmitters(entity) 0x8EB580 (or KillEmitters 0x8EB540) called thiscall(template, &entity) | PS_PARTLIST 0x5B76AB, PS_PARTTRIG 0x5B76D8 (player take_a_hit's Blood_Dripping), PS_STOPEMIT, PS_KILLEMIT |
| the sting | DB_SOUNDDICT_LIST[107] (Loco_Move_Serious) -> AmbientSoundCtrl -15 Play_Speak {slot, 0} | PS_STING 0x53465E (Tornado start), SV_MUSIC_B |
| the hit | character_root -2 take_a_hit {damage, owner = the enemy root, inflictor = its gun, Ram's torso_cs, impulse, Ram's position + 1 m, 1} to RAM's root: the relay 0x5C4F40 scales by the owner's Damage_mod, the player AI 0x5B70B0 by the difficulty factor (god mode respected) | H_ROOT_HIT, SM_ROOT_AI |
| the charge | Node::SetWorldPosition 0x88E600 (only when the enemy's own motion lags), SetRotationY 0x891360 (measured local -> world, as Suave), character_physic_ctrl -17 / -11 collision off / on | F_SETPOS, F_SETROTY |
| line tests | Navigator::IsPointOccluded 0x91D020 (Suave's probe): chest to chest before a start and at each Tornado hit tick; 1.2 m ahead at knee / chest height during the charge | F_OCCLUDED, SV_OCCL_IGNORE, SV_OCCL_MASK |
| who is near / hostile | quadtree query 0x486950 (mask 40h, 20 m) + list release; FactionID + relations row 0 == 4 | SV_QUERY_A / SV_QUERY_FREE, SV_FACTION_A..C |

Read only: sentry Health +2Ch / DamageTaken +30h / Is_Buddy +B8h / Current_state +1D4h (SM_SLOT_02/03, SM_RD_STATE),
Ram's MoveState +6Ch (PD_MOVESTATE: shoot-dodges 1Bh..20h, 24h), player AI +1Ch Health / +20h DamageTaken
(PS_RAM_HEALTH, for the log), power-up data +20h RunningLocoMove (PS_RUNNING_MOVE: no enemy move during Ram's own),
cache +864h ExclusiveMode (G_CACHE_EXC), Scene +114h TimeMultiplier (G_SCENE_TM), the game-ms clock cache +274h
(SM_CLOCK). Written without a message: sentry +264h KnockedOutTimer (cleared before regain, as Suave); test copy only
(`debug_heal_below`): player AI +20h DamageTaken after the game's touch(slot 3).

### Elite drops (`[psycho] elite_drops`, `psycho/code/psycho_drops.inl`)

No code site either: the same input tick. The REF rows `PS_RES_*`, `PS_PROP_*`, `PS_SHOW`, `PS_SETFLAGS`, `PS_POWERUP_COUNT` are
checked at install; one mismatch turns the drops off and leaves the moves on. Message ids from the game's class
tables: master_control_resource_ctrl -5 command_resource_deprecated(Entity), -10
command_free_resource(Entity), -13 command_request_resource(integer); prop_root -4 command_reset.

| What | Call | Precedent (REF) |
|---|---|---|
| a free goodie prop | ResourceCtrl (MASTER_CTRL data +40h) -13 request(5), result data +0Ch / +10h | PS_RES_REQUEST |
| taken | ResourceStatus data +38h = **2** with the touch (slot 12) | PS_RES_DANGLING: the pool frees status 1 with no placeholder; nothing in the game uses 2 |
| shown | Node vtable +10h SetFlags(flags +28h & FFEh), refused while holder data +88h is set | PS_SHOW, PS_SETFLAGS |
| what it is | data +5Ch = goodie 10 / 11 with the touch (slot 15h), prop_root -4 command_reset | PS_PROP_TAKEOVER |
| where | the Spatial set position (F_SETPOS) | F_SETPOS |
| how long | prop AI data +4 AutoRemoveTimer with the touch (slot 0), after the reset; the DLL frees it at the end anyway | PS_PROP_TIMER, PS_PROP_TIMER0 |
| end | ResourceCtrl -10 free (any status: hides, reset_to_default, TARGET_DB) | PS_RES_DANGLING |
| empty pool | placeholder cmd 2ABh command_giveup_resource, ResourceCtrl -10, on the level goodie furthest from Ram | PS_RES_GIVEUP |
| Ram's count (log, walk helper) | power-up ctrl data +14h -> {counts, maxima} | PS_POWERUP_COUNT |
| the pool walk (sweep) | ResourceList data +14h [5], list types [0xA3BDF0], [0xA3BCE4] | PS_RES_LIST, PS_RES_LIST2 |

From the show on every part of the pickup is the game's: `Goodie_Default` reset (model, glow), prop_ai idle, Ram's trigger
query, activate_goodie, HUD_pickup_effect. Test-only keys: `debug_drop_key`, `debug_drop_walk`, `debug_drop_walk_wait`,
`debug_drop_trace`, `debug_rewinds`, `debug_ram_dies`, `debug_ram_dies_on`.

## Gang wars (`[gang_wars]`; code `gang_wars/code/`, docs `gang_wars/README.md`)

Off unless `[gang_wars] enabled = 1`. **No site of its own**: it runs in the shared input tick (POST_INPUT + KB_PROC /
PAD_PROC) and needs every row of `GW_REFS` in `gw_game.cpp` (the `GW_*` rows below plus G_KILLS_R, G_MISSION,
SM_CLOCK, F_TOUCH, F_GETPOS, F_GETPROP, F_QUERY, F_LISTFREE, SV_QUERY_A, SV_QUERY_FREE, SV_CLASSOBJ, SV_CLS_SENTRY,
SV_CLS_ROOT, SV_CAM_RELEASE, SM_ROOT_AI, SM_RD_ROOT, SM_RD_FACTION, SM_RD_STATE, SM_RD_COMBAT, SM_SLOT_01/02/03/25, GW_ALARM, F_CMD, F_L2W);
one mismatch and the feature stays off (logged). Host test 18 installs it on the mapped image (no own site patched, every
reference matches, ini read); `test_gw.exe` tests the pure logic (101 checks: gangs, pairs, spare allocation, the
matrix design against the shipped 19 x 19 table, war / calm / idle / provoke / spark / release; 105 checks).

| Id | Address | What it proves |
| GW_GETREL | 0x7B76BD | FactionRelations = FactionTable data +4; rows: list +4 -> +4, stride [0xA3B88C]+24h; cells: row list +4 -> +4, stride [0xA3BB04]+24h |
| GW_BASECOUNT | 0x7B782D | [[0xA3B590]+18h]+258h = 19 base factions; get_modified_faction(f) = f + 19 |
| GW_TOUCHCACHE | 0x7B6030 | TargetDatabase touch_cache = data +14h cache_valid := 0 (the DLL does the same after a change) |
| GW_TDB / GW_TDB_QUERY | 0x7B1B83 / 0x7B1BA5 | TARGET_DB = cache +26Ch; the vision query is {1, the looker's root FactionID, 4 hostile} |
| GW_HIT_REL | 0x5E3C8B | take_a_hit: relation[victim faction][shooter faction]; 8 -> friend, 1 friendly fire, 2 neutral -> the provoke rule (modified faction hostile to the shooter) |
| GW_KILLROW0 | 0x5D6F3B | death state: FactionTable = cache +270h; an enemy kill when relation[0][victim faction] == 4, whoever killed |
| GW_MODEL / GW_MODELIDX | 0x5D4B61 / 0x5D4F84 | sentry data +D4h = Model (indexes DB_MODELSTAT_LIST) |
| GW_PH | 0x5E7C09 | sentry data +D0h = My_Placeholder |
| GW_KILLOWNER | 0x5E4E7D | lethal hit: KillOwner (+178h) = Take_a_hit_owner (+14Ch) |
| GW_ALARM | 0x5E8C0A | sentry registration: alarm(Entity) (378) handled in the passive state 0x5DC360 by 0x5E7CA0 (Alarm_pos, GOTO 13) |

Game memory written (nothing else):

| What | Where | How |
| a gang member's faction | character_root data +18h (slot 2) | `touch(root, 2)` then the store, as the game's own provoke rule (0x5E3580) does; back to the placement's faction when calm, when paused, when no longer eligible |
| the spare factions' relations | FactionRelations cells of rows / columns `spare_factions` and spare + 19 | in memory only; restored to the values captured before the first write when the spare is not in use and while paused |
| TargetDatabase cache_valid | TARGET_DB data +14h := 0 | after every change (the game's touch_cache) |
| (message) alarm(Entity) 378 | to an idling member's AICtrl, with the rival's AICtrl | Node::SendCommand (F_CMD), only in passive states 4..8, once when its war starts (`wake`) |

Placeholder properties read with Node::GetProperty (F_GETPROP): FactionID 0x2A8, IsVIP 642.


## Overlays and the minimap (`dll/src/gp_overlay.cpp`, `gp_hud.cpp`, `gp_hud_tiles.cpp`; docs `../hud/README.md`)

**Shared overlay hooks.** MT_SPRITE_VT / MT_EXEC_A / MT_EXEC_B (table above) belong to
`gp_overlay.cpp`: features register clients (`gp_overlay_add_sprite(match, post, draw, who)`,
`gp_overlay_add_frame(draw, who)`) and `gp_overlay_install()` (called once by gp_game.cpp after every feature registered)
patches the sprite sites when a sprite client exists and OV_FRAME when a frame client exists. `match(sprite)` runs in the
Sprite per-frame render after the original (0 = not mine, 1 = `post()` only: it may edit the points of the list the
sprite just built, 2 = also remember the submitted list), `draw(sprite, list)` runs right after that list was executed,
frame clients after every layer of the frame. A fault in a client turns that client off for the session (logged).
[mayhem_title] is a sprite client (unchanged behaviour: host test 17), [minimap] a sprite and a frame client.

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there | Verified |
|---|---|---|---|---|---|
| OV_FRAME | 0x421BBF | `8bcee86cf9ffff` (0x421BBD) | call retarget | GfxInternal::Render 0x421B30: `call 0x421530` (the layer renderer for every layer of the frame, thiscall(first, last, flag), ret 0Ch) -> `FrameWrap` (fastcall with a dummy edx = thiscall, 3 arguments): the original, then the frame clients | S H X D, G |

Not overlapping the renderer bridge (its patches: 0x421B30 entry 5 bytes, 0x421530 entry 8 bytes, 0x434290 entry): the
bridge's hooks still run inside the original calls.

**[minimap]** (sprite client of `Radar` / `Radar_BG`, frame client for the world markers; POST_INPUT for the blip
positions). REF rows: HR_CLASS (HUD_radar handler 0x6F0C80), HR_POS (+68h PlayerPos, +74h PlayerPosY), HR_VIEWV (+78h
PlayerView), HR_BLIPS (+34h list(s_RadarBlip), element size from [0xA3BB40] +24h, blip +8 target), HR_CLAMP (+0Ch
RadarRadius), HR_PLACE (+14h half size, +1Ch Factor), HR_RANGE (init: RadarRadius 50, +10h = 1 / R), HR_VIEWS / HR_VIEWMTX / HR_VIEWFOV (views object [0xA35E04] +24h: 0x114 per view, matrix +18h, fov +E0h,
ratioxy +E4h), HR_PROJ (aspect from renderer +320h / +324h), plus F_GETPOS, MT_QUAD, MT_SPR_IDX, MT_SPR_LIST, PD_NODE_NAME,
PH_NODE_PARENT. Writes: with `zoom != 1` the radar's +0Ch / +10h / +1Ch (no member-write notify, never in a save); with
`rotate = 0` the points of the radar sprites' own render lists of the frame (background, blips, north blip), recognised
again when the game did not rewrite them. Nothing else.

The map itself touches no game memory. Tiles (`gp_hud_tiles.cpp`) are read by a background thread that uses nothing of
the game; textures, uploads and the draw happen on the render thread, inside the radar's sprite client. Device state
beyond `gp_ov_save` (stage 1 texture and states, sampler 1, stage 2 ops, the LOD bias, the FVF) is read back and put back
by `tiles_draw` itself. `gp_overlay_set_pre` gives a sprite client a call right before its remembered list runs: the paper
behind the radar's parchment (`tiles_underlay`), tile packs only.

There is no weapon wheel: MOUSE_DX / MOUSE_DY are patched only for the mouse-sensitivity options and the pad's fake
DirectInput state has no mask. Weapon switching is the game's own: `select_*` (keys 1-8, scan 2-9) -> -25,
change_up / change_down (mouse wheel, pad RB / LB) -> -33 / -31. X (scan 45) is `do_exit_stunt`, no weapon key. A leftover `[weapon_wheel]` section in a player's ini is ignored (host test 20).

Verified in game (test copy, with an in-game test harness, not included; tile streaming included):
map in the radar turning with the view, rim arrow, world marker as diamond + distance on screen and as edge arrow off
screen (Ambush mission waypoint, type 2). Costs: `../hud/README.md`.

The three sprite / list sites above are the shared sprite hooks of `dll/src/gp_hudstage.cpp`
(patched once, for [mayhem_title], the RADAR stage and [mayhem_heat]); [mayhem_title] registers its "after the
sprite" and "after the list" callbacks there. MT_TEXTBOX_VT stays [mayhem_title]'s own.

## Shared HUD stages (`dll/src/gp_hudstage.cpp`)

Sprite hooks: `gp_sprite_hooks_add(before, after, exec, who)` + `gp_sprite_hooks_install(who)` patch MT_SPRITE_VT,
MT_EXEC_A, MT_EXEC_B once (SpriteWrap: every client's `before`, the original 0x8F6CC0, every client's `after`;
ExecWrap: the original 0x434290, then every client's `exec`). Up to 6 clients.

RADAR stage (`gp_radar_stage_install`): finds `Radar/Radar_BG` (HUDAuxCtrl.fragment, Sprite, virtual HUD +9Ch bit 6;
the TextureDict copy of the same name is not drawn and has no virtual-HUD bit), remembers its submitted list each frame
and, after that list ran, calls the draw clients with the quad the game built (read from the list like MT: op 32h /
33h, index +9Ch bits 25..31). Tint: the product of the clients' tints x the sprite's own corner colours is written into
+70h..+7Ch right before the sprite's render, with the colour setter's side effects (+14h bit 3: rebuild; +9Ch bit 10:
one colour), without the member-write notify (no rewind / save record). Faults turn the stage off (logged).

REF rows: HS_SPR_COLRD (render reads +9Ch bit 10 and +70h), HS_SPR_COLSET (the setter: +70h, +14h bit 3, bit 10),
HS_SPR_DIRTY (+14h bit 3 -> rebuild), HS_SPR_OPACITY (+98h), plus MT_DEVICE, MT_SPR_IDX, MT_QUAD, MT_SPR_UV, DS_SCREEN,
PD_NODE_NAME, PH_NODE_PARENT.

## Mayhem heat (`[mayhem_heat]`; code `dll/src/gp_heat.cpp`, logic `gp_heat_logic.cpp`, art `../mayhem_heat/`)

Off unless `enabled = 1`; then the 4 event sites, the input tick and (for the HUD) the RADAR stage. 22 REF rows checked
first (all-or-nothing).

| Id | Address (patch) | Shipped bytes checked (from) | Kind | What the DLL does there |
|---|---|---|---|---|
| HT_KILL_E | 0x5D718D | `406a0789442438e81e432900` (0x5D7186) | call retarget | sentry dying state 0x5D6B70, a HOSTILE death just counted (mission data +28h + 1): `call 0x86B4B0` (touch slot 7, thiscall, ret 4) -> KillHostileStub (naked: pushad / pushfd, queue {victim sentry = EDI}, popfd / popad, jmp 0x86B4B0) |
| HT_KILL_N | 0x5D7225 | `406a0889442438e886422900` (0x5D721E) | call retarget | same state, a NEUTRAL death counted (+2Ch): touch slot 8 -> KillNeutralStub (same) |
| HT_DESTROY | 0x6715C4 | `8b433c85c075198b0dc0b8a300` (0x6715BD) | jmp (6 bytes) | Destroyable_Object command_destroy_object: `mov ecx,[0xA3B8C0]` (the only way into the tail, also from 0x671570) -> DestroyStub: queue {object EDI, data ESI}, the mov, jmp 0x6715CA |
| HT_CAR | 0x809DE9 | `895c2450e872000600` (0x809DE5) | call retarget | vehicle_ai explode state 0x809910, right after Explosion_Big: SendMessage(camera AI, -6 shake) 0x869E60 (thiscall, ret 8) -> CarStub: queue {vehicle_ai EBX, data EBP}, jmp 0x869E60 |
| HT_SCORE | 0x64F575 | `5256894c2414e876d9ffff` (0x64F56F) | call retarget | PlayerStatus `command_add_to_overall_score` (command 0x25F) handler 0x64F560: the argument copied to its own stack slot, then the body cdecl(thread, int* amount) 0x64CEF0 -> ScoreWrap: *amount x the heat multiplier (none / 1 = x1, 2..5 = x1.25 .. x2 with `score_top` 2), then the body (REF HT_SCORE_ADD 0x64CEF9: esi = [0xA3B75C] PlayerStatus, edi = data, data +24h OverallScore += *amount). Only when `score_multiplier = 1` and both match |

REF rows: HT_KILLOWNER (KillOwner +178h 0 = not counted), HT_KILLER_RAM (the game's own KillOwner == player root test),
HT_DESTR_EXPL (+7Ch IsExplodeable), HT_SHELL_FLAG (shell node flag 4 set at the blast), HT_LOCO_RUN / HT_LOCO_IDLE
(PowerupCtrl RunningLocoMove +20h), G_COMBO / G_COMBO_PTS (Combo_Ctrl +3Ch), G_KILLS_R, PH_SHELLS (cache +550h shell
pool), SV_CLASSOBJ, SV_CLS_ROOT, SV_CLS_SENTRY, SM_RD_ROOT, SM_ROOT_AI, SV_QUERY_A, SV_QUERY_FREE, F_QUERY,
F_LISTFREE, DF_GETPOS, F_TOUCH, F_SENDMSG.

The stubs only queue (64 entries); the input tick classifies and applies them: killer = sentry data +178h KillOwner ==
`[[0xA3B590]+18h]+84h` (Ram); faction = root (sentry +40h) data +18h, Sub_Type +14h; 14 police -> cop kill, 6 / Sub 9 /
11 on the neutral path -> civilian kill, 7 animals -> small; destroyables: DestructorOwner +20h, IsExplodeable +7Ch;
cars: data +28h take-a-hit owner (inferred, logged). Polls in the tick: Combo_Ctrl +3Ch rises (style points), Ram's
PowerupCtrl (root data +8Ch) +20h 0 -> slot (loco move), shell pool node flag 4 0 -> 1 with owner (data +24h) Ram.
Hostiles near: the Tornado's quadtree query (mask 40h, 30 m) once a second, live sentries not of factions 0 / 6 / 7 /
10 / 11 / 18. Mission change / clock going back = heat cleared.

Escalation: `heat_drive()` is read by gp_difficulty.cpp's reinforcement pass every 0.5 s (share per death, wave count
due, comebacks per placement, rooftops first); `heat_wave_spent()` counts what it re-armed. With `[difficulty]` off the
engine alone is installed for heat (no menu, tuning or cap). No other game write.


## Mayhem props (`[mayhem]` chain reactions / breakable cover; code `mayhem/code/`, docs `mayhem/README.md`)

Off unless `[mayhem] chain_reactions = 1` or `breakable_cover = 1` AND `[physics]` is installed (Box3D loaded); then
every MH_ REF row plus F_CMD, F_QUERY, F_LISTFREE, F_GETPOS, F_SETFLAGS, SV_CLASSOBJ, PH_REGISTRY, PH_REG_VEC,
PH_GAME_DT, PH_NODE_OFF, PH_PIVNODE, PH_VT_*, PH_VC_VERTS, PH_OBOX_QUAT, PH_F_*, SL_PLAYER and MT_DEVICE must match,
else nothing is patched (logged). Each site is optional (a site with other bytes is skipped and logged). Verification:
S, H (host test 18: alone = 0 patches; with [physics] = 13: 3 physics + 5 mayhem + input tick + fixes; the car, hit,
query and draw wrappers on stand-ins), U (`test_mh.exe`), G (test copy runs, `mayhem/README.md`).

| MH_CAR_A | 0x80A756 | `56891d5cb7a300e805c1ffff` (0x80A74F) | call retarget | vehicle_ai explode state 0x809910: the car's explosion cdecl(thread, 0) 0x806860 ([0xA3B75C] = the vehicle_ai node) -> `MhCarWrap`: the original, then the blast is queued (chain reactions, Box3D blast `car_blast_*`) | S H |
| MH_CAR_B | 0x80A8EC | `6a0056891d5cb7a300e86fbfffff` (0x80A8E3) | call retarget | the same call on the other path of 0x809910 | S H |
| MH_DO_QUERY | 0x66A6EE | `8b0d78b5a30052c784245c04000001000000e85dc2e1ff` (0x66A6DC) | call retarget | Destroyable_Object do_explosion 0x66A600, IsExplodeable branch: the ExplosiveRange quadtree query thiscall([0xA3B578], {out, pos, radius, 440h, 1}) 0x486950 -> `MhQueryWrap`: the original, then the blast (the query's centre; [0xA3B75C] = the exploding object, checked: class and position) is queued | S H |
| MH_DO_HIT | 0x66BCDF | `8b542428518b4c900868d7000000e85ce61f00` (0x66BCD1) | call retarget | do_explosion's `take_a_hit` to each entity found: Node::SendCommand thiscall(entity, 215, args) ret 8 -> `MhDoHitWrap`: an explosive destroyable (alive, not Locked / ImmuneToDamage) is scheduled as a chain link instead (chain reactions on); everything else gets the original call | S H |
| MH_DRAW | 0x421C71 | `8b0d149fa300e86ab90300` (0x421C6B) | call retarget | GfxInternal::Render 0x421B30, world branch: after the world layers (0x421C66 call 0x421530) the frame-end motion blur thiscall([0xA39F14]) 0x45D5E0 -> `MhBlurWrap`: the debris pieces drawn with the device (depth-tested, before the blur and the HUD layers), then the original | S H |

REF rows: MH_CLS_DO / MH_CLS_DOPH / MH_CLS_VROOT / MH_CLS_VAI (class registrations: handler 0x6691A0 / 0x67BD80 /
0x824A70 / 0x803A50 at class object +60h), MH_DO_EXPLODEABLE (data +7Ch gates the explosion in command_destroy_object
0x671410), MH_DO_ISDEAD (+144h), MH_DO_RANGE (do_explosion's radius = +118h), MH_DO_HEALTH (init 0x66F9B0: Health
+114h = Health_m +B8h + the dictionary value), MH_VAI_DESTROYED (vehicle_ai +C8h), MH_GFX_LAYERS (the world layers are
drawn right before the blur call). Checked at install for debris_style 1 only (a mismatch =
procedural materials, nothing else off): MH_TEXSETS 0x856F42 (ModelAsset::GetPivotMeshTexture 0x856F10: TextureSets
[asset +20h], 8-byte entries, TextureAsset +28h & ~3 = Texture; count +24h, capacity +28h from the append at 0x856F60's
caller), MH_D3DTEX 0x4634A0 (Texture::GetDirect3DTexture: [Texture +4] & ~3). The broken prop's texture is AddRef'd
by each debris piece and released when it goes; its smallest mip <= 64 x 64 is locked read-only once per break.

Game functions called (thiscall, SEH-guarded, the script current-node globals 0xA3B75C/60/64 saved and restored around
each): Node::SendCommand 0x86A340 (`take_a_hit` 215 with the do_explosion argument block {damage, owner, inflictor,
collision volume, impulse[3], position[3], type 1} to a destroyable / a vehicle_root; `command_crouch_out` 1054 to an
enemy's character_animation_ctrl), quadtree query 0x486950 + list release 0x8718D0 (mask 440h mode 1 around blasts and
Ram; 40h mode 0 for enemies near a broken cover prop), GetWorldPosition 0x88CA80, GetWorldMatrix 0x4842C0,
Model::GetPivotCollision 0x884860, model name 0x884350, Node::SetFlags thunk 0x500BE0 (wreck hidden: flags | 1; shown
again with & FFEh on pool reuse / rewind); test copy only: SetWorldPosition 0x88E600. Memory written: Destroyable_Object
data +114h Health of cover props (x cover_health_scale, no notify; re-applied after the pool re-initialises one). The
D3D9 device (renderer [0xA39F14] +0): render / texture-stage / sampler states, shaders, FVF / declaration, texture 0
and the WORLD / VIEW / PROJECTION transforms touched by the debris draw are read first and written back.


## Ragdoll (`[ragdoll]`; code `ragdoll/code/`, docs `ragdoll/README.md`; Box3D part in `TODPhysics.dll`)

Off unless `[ragdoll] enabled = 1`. It needs the [physics] module's Box3D world: with `[physics] enabled = 0` the physics
module runs in "world only" mode (level collision near Ram, no props, its 3 call sites + the input tick still patched),
and `TODPhysics.dll` must export `TODPhysics_GetRagdollApi` (`ragdoll/code/rd_api.h`, version 1). Installed after every
other feature (their REF checks run on unpatched bytes). Every RD_ REF row plus SM_RD_STATE, SM_ROOT_AI, SV_CLS_ROOT,
SV_CLS_SENTRY, SV_CLASSOBJ, SV_MODELRES, PH_F_PIVCOLL, PH_F_WORLDMAT, PH_GAME_DT, SL_PLAYER, F_GETPOS, F_SETPOS,
F_SETROTY, F_L2W and F_GETPROP must match, else nothing is patched (logged). Verification: S, H (host test 18: off = 0
patches; alone = the 2 ragdoll sites + the physics world's 3 sites + input tick + fixes = 10; CharUpdWrap thiscall with a
character it does not know leaves it untouched; HitStub at the call-site shape records the hit, `ret 8`, eax / ebx /
esi / edi / ebp kept), U (`test_rd.exe`, 292 checks on real skeletons + Box3D), G (`ragdoll/README.md`).

| RD_CHARUPD | 0x897EFD | `8bcfe8de8d0700` (0x897EFB) | call retarget | Scene::Update's character loop (list [0xA3DD5C], count [0xA3DD60]; hidden / faded characters skipped): `call 0x910CE0` Character::Update (thiscall, no arguments; samples the layers, UpdatePose 0x90FFF0 -> compose 0x90EA50 writes each bone's local rotation and Bip's local position, then +14h \|= 2 on every bone) -> `CharUpdWrap` (__fastcall): the original, then the animated pose is read (targets / velocities) and, for a ragdolled character, every driven bone's local (NodeMatrix +00h stored quaternion, Bip also +10h position) is written and the bones marked dirty again. Nothing reads the bones between the compose and this write; world matrices are lazy (0x892940) and the renderer skins on the CPU from them | S H X, G |
| RD_CORPSE | 0x606D44 | `8b57406a00680000003f8bcfc742081a000000` (0x606D41) | jmp hook 7 | character_Take_A_Hit_Move_ctrl corpse state 0x601410 (edi = the script thread): `push 0; push 0.5f` before the first SLEEP after the death-pose placeholder was taken over -> `CorpseStub`: pushad, `CorpseSecsC(thread)` (frame +0 entity -> parents up to the character_root; `corpse_seconds` for an enemy while fewer than `corpse_max` wait, else 0.5; the player always 0.5), popad, `push 0; push <seconds>`, back to 0x606D4B | S H X (host: no enemy -> 0.5 pushed, registers kept), G (log: `corpse ... waits 4.0 s`) |
| RD_HIT | 0x5C535E | `8b4f586a006834050000e8dd4f2a00` (0x5C5354) | call retarget | character_root take_a_hit relay 0x5C4F40: `call 0x86A340` SendCommand(AICtrl, 0x534 take_a_hit, 0) after the hit went into the AI's properties 0x64A..0x650; at the call esi = the hit {float damage, owner, inflictor, volume, float impulse[3], float position[3], int type}, ebp = the root -> `HitStub` (naked: esi / ebp saved) -> `HitWrap` (__fastcall, ret 8): the hit recorded, then the original | S H X, G (bullets: type 0, impulse = unit direction, position = the impact point; melee type 2; explosive type 1 with a large impulse) |

REF rows: RD_COMPOSE_BONES 0x90EA96 (bone node = Character +1D8h [i]; 32-byte pose entries, valid byte +0Ch; a bone with
node flag +28h & 4 is skipped by the compose), RD_COMPOSE_DIRTY 0x90EB25 (count +1DCh; +14h \|= 2), RD_BONE_SET 0x5A1710
(NodeMatrix = node +3Ch, owner +60h, +00h stored quaternion), RD_WORLD_DIRTY 0x892940 (world recomputed while the owner's
+14h & 2), RD_STATE28 0x5D6BD8 (the sentry death state sets Current_state +1D4h = 28), RD_CAMERA 0x87E160 (debug draw:
active camera Scene +6Ch, matrix 0x87BC60, fov +B4h).
Memory read: Character +1D0h model asset (pivot records +30h stride 7Ch: +00h position, +0Ch stored rotation = the bind
pose, pivot i = bone i), +1D8h / +1DCh bones, +40h parent = the character_root, +1ACh opacity (logged only); bone node
+3Ch NodeMatrix (+00h quaternion, +10h position, +60h owner), +40h parent, +4Ch name; root data +14h Sub_Type, +50h the
Character node, +58h AICtrl, +68h PhysicCtrl; sentry data +2Ch Health, +30h DamageTaken, +1D4h Current_state.
Game functions called (thiscall, SEH-guarded): 0x4842C0 GetWorldMatrix, 0x88CA80 GetWorldPosition, 0x88E600
SetWorldPosition + Node::SendMessage 0x869E60 character_physic_ctrl -22 command_reset_to_parent (a corpse's root follows
its pelvis; a pushed active character steps along), 0x86A6A0 GetProperty (torso_cs of the debug hit). Written: bone
locals (above), the root position of ragdolled characters. Test copy only: 0x891360 SetRotationY + 0x88C9C0 (debug
bring), sentry -23 command_no_ai_control, character_root -2 take_a_hit (debug hit / kill), weapon_shell -11 / -12 / -9
(debug boom, as the physics rig), character_goodie_ctrl -8 command_induce_god_mode (debug god), `gp_yaw_add` (debug turn).
Interplay: the renderer bridge patches Character::Render 0x911EA0 (not this call); it reads the bone locals at render
time, so it sends the ragdoll pose.

## Bigger Fights (`[bigger_fights]`; code `dll/src/gp_bigfights.cpp`)

Off unless `enabled = 1`. Experimental, goes with the data option `bigger_fights` (it breaks saves, docs/mods/bigger_fights.md).
Refused as a whole when one of F_CMD, F_SETFLAGS, F_SETPOS, F_OCCLUDED, F_FINDCHILD, SV_CLASSOBJ, DF_GETPOS, DF_SENDCMD,
DF_SPMASK_GET, DF_PH_QUERY, DF_PH_KILLMASK, DF_PH_RES, DF_RC_MAX6, DF_RC_MAX8, BF_RC_WEAPON, BF_RC_WEAR, BF_SPRINTF,
BF_CLS_TRAFFIC, BF_VEH_DRIVE, G_MISSION, G_MISSION_CM, G_KILLS_R differs. Runs in the input tick, once per second. Verification: S,
H (host test 24: off = 0 patches; on = 5 slot sites + input tick 3 + fixes 2), U (`test_bigfights.exe`), data (`tests/data/test_bigger_fights.py`),
G.

| Id | Address | Bytes | Kind | What |
|---|---|---|---|---|
| BF_SLOT1 | 0x8976C4 | see sites.h | call retarget | scene save / load state: sprintf(out, "Slot%02d", slot) -> slot name stub |
| BF_SLOT2 | 0x8978AD | see sites.h | call retarget | the same, second path |
| BF_SLOT3 | 0x926555 | see sites.h | call retarget | slot name of the save list, delete, exists (0x926540) |
| BF_SLOT4 | 0x895D46 | see sites.h | call retarget | start of a save (0x895CB0) |
| BF_SLOT5 | 0x926B74 | see sites.h | call retarget | slot file exists (0x926B60) |

REF rows: BF_SPRINTF 0x415300, BF_RC_WEAPON 0x76355F, BF_RC_WEAR 0x76350A, BF_CLS_TRAFFIC 0x6564A8 (class handler 0x655480),
BF_VEH_DRIVE 0x816DF0 (the ghost step of id 413).
Memory written: resource sub-controller MaxAllocated (data +18h) of the weapon and wear controllers (the character cap
goes through gp_difficulty's cap code); vehicle_placeholder data +100h StoppedByAI, +F4h, +F8h before a car starts.
Game functions called (thiscall, SEH-guarded): 0x88CA80 GetWorldPosition, 0x88D6C0 SetPosition, 0x88E600 SetWorldPosition, 0x500BE0
SetFlags, 0x86A340 SendCommand (0x19F trigger with Ram, 413 drive), 0x86A6A0 GetProperty, 0x486950 + 0x8718D0 quadtree query and list
release, 0x91D020 IsPointOccluded, 0x88EB60 FindChild; through `df_rearm_placeholder`: 0x86A6C0 SetProperty (DamageTaken), 0x5A18E0,
0x5A1970, 0x48F3A0.

## Drops (`[drops]`; code `drops/code/`, docs `docs/mods/drops.md`)

No code site: the shared input tick and post-input hook, as PSYCHO. Install checks the REF rows below; one mismatch turns
the feature off.

| What | Call / data | REF |
|---|---|---|
| the gun's dropped bullets (log) | default_weapon data +80h BulletsToDrop, +7Ch MaxBulletsToDrop (init 0x666530) | DR_WPN_BULLETS |
| the ammo bands | BulletDrop (cache +2C0h) data +0Ch..+30h; `[difficulty]` writes them with `loot_ammo_factor()`, else `[drops]` does (vanilla 10 / 250 checked first) | DF_BULLETDROP |
| the combo flag (log) | mission data (cache +25Ch) +78h != 0: the combo kill handler 0x69CB20 counts nothing | DR_COMBO_BLOCK |
| a pickup | the elite-drops pool path (request -13 type 5, touch, status, show, sub type +5Ch, reset -4, AutoRemoveTimer), with ResourceStatus **3** | PS_RES_*, PS_PROP_*, PS_SHOW, PS_SETFLAGS |
| its OrgPos | Goodie_Default data +38h (touch 0Ch) = the node's matrix position at command_reset (0x68AC8B). The prop goes to the spot BEFORE the reset, and OrgPos is put on the spot after it | DR_GOODIE_ORGPOS |
| enemies | quadtree query mask 40h, root / sentry class handlers, relations row 0 = 4 hostile, sentry data +2Ch / +30h / +1D4h | F_QUERY, SV_CLS_*, SV_FACTION_*, SM_ROOT_AI |
| Ram's health / power-ups | player AI data +1Ch / +20h; power-up ctrl data +14h -> {counts, maxima} | PS_RAM_HEALTH, PS_POWERUP_COUNT |

ResourceStatus 3 is used by nobody else: the pool's dangling check (0x75E9E0) frees only status 1 without a placeholder,
elite drops own status 2, and each sweep frees only its own status left by a save. Test-only keys: `debug_probe_key`,
`debug_probe_seconds`, `debug_drop_key`, `debug_drop_goodie`, `debug_walk_to_goodie`, `debug_kill_seconds`, `debug_marks`.
