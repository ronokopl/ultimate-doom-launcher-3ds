"""Filesystem sheet rows with a romfs:/ path -> build/romfs/ (embedded in the .3dsx / CIA).

source column: 'gzdoom/...' = read-only source data next to the port; 'engine/.../static' = directory zipped into a pk3;
anything else = a file relative to the port root. Copies are skipped when size+mtime already match.
The engine pk3's MENUDEF gets the Menu sheet rows: a "3DS Options" submenu (linked from Options), options injected
into existing menus, and Customize Controls entries.
"""
import os, re, shutil, zipfile
from export import rows
from pk3 import ROOT

OUT = os.path.join(ROOT, 'build', 'romfs')
PARENT = os.path.dirname(ROOT)
OWN_MENU = 'BD3DSOptions'	# the "3DS Options" submenu, created here and linked from the main Options menu

def resolve(src):
    src = src.split(' (')[0].strip()
    return os.path.join(PARENT if src.startswith('gzdoom/') else ROOT, src)

def option_value(name, labels):
    return 'OptionValue "%s"\n{\n%s\n}\n' % (name, '\n'.join('\t%d, "%s"' % (i, l) for i, l in enumerate(labels)))

def menu_struts():
    """Menu sheet rows -> MENUDEF lines injected into the engine's menus (2.8.1 has no AddOptionMenu)."""
    lists = {'vid_3dsmode': [v['menu_label'] for v in rows('platform', 'Video')],
             'bottom_mode': [m['menu_label'] for m in sorted(rows('platform', 'BottomModes'), key=lambda m: int(m['value']))]}
    inject, values, controls = {}, [], []
    for m in rows('platform', 'Menu'):
        if m.get('widget') == 'control':	# Customize Controls entry: values = console command
            controls.append('\tControl "%s", "%s"' % (m['option'], m['values']))
            continue
        if m.get('cvar', '~') == '~':
            continue	# engine-provided menu (e.g. Customize Controls)
        menu = {'Display Options': 'VideoOptions', '3DS Options': OWN_MENU}.get(m['menu'], m['menu'])
        name = 'BD3DS_' + m['cvar']
        if m.get('widget') == 'slider':	# values = "min, max, step, decimals"
            inject.setdefault(menu, []).append('\tSlider "%s", "%s", %s' % (m['option'], m['cvar'], m['values']))
            continue
        if m['values'] == 'Off|On':	# engine OptionValue for booleans
            name = 'OnOff'
        elif m['cvar'] in lists:	# list from another sheet (Video, BottomModes)
            values.append(option_value(name, lists[m['cvar']]))
        elif '|' in m['values']:	# inline list: index = cvar value
            values.append(option_value(name, m['values'].split('|')))
        inject.setdefault(menu, []).append('\tOption "%s", "%s", "%s"' % (m['option'], m['cvar'], name))
    return inject, values, controls

def after_title(text, menu, lines):
    i = text.index('OptionMenu "%s"' % menu)
    title_end = text.index('\n', text.index('Title', text.index('{', i)))
    return text[:title_end + 1] + lines + text[title_end + 1:]

def patch_menudef(text):
    inject, values, controls = menu_struts()
    own = inject.pop(OWN_MENU, [])
    for menu, lines in inject.items():
        text = after_title(text, menu, '\n'.join(lines) + '\n')
    if own:
        text = after_title(text, 'OptionsMenu', '\tSubmenu "3DS Options", "%s"\n' % OWN_MENU)
        text += '\nOptionMenu "%s"\n{\n\tTitle "3DS OPTIONS"\n%s\n}\n' % (OWN_MENU, '\n'.join(own))
    if controls:	# right after the first Control (Fire)
        m = re.compile(r'\n\s*Control\s').search(text, text.index('OptionMenu "CustomizeControls"'))
        line_end = text.index('\n', m.end())
        text = text[:line_end + 1] + '\n'.join(controls) + '\n' + text[line_end + 1:]
    return '// [3DS] generated from sheets/platform.xlsx (Menu, Video, BottomModes) by tools/gen_romfs.py\n' + \
        '\n'.join(values) + '\n' + text

def zip_dir(src, dst):
    """Always rebuilt (small): the engine pk3 plus the generated menu struts."""
    with zipfile.ZipFile(dst + '.tmp', 'w', zipfile.ZIP_DEFLATED) as z:
        for d, _, fs in os.walk(src):
            for f in sorted(fs):
                full = os.path.join(d, f)
                arc = os.path.relpath(full, src).replace('\\', '/')
                if arc.lower() == 'menudef.txt':
                    z.writestr(arc, patch_menudef(open(full, encoding='latin1').read()).encode('latin1'))
                else:
                    z.write(full, arc)
    os.replace(dst + '.tmp', dst)
    return 'zipped (menu struts injected)'

def main():
    os.makedirs(OUT, exist_ok=True)
    for r in rows('platform', 'Filesystem'):
        path = r.get('path', '')
        if not path.startswith('romfs:/'):
            continue
        dst = os.path.join(OUT, path[len('romfs:/'):])
        src = resolve(r['source'])
        if os.path.isdir(src):
            state = zip_dir(src, dst)
        else:
            if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src) and os.path.getmtime(dst) >= os.path.getmtime(src):
                state = 'up to date'
            else:
                shutil.copy2(src, dst); state = 'copied'
        print('%-14s %-22s %7.1f MB  %s' % (r['role'], path, os.path.getsize(dst) / 2**20, state))

if __name__ == '__main__':
    main()
