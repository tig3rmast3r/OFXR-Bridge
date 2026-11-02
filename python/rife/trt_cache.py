"""Versioned, content-addressed RIFE TensorRT cache (no GPU/dependency imports)."""
import hashlib
import json
import os
from pathlib import Path
import tempfile

SCHEMA = 1
ADAPTER_VERSION = "rife44-flow-mask-op19-batch-v1"


def file_sha256(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def cache_directory():
    base = os.environ.get("LOCALAPPDATA")
    if not base:
        base = str(Path.home() / "AppData" / "Local")
    return Path(base) / "ofxr-bridge" / "RIFECache"


def cache_key(spec):
    return hashlib.sha256(json.dumps(spec, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, sort_keys=True, separators=(",", ":"))
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def validate_entry(directory, key, spec):
    """Return an integrity-checked engine path; reject stale/partial/wrong caches."""
    directory = Path(directory)
    engine = directory / (key + ".engine")
    metadata = directory / (key + ".json")
    try:
        data = json.loads(metadata.read_text(encoding="utf-8"))
        if data.get("schema") != SCHEMA or data.get("key") != key or data.get("spec") != spec:
            return None
        size = engine.stat().st_size
        if size <= 0 or size != data.get("engine_bytes"):
            return None
        if file_sha256(engine) != data.get("engine_sha256"):
            return None
        return engine
    except (OSError, ValueError, TypeError):
        return None
