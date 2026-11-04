"""Code-only update invariants; no download, model load or GPU use."""
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tools"))
sys.path.insert(0, str(SOURCE / "python/rife"))
import update_rife_overlay as update
from runtime_lease import RuntimeBusy, RuntimeLease


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="OFXR overlay update spaces ")
        self.folder = Path(self.temporary.name)
        self.package = self.folder / "package"
        shutil.copytree(SOURCE / "python/rife", self.package / "python/rife")
        self.root = self.folder / "runtime";self.root.mkdir()
        for name in ("python/python.exe", "models/weights.pkl", "backends/TensorRT.dll", "app/worker.py"):
            path = self.root / name;path.parent.mkdir(parents=True, exist_ok=True);path.write_bytes(b"existing contents")
        self.receipt = dict(schema=1, lock_sha256=update.digest(update.normalized(self.package / "python/rife/runtime-lock.json")),
                            overlay_sha256="older release", installed_utc="original date", backend="nvidia")
        update.atomic_json(self.root / "install-receipt.json", self.receipt)

    def tearDown(self):self.temporary.cleanup()

    def assert_old(self):
        self.assertEqual((self.root / "app/worker.py").read_bytes(), b"existing contents")
        self.assertEqual(json.loads((self.root / "install-receipt.json").read_text()), self.receipt)

    def test_updates_all_files_with_lf_and_keeps_libraries_weights_and_cache(self):
        engines = self.folder / "RIFECache";engines.mkdir();(engines / "old.engine").write_bytes(b"engine")
        self.assertTrue(update.sync(self.root, self.package))
        manifest = json.loads((self.package / "python/rife/overlay-manifest.json").read_text())
        for name, expected in manifest["files"].items():
            content = (self.root / "app" / name).read_bytes()
            self.assertEqual(hashlib.sha256(content).hexdigest(), expected)
            self.assertNotIn(b"\r\n", content)
        for name in ("python/python.exe", "models/weights.pkl", "backends/TensorRT.dll"):
            self.assertEqual((self.root / name).read_bytes(), b"existing contents")
        self.assertEqual((engines / "old.engine").read_bytes(), b"engine")
        self.assertEqual(json.loads((self.root / "install-receipt.json").read_text())["installed_utc"], "original date")

    def test_aligned_files_not_rewritten_and_can_be_checked_with_active_reader(self):
        update.sync(self.root, self.package)
        before = (self.root / "app/worker.py").stat().st_mtime_ns
        with RuntimeLease(self.root):self.assertFalse(update.sync(self.root, self.package))
        self.assertEqual((self.root / "app/worker.py").stat().st_mtime_ns, before)

    def test_old_runtime_in_use_never_overwritten(self):
        with RuntimeLease(self.root):
            with self.assertRaises(RuntimeBusy):update.sync(self.root, self.package)
        self.assert_old()

    def test_dependency_version_mismatch_requires_repair_without_changing_code(self):
        self.receipt["lock_sha256"] = "other dependencies"
        update.atomic_json(self.root / "install-receipt.json", self.receipt)
        with self.assertRaisesRegex(update.OverlayRepairRequired, "dependency versions"):
            update.sync(self.root, self.package)
        self.assert_old()

    def test_release_hash_error_preserves_old_code(self):
        (self.package / "python/rife/worker.py").write_text("tampered")
        with self.assertRaisesRegex(update.OverlayRepairRequired, "manifest"):
            update.sync(self.root, self.package)
        self.assert_old()

    def test_failed_promotion_restores_app_and_receipt(self):
        rename = Path.rename
        def refuse_stage(path, target):
            if path.name.startswith(".app.stage-"):raise OSError("activation denied")
            return rename(path, target)
        with patch.object(Path, "rename", refuse_stage):
            with self.assertRaisesRegex(OSError, "activation denied"):
                update.sync(self.root, self.package)
        self.assert_old()
        self.assertFalse(update.transaction_path(self.root).exists())

    def test_receipt_write_failure_rolls_back_promoted_code(self):
        atomic = update.atomic_json
        def refuse_receipt(path, content):
            if path.name == "install-receipt.json":raise OSError("receipt denied")
            return atomic(path, content)
        with patch.object(update, "atomic_json", refuse_receipt):
            with self.assertRaisesRegex(OSError, "receipt denied"):
                update.sync(self.root, self.package)
        self.assert_old()

    def test_crash_recovery_restores_previous_before_next_update(self):
        stage = self.root / (".app.stage-" + "a" * 32)
        previous = self.root / (".app.previous-" + "b" * 32)
        stage.mkdir();(stage / "worker.py").write_text("new contents")
        before = (self.root / "install-receipt.json").read_text()
        data = dict(schema=1, root=str(self.root), stage=str(stage), previous=str(previous),
                    phase="prepared", had_app=True, receipt_before=before)
        update.atomic_json(update.transaction_path(self.root), data)
        (self.root / "app").rename(previous);stage.rename(self.root / "app")
        with RuntimeLease(self.root, exclusive=True):update.recover_locked(self.root)
        self.assert_old()

    def test_windows_release_line_endings_do_not_cause_false_mismatch(self):
        for path in (self.package / "python/rife").rglob("*"):
            if path.is_file():path.write_bytes(update.normalized(path).replace(b"\n", b"\r\n"))
        self.assertTrue(update.sync(self.root, self.package))
        self.assertFalse(update.sync(self.root, self.package))
        self.assertEqual((self.root / "app/trt_compile.py").read_bytes(), update.normalized(self.package / "python/rife/trt_compile.py"))

    def test_missing_runtime_never_triggers_installation(self):
        self.assertIsNone(update.sync(self.folder / "missing", self.package))
        self.assertFalse((self.folder / "missing").exists())


if __name__ == "__main__":unittest.main()
