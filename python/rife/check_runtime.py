"""Bounded, isolated RIFE dependency and small CUDA-inference check."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_lease import RuntimeBusy, RuntimeLease

EXIT = {"ready": 0, "missing": 10, "repair": 11, "unsupported": 12, "busy": 13, "error": 1}


def failure_status(error):
    text = str(error).casefold()
    if any(word in text for word in ("out of memory", "cuda_error_out_of_memory", "device busy", "device is busy", "device unavailable")):
        return "busy"
    if any(word in text for word in ("no kernel image", "unsupported gpu", "driver version is insufficient", "cuda_error_system_driver_mismatch", "cuda_error_no_device", "no nvidia driver", "nvidia driver on your system", "cuda driver initialization failed")):
        return "unsupported"
    return "repair"


def normalized(path):
    return Path(path).read_bytes().replace(b"\r\n", b"\n")


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check(root, package):
    root = Path(root).resolve()
    package = Path(package).resolve()
    lock = json.loads((package / "python/rife/runtime-lock.json").read_text())
    manifest = json.loads((package / "python/rife/overlay-manifest.json").read_text())
    result = {"schema": 1, "status": "repair", "root": str(root), "backend": "",
              "message": "", "installed_bytes": 7_900_000_000,
              "required_free_bytes": 14_000_000_000, "log_path": ""}
    if not (root / "python/python.exe").is_file() or not (root / "app/worker.py").is_file():
        result.update(status="missing", message="The portable RIFE runtime is not installed.")
        return result
    with RuntimeLease(root):
        receipt = root / "install-receipt.json"
        if receipt.is_file():
            data = json.loads(receipt.read_text())
            if data.get("lock_sha256") != hashlib.sha256(normalized(package / "python/rife/runtime-lock.json")).hexdigest():
                raise RuntimeError("The installed RIFE dependency lock differs from this release.")
            if data.get("overlay_sha256") != hashlib.sha256(normalized(package / "python/rife/overlay-manifest.json")).hexdigest():
                raise RuntimeError("The installed RIFE overlay differs from this release.")
        else:
            legacy = json.loads((root / "runtime.json").read_text())
            for field in ("python", "torch", "assets", "model_source", "msvc_crt", "tensorrt_backend"):
                if legacy.get(field) != lock.get(field):
                    raise RuntimeError("Legacy RIFE dependency pins differ from this release: " + field)
        # Legacy shared runtimes remain usable if their original worker/model
        # overlay matches. New managed installations require every overlay file.
        for name, digest in manifest["files"].items():
            path = root / "app" / name
            if not receipt.is_file() and name in ("check_runtime.py", "runtime_lease.py", "runtime-lock.json"):
                continue
            if not path.is_file() or hashlib.sha256(normalized(path)).hexdigest() != digest:
                raise RuntimeError("Missing or modified RIFE adapter file: " + name)
        for item in lock["msvc_crt"]["files"]:
            if not (root / "python" / item["name"]).is_file() or sha(root / "python" / item["name"]) != item["sha256"]:
                raise RuntimeError("Missing or modified portable CRT: " + item["name"])
        for item in lock["assets"]:
            if item["path"].startswith("models/") and (not (root / item["path"]).is_file() or sha(root / item["path"]) != item["sha256"]):
                raise RuntimeError("Missing or modified RIFE model: " + item["path"])
        if sys.version_info[:3] != tuple(map(int, lock["python"].split("."))):
            raise RuntimeError("The portable Python version is incorrect.")
        sys.path.insert(0, str(root / "app"))
        import torch
        from cuda.bindings import driver as cu
        if torch.__version__ != lock["torch"]:
            raise RuntimeError("The portable PyTorch version is incorrect.")
        if not torch.cuda.is_available() or torch.cuda.device_count() < 1:
            result.update(status="unsupported", message="No usable NVIDIA CUDA GPU/driver was found. Installing RIFE cannot fix this driver or hardware limitation.")
            return result
        from worker import Runner
        runner = Runner()
        try:
            device = 0
            torch.cuda.set_device(device)
            torch.cuda.init()
            if cu.cuInit(0)[0] != cu.CUresult.CUDA_SUCCESS:
                raise RuntimeError("CUDA driver initialization failed.")
            runner.device = device
            runner.select_model(3)
            a = torch.zeros((1, 3, 32, 32), device=device, dtype=torch.float16)
            b = torch.ones_like(a) * 0.25
            with torch.inference_mode():
                output = runner.infer(a, b)
            torch.cuda.synchronize()
            if output.shape != (1, 5, 32, 32) or not bool(torch.isfinite(output).all()):
                raise RuntimeError("RIFE inference returned invalid flow/mask data.")
        finally:
            runner.close()
        backend = "PyTorch CUDA"
        note = ""
        try:
            from trt_backend import load_backend
            trt = load_backend(root)
            if trt.__version__ != lock["tensorrt_backend"]["version"]:
                raise RuntimeError("TensorRT version mismatch")
            backend += " + TensorRT " + trt.__version__
        except Exception as error:
            note = " TensorRT is unavailable; the working PyTorch fallback remains usable: " + str(error)
        result.update(status="ready", backend=backend, message="RIFE is ready on " + torch.cuda.get_device_name(device) + "." + note)
        return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    args = parser.parse_args()
    result = {"schema": 1, "status": "repair", "root": str(args.root), "message": "",
              "backend": "", "installed_bytes": 7_900_000_000, "required_free_bytes": 14_000_000_000, "log_path": ""}
    try:
        result = check(args.root, args.package)
    except RuntimeBusy as error:
        result.update(status="busy", message=str(error))
    except Exception as error:
        status = failure_status(error)
        prefix = {"busy": "RIFE cannot run while the GPU is unavailable or out of memory: ",
                  "unsupported": "RIFE is unsupported by the current NVIDIA GPU/driver: ",
                  "repair": "RIFE needs repair: "}[status]
        result.update(status=status, message=prefix + str(error))
    args.result.parent.mkdir(parents=True, exist_ok=True)
    args.result.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
    print(result["message"], flush=True)
    return EXIT[result["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
