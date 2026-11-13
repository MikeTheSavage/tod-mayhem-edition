// Quick look for development: load headless, step the camera smoothly, screenshot given times.
// usage: node scripts/smoke.mjs <out_dir> <t1> <t2> ... [--cam=front] [--debug=1] [--pairing=dea] [--ring=4]
import { ensureServer, launch, waitReady } from './browser.mjs'
import path from 'node:path'
import fs from 'node:fs'

const args = process.argv.slice(2)
const out = args[0]
fs.mkdirSync(out, { recursive: true })
const opt = Object.fromEntries(args.filter(a => a.startsWith('--')).map(a => a.slice(2).split('=')))
const times = args.slice(1).filter(a => !a.startsWith('--')).map(Number)
const srv = await ensureServer()
const { browser, page, errors, logs } = await launch({ width: +(opt.w || 1600), height: +(opt.h || 900) })
const q = new URLSearchParams({ render: '1', cam: opt.cam || 'front' })
for (const k of ['debug', 'pairing', 'ring', 'shooters']) if (opt[k]) q.set(k, opt[k])
q.set('w', opt.w || 1600); q.set('h', opt.h || 900)
await page.goto(`${srv.url}/?${q}`)
try {
  await waitReady(page, +(opt.timeout || 120000))
} catch (e) {
  console.log('NOT READY', await page.evaluate(() => document.getElementById('loadingText')?.textContent))
  console.log(errors.join('\n'))
  console.log(logs.slice(-30).join('\n'))
  await page.screenshot({ path: path.join(out, 'fail.png') })
  await browser.close(); await srv.close()
  process.exit(1)
}
let last = 0
for (const t of times) {
  for (let x = last; x < t; x += 1 / 15) await page.evaluate(x => window.__trainer.renderAt(x, 1 / 15), x)
  const info = await page.evaluate(t => window.__trainer.renderAt(t, 1 / 15), t)
  last = t
  const f = path.join(out, `${opt.cam || 'front'}_${t.toFixed(2)}.png`)
  await page.screenshot({ path: f })
  console.log(f, JSON.stringify(info))
}
console.log('errors:', errors.length ? errors.join('\n') : 'none')
await browser.close()
await srv.close()
