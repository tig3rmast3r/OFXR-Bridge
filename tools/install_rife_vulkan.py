"""Transactional Radeon/Vulkan runtime; CPU Torch is an offline compiler only."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "tools"))
sys.path.insert(0, str(PACKAGE / "python/rife"))
import install_rife_runtime as base
from runtime_lease import RuntimeLease, RuntimeBusy


def json_read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def payload_files(package):
    source = package / "rife-vulkan"
    manifest = json_read(source / "manifest.json")
    if manifest.get("schema") != 1 or manifest.get("backend") != "vulkan":
        raise RuntimeError("Invalid Radeon payload manifest")
    for name, digest in manifest["files"].items():
        path = (source / name).resolve()
        if not path.is_relative_to(source.resolve()) or base.sha(path) != digest:
            raise RuntimeError("Radeon payload integrity mismatch: " + name)
    return source, manifest


def run_native(root, arguments, timeout=180):
    binary = root / "vulkan/ofxr_rife_vulkan.exe"
    result = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, text=True,
                            timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.stderr.strip():
        base.log(result.stderr.strip())
    value = json.loads(result.stdout)
    if result.returncode or not value.get("ok"):
        raise RuntimeError(value.get("error", "Radeon Vulkan validation failed"))
    return value


def health(root, package, update=False):
    result = dict(schema=1, status="error", message="", root=str(root), backend="AMD Radeon / Vulkan (experimental)",
                  installed_bytes=0, required_free_bytes=2_000_000_000, log_path="")
    if not (root / "vulkan/backend.json").is_file():
        return {**result, "status": "missing", "message": "The Radeon Vulkan runtime is not installed."}
    try:
        with RuntimeLease(root):
            receipt = json_read(root / "vulkan/backend.json")
            _, shipped = payload_files(package)
            if receipt["payload"] != shipped:
                return {**result, "status": "repair", "message": "The Radeon runtime belongs to another build. Reinstall RIFE to align its native worker and compiler."}
            for name, digest in receipt["files"].items():
                path = (root / name).resolve()
                if not path.is_relative_to(root.resolve()) or not path.is_file() or base.sha(path) != digest:
                    return {**result, "status": "repair", "message": "Radeon runtime integrity mismatch: " + name}
            if not update:
                check = subprocess.run([str(root / "python/python.exe"), "-I", "-B", "-c",
                    "import json,sys,torch;print(json.dumps({'python':sys.version_info[:3],'torch':torch.__version__,'cuda':torch.version.cuda}))"],
                    capture_output=True, text=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
                if check.returncode or json.loads(check.stdout) != {"python": [3, 12, 10], "torch": "2.9.1+cpu", "cuda": None}:
                    return {**result, "status": "repair", "message": "The pinned CPU-only Radeon compiler dependencies are damaged. Reinstall RIFE."}
                probe = run_native(root, ["--probe", "-1"])
                command = [str(root / "python/python.exe"), "-I", "-B", str(root / "tools/vulkan_compile.py"), "--root", str(root),
                           "--worker", str(root / "vulkan/ofxr_rife_vulkan.exe"), "--pnnx", str(root / "vulkan/pnnx.exe"),
                           "--cache", str(root / "vulkan/cache"), "--width", "64", "--height", "64", "--gpu", str(probe["gpu"])]
                prepared = subprocess.run(command, capture_output=True, text=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
                if prepared.returncode:
                    raise RuntimeError("Small Vulkan check plan could not be prepared: " + prepared.stderr[-1000:])
                run_native(root, ["--selftest", root, probe["gpu"]])
            return {**result, "status": "ready", "message": "Radeon Vulkan runtime verified." if update else "Vulkan RIFE4.4 and actual D3D12/Vulkan shared-buffer inference passed. Radeon hardware validation is tracked separately."}
    except RuntimeBusy as error:
        return {**result, "status": "busy", "message": str(error)}
    except (FileNotFoundError, json.JSONDecodeError, KeyError) as error:
        return {**result, "status": "repair", "message": "Damaged Radeon runtime metadata: " + str(error)}
    except Exception as error:
        return {**result, "status": "error", "message": str(error)}


def install(root, cache, crt_source, package=PACKAGE):
    root = root.absolute()
    parent = root.parent
    parent.mkdir(parents=True, exist_ok=True)
    if root.is_symlink() or (root.exists() and getattr(root.lstat(), "st_file_attributes", 0) & 0x400):
        raise RuntimeError("The Vulkan runtime destination must be a regular directory")
    payload, payload_manifest = payload_files(package)
    dependencies = json_read(payload / "dependencies.json")
    lock = json_read(package / "python/rife/runtime-lock.json")
    with RuntimeLease(root, exclusive=True):
        base.recover_transaction(root)
        if shutil.disk_usage(parent).free < 2_000_000_000:
            raise RuntimeError("Radeon RIFE needs 2 GB free for staging and its offline compiler")
        stage = parent / ("." + root.name + ".stage-" + uuid.uuid4().hex)
        previous = parent / ("." + root.name + ".previous-" + uuid.uuid4().hex)
        stage.mkdir()
        promoted = False
        try:
            base.log("Installing the Radeon Vulkan worker and CPU-only offline compiler")
            assets = [a for a in lock["assets"] if not any(key in a["path"] for key in ("torch-", "cuda_", "models/"))]
            assets += [dependencies["cpu_torch"], next(a for a in lock["assets"] if a["path"] == "models/flownet_v4.4.pkl")]
            for asset in assets:
                archive = base.download(asset, cache)
                if asset["path"].startswith("models/"):
                    target = stage / asset["path"]
                    target.parent.mkdir(exist_ok=True)
                    shutil.copy2(archive, target)
                else:
                    base.setup.extract(archive, stage / ("python/Lib/site-packages" if archive.suffix == ".whl" else "python"), wheel=archive.suffix == ".whl")
            shutil.copytree(payload, stage / "vulkan")
            for item in lock["msvc_crt"]["files"]:
                source = crt_source / item["name"]
                if base.sha(source) != item["sha256"]:
                    raise RuntimeError("Portable CRT integrity mismatch")
                for target in (stage / "python", stage / "vulkan"):
                    shutil.copy2(source, target / item["name"])
            (stage / "python/python312._pth").write_text("python312.zip\n.\nLib/site-packages\n../app\nimport site\n", encoding="utf-8", newline="\n")
            overlay = base.install_overlay(stage, package, lock, cache)
            (stage / "tools").mkdir()
            shutil.copy2(package / "tools/vulkan_compile.py", stage / "tools/vulkan_compile.py")
            receipt_files = {p.relative_to(stage).as_posix(): base.sha(p) for folder in (stage / "app", stage / "tools", stage / "vulkan")
                             for p in folder.rglob("*") if p.is_file()}
            receipt_files["models/flownet_v4.4.pkl"] = base.sha(stage / "models/flownet_v4.4.pkl")
            backend = dict(schema=1, backend="vulkan", payload=payload_manifest, files=receipt_files)
            (stage / "vulkan/backend.json").write_text(json.dumps(backend, indent=2) + "\n", encoding="utf-8")
            (stage / "install-receipt.json").write_text(json.dumps(dict(schema=1, backend="vulkan", overlay_sha256=overlay,
                installed_utc=datetime.now(timezone.utc).isoformat()), indent=2), encoding="utf-8")
            base.log("Preparing a small device-specific graph and testing D3D12/Vulkan interop")
            probe = run_native(stage, ["--probe", "-1"])
            command = [str(stage / "python/python.exe"), "-I", "-B", str(stage / "tools/vulkan_compile.py"), "--root", str(stage),
                       "--worker", str(stage / "vulkan/ofxr_rife_vulkan.exe"), "--pnnx", str(stage / "vulkan/pnnx.exe"),
                       "--cache", str(stage / "vulkan/cache"), "--width", "64", "--height", "64", "--gpu", str(probe["gpu"])]
            completed = subprocess.run(command, capture_output=True, text=True, timeout=600, creationflags=subprocess.CREATE_NO_WINDOW)
            base.log(completed.stdout + completed.stderr)
            if completed.returncode:
                raise RuntimeError("Radeon offline compiler validation failed")
            run_native(stage, ["--selftest", stage, probe["gpu"]])
            base.write_transaction(root, stage, previous, "prepared")
            if root.exists():
                root.rename(previous)
            try:
                stage.rename(root)
                promoted = True
                base.write_transaction(root, stage, previous, "committed")
            except BaseException:
                base.recover_transaction(root)
                raise
            # Preserve a previous installation on success as well as failure.
            base.transaction_path(root).unlink(missing_ok=True)
            base.log("Radeon runtime ready. Previous runtime preserved at " + str(previous))
            return dict(schema=1, status="ready", message="Experimental Radeon Vulkan RIFE installed; local Vulkan inference and D3D12 sharing passed.",
                        root=str(root), backend="AMD Radeon / Vulkan (experimental)", installed_bytes=sum(p.stat().st_size for p in root.rglob("*") if p.is_file()),
                        required_free_bytes=2_000_000_000, log_path="")
        finally:
            if not promoted and stage.exists():
                base.log("Failed staging runtime preserved for diagnosis: " + str(stage))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--package", type=Path, default=PACKAGE)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--update", action="store_true")
    args = parser.parse_args()
    result = health(args.root.resolve(), args.package.resolve(), args.update)
    args.result.parent.mkdir(parents=True, exist_ok=True)
    args.result.write_text(json.dumps(result), encoding="utf-8")
    print(result["message"])
    return base.EXIT[result["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
