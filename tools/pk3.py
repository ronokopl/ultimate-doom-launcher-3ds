"""Read-only access to the source pk3 + shared helpers for every extractor."""
import zipfile, re, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Game data you own (never part of this repository): set UDL_GAME_DATA, default: a "gamedata" folder next to the project
GAME_DATA = os.environ.get('UDL_GAME_DATA', os.path.join(os.path.dirname(ROOT), 'gamedata'))
SRC_PK3 = os.path.join(GAME_DATA, 'brutalv21.pk3')
SHEETS = os.path.join(ROOT, 'sheets')
JSON = os.path.join(ROOT, 'json')
GEN = os.path.join(ROOT, 'gen')

_zip = None
def pk3():
    global _zip
    if _zip is None:
        _zip = zipfile.ZipFile(SRC_PK3)  # opened read-only
    return _zip

def entries():
    return [i for i in pk3().infolist() if not i.is_dir()]

def _index():
    return {i.filename.lower(): i.filename for i in entries()}

def read(path):
    """Read an entry by path, case-insensitively."""
    real = _index().get(path.lower())
    if real is None:
        raise KeyError(path)
    return pk3().read(real).decode('latin1')

def lumpname(path):
    """ZDoom pk3 lump name: basename up to first dot, upper, max 8 chars."""
    base = path.rsplit('/', 1)[-1]
    return base.split('.', 1)[0].upper()[:8]

def root_lumps(name):
    """All root-level entries whose lump name == name, in archive order."""
    return [i.filename for i in entries() if '/' not in i.filename and lumpname(i.filename) == name]

def strip_comments(text):
    """Remove // and /* */ comments, keeping strings and line structure."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1]); i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j)); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)

TOK = re.compile(r'"(?:\\.|[^"\\])*"|[(){},;:=]|[^\s(){},;:="]+')

def tokenize(text):
    """Tokens as (text, line, start, end). Comments must be stripped first."""
    toks, line, last = [], 1, 0
    for m in TOK.finditer(text):
        line += text.count('\n', last, m.start()); last = m.start()
        toks.append((m.group(), line, m.start(), m.end()))
    return toks

def paren_end(toks, k):
    """toks[k] is '('; return index of matching ')'."""
    depth = 0
    for j in range(k, len(toks)):
        t = toks[j][0]
        if t == '(':
            depth += 1
        elif t == ')':
            depth -= 1
            if depth == 0:
                return j
    raise ValueError('unbalanced parens at line %d' % toks[k][1])

def brace_end(toks, k):
    depth = 0
    for j in range(k, len(toks)):
        t = toks[j][0]
        if t == '{':
            depth += 1
        elif t == '}':
            depth -= 1
            if depth == 0:
                return j
    raise ValueError('unbalanced braces at line %d' % toks[k][1])

def acs_chunks(data):
    """{chunk_id: [payload,...]} for an ACSE/ACSe object (direct or ACS0-wrapped). {} for plain ACS0."""
    import struct
    magic, dirofs = data[:4], struct.unpack('<I', data[4:8])[0]
    if magic in (b'ACSE', b'ACSe'):
        start, end = 8, dirofs  # chunks sit before the directory offset in direct format
        if dirofs < 8:
            return {}
    elif magic == b'ACS\0' and dirofs >= 8 and data[dirofs - 4:dirofs] in (b'ACSE', b'ACSe'):
        start, end = struct.unpack('<I', data[dirofs - 8:dirofs - 4])[0], dirofs - 8
    else:
        return {}
    out, p = {}, start
    while p + 8 <= end:
        cid, ln = data[p:p + 4].decode('latin1'), struct.unpack('<I', data[p + 4:p + 8])[0]
        out.setdefault(cid, []).append(data[p + 8:p + 8 + ln]); p += 8 + ln
    return out

def acs_strings(chunk):
    import struct
    n = struct.unpack('<I', chunk[:4])[0]
    offs = struct.unpack('<%dI' % n, chunk[4:4 + 4 * n])
    return [chunk[o:chunk.index(b'\0', o)].decode('latin1') for o in offs]

IWADS = {'doom2': os.path.join(GAME_DATA, 'Doom2.wad'), 'doom': os.path.join(GAME_DATA, 'DOOM.WAD')}

def wad_lumps(path):
    """[(name, size, namespace)] from a WAD directory (read-only)."""
    import struct
    with open(path, 'rb') as f:
        magic, n, off = struct.unpack('<4sii', f.read(12))
        f.seek(off)
        d = f.read(16 * n)
    out, ns = [], 'global'
    for i in range(n):
        pos, size, name = struct.unpack('<ii8s', d[i * 16:i * 16 + 16])
        name = name.split(b'\0')[0].decode('latin1').upper()
        if name.endswith('_START') and len(name) <= 8:
            ns = {'S': 'sprites', 'SS': 'sprites', 'F': 'flats', 'FF': 'flats', 'P': 'patches',
                  'P1': 'patches', 'P2': 'patches', 'P3': 'patches'}.get(name[:-6], ns); continue
        if name.endswith('_END') and len(name) <= 6:
            ns = 'global'; continue
        out.append((name, size, ns))
    return out
