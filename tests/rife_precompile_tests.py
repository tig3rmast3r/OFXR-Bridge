"""Focused offline preparation tests. No model, download or GPU is required."""
import contextlib
import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import Mock, patch

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tools"))
sys.path.insert(0, str(SOURCE / "python/rife"))
import precompile_rife as tool
from trt_cache import cache_key


class PublicationTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows PowerShell reader")
    def test_actual_powershell_reader_allows_concurrent_atomic_replacement(self):
        from trt_cache import atomic_json
        with tempfile.TemporaryDirectory(prefix="OFXR concurrent progress spaces ") as temporary:
            folder = Path(temporary); path = folder / "progress.json"
            atomic_json(path, {"completed": 0})
            script = folder / "reader.ps1"
            script.write_text('''param($Source,$Progress,$Ready,$Stop)
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($Source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Production PowerShell script has parse errors'}
$fn=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Read-PrecompileJson'},$true)
Invoke-Expression $fn.Extent.Text
$null=Read-PrecompileJson $Progress
[IO.File]::WriteAllText($Ready,'ready')
$count=0
while(-not [IO.File]::Exists($Stop)) {
 $p=Read-PrecompileJson $Progress
 if($null -eq $p.completed){throw 'Partial JSON read'}
 $count++
}
Write-Output $count
''')
            ready = folder / "ready"; stop = folder / "stop"
            process = subprocess.Popen(["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(script),
                str(SOURCE / "tools/Precompile-Rife.ps1"), str(path), str(ready), str(stop)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 12
                while not ready.exists() and process.poll() is None and time.monotonic() < deadline: time.sleep(.01)
                self.assertTrue(ready.exists(), "isolated reader started")
                for index in range(500):
                    # Exercise both production ends: Windows can still briefly
                    # deny replacement during handle/rename transitions.
                    tool.publish_json(path, {"completed": index + 1})
                    time.sleep(.001)
                stop.touch()
                stdout, stderr = process.communicate(timeout=8)
                self.assertEqual(process.returncode, 0, stderr.decode(errors="replace"))
                self.assertGreater(int(stdout.strip()), 0)
                self.assertEqual(json.loads(path.read_text())["completed"], 500)
            finally:
                stop.touch()
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=5)  # test-owned reader only
                process.communicate(timeout=5)

    @unittest.skipUnless(os.name == "nt", "Windows delete-sharing contract")
    def test_real_locked_destination_retries_then_publishes_without_partial_json(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        with tempfile.TemporaryDirectory(prefix="OFXR locked progress spaces ") as temporary:
            path = Path(temporary) / "progress.json"
            path.write_text('{"completed":15}')
            handle = kernel.CreateFileW(str(path), 0x80000000, 3, None, 3, 0x80, None)
            self.assertNotEqual(handle, wintypes.HANDLE(-1).value)
            held = True
            def release(delay):
                nonlocal held
                self.assertEqual(json.loads(path.read_text()), {"completed": 15})
                self.assertTrue(kernel.CloseHandle(handle)); held = False
            try:
                with patch.object(tool.time, "sleep", side_effect=release) as sleep:
                    tool.publish_json(path, {"completed": 16})
                sleep.assert_called_once_with(.01)
                self.assertEqual(json.loads(path.read_text()), {"completed": 16})
                self.assertEqual(list(Path(temporary).glob("*.tmp")), [])
            finally:
                if held: kernel.CloseHandle(handle)

    def test_persistent_sharing_denial_is_bounded_and_preserves_old_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "result.json"
            path.write_text('{"old":true}')
            error = PermissionError("locked"); error.winerror = 5
            with patch("trt_cache.os.replace", side_effect=error) as replace, patch.object(tool.time, "sleep") as sleep:
                with self.assertRaises(PermissionError): tool.publish_json(path, {"new": True})
            self.assertEqual(replace.call_count, 6)
            self.assertEqual(sleep.call_count, 5)
            self.assertEqual(json.loads(path.read_text()), {"old": True})
            self.assertEqual(list(Path(temporary).glob("*.tmp")), [])

    def test_non_sharing_io_failure_is_not_retried_or_hidden(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch("trt_cache.os.replace", side_effect=OSError("disk full")) as replace, patch.object(tool.time, "sleep") as sleep:
                with self.assertRaisesRegex(OSError, "disk full"):
                    tool.publish_json(Path(temporary) / "result.json", {})
            replace.assert_called_once(); sleep.assert_not_called()


class PlanTests(unittest.TestCase):
    def game(self, w=4400, h=4030, eyes=1, luid=123):
        return dict(game="Game.exe", adapter_luid=luid, shapes=[dict(width=w, height=h, eyes=eyes)])

    def test_isolated_cli_imports_sibling_helper_before_reporting_preflight(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            command = [sys.executable, "-I", "-B", str(SOURCE / "tools/precompile_rife.py")]
            for key in ("root", "result", "progress", "cancel", "log"):
                command += ["--" + key, str(folder / key)]
            command += ["--games", str(folder / "empty-games")]
            done = subprocess.run(command, capture_output=True, timeout=10)
            self.assertEqual(done.returncode, 1)
            result = json.loads((folder / "result").read_text())
            self.assertIn("No recorded game resolutions", result["message"])
            self.assertNotIn(b"ModuleNotFoundError", done.stderr)

    def test_exact_native_dimensions_all_seven_qualities_and_no_extra_eye_batch(self):
        plan = tool.build_plan([self.game()], {123: 0})
        self.assertEqual(len(plan), 7)
        self.assertEqual(plan[0][0], (0, 440, 403, 1))
        self.assertEqual({row[0][3] for row in plan}, {1})
        self.assertTrue(any(row[0][1:3] == (793, 726) for row in plan))  # float32 native ceil(.18)

    def test_shared_eye_quad_and_games_deduplicate_only_identical_shapes(self):
        a = self.game(); b = {**a, "game": "Other.exe"}
        self.assertEqual(len(tool.build_plan([a, b], {123: 0})), 7)
        self.assertEqual(len(tool.build_plan([a, self.game(eyes=2)], {123: 0})), 14)
        quad = {**a, "shapes": a["shapes"] + [dict(width=2000, height=1800, eyes=1)]}
        self.assertEqual(len(tool.build_plan([quad], {123: 0})), 14)

    def test_latest_file_only_and_malformed_input_is_not_silently_accepted(self):
        with tempfile.TemporaryDirectory(prefix="OFXR games spaces ") as temporary:
            file = Path(temporary) / "game.json"
            record = self.game();record["schema"] = 1
            file.write_text(json.dumps(record)); games, errors = tool.read_games(temporary)
            self.assertEqual((len(games), errors), (1, []))
            record["shapes"] = [dict(width=3000, height=2800, eyes=2)]
            file.write_text(json.dumps(record));games, errors = tool.read_games(temporary)
            self.assertEqual(games[0]["shapes"], record["shapes"])
            record["shapes"][0]["eyes"] = 4
            file.write_text(json.dumps(record));games, errors = tool.read_games(temporary)
            self.assertEqual(games, []);self.assertEqual(len(errors), 1)

    def test_missing_game_adapter_is_explicit_and_unknown_adapter_uses_default(self):
        with self.assertRaisesRegex(ValueError, "not available"):
            tool.build_plan([self.game()], {456: 1, 789: 2})
        self.assertEqual(tool.build_plan([self.game()], {456: 1})[0][0][0], 1)
        self.assertEqual(tool.build_plan([self.game(luid=0)], {456: 1})[0][0][0], 1)

    def test_compile_explicitly_retries_failed_cache_then_checks_actual_inference(self):
        import trt_backend
        backend = Mock(runtime_root=Path("runtime"), device=0)
        backend.directory = Path("cache");backend._spec.return_value = {"shape": "fixture"}
        backend._job = None
        def start(*unused):backend._job = {"running": True}
        def poll():backend._job = None
        backend._start.side_effect = start;backend._poll.side_effect = poll
        output = types.SimpleNamespace(shape=(1, 5, 403, 440))
        engine = Mock();engine.infer.return_value = output
        torch = Mock();torch.inference_mode.return_value = contextlib.nullcontext()
        torch.isfinite.return_value.all.return_value.item.return_value = True
        with patch("trt_cache.validate_entry", side_effect=[None, Path("engine")]), patch.object(trt_backend, "Engine", return_value=engine), patch.dict(sys.modules, {"torch": torch}):
            key, cached = tool.prepare_one(backend, (0, 440, 403, 1), lambda: False, lambda unused: None)
        self.assertFalse(cached);backend._failed.assert_not_called()
        backend._start.assert_called_once();engine.infer.assert_called_once()

    def test_cancellation_stops_only_own_builder(self):
        backend = Mock(directory=Path("cache"));backend._spec.return_value = {"shape": "fixture"}
        backend._job = {"running": True}
        with patch("trt_cache.validate_entry", return_value=None):
            with self.assertRaises(InterruptedError):
                tool.prepare_one(backend, (0, 440, 403, 1), lambda: True)
        backend.close.assert_called_once()

    def test_valid_cache_never_recompiled(self):
        import trt_backend
        backend = Mock(directory=Path("cache"), runtime_root=Path("runtime"), device=0)
        backend._spec.return_value = {"shape": "fixture"}
        engine = Mock();engine.infer.return_value = types.SimpleNamespace(shape=(2, 5, 32, 32))
        torch = Mock();torch.inference_mode.return_value = contextlib.nullcontext()
        torch.isfinite.return_value.all.return_value.item.return_value = True
        with patch("trt_cache.validate_entry", return_value=Path("engine")), patch.object(trt_backend, "Engine", return_value=engine), patch.dict(sys.modules, {"torch": torch}):
            _, cached = tool.prepare_one(backend, (0, 32, 32, 2), lambda: False)
        self.assertTrue(cached);backend._start.assert_not_called()

    def test_progress_finishes_all_combinations_and_reports_failures(self):
        import trt_backend
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "runtime"
            for name in ("trt_compile.py", "models/IFNet_HDv3_v4_4.py", "models/warplayer.py"):
                dest = root / "app" / name;dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(SOURCE / "python/rife" / name, dest)
            games = root.parent / "games";games.mkdir()
            (games / "game.json").write_text(json.dumps({**self.game(), "schema": 1}))
            args = types.SimpleNamespace(root=root, games=games, cache=root.parent / "cache",
                result=root.parent / "result.json", progress=root.parent / "progress.json",
                cancel=root.parent / "cancel", log=root.parent / "run.log")
            backend = Mock();backend._initialize.return_value = True
            results = [RuntimeError("compile failed")] + [("key", True)] * 6
            with patch("update_rife_overlay.sync_locked", return_value=False), patch.object(tool, "cuda_adapters", return_value={123: 0}), patch.object(trt_backend, "Backend", return_value=backend) as factory, patch.object(tool, "prepare_one", side_effect=results) as prepare, patch("torch.cuda.set_device"), patch("torch.cuda.empty_cache"):
                self.assertEqual(tool.run(args), 1)
            for call in factory.call_args_list:
                self.assertEqual(call.args[1], root / "app", "cache identity must use installed worker bytes, not release checkout bytes")
            result = json.loads(args.result.read_text())
            self.assertEqual((result["completed"], result["total"], result["status"]), (7, 7, "error"))
            self.assertEqual(prepare.call_count, 7)
            self.assertEqual(json.loads(args.progress.read_text())["completed"], 7)

    def test_global_source_problem_is_one_preflight_error_not_seven_failed_engines(self):
        from update_rife_overlay import OverlayRepairRequired
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary);games=folder/"games";games.mkdir()
            (games/"game.json").write_text(json.dumps({**self.game(), "schema":1}))
            args=types.SimpleNamespace(root=folder/"runtime",games=games,cache=folder/"cache",
                result=folder/"result.json",progress=folder/"progress.json",cancel=folder/"cancel",log=folder/"log")
            with patch("update_rife_overlay.sync_locked", side_effect=OverlayRepairRequired("dependency versions differ")), patch.object(tool,"prepare_one") as compile_engine:
                self.assertEqual(tool.run(args),11)
            compile_engine.assert_not_called()
            result=json.loads(args.result.read_text())
            self.assertEqual((result["completed"],result["total"],result["status"]),(0,0,"repair"))
            self.assertIn("dependency versions differ",result["message"])

    def test_unwritable_progress_does_not_abort_batch_or_hide_compile_failure(self):
        import trt_backend
        for fail_engine in (False, True):
            with self.subTest(fail_engine=fail_engine), tempfile.TemporaryDirectory() as temporary:
                folder = Path(temporary); games = folder / "games"; games.mkdir()
                (games / "game.json").write_text(json.dumps({**self.game(), "schema": 1}))
                args = types.SimpleNamespace(root=folder / "runtime", games=games, cache=folder / "cache",
                    result=folder / "result.json", progress=folder / "progress.json", cancel=folder / "cancel", log=folder / "log")
                backend = Mock(); backend._initialize.return_value = True
                original = tool.publish_json
                def publish(path, value):
                    if path == args.progress: raise PermissionError("progress locked")
                    return original(path, value)
                outcomes = ([RuntimeError("actual compile failure")] if fail_engine else [("key", True)]) + [("key", True)] * 6
                with patch("update_rife_overlay.sync_locked", return_value=False), patch.object(tool, "cuda_adapters", return_value={123: 0}), patch.object(trt_backend, "Backend", return_value=backend), patch.object(tool, "prepare_one", side_effect=outcomes) as prepare, patch.object(tool, "publish_json", side_effect=publish), patch("torch.cuda.set_device"), patch("torch.cuda.empty_cache"):
                    self.assertEqual(tool.run(args), 1 if fail_engine else 0)
                result = json.loads(args.result.read_text())
                self.assertEqual((result["completed"], result["total"], result["status"]), (7, 7, "error" if fail_engine else "ready"))
                self.assertEqual(prepare.call_count, 7)
                self.assertEqual(args.log.read_text().count("Progress update unavailable"), 1)
                if fail_engine: self.assertIn("actual compile failure", args.log.read_text())

    def test_unwritable_progress_preserves_cancellation_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary); games = folder / "games"; games.mkdir()
            (games / "game.json").write_text(json.dumps({**self.game(), "schema": 1}))
            args = types.SimpleNamespace(root=folder / "runtime", games=games, cache=folder / "cache",
                result=folder / "result.json", progress=folder / "progress.json", cancel=folder / "cancel", log=folder / "log")
            args.cancel.touch()
            original = tool.publish_json
            def publish(path, value):
                if path == args.progress: raise PermissionError("progress locked")
                return original(path, value)
            with patch("update_rife_overlay.sync_locked", return_value=False), patch.object(tool, "cuda_adapters", return_value={123: 0}), patch.object(tool, "publish_json", side_effect=publish):
                self.assertEqual(tool.run(args), 1)
            self.assertIn("cancelled", json.loads(args.result.read_text())["message"])


if __name__ == "__main__":
    unittest.main()
