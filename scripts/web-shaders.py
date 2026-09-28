"""Embed the shared Vulkan/WebGL shaders; never maintain generated copies."""
import argparse
import re
from pathlib import Path

def expand(path):
    source = path.read_text(encoding='utf-8')
    return re.sub(r'^#include "([^"\n]+)"$', lambda m: expand(path.parent / m[1]), source, flags=re.M)

def generate(folder):
    result = '#pragma once\nnamespace gt2view::web {\n'
    for ext in ['vert', 'frag']:
        source = expand(folder / f'scene.{ext}')
        # Vulkan links varyings by location; GLSL ES 3.00 links them by name.
        # Map both sides to common names without renaming the fragment output.
        names = ['Texel', 'Color', 'Page', 'Clut', 'Flags', 'TexelAffine', 'ColorAffine', 'Rect', 'Screen', 'Cache']
        prefix = 'out' if ext == 'vert' else 'in'
        aliases = '\n'.join(f'#define {prefix}{name} gt2{name}' for name in names)
        source = source.replace('#version 450', '#version 300 es\n#define GT2_WEBGL 1\n' + aliases, 1)
        result += f'inline constexpr char {ext}[] = R"GT2SHADER({source})GT2SHADER";\n'
    return result + '}\n'

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(args.source), encoding='utf-8')
