"""Compile selected shadPS4/GoldHEN XML patches for GoW3R.

Patch addresses are PS4 virtual addresses (eboot base 0x400000). Only literal
writes are accepted; pattern patches are rejected deliberately.
"""

import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
EBOOT_BASE = 0x400000
GOW3R_TITLE_IDS = {"CUSA01623", "CUSA01715"}
DEFAULT_XML = ROOT / "patches" / "God_of_War_III_Remastered.xml"
TEXTURE_FIX = "Bug Fix - Texture Corruption Fix"
FPS_120 = "Frame Rate Patch - 120 FPS"
NO_VIDEO_PATCH = "Sem patch de textura/resolução"
# DMEM in the fork is additional to the retail 5056 MiB pool. This runtime uses
# a total budget, so translate the XML requirements before launching.
VIDEO_PROFILES = {
    TEXTURE_FIX: (1920, 1080, 2048),
    "Resolution Patch - 480p": (854, 480, 2048),
    "Resolution Patch - 720p": (1280, 720, 2048),
    "Resolution Patch - 1440p": (2560, 1440, 3072),
    "Resolution Patch - 1800p": (3200, 1800, 4608),
    "Resolution Patch - 4K": (3840, 2160, 6144),
}


def catalog(xml=DEFAULT_XML, app_version="01.02"):
    """Return the version-compatible eboot patches, preserving XML metadata."""
    root = ET.parse(xml).getroot()
    title_ids = {item.text.strip() for item in root.iter("ID") if item.text}
    if not title_ids & GOW3R_TITLE_IDS:
        raise ValueError("patch XML does not target God of War III Remastered")
    return {
        item.get("Name"): dict(item.attrib)
        for item in root.iter("Metadata")
        if item.get("AppVer") == app_version
        and item.get("AppElf", "eboot.bin") == "eboot.bin"
        and item.get("Name")
    }


def validate_selection(names):
    profiles = [name for name in names if name in VIDEO_PROFILES]
    if len(profiles) > 1:
        raise ValueError("choose only one texture/resolution patch: " + "; ".join(profiles))


def runtime_options(names, dmem_mb=7104):
    """Compute the memory, output size and timing required by selected patches."""
    validate_selection(names)
    budget = int(dmem_mb)
    options = {}
    for name in names:
        if name in VIDEO_PROFILES:
            width, height, additional = VIDEO_PROFILES[name]
            budget = max(budget, 5056 + additional)
            if name != TEXTURE_FIX:
                options.update(GOW3R_INTERNAL_WIDTH=str(width),
                               GOW3R_INTERNAL_HEIGHT=str(height))
        if name == FPS_120:
            options.update(GOW3R_VBLANK_HZ="120", GOW3R_FPS_LIMIT="120")
    if not 5056 <= budget <= 16384:
        raise ValueError("total direct memory budget must be between 5056 and 16384 MiB")
    options["GOW3R_DMEM_MB"] = str(budget)
    return options


def eboot_segments(elf):
    phoff, = struct.unpack_from("<Q", elf, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x36)
    segments = []
    for index in range(phnum):
        kind, _, _, vaddr, _, _, memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", elf, phoff + index * phentsize
        )
        if kind == 1:
            segments.append((vaddr, vaddr + memsz))
    return segments


def encode(line):
    kind, value = line.get("Type"), line.get("Value")
    if kind == "bytes":
        return bytes.fromhex(value.replace(" ", ""))
    if kind in ("bytes16", "bytes32", "bytes64"):
        return int(value, 0).to_bytes(int(kind[5:]) // 8, "little")
    if kind == "float32":
        return struct.pack("<f", float(value))
    if kind == "float64":
        return struct.pack("<d", float(value))
    if kind == "utf8":
        return value.encode() + b"\0"
    if kind == "utf16":
        return value.encode("utf-16-le") + b"\0\0"
    raise ValueError(f"unsupported patch type {kind!r}")


def compile_patches(xml, names, app_version, segments):
    validate_selection(names)
    found = {}
    for metadata in ET.parse(xml).getroot().iter("Metadata"):
        if (
            metadata.get("Name") in names
            and metadata.get("AppVer") == app_version
            and metadata.get("AppElf", "eboot.bin") == "eboot.bin"
        ):
            found[metadata.get("Name")] = metadata
    missing = [name for name in names if name not in found]
    if missing:
        raise ValueError(f"patches not found for app version {app_version}: {missing}")

    writes = []
    for name in names:
        for line in found[name].iter("Line"):
            offset = int(line.get("Address"), 0) - EBOOT_BASE
            data = encode(line)
            if not any(start <= offset and offset + len(data) <= end for start, end in segments):
                raise ValueError(f"{name}: address {line.get('Address')} is outside the eboot")
            writes.append((offset, data))
    return writes


def external_patches(directory, app_version="01.02", exclude=DEFAULT_XML):
    """Return compatible third-party patches found in ``directory``."""
    found = []
    directory = Path(directory)
    for path in sorted(directory.glob("*.xml")) if directory.is_dir() else []:
        if path.resolve() == Path(exclude).resolve():
            continue
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f"Patches: {path.name}: {error}", file=sys.stderr)
            continue
        title_ids = {item.text.strip() for item in root.iter("ID") if item.text}
        if title_ids and not title_ids & GOW3R_TITLE_IDS:
            continue
        for metadata in root.iter("Metadata"):
            if (
                metadata.get("AppVer") == app_version
                and metadata.get("AppElf", "eboot.bin") == "eboot.bin"
            ):
                found.append((f"{path.name}/{metadata.get('Name')}", path, metadata))
    return found


def external_selection(found, config):
    settings = {}
    if config and Path(config).is_file():
        settings = json.loads(Path(config).read_text(encoding="utf-8"))
    enabled = set(settings.get("enabled", []))
    disabled = set(settings.get("disabled", []))
    return [
        item
        for item in found
        if item[0] in enabled
        or (item[0] not in disabled and item[2].get("isEnabled", "false").lower() == "true")
    ]


def compile_external(selected, segments):
    writes = []
    for key, _, metadata in selected:
        try:
            current = []
            for line in metadata.iter("Line"):
                offset = int(line.get("Address") or "", 0) - EBOOT_BASE
                data = encode(line)
                if not any(start <= offset and offset + len(data) <= end for start, end in segments):
                    raise ValueError(f"address {line.get('Address')} is outside the eboot")
                current.append((offset, data))
        except ValueError as error:
            print(f"Patches: skipped {key}: {error}", file=sys.stderr)
            continue
        writes += current
        print(f"Patches: external {key} ({len(current)} writes)")
    return writes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xml", type=Path, default=DEFAULT_XML)
    parser.add_argument("--app-version", default="01.02")
    parser.add_argument("--extra", default="", help="patch names separated by ';'")
    parser.add_argument("--patches-dir", type=Path, help="third-party patch XML directory")
    parser.add_argument("--patches-config", type=Path, help="JSON enabled/disabled selection")
    parser.add_argument("--out", type=Path, default=ROOT / "out")
    parser.add_argument("--runtime-env", type=Path,
                        help="write a CMD file containing numeric runtime requirements")
    args = parser.parse_args()

    names = []
    for name in (item.strip() for item in args.extra.split(";")):
        if name and name.lower() != "none" and name not in names:
            names.append(name)

    try:
        segments = eboot_segments((args.out / "eboot.elf").read_bytes())
        writes = compile_patches(args.xml, names, args.app_version, segments)
        options = runtime_options(names, os.environ.get("GOW3R_DMEM_MB", "7104"))
    except (ValueError, OSError, ET.ParseError, struct.error) as error:
        parser.error(str(error))
    if args.patches_dir:
        found = external_patches(args.patches_dir, args.app_version, args.xml)
        writes += compile_external(external_selection(found, args.patches_config), segments)

    # BBPATCH2 is retained as the established loader ABI magic, not as a game identifier.
    blob = struct.pack("<8sQQ", b"BBPATCH2", EBOOT_BASE, len(writes))
    for offset, data in writes:
        blob += struct.pack("<QQ", offset, len(data)) + data
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "patches.bin").write_bytes(blob)
    if args.runtime_env:
        # All names are fixed above and all values are numeric. Never put XML
        # names, notes or arbitrary environment text into executable CMD syntax.
        lines = ["@echo off"] + [f'set "{key}={int(value)}"' for key, value in options.items()]
        args.runtime_env.write_text("\n".join(lines) + "\n", encoding="ascii")
    print("Patch runtime requirements: " + ", ".join(f"{key}={value}" for key, value in options.items()))
    print(f"GoW3R patches: {', '.join(names) if names else 'none'}; {len(writes)} writes")


if __name__ == "__main__":
    main()
