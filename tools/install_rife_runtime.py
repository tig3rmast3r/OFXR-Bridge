"""Pinned per-user installer. Invoked by the PowerShell bootstrap, never pip."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import traceback
import urllib.request
import uuid

sys.dont_write_bytecode = True
sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "tools"))
sys.path.insert(0, str(PACKAGE / "python/rife"))
from runtime_lease import RuntimeBusy, RuntimeLease
import prepare_rife_runtime as setup

EXIT = {"ready": 0, "missing": 10, "repair": 11, "unsupported": 12, "busy": 13, "error": 1}
REQUIRED_FREE = 14_000_000_000
INSTALLED_BYTES = 7_900_000_000
RENAME_DELAYS = (0.25, 0.5, 1.0, 2.0, 2.0, 2.0)


def normalized(path):
    return Path(path).read_bytes().replace(b"\r\n", b"\n")


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def log(message):
    print(message, flush=True)
    if LOG:
        with LOG.open("a", encoding="utf-8") as stream:
            stream.write(datetime.now(timezone.utc).isoformat() + " " + message + "\n")


LOG = None


def lock_owners(source, destination):
    """Diagnostic child only: query Restart Manager, never stop/restart apps."""
    if os.name != "nt":
        print("Lock-owner query is only available on Windows.")
        return
    import ctypes
    from ctypes import wintypes as w

    class UniqueProcess(ctypes.Structure):
        _fields_ = [("pid", w.DWORD), ("started", w.FILETIME)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [("process", UniqueProcess), ("app", w.WCHAR * 256),
                    ("service", w.WCHAR * 64), ("kind", ctypes.c_int),
                    ("status", w.ULONG), ("session", w.DWORD), ("restartable", w.BOOL)]

    samples = ("python/python.exe", "python/python312.dll",
               "python/Lib/site-packages/torch/lib/torch_cuda.dll",
               "python/Lib/site-packages/torch/lib/torch_cpu.dll",
               "python/Lib/site-packages/torch/lib/c10.dll",
               "backends/tensorrt/tensorrt_libs/nvinfer_10.dll", "app/worker.py")
    files = [str(Path(directory) / name) for directory in (source, destination)
             for name in samples if (Path(directory) / name).is_file()]
    if not files:
        print("No sampled runtime files exist; lock owner is unknown.")
        return
    manager = ctypes.WinDLL("rstrtmgr")
    manager.RmStartSession.argtypes = [ctypes.POINTER(w.DWORD), w.DWORD, w.LPWSTR]
    manager.RmRegisterResources.argtypes = [w.DWORD, w.UINT, ctypes.POINTER(w.LPCWSTR),
        w.UINT, ctypes.POINTER(UniqueProcess), w.UINT, ctypes.POINTER(w.LPCWSTR)]
    manager.RmGetList.argtypes = [w.DWORD, ctypes.POINTER(w.UINT), ctypes.POINTER(w.UINT),
        ctypes.POINTER(ProcessInfo), ctypes.POINTER(w.DWORD)]
    manager.RmEndSession.argtypes = [w.DWORD]
    for name in ("RmStartSession", "RmRegisterResources", "RmGetList", "RmEndSession"):
        getattr(manager, name).restype = w.DWORD
    session = w.DWORD()
    code = manager.RmStartSession(ctypes.byref(session), 0, ctypes.create_unicode_buffer(33))
    if code:
        raise OSError(code, "Restart Manager session failed")
    try:
        paths = (w.LPCWSTR * len(files))(*files)
        code = manager.RmRegisterResources(session, len(files), paths, 0, None, 0, None)
        if code:
            raise OSError(code, "Restart Manager resource registration failed")
        needed, count, reasons = w.UINT(), w.UINT(), w.DWORD()
        code = manager.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), None, ctypes.byref(reasons))
        for unused in range(2):
            if code != 234:
                break
            if not 0 < needed.value <= 64:
                raise RuntimeError("Lock-owner list exceeds the diagnostic limit")
            entries = (ProcessInfo * needed.value)()
            count.value = needed.value
            code = manager.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), entries, ctypes.byref(reasons))
            if not code:
                for entry in entries[:count.value]:
                    print("Sampled-file user: pid=" + str(entry.process.pid) + " app=" +
                          repr(entry.app) + " service=" + repr(entry.service) + " status=" + str(entry.status))
        if code:
            raise OSError(code, "Restart Manager process query failed")
        if not count.value:
            print("No applications reported for sampled runtime files.")
        print("This sample cannot exclude directory handles, filter drivers or access-permission restrictions.")
    finally:
        manager.RmEndSession(session)


def rename_diagnostics(source, destination, query_owners=True):
    """Best effort; logging must not replace the original filesystem error."""
    try:
        log("Rename diagnostics: installer_pid=" + str(os.getpid()) + " executable=" +
            str(sys.executable) + " cwd=" + str(Path.cwd()))
        for label, path in (("source", source), ("destination", destination)):
            try:
                status = path.stat()
                log(label + "=" + str(path) + " exists=True attributes=" +
                    hex(getattr(status, "st_file_attributes", 0)) + " mode=" + oct(status.st_mode))
            except OSError as error:
                log(label + "=" + str(path) + " stat=" + repr(error))
        if query_owners:
            # Same single-file patch; isolated child bounds a slow OS query.
            command = [sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                       "--diagnose-locks", str(source), str(destination)]
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=3,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                for line in (result.stdout + "\n" + result.stderr).splitlines():
                    if line.strip():
                        log("Lock-owner diagnostic: " + line[:2000])
                log("Lock-owner diagnostic exit=" + str(result.returncode))
            except subprocess.TimeoutExpired:
                log("Lock-owner query timed out after 3s; its diagnostic child was stopped. Owner remains unknown.")
    except Exception as error:
        print("Additional rename diagnostics unavailable: " + repr(error), file=sys.stderr, flush=True)


def rename_runtime(source, destination, phase):
    source, destination = Path(source), Path(destination)
    started = time.monotonic()
    attempts = len(RENAME_DELAYS) + 1
    log("Runtime rename begin: phase=" + phase + " source=" + str(source) + " destination=" + str(destination))
    for attempt in range(attempts):
        try:
            source.rename(destination)
        except OSError as error:
            code = getattr(error, "winerror", None)
            retry = code in (5, 32, 33) and attempt < len(RENAME_DELAYS)
            log("Runtime rename failed: phase=" + phase + " attempt=" + str(attempt + 1) +
                "/" + str(attempts) + " elapsed_s=" + format(time.monotonic() - started, ".3f") +
                " winerror=" + str(code) + " error=" + repr(error))
            if attempt == 0 or not retry:
                rename_diagnostics(source, destination, query_owners=attempt == 0)
            if not retry:
                error.add_note("RIFE rename phase=" + phase + "; attempts=" + str(attempt + 1))
                raise
            log("Retrying the same rename in " + str(RENAME_DELAYS[attempt]) + "s; no runtime files removed or external applications stopped.")
            time.sleep(RENAME_DELAYS[attempt])
        else:
            log("Runtime rename succeeded: phase=" + phase + " attempt=" + str(attempt + 1) +
                " elapsed_s=" + format(time.monotonic() - started, ".3f"))
            return


def transaction_path(root):
    return root.parent / ("." + root.name + ".transaction.json")


def write_transaction(root, stage, previous, phase):
    path = transaction_path(root)
    partial = path.with_suffix(".partial")
    data = {"schema": 1, "root": str(root), "stage": str(stage), "previous": str(previous), "phase": phase}
    with partial.open("w", encoding="utf-8") as stream:
        json.dump(data, stream)
        stream.flush()
        os.fsync(stream.fileno())
    partial.replace(path)


def recover_transaction(root):
    """Caller holds the exclusive root lease; all journal paths are bounded."""
    journal = transaction_path(root)
    if not journal.is_file():
        return False
    data = json.loads(journal.read_text())
    if data.get("schema") != 1 or os.path.normcase(data.get("root", "")) != os.path.normcase(str(root)) or data.get("phase") not in ("prepared", "committed"):
        raise RuntimeError("Invalid RIFE installation recovery journal")
    paths = []
    for field in ("stage", "previous"):
        path = Path(data[field])
        suffix = "stage" if field == "stage" else "previous"
        if not path.is_absolute() or path.parent != root.parent or not re.fullmatch(re.escape("." + root.name + "." + suffix + "-") + "[0-9a-f]{32}", path.name):
            raise RuntimeError("Recovery path escapes the managed runtime parent")
        if path.exists() and (path.is_symlink() or path.lstat().st_file_attributes & 0x400):
            raise RuntimeError("Recovery refuses a linked runtime directory")
        paths.append(path)
    stage, previous = paths
    if previous.exists() and (data["phase"] == "prepared" or not root.exists()):
        if root.exists():
            if stage.exists():
                raise RuntimeError("Ambiguous recovery directories; original runtime preserved")
            rename_runtime(root, stage, "recovery-move-uncommitted")
        rename_runtime(previous, root, "recovery-restore-previous")
        log("Recovered the previous working RIFE runtime")
    if stage.exists():
        assert stage.resolve().is_relative_to(root.parent.resolve())
        shutil.rmtree(stage)
    if data["phase"] == "committed" and root.exists() and previous.exists():
        assert previous.resolve().is_relative_to(root.parent.resolve())
        shutil.rmtree(previous)
    journal.unlink()
    return True


def download(asset, cache):
    cache = cache.resolve()
    path = (cache / asset["path"]).resolve()
    if not path.is_relative_to(cache):
        raise RuntimeError("Download path escapes installer cache")
    path.parent.mkdir(parents=True, exist_ok=True)
    valid = lambda p: p.is_file() and p.stat().st_size == asset["bytes"] and sha(p) == asset["sha256"]
    if valid(path):
        log("Verified cached " + path.name)
        return path
    part = path.with_name(path.name + ".partial")
    log("Downloading " + path.name + " (" + str(asset["bytes"]) + " bytes)")
    with urllib.request.urlopen(asset["url"], timeout=120) as response, part.open("wb") as output:
        received = 0
        mark = 0
        while block := response.read(4 * 1024 * 1024):
            output.write(block)
            received += len(block)
            if received - mark >= 128 * 1024 * 1024:
                log("  " + path.name + ": " + str(received * 100 // asset["bytes"]) + "%")
                mark = received
    if not valid(part):
        raise RuntimeError("SHA256 or size mismatch: " + path.name)
    part.replace(path)
    log("Verified SHA256 " + path.name)
    return path


def install_overlay(stage, package, lock, cache):
    source = lock["upstream_source"]
    archive = download(source, cache)
    upstream = stage / "upstream-source"
    setup.extract(archive, upstream)
    origin = upstream / source["directory"]
    model_names = ["__init__.py", "warplayer.py", "IFNet_HDv3_v4_16_lite.py",
                   "IFNet_HDv3_v4_25_lite.py", "IFNet_HDv3_v4_3.py", "IFNet_HDv3_v4_4.py", "IFNet_HDv3_v4_6.py"]
    for name in model_names:
        target = stage / "app/models" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(origin / "vsrife" / name, target)
    (stage / "licenses").mkdir()
    shutil.copy2(origin / "LICENSE", stage / "licenses/vs-rife-LICENSE")
    crt_license = package / "licenses/MSVC_CRT_LICENSE.rtf"
    if crt_license.is_file():
        shutil.copy2(crt_license, stage / "licenses/MSVC_CRT_LICENSE.rtf")
    manifest_path = package / "python/rife/overlay-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if manifest["base_commit"] != lock["model_source"]["commit"]:
        raise RuntimeError("Overlay and base RIFE revision disagree")
    for name, digest in manifest["files"].items():
        src = (package / "python/rife" / name).resolve()
        if not src.is_relative_to((package / "python/rife").resolve()):
            raise RuntimeError("Overlay path escapes the release")
        content = normalized(src)
        if hashlib.sha256(content).hexdigest() != digest:
            raise RuntimeError("Modified release overlay file: " + name)
        target = (stage / "app" / name).resolve()
        if not target.is_relative_to((stage / "app").resolve()):
            raise RuntimeError("Overlay destination escapes runtime")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    (stage / "app/overlay-manifest.json").write_bytes(normalized(manifest_path))
    # Discard unneeded VapourSynth integration from the pinned base archive.
    assert upstream.resolve().is_relative_to(stage.resolve())
    shutil.rmtree(upstream)
    return hashlib.sha256(normalized(manifest_path)).hexdigest()


def health(stage, package, output):
    command = [str(stage / "python/python.exe"), "-I", "-B", str(package / "python/rife/check_runtime.py"),
               "--root", str(stage), "--package", str(package), "--result", str(output)]
    environment = {k: v for k, v in os.environ.items() if k.upper() in ("SYSTEMROOT", "WINDIR", "TEMP", "TMP", "LOCALAPPDATA")}
    environment["PATH"] = str(stage / "python") + os.pathsep + str(stage / "python/Lib/site-packages/torch/lib") + os.pathsep + str(Path(os.environ["SystemRoot"]) / "System32")
    started = time.monotonic()
    log("Validator starting: executable=" + command[0] + " cwd=" + str(stage))
    completed = subprocess.run(command, env=environment, cwd=stage, capture_output=True, text=True,
                               timeout=115, creationflags=subprocess.CREATE_NO_WINDOW)
    log("Validator process exited: code=" + str(completed.returncode) +
        " elapsed_s=" + format(time.monotonic() - started, ".3f"))
    if completed.stdout.strip():
        log(completed.stdout.strip())
    if completed.stderr.strip():
        log(completed.stderr.strip())
    if not output.is_file():
        raise RuntimeError("RIFE validation produced no result")
    result = json.loads(output.read_text(encoding="utf-8-sig"))
    if completed.returncode != 0 or result["status"] != "ready":
        raise RuntimeError(result.get("message", "RIFE validation failed"))
    return result


def install(root, cache, crt_source, package=PACKAGE, skip_tensorrt=False, reinstall=False):
    root = root.absolute()
    parent = root.parent
    parent.mkdir(parents=True, exist_ok=True)
    if root.is_symlink() or (root.exists() and root.lstat().st_file_attributes & 0x400):
        raise RuntimeError("The managed RIFE destination must be a regular directory, not a junction")
    with RuntimeLease(root, exclusive=True):
        recover_transaction(root)
        engine_cache = root.parent.parent / "RIFECache"
        if reinstall:
            if root.parent.name != "RIFE" or root.parent.parent.name != "ofxr-bridge":
                raise RuntimeError("Clean reinstall is restricted to the managed OFXR RIFE directory")
            if engine_cache.exists() and (engine_cache.is_symlink() or engine_cache.lstat().st_file_attributes & 0x400):
                raise RuntimeError("Clean reinstall refuses a linked TensorRT cache")
        if shutil.disk_usage(parent).free < REQUIRED_FREE:
            raise RuntimeError("RIFE needs at least 14 GB of free space for downloads and staged installation")
        lock_path = package / "python/rife/runtime-lock.json"
        lock = json.loads(lock_path.read_text())
        stage = parent / ("." + root.name + ".stage-" + uuid.uuid4().hex)
        previous = parent / ("." + root.name + ".previous-" + uuid.uuid4().hex)
        stage.mkdir()
        promoted = False
        recovery_failed = False
        try:
            log("Installing pinned Python, PyTorch and RIFE models")
            for asset in lock["assets"]:
                archive = download(asset, cache)
                if asset["path"].startswith("models/"):
                    target = stage / asset["path"]
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(archive, target)
                elif archive.suffix == ".whl":
                    setup.extract(archive, stage / "python/Lib/site-packages", wheel=True)
                elif archive.suffix == ".zip":
                    setup.extract(archive, stage / "python")
                else:
                    raise RuntimeError("Unsupported core dependency format")
            for item in lock["msvc_crt"]["files"]:
                original = crt_source / item["name"]
                if not original.is_file() or sha(original) != item["sha256"]:
                    raise RuntimeError("Missing or incorrect bundled portable CRT: " + item["name"])
                shutil.copy2(original, stage / "python" / item["name"])
            (stage / "python/python312._pth").write_text("python312.zip\n.\nLib/site-packages\n../app\nimport site\n", encoding="utf-8")
            if not skip_tensorrt:
                log("Installing pinned TensorRT backend")
                backend = lock["tensorrt_backend"]
                destination = stage / "backends/tensorrt"
                for asset in backend["assets"]:
                    archive = download(asset, cache)
                    if asset.get("format") == "tensorrt-wrapper-sdist":
                        setup.extract_tensorrt_wrapper(archive, destination)
                    else:
                        setup.extract(archive, destination, wheel=True)
                (destination / "ofxr-backend.json").write_text(json.dumps(backend, indent=2), encoding="utf-8")
            log("Applying the complete OFXR overlay shipped with this release")
            overlay = install_overlay(stage, package, lock, cache)
            receipt = {"schema": 1, "lock_sha256": hashlib.sha256(normalized(lock_path)).hexdigest(),
                       "overlay_sha256": overlay, "base_commit": lock["model_source"]["commit"],
                       "installed_utc": datetime.now(timezone.utc).isoformat(), "backend": "nvidia"}
            (stage / "install-receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
            (stage / "runtime.json").write_bytes(normalized(lock_path))
            log("Checking imports, pinned versions and actual small NVIDIA CUDA inference")
            result = health(stage, package, parent / "stage-health.json")
            write_transaction(root, stage, previous, "prepared")
            try:
                if root.exists():
                    rename_runtime(root, previous, "move-previous-runtime")
                rename_runtime(stage, root, "activate-validated-runtime")
                promoted = True
                write_transaction(root, stage, previous, "committed")
            except BaseException as error:
                try:
                    recover_transaction(root)
                except Exception as recovery_error:
                    recovery_failed = True
                    error.add_note("RIFE recovery also failed: " + repr(recovery_error))
                    log("Recovery failed; runtime directories and journal preserved:\n" + traceback.format_exc())
                promoted = False
                raise
            if previous.exists():
                assert previous.resolve().is_relative_to(parent.resolve())
                try:
                    shutil.rmtree(previous)
                except OSError:
                    log("The new runtime is ready; previous runtime cleanup is deferred: " + str(previous))
            if not previous.exists():
                transaction_path(root).unlink(missing_ok=True)
            if reinstall and engine_cache.exists():
                # Exact managed sibling only; games/ and logs/ remain outside
                # the replaced runtime. An active worker cannot hold this lease.
                if engine_cache.resolve().parent != root.parent.parent.resolve():
                    raise RuntimeError("TensorRT cache escapes the managed OFXR directory")
                shutil.rmtree(engine_cache)
                log("Previous TensorRT engines removed; recorded games preserved")
            result.update(root=str(root), installed_bytes=sum(p.stat().st_size for p in root.rglob("*") if p.is_file()))
            log("RIFE installation complete: " + str(root))
            return result
        finally:
            if not promoted and stage.exists() and not recovery_failed:
                assert stage.resolve().is_relative_to(parent.resolve())
                active_error = sys.exc_info()[0] is not None
                try:
                    shutil.rmtree(stage)
                except OSError:
                    log("Staging cleanup failed; directory retained: " + str(stage) + "\n" + traceback.format_exc())
                    if not active_error:
                        raise


def main():
    global LOG
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--crt-source", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--skip-tensorrt", action="store_true")
    parser.add_argument("--reinstall", action="store_true")
    parser.add_argument("--backend", choices=("nvidia", "vulkan"), default="nvidia")
    args = parser.parse_args()
    logs = args.root.parent / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    LOG = logs / ("install-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + str(os.getpid()) + ".log")
    log("Installer rename-retry-v1; pid=" + str(os.getpid()) + " executable=" + str(sys.executable))
    result = {"schema": 1, "status": "error", "message": "", "root": str(args.root), "backend": "",
              "installed_bytes": INSTALLED_BYTES, "required_free_bytes": REQUIRED_FREE, "log_path": str(LOG)}
    if args.backend == "vulkan":
        result.update(backend="AMD Radeon / Vulkan (experimental)", installed_bytes=0, required_free_bytes=2_000_000_000)
    try:
        if args.backend == "vulkan":
            from install_rife_vulkan import install as install_vulkan
            result.update(install_vulkan(args.root, args.cache, args.crt_source))
        else:
            result.update(install(args.root, args.cache, args.crt_source, skip_tensorrt=args.skip_tensorrt, reinstall=args.reinstall))
    except RuntimeBusy as error:
        result.update(status="busy", message=str(error))
    except Exception as error:
        result.update(status="error", message="RIFE installation failed: " + str(error))
        log(result["message"])
        log(traceback.format_exc())
    result["log_path"] = str(LOG)
    args.result.parent.mkdir(parents=True, exist_ok=True)
    args.result.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
    return EXIT[result["status"]]


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--diagnose-locks":
        lock_owners(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(main())
