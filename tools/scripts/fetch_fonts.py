r"""Download the Pretendard UI font the overlay prefers.

The overlay draws its text with Pretendard when it can find it and falls back to
Malgun Gothic (which ships with Windows) when it cannot, so this script is a
quality upgrade and not a build dependency - nothing here is required to compile
or inject the trainer.

Pretendard is not redistributed in this repository: it is SIL OFL 1.1 licensed
and roughly 2 MB per weight, which does not belong in a source tree that is
otherwise text. Instead it is fetched into the same per-user directory the
trainer already uses for config.ini, which is where widgets.cpp looks first.

    python tools/scripts/fetch_fonts.py

Destination: %LOCALAPPDATA%\cbpk\fonts\
"""

import os
import sys
import urllib.request
import zipfile
import io

VERSION = "1.3.9"
ARCHIVE = f"https://github.com/orioncactus/pretendard/releases/download/v{VERSION}/Pretendard-{VERSION}.zip"
# The overlay loads a regular for body copy and a semibold for titles and section
# labels. Nothing else in the archive is used.
WANTED = ("Pretendard-Regular.ttf", "Pretendard-SemiBold.ttf")


def destination() -> str:
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        print("LOCALAPPDATA is not set; this script is Windows-only.")
        sys.exit(1)
    return os.path.join(local, "cbpk", "fonts")


def main() -> int:
    target = destination()
    os.makedirs(target, exist_ok=True)

    if all(os.path.exists(os.path.join(target, name)) for name in WANTED):
        print(f"Pretendard already present in {target}")
        return 0

    print(f"Downloading {ARCHIVE}")
    with urllib.request.urlopen(ARCHIVE, timeout=120) as response:
        payload = response.read()
    print(f"Downloaded {len(payload)} bytes")

    written = 0
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        for entry in archive.namelist():
            name = os.path.basename(entry)
            if name not in WANTED:
                continue
            data = archive.read(entry)
            path = os.path.join(target, name)
            with open(path, "wb") as handle:
                handle.write(data)
            print(f"Wrote {path} ({len(data)} bytes)")
            written += 1

    if written != len(WANTED):
        print(f"Expected {len(WANTED)} font files, wrote {written}. The archive layout may have changed.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
