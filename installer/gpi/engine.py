"""The install and restore jobs: steps with live status, a log file in the backup folder.

Install order (the same as `Install Gameplay Plus Update.cmd`, plus the full backup and clean switch-off):
  preflight -> backup (+ verify) -> build the data zip from the player's own game files -> seamless prefetch table ->
  data zip into configpc.txt (install_gameplay_mod.py) -> DLLs -> TODGameplay.ini (merge_ini.py) -> display lines ->
  difficulty marker -> save the selection and check every installed file.
"""
import contextlib
import datetime
import io
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import traceback

from . import backup as B, catalog, gamestate as GS, plan as P, procs, repo

EIDOS_KEY = r'Software\Eidos\Total Overdose'


class StepError(Exception):
    pass


class Job:
    def __init__(self, kind, steps):
        self.kind = kind
        self.id = '%s-%d' % (kind, int(time.time() * 1000))
        self.steps = [dict(id=i, label=l, status='pending', detail='') for i, l in steps]
        self.state = 'running'
        self.progress = None
        self.lines = []
        self.result = None
        self.error = None
        self.started = time.time()
        self.finished = None
        self.log_path = None
        self.flags = {}               # optional steps this install runs (hdfx, minimap): the page shows them
        self._buf = []
        self._lock = threading.Lock()

    def step(self, sid, status, detail=None):
        with self._lock:
            for s in self.steps:
                if s['id'] == sid:
                    s['status'] = status
                    if detail is not None:
                        s['detail'] = detail
            self.progress = None

    def log(self, msg):
        line = '%s  %s' % (datetime.datetime.now().strftime('%H:%M:%S'), msg)
        with self._lock:
            self.lines.append(line)
            if len(self.lines) > 600:
                self.lines = self.lines[-600:]
            if self.log_path:
                with open(self.log_path, 'a', encoding='utf-8') as f:
                    for b in self._buf:
                        f.write(b + '\n')
                    f.write(line + '\n')
                self._buf = []
            else:
                self._buf.append(line)

    def set_log(self, path):
        self.log_path = path
        self.log('log file: %s' % path)

    def snapshot(self):
        with self._lock:
            return dict(id=self.id, kind=self.kind, state=self.state, steps=[dict(s) for s in self.steps],
                        progress=self.progress, log=self.lines[-200:], result=self.result, error=self.error,
                        log_path=self.log_path, flags=dict(self.flags), seconds=round((self.finished or time.time()) - self.started, 1))


# ------------------------------------------------------------------ registry (the game's own key, HKCU)
def reg_export(path):
    r = subprocess.run(['reg', 'export', 'HKCU\\' + EIDOS_KEY, path, '/y'], capture_output=True, text=True,
                       creationflags=0x08000000)
    return r.returncode == 0


def reg_import(path):
    r = subprocess.run(['reg', 'import', path], capture_output=True, text=True, creationflags=0x08000000)
    return 'imported' if r.returncode == 0 else 'import failed: %s' % (r.stderr or r.stdout).strip()


def reg_set_size(w, h):
    import winreg
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, EIDOS_KEY) as k:
        winreg.SetValueEx(k, 'XRes', 0, winreg.REG_DWORD, int(w))
        winreg.SetValueEx(k, 'YRes', 0, winreg.REG_DWORD, int(h))


# ------------------------------------------------------------------ helpers
def is_our_dsound(path):
    try:
        with open(path, 'rb') as f:
            return b'TODGameplay' in f.read()
    except OSError:
        return False


def hdfx_state(game_dir):
    """What the game folder holds for the HD effects: 'none', 'ours' (a TOD HD d3d9.dll with the Gameplay+ ini or no
    ini), 'todhd' (a TOD HD install with its own ini: never touched) or 'foreign' (another d3d9.dll)."""
    d3d = os.path.join(game_dir, P.HDFX_DLL)
    if not os.path.isfile(d3d):
        return 'none'
    hs = repo.hdfx_stage()
    ini = os.path.join(game_dir, P.HDFX_INI)
    if hs.is_todhd_dll(d3d):
        return 'ours' if (not os.path.isfile(ini) or hs.is_ours_ini(ini)) else 'todhd'
    return 'foreign'


def hdfx_remove(game_dir, log):
    """HD effects off: remove exactly the files the feature owns (only when they are ours) and put a set-aside
    d3d9.dll back."""
    st = hdfx_state(game_dir)
    removed = []
    if st == 'ours':
        for rel in P.HDFX_FILES:
            p = os.path.join(game_dir, *rel.split('/'))
            if os.path.isfile(p):
                os.remove(p)
                removed.append(rel)
        B.remove_empty_dirs(game_dir, P.HDFX_FILES)
    aside = os.path.join(game_dir, P.HDFX_ASIDE)
    if os.path.isfile(aside) and not os.path.isfile(os.path.join(game_dir, P.HDFX_DLL)):
        os.replace(aside, os.path.join(game_dir, P.HDFX_DLL))
        log('put the d3d9.dll that was there before the HD effects back')
    if removed:
        log('HD effects removed: %d files (d3d9.dll, tod_hd_fx.ini, tod_hd_light.txt, tod_hd_fx\\)' % len(removed))
    return removed


def default_hdfx(game_dir, staging, sets, user_ini, log):
    """Stage the HD effects for this game folder (manifests + sky irradiance from its own archives)."""
    hs = repo.hdfx_stage()
    res = hs.stage(game_dir, staging, sets, user_ini, log=log)
    return res, hs.staged_files(staging)


MINIMAP_MAPS = 9            # maps the bake lists in the shipped game (for the progress bar; the bake prints each one)


def default_minimap(game_dir, out_dir, log, progress):
    """Bake the maps from the player's own game files: `python -I hud\\tools\\bake_minimap.py --game G
    --out <G>\\TODGameplay_minimap` as a child process (a packed exe without Python runs it in-process; either way it
    calls dist\\TODMinimapBake.exe, which uses the cores). One log line per map; progress = maps done of MINIMAP_MAPS.
    A map whose style or sources are unchanged is skipped by the tool. Returns {maps, seconds}."""
    script = os.path.join(repo.HUD_TOOLS, 'bake_minimap.py')
    args = ['--game', game_dir, '--out', out_dir]
    done, listed = [0], [0]

    def line(ln):
        ln = ln.rstrip()
        if not ln:
            return
        log('minimap: ' + ln)
        if ln.startswith('done:'):
            try:
                listed[0] = int(ln.split()[1])
            except (IndexError, ValueError):
                pass
        elif (' px at ' in ln and ': ' in ln) or ln.endswith(': up to date') or ln.endswith('skipped'):
            done[0] += 1
            progress(min(done[0], MINIMAP_MAPS), max(MINIMAP_MAPS, done[0]))
    t0 = time.time()
    progress(0, MINIMAP_MAPS)
    if getattr(sys, 'frozen', False):
        bm = repo.bake_minimap()

        class W(io.TextIOBase):
            buf = ''

            def write(self, s):
                self.buf += s
                while '\n' in self.buf:
                    a, self.buf = self.buf.split('\n', 1)
                    line(a)
                return len(s)
        with contextlib.redirect_stdout(W()):
            rc = bm.main(args)
    else:
        with subprocess.Popen([sys.executable, '-I', '-u', script] + args, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, cwd=repo.HUD_TOOLS, creationflags=0x08000000, text=True,
                              encoding='utf-8', errors='replace', env=dict(os.environ)) as p:   # -u: -I ignores PYTHONUNBUFFERED
            for ln in p.stdout:
                line(ln)
            rc = p.wait()
    if rc:
        raise StepError('bake_minimap.py failed (exit %s)' % rc)
    return dict(maps=listed[0] or done[0], seconds=time.time() - t0)


def minimap_remove(game_dir, log):
    """Minimap off: remove exactly the files the bake made (backup.minimap_files), then the folder if it is empty."""
    removed = B.minimap_files(game_dir)
    for rel in removed:
        os.remove(os.path.join(game_dir, *rel.split('/')))
    B.remove_empty_dirs(game_dir, [P.MINIMAP_DIR + '/x'])
    if removed:
        log('minimap removed: %d files of %s\\ (the ones the bake made)' % (len(removed), P.MINIMAP_DIR))
    return removed


def space_needed(game_dir, plan, backup_root, kind):
    """{drive: dict(need, free, what)} for the backup, the staging folder and the game folder."""
    files = B.backup_files(game_dir, kind) if os.path.isdir(game_dir) else []
    bk = sum(s for _, s in files)
    zip_est = 0
    if plan['data_needed']:
        z = os.path.join(game_dir, P.ZIP)
        zip_est = max(os.path.getsize(z) if os.path.isfile(z) else 0, 160 << 20) + (40 << 20)
        if plan['data'].get('hud_textures', 'off') != 'off':
            zip_est += 32 << 20                  # HD HUD icons: overdose.main about 28 MB bigger
    need = {}

    def add(path, n, what):
        d = GS.drive_of(path)
        e = need.setdefault(d, dict(need=0, free=GS.free_bytes(path), what=[]))
        e['need'] += n
        if n:
            e['what'].append('%s %s' % (what, GS.human(n)))
    add(backup_root, bk, 'backup')
    add(backup_root, zip_est, 'data build')
    add(game_dir, zip_est + (16 << 20) + ((48 << 20) if plan.get('minimap') else 0), 'game folder')
    for e in need.values():
        e['ok'] = e['free'] >= e['need'] + (64 << 20)
    return dict(drives=need, backup_bytes=bk, backup_files=len(files))


def side_file(documents):
    """The display side file for a run: documents None = the player's Documents (real installs), '' = none (sandboxed
    test runs without --documents: the Documents folder is never touched)."""
    docs = GS.documents_dir() if documents is None else documents
    return GS.display_side_path(docs) if docs else None


def check(game_dir, selection, backup_root, kind, registry=True, lister=procs.list_processes, documents=None):
    """Everything the bottom bar shows; `problems` blocks the install, `warnings` do not."""
    problems, warnings = [], []
    if not game_dir or not os.path.isfile(os.path.join(game_dir, 'TOD.exe')):
        problems.append('No TOD.exe in the game folder. Pick the Total Overdose folder.')
        return dict(problems=problems, warnings=warnings, exe=None, plan=None, space=None, running=[],
                    display_kept=None)
    exe = GS.exe_check(game_dir)
    if not exe['match']:
        warnings.append(exe['text'])
    running = procs.game_processes(game_dir, lister)
    if running:
        problems.append('Total Overdose is running. Quit the game first: nothing is installed or restored while it '
                        'runs.')
    shipped = GS.read_text(repo.SHIPPED_INI)
    plan = P.make_plan(selection, shipped, GS.native_size(), registry)
    if not plan['selection']['groups']['essentials']:
        warnings.append(catalog.BY_ID['essentials']['warn_off'])
    ds = os.path.join(game_dir, 'dsound.dll')
    if os.path.isfile(ds) and not is_our_dsound(ds):
        problems.append('The game folder has another dsound.dll (not the Gameplay+ loader). Rename it and set '
                        '[loader] chain_dsound in TODGameplay.ini (dll\\README.md), then install.')
    if plan.get('song'):
        if not os.path.isfile(plan['song']):
            problems.append('Your song file for Suave was not found (%s). Pick it again, or clear it: Suave then '
                            'installs without music.' % plan['song'])
        elif not os.path.isfile(repo.SUAVE_FINGERPRINT):
            problems.append('Missing suave\\audio\\fingerprint.json (needed to find the song in your file).')
        else:
            try:
                repo.song_import()._sf()
            except Exception as e:                          # noqa: BLE001  (numpy / soundfile not installed)
                problems.append('This copy of the installer cannot read audio files (%s: %s). Clear the song file '
                                'to install Suave without music, or use the packed installer.' % (type(e).__name__, e))
    if plan.get('hdfx'):
        st = hdfx_state(game_dir)
        if st == 'todhd':
            problems.append('The game folder has a TOD HD install (its d3d9.dll and its own tod_hd_fx.ini). The HD '
                            'Effects group would replace it: turn the group off, or remove TOD HD from this folder first.')
        elif st == 'foreign':
            if os.path.isfile(os.path.join(game_dir, P.HDFX_ASIDE)):
                problems.append('The game folder has another d3d9.dll and an older %s. Move one of them away, then '
                                'install.' % P.HDFX_ASIDE)
            else:
                warnings.append('The game folder has another d3d9.dll (not the HD effects). It is set aside as %s '
                                '(and kept in the backup) and put back when HD Effects are turned off.' % P.HDFX_ASIDE)
        if not os.path.isfile(os.path.join(repo.HDFX, 'release', 'd3d9.dll')):
            problems.append('Missing hdfx\\release\\d3d9.dll (the HD effects DLL).')
    for f in tuple(P.DLL_FILES) + ((P.PHYSICS_DLL, P.PHYSICS_MAP) if plan['physics_dll'] else ()) + \
            (tuple(P.MAYHEM_DDS) if plan['mayhem_dds'] else ()):
        if not os.path.isfile(os.path.join(repo.DIST, f)):
            problems.append('Missing %s in %s (build the DLLs first).' % (f, repo.DIST))
    if plan['minimap'] and not os.path.isfile(os.path.join(repo.HUD_TOOLS, 'bake_minimap.py')):
        problems.append('Missing hud\\tools\\bake_minimap.py (the minimap bake).')
    if plan['minimap'] and not os.path.isfile(os.path.join(repo.DIST, 'TODMinimapBake.exe')):
        problems.append('Missing TODMinimapBake.exe in %s (the minimap bake; build the DLLs first).' % repo.DIST)
    space = None
    if backup_root:
        if os.path.normcase(os.path.abspath(backup_root)).startswith(os.path.normcase(os.path.abspath(game_dir)) + os.sep):
            problems.append('The backup folder must not be inside the game folder.')
        space = space_needed(game_dir, plan, backup_root, kind)
        for d, e in space['drives'].items():
            if not e['ok']:
                problems.append('Not enough space on %s: needs %s (%s), %s free. Pick a backup location on another '
                                'drive%s.' % (d, GS.human(e['need']), ', '.join(e['what']), GS.human(e['free']),
                                              ' or choose "changed files only"' if kind == 'full' else ''))
    else:
        problems.append('Pick a backup location.')
    st = GS.installed_state(game_dir)
    if st.get('static_difficulty'):
        warnings.append('The installed data zip has the old static difficulty "%s" baked in. This install rebuilds it '
                        'with difficulty = normal; pick the level in SETTINGS instead.' % st['static_difficulty'])
    return dict(problems=problems, warnings=warnings, exe=exe, plan=plan, space=space,
                running=[dict(pid=p, path=q) for p, q in running],
                display_kept=GS.display_side_info(side_file(documents)))


def write_configpc_display(config, fullscreen, w, h, log):
    gfs = repo.gamefs()
    igm = repo.data_installer()
    lines, nl, final = gfs.read_config_lines(config)
    out, has_f, has_s = [], False, False
    fs = 'true' if fullscreen else 'false'
    for ln in lines:
        s = ln.strip().lower()
        if s.startswith('fullscreen') and '=' in s and s.split('=')[0].strip() == 'fullscreen':
            out.append('fullscreen = %s' % fs)
            has_f = True
        elif s.startswith('screensize') and '=' in s and s.split('=')[0].strip() == 'screensize':
            out.append('screensize = %d,%d' % (w, h))
            has_s = True
        else:
            out.append(ln)
    if not has_s:
        out.insert(0, 'screensize = %d,%d' % (w, h))
    if not has_f:
        out.insert(0, 'fullscreen = %s' % fs)
    if out != lines:
        igm.write_lines(config, out, nl, True)
        log('configpc.txt: fullscreen = %s, screensize = %d,%d' % (fs, w, h))
    else:
        log('configpc.txt: display lines already fullscreen = %s, screensize = %d,%d' % (fs, w, h))


def merge_ini(game_dir, plan, staging, log):
    """Apply the plan's ini keys with seamless/tools/merge_ini.py on a staged copy, then swap it in."""
    mi = repo.merge_ini()
    ini = os.path.join(game_dir, P.INI)
    if not os.path.isfile(ini):
        if not plan['dll_needed']:
            log('TODGameplay.ini: none and no DLL feature selected: nothing to write')
            return []
        B.replace_file(repo.SHIPPED_INI, ini)
        log('TODGameplay.ini: none yet, copied the shipped one (every feature off) before the merge')
    text = GS.read_text(ini)
    sets, skipped, retired = [], [], []
    for sec, key, val, mode in plan['ini']:
        if mode == 'retired':                         # a feature that no longer ships: only an older ini has it
            if P.ini_has(text, sec, key):
                retired.append((sec, key, val))
            continue
        if mode == 'if_present' and not P.ini_has(text, sec, key):
            skipped.append('[%s] %s' % (sec, key))
            continue
        sets.append('%s.%s=%s' % (sec, key, val))
    os.makedirs(staging, exist_ok=True)
    tmp = os.path.join(staging, P.INI)
    shutil.copyfile(ini, tmp)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = mi.main([tmp, repo.SHIPPED_INI] + sets)
    for ln in buf.getvalue().splitlines():
        if not ln.startswith('written:') and not ln.startswith('unchanged:'):
            log('ini ' + ln)
    if rc:
        raise StepError('merge_ini.py failed (%s)' % rc)
    if retired:
        with open(tmp, encoding='utf-8', errors='surrogateescape', newline='') as f:
            t = f.read()
        for sec, key, val in retired:
            t = P.retire_key(t, sec, key, val)
            log('ini: [%s] %s = %s (that feature is gone)' % (sec, key, val))
        with open(tmp, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
            f.write(t)
    with open(tmp, 'rb') as a, open(ini, 'rb') as b:
        same = a.read() == b.read()
    if not same:
        B.replace_file(tmp, ini)
        log('TODGameplay.ini written (%d keys set, every other line kept)' % len(sets))
    else:
        log('TODGameplay.ini unchanged')
    if skipped:
        log('ini: already off (key not in your ini): %s' % ', '.join(skipped))
    return sets


def music_cache_dir(base=None):
    """Per-user cache of the Suave segments cut from the player's song (never the game folder)."""
    base = base or os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    return os.path.join(base, 'Gameplay Plus Installer', 'suave_music')


def default_song_import(song, out_dir, log):
    """Cut the four segments from the player's copy of the song -> the manifest path. A refusal is a StepError whose
    text is for the player."""
    si = repo.song_import()
    try:
        si.import_song(song, out_dir, repo.SUAVE_FINGERPRINT, log=log)
    except si.SongImportError as e:
        raise StepError('Your song file was refused: %s Nothing was changed. Clear the song file box to install Suave '
                        'without music, or pick the right file.' % e)
    return os.path.join(out_dir, 'manifest.json')


def default_build(game_dir, data, out_zip, report_dir, log, suave_manifest=None):
    b = repo.builder()
    rep = b.build(game_dir, data, out_zip, report_dir, log=log, suave_manifest=suave_manifest)
    if rep.get('result') != 'PASS':
        raise StepError('data build: %s' % rep.get('result'))
    return rep


def default_prefetch(game_dir, zip_path, out, log):
    mp = repo.prefetch()
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = mp.main(game_dir, zip_path, out)
    for ln in buf.getvalue().splitlines():
        log(ln)
    if rc:
        raise StepError('make_prefetch_table.py failed (%s)' % rc)


INSTALL_STEPS = [
    ('preflight', 'Check the game folder'),
    ('music', 'Suave music from your copy of the song'),
    ('backup', 'Back up the game folder'),
    ('build', 'Build the data zip from your game files'),
    ('prefetch', 'Seamless prefetch table'),
    ('data', 'Data zip into configpc.txt'),
    ('dlls', 'Gameplay+ DLLs'),
    ('ini', 'TODGameplay.ini'),
    ('hdfx', 'HD effects'),
    ('minimap', 'Minimap from your game files'),
    ('display', 'Display settings'),
    ('difficulty', 'Difficulty marker'),
    ('finish', 'Save the selection and check the files'),
]


def run_install(job, game_dir, selection, backup_root, kind, registry=True, lister=procs.list_processes,
                build_fn=default_build, prefetch_fn=default_prefetch, hdfx_fn=default_hdfx,
                minimap_fn=default_minimap, documents=None, song_fn=default_song_import, music_cache=None):
    """The install job body (runs in a thread). Raises nothing: the job's state says how it ended.
    documents: the Documents folder for the display side file (None = the player's own, '' = never touched).
    song_fn / music_cache: the Suave music import (the player's song file -> segments in the cache folder)."""
    cur = [None]

    def step(sid, status, detail=None):
        cur[0] = sid if status == 'running' else cur[0]
        job.step(sid, status, detail)

    def progress(done, total):
        job.progress = dict(done=done, total=total)

    def guard():
        if procs.game_processes(game_dir, lister):
            raise StepError('Total Overdose started while installing: stopped before touching the game folder '
                            'further. Quit it and use Restore if anything was changed.')

    staging = None
    bk_dir = None
    try:
        step('preflight', 'running')
        c = check(game_dir, selection, backup_root, kind, registry, lister, documents)
        if c['problems']:
            raise StepError(' '.join(c['problems']))
        plan = c['plan']
        job.flags = dict(hdfx=bool(plan['hdfx']), minimap=bool(plan['minimap']))
        side = side_file(documents) if plan['display_apply'] else None
        for w in c['warnings']:
            job.log('warning: ' + w)
        job.log('game folder: %s' % game_dir)
        job.log('exe: %s' % c['exe']['text'])
        job.log('selection on: %s' % ', '.join(P.summary(plan)['on']))
        job.log('selection off: %s' % ', '.join(P.summary(plan)['off']))
        step('preflight', 'warn' if c['warnings'] else 'done',
             'exe matches' if c['exe']['match'] else 'exe differs: DLL features inactive')

        # the player's song, cut on this PC into the per-user cache; a refused file stops the install here, before
        # anything is backed up or written
        step('music', 'running')
        manifest = None
        if plan['song']:
            manifest = song_fn(plan['song'], music_cache or music_cache_dir(), job.log)
            step('music', 'done', 'four segments cut from your copy')
        elif plan['selection']['groups']['suave']:
            job.log('suave: no song file given: installed without music ([suave] music = 0)')
            step('music', 'skipped', 'no song file: Suave without music')
        else:
            step('music', 'skipped', 'Suave is off')

        step('backup', 'running')
        os.makedirs(backup_root, exist_ok=True)
        before = GS.installed_state(game_dir)
        bk_dir, man = B.make_backup(game_dir, backup_root, kind, job.log, progress,
                                    extra=dict(selection_before=before.get('saved') or before.get('detected'),
                                               note='before installing Gameplay+'))
        job.set_log(os.path.join(bk_dir, 'install.log'))
        if plan['registry']:
            ok = reg_export(os.path.join(bk_dir, 'eidos.reg'))
            job.log('registry key exported: %s' % ('yes' if ok else 'no (key missing)'))
        if side and os.path.isfile(side):
            B.backup_side_file(bk_dir, side, job.log)
        elif plan['display_apply']:
            job.log('display side file: %s' % ('none yet (%s): nothing to replace' % side if side else
                                               'not touched (test mode: no Documents folder given)'))
        step('backup', 'done', '%s: %d files, %s, verified' % ('full' if kind == 'full' else 'changed files only',
                                                               len(man['files']), GS.human(man['bytes'])))
        staging = os.path.join(backup_root, '_staging')
        if os.path.isdir(staging):
            shutil.rmtree(staging, ignore_errors=True)
        os.makedirs(staging)
        zip_out = os.path.join(staging, P.ZIP)

        step('build', 'running')
        if plan['data_needed']:
            sel = {k: v for k, v in plan['data'].items()}
            job.log('data options: %s' % ', '.join('%s=%s' % kv for kv in sorted(sel.items())))
            t0 = time.time()
            build_fn(game_dir, sel, zip_out, os.path.splitext(zip_out)[0] + '.report', job.log,
                     **(dict(suave_manifest=manifest) if manifest else {}))
            step('build', 'done', '%s in %.0f s' % (GS.human(os.path.getsize(zip_out)), time.time() - t0))
        else:
            step('build', 'skipped', 'every data option is vanilla: the data zip is removed')

        step('prefetch', 'running')
        pre_out = os.path.join(staging, P.PREFETCH)
        if plan['prefetch'] and plan['data_needed']:
            prefetch_fn(game_dir, zip_out, pre_out, job.log)
            step('prefetch', 'done', 'rebuilt for the new zip')
        else:
            step('prefetch', 'skipped', 'seamless is off')

        guard()
        step('data', 'running')
        igm = repo.data_installer()
        igm.game_running = lambda: bool(procs.game_processes(game_dir, lister))
        config = os.path.join(game_dir, 'configpc.txt')
        if plan['data_needed']:
            igm.install(config, zip_out, log=job.log)
            step('data', 'done', 'todgameplay_data.zip first on the filesystem line')
        else:
            if os.path.isfile(config):
                igm.uninstall(config, log=job.log)
            step('data', 'done', 'data zip removed')
        pre_dst = os.path.join(game_dir, P.PREFETCH)
        if plan['prefetch'] and plan['data_needed']:
            B.replace_file(pre_out, pre_dst)
            job.log('copied %s' % P.PREFETCH)
        elif os.path.isfile(pre_dst):
            os.remove(pre_dst)
            job.log('removed %s (seamless off)' % P.PREFETCH)

        guard()
        step('dlls', 'running')
        done = []
        if plan['dll_needed']:
            for f in P.DLL_FILES:
                B.replace_file(os.path.join(repo.DIST, f), os.path.join(game_dir, f))
                done.append(f)
        else:
            for f in P.DLL_FILES:
                p = os.path.join(game_dir, f)
                if os.path.isfile(p) and (f != 'dsound.dll' or is_our_dsound(p)):
                    os.remove(p)
                    job.log('removed %s (no DLL feature selected)' % f)
        for f in (P.PHYSICS_DLL, P.PHYSICS_MAP):     # the map (hang reports) always goes with its DLL
            php = os.path.join(game_dir, f)
            if plan['physics_dll']:
                B.replace_file(os.path.join(repo.DIST, f), php)
                done.append(f)
            elif os.path.isfile(php):
                os.remove(php)
                job.log('removed %s (physics off)' % f)
        for f in P.MAYHEM_DDS:                       # the MAYHEM title art: with any Mayhem group
            dst = os.path.join(game_dir, f)
            if plan['mayhem_dds']:
                B.replace_file(os.path.join(repo.DIST, f), dst)
                done.append(f)
            elif os.path.isfile(dst):
                os.remove(dst)
                job.log('removed %s (no Mayhem group on)' % f)
        for f in done:
            job.log('copied %s' % f)
        step('dlls', 'done', ', '.join(done) if done else 'none needed (removed)')

        step('ini', 'running')
        sets = merge_ini(game_dir, plan, staging, job.log)
        step('ini', 'done', '%d keys set, your other lines kept' % len(sets))

        guard()
        step('hdfx', 'running')
        hd_written = []
        if plan['hdfx']:
            ini_path = os.path.join(game_dir, P.HDFX_INI)
            user_ini = GS.read_text(ini_path) if hdfx_state(game_dir) == 'ours' and os.path.isfile(ini_path) else None
            t0 = time.time()
            hstage = os.path.join(staging, 'hdfx')
            res, rels = hdfx_fn(game_dir, hstage, plan['hdfx']['sets'], user_ini, job.log)
            guard()
            if hdfx_state(game_dir) == 'foreign':
                os.replace(os.path.join(game_dir, P.HDFX_DLL), os.path.join(game_dir, P.HDFX_ASIDE))
                job.log('set the other d3d9.dll aside as %s' % P.HDFX_ASIDE)
            for rel in rels:
                B.replace_file(os.path.join(hstage, rel), os.path.join(game_dir, rel))
                hd_written.append(rel.replace('\\', '/'))
            stale = [r for r in P.HDFX_FILES if r not in hd_written and os.path.isfile(os.path.join(game_dir, *r.split('/')))]
            for r in stale:                          # e.g. no sh.txt this time: an old one must not stay behind
                os.remove(os.path.join(game_dir, *r.split('/')))
            job.log('HD effects: %d files; %s; ini keys %s%s' % (
                len(rels), 'material rows %(classes)d, sprite rows %(particles)d, sky faces %(sky)d, sky irradiance %(sh)s'
                % res, ' '.join(plan['hdfx']['sets']), '; your own values kept' if user_ini else ''))
            step('hdfx', 'done', '%d files, data built from your game in %.0f s' % (len(rels), time.time() - t0))
        else:
            removed = hdfx_remove(game_dir, job.log)
            step('hdfx', 'skipped' if not removed else 'done', 'HD effects off' + (' (removed)' if removed else ''))

        guard()
        step('minimap', 'running')
        if plan['minimap']:
            out = os.path.join(game_dir, P.MINIMAP_DIR)
            try:
                r = minimap_fn(game_dir, out, job.log, progress)
                step('minimap', 'done', '%d maps from your game files in %.0f s' % (r.get('maps', 0), r.get('seconds', 0)))
            except Exception as e:                    # noqa: BLE001  the radar works without the pictures
                job.log('minimap: the bake failed (%s); the radar stays a radar, the objective markers still work. '
                        'Install again to retry.' % e)
                step('minimap', 'warn', 'not baked: %s' % e)
            guard()
        else:
            removed = minimap_remove(game_dir, job.log)
            step('minimap', 'done' if removed else 'skipped', 'minimap off' + (' (removed)' if removed else ''))

        step('display', 'running')
        if plan['display']:
            dsp = plan['display']
            write_configpc_display(config, dsp['fullscreen'], dsp['w'], dsp['h'], job.log)
            if plan['registry']:
                reg_set_size(dsp['w'], dsp['h'])
                job.log('registry XRes / YRes = %d x %d' % (dsp['w'], dsp['h']))
            else:
                job.log('registry: left alone (test mode)')
            if side and os.path.isfile(side):
                # the player picked a start mode: the mode kept in game would win at the next start
                if not any(e.get('path') == os.path.abspath(side) for e in B.load_manifest(bk_dir).get('side_files', [])):
                    raise StepError('the display side file was not backed up: not deleted')
                os.remove(side)
                job.log('deleted %s (backed up first) so the start mode applies' % side)
            step('display', 'done', '%s %dx%d' % ('borderless' if dsp['fullscreen'] else 'windowed', dsp['w'], dsp['h']))
        else:
            step('display', 'skipped', '16:9 display off: configpc.txt display lines left as they are')

        step('difficulty', 'running')
        mk = os.path.join(game_dir, P.DIFF_MARKER)
        if plan['difficulty_marker']:
            B.write_bytes(mk, b'menu\r\n')
            step('difficulty', 'done', '%s = menu' % P.DIFF_MARKER)
        else:
            if os.path.isfile(mk):
                os.remove(mk)
                job.log('removed %s' % P.DIFF_MARKER)
            step('difficulty', 'skipped', 'difficulty menu off')

        step('finish', 'running')
        problems = []
        for f in done:
            if B.sha_file(os.path.join(repo.DIST, f)) != B.sha_file(os.path.join(game_dir, f)):
                problems.append(f)
        if plan['data_needed'] and B.sha_file(zip_out) != B.sha_file(os.path.join(game_dir, P.ZIP)):
            problems.append(P.ZIP)
        if plan['hdfx'] and (B.sha_file(os.path.join(repo.HDFX, 'release', 'd3d9.dll')) !=
                             B.sha_file(os.path.join(game_dir, P.HDFX_DLL))):
            problems.append(P.HDFX_DLL)
        if problems:
            raise StepError('installed files differ from their sources: %s' % ', '.join(problems))
        info = dict(tool='Gameplay+ installer', installed_at=datetime.datetime.now().isoformat(timespec='seconds'),
                    selection=P.saved_selection(plan['selection']), data_options=plan['data'] if plan['data_needed'] else None,
                    ini_keys=sets, backup=bk_dir, backup_kind=kind, exe_match=c['exe']['match'])
        B.write_bytes(os.path.join(game_dir, P.SELECTION_FILE), json.dumps(info, indent=1).encode('utf-8'))
        step('finish', 'done', 'every installed file checked')
        shutil.rmtree(staging, ignore_errors=True)
        written = (done + ([P.PREFETCH] if plan['prefetch'] and plan['data_needed'] else []) +
                   ([P.DIFF_MARKER] if plan['difficulty_marker'] else []) + hd_written + B.minimap_files(game_dir))
        job.result = dict(ok=True, backup=bk_dir, log=job.log_path, summary=P.summary(plan), ini_keys=sets,
                          data=plan['data'] if plan['data_needed'] else None, exe=c['exe'], files=written,
                          warnings=c['warnings'])
        job.log('INSTALL DONE')
        job.state = 'done'
    except Exception as e:                            # noqa: BLE001  (the job reports every failure)
        msg = str(e) if isinstance(e, (StepError, B.BackupError)) else '%s: %s' % (type(e).__name__, e)
        job.log('FAILED: %s' % msg)
        if not isinstance(e, (StepError, B.BackupError)):
            job.log(traceback.format_exc())
        if cur[0]:
            job.step(cur[0], 'failed', msg)
        for s in job.steps:
            if s['status'] == 'pending':
                s['status'] = 'skipped'
        if staging:
            shutil.rmtree(staging, ignore_errors=True)
        job.error = msg
        job.result = dict(ok=False, backup=bk_dir, log=job.log_path,
                          touched=cur[0] not in (None, 'preflight', 'backup', 'build', 'prefetch'))
        job.state = 'failed'
    finally:
        job.finished = time.time()


RESTORE_STEPS = [
    ('preflight', 'Check the game folder'),
    ('verify', 'Verify the backup and copy it back'),
    ('finish', 'Done'),
]


def run_restore(job, game_dir, backup_dir, registry=True, lister=procs.list_processes, documents=None):
    """documents: as run_install (None = the player's own Documents, '' = the side file is never touched)."""
    try:
        job.step('preflight', 'running')
        if not os.path.isfile(os.path.join(backup_dir, 'manifest.json')):
            raise StepError('Not a Gameplay+ backup (no manifest.json).')
        if not os.path.isdir(game_dir):
            raise StepError('The game folder does not exist.')
        if procs.game_processes(game_dir, lister):
            raise StepError('Total Overdose is running. Quit the game first: nothing is restored while it runs.')
        man = B.load_manifest(backup_dir)
        if man.get('game_dir') and os.path.normcase(man['game_dir']) != os.path.normcase(os.path.abspath(game_dir)):
            job.log('note: the backup was made from %s' % man['game_dir'])
        job.set_log(os.path.join(backup_dir, 'restore.log'))
        job.step('preflight', 'done', '%s backup of %s' % (man.get('kind'), man.get('created')))
        job.step('verify', 'running')
        res = B.restore(backup_dir, game_dir, job.log, lambda d, t: setattr(job, 'progress', dict(done=d, total=t)),
                        registry_import=reg_import if registry else None, side_target=side_file(documents))
        job.step('verify', 'done', '%d copied back, %d already identical, %d removed' % (
            len(res['copied']), res['identical'], len(res['removed'])))
        job.step('finish', 'done')
        job.result = dict(ok=True, **res, log=job.log_path)
        job.state = 'done'
    except Exception as e:                            # noqa: BLE001
        msg = str(e)
        job.log('FAILED: %s' % msg)
        for s in job.steps:
            if s['status'] == 'running':
                s['status'] = 'failed'
                s['detail'] = msg
            elif s['status'] == 'pending':
                s['status'] = 'skipped'
        job.error = msg
        job.result = dict(ok=False, log=job.log_path)
        job.state = 'failed'
    finally:
        job.finished = time.time()
