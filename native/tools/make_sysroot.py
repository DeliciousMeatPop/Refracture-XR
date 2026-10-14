"""Builds the Android root filesystem refract_native runs games on from Google's stock ARM64 Android 14 system image
(sdkmanager "system-images;android-34;default;arm64-v8a"), so no Quest firmware dump is needed.

Copies /system/lib64, the linker, framework-res.apk, framework.jar (native/java/build.sh converts it), build.prop and the APEX payloads games use (runtime = bionic,
art, i18n, tzdata, statsd, conscrypt) into <OUT>, resolving symlinks by copying (NTFS has none).

usage: python make_sysroot.py [OUT] [--image system.img]   (OUT default: <repo>/build-native/sysroot)
Needs: pip install ext4
"""
import argparse
import os
import shutil
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

import ext4

REPO = Path(__file__).resolve().parents[2]
APEXES = ("runtime", "art", "i18n", "tzdata", "os.statsd", "conscrypt")
SKIP_DIRS = {"hwasan"}


def default_image() -> Path:
    for sdk in (os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
                Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk", Path.home() / "Android" / "Sdk"):
        if sdk:
            image = Path(sdk) / "system-images" / "android-34" / "default" / "arm64-v8a" / "system.img"
            if image.is_file():
                return image
    sys.exit('Android 14 ARM64 image not found: sdkmanager "system-images;android-34;default;arm64-v8a"')


def logical_partitions(f) -> dict:
    """name -> (byte offset, byte size) of the partitions in the image's GPT 'super' partition (Android LP metadata)."""
    f.seek(512)
    header = f.read(92)
    if header[:8] != b"EFI PART":
        sys.exit("system.img is not a GPT disk image")
    entries_lba, count, size = struct.unpack_from("<QII", header, 72)
    f.seek(entries_lba * 512)
    super_start = None
    for _ in range(count):
        entry = f.read(size)
        if entry[56:].decode("utf-16le").rstrip("\0") == "super":
            super_start = struct.unpack_from("<Q", entry, 32)[0] * 512
    if super_start is None:
        sys.exit("no super partition in system.img")
    f.seek(super_start + 12288)  # after the reserved sector, both geometry copies
    meta = f.read(256)
    magic, _major, _minor, header_size = struct.unpack_from("<IHHI", meta, 0)
    if magic != 0x414C5030:
        sys.exit("bad super partition metadata")
    tables_size = struct.unpack_from("<I", meta, 44)[0]
    parts, extents = struct.unpack_from("<III", meta, 80), struct.unpack_from("<III", meta, 92)
    f.seek(super_start + 12288 + header_size)
    tables = f.read(tables_size)
    result = {}
    for i in range(parts[1]):
        entry = tables[parts[0] + i * parts[2]:][:parts[2]]
        name = entry[:36].split(b"\0")[0].decode()
        first, count = struct.unpack_from("<II", entry, 40)
        runs = [struct.unpack_from("<QIQ", tables[extents[0] + (first + j) * extents[2]:], 0) for j in range(count)]
        if len(runs) != 1 or runs[0][1] != 0:
            continue  # only single linear extents (what the SDK images use)
        result[name] = (super_start + runs[0][2] * 512, runs[0][0] * 512)
    return result


def resolve(volume, path: str):
    """The inode at path with symlinks followed (also in the middle of the path), or None."""
    queue, current, hops = [p for p in path.split("/") if p], [], 0
    while queue:
        part = queue.pop(0)
        if part == ".":
            continue
        if part == "..":
            if current:
                current.pop()
            continue
        current.append(part)
        try:
            node = volume.inode_at("/" + "/".join(current))
        except FileNotFoundError:
            return None
        if isinstance(node, ext4.inode.SymbolicLink):
            hops += 1
            if hops > 40:
                return None
            target = node.readlink().decode()
            current.pop()
            if target.startswith("/"):
                current = []
            queue = [p for p in target.split("/") if p] + queue
    return volume.inode_at("/" + "/".join(current)) if current else volume.root


def copy_file(node, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with node.open() as src, open(dest, "wb") as out:
        shutil.copyfileobj(src, out, 1 << 20)


def copy_tree(volume, src: str, dest: Path, only_suffix: tuple = ()) -> int:
    """Copies directory src (files and subdirectories; symlinks followed) to dest."""
    root = resolve(volume, src)
    if not isinstance(root, ext4.inode.Directory):
        return 0
    count = 0
    for entry, _ in root.opendir():
        name = entry.name_str
        if name in (".", "..") or name in SKIP_DIRS:
            continue
        node = resolve(volume, f"{src}/{name}")
        if isinstance(node, ext4.inode.Directory):
            count += copy_tree(volume, f"{src}/{name}", dest / name, only_suffix)
        elif isinstance(node, ext4.inode.File) and (not only_suffix or name.endswith(only_suffix)):
            copy_file(node, dest / name)
            count += 1
    return count


def apex_payload(volume, name: str, tmp: Path) -> Path:
    """Extracts the ext4 payload of /system/apex/com.android.<name>.apex (or the compressed .capex that wraps one)."""
    for suffix in (".apex", ".capex"):
        node = resolve(volume, f"/system/apex/com.android.{name}{suffix}")
        if node is None:
            continue
        package = tmp / f"{name}{suffix}"
        copy_file(node, package)
        with zipfile.ZipFile(package) as z:
            if "original_apex" in z.namelist():  # compressed APEX: the real one is inside
                z.extract("original_apex", tmp / name)
                package = tmp / name / "original_apex"
        with zipfile.ZipFile(package) as z:
            z.extract("apex_payload.img", tmp / f"{name}-payload")
        return tmp / f"{name}-payload" / "apex_payload.img"
    sys.exit(f"com.android.{name} APEX not found in the image")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", type=Path, default=REPO / "build-native" / "sysroot")
    ap.add_argument("--image", type=Path, default=None)
    args = ap.parse_args()
    image = args.image or default_image()
    out = args.out
    if out.exists():
        shutil.rmtree(out)
    with open(image, "rb") as f, tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        offset, _size = logical_partitions(f)["system"]
        system = ext4.Volume(f, offset=offset)
        n = copy_tree(system, "/system/lib64", out / "system" / "lib64", (".so",))
        print(f"system/lib64: {n} libraries")
        for item in ("/system/framework/framework-res.apk", "/system/framework/framework.jar", "/system/build.prop"):
            node = resolve(system, item)
            if node is not None:
                copy_file(node, out / item.lstrip("/"))
        for name in APEXES:
            with open(apex_payload(system, name, tmp), "rb") as payload:
                volume = ext4.Volume(payload)
                n = copy_tree(volume, "/lib64", out / "apex" / f"com.android.{name}" / "lib64")
                n += copy_tree(volume, "/bin", out / "apex" / f"com.android.{name}" / "bin")
                n += copy_tree(volume, "/etc", out / "apex" / f"com.android.{name}" / "etc")
                n += copy_tree(volume, "/javalib", out / "apex" / f"com.android.{name}" / "javalib")
                print(f"apex com.android.{name}: {n} files")
        linker = out / "apex" / "com.android.runtime" / "bin" / "linker64"
        (out / "system" / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy2(linker, out / "system" / "bin" / "linker64")
    for d in ("system/app", "vendor/lib64", "product/lib64", "system_ext/lib64", "odm"):
        (out / d).mkdir(parents=True, exist_ok=True)
    (out / "refract_symlinks.txt").write_text("", encoding="utf-8")
    print(f"sysroot: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
