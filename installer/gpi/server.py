"""Local HTTP server (127.0.0.1, free port) serving the single-page UI and its JSON API. Stdlib only."""
import json
import mimetypes
import os
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import backup as B, catalog, engine, gamestate as GS, plan as P, procs, repo

APP_NAME = 'Gameplay+ Installer'


def prefs_path():
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    return os.path.join(base, 'Gameplay Plus Installer', 'prefs.json')


class App:
    def __init__(self, game=None, sandbox=None, registry=True, music_dir=None, idle_exit=45, serve_tests=False,
                 documents=None):
        self.game = game
        self.sandbox = os.path.normcase(os.path.abspath(sandbox)) if sandbox else None
        # the Documents folder of the display side file: --documents / GPI_DOCUMENTS, else the player's own; a
        # sandboxed run without either never touches any Documents folder ('')
        if documents or os.environ.get(GS.DOCS_ENV):
            self.documents = GS.documents_dir(documents)
        else:
            self.documents = '' if self.sandbox else None
        if self.sandbox and self.documents and not self.allowed(self.documents):
            self.documents = ''                 # a sandboxed run touches nothing outside the sandbox
        self.registry = registry
        self.music_dir = music_dir or os.path.join(repo.WEB, 'music')
        self.test_results = []
        self.serve_tests = serve_tests      # /tests/ = installer/tests (test pages; never in a normal run)
        self.job = None
        self.job_thread = None
        self.last_ping = None
        self.idle_exit = idle_exit
        self.lock = threading.Lock()

    # -- safety: a sandboxed run (tests) refuses any game folder / backup root outside the sandbox
    def allowed(self, path):
        if not self.sandbox or not path:
            return True
        p = os.path.normcase(os.path.abspath(path))
        return p == self.sandbox or p.startswith(self.sandbox + os.sep)

    def prefs(self):
        try:
            with open(prefs_path(), encoding='utf-8') as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def save_prefs(self, d):
        p = self.prefs()
        p.update({k: d[k] for k in ('muted', 'volume', 'last_backup_root') if k in d})
        os.makedirs(os.path.dirname(prefs_path()), exist_ok=True)
        with open(prefs_path(), 'w', encoding='utf-8') as f:
            json.dump(p, f)
        return p

    def state(self, game=None):
        cands = GS.candidates() if not self.sandbox else []
        game = game or self.game or (cands[0]['path'] if cands else '')
        ok = bool(game) and os.path.isfile(os.path.join(game, 'TOD.exe'))
        sel, how = GS.initial_selection(game) if ok else (catalog.default_selection(), 'defaults')
        st = GS.installed_state(game) if ok else {}
        root = None
        if ok:
            saved_bk = (st.get('saved_info') or {}).get('backup')
            root = os.path.dirname(saved_bk) if saved_bk else GS.default_backup_root(game)
        shipped = GS.read_text(repo.SHIPPED_INI)
        return dict(app=APP_NAME, game=game, game_ok=ok, candidates=cands, selection=sel, selection_from=how,
                    installed=st, backup_root=root, catalog=catalog.public(),
                    mayhem_title=P.mayhem_title_available(shipped), prefs=self.prefs(), sandbox=self.sandbox,
                    registry=self.registry, job=self.job.snapshot() if self.job else None,
                    backups=B.list_backups(root) if root else [], native=GS.native_size(),
                    music=self.music(), display_kept=GS.display_side_info(engine.side_file(self.documents)))

    STEMS = ['stem1_guitar.ogg', 'stem2_bass_perc.ogg', 'stem3_trumpet.ogg', 'stem4_strings_choir.ogg']

    def music(self):
        """The Mayhem stems in music_dir (read only): music.json's stem order (strings or {file|name|path}) or the four
        default names; each stem with its URL when the file exists. `meta` is music.json as it is (BPM, time
        signature, loop length, credit)."""
        meta = {}
        try:
            with open(os.path.join(self.music_dir, 'music.json'), encoding='utf-8') as f:
                meta = json.load(f)
        except (OSError, ValueError):
            meta = {}
        if not isinstance(meta, dict):
            meta = {}
        names = []
        for k, s in enumerate(meta.get('stems') or meta.get('stem_order') or []):
            if isinstance(s, dict):
                order = s.get('order')
                s = s.get('file') or s.get('path') or s.get('name')
                if not s and isinstance(order, int) and 1 <= order <= len(self.STEMS):
                    s = self.STEMS[order - 1]           # an entry without a file name: the default name of its slot
                elif not s and k < len(self.STEMS):
                    s = self.STEMS[k]
            if isinstance(s, str) and s:
                names.append(os.path.basename(s))
        names = names or list(self.STEMS)
        stems = []
        for n in names:
            ok = os.path.isfile(os.path.join(self.music_dir, n))
            stems.append(dict(file=n, url='/music/' + urllib.parse.quote(n) if ok else None))
        return dict(available=any(s['url'] for s in stems), stems=stems, meta=meta)

    def check(self, d):
        game, root = d.get('game'), d.get('backup_root')
        r = engine.check(game, d.get('selection'), root, d.get('backup_kind', 'full'), self.registry,
                         documents=self.documents)
        if not self.allowed(game) or not self.allowed(root):
            r['problems'].append('Test mode: this installer run is limited to %s.' % self.sandbox)
        if r.get('plan'):
            p = r['plan']
            r['effects'] = p['effects']
            r['summary'] = P.summary(p)
            r['plan'] = dict(data_needed=p['data_needed'], dll_needed=p['dll_needed'], data=p['data'],
                             ini=p['ini'], display=p['display'], physics_dll=p['physics_dll'], minimap=p['minimap'],
                             hdfx=bool(p['hdfx']), display_apply=p['display_apply'])
        if r.get('space'):
            for e in r['space']['drives'].values():
                e['need_h'], e['free_h'] = GS.human(e['need']), GS.human(e['free'])
            r['space']['backup_h'] = GS.human(r['space']['backup_bytes'])
        r['running_any'] = [dict(pid=p, path=q) for p, q in procs.any_game_process()]
        return r

    def start(self, kind, d):
        with self.lock:
            if self.job and self.job.state == 'running':
                return dict(error='A job is already running.')
            game = d.get('game')
            if not self.allowed(game) or not self.allowed(d.get('backup_root') or d.get('backup_dir')):
                return dict(error='Test mode: limited to %s.' % self.sandbox)
            if kind == 'install':
                job = engine.Job('install', engine.INSTALL_STEPS)
                args = (job, game, d.get('selection'), d.get('backup_root'), d.get('backup_kind', 'full'),
                        self.registry)
                kw = dict(documents=self.documents,
                          music_cache=engine.music_cache_dir(self.sandbox) if self.sandbox else None)
                target = engine.run_install
                if d.get('backup_root'):
                    self.save_prefs(dict(last_backup_root=d['backup_root']))
            else:
                job = engine.Job('restore', engine.RESTORE_STEPS)
                args = (job, game, d.get('backup_dir'), self.registry)
                kw = dict(documents=self.documents)
                target = engine.run_restore
            self.job = job
            self.job_thread = threading.Thread(target=target, args=args, kwargs=kw, daemon=True)
            self.job_thread.start()
            return dict(job=job.snapshot())

    def browse(self, d):
        """A native folder picker (PowerShell + WinForms, so it also works from a packed exe)."""
        start = d.get('start') or ''
        ps = ("Add-Type -AssemblyName System.Windows.Forms;"
              "$f = New-Object System.Windows.Forms.Form; $f.TopMost = $true;"
              "$d = New-Object System.Windows.Forms.FolderBrowserDialog;"
              "$d.Description = '%s'; $d.ShowNewFolderButton = $true;"
              "if (Test-Path -LiteralPath $env:GPI_START) { $d.SelectedPath = $env:GPI_START };"
              "if ($d.ShowDialog($f) -eq 'OK') { [Console]::Out.Write($d.SelectedPath) }"
              % (d.get('title', 'Pick a folder').replace("'", "''")))
        env = dict(os.environ, GPI_START=start)
        r = subprocess.run(['powershell', '-NoProfile', '-STA', '-Command', ps], capture_output=True, text=True,
                           env=env, creationflags=0x08000000)
        return dict(path=r.stdout.strip() or None)

    def browse_file(self, d):
        """A native file picker (PowerShell + WinForms OpenFileDialog; read only: nothing is opened or copied)."""
        start = d.get('start') or ''
        ps = ("Add-Type -AssemblyName System.Windows.Forms;"
              "$f = New-Object System.Windows.Forms.Form; $f.TopMost = $true;"
              "$d = New-Object System.Windows.Forms.OpenFileDialog;"
              "$d.Title = $env:GPI_TITLE; $d.Filter = $env:GPI_FILTER; $d.CheckFileExists = $true;"
              "if ($env:GPI_START -and (Test-Path -LiteralPath $env:GPI_START)) { $d.FileName = $env:GPI_START }"
              "elseif ($env:GPI_DIR -and (Test-Path -LiteralPath $env:GPI_DIR)) { $d.InitialDirectory = $env:GPI_DIR };"
              "if ($d.ShowDialog($f) -eq 'OK') { [Console]::OutputEncoding = [Text.Encoding]::UTF8; "
              "[Console]::Out.Write($d.FileName) }")
        env = dict(os.environ, GPI_START=start, GPI_TITLE=d.get('title', 'Pick a file'),
                   GPI_FILTER=d.get('filter') or 'All files|*.*',
                   GPI_DIR=os.path.join(os.path.expanduser('~'), 'Downloads'))
        r = subprocess.run(['powershell', '-NoProfile', '-STA', '-Command', ps], capture_output=True, env=env,
                           creationflags=0x08000000)
        return dict(path=r.stdout.decode('utf-8', 'replace').strip() or None)

    def song_check(self, d):
        """Does this file hold the song? (read + match only; the same refusals as the install, in plain words)"""
        path = (d.get('path') or '').strip()
        if not path:
            return dict(ok=False, empty=True)
        try:
            si = repo.song_import()
            fp = si.load_fingerprint(repo.SUAVE_FINGERPRINT)
            song, info = si.read_song(path)
            loc = si.locate(song, fp)
        except ImportError as e:
            return dict(ok=False, message='This copy of the installer cannot read audio files (%s).' % e)
        except Exception as e:                              # noqa: BLE001  (SongImportError: the text is for the player)
            return dict(ok=False, message=str(e))
        off = loc['offset_s']
        return dict(ok=True, offset_s=off, message='This is the song (%s, %.0f s%s). The music will be cut from it.' % (
            info['format'].split(' ')[0], info['seconds'],
            '' if abs(off) < 0.002 else ', the song starts %.2f s in' % off))

    def space(self, d):
        game, root = d.get('game'), d.get('backup_root')
        r = engine.space_needed(game, P.make_plan(d.get('selection'), GS.read_text(repo.SHIPPED_INI)), root,
                                d.get('backup_kind', 'full'))
        return r


def make_handler(app):
    class H(BaseHTTPRequestHandler):
        server_version = 'GameplayPlusInstaller/1'

        def log_message(self, fmt, *args):
            pass

        def send_json(self, obj, code=200):
            body = json.dumps(obj).encode('utf-8')
            self.send_response(code)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def body(self):
            n = int(self.headers.get('Content-Length') or 0)
            raw = self.rfile.read(n) if n else b''
            return raw

        def local_only(self):
            host = (self.headers.get('Host') or '').split(':')[0]
            if host not in ('127.0.0.1', 'localhost'):
                self.send_json(dict(error='local only'), 403)
                return False
            return True

        def do_GET(self):
            if not self.local_only():
                return
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            app.last_ping = time.time()
            if u.path == '/api/state':
                return self.send_json(app.state(q.get('game', [None])[0]))
            if u.path == '/api/job':
                return self.send_json(dict(job=app.job.snapshot() if app.job else None))
            if u.path == '/api/backups':
                return self.send_json(dict(backups=B.list_backups(q.get('root', [''])[0])))
            if u.path == '/api/ping':
                return self.send_json(dict(ok=True))
            if u.path == '/api/music':
                return self.send_json(app.music())
            path = u.path if u.path != '/' else '/index.html'
            base = repo.WEB
            if path.startswith('/music/'):
                base, path = app.music_dir, '/' + urllib.parse.unquote(path[len('/music/'):])
            elif path.startswith('/tests/') and app.serve_tests:
                base, path = os.path.join(os.path.dirname(repo.WEB), 'tests'), path[len('/tests'):]
            f = os.path.normpath(os.path.join(base, path.lstrip('/')))
            if not f.startswith(os.path.normpath(base) + os.sep) or not os.path.isfile(f):
                return self.send_json(dict(error='not found'), 404)
            with open(f, 'rb') as fh:
                data = fh.read()
            self.send_response(200)
            self.send_header('Content-Type', (mimetypes.guess_type(f)[0] or 'application/octet-stream') +
                             ('; charset=utf-8' if f.endswith(('.html', '.js', '.css')) else ''))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            if not self.local_only():
                return
            if (self.headers.get('Origin') or '').split('//')[-1].split(':')[0] not in ('', '127.0.0.1', 'localhost'):
                return self.send_json(dict(error='bad origin'), 403)
            u = urllib.parse.urlparse(self.path)
            app.last_ping = time.time()
            try:
                d = json.loads(self.body() or b'{}')
            except ValueError:
                return self.send_json(dict(error='bad json'), 400)
            try:
                if u.path == '/api/check':
                    return self.send_json(app.check(d))
                if u.path == '/api/install':
                    return self.send_json(app.start('install', d))
                if u.path == '/api/restore':
                    return self.send_json(app.start('restore', d))
                if u.path == '/api/browse':
                    return self.send_json(app.browse(d))
                if u.path == '/api/browse-file':
                    return self.send_json(app.browse_file(d))
                if u.path == '/api/song':
                    return self.send_json(app.song_check(d))
                if u.path == '/api/prefs':
                    return self.send_json(app.save_prefs(d))
                if u.path == '/api/test-result' and app.serve_tests:   # test pages report here (tests only)
                    app.test_results.append(d)
                    return self.send_json(dict(ok=True))
                if u.path == '/api/log':                      # client-side errors land in the console log
                    sys.stderr.write('[ui] %s\n' % d.get('msg'))
                    return self.send_json(dict(ok=True))
            except Exception as e:                            # noqa: BLE001
                return self.send_json(dict(error='%s: %s' % (type(e).__name__, e)), 500)
            return self.send_json(dict(error='not found'), 404)
    return H


class Server(ThreadingHTTPServer):
    request_queue_size = 64          # a page load fires several requests at once; the data build keeps the GIL busy
    daemon_threads = True


def serve(app, port=0):
    httpd = Server(('127.0.0.1', port), make_handler(app))
    httpd.daemon_threads = True
    return httpd


def watchdog(app, httpd):
    """Quit when the window is gone: no request for idle_exit seconds after the first one, and no job running."""
    while True:
        time.sleep(3)
        if app.idle_exit and app.last_ping and time.time() - app.last_ping > app.idle_exit and \
                not (app.job and app.job.state == 'running'):
            httpd.shutdown()
            return
