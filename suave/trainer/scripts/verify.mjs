// Headless verification of the Suave Trainer (no game involved):
//  1. loads without console errors;  2. the timeline plays through every phase;
//  3. the pairing switch and the camera presets work;  4. debug overlay + hand-contact readout;
//  5. "play as Ram": walk with WASD, press E next to an enemy, the move runs and the ring goes down.
// Screenshots + verify_report.json go to proof/.   usage: npm run verify
import { ensureServer, launch, waitReady, ROOT } from './browser.mjs'
import path from 'node:path'
import fs from 'node:fs'

const OUT = path.join(ROOT, 'proof')
fs.mkdirSync(OUT, { recursive: true })
const report = { date: new Date().toISOString(), checks: [], screenshots: [] }
const check = (name, ok, detail) => { report.checks.push({ name, ok: !!ok, detail }); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  ' + JSON.stringify(detail) : ''}`) }
const sleep = ms => new Promise(r => setTimeout(r, ms))

const srv = await ensureServer()
const { browser, page, errors } = await launch({ width: 1600, height: 900 })
const shot = async (name, opts = {}) => { const f = path.join(OUT, name + '.jpg'); await page.screenshot({ path: f, type: 'jpeg', quality: 88, ...opts }); report.screenshots.push(f); return f }

try {
  await page.goto(srv.url + '/')
  await waitReady(page)
  await sleep(1500)
  await page.check('#mute')                      // headless: keep the music silent, the clock runs on
  check('loads, no console errors', errors.length === 0, errors.slice(0, 5))
  await shot('01_loaded')

  // ---- timeline plays every phase (real playback at 2x) ----
  await page.selectOption('#speed', '2')
  const seen = new Set()
  await page.click('#play')
  const t0 = Date.now()
  const dur = await page.evaluate(() => window.__trainer.duration())
  while (Date.now() - t0 < 16000) {
    const s = await page.evaluate(() => ({ phase: document.getElementById('phaseName').textContent, t: window.__trainer.state.t, playing: window.__trainer.state.playing }))
    seen.add(s.phase)
    if (!s.playing && s.t >= dur - 0.01) break
    await sleep(80)
  }
  const endT = await page.evaluate(() => window.__trainer.state.t)
  const timingInfo = await page.evaluate(() => ({ timing: window.__trainer.timing(), sa: window.__trainer.info().sa, danceSource: !!window.__trainer.info().danceSource }))
  check('timeline plays all phases', ['Activate', 'Gather', 'Dance', 'Plant', 'Escape', 'Boom'].every(p => seen.has(p)) && endT >= dur - 0.01, { seen: [...seen], endT, duration: dur, ...timingInfo })
  await page.selectOption('#speed', '1')

  // phase stills via the scrubber (front camera)
  const stillsFor = async prefix => {
    const ph = Object.fromEntries((await page.evaluate(() => window.__trainer.phases())).map(p => [p.id, p]))
    const d = await page.evaluate(() => window.__trainer.duration())
    const T = await page.evaluate(() => window.__trainer.times())
    const stills = [['intro_pushin_title', 0.45], ['intro_takes_hand', 3 * T.introBeat], ['intro_two_shot', 5 * T.introBeat],
      ['gather', (ph.gather.t0 + ph.gather.t1) / 2], ['dance', ph.dance.t0 + (ph.dance.t1 - ph.dance.t0) * 0.42],
      ['4a_dynamite_belt', T.shot4a + 0.45], ['4b_low_front_dive', T.jump + 0.15], ['5_partner_looks_down', T.shot5 + 0.6],
      ['6_boom_behind_ram', T.boom + 0.3], ['6_landing', T.land + 0.5], ['end', d - 0.3]]
    for (const [i, [name, t]] of stills.entries()) {
      await page.evaluate(t => { const tr = window.__trainer; for (let x = Math.max(0, t - 2); x < t; x += 1 / 30) tr.renderAt(x, 1 / 30); tr.renderAt(t, 1 / 30) }, t)
      await shot(`${prefix}_${String(i + 1).padStart(2, '0')}_${name}`)
    }
  }
  await stillsFor('02_game_sa')

  // ---- pairing switch ----
  const pairOk = []
  for (const p of ['civil', 'dea', 'soldier']) {
    await page.selectOption('#pairing', p)
    await page.evaluate(() => { const tr = window.__trainer; for (let x = 4; x < 6.4; x += 1 / 30) tr.renderAt(x, 1 / 30) })
    const info = await page.evaluate(() => window.__trainer.info())
    pairOk.push({ want: p, partner: info.partnerKind, joinedCm: info.contact?.joined != null ? +(info.contact.joined * 100).toFixed(1) : null })
    await shot(`09_pairing_${p}`)
  }
  check('pairing switch changes the partner body', pairOk.every(x => x.want === x.partner), pairOk)

  // ---- camera presets ----
  const camPos = {}
  for (const [i, p] of ['cuts', 'gameplay', 'front', 'top', 'orbit', 'free'].entries()) {
    await page.click(`#cams .chip[data-p="${p}"]`)
    await page.evaluate(() => { const tr = window.__trainer; for (let x = 8.5; x < 10; x += 1 / 30) tr.renderAt(x, 1 / 30) })
    const info = await page.evaluate(() => window.__trainer.info())
    camPos[p] = { preset: info.preset, cam: info.cam.map(v => +v.toFixed(2)) }
    await shot(`10_camera_${i + 1}_${p}`)
  }
  const distinct = new Set(Object.values(camPos).slice(0, 5).map(c => c.cam.join(','))).size
  check('camera presets switch and frame differently', Object.entries(camPos).every(([k, v]) => v.preset === k) && distinct === 5, camPos)
  await page.click('#cams .chip[data-p="front"]')

  // ---- debug: rings, skeletons, contact readout ----
  await page.check('#debug')
  await page.evaluate(() => { const tr = window.__trainer; for (let x = 3.5; x < 5.2; x += 1 / 30) tr.renderAt(x, 1 / 30) })
  const readout = await page.evaluate(() => document.getElementById('debugOut').innerText)
  const contact = await page.evaluate(() => window.__trainer.info().contact)
  check('debug readout shows hand contact (Suave clips: measured, reported)', /(joined hands|Ram L > partner R)\s+\d/.test(readout), { readout: readout.split('\n'), contact })
  await shot('11_debug_rings_skeletons_sa')
  // placeholder path: the IK hold must keep the hands together
  await page.evaluate(() => window.__trainer.setUseSA(false))
  await page.evaluate(() => { const tr = window.__trainer; for (let x = 3.5; x < 5.2; x += 1 / 30) tr.renderAt(x, 1 / 30) })
  const pc = await page.evaluate(() => window.__trainer.info().contact)
  check('placeholder hold keeps hand contact < 6 cm', pc.joined < 0.06 && pc.back < 0.06 && pc.shoulder < 0.06, pc)
  await shot('11b_debug_placeholder_hold')
  await page.evaluate(() => window.__trainer.setUseSA(true))
  await page.click('#cams .chip[data-p="top"]')
  await page.evaluate(() => { const tr = window.__trainer; for (let x = 2.5; x < 3.5; x += 1 / 30) tr.renderAt(x, 1 / 30) })
  await shot('11c_debug_topdown')
  await page.uncheck('#debug')
  await page.click('#cams .chip[data-p="front"]')

  // ---- the design-doc timeline with the placeholder clips ----
  await page.evaluate(() => { window.__trainer.setUseSA(false); window.__trainer.setTimingProfile('design') })
  const dph = await page.evaluate(() => window.__trainer.phases().map(p => [p.id, +p.t0.toFixed(2), +p.t1.toFixed(2)]))
  // design doc phases shifted by the 3 s intro (a longer, clearer activation); the slow-motion
  // dive then needs the boom inside the flight, so escape and boom follow from the dive length
  check('design timing: intro 0-3, gather 3-5, dance 5-15, plant 15-16, then escape and boom', JSON.stringify(dph.slice(0, 4).map(p => [p[1], p[2]])) === JSON.stringify([[0, 3], [3, 5], [5, 15], [15, 16]]) && dph[4][1] === 16 && dph[5][1] > dph[4][1], dph)
  await stillsFor('12_design_placeholder')
  await page.evaluate(() => { window.__trainer.setUseSA(true); window.__trainer.setTimingProfile('game') })

  // ---- facial expressions on every body ----
  const faces = await page.evaluate(() => window.__trainer.faceCheck())
  check('facial clips drive the face bones on Ram, soldier, civil and DEA', Object.values(faces).every(f => Object.values(f.tracks).every(n => n >= 8) && f.chinDeg > 1), faces)
  await page.evaluate(() => { window.__trainer.setPairing('soldier'); window.__trainer.setPreset('cuts') })

  // ---- play as Ram ----
  await page.click('#playMode')
  await sleep(800)
  const p0 = await page.evaluate(() => window.__trainer.play())
  await page.keyboard.down('KeyW'); await sleep(1300); await page.keyboard.up('KeyW')
  await page.keyboard.down('ShiftLeft'); await page.keyboard.down('KeyD'); await sleep(700); await page.keyboard.up('KeyD'); await page.keyboard.up('ShiftLeft')
  const p1 = await page.evaluate(() => window.__trainer.play())
  const walked = Math.hypot(p1.pos[0] - p0.pos[0], p1.pos[2] - p0.pos[2])
  check('play mode: WASD walks Ram', p0.on && walked > 1.5, { from: p0.pos.map(v => +v.toFixed(2)), to: p1.pos.map(v => +v.toFixed(2)), walked: +walked.toFixed(2) })
  await shot('13_play_walk')
  // walk up to the first enemy (3.2, -7.5) and press E
  await page.evaluate(() => window.__trainer.teleportRam(2.6, -10.8, 0.15))
  await sleep(400)
  const hint = await page.evaluate(() => document.getElementById('playhint').innerText)
  await shot('14_play_grab_prompt')
  await page.keyboard.press('KeyE')
  await sleep(300)
  const p2 = await page.evaluate(() => window.__trainer.play())
  check('play mode: E triggers Suave on the nearest enemy', p2.move === true, { hint, p2 })
  await sleep(2400); await shot('15_play_suave_gather')
  await sleep(4000); await shot('16_play_suave_dance')
  // finish faster
  await page.selectOption('#speed', '2')
  await page.evaluate(() => window.__trainer.setPlaying(true))
  let p3
  for (let i = 0; i < 120; i++) { p3 = await page.evaluate(() => window.__trainer.play()); if (p3.t > 15.6 || !p3.move) break; await sleep(100) }
  await shot('17_play_suave_boom')
  for (let i = 0; i < 100; i++) { p3 = await page.evaluate(() => window.__trainer.play()); if (!p3.move) break; await sleep(100) }
  await sleep(600)
  check('play mode: move ends, partner and ring are down, control returns', !p3.move && p3.alive < 14, p3)
  await shot('18_play_after')
  await page.keyboard.press('Escape')
  await sleep(500)
  check('no console errors during the whole run', errors.length === 0, errors.slice(0, 5))
} catch (e) {
  check('verify run completed', false, String(e.stack || e))
  await shot('99_failure').catch(() => {})
} finally {
  report.passed = report.checks.every(c => c.ok)
  fs.writeFileSync(path.join(OUT, 'verify_report.json'), JSON.stringify(report, null, 2))
  console.log(report.passed ? 'ALL PASS' : 'SOME CHECKS FAILED')
  await browser.close()
  await srv.close()
  process.exit(report.passed ? 0 : 1)
}
