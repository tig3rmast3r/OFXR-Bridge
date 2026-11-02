"""Standard-library portable setup tests; no downloads/runtime/GPU changes."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("rife_setup", ROOT / "tools" / "prepare_rife_runtime.py")
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_wheel_extracts_purelib_to_private_destination(self):
        path = self.root / "test.whl"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("sample.data/purelib/package/module.py", "private")
            archive.writestr("sample.data/scripts/not-a-library", "skip")
        setup.extract(path, self.root / "backend", wheel=True)
        self.assertEqual((self.root / "backend/package/module.py").read_text(), "private")
        self.assertFalse((self.root / "backend/sample.data/scripts/not-a-library").exists())

    def test_zip_traversal_is_rejected(self):
        path = self.root / "test.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("../outside.py", "bad")
        with self.assertRaisesRegex(RuntimeError, "escapes"):
            setup.extract(path, self.root / "backend")
        self.assertFalse((self.root / "outside.py").exists())

    def tar(self, name, content=b"wrapper", kind=None, link=""):
        path = self.root / "wrapper.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            item = tarfile.TarInfo(name)
            if kind:
                item.type = kind
                item.linkname = link
                archive.addfile(item)
            else:
                item.size = len(content)
                archive.addfile(item, io.BytesIO(content))
        return path

    def test_official_wrapper_sdist_extracts_only_tensorrt_package(self):
        path = self.tar("tensorrt_cu12-10.14.1.48/tensorrt/__init__.py")
        setup.extract_tensorrt_wrapper(path, self.root / "backend")
        self.assertEqual((self.root / "backend/tensorrt/__init__.py").read_bytes(), b"wrapper")

    def test_tar_traversal_and_links_are_rejected(self):
        path = self.tar("version/tensorrt/../../outside.py")
        with self.assertRaisesRegex(RuntimeError, "escapes"):
            setup.extract_tensorrt_wrapper(path, self.root / "backend")
        self.assertFalse((self.root / "outside.py").exists())
        path = self.tar("version/tensorrt/link", kind=tarfile.SYMTYPE, link="../../outside")
        with self.assertRaisesRegex(RuntimeError, "link"):
            setup.extract_tensorrt_wrapper(path, self.root / "backend")

    def test_cached_download_is_verified_without_network(self):
        path = self.root / "cached.whl"
        path.write_bytes(b"valid")
        asset = {"path": path.name, "bytes": 5, "sha256": hashlib.sha256(b"valid").hexdigest(), "url": "https://invalid/"}
        with patch("urllib.request.urlopen") as network:
            self.assertEqual(setup.download(asset, self.root), path)
            network.assert_not_called()
        path.write_bytes(b"wrong")
        with self.assertRaisesRegex(RuntimeError, "Checksum"):
            setup.download(asset, self.root)

    def test_optional_backend_installs_in_one_private_folder(self):
        wheel = self.root / "backend.whl"
        with zipfile.ZipFile(wheel, "w") as archive:
            archive.writestr("tensorrt_bindings/__init__.py", "binding")
        wrapper = self.tar("version/tensorrt/__init__.py")
        assets = [{"path": path.name, "bytes": path.stat().st_size,
                   "sha256": setup.digest(path), "url": "https://invalid/",
                   **({"format": "tensorrt-wrapper-sdist"} if path == wrapper else {})} for path in (wheel, wrapper)]
        setup.prepare_tensorrt({"tensorrt_backend": {"version": "test", "assets": assets}}, self.root)
        self.assertTrue((self.root / "backends/tensorrt/tensorrt/__init__.py").is_file())
        self.assertTrue((self.root / "backends/tensorrt/tensorrt_bindings/__init__.py").is_file())
        self.assertTrue((self.root / "backends/tensorrt/ofxr-backend.json").is_file())
        self.assertFalse((self.root / "python/Lib/site-packages/tensorrt").exists())

    def test_backend_lock_is_pinned_and_keeps_old_core_assets(self):
        lock = json.loads((ROOT / "python/rife/runtime-lock.json").read_text())
        backend = lock["tensorrt_backend"]
        self.assertEqual(backend["version"], "10.14.1.48")
        self.assertEqual(backend["opset"], 19)
        self.assertEqual(len(backend["assets"]), 6)
        self.assertFalse(any("tensorrt" in asset["path"] for asset in lock["assets"]))
        for asset in backend["assets"]:
            self.assertEqual(len(asset["sha256"]), 64)
            self.assertGreater(asset["bytes"], 0)
            self.assertTrue(asset["url"].startswith("https://"))

    def test_cli_has_offline_optional_backend_switch(self):
        result = subprocess.run([sys.executable, "-B", str(ROOT / "tools/prepare_rife_runtime.py"), "--help"],
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"--skip-tensorrt", result.stdout)


if __name__ == "__main__":
    unittest.main()
