"""Explicit, offline preparation of the exact RIFE engines used by found games."""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import sys
import threading
import time

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "python/rife"))
QUALITIES = (10, 15, 18, 21, 25, 30)


def read_games(folder):
    games, errors = [], []
    for path in sorted(Path(folder).glob("*.json")):
        try:
            if path.is_symlink() or path.stat().st_size > 16384:
                raise ValueError("oversized or linked record")
            value = json.loads(path.read_text(encoding="utf-8"))
            if value.get("schema") != 1 or not isinstance(value.get("game"), str):
                raise ValueError("invalid record header")
            shapes = value.get("shapes")
            if not isinstance(shapes, list) or not 1 <= len(shapes) <= 4:
                raise ValueError("invalid shape list")
            for shape in shapes:
                if not all(type(shape.get(k)) is int for k in ("width", "height", "eyes")):
                    raise ValueError("non-integer dimensions")
                if not (1 <= shape["width"] <= 16384 and 1 <= shape["height"] <= 16384 and shape["eyes"] in (1, 2)):
                    raise ValueError("unsupported dimensions or eye layout")
            luid = int(value.get("adapter_luid", "0"))
            if not 0 <= luid < 2**64:
                raise ValueError("invalid adapter LUID")
            games.append({**value, "adapter_luid": luid})
        except (OSError, ValueError, TypeError, AttributeError) as error:
            errors.append(path.name + ": " + str(error))
    return games, errors


def build_plan(games, adapters):
    """Deduplicate by actual network shape/device; multipliers/routes add no keys."""
    plan = {}
    for game in games:
        luid = game["adapter_luid"]
        if luid and luid not in adapters:
            raise ValueError("NVIDIA adapter used by " + game["game"] + " is not available")
        device = adapters[luid] if luid else next(iter(adapters.values()))
        for shape in game["shapes"]:
            for quality in QUALITIES:
                # Match native ceil(unsigned_extent * float(percent)/100).
                # Emulate float32 operations rather than rounding the full-size viewport.
                import struct
                f32 = lambda v: struct.unpack("f", struct.pack("f", v))[0]
                scale = f32(f32(quality) / f32(100))
                width = max(32, math.ceil(f32(shape["width"] * scale)))
                height = max(32, math.ceil(f32(shape["height"] * scale)))
                key = (device, width, height, shape["eyes"])
                plan.setdefault(key, set()).add(game["game"])
    return [(key, sorted(names)) for key, names in sorted(plan.items())]


def cuda_adapters():
    import torch
    from cuda.bindings import driver as cu
    if cu.cuInit(0)[0] != cu.CUresult.CUDA_SUCCESS:
        raise RuntimeError("NVIDIA CUDA initialization failed")
    out = {}
    for index in range(torch.cuda.device_count()):
        code, device = cu.cuDeviceGet(index)
        if code != cu.CUresult.CUDA_SUCCESS:
            continue
        code, luid, _ = cu.cuDeviceGetLuid(device)
        if code == cu.CUresult.CUDA_SUCCESS:
            out[int.from_bytes(luid, "little")] = index
    if not out:
        raise RuntimeError("No supported NVIDIA CUDA adapter is available")
    return out


def prepare_one(backend, shape, cancelled, sleep=time.sleep):
    """Explicit retry bypasses in-game negative-cache backoff, never valid engines."""
    from trt_cache import cache_key, validate_entry
    from types import SimpleNamespace
    _, width, height, eyes = shape
    spec = backend._spec(SimpleNamespace(shape=(eyes, 3, height, width)))
    key = cache_key(spec)
    path = validate_entry(backend.directory, key, spec)
    cached = path is not None
    if not cached:
        backend._start(key, spec)
        while backend._job is not None:
            if cancelled():
                backend.close()
                raise InterruptedError("Pre-compilation cancelled")
            sleep(.2)
            backend._poll()
        path = validate_entry(backend.directory, key, spec)
        if path is None:
            raise RuntimeError(backend.note or "TensorRT did not produce a valid engine")
    if cancelled():
        raise InterruptedError("Pre-compilation cancelled")
    # Verify the resulting engine can actually run on this GPU, not just deserialize.
    import torch
    from trt_backend import Engine
    with torch.inference_mode():
        engine = Engine(path, spec, backend.runtime_root)
        a = torch.zeros((eyes, 3, height, width), device=backend.device, dtype=torch.float16)
        output = engine.infer(a, a, .5)
        torch.cuda.synchronize(backend.device)
        if output.shape != (eyes, 5, height, width) or not torch.isfinite(output).all().item():
            raise RuntimeError("Compiled RIFE engine failed its CUDA inference check")
    return key, cached


def run(args):
    from trt_cache import atomic_json, cache_directory
    from runtime_lease import RuntimeBusy, RuntimeLease
    result = dict(schema=1, status="error", message="", root=str(args.root), backend="NVIDIA / TensorRT",
                  installed_bytes=0, required_free_bytes=0, log_path=str(args.log), completed=0, total=0)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    def report(message, status="running"):
        current = min(result["completed"] + 1, result["total"]) if status == "running" else result["completed"]
        atomic_json(args.progress, dict(status=status, completed=result["completed"], current=current, total=result["total"], message=message))
        with args.log.open("a", encoding="utf-8") as stream:
            stream.write(datetime.now(timezone.utc).isoformat() + " " + message + "\n")
    try:
        games, errors = read_games(args.games or args.root.parent / "games")
        if not games:
            raise RuntimeError("No recorded game resolutions. Open a VR game with this OFXR build first.")
        if errors:
            raise RuntimeError("Invalid game records: " + "; ".join(errors))
        # Excludes installers, other offline runs and active managed RIFE workers.
        with RuntimeLease(args.root, exclusive=True):
            import torch
            from trt_backend import Backend
            adapters = cuda_adapters()
            plan = build_plan(games, adapters)
            result["total"] = len(plan)
            directory = args.cache or cache_directory()
            failures = []
            for shape, names in plan:
                if args.cancel.exists():
                    raise InterruptedError("Pre-compilation cancelled; completed engines remain available")
                device, width, height, eyes = shape
                torch.cuda.set_device(device)
                backend = Backend(args.root, PACKAGE / "python/rife", directory)
                backend.set_device(device)
                try:
                    if not backend._initialize():
                        raise RuntimeError(backend.note)
                    # Do not create engines for a different worker overlay by accident.
                    from trt_cache import file_sha256
                    for name in ("trt_compile.py", "models/IFNet_HDv3_v4_4.py", "models/warplayer.py"):
                        if file_sha256(args.root / "app" / name) != file_sha256(PACKAGE / "python/rife" / name):
                            raise RuntimeError("Installed RIFE code differs from this release. Run Check RIFE and repair it first.")
                    report(f"{width}×{height}, {eyes} eye(s): {', '.join(names)}")
                    key, cached = prepare_one(backend, shape, args.cancel.exists)
                    report(("Already ready: " if cached else "Compiled and checked: ") + key)
                except InterruptedError:
                    raise
                except Exception as error:
                    failures.append(str(error))
                    report("Failed: " + str(error))
                finally:
                    backend.close()
                    torch.cuda.empty_cache()
                result["completed"] += 1
                report("Combination finished")
            if failures:
                raise RuntimeError(f"{len(failures)}/{result['total']} combinations failed. See log: {args.log}")
        result.update(status="ready", message=f"TensorRT ready: {result['completed']}/{result['total']} combinations checked.")
        report(result["message"], "complete")
    except RuntimeBusy as error:
        result.update(status="busy", message=str(error));report(result["message"], "error")
    except Exception as error:
        result.update(status="error", message=str(error));report(result["message"], "cancelled" if isinstance(error, InterruptedError) else "error")
    atomic_json(args.result, result)
    return {"ready": 0, "busy": 13}.get(result["status"], 1)


def main():
    parser = argparse.ArgumentParser()
    for name in ("root", "result", "progress", "cancel", "log"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--games", type=Path)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--parent-watchdog", action="store_true")
    args = parser.parse_args()
    if args.parent_watchdog:
        from trt_compile import watch_parent
        threading.Thread(target=watch_parent, daemon=True).start()
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
