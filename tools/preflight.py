"""PREFLIGHT for Doom Launcher 3DS: overlaps every sheet before each compile.

Mods are chosen on the 3DS at runtime, so there are no mod sheets to check here (that is what the ModCache
and the engine do on the device). This checks that the platform/build sheets agree with each other and with
the code: every cell checked, sources/subsystems/patches consistent, Launcher layout fits the screens and
uses known keys/actions, ModCache rules are implemented, SD paths share one folder, memory budget fits.
Writes json/preflight.json; prints a summary whose "errors" count gates build3ds.py.
"""
import os, re, json, collections
from export import load, rows
from pk3 import ROOT, JSON

ISSUES = []
UNCHECKED = (None, '')
CTRU = {'A', 'B', 'SELECT', 'START', 'DRIGHT', 'DLEFT', 'DUP', 'DDOWN', 'R', 'L', 'X', 'Y', 'ZL', 'ZR'}  # gen_c.py CTRU
ACTIONS = {'tab', 'select', 'move_up', 'move_down', 'save_profile', 'delete_profile', 'launch', 'engine'}  # launcher_3ds.cpp Action()
REQUIRED_UI = {'title', 'credit', 'summary', 'hints', 'tab_iwad', 'tab_mods', 'tab_profiles', 'list',
               'btn_up', 'btn_down', 'btn_save', 'btn_delete', 'btn_launch'}  # launcher_3ds.cpp Ui(...)
MODCACHE_RULES = {'image_png', 'image_jpeg', 'sfx_ogg', 'cache_key', 'cache_skip'}  # modcache_3ds.cpp
SCREENS = {'top': (400, 240), 'bottom': (320, 240)}
LAUNCHER_PATHS = {'iwad_dir', 'mod_dir', 'cache_dir', 'profiles', 'config', 'saves', 'args_file', 'gz_launch', 'uz_launch', 'settings'}


def issue(sev, check, sheet, row, col, msg):
    ISSUES.append({'sev': sev, 'check': check, 'sheet': sheet, 'row': str(row), 'col': col, 'msg': msg})


def src(path):
    p = os.path.join(ROOT, path)
    return open(p, encoding='utf-8', errors='replace').read() if os.path.exists(p) else None


def main():
    # 1. every cell checked
    totals, unfinished = collections.Counter(), collections.Counter()
    for book in ('build', 'platform'):
        for name, v in load(book).items():
            for r in v:
                totals[book + '/' + name] += 1
                bad = [k for k, val in r.items() if val in UNCHECKED and k != 'status']
                if bad:
                    unfinished[book + '/' + name] += 1
                    issue('error', 'unchecked', book + '/' + name, r.get(next(iter(r))), ','.join(bad), 'unchecked cell(s)')

    # 2. sources and subsystems
    sources = rows('build', 'Sources')
    compiled = {s['unit'] for s in sources if s.get('compile') == 'yes'}
    for s in sources:
        if s.get('compile') == 'yes' and not os.path.exists(os.path.join(ROOT, s['unit'])):
            issue('error', 'source-missing', 'build/Sources', s['unit'], 'unit', 'file does not exist')
    modules = {m['module'] for m in rows('build', 'Modules')}
    for s in sources:
        if s.get('compile') == 'yes' and s.get('module') not in modules:
            issue('error', 'module', 'build/Sources', s['unit'], 'module', 'module %s has no Modules row' % s.get('module'))
    for r in rows('platform', 'Subsystems'):
        impl = r.get('impl', '~')
        if impl.endswith(('.cpp', '.c')) and impl not in compiled:
            issue('error', 'subsystem-source', 'platform/Subsystems', r['subsystem'], 'impl', '%s is not a compiled Sources unit' % impl)

    # 3. patches: file exists, carries a [3DS] marker, compiled when it is a source file
    for r in rows('build', 'Patches'):
        files = [f.strip() for f in r['file'].split('+')]
        base = os.path.dirname(files[0])
        for f in (f if '/' in f else base + '/' + f for f in files):
            text = src(f)
            if text is None:
                issue('error', 'patch-file', 'build/Patches', r['file'], 'file', '%s missing' % f)
            elif '[3DS]' not in text:
                issue('error', 'patch-marker', 'build/Patches', r['file'], 'file', '%s has no [3DS] marker' % f)
            elif f.endswith(('.cpp', '.c')) and f not in compiled:
                issue('error', 'patch-source', 'build/Patches', r['file'], 'file', '%s is not compiled' % f)

    # 4. launcher layout, keys and actions
    ui = rows('platform', 'Launcher')
    names = collections.Counter(r['element'] for r in ui)
    for n, c in names.items():
        if c > 1:
            issue('error', 'ui-duplicate', 'platform/Launcher', n, 'element', 'element defined %d times' % c)
    for n in REQUIRED_UI - set(names):
        issue('error', 'ui-missing', 'platform/Launcher', n, 'element', 'launcher_3ds.cpp needs this element')
    boxes = collections.defaultdict(list)
    for r in ui:
        x, y, w, h = (int(r[k]) for k in ('x', 'y', 'w', 'h'))
        sw, sh = SCREENS.get(r['screen'], (0, 0))
        if r['screen'] not in SCREENS or x < 0 or y < 0 or x + w > sw or y + h > sh:
            issue('error', 'ui-bounds', 'platform/Launcher', r['element'], 'x,y,w,h', 'outside the %s screen' % r['screen'])
        if r.get('key', '~') != '~' and r['key'] not in CTRU:
            issue('error', 'ui-key', 'platform/Launcher', r['element'], 'key', 'unknown 3DS button %s' % r['key'])
        if r.get('action', '~') != '~' and r['action'] not in ACTIONS:
            issue('error', 'ui-action', 'platform/Launcher', r['element'], 'action', 'launcher_3ds.cpp has no action %s' % r['action'])
        if r['kind'] in ('button', 'tab', 'list'):
            for (n2, b) in boxes[r['screen']]:
                if x < b[0] + b[2] and b[0] < x + w and y < b[1] + b[3] and b[1] < y + h:
                    issue('error', 'ui-overlap', 'platform/Launcher', r['element'], 'x,y,w,h', 'overlaps %s' % n2)
            boxes[r['screen']].append((r['element'], (x, y, w, h)))
    keys = collections.Counter(r['key'] for r in ui if r.get('key', '~') != '~')
    for k, c in keys.items():
        if c > 1:
            issue('error', 'ui-key-twice', 'platform/Launcher', k, 'key', 'button bound to %d elements' % c)

    # 5. mod cache rules are implemented and write into the cache folder
    fs = {r['role']: r['path'] for r in rows('platform', 'Filesystem')}
    for r in rows('platform', 'ModCache'):
        if r['rule'] not in MODCACHE_RULES:
            issue('error', 'modcache-rule', 'platform/ModCache', r['rule'], 'rule', 'modcache_3ds.cpp does not implement it')
        if r['rule'].startswith('cache_') and not r['cache_entry'].startswith('cache/'):
            issue('error', 'modcache-path', 'platform/ModCache', r['rule'], 'cache_entry', 'cache files must live in cache/')

    # 6. filesystem: launcher folders present, every sdmc path under one base folder, romfs sources exist
    for role in LAUNCHER_PATHS - set(fs):
        issue('error', 'fs-missing', 'platform/Filesystem', role, 'role', 'launcher needs this path')
    sd = [p for p in fs.values() if p.startswith('sdmc:/')]
    base = os.path.commonprefix(sd)
    if sd and base.rstrip('/').count('/') < 2:  # sdmc:/3ds/<App>
        issue('error', 'fs-base', 'platform/Filesystem', '*', 'path', 'sdmc paths do not share one app folder (%s)' % base)
    for r in rows('platform', 'Filesystem'):
        if r['path'].startswith('romfs:/') and not os.path.exists(os.path.join(ROOT, r['source'].split(' (')[0])):
            issue('error', 'fs-source', 'platform/Filesystem', r['role'], 'source', 'romfs source missing')

    # 6b. compat: every implemented feature's files are covered by a Patches row
    patched = ' '.join(r['file'] for r in rows('build', 'Patches'))
    for r in rows('platform', 'Compat'):
        if r['resolution'] == 'implemented':
            for f in r['files'].split():
                if f not in patched:
                    issue('error', 'compat-patch', 'platform/Compat', r['feature'], 'files', '%s has no Patches row' % f)
        elif r['resolution'] not in ('already in 2.8.1', 'ignored', 'unsupported'):
            issue('error', 'compat-resolution', 'platform/Compat', r['feature'], 'resolution', 'unknown resolution')

    # 7. menu
    cvars = collections.Counter(m['cvar'] for m in rows('platform', 'Menu') if m.get('cvar', '~') != '~')
    for c, n in cvars.items():
        if n > 1:
            issue('error', 'menu-cvar', 'platform/Menu', c, 'cvar', 'cvar on %d menu rows' % n)
    for m in rows('platform', 'Menu'):
        if m.get('widget') == 'slider' and len(str(m['values']).split(',')) != 4:
            issue('error', 'menu-slider', 'platform/Menu', m['option'], 'values', 'slider needs "min, max, step, decimals"')

    # 8. memory budget
    B = {r['key']: r.get('value') for r in rows('build', 'Build')}
    num = lambda k: float(B.get(k) or 0)
    fixed = sum(num(k) for k in ('engine_binary_mb', 'engine_heap_mb', 'texture_cache_mb', 'sound_cache_mb'))
    if fixed > num('app_ram_mb'):
        issue('error', 'memory', 'build/Build', 'app_ram_mb', 'value', 'budget %.0fMB > app RAM %.0fMB' % (fixed, num('app_ram_mb')))

    by_check = collections.Counter((i['sev'], i['check']) for i in ISSUES)
    summary = {'errors': sum(v for (s, _), v in by_check.items() if s == 'error'),
               'warnings': sum(v for (s, _), v in by_check.items() if s == 'warn'),
               'by_check': {'%s:%s' % k: v for k, v in sorted(by_check.items())},
               'rows': dict(totals), 'unfinished_rows': dict(unfinished),
               'memory': {'budget_total_mb': fixed, 'app_ram_mb': num('app_ram_mb')}}
    os.makedirs(JSON, exist_ok=True)
    with open(os.path.join(JSON, 'preflight.json'), 'w', encoding='utf-8') as f:
        json.dump({'summary': summary, 'issues': ISSUES}, f, indent=1)
    print(json.dumps({k: summary[k] for k in ('errors', 'warnings', 'by_check', 'unfinished_rows', 'memory')}, indent=1))
    for i in ISSUES[:40]:
        print('  %(sev)s %(check)s %(sheet)s [%(row)s] %(col)s: %(msg)s' % i)
    return 1 if summary['errors'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
