"""Build the SDK's native library and stage it for both language APIs."""
from pathlib import Path
import json
import platform
import re
import shutil
import subprocess
import sys
import tempfile

sdk = Path(__file__).resolve().parent
manifest = sdk / "Cargo.toml"
cargo = ["cargo", "--manifest-path", str(manifest)]
metadata = json.loads(subprocess.check_output(
    [cargo[0], "metadata", *cargo[1:], "--format-version", "1", "--no-deps"]))
subprocess.run([cargo[0], "build", *cargo[1:], "--release", "--locked", "-p", "yfoc-motor-sdk"], check=True)
release = Path(metadata["target_directory"]) / "release"
libs = sdk / "lib"
libs.mkdir(exist_ok=True)
if sys.platform == "win32":
    shutil.copy2(release / "yfoc_motor_sdk.dll", libs)
    gnu = release / "libyfoc_motor_sdk.dll.a"
    msvc = release / "yfoc_motor_sdk.dll.lib"
    if gnu.exists():
        shutil.copy2(gnu, libs)
    if msvc.exists():
        shutil.copy2(msvc, libs / "yfoc_motor_sdk.lib")
    if platform.machine().lower() in {"amd64", "x86_64"} and (not gnu.exists() or not msvc.exists()):
        # Generate the other toolchain's import library from the public C symbols.
        tool = shutil.which("llvm-dlltool") or shutil.which("dlltool")
        if tool:
            exports = re.findall(r"\b(yfoc_[a-z_]+)\(", (sdk / "include/yfoc_motor.h").read_text())
            with tempfile.TemporaryDirectory() as tmp:
                definition = Path(tmp) / "motor.def"
                definition.write_text("LIBRARY yfoc_motor_sdk.dll\nEXPORTS\n" + "\n".join(exports) + "\n")
                output = "yfoc_motor_sdk.lib" if gnu.exists() else "libyfoc_motor_sdk.dll.a"
                subprocess.run([tool, "-m", "i386:x86-64", "-d", str(definition),
                                "-l", str(libs / output)], check=True)
else:
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    shutil.copy2(release / ("libyfoc_motor_sdk" + suffix), libs)
print(libs)
