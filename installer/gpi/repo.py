"""Where the installer finds the repo's own files (data builder, merge_ini, dist DLLs, web UI).

Running from the repo: the installer folder's parent is the repo root. Packed with PyInstaller (one file): the bundle
is unpacked to sys._MEIPASS with the same relative layout (see installer/README.md), so the same paths work.
"""
import os
import sys

if getattr(sys, 'frozen', False):
    ROOT = getattr(sys, '_MEIPASS', os.path.dirname(sys.executable))
else:
    ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DATA = os.path.join(ROOT, 'data')
DATA_LIB = os.path.join(DATA, 'lib')
SEAMLESS_TOOLS = os.path.join(ROOT, 'seamless', 'tools')
DIST = os.path.join(ROOT, 'dist')
WEB = os.path.join(ROOT, 'installer', 'web')
SHIPPED_INI = os.path.join(DIST, 'TODGameplay.ini')
SUAVE_AUDIO = os.path.join(ROOT, 'suave', 'audio')
SUAVE_FINGERPRINT = os.path.join(SUAVE_AUDIO, 'fingerprint.json')
HDFX = os.path.join(ROOT, 'hdfx')
HDFX_TOOLS = os.path.join(HDFX, 'tools')
HUD_TOOLS = os.path.join(ROOT, 'hud', 'tools')

_ready = False


def add_paths():
    """Put the repo's Python tool folders on sys.path (the installer imports them, it does not copy their logic)."""
    global _ready
    if _ready:
        return
    sys.dont_write_bytecode = True
    for p in (SEAMLESS_TOOLS, DATA_LIB, DATA, HDFX_TOOLS):
        if p not in sys.path:
            sys.path.insert(0, p)
    _ready = True


def builder():
    add_paths()
    from gameplay import builder as b      # noqa: E402
    return b


def options():
    add_paths()
    from gameplay import options as o      # noqa: E402
    return o


def gamefs():
    add_paths()
    from gameplay import gamefs as g       # noqa: E402
    return g


def data_installer():
    add_paths()
    import install_gameplay_mod as m       # noqa: E402
    return m


def merge_ini():
    add_paths()
    import merge_ini as m                  # noqa: E402
    return m


def prefetch():
    add_paths()
    import make_prefetch_table as m        # noqa: E402
    return m


def song_import():
    """suave/audio/song_import.py (needs numpy + soundfile; ImportError when they are missing)."""
    add_paths()
    if SUAVE_AUDIO not in sys.path:
        sys.path.insert(0, SUAVE_AUDIO)
    import song_import as m                # noqa: E402
    return m


def hdfx_stage():
    add_paths()
    import hdfx_stage as m                 # noqa: E402
    return m


def bake_minimap():
    add_paths()
    if HUD_TOOLS not in sys.path:
        sys.path.insert(0, HUD_TOOLS)
    import bake_minimap as m               # noqa: E402
    return m
