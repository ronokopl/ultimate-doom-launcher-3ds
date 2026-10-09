"""One-shot, re-runnable: sheet rows for gyro/touch aiming, bottom-screen modes and the audio fallbacks.

Shared by Doom Launcher 3DS and Brutal Doom 3DS (run it in each project's tools/ folder):
  platform.xlsx  Menu (3DS Options submenu + Customize Controls entry), BottomScreen (kind/modes columns, new
                 elements), BottomModes (new), Subsystems (aim, dspdump, csnd), Audio (output row)
  build.xlsx     Sources (aim_3ds.cpp, dspdump_3ds.cpp, csnd_3ds.cpp), Build (fs_access)
Status cells are left empty: tools/verify_build.py sets them.
"""
import os
from openpyxl import load_workbook
from pk3 import SHEETS

MENU = '3DS Options'
GYRO_MODES = 'Off|Hold button|Toggle button|Always on'
# physical 3DS button read directly (not through key bindings), so it can share a button with e.g. ADS
GYRO_BUTTONS = 'None|ZL|ZR|L|R|A|B|X|Y|D-Pad Up|D-Pad Down|D-Pad Left|D-Pad Right|SELECT'


def table(ws):
    hdr = [c.value for c in ws[1]]
    return hdr, {str(r[0].value): r for r in ws.iter_rows(min_row=2) if r[0].value is not None}


def ensure_cols(ws, cols):
    hdr = [c.value for c in ws[1]]
    for c in cols:
        if c not in hdr:
            ws.cell(1, len(hdr) + 1).value = c
            hdr.append(c)
    # keep status last
    return hdr


def set_row(ws, key, **vals):
    hdr, idx = table(ws)
    row = idx.get(key)
    if row is None:
        ws.append([key] + [None] * (len(hdr) - 1))
        row = list(ws.iter_rows(min_row=ws.max_row))[0]
    for k, v in vals.items():
        row[hdr.index(k)].value = v
    if 'status' in hdr:
        row[hdr.index('status')].value = None


def new_sheet(wb, name, header, rows):
    if name in wb.sheetnames:
        del wb[name]
    ws = wb.create_sheet(name)
    ws.append(header)
    for r in rows:
        ws.append([r.get(h, '~') for h in header])


BOTTOM_MODES = [
    dict(mode='map', value=0, menu_label='Automap', canvas='yes', backlight='on', notes='live automap + weapon strip (default)'),
    dict(mode='touchpad', value=1, menu_label='Touch aim pad', canvas='no', backlight='on', notes='whole upper area aims with the stylus + weapon strip'),
    dict(mode='status', value=2, menu_label='Status display', canvas='no', backlight='on', notes='big health/armor/ammo/keys/weapon + weapon strip'),
    dict(mode='grid', value=3, menu_label='Weapon grid', canvas='no', backlight='on', notes='large weapon slot and action buttons'),
    dict(mode='title', value=4, menu_label='Title art (dimmed)', canvas='no', backlight='on', notes='the mod title picture, 15% darker, + weapon strip'),
    dict(mode='off', value=5, menu_label='Screen off', canvas='no', backlight='off', notes='bottom backlight off; touch still aims'),
]

STRIP = 'map touchpad status title'


def bottom_rows():
    rows = [dict(element='automap', kind='automap', modes='map', x=0, y=0, w=320, h=184)]
    for n in range(1, 10):
        rows.append(dict(element='slot%d' % n, kind='button', modes=STRIP, x=2 + 35 * (n - 1), y=186, w=34, h=52,
                         command='slot %d' % n, label=str(n), texture='SHAWN2', font_owned='STTNUM', font_empty='STTNUM'))
    rows += [
        dict(element='aim_map', kind='aim', modes='map title', x=0, y=0, w=320, h=184, command='aim'),
        dict(element='aim_status', kind='aim', modes='status', x=0, y=0, w=320, h=184, command='aim_if_enabled'),
        dict(element='aim_pad', kind='aim', modes='touchpad', x=0, y=0, w=320, h=184, command='aim', label='TOUCH TO AIM',
             font_owned='STCFN'),
        dict(element='aim_off', kind='aim', modes='off', x=0, y=0, w=320, h=240, command='aim'),
        dict(element='title_art', kind='art', modes='title', x=0, y=0, w=320, h=240, label='titlepage dimmed 15%'),
        dict(element='st_health', kind='status', modes='status', x=4, y=6, w=100, h=78, label='HEALTH', font_owned='STTNUM', font_empty='STCFN'),
        dict(element='st_armor', kind='status', modes='status', x=110, y=6, w=100, h=78, label='ARMOR', font_owned='STTNUM', font_empty='STCFN'),
        dict(element='st_ammo', kind='status', modes='status', x=216, y=6, w=100, h=78, label='AMMO', font_owned='STTNUM', font_empty='STCFN'),
        dict(element='st_weapon', kind='status', modes='status', x=4, y=90, w=206, h=90, label='WEAPON', font_owned='~', font_empty='STCFN'),
        dict(element='st_keys', kind='status', modes='status', x=216, y=90, w=100, h=90, label='KEYS', font_owned='~', font_empty='STCFN'),
    ]
    grid = ['slot 1', 'slot 2', 'slot 3', 'slot 4', 'slot 5', 'slot 6', 'slot 7', 'slot 8', 'slot 9', 'slot 0',
            '+use', '+jump', '+crouch', 'togglemap', 'weapprev', 'weapnext']
    labels = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'USE', 'JUMP', 'CROUCH', 'MAP', 'PREV', 'NEXT']
    for i, (cmd, lab) in enumerate(zip(grid, labels)):
        r, c = divmod(i, 4)
        rows.append(dict(element='g_' + lab.lower(), kind='button', modes='grid', x=2 + 80 * c, y=2 + 60 * r, w=76, h=56,
                         command=cmd, label=lab, texture='SHAWN2', font_owned='STTNUM' if lab.isdigit() else 'STCFN',
                         font_empty='STTNUM' if lab.isdigit() else 'STCFN'))
    return rows


def menu_rows(wb):
    """3DS Options rows (re-runnable on its own: tools/aim_sheets.py --menu)."""
    ws = wb['Menu']
    for opt in ('Screen resolution', 'Render scale', 'Precache monster frames'):
        hdr, idx = table(ws)
        if opt in idx:
            set_row(ws, opt, menu=MENU)
    menu = [
        ('Bottom screen', 'bottom_mode', 'BottomModes sheet', 'Automap', 'option'),
        # L <-> ZL, R <-> ZR before the bindings (i_input_3ds.cpp): mods that fire on bumpers vs triggers
        ('Swap bumpers and triggers', 'in_3dsswapshoulders', 'Off|On', 'Off', 'option'),
        # defaults = the setup tested on hardware (gyro itself off until the player enables it). Physical motion
        # names: yaw = turning the console, pitch = tilting it forward/back, roll = tilting it sideways (the tested
        # setup was listed as "Pitch"/"Roll inverted" under libctru's sensor names; new cvar names, so old saves
        # of the swapped meaning do not carry over)
        ('Gyro aiming', 'gyro_mode', GYRO_MODES, 'Off', 'option'),
        ('Gyro button', 'gyro_button', GYRO_BUTTONS, 'ZL', 'option'),
        ('Gyro left/right from', 'gyro_axis_lr', 'Yaw|Roll|Pitch', 'Yaw', 'option'),
        ('Gyro up/down from', 'gyro_axis_ud', 'Pitch|Roll|Yaw|Off', 'Pitch', 'option'),
        ('Gyro invert left/right', 'gyro_invert_lr', 'Off|On', 'Off', 'option'),
        ('Gyro invert up/down', 'gyro_invert_ud', 'Off|On', 'Off', 'option'),
        ('Gyro drift correction', 'gyro_autocalib', 'Off|On', 'On', 'option'),
        ('Gyro horizontal speed', 'gyro_sens_x', '0.1, 10.0, 0.1, 1', '1.5', 'slider'),
        ('Gyro vertical speed', 'gyro_sens_y', '0.1, 10.0, 0.1, 1', '1.5', 'slider'),
        ('Touch aim on status screen', 'touch_aim', 'Off|On', 'Off', 'option'),
        ('Touch horizontal speed', 'touch_sens_x', '0.1, 10.0, 0.1, 1', '1.0', 'slider'),
        ('Touch vertical speed', 'touch_sens_y', '0.1, 10.0, 0.1, 1', '1.0', 'slider'),
        ('Touch invert vertical', 'touch_invert_y', 'Off|On', 'Off', 'option'),
    ]
    # rows are re-appended in this order (the generated menu follows row order); old ones are dropped
    managed = [m[0] for m in menu] + ['Gyro turns with', 'Gyro invert horizontal', 'Gyro invert vertical',
                                      'Touch aiming', 'Gyro aim (hold / toggle)', 'Gyro invert yaw',
                                      'Gyro invert pitch', 'Gyro invert roll']
    for r in reversed(range(2, ws.max_row + 1)):
        if ws.cell(r, 1).value in managed:
            ws.delete_rows(r)
    for opt, cvar, values, default, widget in menu:
        set_row(ws, opt, menu=MENU, cvar=cvar, values=values, default=default, widget=widget)


def platform():
    p = os.path.join(SHEETS, 'platform.xlsx')
    wb = load_workbook(p)
    menu_rows(wb)

    ws = wb['BottomScreen']
    ensure_cols(ws, ['kind', 'modes'])
    hdr = [c.value for c in ws[1]]
    for r in reversed(range(2, ws.max_row + 1)):
        ws.delete_rows(r)
    for r in bottom_rows():
        ws.append([r.get(h, '~') if h != 'status' else None for h in hdr])
    new_sheet(wb, 'BottomModes', ['mode', 'value', 'menu_label', 'canvas', 'backlight', 'notes', 'status'],
              [dict(r, status=None) for r in BOTTOM_MODES])
    for name in wb['BottomModes'].iter_rows(min_row=2):
        name[6].value = None

    ws = wb['Filesystem']	# shared sound/profiler code writes diagnostics into the app folder
    hdr, idx = table(ws)
    if 'app_dir' not in idx:
        cfg = idx['config'][hdr.index('path')].value
        set_row(ws, 'app_dir', path=cfg[:cfg.rfind('/') + 1], source='runtime (diagnostics: ndsp.txt, hitches.txt, fb_*.raw dumps)')
    ws = wb['Subsystems']
    set_row(ws, 'aim', replaces='~', impl='platform/3ds/aim_3ds.cpp', api='libctru hid (gyro) + engine view',
            notes='gyro aiming (hold/toggle/always) and stylus touchpad aiming (Menu sheet: 3DS Options)')
    set_row(ws, 'dspdump', replaces='~', impl='platform/3ds/dspdump_3ds.cpp', api='fs (title content archive)',
            notes='no sdmc:/3ds/dspfirm.cdc: extract the DSP firmware from this console\'s HOME Menu (DSP1 method)')
    set_row(ws, 'csnd', replaces='~', impl='platform/3ds/csnd_3ds.cpp', api='csnd',
            notes='DSP unavailable: software mixer for the ndspsound voices/streams, played on two CSND channels')
    ws = wb['Audio']
    set_row(ws, 'output', source_format='mixed voices', target='ndsp (DSP firmware) or csnd fallback',
            where='runtime (dspdump_3ds.cpp, csnd_3ds.cpp)', notes='firmware auto-extracted on first start; CSND when the DSP cannot start')
    wb.save(p)


def build():
    p = os.path.join(SHEETS, 'build.xlsx')
    wb = load_workbook(p)
    for unit, sub in (('platform/3ds/aim_3ds.cpp', 'aim'), ('platform/3ds/dspdump_3ds.cpp', 'dspdump'),
                      ('platform/3ds/csnd_3ds.cpp', 'csnd')):
        set_row(wb['Sources'], unit, module='platform', compile='yes', reason='3DS platform layer (Subsystems sheet: %s)' % sub, flags='~')
    set_row(wb['Build'], 'fs_access', value='CategorySystemApplication CategoryFileSystemTool Core CtrNandRo DirectSdmc DirectSdmcWrite',
            notes='exheader FileSystemAccess; the first four let dspdump_3ds.cpp read the HOME Menu code from NAND (as DSP1 does)')
    wb.save(p)


if __name__ == '__main__':
    import sys
    if '--menu' in sys.argv:	# only the 3DS Options rows (the other sheets may have been edited since)
        p = os.path.join(SHEETS, 'platform.xlsx')
        wb = load_workbook(p)
        menu_rows(wb)
        wb.save(p)
        print('3DS Options menu rows updated')
    else:
        platform()
        build()
        print('aim/bottom/audio sheet rows updated')
