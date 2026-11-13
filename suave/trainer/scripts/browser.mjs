// Shared headless-browser helpers: system Chrome via playwright-core (Defender quarantines the
// bundled Chromium on this machine), and a Vite dev server started in-process when none is running.
import { chromium } from 'playwright-core'
import { createServer } from 'vite'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const CHROME = [
  process.env.CHROME_PATH,
  'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
  'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
  'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
].filter(Boolean)

export async function ensureServer(port = 6390) {
  const url = `http://127.0.0.1:${port}`
  try { const r = await fetch(url + '/api/config'); if (r.ok) return { url, close: async () => {} } } catch { /* not running */ }
  const server = await createServer({ root: ROOT, configFile: path.join(ROOT, 'vite.config.mjs'), server: { port, strictPort: true, host: '127.0.0.1' }, logLevel: 'warn' })
  await server.listen()
  return { url, close: () => server.close() }
}

export async function launch({ width = 1600, height = 900, headless = true } = {}) {
  const exe = CHROME.find(p => fs.existsSync(p))
  const browser = await chromium.launch({
    executablePath: exe, headless,
    args: ['--use-angle=d3d11', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required', '--mute-audio'],
  })
  const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: 1 })
  const errors = [], logs = []
  page.on('console', m => { const s = `[${m.type()}] ${m.text()}`; logs.push(s); if (m.type() === 'error') errors.push(s) })
  page.on('pageerror', e => errors.push('[pageerror] ' + e.message))
  return { browser, page, errors, logs }
}

export async function waitReady(page, timeout = 180000) {
  await page.waitForFunction(() => window.__trainer && window.__trainer.ready, null, { timeout, polling: 250 })
}
