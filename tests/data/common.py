"""Shared setup for the data-option tests: paths, skips and a cached base context."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, 'data')
WORK = os.path.join(ROOT, 'work', 'tests')
sys.path.insert(0, DATA)
sys.dont_write_bytecode = True

GOG = os.environ.get('TODGP_GOG', r'C:\GOG Games\Total Overdose')
HD = os.environ.get('TODGP_HD', '')
SAVES = os.environ.get('TODGP_SAVES', os.path.join(ROOT, 'work', 'saves_copy'))
SLOW = os.environ.get('TODGP_SLOW') == '1'      # cutscene blocks and the TOD HD base (minutes)
HAVE_GOG = os.path.exists(os.path.join(GOG, 'blocks.naz'))
HAVE_HD = os.path.exists(os.path.join(HD, 'hd_override1.zip'))

os.makedirs(WORK, exist_ok=True)


def out(name):
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, 'todgameplay_data.zip')
