import { defineConfig } from 'vite'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// The trainer reads game exports, the Suave clips and music segments, and a local copy of the song at runtime, straight
// from their folders (read only). Paths live in config.local.json (git-ignored); the example file
// documents the defaults. Nothing is copied into the repo.
const here = path.dirname(fileURLToPath(import.meta.url))
function loadConfig() {
  const local = path.join(here, 'config.local.json')
  const example = path.join(here, 'config.example.json')
  const base = JSON.parse(fs.readFileSync(example, 'utf8'))
  if (fs.existsSync(local)) Object.assign(base, JSON.parse(fs.readFileSync(local, 'utf8')))
  return base
}

const MIME = {
  '.glb': 'model/gltf-binary', '.gltf': 'model/gltf+json', '.bin': 'application/octet-stream',
  '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
  '.mp3': 'audio/mpeg', '.wav': 'audio/wav', '.ogg': 'audio/ogg', '.m4a': 'audio/mp4', '.flac': 'audio/flac',
}
const AUDIO_EXT = new Set(['.mp3', '.wav', '.ogg', '.m4a', '.flac'])

// Resolve <root>/<rel> and refuse anything that escapes the root (lexically or through a junction).
function inside(root, rel) {
  if (!root || typeof rel !== 'string' || rel.includes('\0')) return null
  if (rel.split(/[\\/]+/).some(s => s === '..')) return null
  const full = path.resolve(root, rel.replaceAll('/', path.sep))
  const r = path.relative(root, full)
  if (!r || r.startsWith('..') || path.isAbsolute(r)) return null
  if (!fs.existsSync(full)) return null
  try {
    const real = fs.realpathSync.native(full)
    const rr = path.relative(fs.realpathSync.native(root), real)
    if (!rr || rr.startsWith('..') || path.isAbsolute(rr)) return null
    return fs.statSync(real).isFile() ? real : null
  } catch { return null }
}

function sendFile(req, res, full) {
  const stat = fs.statSync(full)
  res.setHeader('Content-Type', MIME[path.extname(full).toLowerCase()] || 'application/octet-stream')
  res.setHeader('Cache-Control', 'no-cache')
  res.setHeader('Last-Modified', stat.mtime.toUTCString())
  const range = req.headers.range && /bytes=(\d*)-(\d*)/.exec(req.headers.range)
  if (range) {
    const start = range[1] ? +range[1] : 0
    const end = range[2] ? +range[2] : stat.size - 1
    res.statusCode = 206
    res.setHeader('Content-Range', `bytes ${start}-${end}/${stat.size}`)
    res.setHeader('Content-Length', end - start + 1)
    if (req.method === 'HEAD') return res.end()
    return fs.createReadStream(full, { start, end }).pipe(res)
  }
  res.setHeader('Content-Length', stat.size)
  if (req.method === 'HEAD') return res.end()
  fs.createReadStream(full).on('error', e => { res.statusCode = 500; res.end(String(e)) }).pipe(res)
}

function json(res, obj, status = 200) {
  res.statusCode = status
  res.setHeader('Content-Type', 'application/json')
  res.setHeader('Cache-Control', 'no-cache')
  res.end(JSON.stringify(obj))
}

function listAudio(dir) {
  const out = []
  if (!dir || !fs.existsSync(dir)) return out
  const walk = (d, depth) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name)
      if (e.isDirectory() && depth < 3) walk(p, depth + 1)
      else if (e.isFile() && AUDIO_EXT.has(path.extname(e.name).toLowerCase()))
        out.push({ rel: path.relative(dir, p).split(path.sep).join('/'), bytes: fs.statSync(p).size, mtime: fs.statSync(p).mtimeMs })
    }
  }
  walk(dir, 0)
  return out
}

function listJson(dir) {
  const out = []
  if (!dir || !fs.existsSync(dir)) return out
  const walk = (d, depth) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name)
      if (e.isDirectory() && depth < 2) walk(p, depth + 1)
      else if (e.isFile() && e.name.toLowerCase().endsWith('.json')) out.push(path.relative(dir, p).split(path.sep).join('/'))
    }
  }
  walk(dir, 0)
  return out
}

function trainerServer() {
  const handler = (req, res, next) => {
    if (!req.url) return next()
    const url = decodeURIComponent(req.url.split('?')[0])
    const cfg = loadConfig()
    try {
      if (url === '/api/config') {
        const musicOk = !!cfg.musicPath && fs.existsSync(cfg.musicPath)
        return json(res, {
          todOk: fs.existsSync(cfg.todOut || ''),
          saDir: !!cfg.saExport && fs.existsSync(cfg.saExport),
          music: musicOk ? { url: '/music/local', segments: cfg.musicSegments || [] } : null,
        })
      }
      if (url === '/api/sa-manifest') {
        const f = cfg.saExport && path.join(cfg.saExport, 'manifest.json')
        if (!f || !fs.existsSync(f)) return json(res, { found: false })
        const stat = fs.statSync(f)
        let manifest = null, error = null
        try { manifest = JSON.parse(fs.readFileSync(f, 'utf8')) } catch (e) { error = String(e) }
        return json(res, { found: true, mtime: stat.mtimeMs, manifest, error })
      }
      if (url === '/api/sb-audio') {
        const dir = cfg.sbAudio
        const files = listAudio(dir)
        const jsons = listJson(dir)
        const meta = {}
        for (const j of jsons) {
          try { meta[j] = JSON.parse(fs.readFileSync(path.join(dir, j), 'utf8')) } catch { /* not json */ }
        }
        return json(res, { found: files.length > 0, files, meta })
      }
      if (url === '/music/local') {
        if (!cfg.musicPath || !fs.existsSync(cfg.musicPath)) { res.statusCode = 404; return res.end('no music configured') }
        return sendFile(req, res, cfg.musicPath)
      }
      const roots = { '/tod/': cfg.todOut, '/sa/': cfg.saExport, '/sb/': cfg.sbAudio }
      for (const [prefix, root] of Object.entries(roots)) {
        if (url.startsWith(prefix)) {
          const full = inside(root, url.slice(prefix.length))
          if (!full) { res.statusCode = 404; return res.end('not found') }
          return sendFile(req, res, full)
        }
      }
    } catch (e) {
      res.statusCode = 500
      return res.end(String(e))
    }
    next()
  }
  return {
    name: 'suave-trainer-server',
    configureServer(server) { server.middlewares.use(handler) },
    configurePreviewServer(server) { server.middlewares.use(handler) },
  }
}

export default defineConfig({
  plugins: [trainerServer()],
  server: { port: 6390, strictPort: true, host: '127.0.0.1' },
  preview: { port: 6391, host: '127.0.0.1' },
})
