"""Actual headset-free Vulkan preparation, protocol and shared-resource checks."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def test(root, width, height, result_path):
    sys.path.insert(0, str(root / "app"))
    from runtime_lease import RuntimeLease
    width, height = (width+31)//32*32, (height+31)//32*32
    binary = root / "vulkan/ofxr_rife_vulkan.exe"
    with RuntimeLease(root):
        probe = subprocess.run([str(binary), "--probe", "-1"], capture_output=True, text=True, timeout=30, check=True)
        gpu = json.loads(probe.stdout)["gpu"]
        command = [str(root / "python/python.exe"), "-I", "-B", str(root / "tools/vulkan_compile.py"), "--root", str(root),
                   "--worker", str(binary), "--pnnx", str(root / "vulkan/pnnx.exe"), "--cache", str(root / "vulkan/cache"),
                   "--width", str(width), "--height", str(height), "--gpu", str(gpu)]
        prepared = subprocess.run(command, capture_output=True, text=True, timeout=1200)
        if prepared.returncode:
            raise RuntimeError("Preparation failed: " + prepared.stderr[-4000:])
        execution = subprocess.run([str(binary), "--selftest", str(root), str(gpu), str(width), str(height)], capture_output=True, text=True, timeout=180)
        result = json.loads(execution.stdout)
        result.update(protocol_pass=False)
        # Exercise real line-protocol parsing and reject the whole invalid batch
        # before any external buffer is accessed, then verify worker recovery.
        requests = ({"op": "infer_batch", "requests": [{"id": 1, "model": 3, "fraction": .5}, {"id": 1, "model": 3, "fraction": .5}]},
                    {"op": "infer", "id": 0, "model": 3, "fraction": 2}, {"op": "memory"})
        protocol = subprocess.run([str(binary), "--worker", str(root)], input="".join(json.dumps(r)+"\n" for r in requests),
                                  capture_output=True, text=True, timeout=30)
        receipts = [json.loads(line) for line in protocol.stdout.splitlines()]
        hello = receipts[0]
        result["protocol_pass"] = bool(hello.get("ok") and hello.get("protocol") == 2 and hello.get("fraction_batch")
                                        and [r.get("ok") for r in receipts[1:]] == [False, False, True] and protocol.returncode == 0)
        result["ok"] = bool(result.get("ok") and result["protocol_pass"] and execution.returncode == 0)
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(("PASS" if result["ok"] else "FAIL") + f": {width}x{height}, two eyes, three output banks, D3D12/Vulkan and protocol. Report: {result_path}")
        return 0 if result["ok"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--width", type=int, default=64)
    parser.add_argument("--height", type=int, default=64)
    args = parser.parse_args()
    try:
        raise SystemExit(test(args.root.resolve(), args.width, args.height, args.result.resolve()))
    except Exception as error:
        args.result.parent.mkdir(parents=True, exist_ok=True)
        args.result.write_text(json.dumps(dict(schema=1, ok=False, root=str(args.root), error=str(error)), indent=2) + "\n", encoding="utf-8")
        print("FAIL: " + str(error), file=sys.stderr)
        raise SystemExit(1)
