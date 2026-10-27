"""Convert GTPS2ModelTool OBJ dumps into the glTF subset used by GT2.

Keeps only the first model / highest detail level, excluding PS2 reflection,
shadow and destination-alpha passes. No dependency on Blender or Python packages.
"""
import collections
import json
import math
from pathlib import Path
import re
import shutil
import struct


def number(value):
    result = float(value.replace(',', '.'))
    if not math.isfinite(result):
        raise ValueError('Non-finite vertex data')
    return result


def shape_commands(path):
    shapes = {}
    shape = None
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if line.startswith('  model') and not line.startswith('  model0:'):
            break
        m = re.match(r'          (shape\w+):', line)
        if m:
            shape = m[1]
            shapes[shape] = []
        elif shape:
            shapes[shape].append(line.strip())
    return {name: '\n'.join(commands) for name, commands in shapes.items()}


def convert(dump, output, *, cockpit=False, exclude_shapes=(), cockpit_skip_textures=(),
            include_shapes=(), opaque_textures=(), mirror_textures=()):
    dump, output = Path(dump), Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    obj = dump / 'model0.obj'
    if not obj.exists():
        obj = dump / 'model0.lod0.obj'
    if not obj.exists():
        raise ValueError('No highest-detail model: ' + str(dump))
    commands = shape_commands(dump / 'model_set.yaml')
    addressing_path = dump / 'texture_addressing.json'
    addressing = json.loads(addressing_path.read_text()) if addressing_path.exists() else {}
    # Omitted wrap modes in the pinned dumper mean REGION_CLAMP. Repeating
    # these images turns one windshield border into multiple opaque stripes.
    wraps = {}
    texture = None
    for line in (dump / 'model_set.yaml').read_text(encoding='utf-8-sig').splitlines():
        match = re.match(r'  ([0-9]+\.[0-9]+\.png):', line)
        if match:
            texture = match[1]; wraps[texture] = [33071, 33071]
        elif texture:
            for axis, field in enumerate(('WrapModeS:', 'WrapModeT:')):
                if field in line and 'SCE_GS_REPEAT' in line:
                    wraps[texture][axis] = 10497
    mats = {}
    current = None
    for line in obj.with_suffix('.mtl').read_text(encoding='utf-8-sig').splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] == 'newmtl':
            current = mats.setdefault(t[1], {'color': [1, 1, 1, 1]})
        elif current is not None and t[0] == 'Kd':
            current['color'] = [number(x) for x in t[1:5]]
            while len(current['color']) < 4:
                current['color'].append(1)
        elif current is not None and t[0] == 'map_Kd':
            current['texture'] = ' '.join(t[1:])
        elif current is not None and t[0] == 'Ks':
            current['reflection'] = max(number(x) for x in t[1:4]) > 0
    positions, uv, normals = [], [], []
    groups = collections.defaultdict(list)
    shape, material = '', ''
    for line in obj.read_text(encoding='utf-8-sig').splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] == 'v':
            positions.append([number(x) for x in t[1:]])
        elif t[0] == 'vt':
            uv.append([number(x) for x in t[1:3]])
        elif t[0] == 'vn':
            normals.append([number(x) for x in t[1:4]])
        elif t[0] == 'o':
            shape = t[1]
        elif t[0] == 'usemtl':
            material = t[1]
        elif t[0] == 'f':
            if len(t) != 4:
                raise ValueError('Expected triangle OBJ faces')
            cmd = commands.get(shape, '')
            if shape not in include_shapes and (shape.endswith('_tail_lamp_on') or shape in exclude_shapes or 'UseExternalTexture: true' in cmd or
                    'UseUnknownShadowFlag: true' in cmd or 'EnableDestinationAlphaTest' in cmd or
                    'ColorMask(0x00FFFFFF)' in cmd or
                    ('DisableDepthMask' in cmd and 'BlendFunction(0, 2, 1, 1, 0)' in cmd)):
                continue
            # Glass is omitted for the seated view so no opaque baked window
            # texture blocks the headset; the exterior retains the original UVs.
            if cockpit and Path(mats.get(material, {}).get('texture', '')).name in cockpit_skip_textures:
                continue
            for token in t[1:]:
                groups[(shape, material)].append(tuple(int(x) if x else 0 for x in token.split('/')))
    gltf = {'asset': {'version': '2.0', 'generator': 'GT2 GT4 model set importer'},
            'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
            'meshes': [{'primitives': []}], 'materials': [], 'images': [], 'textures': [],
            'buffers': [], 'bufferViews': [], 'accessors': [], 'samplers': []}
    binary = bytearray()
    textures, material_ids = {}, {}

    def accessor(values, width):
        while len(binary) % 4:
            binary.append(0)
        at = len(binary)
        for value in values:
            binary.extend(struct.pack('<' + 'f' * width, *value))
        view = len(gltf['bufferViews'])
        gltf['bufferViews'].append({'buffer': 0, 'byteOffset': at, 'byteLength': len(binary) - at})
        index = len(gltf['accessors'])
        gltf['accessors'].append({'bufferView': view, 'componentType': 5126,
                                  'count': len(values), 'type': 'VEC' + str(width)})
        return index

    statistics = []
    for (shape, name), corners in groups.items():
        mat = mats.get(name, {'color': [1, 1, 1, 1]})
        # PS2 alpha state belongs to a draw, not to the shared texture/name.
        # In particular alpha also stores reflectivity on paint and glass.
        # FBMSK bits protect channels: FF000000 writes RGB, 00FFFFFF only alpha.
        cmd = commands.get(shape, '')
        # Disabling TEST does not disable alpha blending. Those passes still
        # have zero-coverage texels (notably Clio's roof/window overlay).
        blended = 'BlendFunction(0, 1, 0, 1, 0)' in cmd
        opaque = (Path(mat.get('texture', '')).name in opaque_textures or
                  (not blended and ('DisableAlphaTest' in cmd or
                                    'AlphaFunction(GEQUAL, 0)' in cmd)))
        material_key = (name, opaque)
        if material_key not in material_ids:
            material_ids[material_key] = len(gltf['materials'])
            pbr = {'baseColorFactor': mat['color'], 'metallicFactor': 0, 'roughnessFactor': 0.65}
            if mat.get('texture'):
                source = (dump / mat['texture']).resolve()
                if not source.is_relative_to(dump.resolve()):
                    raise ValueError('Texture path outside model dump')
                if str(source) not in textures:
                    index = len(gltf['images'])
                    destination = output.parent / 'textures' / source.name
                    destination.parent.mkdir(exist_ok=True)
                    shutil.copyfile(source, destination)
                    gltf['images'].append({'uri': 'textures/' + source.name})
                    wrap = wraps.get(source.name, [10497, 10497])
                    gltf['samplers'].append({'wrapS': wrap[0], 'wrapT': wrap[1]})
                    gltf['textures'].append({'source': index, 'sampler': index})
                    textures[str(source)] = index
                pbr['baseColorTexture'] = {'index': textures[str(source)]}
            gltf['materials'].append({'name': name, 'doubleSided': True, 'pbrMetallicRoughness': pbr,
                                      'alphaMode': 'OPAQUE' if opaque else 'MASK', 'alphaCutoff': 0.5/255,
                                      'extras': {'gt2pc': {'reflection': mat.get('reflection', False)}}})
            if cockpit and Path(mat.get('texture', '')).name in mirror_textures:
                gltf['materials'][-1]['extras']['gt2pc']['rearView'] = True
        uv_scale = [1, 1]
        address = addressing.get(Path(mat.get('texture', '')).name)
        if address:
            source = dump / mat['texture']
            header = source.read_bytes()[:24]
            if header[:8] != b'\x89PNG\r\n\x1a\n': raise ValueError('Expected PNG texture')
            width, height = struct.unpack('>II', header[16:24])
            uv_scale = [address['width']/width, address['height']/height]
        p, n, u, color = [], [], [], []
        for corner in corners:
            pi, ti, ni = (corner + (0, 0, 0))[:3]
            v = positions[pi - 1 if pi > 0 else pi]
            # Dumped GT4 and GT2 car meshes share +Y up / -Z forward.
            p.append(v[:3])
            normal = normals[ni - 1 if ni > 0 else ni] if ni else [0, 1, 0]
            n.append(normal)
            tex = uv[ti - 1 if ti > 0 else ti] if ti else [0, 0]
            # The dumper crops at CLAMP.MAX while UVs still address TEX0's
            # power-of-two allocation. Scale to the cropped PNG dimensions.
            u.append([tex[0]*uv_scale[0], (1 - tex[1])*uv_scale[1]])
            color.append(v[3:6] if len(v) >= 6 else [1, 1, 1])
        gltf['meshes'][0]['primitives'].append({'attributes': {
            'POSITION': accessor(p, 3), 'NORMAL': accessor(n, 3),
            'TEXCOORD_0': accessor(u, 2), 'COLOR_0': accessor(color, 3)},
            'material': material_ids[material_key], 'mode': 4})
        statistics.append({'shape': shape, 'material': name, 'triangles': len(p) // 3,
                           'min': [min(v[k] for v in p) for k in range(3)],
                           'max': [max(v[k] for v in p) for k in range(3)]})
    if not statistics:
        raise ValueError('No usable geometry after filtering')
    bin_path = output.with_suffix('.bin')
    gltf['buffers'].append({'uri': bin_path.name, 'byteLength': len(binary)})
    bin_path.write_bytes(binary)
    output.write_text(json.dumps(gltf, separators=(',', ':')), encoding='utf8')
    return {'triangles': sum(s['triangles'] for s in statistics), 'shapes': statistics,
            'textures': len(textures)}
