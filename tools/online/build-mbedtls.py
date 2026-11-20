#!/usr/bin/env python3
"""Fetch the pinned Mbed TLS source and build the TLS library of the net/ HTTPS client into a private prefix.

    python3 tools/online/build-mbedtls.py [--target vita|host32|host64|both|all] [--work DIR] [--fetch-only] [--rebuild]

Galaxy 3.0 multiplayer: vendored from Strikers Charged 1.3 tools/build-mbedtls.py (+ the two checkout helpers of its
tools/build-deps.py, inlined below). Pin: gpsp/vita/deps/lock.json, "network_dependencies".
Recipe: gpsp/vita/deps/Makefile.mbedtls (client-only profile gpsp/vita/deps/mbedtls/np_user_config.h). Output, under DIR (default
build/dependencies; pass e.g. --work /tls so the Vita prefix is /tls/mbedtls/vita):
    sources/Mbed-TLS_mbedtls/            the pinned checkout (never reset or modified)
    mbedtls/<target>/include, lib        the private prefix; pass it to the Vita build as
                                         tools/vgx_build/build.py --mbedtls <that path> (or SMG_MBEDTLS=<that path>)
    mbedtls/<target>/build-inputs.json   immutable prefix build identity (different inputs require a NEW work root)
    mbedtls/<target>/build-receipt.json  source, tools, flags, .d/object/input and library hashes
Nothing is installed into VitaSDK. host32 / host64 are the x86 builds host tests may link.

SPDX-License-Identifier: MIT
Copyright (c) 2026 CBOC-Vita contributors. See licenses/CBOC-tools-MIT.txt."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "gpsp/vita/deps/lock.json"


def git(directory, *args):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    return subprocess.check_output(["git", "-c", "core.hooksPath=" + os.devnull, *args],
                                   cwd=directory, env=env, text=True).strip()


def load_lock(path, key="dependencies"):
    rows = json.loads(path.read_text())[key]
    seen = set()
    if not rows:
        raise ValueError("Dependency lock is empty")
    for row in rows:
        name, url, commit = row["name"], row["url"], row["commit"]
        if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", name) or name.lower() in seen
                or not re.fullmatch(r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\.git", url)
                or not re.fullmatch(r"[0-9a-f]{40}", commit)):
            raise ValueError("Invalid or unpinned dependency lock entry")
        seen.add(name.lower())
    return rows


def ensure_checkout(destination, url, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("A full pinned revision is required")
    if destination.is_symlink():
        raise ValueError("Dependency checkout must not be a symlink")
    if destination.exists():
        if not (destination / ".git").is_dir() or (destination / ".git").is_symlink():
            raise ValueError("Existing dependency directory is not a managed Git checkout")
        if git(destination, "remote", "get-url", "origin") != url:
            raise ValueError("Dependency origin differs; existing files were left alone")
        if git(destination, "rev-parse", "HEAD") != commit:
            raise ValueError("Dependency revision differs; existing files were left alone")
        if git(destination, "status", "--porcelain", "--untracked-files=all"):
            raise ValueError("Dependency has local changes; existing files were left alone")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Only promote a verified checkout; failed downloads remain safe to retry.
    with tempfile.TemporaryDirectory(prefix="." + destination.name + "-fetch-", dir=destination.parent) as temp:
        checkout = Path(temp) / "checkout"
        checkout.mkdir()
        git(checkout, "init", "--quiet")
        git(checkout, "remote", "add", "origin", url)
        git(checkout, "fetch", "--quiet", "--depth=1", "origin", commit)
        git(checkout, "checkout", "--quiet", "--detach", commit)
        if git(checkout, "rev-parse", "HEAD") != commit:
            raise ValueError("Fetched dependency revision did not match")
        if destination.exists() or destination.is_symlink():
            raise ValueError("Dependency destination appeared during download; existing files were left alone")
        checkout.rename(destination)


class _Deps:
    load_lock = staticmethod(load_lock)
    ensure_checkout = staticmethod(ensure_checkout)


def load_build_deps():
    return _Deps


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def reject_symlinks(path):
    for part in (path, *path.parents):
        if part.is_symlink():
            raise ValueError("Build inputs and outputs must not contain symlinks")


def require_no_patches(row):
    """The pin is stock upstream: a lock entry that names a patch is refused, so nothing is silently skipped."""
    if row.get("patches"):
        raise ValueError("The Mbed TLS pin is built unpatched; remove the patch descriptors from the lock")


def write_json_atomic(path, value):
    data = json.dumps(value, indent=2, sort_keys=True) + "\n"
    reject_symlinks(path)
    if path.exists() and path.read_text() == data:
        return
    with tempfile.NamedTemporaryFile(mode="w", prefix="." + path.name + "-", dir=path.parent,
                                     delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def prepare_prefix(prefix, identity):
    reject_symlinks(prefix)
    marker = prefix / "build-inputs.json"
    receipt = prefix / "build-receipt.json"
    if prefix.exists() and any(prefix.iterdir()):
        reject_symlinks(marker)
        reject_symlinks(receipt)
        if not marker.is_file() or json.loads(marker.read_text()) != identity:
            raise ValueError("Existing TLS prefix provenance differs; choose a new --work directory")
        if receipt.exists():
            recorded = json.loads(receipt.read_text())
            if any(recorded.get(key) != value for key, value in identity.items()):
                raise ValueError("Existing TLS prefix receipt provenance differs; choose a new --work directory")
            verify_recorded_prefix(prefix, recorded)
        for entry in prefix.rglob("*"):
            reject_symlinks(entry)
    prefix.mkdir(parents=True, exist_ok=True)
    write_json_atomic(marker, identity)
    return receipt.is_file()


def verify_recorded_prefix(prefix, receipt):
    def verify(relative, expected):
        path = Path(relative)
        if (path.is_absolute() or ".." in path.parts or not isinstance(expected, str) or
                not re.fullmatch(r"[0-9a-f]{64}", expected)):
            raise ValueError("Invalid existing TLS prefix receipt provenance")
        selected = prefix / path
        reject_symlinks(selected)
        if not selected.is_file() or sha256(selected) != expected:
            raise ValueError("Existing TLS prefix artifact/input hash changed; choose a new --work directory")

    for field, directory, extension in (("libraries", "lib", ".a"), ("objects", "obj", ".o"),
                                        ("depfiles", "obj", ".d")):
        recorded = receipt.get(field)
        if (not isinstance(recorded, dict) or not recorded or
                set(recorded) != {p.name for p in (prefix / directory).glob("*" + extension)}):
            raise ValueError("Existing TLS prefix artifact inventory provenance differs")
        if field == "libraries" and set(recorded) != {"libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a"}:
            raise ValueError("Existing TLS prefix library provenance differs")
        for name, expected in recorded.items():
            if Path(name).name != name:
                raise ValueError("Invalid existing TLS prefix artifact path")
            verify(directory + "/" + name, expected)
    inputs = receipt.get("compile_inputs")
    if not isinstance(inputs, dict) or not inputs:
        raise ValueError("Existing TLS prefix compile input provenance is missing")
    for name, expected in inputs.items():
        label, separator, relative = name.partition("/")
        if not separator or label not in ("source", "prefix"):
            raise ValueError("Invalid existing TLS compile input provenance")
        if label == "prefix":
            verify(relative, expected)


def collect_compile_inputs(source, prefix):
    objdir = prefix / "obj"
    expected = {path.stem for path in (source / "library").glob("*.c")}
    depfiles = {path.stem: path for path in objdir.glob("*.d")}
    objects = {path.stem: path for path in objdir.glob("*.o")}
    if set(depfiles) != expected or set(objects) != expected:
        raise ValueError("Missing or unexpected TLS object/depfile inputs")
    inputs = {}
    for stem, depfile in sorted(depfiles.items()):
        reject_symlinks(depfile)
        reject_symlinks(objects[stem])
        target, separator, prerequisites = depfile.read_text().replace("\\\n", " ").partition(":")
        if not separator or Path(target.strip()) != objects[stem]:
            raise ValueError("Unexpected TLS depfile target")
        dependencies = shlex.split(prerequisites)
        expected_source = source / ("library/" + stem + ".c")
        if str(expected_source) not in dependencies:
            raise ValueError("TLS object does not name its expected source input")
        for name in dependencies:
            path = Path(name)
            reject_symlinks(path)
            for label, base in (("source", source), ("prefix", prefix)):
                try:
                    relative = path.relative_to(base)
                except ValueError:
                    continue
                if ".." in relative.parts or not path.is_file():
                    raise ValueError("Invalid TLS compile input")
                inputs[label + "/" + relative.as_posix()] = sha256(path)
                break
            else:
                raise ValueError("TLS compile input is outside verified roots")
    return {"compile_inputs": dict(sorted(inputs.items())),
            "depfiles": {path.name: sha256(path) for _, path in sorted(depfiles.items())},
            "objects": {path.name: sha256(path) for _, path in sorted(objects.items())}}


def tool_identity(command, env):
    executable = shutil.which(command, path=env.get("PATH"))
    if executable is None:
        raise ValueError("TLS build tool not found: " + command)
    version = subprocess.check_output([command, "--version"], env=env, text=True).splitlines()[0]
    return {"command": command, "version": version,
            "executable_sha256": sha256(Path(executable).resolve())}


def build_identity(source, prefix, row, target, sdk):
    env = dict(os.environ, VITASDK=str(sdk), LC_ALL="C")
    for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
        env.pop(key, None)
    command = ["make", "-f", str(ROOT / row["recipe"]), "-j2", "SRC=" + str(source),
               "OUT=" + str(prefix), "TARGET=" + target]
    config = subprocess.check_output(command + ["--silent", "print-build-config"], cwd=ROOT, env=env)
    parts = config.decode("utf-8").split("\0")
    if len(parts) != 4 or parts[-1] or not all(parts[:3]):
        raise ValueError("Unexpected TLS recipe build configuration")
    cc, ar, flags, _ = parts
    for original, normalized in ((source, "@SOURCE@"),
                                 (prefix, "@PREFIX@"), (ROOT, "@PROJECT@")):
        flags = flags.replace(str(original), normalized)
    identity = {"schema_version": 3, "dependency": row, "target": target,
                "profile_sha256": sha256(ROOT / row["profile"]),
                "recipe_sha256": sha256(ROOT / row["recipe"]),
                "builder_sha256": sha256(Path(__file__)),
                "compiler": tool_identity(cc, env), "archiver": tool_identity(ar, env),
                "flags": shlex.split(flags)}
    return identity, command, env


def build_target(source, prefix, row, target, sdk, rebuild):
    identity, command, env = build_identity(source, prefix, row, target, sdk)
    complete = prepare_prefix(prefix, identity)
    if rebuild or not complete:
        command.insert(1, "-B")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    load_build_deps().ensure_checkout(source, row["url"], row["commit"])
    compiled = collect_compile_inputs(source, prefix)
    libraries = {}
    for name in ("libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a"):
        library = prefix / "lib" / name
        reject_symlinks(library)
        libraries[name] = sha256(library)
    receipt = {**identity, **compiled, "libraries": libraries}
    write_json_atomic(prefix / "build-receipt.json", receipt)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--target", choices=["vita", "host32", "host64", "both", "all"], default="vita")
    parser.add_argument("--work", type=Path, default=ROOT / "build/dependencies")
    parser.add_argument("--fetch-only", action="store_true", help="Prepare the source without compiling")
    parser.add_argument("--rebuild", action="store_true", help="Recompile matching prefix inputs; changed provenance requires a new --work")
    args = parser.parse_args()
    deps = load_build_deps()
    try:
        rows = deps.load_lock(LOCK, "network_dependencies")
        row = next(r for r in rows if r["name"] == "Mbed-TLS_mbedtls")
        reject_symlinks(args.work.absolute())
        work = args.work.resolve()
        for folder in (work, work / "sources", work / "mbedtls"):
            if folder.is_symlink():
                raise ValueError("Build folders must not be symlinks")
        source = work / "sources" / row["name"]
        print("Checking " + row["name"], flush=True)
        deps.ensure_checkout(source, row["url"], row["commit"])
        if args.fetch_only:
            print("Pinned source is ready in " + str(source))
            return 0
        if platform.system() != "Linux" or any(c.isspace() for c in str(ROOT) + str(work)):
            raise ValueError("Build on Linux in paths without spaces")
        targets = {"both": ["vita", "host32"], "all": ["vita", "host32", "host64"]}.get(args.target, [args.target])
        sdk = Path(os.environ.get("VITASDK", "/opt/vitasdk"))
        if "vita" in targets and not (sdk / "bin/arm-vita-eabi-gcc").is_file():
            raise ValueError("Set VITASDK to your Linux VitaSDK directory")
        require_no_patches(row)
        for target in targets:
            prefix = work / "mbedtls" / target
            build_target(source, prefix, row, target, sdk, args.rebuild)
            print("READY: " + str(prefix) + "  (build.py --mbedtls " + str(prefix) + "  or  SMG_MBEDTLS=" + str(prefix) + ")")
        return 0
    except (OSError, ValueError, KeyError, StopIteration, subprocess.CalledProcessError) as error:
        print("STOP: " + str(error), file=sys.stderr)
        print("No existing dependency checkout was reset or removed.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
