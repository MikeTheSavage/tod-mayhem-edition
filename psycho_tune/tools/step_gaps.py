"""step_gaps.py - find the stretches of a TODGameplay.log where the on-foot input tick did not run.

Everything that ticks from the player's on-foot input tick (psycho cost, bigger-fights / difficulty stats, smart AI, gang
wars) goes quiet while Ram drives, sits in a seat or a script moves him. The ragdoll's
per-character lines come from the character update (every frame) and keep coming. A ragdoll line inside such a stretch
is a ragdoll the physics never stepped (frozen enemies).

  python -I step_gaps.py LOG [--min SECONDS]      (default 8 s)
"""
import re
import sys

# lines written only from the input tick (heat / physics lines also come from damage and blast hooks: not used)
TICK = re.compile(r'(psycho: cost|bigger fights: mission -?\d+: \d+ in the pool|difficulty: BRUTAL fight|smart: |gang wars: '
                  r'target switch|heat: state)')
RAGDOLL = re.compile(r'ragdoll: (character|the death-pose|corpse)')
STARTED = re.compile(r'ragdoll: character \S+ (active ragdoll|death ragdoll|limp)')
FROZEN0 = re.compile(r'frozen \(the game swaps in its corpse\) after 0\.0 s|after 0\.0 s')


def ts(line):
    try:
        h, m, s = line[:12].split(':')
        return int(h) * 3600 + int(m) * 60 + float(s)
    except ValueError:
        return None


def main():
    path = sys.argv[1]
    lim = float(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 8.0
    last, last_line, between = None, '', []
    gaps = []
    for line in open(path, encoding='utf-8', errors='replace').read().splitlines():
        t = ts(line)
        if t is None:
            continue
        if 'TODGameplay build' in line:          # a new game start: no gap across it
            last, between = None, []
            continue
        if TICK.search(line):
            if last is not None and t - last > lim:
                rd = [b for b in between if RAGDOLL.search(b)]
                gaps.append((t - last, last_line[:12], line[:12], len(rd), sum(1 for b in rd if STARTED.search(b)),
                             sum(1 for b in rd if FROZEN0.search(b)), rd[:3]))
            last, last_line, between = t, line, []
        else:
            between.append(line)
    print('%s: %d stretch(es) of more than %.0f s without the on-foot input tick' % (path, len(gaps), lim))
    for d, a, b, n, st, fz, ex in gaps:
        print('  %5.1f s  %s -> %s  ragdoll lines %3d (started %d, 0.0 s simulations %d)' % (d, a, b, n, st, fz))
        for e in ex:
            print('            ' + e[:170])
    with_rd = [g for g in gaps if g[3]]
    print('with ragdoll lines inside: %d, ragdolls started inside: %d' % (len(with_rd), sum(g[4] for g in gaps)))


if __name__ == '__main__':
    main()
