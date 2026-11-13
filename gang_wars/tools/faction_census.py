"""Faction census of every character_placeholder in every block of the game's blocks.naz (read only).

  python -I faction_census.py <game folder> OUT.tsv

Uses the author's AI tuning lab block / fragment readers (not included; set TOD_AI_LAB to their folder). One row per placeholder:
block, fragment, node, name, ModelStat, FactionID, UsingModifiedFaction, UsingSecretFaction, SecretFaction,
weapon, Is_Buddy, MissionObjectiveKill, Health."""
import sys, os, re, collections
LAB = os.environ.get("TOD_AI_LAB", "")  # the author's AI tuning lab readers (not included)
sys.path.insert(0, LAB); sys.path.insert(0, os.path.join(LAB, 'proj'))
sys.dont_write_bytecode = True
import todblock as tb
import todfrag as tf
import census as cz


def main():
    game, out = sys.argv[1], sys.argv[2]
    arch = tb.Archive(os.path.join(game, 'blocks.naz'))
    names = sorted(arch.names)
    main_name = [n for n in names if n.lower().endswith('overdose.main')][0]
    tmp_main = out + '.main.tmp'
    with open(tmp_main, 'wb') as f:
        f.write(arch.read(main_name))
    dbn = cz.db_names(tmp_main)
    os.remove(tmp_main)
    rows = []
    for name in names:
        low = name.lower()
        if re.search(r'_(de|es|fr|it|uk)\.[a-z]+$', low):
            continue
        if not low.endswith(('.mission', '.submap', '.map', '.main', '.scene')):
            continue
        data = arch.read(name)
        tmp = out + '.blk.tmp' + os.path.splitext(name)[1]
        with open(tmp, 'wb') as f:
            f.write(data)
        try:
            rs = cz.census(tmp, dbn)
        except Exception as e:
            print('FAIL', name, e, file=sys.stderr); rs = []
        os.remove(tmp)
        for r in rs:
            r['block'] = os.path.basename(name)
        rows += rs
    cols = ['block', 'fragment', 'node', 'name', 'ModelStat', 'FactionID', 'UsingModifiedFaction', 'UsingSecretFaction',
            'SecretFaction', 'weapon', 'Is_Buddy', 'MissionObjectiveKill', 'EffHealth', 'Model']
    with open(out, 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for r in rows:
            f.write('\t'.join(str(r.get(c, '')) for c in cols) + '\n')
    print(len(rows), 'placeholders')


if __name__ == '__main__':
    main()
