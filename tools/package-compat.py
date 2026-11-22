"""Build the JM1 bench profiles and native rollback image into one verified local ZIP."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

FIRMWARE = Path(__file__).resolve().parents[1] / "firmware"
BUILD = FIRMWARE / "build"
APP_BASE = 0x08000000
NV_BASE = 0x0803E800


def flash_end(elf):
    """Check the physical addresses of all file-backed ELF32 LOAD segments."""
    if elf[:7] != b"\x7fELF\x01\x01\x01" or struct.unpack_from("<H", elf, 18)[0] != 40:
        raise ValueError("expected a little-endian ARM ELF32 image")
    table = struct.unpack_from("<I", elf, 28)[0]
    size, count = struct.unpack_from("<HH", elf, 42)
    ranges = []
    for i in range(count):
        kind, offset, _, address, filesz = struct.unpack_from("<5I", elf, table + i * size)
        if kind == 1 and filesz:
            if offset + filesz > len(elf) or not APP_BASE <= address < address + filesz <= NV_BASE:
                raise ValueError(f"LOAD outside application flash or ELF at {address:#010x}")
            ranges.append((address, address + filesz))
    if not ranges or min(start for start, _ in ranges) != APP_BASE:
        raise ValueError("missing application image at flash base")
    return max(end for _, end in ranges)


def main():
    presets = json.loads((FIRMWARE / "CMakePresets.json").read_text())["configurePresets"]
    release = [p["name"] for p in presets if "CAN_PROTOCOL" in p.get("cacheVariables", {})]
    records = []
    BUILD.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=BUILD) as work:
        archive = Path(work) / "JM1-CAN-bench.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            for preset in release:
                subprocess.run(["cmake", "--preset", preset], cwd=FIRMWARE, check=True)
                subprocess.run(["cmake", "--build", "--preset", preset], cwd=FIRMWARE, check=True)
                model = "v1" if preset == "gd32f303" else preset.removeprefix("gd32f303-")
                image = (BUILD / preset / "YHorizon-JM.elf").read_bytes()
                end = flash_end(image)
                name = f"JM1-{model}.elf"
                bundle.writestr(name, image)
                records.append(dict(model=model, filename=name, sha256=hashlib.sha256(image).hexdigest(),
                                    flash_end_exclusive=hex(end)))
            manifest = dict(rollback="JM1-v1.elf", calibration_flash_start=hex(NV_BASE),
                            verification="build and flash layout only", images=records)
            bundle.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        destination = BUILD / archive.name
        archive.replace(destination)
        print(destination)


if __name__ == "__main__":
    main()
