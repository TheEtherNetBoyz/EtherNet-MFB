#!/usr/bin/env python3

import pathlib
import sys


BAD_LIBRARY_DIR = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib"
BAD_TOKENS = (
    f"-L{BAD_LIBRARY_DIR}",
    f"-Wl,-rpath,{BAD_LIBRARY_DIR}",
)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {pathlib.Path(sys.argv[0]).name} <build.ninja>", file=sys.stderr)
        return 2

    ninja_file = pathlib.Path(sys.argv[1])
    contents = ninja_file.read_text()
    removed = 0

    for token in BAD_TOKENS:
        count = contents.count(token)
        contents = contents.replace(token, "")
        removed += count

    ninja_file.write_text(contents)

    if BAD_LIBRARY_DIR in contents:
        print(f"error: unsupported Command Line Tools path remains in {ninja_file}", file=sys.stderr)
        for line_number, line in enumerate(contents.splitlines(), start=1):
            if BAD_LIBRARY_DIR in line:
                print(f"{line_number}:{line}", file=sys.stderr)
        return 1

    print(f"Removed {removed} incompatible macOS linker path entries from {ninja_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
