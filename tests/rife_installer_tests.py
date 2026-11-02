"""Installer isolation/transaction/overlay tests; no download, model or GPU load."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tools"))
sys.path.insert(0, str(SOURCE / "python/rife"))
import install_rife_runtime as installer
import check_runtime
from runtime_lease import RuntimeBusy, RuntimeLease


def asset(path, relative):
    return {"path": relative, "url": "https://invalid.example/pinned", "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="OFXR RIFE install ü ")
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "runtime"
        self.cache = self.directory / "cache"
        self.cache.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def fixture(self):
        package = self.directory / "package"
        shutil.copytree(SOURCE / "python/rife", package / "python/rife", ignore=shutil.ignore_patterns("__pycache__"))
        lock = json.loads((package / "python/rife/runtime-lock.json").read_text())
        python = self.cache / "python.zip"
        with zipfile.ZipFile(python, "w") as z:
            z.writestr("python.exe", b"fixture-interpreter")
        base = self.cache / "base.zip"
        with zipfile.ZipFile(base, "w") as z:
            for name in ["__init__.py", "warplayer.py", "IFNet_HDv3_v4_16_lite.py", "IFNet_HDv3_v4_25_lite.py", "IFNet_HDv3_v4_3.py", "IFNet_HDv3_v4_4.py", "IFNet_HDv3_v4_6.py"]:
                z.writestr("base/vsrife/" + name, b"upstream original, not OFXR")
            z.writestr("base/LICENSE", b"upstream-license")
        lock["assets"] = [asset(python, "python.zip")]
        lock["msvc_crt"]["files"] = []
        lock["upstream_source"] = {**asset(base, "base.zip"), "directory": "base"}
        (package / "python/rife/runtime-lock.json").write_text(json.dumps(lock))
        manifest_path = package / "python/rife/overlay-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        for name in manifest["files"]:
            manifest["files"][name] = hashlib.sha256(installer.normalized(package / "python/rife" / name)).hexdigest()
        manifest_path.write_text(json.dumps(manifest))
        return package, lock, manifest

    def old_runtime(self):
        self.root.mkdir()
        (self.root / "existing-worker.txt").write_text("working-runtime")

    @unittest.skipUnless(os.name == "nt", "Windows PowerShell check")
    def test_check_ignores_package_and_ancestor_runtimes(self):
        package = self.directory / "lab/packages/package with spaces"
        (package / "tools").mkdir(parents=True)
        (package / "python/rife").mkdir(parents=True)
        shutil.copy2(SOURCE / "tools/Setup-RifeRuntime.ps1", package / "tools")
        shutil.copy2(SOURCE / "python/rife/runtime-lock.json", package / "python/rife")
        for candidate in (self.directory / "lab/runtime/rife", package / "rife"):
            (candidate / "python").mkdir(parents=True)
            (candidate / "app").mkdir()
            (candidate / "python/python.exe").write_bytes(b"must never be executed")
            (candidate / "app/worker.py").write_text("must never be loaded")
        local = self.directory / "isolated AppData"
        managed = local / "ofxr-bridge/RIFE/runtime"
        result = self.directory / "check.json"
        environment = dict(os.environ, LOCALAPPDATA=str(local))
        command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                   "-File", str(package / "tools/Setup-RifeRuntime.ps1"), "-Check", "-ResultJson", str(result)]
        check = subprocess.run(command, env=environment, capture_output=True, timeout=20)
        self.assertIn(check.returncode, (10, 12), check.stdout + check.stderr)
        status = json.loads(result.read_text(encoding="utf-8"))
        self.assertEqual(Path(status["root"]), managed)
        self.assertIn(status["status"], ("missing", "unsupported"))
        self.assertFalse(managed.exists())

    def test_source_lock_and_complete_same_release_overlay(self):
        lock = json.loads((SOURCE / "python/rife/runtime-lock.json").read_text())
        manifest = json.loads((SOURCE / "python/rife/overlay-manifest.json").read_text())
        self.assertEqual(lock["upstream_source"]["sha256"], "b32fe98ea3ce3a8db3a3020514df99d9696344e431038d2ae7c0c622214a4259")
        self.assertEqual(lock["model_source"]["commit"], manifest["base_commit"])
        expected = {p.relative_to(SOURCE / "python/rife").as_posix() for p in (SOURCE / "python/rife").rglob("*") if p.is_file() and "__pycache__" not in p.parts and p.name != "overlay-manifest.json"}
        self.assertEqual(set(manifest["files"]), expected)
        for name, digest in manifest["files"].items():
            self.assertEqual(hashlib.sha256(installer.normalized(SOURCE / "python/rife" / name)).hexdigest(), digest, name)

    def test_pinned_base_is_overwritten_by_exact_complete_release_files(self):
        package, lock, manifest = self.fixture()
        self.root.mkdir()
        installer.install_overlay(self.root, package, lock, self.cache)
        for name, digest in manifest["files"].items():
            self.assertEqual(installer.sha(self.root / "app" / name), digest)
        self.assertEqual((self.root / "app/models/__init__.py").read_bytes(), b"")
        self.assertEqual((self.root / "licenses/vs-rife-LICENSE").read_bytes(), b"upstream-license")
        self.assertFalse((self.root / "upstream-source").exists())

    def test_corrupt_overlay_is_rejected(self):
        package, lock, unused = self.fixture()
        (package / "python/rife/worker.py").write_text("tampered")
        self.root.mkdir()
        with self.assertRaisesRegex(RuntimeError, "Modified release overlay"):
            installer.install_overlay(self.root, package, lock, self.cache)

    def test_cached_download_hash_checked_and_bad_network_bytes_never_promoted(self):
        path = self.cache / "item.zip"
        path.write_bytes(b"good")
        item = asset(path, "item.zip")
        with patch("urllib.request.urlopen") as network:
            self.assertEqual(installer.download(item, self.cache), path)
            network.assert_not_called()
        path.write_bytes(b"bad!")
        with patch("urllib.request.urlopen", return_value=io.BytesIO(b"bad!")):
            with self.assertRaisesRegex(RuntimeError, "SHA256"):
                installer.download(item, self.cache)
        self.assertEqual(path.read_bytes(), b"bad!")

    def test_download_path_cannot_escape_cache(self):
        with self.assertRaisesRegex(RuntimeError, "escapes"):
            installer.download({"path": "../outside", "bytes": 0, "sha256": "", "url": ""}, self.cache)

    def test_multiple_readers_and_exclusive_installer_gate(self):
        self.old_runtime()
        with RuntimeLease(self.root), RuntimeLease(self.root):
            with self.assertRaises(RuntimeBusy):
                RuntimeLease(self.root, exclusive=True)
        with RuntimeLease(self.root, exclusive=True):
            with self.assertRaises(RuntimeBusy):
                RuntimeLease(self.root)
        with RuntimeLease(self.root):
            pass

    def test_busy_gpu_and_unsupported_driver_never_offer_reinstall(self):
        for message in ["CUDA out of memory", "CUDA_ERROR_OUT_OF_MEMORY", "CUDA device is busy", "device unavailable"]:
            self.assertEqual(check_runtime.failure_status(RuntimeError(message)), "busy")
        for message in ["no kernel image is available for execution", "CUDA driver version is insufficient", "CUDA_ERROR_SYSTEM_DRIVER_MISMATCH", "Found no NVIDIA driver on your system"]:
            self.assertEqual(check_runtime.failure_status(RuntimeError(message)), "unsupported")
        self.assertEqual(check_runtime.failure_status(ImportError("DLL load failed")), "repair")

    def interrupted_promotion(self, promoted=False, committed=False):
        self.old_runtime()
        stage = self.directory / (".runtime.stage-" + "a" * 32)
        previous = self.directory / (".runtime.previous-" + "b" * 32)
        stage.mkdir()
        (stage / "new-worker.txt").write_text("new-runtime")
        installer.write_transaction(self.root, stage, previous, "prepared")
        self.root.rename(previous)
        if promoted:
            stage.rename(self.root)
        if committed:
            installer.write_transaction(self.root, stage, previous, "committed")
        return stage, previous

    def test_restart_recovers_old_runtime_after_kill_between_renames(self):
        stage, previous = self.interrupted_promotion()
        with RuntimeLease(self.root, exclusive=True):
            installer.recover_transaction(self.root)
        self.assertEqual((self.root / "existing-worker.txt").read_text(), "working-runtime")
        self.assertFalse(stage.exists() or previous.exists() or installer.transaction_path(self.root).exists())

    def test_restart_rolls_back_uncommitted_new_runtime(self):
        self.interrupted_promotion(promoted=True)
        with RuntimeLease(self.root, exclusive=True):
            installer.recover_transaction(self.root)
        self.assertTrue((self.root / "existing-worker.txt").is_file())
        self.assertFalse((self.root / "new-worker.txt").exists())

    def test_restart_keeps_committed_runtime_and_finishes_cleanup(self):
        stage, previous = self.interrupted_promotion(promoted=True, committed=True)
        with RuntimeLease(self.root, exclusive=True):
            installer.recover_transaction(self.root)
        self.assertTrue((self.root / "new-worker.txt").is_file())
        self.assertFalse(stage.exists() or previous.exists())

    def test_recovery_rejects_paths_outside_managed_parent(self):
        stage, previous = self.interrupted_promotion()
        data = json.loads(installer.transaction_path(self.root).read_text())
        data["previous"] = str(self.directory.parent / previous.name)
        installer.transaction_path(self.root).write_text(json.dumps(data))
        with self.assertRaisesRegex(RuntimeError, "escapes"):
            installer.recover_transaction(self.root)
        self.assertTrue((previous / "existing-worker.txt").exists())

    def test_powershell_check_recovers_before_testing_canonical_interpreter(self):
        stage, previous = self.interrupted_promotion(promoted=True)
        output = self.directory / "ps-result.json"
        powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        process = subprocess.run([str(powershell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(SOURCE / "tools/Setup-RifeRuntime.ps1"), "-Check", "-Root", str(self.root), "-ResultJson", str(output)], capture_output=True, timeout=15)
        self.assertIn(process.returncode, (10, 12), process.stderr)
        self.assertEqual((self.root / "existing-worker.txt").read_text(), "working-runtime")
        self.assertFalse(stage.exists() or previous.exists())

    def test_powershell_cleanup_only_removes_its_owned_default_cache(self):
        cache = self.directory / "download-cache"
        cache.mkdir()
        (cache / "verified-archive").write_text("cached")
        script = self.directory / "cache-test.ps1"
        script.write_text(r'''param([string]$Source,[string]$Parent)
$ErrorActionPreference='Stop'
$errors=$null;$tokens=$null
$tree=[Management.Automation.Language.Parser]::ParseFile($Source,[ref]$tokens,[ref]$errors)
$function=$tree.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Remove-OwnedDownloadCache'},$true)
Invoke-Expression $function.Extent.Text
$parentRoot=$Parent;$CacheRoot=Join-Path $Parent 'download-cache'
Remove-OwnedDownloadCache $false
if(-not (Test-Path -LiteralPath $CacheRoot)){throw 'Explicit external cache was deleted'}
Remove-OwnedDownloadCache $true
if(Test-Path -LiteralPath $CacheRoot){throw 'Owned default cache survived successful cleanup'}
$external=Join-Path $Parent 'other-downloads';[IO.Directory]::CreateDirectory($external)|Out-Null
[IO.File]::WriteAllText((Join-Path $external 'keep'),'preserve')
$CacheRoot=$external
Remove-OwnedDownloadCache $true
if(-not (Test-Path -LiteralPath (Join-Path $external 'keep'))){throw 'Cleanup escaped its default directory'}
''', encoding="utf-8-sig")
        powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        result = subprocess.run([str(powershell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(script), "-Source", str(SOURCE / "tools/Setup-RifeRuntime.ps1"), "-Parent", str(self.directory)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(cache.exists())
        self.assertEqual((self.directory / "other-downloads/keep").read_text(), "preserve")

    def test_failed_download_preserves_old_runtime(self):
        self.old_runtime()
        with patch.object(installer, "download", side_effect=RuntimeError("offline")):
            with self.assertRaisesRegex(RuntimeError, "offline"):
                installer.install(self.root, self.cache, self.directory)
        self.assertEqual((self.root / "existing-worker.txt").read_text(), "working-runtime")
        self.assertFalse(list(self.directory.glob(".runtime.stage-*")))

    def test_insufficient_space_has_no_runtime_side_effect(self):
        self.old_runtime()
        with patch.object(installer.shutil, "disk_usage", return_value=shutil._ntuple_diskusage(100, 99, 1)):
            with self.assertRaisesRegex(RuntimeError, "14 GB"):
                installer.install(self.root, self.cache, self.directory)
        self.assertEqual((self.root / "existing-worker.txt").read_text(), "working-runtime")

    def test_failed_health_check_preserves_old_and_cleans_stage(self):
        self.old_runtime()
        package, unused, unused2 = self.fixture()
        with patch.object(installer, "health", side_effect=RuntimeError("failed inference")):
            with self.assertRaisesRegex(RuntimeError, "failed inference"):
                installer.install(self.root, self.cache, self.directory, package, True)
        self.assertEqual((self.root / "existing-worker.txt").read_text(), "working-runtime")
        self.assertFalse(list(self.directory.glob(".runtime.stage-*")))

    def test_failed_activation_rolls_back_previous_runtime(self):
        self.old_runtime()
        package, unused, unused2 = self.fixture()
        rename = Path.rename
        def refuse_stage(path, target):
            if ".stage-" in path.name:
                raise OSError("activation denied")
            return rename(path, target)
        with patch.object(installer, "health", return_value={"status": "ready", "message": "ready"}), patch.object(Path, "rename", refuse_stage):
            with self.assertRaisesRegex(OSError, "activation denied"):
                installer.install(self.root, self.cache, self.directory, package, True)
        self.assertEqual((self.root / "existing-worker.txt").read_text(), "working-runtime")
        self.assertFalse(list(self.directory.glob(".runtime.stage-*")))

    def test_success_promotes_only_validated_stage(self):
        self.old_runtime()
        package, unused, manifest = self.fixture()
        with patch.object(installer, "health", return_value={"status": "ready", "message": "ready"}):
            result = installer.install(self.root, self.cache, self.directory, package, True)
        self.assertEqual(result["status"], "ready")
        self.assertFalse((self.root / "existing-worker.txt").exists())
        self.assertTrue((self.root / "install-receipt.json").is_file())
        for name, digest in manifest["files"].items():
            self.assertEqual(installer.sha(self.root / "app" / name), digest)

    def test_cli_missing_repair_and_busy_results_without_gpu_or_downloads(self):
        output = self.directory / "status.json"
        check = [sys.executable, "-I", "-B", str(SOURCE / "python/rife/check_runtime.py"), "--root", str(self.root), "--package", str(SOURCE), "--result", str(output)]
        result = subprocess.run(check, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 10, result.stderr)
        self.assertEqual(json.loads(output.read_text())["status"], "missing")
        (self.root / "python").mkdir(parents=True)
        (self.root / "app").mkdir()
        (self.root / "python/python.exe").write_bytes(b"stub")
        (self.root / "app/worker.py").write_text("stub")
        result = subprocess.run(check, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 11, result.stderr)
        self.assertEqual(json.loads(output.read_text())["status"], "repair")
        with RuntimeLease(self.root):
            result = subprocess.run([sys.executable, "-I", "-B", str(SOURCE / "tools/install_rife_runtime.py"), "--root", str(self.root), "--cache", str(self.cache), "--crt-source", str(self.directory), "--result", str(output)], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 13, result.stderr)
        self.assertEqual(json.loads(output.read_text())["status"], "busy")


if __name__ == "__main__":
    unittest.main()
