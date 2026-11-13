r"""TOD Gameplay+ data mod builder.

  python build_gameplay_mod.py --game "C:\GOG Games\Total Overdose" --options options.ini --out work\todgameplay_data.zip
  python build_gameplay_mod.py --game "<TOD HD game folder>" --options options.ini --out work\hd\todgameplay_data.zip
  python build_gameplay_mod.py --list                       every option and level

Reads the player's own blocks (read only) in the game's own lookup order (configpc.txt `filesystem =`, so TOD HD's
hd_override*.zip block is used where it overrides the GOG one), applies the selected options in a fixed order and
writes ONE stored zip plus a report folder (<out>.report/: report.json, diff.md). Put the zip first on the
`filesystem =` line with install_gameplay_mod.py. Exit code 0 = PASS (or nothing to build), 1 = refused.
Options may also be given on the command line: --set difficulty=hard --set camera_distance=far.
"""
import argparse
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gameplay import builder, options  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', help='game folder (holds TOD.exe, blocks.naz and configpc.txt); read only')
    ap.add_argument('--options', help='options.ini with a [gameplay] section')
    ap.add_argument('--set', action='append', default=[], metavar='OPTION=LEVEL')
    ap.add_argument('--out', help='output zip (normally named %s)' % builder.ZIP_NAME)
    ap.add_argument('--report', help='report folder (default: <out>.report)')
    ap.add_argument('--saves', help='a folder of save slots (copies) to check which edited values saves also store')
    ap.add_argument('--list', action='store_true', help='print the options and exit')
    a = ap.parse_args(argv)
    if a.list:
        for o in options.OPTIONS:
            print('%-16s %-40s %s' % (o['key'], ' | '.join(o['levels']), o['help']))
        return 0
    if not (a.game and a.out):
        ap.error('--game and --out are required')
    try:
        sel = builder.read_options_ini(a.options) if a.options else {}
        extra = {}
        for s in a.set:
            if '=' not in s:
                ap.error('--set wants OPTION=LEVEL')
            k, v = s.split('=', 1)
            extra[k] = v
        sel = builder.parse_selection({**sel, **extra})
        print('TOD Gameplay+ data build: ' + ', '.join('%s=%s' % kv for kv in sel.items()))
        rep = builder.build(a.game, sel, a.out, a.report or (os.path.splitext(a.out)[0] + '.report'), a.saves)
    except builder.BuildError as e:
        print('REFUSED: %s' % e)
        return 1
    print(rep['result'])
    if rep.get('zip'):
        print('zip: %s (%d bytes, %d blocks)' % (rep['zip']['path'], rep['zip']['bytes'], len(rep['zip']['entries'])))
        print('report: %s' % (a.report or (os.path.splitext(a.out)[0] + '.report')))
        print('install: python install_gameplay_mod.py install --config "<game>/configpc.txt" --zip "%s"' % a.out)
    if rep.get('saves'):
        print('saves: ' + rep['saves']['summary'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
