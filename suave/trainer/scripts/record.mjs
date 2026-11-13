// Deterministic preview videos: steps the timeline frame by frame in headless Chrome (render mode,
// HUD + SUAVE title included), pipes the frames into ffmpeg and muxes the selected music cut.
// usage: npm run record -- [--cams=cuts] [--pairings=soldier,civil,dea] [--timing=game|design]
//                           [--seg=<segment id>] [--w=1280] [--h=720] [--fps=30] [--out=..\previews]
// Output: <out>/suave_<timing>_<pairing>_<cam>.mp4 (git-ignored: they carry a cut of the song).
import { ensureServer, launch, waitReady, ROOT } from './browser.mjs'
import { spawn, execSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'

const opt = Object.fromEntries(process.argv.slice(2).filter(a => a.startsWith('--')).map(a => { const [k, ...v] = a.slice(2).split('='); return [k, v.join('=') || '1'] }))
const W = +(opt.w || 1280), H = +(opt.h || 720), FPS = +(opt.fps || 30)
const cams = (opt.cams || 'cuts').split(',')
const pairings = (opt.pairings || opt.pairing || 'soldier,civil,dea').split(',')
const OUT = path.resolve(ROOT, opt.out || '../previews')
fs.mkdirSync(OUT, { recursive: true })

function findFfmpeg() {
  if (process.env.FFMPEG_PATH && fs.existsSync(process.env.FFMPEG_PATH)) return process.env.FFMPEG_PATH
  try { const p = execSync('where ffmpeg', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().split(/\r?\n/)[0].trim(); if (p) return p } catch { /* not on PATH */ }
  try { const p = execSync('python -c "import imageio_ffmpeg;print(imageio_ffmpeg.get_ffmpeg_exe())"', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim(); if (p && fs.existsSync(p)) return p } catch { /* no imageio-ffmpeg */ }
  throw new Error('ffmpeg not found: set FFMPEG_PATH, put ffmpeg on PATH, or `python -m pip install imageio-ffmpeg`')
}
const FFMPEG = findFfmpeg()
const cfgFile = fs.existsSync(path.join(ROOT, 'config.local.json')) ? 'config.local.json' : 'config.example.json'
const cfg = JSON.parse(fs.readFileSync(path.join(ROOT, cfgFile), 'utf8'))

const srv = await ensureServer()
const { browser, page, errors } = await launch({ width: W, height: H })
const q = new URLSearchParams({ render: '1', w: W, h: H, pairing: pairings[0] })
if (opt.timing) q.set('timing', opt.timing)
if (opt.seg) q.set('seg', opt.seg)
await page.goto(`${srv.url}/?${q}`)
await waitReady(page)
const made = []
for (const pairing of pairings) {
  await page.evaluate(p => window.__trainer.setPairing(p), pairing)
  const meta = await page.evaluate(() => ({ duration: window.__trainer.duration(), timing: window.__trainer.timing(), info: window.__trainer.info(), seg: window.__trainer.music.current }))
  const seg = meta.seg
  let audioArgs = [], audioDelay = 0
  if (seg) {
    // resolve the audio file on disk from the segment's URL (never copied anywhere but the mp4)
    let file = null, start = seg.start || 0
    if (seg.source === 'sb') file = path.join(cfg.sbAudio, seg.file)
    else if (seg.source === 'local-mp3') file = cfg.musicPath
    // the segment's beat 0 plays at the end of the intro (timeline T.musicZero): song cuts start that much
    // earlier (pre-roll), files that start at beat 0 are delayed
    const zero = await page.evaluate(() => window.__trainer.times().musicZero)
    const at = start - zero
    if (file && fs.existsSync(file)) {
      audioArgs = at >= 0 ? ['-ss', String(at), '-t', String(meta.duration + 0.5), '-i', file] : ['-t', String(meta.duration + 0.5), '-i', file]
      audioDelay = at >= 0 ? 0 : -at
    }
  }
  console.log(`timing ${meta.timing}, ${meta.duration.toFixed(2)} s, pairing ${meta.info.pairing}, clips ${meta.info.danceSource ? 'Suave clips' : 'placeholder'}, music ${seg ? seg.label : 'none'}`)

  for (const cam of cams) {
    const file = path.join(OUT, `suave_${meta.timing}_${meta.info.pairing}_${cam}.mp4`)
    await page.evaluate(c => window.__trainer.setPreset(c), cam)
    const args = ['-y', '-loglevel', 'error', '-f', 'image2pipe', '-framerate', String(FPS), '-c:v', 'mjpeg', '-i', '-', ...audioArgs,
      '-map', '0:v', ...(audioArgs.length ? ['-map', '1:a', '-af', `${audioDelay ? `adelay=${Math.round(audioDelay * 1000)}:all=1,` : ''}afade=t=in:st=${audioDelay.toFixed(2)}:d=0.45,apad,afade=t=out:st=${(meta.duration - 0.6).toFixed(2)}:d=0.6`, '-c:a', 'aac', '-b:a', '192k'] : []),
      '-t', meta.duration.toFixed(3), '-vf', 'scale=in_range=pc:out_range=tv', '-c:v', 'libx264', '-preset', 'medium', '-crf', '18', '-pix_fmt', 'yuv420p', '-color_range', 'tv', '-movflags', '+faststart', file]
    const ff = spawn(FFMPEG, args, { stdio: ['pipe', 'inherit', 'inherit'] })
    const done = new Promise((res, rej) => ff.on('close', c => c === 0 ? res() : rej(new Error('ffmpeg exit ' + c))))
    const n = Math.ceil(meta.duration * FPS)
    const t0 = Date.now()
    for (let i = 0; i <= n; i++) {
      const t = Math.min(meta.duration, i / FPS)
      await page.evaluate(([t, dt]) => window.__trainer.renderAt(t, dt), [t, 1 / FPS])
      const jpg = await page.screenshot({ type: 'jpeg', quality: 92 })
      if (!ff.stdin.write(jpg)) await new Promise(r => ff.stdin.once('drain', r))
      if (i % 60 === 0) process.stdout.write(`\r${cam}: frame ${i}/${n} (${((Date.now() - t0) / 1000).toFixed(0)} s)   `)
    }
    ff.stdin.end()
    await done
    console.log(`\n${cam}: ${file} (${(fs.statSync(file).size / 1e6).toFixed(1)} MB)`)
    made.push(file)
  }
}
if (errors.length) console.log('console errors:', errors)
await browser.close()
await srv.close()
console.log(made.join('\n'))
