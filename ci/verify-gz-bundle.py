"""Fail packaging if the bundled GZ manifest, resources or native code are missing."""
import argparse
import io
import json
from pathlib import Path
import zipfile


def verify(bundle, native_library=None):
    source = Path(__file__).resolve().parents[1] / "mods" / "gz"
    expected = json.loads((source / "mod.json").read_text(encoding="utf-8"))
    archive = None
    if bundle.suffix == ".apk":
        with zipfile.ZipFile(bundle) as apk:
            archive = zipfile.ZipFile(io.BytesIO(apk.read("assets/mods/dusk_gz.dusk")))
    elif bundle.is_file():
        archive = zipfile.ZipFile(bundle)
    try:
        read = archive.read if archive else lambda name: (bundle / name).read_bytes()
        manifest = json.loads(read("mod.json"))
        for key in ("id", "version"):
            if manifest[key] != expected[key]:
                raise ValueError(f"Bundled GZ {key} does not match the submodule")
        resources = list((source / "res").rglob("*"))
        for resource in resources:
            if resource.is_file():
                name = resource.relative_to(source).as_posix()
                if read(name) != resource.read_bytes():
                    raise ValueError(f"Missing or altered GZ resource: {name}")
        if native_library:
            native_present = native_library.is_file() and native_library.stat().st_size > 0
        elif archive:
            native_present = any(
                name.startswith("lib/") and Path(name).name in ("mod.dll", "mod.so", "mod.dylib")
                and archive.getinfo(name).file_size > 0
                for name in archive.namelist()
            )
        else:
            native_present = any(
                path.is_file() and path.name in ("mod.dll", "mod.so", "mod.dylib")
                and path.stat().st_size > 0
                for path in (bundle / "lib").rglob("*")
            )
        if not native_present:
            raise ValueError("Bundled GZ native library is missing or empty")
    finally:
        if archive:
            archive.close()
    print(f"Verified bundled GZ {expected['version']}: {bundle}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--native-library", type=Path, help="Relocated iOS framework library")
    args = parser.parse_args()
    verify(args.bundle, args.native_library)
