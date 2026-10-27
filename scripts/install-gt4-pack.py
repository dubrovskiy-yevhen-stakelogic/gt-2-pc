"""Install a verified GT4 bundle into a prepared desktop runtime, keeping backups."""
import argparse
import importlib.util
import shutil
import tarfile
import uuid
from pathlib import Path


def linked(path):
    return path.is_symlink() or (path.exists() and bool(getattr(path.stat(), 'st_file_attributes', 0) & 0x400))


def install(bundle, runtime):
    runtime = runtime.resolve(strict=True)
    bundle = bundle.resolve(strict=True)
    spec = importlib.util.spec_from_file_location('gt4_bundle', Path(__file__).with_name('package-quest-gt4.py'))
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    verifier.verify(bundle)
    roots = [Path('modelsets/gt4')]
    with tarfile.open(bundle) as archive:
        names = set(archive.getnames())
        for mode in ('arcade', 'simulation'):
            if f'{mode}/expansions/gt4/catalogue.version' in names:
                if not (runtime/mode/'disc.raw2352').is_file():
                    raise ValueError(f'Prepare the {mode} disc in this runtime before installing this bundle')
                roots.append(Path(mode)/'expansions/gt4')
        if not any((runtime/m/'disc.raw2352').is_file() for m in ('arcade', 'simulation')):
            raise ValueError('Select a prepared runtime containing arcade/ or simulation/')
        for rel in roots:
            p = runtime
            for part in rel.parts:
                p = p/part
                if linked(p):
                    raise ValueError('Model and expansion destinations must not contain links')
            if not p.resolve().is_relative_to(runtime):
                raise ValueError('Destination escapes runtime')
        token = uuid.uuid4().hex
        stage = runtime/('.gt4-stage-'+token)
        backup = runtime/'gt4-backups'/token
        if linked(backup.parent):
            raise ValueError('Backup directory must not be a link')
        stage.mkdir()
        completed = []
        moved_old = []
        try:
            for member in archive:
                path = stage/member.name
                if not path.resolve().is_relative_to(stage):
                    raise ValueError('Bundle path escapes staging directory')
                path.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, path.open('xb') as target:
                    shutil.copyfileobj(source, target)
            # Bundle validation precedes all changes to installed assets.
            for rel in roots:
                target = runtime/rel
                if target.exists():
                    previous = backup/rel
                    previous.parent.mkdir(parents=True, exist_ok=True)
                    target.rename(previous)
                    moved_old.append(rel)
                target.parent.mkdir(parents=True, exist_ok=True)
                (stage/rel).rename(target)
                completed.append(rel)
        except Exception:
            for rel in reversed(completed):
                target = runtime/rel
                if target.exists():
                    (stage/rel).parent.mkdir(parents=True, exist_ok=True)
                    target.rename(stage/rel)
            for rel in reversed(moved_old):
                (backup/rel).rename(runtime/rel)
            raise
        finally:
            assert stage.resolve().is_relative_to(runtime)
            shutil.rmtree(stage)
    print(f'GT4 models and catalogues installed: {runtime}')
    if moved_old:
        print(f'Previous GT4 assets retained: {backup}')
    print('Saves and graphics settings were preserved. Enable GT4 models in Graphics if previously disabled.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    args = parser.parse_args()
    install(args.bundle, args.runtime)
