"""sheets/*.xlsx -> json/<book>.json. The only bridge from the source of truth to every other tool.

JSON shape: {sheet: [rows]} or, for sheets with a grouping column, {sheet: {group: [rows]}}.
Compact rows: '~' cells are omitted (absent key == '~'); empty cells export as null (= unchecked).
"""
import os, json, glob, sys
import sheet
from pk3 import SHEETS, JSON

GROUP_BY = {'States': 'actor', 'Actors': 'group'}

def compact(r):
    return {k: v for k, v in r.items() if v != '~'}

def export(path):
    book = sheet.read(path)
    out = {}
    for name, rows in book.items():
        g = GROUP_BY.get(name)
        if g:
            grouped = {}
            for r in rows:
                grouped.setdefault(str(r.get(g)), []).append(compact(r))
            out[name] = grouped
        else:
            out[name] = [compact(r) for r in rows]
    dst = os.path.join(JSON, os.path.basename(path)[:-5] + '.json')
    os.makedirs(JSON, exist_ok=True)
    with open(dst, 'w', encoding='utf-8') as f:
        json.dump(out, f, separators=(',', ':'), ensure_ascii=False)
    return dst, {k: sum(len(x) for x in v.values()) if isinstance(v, dict) else len(v) for k, v in out.items()}

def load(book):
    """Shared loader for every downstream tool."""
    with open(os.path.join(JSON, book + '.json'), encoding='utf-8') as f:
        return json.load(f)

def rows(book, name):
    """Flat row list for a sheet regardless of grouping; '~' restored for omitted keys via .get(k, '~')."""
    v = load(book)[name]
    return [r for g in v.values() for r in g] if isinstance(v, dict) else v

if __name__ == '__main__':
    only = sys.argv[1:]
    for p in sorted(glob.glob(os.path.join(SHEETS, '*.xlsx'))):
        if os.path.basename(p).startswith('~$') or (only and os.path.basename(p)[:-5] not in only):
            continue
        print(*export(p))
