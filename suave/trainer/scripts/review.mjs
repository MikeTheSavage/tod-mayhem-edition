// Shot review: one frame per shot of the cuts camera plus 3 frames inside each slow-motion shot, for
// every pairing, labelled, tiled into proof/shots_review.png (ffmpeg).  usage: node scripts/review.mjs
import { ensureServer, launch, waitReady, ROOT } from './browser.mjs'
import { execSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'

const W = 640, H = 360
const tmp = path.join(ROOT, 'proof', '_frames')
fs.rmSync(tmp, { recursive: true, force: true }); fs.mkdirSync(tmp, { recursive: true })
const ffmpeg = process.env.FFMPEG_PATH || execSync('python -c "import imageio_ffmpeg;print(imageio_ffmpeg.get_ffmpeg_exe())"').toString().trim()

const srv = await ensureServer()
const { browser, page, errors } = await launch({ width: W, height: H })
await page.goto(`${srv.url}/?render=1&cam=cuts&w=${W}&h=${H}`)
await waitReady(page)
await page.evaluate(() => {
  const d = document.createElement('div'); d.id = 'reviewLabel'
  d.style.cssText = 'position:fixed;left:6px;bottom:6px;z-index:99;font:700 15px Segoe UI,sans-serif;color:#fff;background:rgba(0,0,0,.6);padding:2px 7px;border-radius:4px'
  document.body.appendChild(d)
})
let n = 0
const pairings = ['soldier', 'civil', 'dea']
for (const pairing of pairings) {
  await page.evaluate(p => { window.__trainer.setPairing(p); window.__trainer.setPreset('cuts') }, pairing)
  const T = await page.evaluate(() => window.__trainer.times())
  const B = 60 / 124.17, D = T.end
  const bar = k => T.musicZero + k * 4 * B + 0.9   // inside the k-th bar of the dance
  const shots = [
    ['1 push-in', T.introBeat], ['2 intro OTS', 3 * T.introBeat], ['3 two-shot', 5 * T.introBeat], ['gather wide', (T.intro + T.dance0) / 2],
    ['dance hero', bar(1)], ['dance OTS Ram', bar(2)], ['dance member', bar(3)], ['dance OTS partner', bar(4)], ['dance wide', bar(5)],
    ['4a belt', T.shot4a + 0.5],
    ['4b a', T.shot4b + 0.4], ['4b b (slow)', T.jump + 0.05], ['4b c (slow)', (T.jump + T.shot5) / 2], ['4b d (slow)', T.shot5 - 0.05],
    ['5 a (slow)', T.shot5 + 0.15], ['5 b (slow)', (T.shot5 + T.shot6) / 2], ['5 c (slow)', T.shot6 - 0.05],
    ['6 a (slow)', T.shot6 + 0.2], ['6 b boom', T.boom + 0.35], ['6 c landing', T.land + 0.3], ['6 end', D - 0.2],
  ]
  let last = 0
  for (const [label, t] of shots) {
    await page.evaluate(([a, b]) => { const tr = window.__trainer; for (let x = a; x < b; x += 1 / 20) tr.renderAt(x, 1 / 20); tr.renderAt(b, 1 / 20) }, [last, t])
    last = t
    await page.evaluate(s => { document.getElementById('reviewLabel').textContent = s }, `${pairing} | ${label} | ${t.toFixed(2)} s`)
    await page.screenshot({ path: path.join(tmp, `f_${String(n++).padStart(3, '0')}.png`) })
  }
}
await browser.close(); await srv.close()
const cols = 7, rows = Math.ceil(n / cols)
const out = path.join(ROOT, 'proof', 'shots_review.png')
execSync(`"${ffmpeg}" -loglevel error -y -framerate 1 -i "${path.join(tmp, 'f_%03d.png')}" -vf "scale=448:252,tile=${cols}x${rows}:padding=4:color=black" -frames:v 1 "${out}"`)
console.log(out, n, 'frames', errors.length ? errors : 'no console errors')
