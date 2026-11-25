"""Package the reviewed 12-car R32 overlay for an existing GT4 installation."""
import argparse
from pathlib import Path
import zipfile
from authored_cockpit_pack import assets

def build(output):
    files=assets();output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for name,path in sorted(files.items()):archive.write(path,'modelsets/gt4/authored/'+name)
        archive.writestr('README.txt','Requires the updated GT2 player and an installed GT4 model pack. Copy modelsets into the game data folder, merging directories. This adds the twelve reviewed Nissan R32 cockpit mappings. No saves or settings are included. Keep attribution.json with each model.\n')
    with zipfile.ZipFile(output) as archive:
        if archive.testzip():raise ValueError('Corrupt cockpit archive')
    print(f'Verified {len(files)} cockpit files: {output}')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path);args=parser.parse_args();build(args.output)
