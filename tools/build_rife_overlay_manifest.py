"""Verify or explicitly refresh the complete same-release RIFE source overlay."""
import argparse
import hashlib
import json
from pathlib import Path


def manifest(source):
    lock = json.loads((source / "runtime-lock.json").read_text())
    files = {}
    for path in sorted(source.rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts and path.name != "overlay-manifest.json":
            files[path.relative_to(source).as_posix()] = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    return {"schema": 1, "base_commit": lock["model_source"]["commit"], "files": files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="Explicit source-maintenance operation; never run by the installer")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "python/rife"
    path = source / "overlay-manifest.json"
    expected = manifest(source)
    if args.write:
        path.write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")
    elif json.loads(path.read_text()) != expected:
        raise SystemExit("RIFE overlay manifest differs from this release's source. Review changes, then explicitly regenerate it.")
    print("Complete same-release RIFE overlay verified:", len(expected["files"]), "files; base", expected["base_commit"])


if __name__ == "__main__":
    main()
