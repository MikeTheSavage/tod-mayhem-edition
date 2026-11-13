// Which clip plays what. Placeholders come from the game's own library (the TODViewer
// .charanim.glb exports share the same 346-clip set on every body); the Suave clips replace the dance,
// circle, push and stumble roles once suave/anim/export/manifest.json appears.

export const CLIPS = {
  idle: 'ram_idle_breathe_aware',
  idleLong: 'ram_idle_long_weightshift',
  rifleIdle: 'ram_weapon_idle_auto_long',   // upper body: rifle held at the hip
  aim: 'ram_aim_hi_auto_long',              // upper body: rifle shouldered
  shoot: 'ram_shoot_hi_auto_long',          // upper body: one shot of recoil
  walk: 'ram_walk', run: 'ram_run', sprint: 'ram_sprint', runBack: 'ram_run_backwards',
  alert: 'ram_alerted_front_gather',
  diveJump: 'ram_shootdodge_front_jump', diveJumpGun: 'ram_shootdodge_front_jump_gun',
  diveAir: 'ram_shootdodge_front_midair', diveAirGun: 'ram_shootdodge_front_midair_gun',
  diveLand: 'ram_shootdodge_front_land', diveLandGun: 'ram_shootdodge_front_land_gun',
  dualIdle: 'ram_weapon_idle_gun_dual',
  flyBack: 'ram_airborne_body_back_midair', flyBackX: 'ram_airborne_body_back_xtrm', landBack: 'ram_airborne_body_back_land',
  defend: 'ram_cc_defend_front',
  cool: 'ram_grab_hat',
}

// metres travelled per locomotion cycle (measured from the clips' foot travel, tuned by eye)
export const CYCLE_M = { ram_walk: 1.45, ram_run: 2.6, ram_sprint: 3.0, ram_run_backwards: 1.7 }

// The exported clip seconds run about twice as fast as the game plays them (the viewer warns the
// authored playback rate is separate); one-shot clips are slowed by this factor.
export const ONE_SHOT_RATE = 0.5

// the clip manifest -> roles. Accepts a few spellings so a small schema drift does not break pickup.
export function parseManifest(m) {
  if (!m) return null
  let list = Array.isArray(m) ? m : (m.clips || m.entries || m.animations || [])
  // export_glb.py writes clips as an object keyed by clip name
  if (!Array.isArray(list)) list = Object.entries(list).map(([name, e]) => ({ ...e, clip: e.clip || name }))
  const out = []
  for (const e of list) {
    if (!e) continue
    const file = e.file || e.glb || e.path || e.url
    if (!file) continue
    const role = String(e.role || e.type || '').toLowerCase()
    const character = String(e.character || e.model || e.body || '').toLowerCase()
    const pairing = String(e.pairing || e.pair || e.partner || '').toLowerCase()
    const fps = +(e.fps || e.frame_rate || 30)
    out.push({
      raw: e, file, role, character, pairing,
      clip: e.clip || e.name || e.animation || null,
      duration: e.duration_s != null ? +e.duration_s : (e.duration != null ? +e.duration : null),
      standIn: !!e.stand_in,
      loop: e.loop === true || e.loop === 'true',
      fps,
      releaseFrame: e.release_frame ?? e.releaseFrame ?? null,
      releaseTime: e.release_time_s ?? e.release_time ?? e.releaseTime ?? (e.release_frame != null ? +e.release_frame / fps : null),
      bpm: e.bpm != null ? +e.bpm : (m.bpm != null ? +m.bpm : null),
      beatOffset: e.beat_offset_s ?? e.beat_offset ?? e.beatOffset ?? e.first_beat ?? (m.beat_offset ?? null),
      startFrame: e.start_frame ?? null,
    })
  }
  return { entries: out, raw: m }
}

// pairing id -> body model keys used in the manifest (group by model, see the design doc)
export const PAIRINGS = {
  soldier: { label: 'Soldier (soldier.model)', model: 'soldier', glb: 'soldier.charanim.glb' },
  civil: { label: 'Civil (civil.model)', model: 'civil', glb: 'civil.charanim.glb' },
  dea: { label: 'DEA (dea_taskforce.model)', model: 'dea_taskforce', glb: 'dea_taskforce.charanim.glb' },
}

export function matchPairing(entry, pairingId) {
  const p = PAIRINGS[pairingId]
  if (entry.pairing) return entry.pairing === pairingId || entry.pairing === p.model
  const s = `${entry.pairing} ${entry.character} ${entry.file}`.toLowerCase()
  if (pairingId === 'dea') return /dea/.test(s)
  if (pairingId === 'civil') return /civil/.test(s)
  return /soldier/.test(s) && !/dea|civil/.test(s) || s.includes(p.model)
}
