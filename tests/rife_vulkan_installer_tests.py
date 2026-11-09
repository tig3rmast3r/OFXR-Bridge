"""Radeon installer rollback/preservation and payload-integrity CPU fixtures."""
import contextlib
import io
import hashlib
import json
import os
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch
import zipfile

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(SOURCE / "tools"), str(SOURCE / "python/rife"), str(SOURCE / "tests")]
import install_rife_vulkan as vulkan
import rife_installer_tests as fixtures


class RadeonInstallerTests(unittest.TestCase):
    setUp = fixtures.InstallerTests.setUp
    tearDown = fixtures.InstallerTests.tearDown
    fixture = fixtures.InstallerTests.fixture
    old_runtime = fixtures.InstallerTests.old_runtime
    def fixture_vulkan(self):
        package, lock, manifest = self.fixture()
        cpu = self.cache / "cpu.whl"
        with zipfile.ZipFile(cpu, "w") as archive:
            archive.writestr("torch/fixture.txt", b"CPU compiler fixture")
        model = self.cache / "weights.pkl"
        model.write_bytes(b"pinned weights fixture")
        model_asset = fixtures.asset(model, "models/flownet_v4.4.pkl")
        target = self.cache / model_asset["path"]
        target.parent.mkdir()
        target.write_bytes(model.read_bytes())
        lock["assets"].append(model_asset)
        (package / "python/rife/runtime-lock.json").write_text(json.dumps(lock))
        for name in manifest["files"]:
            manifest["files"][name] = hashlib.sha256(vulkan.base.normalized(package / "python/rife" / name)).hexdigest()
        (package / "python/rife/overlay-manifest.json").write_text(json.dumps(manifest))
        payload = package / "rife-vulkan"
        payload.mkdir()
        for name in ("ofxr_rife_vulkan.exe", "pnnx.exe"):
            (payload / name).write_bytes(b"native fixture")
        (payload / "dependencies.json").write_text(json.dumps({"cpu_torch": fixtures.asset(cpu, "cpu.whl")}))
        files = {p.name: vulkan.base.sha(p) for p in payload.iterdir()}
        (payload / "manifest.json").write_text(json.dumps(dict(schema=1, backend="vulkan", files=files)))
        (package / "tools").mkdir()
        (package / "tools/vulkan_compile.py").write_text("# fixture compiler\n")
        return package

    def test_radeon_failure_keeps_previous_runtime(self):
        package = self.fixture_vulkan()
        self.old_runtime()
        with patch.object(vulkan, "run_native", side_effect=RuntimeError("driver import unsupported")), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "driver import unsupported"):
                vulkan.install(self.root, self.cache, self.directory, package)
        self.assertEqual((self.root / "existing-worker.txt").read_text(), "working-runtime")
        self.assertFalse(vulkan.base.transaction_path(self.root).exists())

    def test_radeon_success_preserves_old_runtime_and_pins_cpu_compiler(self):
        package = self.fixture_vulkan()
        self.old_runtime()
        with patch.object(vulkan, "run_native", return_value={"ok": True, "gpu": 0}), \
             patch.object(vulkan.subprocess, "run", return_value=types.SimpleNamespace(returncode=0, stdout="compiled", stderr="")), \
             contextlib.redirect_stdout(io.StringIO()):
            result = vulkan.install(self.root, self.cache, self.directory, package)
        self.assertEqual(result["status"], "ready")
        previous = list(self.root.parent.glob(".runtime.previous-*"))
        self.assertEqual(len(previous), 1)
        self.assertEqual((previous[0] / "existing-worker.txt").read_text(), "working-runtime")
        self.assertTrue((self.root / "python/Lib/site-packages/torch/fixture.txt").is_file())
        self.assertFalse((self.root / "backends/tensorrt").exists())
        self.assertEqual(json.loads((self.root / "vulkan/backend.json").read_text())["backend"], "vulkan")
        self.assertFalse(vulkan.base.transaction_path(self.root).exists())

    def test_radeon_tampered_payload_never_replaces_runtime(self):
        package = self.fixture_vulkan()
        self.old_runtime()
        (package / "rife-vulkan/ofxr_rife_vulkan.exe").write_bytes(b"tampered")
        with self.assertRaisesRegex(RuntimeError, "integrity mismatch"):
            vulkan.install(self.root, self.cache, self.directory, package)
        self.assertTrue((self.root / "existing-worker.txt").is_file())


if __name__ == "__main__":
    unittest.main()
