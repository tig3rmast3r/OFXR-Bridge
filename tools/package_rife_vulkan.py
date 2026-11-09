"""Seal the optional Radeon payload; never overwrite an earlier payload."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

PACKAGE = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package(build, deps, output):
    if output.exists():
        raise RuntimeError("Previous Vulkan payload is preserved; select a new --output")
    binary = build / "Release/ofxr_rife_vulkan.exe"
    if not binary.is_file():
        raise RuntimeError("Build the Release Vulkan worker first")
    output.mkdir(parents=True)
    files = {"ofxr_rife_vulkan.exe": binary, "pnnx.exe": deps / "pnnx/pnnx-20261020-windows/pnnx.exe",
             "licenses/ncnn.txt": deps / "ncnn/LICENSE.txt", "licenses/glslang.txt": deps / "ncnn/glslang/LICENSE.txt",
             "dependencies.json": PACKAGE / "native/rife_vulkan/dependencies.json"}
    for name, source in files.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    # nlohmann's MIT licence is embedded in its reviewed single-header source.
    shutil.copy2(deps / "json-LICENSE.MIT", output / "licenses/json.txt")
    (output / "licenses/pnnx.txt").write_bytes((deps / "ncnn/LICENSE.txt").read_bytes())
    source_files = ("native/rife_vulkan/worker.cpp", "native/rife_vulkan/fused_warp.hpp", "native/rife_vulkan/CMakeLists.txt",
                    "tools/prepare_rife_vulkan_deps.py", "tools/vulkan_compile.py", "tools/install_rife_vulkan.py")
    manifest = {"schema": 1, "backend": "vulkan", "experimental": True,
                "files": {p.relative_to(output).as_posix(): digest(p) for p in output.rglob("*") if p.is_file()},
                "source": {name: digest(PACKAGE / name) for name in source_files}}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("Sealed Radeon payload:", output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("build", "deps", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    package(args.build.resolve(), args.deps.resolve(), args.output.resolve())
