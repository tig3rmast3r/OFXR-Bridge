"""Focused offline preparation tests. No model, download or GPU is required."""
import contextlib
import importlib.util
import json
import shutil
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tools"))
sys.path.insert(0, str(SOURCE / "python/rife"))
import precompile_rife as tool
from trt_cache import cache_key


class PlanTests(unittest.TestCase):
    def game(self, w=4400, h=4030, eyes=1, luid=123):
        return dict(game="Game.exe", adapter_luid=luid, shapes=[dict(width=w, height=h, eyes=eyes)])

    def test_exact_native_dimensions_all_six_qualities_and_no_extra_eye_batch(self):
        plan = tool.build_plan([self.game()], {123: 0})
        self.assertEqual(len(plan), 6)
        self.assertEqual(plan[0][0], (0, 440, 403, 1))
        self.assertEqual({row[0][3] for row in plan}, {1})
        self.assertTrue(any(row[0][1:3] == (793, 726) for row in plan))  # float32 native ceil(.18)

    def test_shared_eye_quad_and_games_deduplicate_only_identical_shapes(self):
        a = self.game(); b = {**a, "game": "Other.exe"}
        self.assertEqual(len(tool.build_plan([a, b], {123: 0})), 6)
        self.assertEqual(len(tool.build_plan([a, self.game(eyes=2)], {123: 0})), 12)
        quad = {**a, "shapes": a["shapes"] + [dict(width=2000, height=1800, eyes=1)]}
        self.assertEqual(len(tool.build_plan([quad], {123: 0})), 12)

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
            tool.build_plan([self.game()], {456: 1})
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
            results = [RuntimeError("compile failed")] + [("key", True)] * 5
            with patch.object(tool, "cuda_adapters", return_value={123: 0}), patch.object(trt_backend, "Backend", return_value=backend), patch.object(tool, "prepare_one", side_effect=results) as prepare, patch("torch.cuda.set_device"), patch("torch.cuda.empty_cache"):
                self.assertEqual(tool.run(args), 1)
            result = json.loads(args.result.read_text())
            self.assertEqual((result["completed"], result["total"], result["status"]), (6, 6, "error"))
            self.assertEqual(prepare.call_count, 6)
            self.assertEqual(json.loads(args.progress.read_text())["completed"], 6)


if __name__ == "__main__":
    unittest.main()
