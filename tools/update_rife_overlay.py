"""Align only same-release RIFE source files; never install/download dependencies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import uuid
from datetime import datetime, timezone

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "python/rife"))
from runtime_lease import RuntimeBusy, RuntimeLease
from trt_cache import atomic_json


class OverlayRepairRequired(RuntimeError):
    pass


def normalized(path):
    return Path(path).read_bytes().replace(b"\r\n", b"\n")


def digest(content):
    return hashlib.sha256(content).hexdigest()


def regular(path):
    if path.exists() and (path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400):
        raise OverlayRepairRequired("RIFE code update refuses a linked path: " + str(path))


def write_bytes(path, content):
    partial = path.with_name(path.name + ".overlay-tmp")
    partial.write_bytes(content)
    partial.replace(path)


def transaction_path(root):
    return root / ".ofxr-overlay.transaction.json"


def owned_path(root, text, kind):
    path = Path(text)
    if not path.is_absolute() or path.parent != root or not re.fullmatch(r"\.app\." + kind + r"-[0-9a-f]{32}", path.name):
        raise OverlayRepairRequired("RIFE code recovery path escapes the runtime")
    regular(path)
    return path


def recover_locked(root):
    journal = transaction_path(root)
    if not journal.exists():
        return
    regular(journal)
    data = json.loads(journal.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or data.get("root") != str(root) or data.get("phase") not in ("prepared", "committed"):
        raise OverlayRepairRequired("Invalid RIFE code recovery journal")
    stage = owned_path(root, data["stage"], "stage")
    previous = owned_path(root, data["previous"], "previous")
    app = root / "app";receipt = root / "install-receipt.json"
    regular(app);regular(receipt)
    if data["phase"] == "prepared":
        if previous.exists():
            if app.exists():
                if stage.exists():
                    raise OverlayRepairRequired("Ambiguous RIFE code recovery directories")
                app.rename(stage)
            previous.rename(app)
        elif not data.get("had_app") and app.exists():
            if stage.exists():
                raise OverlayRepairRequired("Ambiguous new RIFE code recovery")
            app.rename(stage)
        before = data.get("receipt_before")
        if before is None:
            receipt.unlink(missing_ok=True)
        else:
            write_bytes(receipt, before.encode("utf-8"))
    elif not app.exists():
        raise OverlayRepairRequired("Committed RIFE source directory is missing")
    for path in (stage, previous):
        if path.exists():
            shutil.rmtree(path)
    journal.unlink()


def payload(root, package):
    source = package / "python/rife"
    lock_bytes = normalized(source / "runtime-lock.json")
    lock = json.loads(lock_bytes)
    manifest_bytes = normalized(source / "overlay-manifest.json")
    manifest = json.loads(manifest_bytes)
    if manifest.get("schema") != 1 or manifest.get("base_commit") != lock["model_source"]["commit"]:
        raise OverlayRepairRequired("RIFE release manifest is inconsistent")
    if len(manifest.get("files", {})) != 17:
        raise OverlayRepairRequired("RIFE release is missing its complete 17-file overlay")
    files = {}
    for name, expected in manifest["files"].items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise OverlayRepairRequired("RIFE release path escapes its overlay")
        path = source / relative
        if not path.resolve().is_relative_to(source.resolve()):
            raise OverlayRepairRequired("RIFE source link escapes its overlay")
        content = normalized(path)
        if digest(content) != expected:
            raise OverlayRepairRequired("RIFE release file differs from its manifest: " + name)
        files[name] = content
    receipt_path = root / "install-receipt.json"
    regular(root);regular(root / "app");regular(receipt_path)
    if receipt_path.is_file():
        before = receipt_path.read_text(encoding="utf-8")
        receipt = json.loads(before)
        if receipt.get("lock_sha256") != digest(lock_bytes):
            raise OverlayRepairRequired("RIFE dependency versions differ. Run Check RIFE to repair the runtime.")
    else:
        before = None
        installed = json.loads((root / "runtime.json").read_text(encoding="utf-8"))
        for name in ("python", "torch", "assets", "model_source", "msvc_crt", "tensorrt_backend"):
            if installed.get(name) != lock.get(name):
                raise OverlayRepairRequired("RIFE dependency versions differ. Run Check RIFE to repair the runtime.")
        receipt = dict(schema=1, backend="nvidia", base_commit=lock["model_source"]["commit"], lock_sha256=digest(lock_bytes))
    receipt["overlay_sha256"] = digest(manifest_bytes)
    files["overlay-manifest.json"] = manifest_bytes
    return files, receipt, before


def current(root, files, receipt):
    try:
        if json.loads((root / "install-receipt.json").read_text(encoding="utf-8")).get("overlay_sha256") != receipt["overlay_sha256"]:
            return False
        # Canonical LF files keep worker/precompiler cache fingerprints stable
        # regardless of the release checkout's Windows line endings.
        return all((root / "app" / name).read_bytes() == content for name, content in files.items())
    except (OSError, ValueError):
        return False


def sync_locked(root, package=PACKAGE):
    root = Path(root).absolute();package = Path(package).absolute()
    recover_locked(root)
    files, receipt, before = payload(root, package)
    if current(root, files, receipt):
        return False
    stage = root / (".app.stage-" + uuid.uuid4().hex)
    previous = root / (".app.previous-" + uuid.uuid4().hex)
    stage.mkdir()
    journal = transaction_path(root)
    try:
        for name, content in files.items():
            target = stage / name;target.parent.mkdir(parents=True, exist_ok=True);target.write_bytes(content)
        if not all((stage / name).read_bytes() == content for name, content in files.items()):
            raise OverlayRepairRequired("RIFE source staging verification failed")
        data = dict(schema=1, root=str(root), stage=str(stage), previous=str(previous),
                    had_app=(root / "app").exists(), receipt_before=before, phase="prepared")
        atomic_json(journal, data)
        try:
            if data["had_app"]:
                (root / "app").rename(previous)
            stage.rename(root / "app")
            receipt["overlay_updated_utc"] = datetime.now(timezone.utc).isoformat()
            atomic_json(root / "install-receipt.json", receipt)
            data["phase"] = "committed";atomic_json(journal, data)
        except BaseException:
            recover_locked(root)
            raise
        recover_locked(root)  # committed: keep new app/receipt, remove owned previous
        return True
    finally:
        if stage.exists() and not journal.exists():
            shutil.rmtree(stage)


def sync(root, package=PACKAGE):
    root = Path(root).absolute()
    if not (root / "python/python.exe").is_file():
        return None
    # An already aligned runtime needs no exclusive replacement, even if a
    # game is using it. A stale runtime requires the normal exclusive lease.
    with RuntimeLease(root):
        if not transaction_path(root).exists():
            files, receipt, _ = payload(root, Path(package).absolute())
            if current(root, files, receipt):
                return False
    with RuntimeLease(root, exclusive=True):
        return sync_locked(root, package)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--package", type=Path, default=PACKAGE)
    parser.add_argument("--result", type=Path, required=True)
    args = parser.parse_args()
    result = dict(schema=1, status="error", message="", root=str(args.root), backend="RIFE source overlay",
                  installed_bytes=0, required_free_bytes=0, log_path="")
    try:
        changed = sync(args.root, args.package)
        result.update(status="missing" if changed is None else "ready", message=
            "RIFE is not installed." if changed is None else "RIFE source files updated; dependencies and engine cache preserved." if changed else "RIFE source files are already aligned.")
    except RuntimeBusy:
        result.update(status="busy", message="RIFE is in use by a game or maintenance operation. Close the game or wait for maintenance, then use Check RIFE to align the source files.")
    except Exception as error:
        result.update(status="repair", message="RIFE source update failed: " + str(error))
    atomic_json(args.result, result)
    print(result["message"], flush=True)
    return {"ready": 0, "missing": 10, "busy": 13, "repair": 11}.get(result["status"], 1)


if __name__ == "__main__":
    raise SystemExit(main())
