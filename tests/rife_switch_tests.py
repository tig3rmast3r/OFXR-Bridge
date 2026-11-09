"""CPU-only real PowerShell switch and installer-entry fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]


class SwitchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="OFXR RIFE switch spaces ")
        self.root = Path(self.temp.name)
        self.local = self.root / "Local AppData"
        self.settings = self.local / "ofxr-bridge/ofxr-bridge.ini"
        self.settings.parent.mkdir(parents=True)
        self.nvidia = self.settings.parent / "RIFE/runtime"
        self.vulkan = self.root / "repo Vulkan"
        for runtime in (self.nvidia, self.vulkan):
            (runtime / "python").mkdir(parents=True)
            (runtime / "python/python.exe").write_bytes(b"fixture")
            (runtime / "app").mkdir()
            (runtime / "app/worker.py").write_text("# fixture")
        (self.vulkan / "vulkan").mkdir()
        (self.vulkan / "vulkan/backend.json").write_text("{}")
        (self.vulkan / "vulkan/ofxr_rife_vulkan.exe").write_bytes(b"fixture")
        self.original = b"[ofxr]\nquality=18\n[tray]\ncustom=keep\n[unrelated]\nvalue=untouched\n"
        self.settings.write_bytes(self.original)

    def tearDown(self):
        self.temp.cleanup()

    def invoke(self, backend, runtime=None, ok=True):
        args = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SOURCE / "tools/Select-RifeRuntime.ps1"),
                "-Backend", backend, "-SettingsPath", str(self.settings), "-ResultJson", str(self.root / "result.json"), "-AllowRunningTray"]
        if runtime is not None:
            args += ["-RuntimeDir", str(runtime)]
        result = subprocess.run(args, env={**os.environ, "LOCALAPPDATA": str(self.local)}, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def test_switch_and_restore_preserve_every_other_setting(self):
        self.invoke("vulkan", self.vulkan)
        result = json.loads((self.root / "result.json").read_text())
        self.assertEqual(Path(result["backup"]).read_bytes(), self.original)
        self.assertIn("rife_runtime_dir=" + self.vulkan.as_posix(), self.settings.read_text())
        self.invoke("nvidia")
        self.assertEqual(self.settings.read_bytes(), self.original)
        self.assertTrue(self.nvidia.is_dir())
        self.assertTrue(self.vulkan.is_dir())

    def test_wrong_backend_does_not_change_settings(self):
        self.invoke("nvidia", self.vulkan, ok=False)
        self.assertEqual(self.settings.read_bytes(), self.original)

    def test_duplicate_tray_sections_are_rejected(self):
        before = self.original + b"[tray]\nextra=keep\n"
        self.settings.write_bytes(before)
        self.invoke("vulkan", self.vulkan, ok=False)
        self.assertEqual(self.settings.read_bytes(), before)

    def test_repeated_switch_has_one_override_only(self):
        self.invoke("vulkan", self.vulkan)
        self.invoke("vulkan", self.vulkan)
        self.assertEqual(self.settings.read_text().count("rife_runtime_dir="), 1)

    def test_entry_point_explicitly_selects_vulkan(self):
        tools = self.root / "installer/tools"
        tools.mkdir(parents=True)
        shutil.copy2(SOURCE / "tools/Install-RifeVulkan.ps1", tools)
        (tools / "Setup-RifeRuntime.ps1").write_text('''param($Backend,$Root,$ResultJson,$CacheRoot,$CrtSource)
[IO.File]::WriteAllText($ResultJson,(@{backend=$Backend;root=$Root}|ConvertTo-Json))
exit 0
''')
        result = self.root / "entry-result.json"
        done = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(tools / "Install-RifeVulkan.ps1"),
                               "-Root", str(self.vulkan), "-ResultJson", str(result)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(json.loads(result.read_text())["backend"], "vulkan")


if __name__ == "__main__":
    unittest.main()
