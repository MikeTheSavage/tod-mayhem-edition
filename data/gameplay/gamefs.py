"""Where the game reads a block from: the configpc.txt `filesystem =` order, read only.

The engine opens every archive of the `filesystem =` line into a slot (OpenZip FUN_00419100) and FindFile returns the
first slot that has the name, so the first archive listed wins. Without a
filesystem line the game uses blocks.naz, sounds.naz, videos00.naz, videos01.naz.

`GameFS(game_dir, skip=...)` reproduces that lookup for the player's install, skipping our own zip, so the builder
starts from exactly the block the game would load without the mod: TOD HD's rebuilt block when hd_override*.zip
carries it, the GOG block otherwise. Nothing here writes anything.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(os.path.dirname(HERE), 'lib')
if LIB not in sys.path:
    sys.path.insert(0, LIB)
import todblock as tb  # noqa: E402

DEFAULT_ARCHIVES = ['blocks.naz', 'sounds.naz', 'videos00.naz', 'videos01.naz']
SHARED = 'data_pc/overdose_the_game/overdose/shared/'
MAIN = SHARED + 'overdose.main'


def read_config_lines(path):
    """configpc.txt as (lines without line ends, newline string, had_final_newline)."""
    with open(path, 'rb') as f:
        raw = f.read()
    text = raw.decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    final = text.endswith('\n')
    lines = text.split(nl)
    if final:
        lines = lines[:-1]
    return lines, nl, final


def parse_filesystem(lines):
    """(index of the active filesystem line or None, list of archive names). The engine reads key = value lines;
    the last filesystem line wins like every other key (FUN_0093D480 overwrites)."""
    idx, names = None, None
    for i, ln in enumerate(lines):
        s = ln.strip()
        if s.startswith('#') or '=' not in s:
            continue
        k, v = s.split('=', 1)
        if k.strip().lower() == 'filesystem':
            idx, names = i, [x.strip() for x in v.split(',') if x.strip()]
    return idx, names


class GameFS:
    def __init__(self, game_dir, skip=()):
        self.game_dir = os.path.abspath(game_dir)
        cfg = os.path.join(self.game_dir, 'configpc.txt')
        names = None
        if os.path.exists(cfg):
            _, names = parse_filesystem(read_config_lines(cfg)[0])
        self.configured = names is not None
        names = names or list(DEFAULT_ARCHIVES)
        skip_l = {s.lower() for s in skip}
        self.order = [n for n in names if n.lower() not in skip_l]
        self.skipped = [n for n in names if n.lower() in skip_l]
        self._arch = {}
        if not os.path.exists(os.path.join(self.game_dir, 'blocks.naz')):
            raise FileNotFoundError('no blocks.naz in %s' % self.game_dir)

    def archive(self, name):
        if name not in self._arch:
            p = os.path.join(self.game_dir, name)
            self._arch[name] = tb.Archive(p) if os.path.exists(p) else None
        return self._arch[name]

    def locate(self, entry):
        """(archive file name, archive path) that the game would read `entry` from, or raise KeyError."""
        for n in self.order:
            a = self.archive(n)
            if a is None:
                continue
            hit = a.find('/' + entry)
            if hit is not None and hit[2].lower() == entry.lower():
                return n, a.path
        raise KeyError('%s is in none of %s' % (entry, self.order))

    def read(self, entry):
        """(bytes, source description) of the block the game would load."""
        n, path = self.locate(entry)
        a = self.archive(n)
        off, size, name = a.find('/' + entry)
        with open(a.path, 'rb') as f:
            f.seek(off)
            data = f.read(size)
        if len(data) != size:
            raise IOError('short read of %s in %s' % (entry, path))
        st = os.stat(path)
        return data, dict(archive=n, path=path, archive_bytes=st.st_size, archive_mtime=int(st.st_mtime))

    def naz(self):
        return self.archive('blocks.naz')
