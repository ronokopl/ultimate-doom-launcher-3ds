"""Build sheet banner_mode = 3d: Blender builds the voxel banner model (tools/blender_banner.py, Blender's own glTF
exporter + preview renders from the HOME Menu camera), pycgfx converts it -> gen/banner.cgfx.

  py tools/gen_banner3d.py [logo|cube|spincube] [out.cgfx]

Without arguments it builds the Build sheet's banner_model when banner_mode = 3d (else does nothing). The cube
and spincube models are the hardware test ladder (tools/banner_tests.py): a still cube, then the same cube on
the spinning rig, so a HOME Menu problem can be pinned to the converter, the animation/rig or the logo model.
"""
import json, os, subprocess, sys
from export import rows
from pk3 import ROOT, GEN

OUT_DIR = os.path.join(GEN, 'banner3d')


def build(model, out_cgfx, B):
    os.makedirs(OUT_DIR, exist_ok=True)
    glb = os.path.join(OUT_DIR, model + '.glb')
    k = float(B['banner_scale'])	# overall size; the camera looks at height 1 (banner-camera.gltf)
    cfg = {'model': model, 'image': os.path.join(ROOT, B['banner_logo_3d']), 'voxels': int(B['banner_voxels']),
           'depth': int(B['banner_depth']), 'text_voxels': int(B['banner_text_voxels']), 'logo_width': 25.0 * k,
           'logo_y': 1.0 + 1.6 * k, 'text_width': 28.0 * k, 'text_y': 1.0 - 10.0 * k, 'spin_s': float(B['banner_spin_s']),
           'camera_gltf': os.path.join(B['pycgfx_dir'], 'banner-camera.gltf'), 'out_glb': glb,
           'preview_dir': os.path.join(OUT_DIR, model), 'preview_angles': [0, 45, 90, 135, 180]}
    os.makedirs(cfg['preview_dir'], exist_ok=True)
    cfgpath = os.path.join(OUT_DIR, model + '.json')
    json.dump(cfg, open(cfgpath, 'w'), indent=1)
    r = subprocess.run([B['blender_exe'], '--background', '--factory-startup', '--python',
                        os.path.join(ROOT, 'tools', 'blender_banner.py'), '--', cfgpath],
                       capture_output=True, text=True)
    ok = [l for l in r.stdout.splitlines() if l.startswith('BANNER_OK')]
    if r.returncode or not ok:
        print(r.stdout[-3000:], r.stderr[-2000:])
        raise SystemExit('Blender failed (%s)' % model)
    r = subprocess.run([sys.executable, os.path.join(B['pycgfx_dir'], 'main.py'), glb, out_cgfx],
                       capture_output=True, text=True, cwd=B['pycgfx_dir'])
    if r.returncode or not os.path.exists(out_cgfx):
        print(r.stdout, r.stderr)
        raise SystemExit('pycgfx failed (%s)' % model)
    size = os.path.getsize(out_cgfx)
    print(out_cgfx, '%d KiB' % (size // 1024), ok[0].split(' previews=')[0], r.stdout.strip())
    if size > 512 * 1024:
        raise SystemExit('banner CGFX over the 512 KiB HOME Menu limit: lower banner_voxels or banner_depth')


def main():
    B = {r['key']: r.get('value', '~') for r in rows('build', 'Build')}
    if len(sys.argv) > 1:
        build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.join(OUT_DIR, sys.argv[1] + '.cgfx'), B)
    elif B.get('banner_mode') == '3d':
        build(B['banner_model'], os.path.join(GEN, 'banner.cgfx'), B)
    else:
        print('banner_mode is not 3d: flat banner only')


if __name__ == '__main__':
    main()
