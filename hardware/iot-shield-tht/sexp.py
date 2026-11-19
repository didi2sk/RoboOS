"""Tiny S-expression reader/writer for KiCad files."""
import re
_tok = re.compile(r'\s*(\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+)')

class Sym(str):
    """a bare word (not quoted)"""

def parse(text):
    stack, cur = [], []
    for m in _tok.finditer(text):
        t = m.group(1)
        if t == '(':
            stack.append(cur); cur = []
        elif t == ')':
            done = cur; cur = stack.pop(); cur.append(done)
        elif t[0] == '"':
            cur.append(t[1:-1].replace('\\"', '"').replace('\\\\', '\\'))
        else:
            cur.append(Sym(t))
    return cur[0]

def dump(e, ind=0):
    if isinstance(e, list):
        simple = all(not isinstance(x, list) for x in e)
        if simple or len(e) < 3 and not any(isinstance(x, list) and len(x) > 3 for x in e):
            return '(' + ' '.join(dump(x) for x in e) + ')'
        out = '(' + dump(e[0])
        for x in e[1:]:
            out += '\n' + '  ' * (ind + 1) + dump(x, ind + 1) if isinstance(x, list) else ' ' + dump(x)
        return out + ')'
    if isinstance(e, Sym): return str(e)
    if isinstance(e, bool): return 'yes' if e else 'no'
    if isinstance(e, (int, float)):
        s = ('%.4f' % e).rstrip('0').rstrip('.') if isinstance(e, float) else str(e)
        return '0' if s == '-0' else s
    return '"' + str(e).replace('\\', '\\\\').replace('"', '\\"') + '"'

def find(e, key):
    return [x for x in e if isinstance(x, list) and x and x[0] == key]

def first(e, key):
    r = find(e, key)
    return r[0] if r else None
