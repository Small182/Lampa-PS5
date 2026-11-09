#!/usr/bin/env python3
"""Replace PS5 presentation assets in an already-built LAMP00001 title ZIP.

No PS5 SDK, WebKit compile, or eboot signing is required. The signed eboot.bin
is copied unchanged, preserving ZIP entry metadata and executable mode bits.
"""

from __future__ import annotations

import argparse
import shutil
import struct
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


TITLE_ID = "LAMP00001"


def validate_icon(data: bytes) -> None:
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "icon must be PNG"
    assert struct.unpack_from(">II", data, 16) == (512, 512), "icon must be 512x512"


def validate_dds(data: bytes) -> None:
    assert data[:4] == b"DDS ", "background must be DDS"
    height, width = struct.unpack_from("<II", data, 12)
    assert (width, height) == (3840, 2160), "background must be 3840x2160"
    assert data[84:88] == b"DX10", "background must have DX10 header"
    assert struct.unpack_from("<I", data, 128)[0] == 98, "background must use BC7 UNORM"
    assert struct.unpack_from("<I", data, 28)[0] == 1, "background must have one mipmap"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=Path, help="existing LAMP00001.zip")
    ap.add_argument("output", type=Path, help="rebranded LAMP00001.zip")
    ap.add_argument("icon_png", type=Path)
    ap.add_argument("background_dds", type=Path)
    args = ap.parse_args()

    assert args.source.resolve() != args.output.resolve(), "source and output must differ"
    icon = args.icon_png.read_bytes()
    dds = args.background_dds.read_bytes()
    validate_icon(icon)
    validate_dds(dds)
    replacements = {
        f"{TITLE_ID}/sce_sys/icon0.png": icon,
        f"{TITLE_ID}/sce_sys/pic0.dds": dds,
        f"{TITLE_ID}/sce_sys/pic1.dds": dds,
    }
    replaced: set[str] = set()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with ZipFile(args.source, "r") as old, ZipFile(
        args.output, "w", compression=ZIP_DEFLATED, compresslevel=6, allowZip64=True
    ) as new:
        names = set(old.namelist())
        missing = set(replacements) - names
        assert not missing, f"title archive is missing: {sorted(missing)}"
        assert f"{TITLE_ID}/eboot.bin" in names, "not a native PS5 title"
        for info in old.infolist():
            if info.filename in replacements:
                new.writestr(info, replacements[info.filename])
                replaced.add(info.filename)
                print(f"[branding] replaced {info.filename}")
            elif info.is_dir():
                new.writestr(info, b"")
            else:
                with old.open(info) as inp, new.open(info, "w") as out:
                    shutil.copyfileobj(inp, out, length=1024 * 1024)

    assert replaced == set(replacements)
    with ZipFile(args.output) as check, ZipFile(args.source) as source_check:
        assert check.testzip() is None, "rebranded zip failed CRC validation"
        assert check.read(f"{TITLE_ID}/eboot.bin") == source_check.read(
            f"{TITLE_ID}/eboot.bin"
        ), "signed eboot changed"
    print(f"[branding] ready: {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
