#!/usr/bin/env python3
"""mypng2icns — convert a PNG into a macOS .icns (png2icns-compatible CLI).

The npm package `png2icns` is unreliable: it races async `sips` calls, emits
invalid `icon_64x64*.png` names that modern `iconutil` rejects, and uses
`sips -Z` which can produce non-square PNGs. This script does the same job
correctly:

  1. Build a temporary *.iconset with Apple's required names/sizes
  2. Resize with exact square pixels (`sips -z`, or Pillow as fallback)
  3. Run `iconutil --convert icns`

Usage (mirrors png2icns):
  ./mypng2icns.py icon.png
  ./mypng2icns.py icon.png -o AppIcon.icns
  ./mypng2icns.py icon.png -o AppIcon.icns -s 16,32,128,256,512

Requires macOS (`iconutil`). Image resize prefers `sips`, else Pillow.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Apple .iconset logical sizes (NOT 64 — that only exists as icon_32x32@2x).
VALID_BASE_SIZES = (16, 32, 128, 256, 512)

# png2icns default included 64; we accept it on the CLI but map/ignore it.
DEFAULT_SIZES = [16, 32, 128, 256, 512]


class ConvertError(RuntimeError):
    pass


def parse_sizes(value: str) -> list[int]:
    parts = [p.strip() for p in value.split(",") if p.strip()]
    if not parts:
        raise argparse.ArgumentTypeError("size list is empty")
    out: list[int] = []
    for p in parts:
        try:
            n = int(p)
        except ValueError as e:
            raise argparse.ArgumentTypeError(f"invalid size {p!r}") from e
        if n <= 0:
            raise argparse.ArgumentTypeError(f"size must be positive: {n}")
        out.append(n)
    return out


def which(cmd: str) -> str | None:
    return shutil.which(cmd)


def resize_sips(src: Path, dest: Path, pixels: int) -> None:
    # Exact square: -z height width (unlike -Z which only caps the long edge).
    cp = subprocess.run(
        ["sips", "-z", str(pixels), str(pixels), str(src), "--out", str(dest)],
        capture_output=True,
        text=True,
    )
    if cp.returncode != 0 or not dest.is_file():
        err = (cp.stderr or cp.stdout or "").strip()
        raise ConvertError(f"sips failed for {pixels}x{pixels}: {err}")


def resize_pillow(src: Path, dest: Path, pixels: int) -> None:
    try:
        from PIL import Image
    except ImportError as e:
        raise ConvertError(
            "neither sips nor Pillow is available; install Pillow or run on macOS"
        ) from e
    with Image.open(src) as im:
        im = im.convert("RGBA")
        # Cover-fit into square then center-crop, so non-square sources still work.
        w, h = im.size
        scale = max(pixels / w, pixels / h)
        nw, nh = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
        im = im.resize((nw, nh), Image.Resampling.LANCZOS)
        left = (nw - pixels) // 2
        top = (nh - pixels) // 2
        im = im.crop((left, top, left + pixels, top + pixels))
        im.save(dest, format="PNG")


def resize_png(src: Path, dest: Path, pixels: int) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if which("sips"):
        # sips -z letterboxes? Actually it stretches to exact HxW.
        # Prefer preserve-aspect crop via Pillow when source is non-square.
        try:
            from PIL import Image

            with Image.open(src) as im:
                if im.size[0] != im.size[1]:
                    resize_pillow(src, dest, pixels)
                    return
        except ImportError:
            pass
        resize_sips(src, dest, pixels)
    else:
        resize_pillow(src, dest, pixels)


def normalize_sizes(requested: list[int]) -> list[int]:
    """Return sorted unique Apple base sizes; warn about invalid entries."""
    bases: set[int] = set()
    for n in requested:
        if n in VALID_BASE_SIZES:
            bases.add(n)
        elif n == 64:
            # npm png2icns defaulted to 64 → icon_64x64.png, which iconutil rejects.
            # 64px is covered by icon_32x32@2x when 32 is present.
            print(
                "warning: size 64 is not a valid .iconset base name "
                "(use 32, which provides icon_32x32@2x at 64px); ignoring",
                file=sys.stderr,
            )
            bases.add(32)
        else:
            print(
                f"warning: size {n} is not a valid Apple iconset base "
                f"{list(VALID_BASE_SIZES)}; ignoring",
                file=sys.stderr,
            )
    if not bases:
        raise ConvertError(
            f"no valid sizes left; choose from {list(VALID_BASE_SIZES)}"
        )
    return sorted(bases)


def icon_filename(base: int, scale: int) -> str:
    if scale == 1:
        return f"icon_{base}x{base}.png"
    return f"icon_{base}x{base}@{scale}x.png"


def build_iconset(src: Path, iconset: Path, bases: list[int]) -> None:
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir(parents=True)
    for base in bases:
        for scale in (1, 2):
            pixels = base * scale
            out = iconset / icon_filename(base, scale)
            resize_png(src, out, pixels)
            # iconutil is picky: dimensions must match the name exactly.
            verify_png_size(out, pixels)


def verify_png_size(path: Path, expected: int) -> None:
    if which("sips"):
        cp = subprocess.run(
            ["sips", "-g", "pixelWidth", "-g", "pixelHeight", str(path)],
            capture_output=True,
            text=True,
        )
        w = h = None
        for line in cp.stdout.splitlines():
            if "pixelWidth:" in line:
                w = int(line.split(":")[-1].strip())
            elif "pixelHeight:" in line:
                h = int(line.split(":")[-1].strip())
        if w != expected or h != expected:
            raise ConvertError(
                f"{path.name}: expected {expected}x{expected}, got {w}x{h}"
            )
        return
    from PIL import Image

    with Image.open(path) as im:
        if im.size != (expected, expected):
            raise ConvertError(
                f"{path.name}: expected {expected}x{expected}, got {im.size[0]}x{im.size[1]}"
            )


def run_iconutil(iconset: Path, output: Path) -> None:
    iconutil = which("iconutil")
    if not iconutil:
        raise ConvertError("iconutil not found (macOS only)")

    output.parent.mkdir(parents=True, exist_ok=True)
    # Prefer long options (current macOS); fall back to short -c/-o.
    attempts = [
        [iconutil, "--convert", "icns", "--output", str(output), str(iconset)],
        [iconutil, "-c", "icns", "-o", str(output), str(iconset)],
    ]
    last_err = ""
    for cmd in attempts:
        cp = subprocess.run(cmd, capture_output=True, text=True)
        if cp.returncode == 0 and output.is_file():
            return
        last_err = (cp.stderr or cp.stdout or "").strip() or f"exit {cp.returncode}"
    raise ConvertError(f"iconutil failed: {last_err}")


def convert(src: Path, output: Path, sizes: list[int], *, keep_iconset: Path | None = None) -> None:
    if not src.is_file():
        raise ConvertError(f"input not found: {src}")
    bases = normalize_sizes(sizes)

    if keep_iconset is not None:
        iconset = keep_iconset
        build_iconset(src, iconset, bases)
        run_iconutil(iconset, output)
        return

    with tempfile.TemporaryDirectory(prefix="mypng2icns_") as tmp:
        iconset = Path(tmp) / f"{output.stem}.iconset"
        build_iconset(src, iconset, bases)
        # iconutil writes beside the iconset unless --output is set; we always set it.
        run_iconutil(iconset, output)


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="mypng2icns",
        description="Convert a .png file to an .icns file (png2icns-compatible).",
    )
    p.add_argument("file", type=Path, help="path to a .png (or other image) file")
    p.add_argument(
        "-o",
        "--out",
        type=Path,
        default=Path("icon.icns"),
        help=".icns output path (default: icon.icns)",
    )
    p.add_argument(
        "-s",
        "--sizes",
        type=parse_sizes,
        default=DEFAULT_SIZES,
        metavar="LIST",
        help=f"comma-separated base sizes (default: {','.join(map(str, DEFAULT_SIZES))}; "
        f"valid: {','.join(map(str, VALID_BASE_SIZES))})",
    )
    p.add_argument(
        "--keep-iconset",
        type=Path,
        default=None,
        metavar="DIR",
        help="write the intermediate .iconset here instead of a temp dir",
    )
    p.add_argument("-v", "--verbose", action="store_true")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        if args.verbose:
            print(f"input:  {args.file}")
            print(f"output: {args.out}")
            print(f"sizes:  {args.sizes}")
        convert(args.file.resolve(), args.out.resolve(), args.sizes, keep_iconset=args.keep_iconset)
    except ConvertError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print("Successfully converted.")
    if args.verbose:
        print(f"wrote {args.out} ({args.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
