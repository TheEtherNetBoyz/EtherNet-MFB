"""Fail packaging if the bundled Online manifest, resources or native code are missing."""
import argparse
import io
import json
from pathlib import Path
import zipfile


def verify(bundle, native_library=None):
    source = Path(__file__).resolve().parents[1] / "mods" / "online"
    expected = json.loads((source / "mod.json").read_text(encoding="utf-8"))
    archive = None
    if bundle.suffix == ".apk":
        with zipfile.ZipFile(bundle) as apk:
            archive = zipfile.ZipFile(io.BytesIO(apk.read("assets/mods/dusklight_online.dusk")))
    elif bundle.is_file():
        archive = zipfile.ZipFile(bundle)
    try:
        read = archive.read if archive else lambda name: (bundle / name).read_bytes()
        manifest = json.loads(read("mod.json"))
        for key in ("id", "version"):
            if manifest[key] != expected[key]:
                raise ValueError(f"Bundled Online {key} does not match the submodule")
        resources = {
            "AlegreyaSC-Bold.ttf": source / "vendor/dusklight_remote_link/res/AlegreyaSC-Bold.ttf",
            "Inter-Regular.ttf": source / "vendor/dusklight_remote_link/res/Inter-Regular.ttf",
            "FONT_LICENSES.txt": source / "res/FONT_LICENSES.txt",
            "THIRD_PARTY_LICENSES.txt": source / "res/THIRD_PARTY_LICENSES.txt",
        }
        if manifest != expected:
            raise ValueError("Bundled Online manifest does not match the submodule")
        for name, resource in resources.items():
            if read("res/" + name) != resource.read_bytes():
                raise ValueError(f"Missing or altered Online resource: {name}")
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
            raise ValueError("Bundled Online native library is missing or empty")
    finally:
        if archive:
            archive.close()
    print(f"Verified bundled Online {expected['version']}: {bundle}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--native-library", type=Path, help="Relocated iOS framework library")
    args = parser.parse_args()
    verify(args.bundle, args.native_library)
