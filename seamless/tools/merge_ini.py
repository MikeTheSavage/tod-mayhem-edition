"""merge_ini.py - switch features on in the player's TODGameplay.ini without touching anything else.

  python -I merge_ini.py USER_INI TEMPLATE_INI section.key=value [section.key=value ...] [--dry-run]

For each section.key=value: if the player's ini has the section and the key, only the value is replaced (the rest of the
line, e.g. an inline ; comment, is kept); if the section exists without the key, the key line from the template (or
`key = value`) is added at the end of that section; if the section is missing, the whole section is copied from the
template (TEMPLATE_INI, the shipped dll/TODGameplay.ini) with the requested values. Every other line stays byte-for-
byte. Line ends follow the file. Prints what changed.
"""
import re
import sys


def sections(lines):
    """[(name lower, start index of the header, end index exclusive)]"""
    out, cur, start = [], None, 0
    for i, ln in enumerate(lines):
        m = re.match(r'\s*\[([^\]]+)\]', ln)
        if m:
            if cur is not None:
                out.append((cur, start, i))
            cur, start = m.group(1).strip().lower(), i
    if cur is not None:
        out.append((cur, start, len(lines)))
    return out


def key_line(lines, a, b, key):
    for i in range(a + 1, b):
        m = re.match(r'(\s*)([A-Za-z0-9_]+)(\s*=\s*)([^;\r\n]*?)(\s*(;.*)?)$', lines[i])
        if m and m.group(2).lower() == key.lower():
            return i, m
    return None, None


def main(argv):
    dry = '--dry-run' in argv
    argv = [a for a in argv if a != '--dry-run']
    user, tmpl, sets = argv[0], argv[1], argv[2:]
    raw = open(user, 'rb').read().decode('latin-1')
    nl = '\r\n' if '\r\n' in raw else '\n'
    lines = raw.split(nl)
    final_nl = raw.endswith(nl)
    if final_nl:
        lines = lines[:-1]
    t_raw = open(tmpl, 'rb').read().decode('latin-1').replace('\r\n', '\n')
    t_lines = t_raw.split('\n')
    changes = []
    for s in sets:
        sk, val = s.split('=', 1)
        sec, key = sk.split('.', 1)
        sec_l = sec.lower()
        secs = {n: (a, b) for n, a, b in sections(lines)}
        if sec_l in secs:
            a, b = secs[sec_l]
            i, m = key_line(lines, a, b, key)
            if i is not None:
                if m.group(4).strip() != val:
                    old = lines[i]
                    lines[i] = m.group(1) + m.group(2) + m.group(3) + val + (m.group(5) or '')
                    changes.append('[%s] %s: %r -> %r' % (sec, key, old.strip(), lines[i].strip()))
                else:
                    changes.append('[%s] %s already %s' % (sec, key, val))
                continue
            # add the key at the end of the section (before trailing blank lines)
            j = b
            while j - 1 > a and not lines[j - 1].strip():
                j -= 1
            ts = {n: (x, y) for n, x, y in sections(t_lines)}
            new = '%s = %s' % (key, val)
            if sec_l in ts:
                ti, tm = key_line(t_lines, ts[sec_l][0], ts[sec_l][1], key)
                if ti is not None:
                    new = tm.group(1) + tm.group(2) + tm.group(3) + val + (tm.group(5) or '')
            lines.insert(j, new)
            changes.append('[%s] %s added: %r' % (sec, key, new.strip()))
            continue
        # copy the section from the template
        ts = {n: (x, y) for n, x, y in sections(t_lines)}
        if sec_l not in ts:
            body = ['[%s]' % sec, '%s = %s' % (key, val)]
        else:
            x, y = ts[sec_l]
            body = list(t_lines[x:y])
            while body and not body[-1].strip():
                body.pop()
            ki, km = key_line(body, 0, len(body), key)
            if ki is not None:
                body[ki] = km.group(1) + km.group(2) + km.group(3) + val + (km.group(5) or '')
            else:
                body.append('%s = %s' % (key, val))
        while lines and not lines[-1].strip():
            lines.pop()
        lines += [''] + body
        changes.append('[%s] section added from the template with %s = %s (%d lines)' % (sec, key, val, len(body)))
    out = nl.join(lines) + (nl if final_nl or True else '')
    for c in changes:
        print(c)
    if not dry and out != raw:
        open(user, 'wb').write(out.encode('latin-1'))
        print('written: %s' % user)
    elif out == raw:
        print('unchanged: %s' % user)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
