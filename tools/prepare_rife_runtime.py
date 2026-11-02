"""Populate one portable RIFE runtime. Standard library only; no global pip.

Usage: python prepare_rife_runtime.py --root D:/.../runtime/rife
Windows users without Python can run Setup-RifeRuntime.ps1 first.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import stat
import tarfile
import urllib.request
import zipfile


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def extract(archive, destination, wheel=False):
    destination = destination.resolve()
    with zipfile.ZipFile(archive) as files:
        for member in files.infolist():
            if member.is_dir():
                continue
            if stat.S_ISLNK(member.external_attr >> 16):
                raise RuntimeError("Archive contains an unsupported symbolic link")
            parts = Path(member.filename).parts
            if wheel and len(parts) > 2 and parts[0].endswith(".data"):
                if parts[1] not in ("purelib", "platlib"):
                    continue
                parts = parts[2:]
            path = (destination / Path(*parts)).resolve()
            if not path.is_relative_to(destination):
                raise RuntimeError("Archive member escapes portable runtime")
            path.parent.mkdir(parents=True, exist_ok=True)
            with files.open(member) as source, path.open("wb") as target:
                shutil.copyfileobj(source, target)


def extract_tensorrt_wrapper(archive, destination):
    """The official tiny sdist supplies the pure tensorrt import wrapper only."""
    destination = destination.resolve()
    with tarfile.open(archive, "r:gz") as files:
        for member in files.getmembers():
            if member.issym() or member.islnk():
                raise RuntimeError("Archive contains an unsupported link")
            if not member.isfile():
                continue
            parts = Path(member.name).parts
            if len(parts) < 3 or parts[1] != "tensorrt":
                continue
            path = (destination / Path(*parts[1:])).resolve()
            if not path.is_relative_to(destination):
                raise RuntimeError("Archive member escapes portable backend")
            path.parent.mkdir(parents=True, exist_ok=True)
            with files.extractfile(member) as source, path.open("wb") as target:
                shutil.copyfileobj(source, target)


def download(asset, root):
    path = root / asset["path"]
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file():
        partial = path.with_suffix(path.suffix + ".partial")
        print("Downloading", path.name, flush=True)
        with urllib.request.urlopen(asset["url"], timeout=120) as response, partial.open("wb") as output:
            shutil.copyfileobj(response, output, 4 * 1024 * 1024)
        if partial.stat().st_size != asset["bytes"] or digest(partial) != asset["sha256"]:
            raise RuntimeError(f"Checksum mismatch: {partial}")
        partial.replace(path)
    if path.stat().st_size != asset["bytes"] or digest(path) != asset["sha256"]:
        raise RuntimeError(f"Checksum mismatch: {path}")
    return path


def prepare_tensorrt(lock, root):
    backend = lock["tensorrt_backend"]
    destination = root / "backends" / "tensorrt"
    for asset in backend["assets"]:
        archive = download(asset, root)
        if asset.get("format") == "tensorrt-wrapper-sdist":
            extract_tensorrt_wrapper(archive, destination)
        elif archive.suffix == ".whl":
            extract(archive, destination, wheel=True)
        else:
            raise RuntimeError("Unsupported TensorRT backend asset")
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "ofxr-backend.json").write_text(json.dumps(backend, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--crt-source", type=Path)
    parser.add_argument("--skip-tensorrt", action="store_true", help="Keep the portable PyTorch fallback without downloading TensorRT")
    args = parser.parse_args()
    root = args.root.resolve()
    source = Path(__file__).resolve().parents[1] / "python/rife"
    lock = json.loads((source / "runtime-lock.json").read_text())
    root.mkdir(parents=True, exist_ok=True)
    crt_source=args.crt_source or root/"downloads/msvc-crt"
    for file in lock["msvc_crt"]["files"]:
        path=crt_source/file["name"]
        if not path.is_file() or digest(path)!=file["sha256"]:
            raise RuntimeError("Provide the pinned x64 Microsoft.VC143.CRT redistributable folder with --crt-source, or copy the complete prepared shared runtime")
    for asset in lock["assets"]:
        path = download(asset, root)
        if path.suffix == ".whl":
            extract(path, root / "python/Lib/site-packages", wheel=True)
        elif path.suffix == ".zip":
            # The bootstrap may already be executing this embedded interpreter.
            if not (root / "python/python.exe").exists():
                extract(path, root / "python")
    if not args.skip_tensorrt and "tensorrt_backend" in lock:
        try:
            prepare_tensorrt(lock, root)
            print("Optional TensorRT backend ready", flush=True)
        except Exception as error:
            # Optional optimization must not make a valid Torch runtime
            # unusable. Retry this installer or use --skip-tensorrt offline.
            print("TensorRT setup unavailable; PyTorch fallback retained:", error, flush=True)
    (root / "python/python312._pth").write_text("python312.zip\n.\nLib/site-packages\n../app\nimport site\n")
    for file in lock["msvc_crt"]["files"]:
        path=crt_source/file["name"]
        target=root/"python"/file["name"]
        if not target.is_file() or digest(target)!=file["sha256"]:
            shutil.copy2(path,target)
        cached=root/"downloads/msvc-crt"/file["name"]
        cached.parent.mkdir(parents=True,exist_ok=True)
        if path.resolve()!=cached.resolve():shutil.copy2(path,cached)
    for path in source.rglob("*"):
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        target = root / "app" / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    (root / "runtime.json").write_text(json.dumps(lock, indent=2) + "\n")
    print("Portable RIFE ready:", root, flush=True)


if __name__ == "__main__":
    main()
