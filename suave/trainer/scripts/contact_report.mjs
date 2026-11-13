// Hand contact of the Suave pair clips as the trainer plays them, per pairing (prints JSON).
import { ensureServer, launch, waitReady } from './browser.mjs'
const srv = await ensureServer()
const { browser, page, errors } = await launch()
await page.goto(srv.url + '/?render=1')
await waitReady(page)
const out = {}
for (const p of ['soldier', 'civil', 'dea']) {
  out[p] = await page.evaluate(p => { window.__trainer.setPairing(p); const i = window.__trainer.info(); return { danceSource: !!i.danceSource, stats: i.contactStats } }, p)
}
console.log(JSON.stringify(out, (k, v) => typeof v === 'number' ? +v.toFixed(3) : v, 1))
console.log('errors:', errors.length ? errors : 'none')
await browser.close(); await srv.close()
