#!/usr/bin/env python3
"""mymacdeployqt — Python reimplementation of Qt's macdeployqt.

Makes a macOS .app bundle self-contained by copying linked frameworks /
dylibs into Contents/Frameworks, deploying Qt plugins and (optionally) QML
imports, rewriting install names with install_name_tool, writing qt.conf,
and optionally stripping, codesigning, and creating a .dmg.

Usage:
  ./mymacdeployqt.py App.app [options]

Options mirror macdeployqt where practical (see --help).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path
from typing import Iterable, Optional


SYSTEM_PREFIXES = (
    "/System/",
    "/usr/lib/",
    "/usr/libexec/",
    "/Library/Apple/",
)

# Plugin groups deployed when the named Qt framework is present (basename without .framework).
PLUGIN_GROUPS = {
    "QtGui": ("platforms", "imageformats", "iconengines", "styles", "platforminputcontexts"),
    "QtNetwork": ("tls", "networkinformation", "generic"),
    "QtSql": ("sqldrivers",),
    "QtMultimedia": ("multimedia",),
    "QtPositioning": ("position",),
    "QtSensors": ("sensors",),
    "QtPrintSupport": ("printsupport",),
    "QtWebEngineCore": (),  # handled separately if needed
}

# Always consider these plugin dirs when any Qt framework is deployed (macdeployqt behavior).
ALWAYS_PLUGIN_DIRS = ("platforms", "imageformats", "iconengines", "styles")

APPSTORE_SKIP_PLUGINS = {
    "libqsqlodbc.dylib",
    "libqsqlpsql.dylib",
}


class DeployError(RuntimeError):
    pass


def log(verbose: int, level: int, msg: str) -> None:
    """level: 1=error/warn, 2=normal, 3=debug — printed when verbose >= level."""
    if verbose >= level:
        print(msg, file=sys.stderr if level <= 1 else sys.stdout)


def run(cmd: list[str], *, check: bool = True, capture: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def is_system_path(path: str) -> bool:
    if path.startswith("@"):
        return False
    p = os.path.realpath(path) if os.path.exists(path) else path
    return any(p.startswith(pref) or path.startswith(pref) for pref in SYSTEM_PREFIXES)


def otool_deps(binary: Path) -> list[str]:
    """Return dependency install names from `otool -L` (excluding the binary's own id)."""
    cp = run(["otool", "-L", str(binary)], capture=True)
    deps: list[str] = []
    for line in cp.stdout.splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        m = re.match(r"^(\S+)\s+\(", line)
        if not m:
            continue
        deps.append(m.group(1))
    # Shared libraries list their own install name as the first entry.
    if deps and macho_filetype(binary) in ("MH_DYLIB", "MH_BUNDLE"):
        deps = deps[1:]
    return deps


def macho_filetype(binary: Path) -> str:
    cp = run(["otool", "-hv", str(binary)], capture=True, check=False)
    for line in cp.stdout.splitlines():
        if "MH_" in line:
            for tok in line.split():
                if tok.startswith("MH_"):
                    return tok
    return "UNKNOWN"


def otool_rpaths(binary: Path) -> list[str]:
    cp = run(["otool", "-l", str(binary)], capture=True)
    rpaths: list[str] = []
    lines = cp.stdout.splitlines()
    i = 0
    while i < len(lines):
        if "LC_RPATH" in lines[i]:
            # next "path ..." within a few lines
            for j in range(i + 1, min(i + 6, len(lines))):
                m = re.search(r"path\s+(\S+)\s+\(offset", lines[j])
                if m:
                    rpaths.append(m.group(1))
                    break
        i += 1
    return rpaths


def expand_loader_tokens(path: str, binary: Path) -> str:
    """Expand @executable_path / @loader_path relative to binary (best-effort)."""
    if path.startswith("@loader_path/"):
        return str((binary.parent / path[len("@loader_path/") :]).resolve())
    if path.startswith("@executable_path/"):
        # Caller may override; default: assume binary is the executable.
        return str((binary.parent / path[len("@executable_path/") :]).resolve())
    return path


def resolve_dependency(
    dep: str,
    binary: Path,
    executable: Path,
    libpaths: list[Path],
) -> Optional[Path]:
    """Resolve an install name to an on-disk library/framework binary."""
    if dep.startswith("@executable_path/"):
        cand = (executable.parent / dep[len("@executable_path/") :]).resolve()
        return cand if cand.is_file() else None
    if dep.startswith("@loader_path/"):
        cand = (binary.parent / dep[len("@loader_path/") :]).resolve()
        return cand if cand.is_file() else None
    if dep.startswith("@rpath/"):
        rest = dep[len("@rpath/") :]
        search: list[Path] = []
        for rp in otool_rpaths(binary):
            rp_exp = expand_loader_tokens(rp, binary)
            if rp_exp.startswith("@executable_path/"):
                rp_exp = str((executable.parent / rp_exp[len("@executable_path/") :]).resolve())
            search.append(Path(rp_exp))
        search.extend(libpaths)
        # Also search rpaths of the main executable.
        for rp in otool_rpaths(executable):
            rp_exp = expand_loader_tokens(rp, executable)
            search.append(Path(rp_exp))
        for base in search:
            cand = (base / rest).resolve()
            if cand.is_file():
                return cand
        return None
    # Absolute or relative path
    p = Path(dep)
    if p.is_file():
        return p.resolve()
    for base in libpaths:
        cand = (base / p.name).resolve()
        if cand.is_file():
            return cand
        # Framework-style: Foo.framework/Versions/A/Foo
        if ".framework/" in dep:
            # try libpath / FrameworkName.framework / ...
            parts = Path(dep).parts
            for i, part in enumerate(parts):
                if part.endswith(".framework"):
                    rel = Path(*parts[i:])
                    cand = (base / rel).resolve()
                    if cand.is_file():
                        return cand
                    break
    return None


def framework_root_from_binary(lib: Path) -> Optional[Path]:
    """If lib lives inside Foo.framework, return the Foo.framework directory."""
    parts = lib.parts
    for i, part in enumerate(parts):
        if part.endswith(".framework"):
            return Path(*parts[: i + 1])
    return None


def deployed_install_name(lib: Path, frameworks_dir_name: str = "Frameworks") -> str:
    """Install name used inside the bundle for a copied library."""
    fw = framework_root_from_binary(lib)
    if fw is not None:
        # Keep relative path from FrameworkName.framework/...
        rel = lib.relative_to(fw.parent)
        return f"@executable_path/../{frameworks_dir_name}/{rel.as_posix()}"
    return f"@executable_path/../{frameworks_dir_name}/{lib.name}"


def destination_for(lib: Path, frameworks_dir: Path) -> tuple[Path, Path]:
    """
    Return (dest_binary, copy_source_root).
    For frameworks, copy_source_root is the .framework dir and dest_binary is inside the copy.
    For dylibs, both point at the dylib under Frameworks/.
    """
    fw = framework_root_from_binary(lib)
    if fw is not None:
        dest_fw = frameworks_dir / fw.name
        rel_inside = lib.relative_to(fw)
        return dest_fw / rel_inside, fw
    dest = frameworks_dir / lib.name
    return dest, lib


def copy_library(
    lib: Path,
    frameworks_dir: Path,
    *,
    always_overwrite: bool,
    verbose: int,
) -> Path:
    """Copy framework or dylib into Frameworks; return path to the deployed binary."""
    dest_bin, source = destination_for(lib, frameworks_dir)
    frameworks_dir.mkdir(parents=True, exist_ok=True)

    if source.is_dir() and source.name.endswith(".framework"):
        dest_fw = frameworks_dir / source.name
        if dest_fw.exists():
            if always_overwrite:
                log(verbose, 2, f"Overwriting framework {dest_fw.name}")
                shutil.rmtree(dest_fw)
            else:
                log(verbose, 3, f"Framework already present: {dest_fw.name}")
                return dest_bin
        log(verbose, 2, f"Copying framework {source.name}")
        # Preserve symlinks (Versions/Current etc.)
        shutil.copytree(source, dest_fw, symlinks=True, dirs_exist_ok=False)
        # Ensure we can rewrite install names (Homebrew libs are often non-writable).
        for root, dirs, files in os.walk(dest_fw):
            for name in files:
                fp = Path(root) / name
                try:
                    mode = fp.stat().st_mode
                    fp.chmod(mode | 0o200)
                except OSError:
                    pass
        return dest_bin

    # Plain dylib
    if dest_bin.exists() and not always_overwrite:
        log(verbose, 3, f"Dylib already present: {dest_bin.name}")
        return dest_bin
    log(verbose, 2, f"Copying dylib {lib.name}")
    shutil.copy2(lib, dest_bin)
    try:
        dest_bin.chmod(dest_bin.stat().st_mode | 0o200)
    except OSError:
        pass
    return dest_bin


def change_install_id(binary: Path, new_id: str, verbose: int) -> None:
    log(verbose, 3, f"install_name_tool -id {new_id} {binary}")
    run(["install_name_tool", "-id", new_id, str(binary)], check=True)


def change_depend(binary: Path, old: str, new: str, verbose: int) -> None:
    if old == new:
        return
    log(verbose, 3, f"install_name_tool -change {old} {new} → {binary.name}")
    cp = run(["install_name_tool", "-change", old, new, str(binary)], check=False, capture=True)
    if cp.returncode != 0:
        # Already rewritten or absent — ignore common noise
        err = (cp.stderr or "") + (cp.stdout or "")
        if "would exceed maximum load command size" in err:
            raise DeployError(f"load command size exceeded while rewriting {binary}: {err}")
        log(verbose, 3, f"  (change skipped/failed: {err.strip()})")


def add_rpath(binary: Path, rpath: str, verbose: int) -> None:
    existing = otool_rpaths(binary)
    if rpath in existing:
        return
    log(verbose, 3, f"install_name_tool -add_rpath {rpath} {binary}")
    run(["install_name_tool", "-add_rpath", rpath, str(binary)], check=False)


def delete_rpath(binary: Path, rpath: str, verbose: int) -> None:
    log(verbose, 3, f"install_name_tool -delete_rpath {rpath} {binary}")
    run(["install_name_tool", "-delete_rpath", rpath, str(binary)], check=False)


def strip_binary(binary: Path, verbose: int) -> None:
    log(verbose, 3, f"strip -x {binary}")
    run(["strip", "-x", str(binary)], check=False)


def find_qt_prefixes(framework_paths: Iterable[Path]) -> list[Path]:
    """Infer Qt installation prefixes from deployed/source Qt framework locations."""
    prefixes: list[Path] = []
    seen: set[Path] = set()
    for fw_bin in framework_paths:
        fw = framework_root_from_binary(fw_bin) or fw_bin
        # .../lib/QtCore.framework → prefix is parent of lib
        if fw.name.endswith(".framework"):
            libdir = fw.parent
            prefix = libdir.parent if libdir.name == "lib" else libdir
        else:
            continue
        if not fw.name.startswith("Qt"):
            continue
        prefix = prefix.resolve()
        if prefix not in seen:
            seen.add(prefix)
            prefixes.append(prefix)
    return prefixes


def plugins_dirs_for_prefixes(prefixes: list[Path]) -> list[Path]:
    out: list[Path] = []
    for p in prefixes:
        candidates = [
            p / "share" / "qt" / "plugins",
            p / "plugins",
            p / "lib" / "qt" / "plugins",
            p / "lib" / "plugins",
        ]
        for c in candidates:
            if c.is_dir() and c not in out:
                out.append(c)
    return out


def qml_dirs_for_prefixes(prefixes: list[Path]) -> list[Path]:
    out: list[Path] = []
    for p in prefixes:
        candidates = [
            p / "share" / "qt" / "qml",
            p / "qml",
            p / "lib" / "qt" / "qml",
        ]
        for c in candidates:
            if c.is_dir() and c not in out:
                out.append(c)
    return out


def find_qmlimportscanner(prefixes: list[Path]) -> Optional[Path]:
    for p in prefixes:
        for c in (
            p / "share" / "qt" / "libexec" / "qmlimportscanner",
            p / "libexec" / "qmlimportscanner",
            p / "bin" / "qmlimportscanner",
        ):
            if c.is_file() and os.access(c, os.X_OK):
                return c
    # Homebrew qtdeclarative
    brew = Path("/opt/homebrew/opt/qtdeclarative/share/qt/libexec/qmlimportscanner")
    if brew.is_file():
        return brew
    which = shutil.which("qmlimportscanner")
    return Path(which) if which else None


def should_skip_plugin(name: str, appstore: bool, use_debug: bool) -> bool:
    if name.endswith("_debug.dylib") and not use_debug:
        return True
    if not use_debug and name.endswith("d.dylib") and name.startswith("libq"):
        # Qt debug suffix variants
        pass
    if "designer" in name.lower():
        return True
    if appstore and name in APPSTORE_SKIP_PLUGINS:
        return True
    return False


def deploy_plugins(
    plugin_roots: list[Path],
    plugins_dest: Path,
    deployed_qt_frameworks: set[str],
    *,
    appstore: bool,
    use_debug: bool,
    always_overwrite: bool,
    verbose: int,
) -> list[Path]:
    """Copy relevant plugin dylibs; return list of copied plugin binaries."""
    wanted_dirs: set[str] = set(ALWAYS_PLUGIN_DIRS)
    for fw in deployed_qt_frameworks:
        wanted_dirs.update(PLUGIN_GROUPS.get(fw, ()))

    copied: list[Path] = []
    for root in plugin_roots:
        for group in sorted(wanted_dirs):
            src_dir = root / group
            if not src_dir.is_dir():
                continue
            dest_dir = plugins_dest / group
            dest_dir.mkdir(parents=True, exist_ok=True)
            for item in sorted(src_dir.iterdir()):
                if not item.is_file() or item.suffix != ".dylib":
                    continue
                if should_skip_plugin(item.name, appstore, use_debug):
                    log(verbose, 3, f"Skipping plugin {item.name}")
                    continue
                dest = dest_dir / item.name
                if dest.exists() and not always_overwrite:
                    copied.append(dest)
                    continue
                log(verbose, 2, f"Copying plugin {group}/{item.name}")
                shutil.copy2(item, dest)
                try:
                    dest.chmod(dest.stat().st_mode | 0o200)
                except OSError:
                    pass
                copied.append(dest)
    return copied


def deploy_qml(
    qmldirs: list[Path],
    qmlimports: list[Path],
    qml_dest: Path,
    prefixes: list[Path],
    *,
    always_overwrite: bool,
    verbose: int,
) -> list[Path]:
    """Scan QML and copy imported modules into Resources/qml. Return extra binaries to fix."""
    import_paths = list(qmlimports) + qml_dirs_for_prefixes(prefixes)
    # Deduplicate while preserving order
    seen: set[Path] = set()
    paths: list[Path] = []
    for p in import_paths:
        rp = p.resolve()
        if rp not in seen and rp.is_dir():
            seen.add(rp)
            paths.append(rp)

    scanner = find_qmlimportscanner(prefixes)
    modules: list[dict] = []

    if scanner and qmldirs:
        for root in qmldirs:
            cmd = [str(scanner), "-rootPath", str(root)]
            for ip in paths:
                cmd.extend(["-importPath", str(ip)])
            log(verbose, 3, f"Running {' '.join(cmd)}")
            cp = run(cmd, capture=True, check=False)
            if cp.returncode != 0:
                log(verbose, 1, f"Warning: qmlimportscanner failed: {cp.stderr}")
                continue
            try:
                data = json.loads(cp.stdout)
            except json.JSONDecodeError:
                log(verbose, 1, "Warning: qmlimportscanner returned invalid JSON")
                continue
            if isinstance(data, list):
                modules.extend(data)
    elif qmldirs:
        log(verbose, 1, "Warning: qmlimportscanner not found; copying QtQuick/QtQml defaults")
        for ip in paths:
            for name in ("QtQuick", "QtQml", "Qt", "QML"):
                src = ip / name
                if src.is_dir():
                    modules.append({"name": name, "path": str(src), "type": "module"})

    binaries: list[Path] = []
    for mod in modules:
        if mod.get("type") not in (None, "module", "directory"):
            # skip javascript/relative unless path provided
            if "path" not in mod:
                continue
        src = Path(mod["path"]) if "path" in mod else None
        if src is None or not src.exists():
            log(verbose, 3, f"QML module missing on disk: {mod}")
            continue
        # Preserve module path relative to an import root when possible
        rel = None
        for ip in paths:
            try:
                rel = src.resolve().relative_to(ip.resolve())
                break
            except ValueError:
                continue
        if rel is None:
            rel = Path(mod.get("name", src.name).replace(".", "/"))
        dest = qml_dest / rel
        if src.is_dir():
            if dest.exists() and not always_overwrite:
                pass
            else:
                if dest.exists() and always_overwrite:
                    shutil.rmtree(dest)
                log(verbose, 2, f"Copying QML module {rel}")
                shutil.copytree(src, dest, symlinks=True, dirs_exist_ok=True)
            for root, _, files in os.walk(dest):
                for f in files:
                    fp = Path(root) / f
                    if fp.suffix in (".dylib", "") and is_macho(fp):
                        binaries.append(fp)
        else:
            dest.parent.mkdir(parents=True, exist_ok=True)
            if not dest.exists() or always_overwrite:
                shutil.copy2(src, dest)
            if is_macho(dest):
                binaries.append(dest)
    return binaries


def is_macho(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
        return magic in (
            b"\xfe\xed\xfa\xce",
            b"\xce\xfa\xed\xfe",
            b"\xfe\xed\xfa\xcf",
            b"\xcf\xfa\xed\xfe",
            b"\xca\xfe\xba\xbe",
            b"\xbe\xba\xfe\xca",
        )
    except OSError:
        return False


def write_qt_conf(resources: Path, *, has_qml: bool, verbose: int) -> None:
    resources.mkdir(parents=True, exist_ok=True)
    lines = ["[Paths]", "Plugins = PlugIns", "Translations = Resources"]
    if has_qml:
        lines.append("Imports = Resources/qml")
        lines.append("Qml2Imports = Resources/qml")
    text = "\n".join(lines) + "\n"
    conf = resources / "qt.conf"
    log(verbose, 2, f"Writing {conf}")
    conf.write_text(text, encoding="utf-8")


def iter_bundle_machos(app: Path) -> list[Path]:
    """Mach-O files under the bundle, frameworks/dylibs before executables (inside-out)."""
    contents = app / "Contents"
    buckets: list[list[Path]] = [[], [], [], []]  # frameworks, plugins, other, macos
    for root, _, files in os.walk(contents):
        root_p = Path(root)
        for name in files:
            fp = root_p / name
            if not is_macho(fp):
                continue
            rel = fp.relative_to(contents).as_posix()
            if rel.startswith("Frameworks/"):
                buckets[0].append(fp)
            elif rel.startswith("PlugIns/"):
                buckets[1].append(fp)
            elif rel.startswith("MacOS/"):
                buckets[3].append(fp)
            else:
                buckets[2].append(fp)
    out: list[Path] = []
    for b in buckets:
        out.extend(sorted(b, key=lambda p: p.as_posix()))
    return out


def codesign_path(
    path: Path,
    identity: str,
    *,
    hardened: bool,
    timestamp: bool,
    verbose: int,
    deep: bool = False,
) -> None:
    cmd = ["codesign", "--force", "--sign", identity]
    if deep:
        cmd.append("--deep")
    if hardened:
        cmd.extend(["--options", "runtime"])
    if timestamp:
        cmd.append("--timestamp")
    # For nil/ad-hoc signing, omit --timestamp=none — it can upset some codesign versions.
    elif identity != "-":
        cmd.append("--timestamp=none")
    cmd.append(str(path))
    log(verbose, 3, " ".join(cmd))
    run(cmd, check=True)


def codesign_bundle(app: Path, identity: str, *, hardened: bool, timestamp: bool, verbose: int) -> None:
    """Resign every Mach-O inside-out, then the .app (required after install_name_tool)."""
    label = "nil/ad-hoc" if identity == "-" else repr(identity)
    log(verbose, 2, f"Codesigning ({label})")
    for macho in iter_bundle_machos(app):
        log(verbose, 3, f"  sign {macho.relative_to(app)}")
        codesign_path(
            macho,
            identity,
            hardened=hardened,
            timestamp=timestamp,
            verbose=verbose,
        )
    codesign_path(
        app,
        identity,
        hardened=hardened,
        timestamp=timestamp,
        verbose=verbose,
        deep=True,
    )
    verify = run(["codesign", "--verify", "--deep", "--strict", str(app)], check=False, capture=True)
    if verify.returncode != 0:
        err = (verify.stderr or verify.stdout or "").strip()
        raise DeployError(f"codesign verify failed: {err}")
    log(verbose, 2, "Codesign verify OK")


def create_dmg(app: Path, fs: str, verbose: int) -> Path:
    dmg = app.with_suffix(".dmg")
    if dmg.exists():
        dmg.unlink()
    log(verbose, 2, f"Creating {dmg}")
    run(
        [
            "hdiutil",
            "create",
            "-fs",
            fs,
            "-volname",
            app.stem,
            "-srcfolder",
            str(app),
            str(dmg),
        ],
        check=True,
    )
    return dmg


def preprocess_argv(argv: list[str]) -> list[str]:
    """Support macdeployqt-style -option=value flags."""
    out: list[str] = []
    for a in argv:
        if a.startswith("-") and "=" in a and not a.startswith("--"):
            opt, _, val = a.partition("=")
            out.extend([opt, val])
        else:
            out.append(a)
    return out


def parse_args(argv: list[str]) -> argparse.Namespace:
    argv = preprocess_argv(argv)
    p = argparse.ArgumentParser(
        prog="mymacdeployqt",
        description="Make a macOS Qt app bundle self-contained (macdeployqt-compatible).",
    )
    p.add_argument("app_bundle", type=Path, help="Path to the .app bundle")
    p.add_argument("-verbose", metavar="N", type=int, default=1, choices=(0, 1, 2, 3))
    p.add_argument("-no-plugins", action="store_true", help="Skip plugin deployment")
    p.add_argument("-dmg", action="store_true", help="Create a .dmg disk image")
    p.add_argument("-no-strip", action="store_true", help="Don't run 'strip' on the binaries")
    p.add_argument(
        "-use-debug-libs",
        action="store_true",
        help="Deploy debug frameworks/plugins (implies -no-strip)",
    )
    p.add_argument(
        "-executable",
        action="append",
        default=[],
        metavar="PATH",
        help="Additional executable that should use the deployed frameworks",
    )
    p.add_argument("-qmldir", action="append", default=[], metavar="PATH", help="Scan QML imports in path")
    p.add_argument(
        "-qmlimport",
        action="append",
        default=[],
        metavar="PATH",
        help="Extra QML module search path",
    )
    p.add_argument("-always-overwrite", action="store_true")
    p.add_argument(
        "-codesign",
        nargs="?",
        const="-",
        default="-",
        metavar="IDENT",
        help="Codesign identity (default: ad-hoc '-').",
    )
    p.add_argument("-no-codesign", action="store_true")
    p.add_argument("-hardened-runtime", action="store_true")
    p.add_argument("-timestamp", action="store_true")
    p.add_argument(
        "-sign-for-notarization",
        metavar="IDENT",
        default=None,
        help="Codesign for notarization (hardened runtime + timestamp)",
    )
    p.add_argument("-appstore-compliant", action="store_true")
    p.add_argument("-libpath", action="append", default=[], metavar="PATH")
    p.add_argument("-fs", default="HFS+", metavar="FILESYSTEM", help="Filesystem for .dmg (default HFS+)")
    return p.parse_args(argv)


class Deployer:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.verbose = args.verbose
        self.app = args.app_bundle.resolve()
        if not self.app.is_dir() or self.app.suffix != ".app":
            raise DeployError(f"Not an application bundle: {self.app}")
        self.contents = self.app / "Contents"
        self.macos = self.contents / "MacOS"
        self.frameworks = self.contents / "Frameworks"
        self.plugins = self.contents / "PlugIns"
        self.resources = self.contents / "Resources"
        self.libpaths = [Path(p).resolve() for p in args.libpath]
        self.no_strip = args.no_strip or args.use_debug_libs
        # Maps realpath of source library → deployed install name
        self.deployed: dict[str, str] = {}
        # Source paths of Qt frameworks (for plugin discovery)
        self.qt_framework_bins: list[Path] = []
        self.deployed_qt_names: set[str] = set()
        self.queue: deque[Path] = deque()
        self.processed: set[str] = set()

    def main_executable(self) -> Path:
        # Prefer CFBundleExecutable from Info.plist; fall back to stem of .app
        plist = self.contents / "Info.plist"
        name = self.app.stem
        if plist.is_file():
            cp = run(["plutil", "-extract", "CFBundleExecutable", "raw", str(plist)], capture=True, check=False)
            if cp.returncode == 0 and cp.stdout.strip():
                name = cp.stdout.strip()
        exe = self.macos / name
        if not exe.is_file():
            # Fall back: first Mach-O in MacOS/
            candidates = [p for p in self.macos.iterdir() if p.is_file() and is_macho(p)]
            if not candidates:
                raise DeployError(f"No executable found in {self.macos}")
            exe = candidates[0]
            log(self.verbose, 1, f"Warning: using {exe.name} as main executable")
        return exe

    def enqueue(self, binary: Path) -> None:
        key = str(binary.resolve())
        if key not in self.processed:
            self.queue.append(binary.resolve())

    def deploy_dependency_of(self, binary: Path, dep_name: str, executable: Path) -> None:
        if dep_name.startswith("@executable_path/../Frameworks/"):
            return  # already rewritten
        if is_system_path(dep_name):
            return

        resolved = resolve_dependency(dep_name, binary, executable, self.libpaths)
        if resolved is None:
            if dep_name.startswith("@"):
                log(self.verbose, 1, f"Warning: could not resolve {dep_name} (from {binary.name})")
            else:
                log(self.verbose, 1, f"Warning: missing dependency {dep_name} (from {binary.name})")
            return

        if is_system_path(str(resolved)):
            return

        src_key = str(resolved.resolve())
        if src_key in self.deployed:
            new_name = self.deployed[src_key]
            change_depend(binary, dep_name, new_name, self.verbose)
            return

        dest_bin = copy_library(
            resolved,
            self.frameworks,
            always_overwrite=self.args.always_overwrite,
            verbose=self.verbose,
        )
        new_name = deployed_install_name(resolved)
        # If already inside our Frameworks from a previous copy, still set id
        change_install_id(dest_bin, new_name, self.verbose)
        change_depend(binary, dep_name, new_name, self.verbose)
        self.deployed[src_key] = new_name

        fw = framework_root_from_binary(resolved)
        if fw is not None and fw.name.startswith("Qt") and fw.name.endswith(".framework"):
            self.qt_framework_bins.append(resolved)
            self.deployed_qt_names.add(fw.name[: -len(".framework")])

        self.enqueue(dest_bin)

    def process_binary(self, binary: Path, executable: Path) -> None:
        key = str(binary.resolve())
        if key in self.processed:
            return
        self.processed.add(key)
        log(self.verbose, 2, f"Processing {binary}")

        # Ensure Frameworks is searchable
        add_rpath(binary, "@executable_path/../Frameworks", self.verbose)

        for dep in otool_deps(binary):
            self.deploy_dependency_of(binary, dep, executable)

        if not self.no_strip:
            strip_binary(binary, self.verbose)

    def fix_absolute_rpaths(self, binary: Path) -> None:
        """Remove absolute Homebrew/build rpaths that should not ship."""
        for rp in list(otool_rpaths(binary)):
            if rp.startswith("/opt/") or rp.startswith("/usr/local/") or rp.startswith("/Users/"):
                delete_rpath(binary, rp, self.verbose)

    def run(self) -> None:
        exe = self.main_executable()
        log(self.verbose, 2, f"Main executable: {exe}")

        extras = [Path(p).resolve() for p in self.args.executable]
        for e in extras:
            if not e.is_file():
                raise DeployError(f"Additional executable not found: {e}")

        self.frameworks.mkdir(parents=True, exist_ok=True)
        self.enqueue(exe)
        for e in extras:
            self.enqueue(e)

        while self.queue:
            binary = self.queue.popleft()
            # Additional executables still rewrite relative to the main app executable path.
            self.process_binary(binary, exe)

        # Plugins
        plugin_bins: list[Path] = []
        prefixes = find_qt_prefixes(self.qt_framework_bins)
        # Also discover prefixes from still-linked paths if any Qt was only @rpath-resolved
        if not prefixes:
            for src in list(self.deployed.keys()):
                if "Qt" in src and ".framework" in src:
                    prefixes = find_qt_prefixes([Path(src)])
                    if prefixes:
                        break

        if not self.args.no_plugins and self.deployed_qt_names:
            plugin_roots = plugins_dirs_for_prefixes(prefixes)
            # Homebrew: scan sibling kegs under /opt/homebrew/opt for qt* plugins
            opt = Path("/opt/homebrew/opt")
            if opt.is_dir():
                for child in opt.iterdir():
                    if child.name.startswith("qt"):
                        plugin_roots.extend(plugins_dirs_for_prefixes([child]))
            seen_pr: set[Path] = set()
            uniq_roots: list[Path] = []
            for pr in plugin_roots:
                if pr not in seen_pr:
                    seen_pr.add(pr)
                    uniq_roots.append(pr)
            plugin_roots = uniq_roots
            plugin_bins = deploy_plugins(
                plugin_roots,
                self.plugins,
                self.deployed_qt_names,
                appstore=self.args.appstore_compliant,
                use_debug=self.args.use_debug_libs,
                always_overwrite=self.args.always_overwrite,
                verbose=self.verbose,
            )
            for pb in plugin_bins:
                self.enqueue(pb)
            while self.queue:
                binary = self.queue.popleft()
                self.process_binary(binary, exe)

        # QML — only when -qmldir is passed (same as macdeployqt)
        has_qml = False
        qmldirs = [Path(p).resolve() for p in self.args.qmldir]
        qmlimports = [Path(p).resolve() for p in self.args.qmlimport]
        if qmldirs:
            for extra in (Path("/opt/homebrew/opt/qtdeclarative"), Path("/opt/homebrew/opt/qtbase")):
                if extra.is_dir() and extra not in prefixes:
                    prefixes.append(extra)
            qml_dest = self.resources / "qml"
            qml_bins = deploy_qml(
                qmldirs,
                qmlimports,
                qml_dest,
                prefixes,
                always_overwrite=self.args.always_overwrite,
                verbose=self.verbose,
            )
            for qb in qml_bins:
                if is_macho(qb):
                    self.enqueue(qb)
            while self.queue:
                binary = self.queue.popleft()
                self.process_binary(binary, exe)
            has_qml = True

        write_qt_conf(self.resources, has_qml=has_qml, verbose=self.verbose)

        # Clean absolute rpaths on all processed binaries
        for key in list(self.processed):
            self.fix_absolute_rpaths(Path(key))

        # Codesign (default: nil/ad-hoc "-") — must run after all install_name_tool edits.
        if self.args.sign_for_notarization:
            codesign_bundle(
                self.app,
                self.args.sign_for_notarization,
                hardened=True,
                timestamp=True,
                verbose=self.verbose,
            )
        elif not self.args.no_codesign:
            identity = self.args.codesign if self.args.codesign else "-"
            codesign_bundle(
                self.app,
                identity,
                hardened=self.args.hardened_runtime,
                timestamp=self.args.timestamp,
                verbose=self.verbose,
            )

        if self.args.dmg:
            create_dmg(self.app, self.args.fs, self.verbose)

        log(self.verbose, 2, f"Done: {self.app}")
        log(self.verbose, 2, f"Deployed {len(self.deployed)} libraries/frameworks")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        Deployer(args).run()
    except DeployError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:
        print(f"ERROR: command failed: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
