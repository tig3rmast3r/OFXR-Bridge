"""Offline Radeon preparation using actual recorded per-game view layouts."""
import importlib.util
import json
from pathlib import Path
import subprocess


def build_plan(games, gpu):
    from precompile_rife import build_plan as existing
    from vulkan_compile import dimensions
    adapters = {game["adapter_luid"]: gpu for game in games}
    jobs = {}
    for shape, names in existing(games, adapters):
        device, width, height, eyes = shape
        width, height = dimensions(width, height)
        jobs.setdefault((device, width, height, eyes), set()).update(names)
    return [(shape, sorted(names)) for shape, names in sorted(jobs.items())]


def run(args):
    from precompile_rife import read_games, publish_json
    from runtime_lease import RuntimeLease, RuntimeBusy
    result = dict(schema=1, status="error", message="", root=str(args.root), backend="AMD Radeon / Vulkan (experimental)",
                  installed_bytes=0, required_free_bytes=0, log_path=str(args.log), completed=0, total=0)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    def report(message, status="running"):
        with args.log.open("a", encoding="utf-8") as log:
            log.write(message + "\n")
        try:
            publish_json(args.progress, dict(status=status, completed=result["completed"], current=min(result["completed"]+1, result["total"]), total=result["total"], message=message))
        except OSError:
            pass  # advisory progress; final result remains mandatory
    try:
        games, errors = read_games(args.games or args.root.parent / "games")
        if errors or not games:
            raise RuntimeError("Invalid or missing recorded game resolutions: " + "; ".join(errors))
        with RuntimeLease(args.root, exclusive=True):
            worker = args.root / "vulkan/ofxr_rife_vulkan.exe"
            probe = json.loads(subprocess.check_output([str(worker), "--probe", "-1"], text=True, timeout=30))
            gpu = probe["gpu"]
            spec = importlib.util.spec_from_file_location("installed_vulkan_compiler", args.root / "tools/vulkan_compile.py")
            compiler = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(compiler)
            plan = build_plan(games, gpu)
            result["total"] = len(plan)
            failures = []
            for (device, width, height, eyes), names in plan:
                if args.cancel.exists():
                    raise InterruptedError("Vulkan preparation cancelled; completed plans remain available")
                report(f"{width}x{height}, {eyes} eye(s): {', '.join(names)}")
                try:
                    path = compiler.prepare(args.root, args.cache or args.root / "vulkan/cache", worker,
                                            args.root / "vulkan/pnnx.exe", width, height, device, eyes)
                    report("Prepared and numerically checked: " + str(path))
                except Exception as error:
                    failures.append(str(error))
                    report("Failed: " + str(error))
                result["completed"] += 1
            if failures:
                raise RuntimeError(f"{len(failures)}/{result['total']} Vulkan combinations failed. See log.")
        result.update(status="ready", message=f"Vulkan ready: {result['completed']}/{result['total']} combinations checked.")
    except RuntimeBusy as error:
        result.update(status="busy", message=str(error))
    except Exception as error:
        result.update(status="error", message=str(error))
    report(result["message"], "complete" if result["status"] == "ready" else "error")
    publish_json(args.result, result)
    return {"ready": 0, "busy": 13}.get(result["status"], 1)
