"""Blender script (run by tools/gen_banner3d.py): builds the 3D HOME Menu banner model with Blender and exports it
with Blender's own glTF exporter; also renders previews from the HOME Menu banner camera.

  blender --background --factory-startup --python tools/blender_banner.py -- <config.json>

config: {"model": "logo" | "cube" | "spincube", "image", "voxels", "depth", "text_voxels", "spin_s",
         "camera_gltf", "out_glb", "preview_dir", "preview_angles"}

Model: the logo (part of the image above the subtitle) is voxelized into cubes; Blender merges duplicate
vertices, removes hidden faces and dissolves coplanar faces (front/back become a few large polygons), so the
mesh stays small. All faces use one opaque texture holding the voxel colours (Closest = nearest filtering),
projected from the front; side faces sample the texels along their edge (empty texels are padded with their
neighbour's colour so edges never sample black). The subtitle is voxelized the same way (thinner) into the
same mesh and texture atlas, so logo and subtitle turn as one. Spin: pycgfx stores rotations as Euler angles with Y limited to +-90 degrees, so the turn is a
rotation about the "world" empty's local Y axis (glTF local Z after the Y-up conversion) inside a frame
tilted 90 degrees about X; the logo is tilted back. Every exported key is then a pure Z angle.
"""
import json, math, sys
import bpy, bmesh
import numpy as np

cfg = json.load(open(sys.argv[sys.argv.index('--') + 1]))
FPS = 30


def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scn = bpy.context.scene
    scn.render.fps = FPS
    return scn


def load_rgba(path):
    img = bpy.data.images.load(path)
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    return (px.reshape(h, w, 4)[::-1] * 255).astype(np.uint8)	# top row first


def split_logo_text(rgba):
    filled = (rgba[..., 3] > 16).any(1)
    h = len(filled)
    best, gap, y = None, 0, h // 2
    while y < h:
        if not filled[y]:
            s = y
            while y < h and not filled[y]:
                y += 1
            if y - s > gap and y < h:
                best, gap = (s, y), y - s
        y += 1

    def crop(a):
        ys, xs = np.nonzero(a[..., 3] > 16)
        return a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    return crop(rgba[:best[0]]), crop(rgba[best[1]:])


def voxelize(rgba, cols):
    h, w = rgba.shape[:2]
    cell = w / cols
    rows = max(1, round(h / cell))
    mask = np.zeros((rows, cols), bool)
    col = np.zeros((rows, cols, 3), np.float32)
    for j in range(rows):
        for i in range(cols):
            blk = rgba[int(j * cell):max(int(j * cell) + 1, int((j + 1) * cell)),
                       int(i * cell):max(int(i * cell) + 1, int((i + 1) * cell))].reshape(-1, 4).astype(np.float32)
            a = blk[:, 3] / 255
            if a.mean() > 0.45:
                mask[j, i] = True
                col[j, i] = (blk[:, :3] * a[:, None]).sum(0) / a.sum() / 255
    return mask, col


def pow2(v):
    p = 8
    while p < v:
        p <<= 1
    return p


def voxel_atlas(name, parts):
    """Voxel colours of all parts stacked in one texture (part k from row rows[k]; 1 empty row between parts),
    bottom row first for Blender; empty texels padded from their neighbours."""
    rows, r = [], 0
    for mask, col, *_ in parts:
        rows.append(r)
        r += mask.shape[0] + 1
    TW, TH = pow2(max(m.shape[1] for m, *_ in parts)), pow2(r)
    tex = np.zeros((TH, TW, 3), np.float32)
    have = np.zeros((TH, TW), bool)
    for (mask, col, *_), r0 in zip(parts, rows):
        R, C = mask.shape
        tex[r0:r0 + R, :C] = col
        have[r0:r0 + R, :C] = mask
    for _ in range(3):	# dilate colours into empty texels
        grow = tex.copy()
        got = have.copy()
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            src = np.roll(np.roll(tex, dy, 0), dx, 1)
            sh = np.roll(np.roll(have, dy, 0), dx, 1)
            fill = sh & ~got
            grow[fill] = src[fill]
            got |= fill
        tex, have = grow, got
    img = bpy.data.images.new(name, TW, TH, alpha=False)
    px = np.ones((TH, TW, 4), np.float32)
    px[..., :3] = tex
    img.pixels.foreach_set(px[::-1].ravel())
    img.filepath_raw = '//' + name + '.png'
    img.file_format = 'PNG'
    img.pack()
    return img, TW, TH, rows


def voxel_object(name, parts):
    """One object, one material: every part = (mask, colours, width, depth in voxels, centre height). Exposed
    voxel faces, merged and dissolved, front-projected UVs into a shared texture atlas."""
    img, TW, TH, rows = voxel_atlas(name + '_tex', parts)
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new('UVMap')
    for (mask, col, width, depth_cells, zc), r0 in zip(parts, rows):
        R, C = mask.shape
        s = width / C
        d = depth_cells * s / 2
        part = bmesh.new()
        vcache = {}

        def v(x, y, z):
            k = (round(x, 5), round(y, 5), round(z, 5))
            if k not in vcache:
                vcache[k] = part.verts.new(k)
            return vcache[k]

        X = lambda i: (i - C / 2) * s
        Z = lambda j: (R / 2 - j) * s	# Blender: Z is up, -Y faces the camera
        for j in range(R):
            for i in range(C):
                if not mask[j, i]:
                    continue
                x0, x1, z0, z1 = X(i), X(i + 1), Z(j + 1), Z(j)
                faces = [((x0, -d, z0), (x1, -d, z0), (x1, -d, z1), (x0, -d, z1)),	# front (-Y)
                         ((x1, d, z0), (x0, d, z0), (x0, d, z1), (x1, d, z1))]		# back (+Y)
                if i == 0 or not mask[j, i - 1]:
                    faces.append(((x0, d, z0), (x0, -d, z0), (x0, -d, z1), (x0, d, z1)))
                if i == C - 1 or not mask[j, i + 1]:
                    faces.append(((x1, -d, z0), (x1, d, z0), (x1, d, z1), (x1, -d, z1)))
                if j == 0 or not mask[j - 1, i]:
                    faces.append(((x0, -d, z1), (x1, -d, z1), (x1, d, z1), (x0, d, z1)))
                if j == R - 1 or not mask[j + 1, i]:
                    faces.append(((x0, d, z0), (x1, d, z0), (x1, -d, z0), (x0, -d, z0)))
                for f in faces:
                    part.faces.new([v(*p) for p in f])
        bmesh.ops.recalc_face_normals(part, faces=part.faces)
        bmesh.ops.dissolve_limit(part, angle_limit=math.radians(0.5), verts=part.verts[:], edges=part.edges[:],
                                 delimit={'NORMAL'})
        # copy into the shared mesh, moved to its height, with atlas UVs
        vmap = {pv: bm.verts.new((pv.co.x, pv.co.y, pv.co.z + zc)) for pv in part.verts}
        for pf in part.faces:
            f = bm.faces.new([vmap[pv] for pv in pf.verts])
            n = pf.normal	# side faces sample half a voxel inward: their own voxel, not the texel border
            for loop, ploop in zip(f.loops, pf.loops):
                x, z = ploop.vert.co.x - n.x * 0.5 * s, ploop.vert.co.z - n.z * 0.5 * s
                loop[uv].uv = ((x / s + C / 2) / TW, (TH - r0 - (R / 2 - z / s)) / TH)
        part.free()
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    mat = bpy.data.materials.new(name + '_mat')
    mat.use_backface_culling = True	# exported as single-sided (pycgfx would duplicate every vertex)
    if hasattr(mat, 'use_nodes'):
        mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get('Principled BSDF') or nt.nodes.new('ShaderNodeBsdfPrincipled')
    texn = nt.nodes.new('ShaderNodeTexImage')
    texn.image = img
    texn.interpolation = 'Closest'
    nt.links.new(texn.outputs['Color'], bsdf.inputs['Base Color'])
    bsdf.inputs['Roughness'].default_value = 1.0
    bsdf.inputs['Metallic'].default_value = 0.0
    out = nt.nodes.get('Material Output') or nt.nodes.new('ShaderNodeOutputMaterial')
    if not out.inputs['Surface'].is_linked:
        nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    me.materials.append(mat)
    return obj, len(me.polygons)


def empty(name, parent=None, loc=(0, 0, 0), rot=(0, 0, 0)):
    e = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(e)
    e.parent = parent
    e.location = loc
    e.rotation_euler = rot
    return e


def spinning_rig(scn, child, height):
    """COMMON -> spin_frame (X +90) -> world (keyframed local Y turn) -> child (X -90)."""
    common = bpy.data.objects['COMMON']
    frame = empty('spin_frame', common, (0, 0, height), (math.radians(90), 0, 0))
    world = empty('world', frame)
    child.parent = world
    child.rotation_euler = (math.radians(-90), 0, 0)
    frames = int(round(cfg['spin_s'] * FPS))
    scn.frame_start, scn.frame_end = 0, frames
    world.rotation_euler = (0, 0, 0)
    world.keyframe_insert('rotation_euler', index=1, frame=0)
    world.rotation_euler = (0, -2 * math.pi, 0)	# counter-clockwise seen from above
    world.keyframe_insert('rotation_euler', index=1, frame=frames)
    fc = world.animation_data.action.fcurves if hasattr(world.animation_data.action, 'fcurves') else None
    if fc is None:	# Blender 4.4+ layered actions
        fc = world.animation_data.action.layers[0].strips[0].channelbags[0].fcurves
    for c in fc:
        for k in c.keyframe_points:
            k.interpolation = 'LINEAR'
    return world


def preview(scn, angles_deg):
    bpy.ops.import_scene.gltf(filepath=cfg['camera_gltf'])
    cam = next(o for o in scn.objects if o.type == 'CAMERA')
    scn.camera = cam
    scn.render.engine = 'BLENDER_WORKBENCH'
    scn.display.shading.light = 'STUDIO'
    scn.display.shading.color_type = 'TEXTURE'
    scn.render.resolution_x, scn.render.resolution_y = 400, 240
    scn.render.film_transparent = False
    world = bpy.data.worlds.new('bg')
    scn.world = world
    paths = []
    for a in angles_deg:
        scn.frame_set(int(round((scn.frame_end - scn.frame_start) * a / 360.0)) if scn.frame_end else 0)
        scn.render.filepath = '%s/preview_%03d.png' % (cfg['preview_dir'], a)
        bpy.ops.render.render(write_still=True)
        paths.append(scn.render.filepath)
    bpy.data.objects.remove(cam)	# not part of the banner
    return paths


def main():
    scn = clear()
    common = empty('COMMON')
    model = cfg['model']
    if model in ('cube', 'spincube'):
        rgba = np.zeros((8, 8, 4), np.uint8)
        rgba[...] = (200, 40, 20, 255)
        rgba[::2, ::2] = (240, 200, 60, 255)
        mask, col = voxelize(rgba, 8)
        cube, faces = voxel_object('cube', [(mask, col, 12.0, 8, 0.0)])
        if model == 'cube':
            cube.parent = common
            cube.location = (0, 0, 1)
        else:
            spinning_rig(scn, cube, 1.0)
    else:
        rgba = load_rgba(cfg['image'])
        logo, text = split_logo_text(rgba)
        mask, col = voxelize(logo, cfg['voxels'])
        tmask, tcol = voxelize(text, cfg['text_voxels'])
        # logo and subtitle are one mesh on the spinning rig: they turn together
        obj, faces = voxel_object('logo', [(mask, col, cfg['logo_width'], cfg['depth'], cfg['logo_y']),
                                           (tmask, tcol, cfg['text_width'], 2, cfg['text_y'])])
        spinning_rig(scn, obj, 0.0)
    bpy.ops.export_scene.gltf(filepath=cfg['out_glb'], export_format='GLB', export_yup=True,
                              export_animations=True, export_cameras=False, export_lights=False,
                              export_apply=False, export_extras=False)
    paths = preview(scn, cfg['preview_angles'])
    print('BANNER_OK faces=%d glb=%s previews=%s' % (faces, cfg['out_glb'], ','.join(paths)))


main()
