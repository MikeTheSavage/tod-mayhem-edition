r"""Gameplay+ Installer - visual installer for TOD Gameplay+ (Total Overdose, GOG PC).

  python -I installer\app.py [--game DIR] [--port N] [--no-browser] [--sandbox DIR] [--no-registry]

Starts a local server on 127.0.0.1 (a free port) and opens it in Microsoft Edge app mode (a plain window), or the
default browser. The server quits about 45 s after the window is closed (unless an install is running).
  --sandbox DIR    refuse every game folder / backup location outside DIR (tests on scratch copies)
  --no-registry    never touch HKCU\Software\Eidos\Total Overdose (tests)
  --music-dir DIR  where the Mayhem track (mayhem.ogg / .mp3 + music.json) is (default installer\web\music)
  --documents DIR  the Documents folder whose Total Overdose\savegames\TODGameplay_display.txt the Display group may
                   replace (tests: a fake one; env GPI_DOCUMENTS works too). With --sandbox and neither, no
                   Documents folder is touched
"""
import argparse
import os
import shutil
import subprocess
import sys
import threading
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
if sys.stdout is None:          # pythonw: no console
    sys.stdout = sys.stderr = open(os.devnull, 'w')

from gpi import server  # noqa: E402


def find_edge():
    for base in (os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramFiles'), os.environ.get('LOCALAPPDATA')):
        if base:
            p = os.path.join(base, 'Microsoft', 'Edge', 'Application', 'msedge.exe')
            if os.path.isfile(p):
                return p
    return shutil.which('msedge')


def open_window(url):
    edge = find_edge()
    if edge:
        try:
            subprocess.Popen([edge, '--app=' + url, '--window-size=1320,920', '--no-first-run'],
                             creationflags=0x08000000)
            return 'edge'
        except OSError:
            pass
    webbrowser.open(url)
    return 'browser'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game')
    ap.add_argument('--port', type=int, default=0)
    ap.add_argument('--no-browser', action='store_true')
    ap.add_argument('--sandbox')
    ap.add_argument('--no-registry', action='store_true')
    ap.add_argument('--music-dir')
    ap.add_argument('--documents', help='the Documents folder of the display side file (tests: a fake one)')
    ap.add_argument('--stay', action='store_true', help='do not quit when the window is closed')
    a = ap.parse_args(argv)
    app = server.App(game=a.game, sandbox=a.sandbox, registry=not a.no_registry, music_dir=a.music_dir,
                     idle_exit=0 if a.stay else 45, documents=a.documents)
    httpd = server.serve(app, a.port)
    url = 'http://127.0.0.1:%d/' % httpd.server_address[1]
    print('Gameplay+ Installer: %s' % url, flush=True)
    if not a.no_browser:
        print('opened in %s' % open_window(url), flush=True)
    threading.Thread(target=server.watchdog, args=(app, httpd), daemon=True).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
