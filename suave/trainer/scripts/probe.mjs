// Debug helper: load the app headless, report readiness, errors and one renderAt timing.
import { ensureServer, launch } from './browser.mjs'
const srv = await ensureServer()
const { browser, page, errors, logs } = await launch()
await page.goto(srv.url + '/?render=1&w=1280&h=720')
const t0 = Date.now()
for (let i = 0; i < 90; i++) {
  const st = await Promise.race([
    page.evaluate(() => ({ ready: !!window.__trainer?.ready, text: document.getElementById('loadingText')?.textContent })),
    new Promise(r => setTimeout(() => r({ hung: true }), 5000)),
  ])
  if (st.hung) {
    console.log('page hung at', (Date.now() - t0) / 1000, 's')
    const cdp = await page.context().newCDPSession(page)
    await cdp.send('Debugger.enable')
    const paused = new Promise(r => cdp.once('Debugger.paused', r))
    await cdp.send('Debugger.pause')
    const ev = await paused
    for (const f of ev.callFrames.slice(0, 12)) console.log('  at', f.functionName, f.url.split('/').pop(), f.location.lineNumber + 1)
    process.exit(0)
  }
  if (st.ready) { console.log('ready after', (Date.now() - t0) / 1000, 's'); break }
  if (i % 5 === 0) console.log(st.text)
  await new Promise(r => setTimeout(r, 1000))
}
for (const t of [1, 10, 15.5, 17]) {
  const a = Date.now()
  const r = await Promise.race([page.evaluate(t => window.__trainer.renderAt(t, 1 / 30), t), new Promise(r => setTimeout(() => r('HUNG'), 10000))])
  console.log('renderAt', t, Date.now() - a, 'ms', JSON.stringify(r).slice(0, 160))
}
console.log('errors:', errors.slice(0, 5))
console.log(logs.slice(-5).join('\n'))
await browser.close(); await srv.close()
process.exit(0)
