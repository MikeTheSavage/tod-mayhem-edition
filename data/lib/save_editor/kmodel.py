"""Engine model for save decoding: C++ classes (saved property ids/types, T), scripts (members), node type strings."""
import os, json, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ktypes
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
CLASSES = json.load(open(os.path.join(D, 'classes.json')))
SCRIPTS = json.load(open(os.path.join(D, 'script_tables.json')))
GPROPS = json.load(open(os.path.join(D, 'global_properties.json')))
_SCRIPTS_L = {k.lower(): k for k in SCRIPTS}
_CL_L = {k.lower(): k for k in CLASSES}

class NodeType:
    def __init__(self, typestr):
        self.typestr = typestr
        m = re.fullmatch(r'([^()]+)\(([^()]+)\)', typestr)
        if m:
            self.script, self.cls = m.group(1), m.group(2)
        elif typestr.lower() in _CL_L:
            self.script, self.cls = None, typestr
        else:  # a bare script name: base class from the script table
            self.script, self.cls = typestr, None
        if self.script is not None:
            sk = _SCRIPTS_L.get(self.script.lower())
            if sk is None:
                raise KeyError('unknown script %s' % self.script)
            self.script = sk
            if self.cls is None:
                self.cls = SCRIPTS[sk]['parent']
        self.cls = _CL_L.get(self.cls.lower(), self.cls)
        if self.cls not in CLASSES:
            raise KeyError('unknown class %s' % self.cls)
        self.T = CLASSES[self.cls]['T']
        self._saved = {}
        c = self.cls
        while c:
            for k, (n, t) in CLASSES[c]['saved'].items():
                self._saved.setdefault(int(k), (n, t))
            c = CLASSES[c]['parent']
        self.members = SCRIPTS[self.script]['members'] if self.script else []

    def prop(self, p):
        """-> (name, parsed type, kind) for record prop index p (p>0)."""
        if p < self.T:
            n, t = self._saved.get(p, (None, None))
            if n is None:
                return None
            return n, ktypes.parse_type(t), 'class'
        i = p - self.T
        if i >= len(self.members):
            return None
        gid = self.members[i]['id']
        n, t = GPROPS[gid].rsplit(':', 1)
        return n, ktypes.parse_type(t), 'script'
