"""CPU-only cache/lifecycle/protocol tests; no CUDA device is initialized."""
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import types
import unittest
import queue
from unittest.mock import Mock, patch

APP = Path(__file__).resolve().parents[1] / "python" / "rife"
sys.dont_write_bytecode = True
sys.path.insert(0, str(APP))
from trt_cache import SCHEMA, atomic_json, cache_key, file_sha256, validate_entry
from trt_backend import Backend, Engine


class Tensor:
    def __init__(self, shape):
        self.shape = shape
        self.copies = []

    def __getitem__(self, item):
        if isinstance(item, slice):
            return Tensor((item.stop - item.start, *self.shape[1:]))
        raise AssertionError(item)

    def copy_(self, other):
        self.copies.append(other.shape)
        return self


def spec():
    return {"schema": SCHEMA, "model": "4.4", "eyes": 2, "width": 555, "height": 511,
            "padded_width": 576, "padded_height": 512, "gpu": "test", "opset": 19}


def entry(directory, value):
    key = cache_key(value)
    path = directory / (key + ".engine")
    path.write_bytes(b"test engine bytes")
    atomic_json(directory / (key + ".json"), {"schema": SCHEMA, "key": key, "spec": value,
                "engine_bytes": path.stat().st_size, "engine_sha256": file_sha256(path)})
    return key, path


class CacheTests(unittest.TestCase):
    def test_actual_dimensions_and_eyes_are_keyed(self):
        value = spec()
        self.assertNotEqual(cache_key(value), cache_key({**value, "height": 510}))
        self.assertNotEqual(cache_key(value), cache_key({**value, "eyes": 1}))
        self.assertNotEqual(cache_key(value), cache_key({**value, "opset": 20}))

    def test_atomic_valid_and_corrupt_entries(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            key, path = entry(directory, spec())
            self.assertEqual(validate_entry(directory, key, spec()), path)
            path.write_bytes(b"different contents")
            self.assertIsNone(validate_entry(directory, key, spec()))
            self.assertFalse(list(directory.glob("*.tmp")))

    def test_engine_mutates_graph_inputs_inside_inference_mode(self):
        import torch
        engine = object.__new__(Engine)
        with torch.inference_mode():
            engine.x = torch.zeros((1, 3, 2, 2), dtype=torch.float16)
            engine.y = torch.zeros_like(engine.x)
            engine.timestep = torch.full((1, 1, 1, 1), .5, dtype=torch.float16)
            engine.output = torch.zeros((1, 5, 2, 2), dtype=torch.float16)
        engine.graph = Mock()
        source = torch.ones((1, 3, 2, 2), dtype=torch.float16)
        self.assertFalse(torch.is_inference_mode_enabled())
        self.assertIs(engine.infer(source, source, .25), engine.output)
        self.assertFalse(torch.is_inference_mode_enabled())
        self.assertEqual(float(engine.timestep.item()), .25)
        self.assertTrue(torch.equal(engine.x, source))
        engine.graph.replay.assert_called_once()

    def test_wrong_metadata_and_partial_write_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            key, path = entry(directory, spec())
            self.assertIsNone(validate_entry(directory, key, {**spec(), "height": 510}))
            (directory / (key + ".json")).write_text("{")
            self.assertIsNone(validate_entry(directory, key, spec()))


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.backend = Backend(self.directory, self.directory, self.directory)
        self.backend.identity = {"gpu": "test", "model": "4.4", "opset": 19}
        self.backend.SETTLE_SECONDS = 0
        self.a = Tensor((2, 3, 511, 555))

    def tearDown(self):
        self.backend.close()
        self.temporary.cleanup()

    def process(self, code=None):
        process = Mock()
        process.pid = 123
        process.stdin = io.BytesIO()
        process.poll.return_value = code
        return process

    def test_missing_optional_backend_is_sticky_torch_fallback(self):
        self.backend.identity = None
        with patch("trt_backend.load_backend", side_effect=RuntimeError("unavailable")) as loader:
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, False))
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, False))
            self.assertEqual(loader.call_count, 1)

    def test_cold_request_starts_one_nonblocking_child(self):
        process = self.process()
        with patch("trt_backend.subprocess.Popen", return_value=process) as popen:
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, True))
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, True))
            self.assertEqual(popen.call_count, 1)
            self.assertIn("--parent-watchdog", popen.call_args.args[0])
            self.assertEqual(popen.call_args.kwargs["stdin"], subprocess.PIPE)

    def test_compilation_timeout_and_backoff(self):
        process = self.process()
        with patch("trt_backend.subprocess.Popen", return_value=process) as popen:
            self.backend.try_infer(self.a, self.a, .5)
            self.backend._job["started"] -= self.backend.BUILD_TIMEOUT + 1
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, False))
            self.assertIn("timed out", self.backend.note)
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, False))
            self.assertEqual(popen.call_count, 1)
            process.terminate.assert_called_once()

    def test_slow_build_survives_old_deadline_and_keeps_single_child(self):
        process = self.process()
        with patch("trt_backend.subprocess.Popen", return_value=process) as popen:
            self.backend.try_infer(self.a, self.a, .5)
            self.backend._job["started"] -= 181
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, True))
            self.assertEqual(popen.call_count, 1)
            process.terminate.assert_not_called()

    def test_success_without_valid_cache_does_not_recompile_each_frame(self):
        process = self.process()
        with patch("trt_backend.subprocess.Popen", return_value=process) as popen:
            self.backend.try_infer(self.a, self.a, .5)
            process.poll.return_value = 0
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, False))
            self.assertIn("invalid cache", self.backend.note)
            self.backend.try_infer(self.a, self.a, .5)
            self.assertEqual(popen.call_count, 1)

    def test_cache_hit_reuses_engine_without_disk_reads_each_frame(self):
        value = self.backend._spec(self.a)
        key, path = entry(self.directory, value)
        generated = Tensor((2, 5, 511, 555))
        engine = Mock()
        engine.infer.return_value = generated
        with patch("trt_backend.Engine", return_value=engine) as factory, patch("trt_backend.validate_entry", wraps=validate_entry) as validator:
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (generated, False))
            self.assertEqual(self.backend.try_infer(self.a, self.a, .25), (generated, False))
            self.assertEqual(factory.call_count, 1)
            self.assertEqual(validator.call_count, 1)
            self.assertEqual(engine.infer.call_count, 2)

    def test_bad_engine_load_falls_back_and_does_not_retry(self):
        entry(self.directory, self.backend._spec(self.a))
        with patch("trt_backend.Engine", side_effect=RuntimeError("bad engine")) as factory:
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, False))
            self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, False))
            self.assertEqual(factory.call_count, 1)

    def test_runtime_engine_error_falls_back(self):
        key = cache_key(self.backend._spec(self.a))
        self.backend._engine_key = key
        self.backend._engine = Mock()
        self.backend._engine.infer.side_effect = RuntimeError("failed enqueue")
        self.assertEqual(self.backend.try_infer(self.a, self.a, .5), (None, False))
        self.assertIsNone(self.backend._engine)
        self.assertIn("execution failed", self.backend.note)

    def test_size_change_drops_unused_engine_and_keeps_one_builder(self):
        self.backend._engine_key = "old"
        self.backend._engine = Mock()
        process = self.process()
        with patch("trt_backend.subprocess.Popen", return_value=process) as popen:
            self.backend.try_infer(self.a, self.a, .5)
            self.assertIsNone(self.backend._engine)
            self.backend.try_infer(Tensor((2, 3, 512, 555)), Tensor((2, 3, 512, 555)), .5)
            self.assertEqual(popen.call_count, 1)

    def test_suspend_cancels_child_and_cleans_only_its_temporaries(self):
        process = self.process()
        with patch("trt_backend.subprocess.Popen", return_value=process):
            self.backend.try_infer(self.a, self.a, .5)
        key = self.backend._job["key"]
        own = self.directory / (key + ".123.random.onnx.tmp")
        other = self.directory / (key + ".456.random.onnx.tmp")
        own.write_text("own")
        other.write_text("other")
        self.backend.suspend()
        self.assertFalse(own.exists())
        self.assertTrue(other.exists())
        process.terminate.assert_called_once()
        self.assertIsNone(self.backend._job)

    def test_invalid_fraction_does_not_poison_cache(self):
        with self.assertRaises(ValueError):
            self.backend.try_infer(self.a, self.a, 1.1)
        self.assertFalse(self.backend._failures)


class WorkerProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import worker
        cls.worker = worker

    def setUp(self):
        self.runner = self.worker.Runner()
        self.runner.select_model = Mock()
        self.runner.configs[1] = [types.SimpleNamespace(tensor=Tensor((2, channels, 511, 555))) for channels in (3, 3, 5)]
        self.runner.infer_cached = Mock(side_effect=lambda a, b, fraction: Tensor((a.shape[0], 5, 511, 555)))

    def test_legacy_request_does_not_initialize_accelerator_and_is_serial(self):
        self.assertFalse(self.runner.optimization_enabled)
        self.assertIsNone(self.runner.trt)
        with patch.object(self.worker.torch.cuda, "current_stream", return_value=Mock()):
            result = self.runner.command({"op": "infer", "id": 1, "model": 3})
        self.assertEqual(result, {})
        self.assertEqual(self.runner.infer_cached.call_count, 2)
        self.assertEqual(self.runner.infer_cached.call_args_list[0].args[0].shape[0], 1)
        self.runner.select_model.assert_any_call(3, load=True)

    def test_optimized_fallback_batches_both_eyes_and_reports_progress(self):
        self.runner.optimization_enabled = True
        self.runner.trt = Mock(note="")
        self.runner.trt.try_infer.return_value = (None, True)
        with patch.object(self.worker.torch.cuda, "current_stream", return_value=Mock()):
            result = self.runner.command({"op": "infer", "id": 1, "model": 3, "fraction": 1/3})
        self.assertEqual(result, {"optimizing": True, "backend": "torch"})
        self.assertEqual(self.runner.infer_cached.call_count, 1)
        self.assertEqual(self.runner.infer_cached.call_args.args[0].shape[0], 2)

    def test_optimized_cache_hit_reports_exact_backend(self):
        self.runner.optimization_enabled = True
        self.runner.trt = Mock(note="")
        self.runner.trt.try_infer.return_value = (Tensor((2, 5, 511, 555)), False)
        with patch.object(self.worker.torch.cuda, "current_stream", return_value=Mock()):
            result = self.runner.command({"op": "infer", "id": 1, "model": 3})
        self.assertEqual(result, {"optimizing": False, "backend": "tensorrt"})
        self.assertFalse(self.runner.infer_cached.called)

    def test_optimization_failure_keeps_generation_and_reports_exact_cause(self):
        self.runner.optimization_enabled = True
        self.runner.trt = Mock(note="TensorRT optimization timed out")
        self.runner.trt.try_infer.return_value = (None, False)
        with patch.object(self.worker.torch.cuda, "current_stream", return_value=Mock()):
            result = self.runner.command({"op": "infer", "id": 1, "model": 3})
        self.assertEqual(result, {"optimizing": False, "backend": "torch",
                                 "optimization_error": "TensorRT optimization timed out"})

    def test_other_models_do_not_optimize(self):
        self.runner.optimization_enabled = True
        self.runner.trt = Mock(note="old failure")
        with patch.object(self.worker.torch.cuda, "current_stream", return_value=Mock()):
            result = self.runner.command({"op": "infer", "id": 1, "model": 2})
        self.assertEqual(result, {"optimizing": False, "backend": "torch"})
        self.assertFalse(self.runner.trt.try_infer.called)
        self.assertEqual(self.runner.infer_cached.call_count, 2)

    def configure(self, optimize=None):
        request = {"op": "configure", "id": 9, "luid": 0, "eyes": 2,
                   "height": 511, "width": 555, "buffers": [[0, 0], [0, 0], [0, 0]]}
        if optimize is not None:
            request["optimize"] = optimize
        return request

    def test_configure_without_capability_keeps_legacy_and_never_creates_backend(self):
        self.runner.configs.clear()
        self.runner.select_device = Mock()
        with patch.object(self.worker, "SharedBuffer", return_value=Mock()), patch.object(self.worker, "Backend") as factory:
            self.runner.command(self.configure())
        self.assertFalse(self.runner.optimization_enabled)
        self.assertIsNone(self.runner.trt)
        self.assertFalse(factory.called)

    def test_configure_capability_is_explicit_and_uses_app_runtime_layout(self):
        self.runner.configs.clear()
        self.runner.select_device = Mock()
        with patch.object(self.worker, "SharedBuffer", return_value=Mock()), patch.object(self.worker, "Backend") as factory:
            self.runner.command(self.configure(True))
        self.assertTrue(self.runner.optimization_enabled)
        factory.assert_called_once_with(self.worker.BACKEND_ROOT, self.worker.APP_ROOT)

    def test_mixed_capability_rejected_before_touching_existing_buffers(self):
        with self.assertRaisesRegex(ValueError, "Mixed"):
            self.runner.command(self.configure(True))
        self.assertIn(1, self.runner.configs)

    def test_deadline_records_error_and_forces_process_exit(self):
        from trt_compile import deadline
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "result.json"
            with patch("trt_compile.time.sleep") as sleep, patch("trt_compile.os._exit") as exit_process:
                deadline(path)
            sleep.assert_called_once_with(Backend.BUILD_TIMEOUT)
            exit_process.assert_called_once_with(124)
            self.assertIn("timed out", json.loads(path.read_text())["error"])


class CompilerOwnershipTests(unittest.TestCase):
    def test_parent_pipe_eof_exits_child_without_gpu_imports(self):
        code = "import sys;sys.path.insert(0," + repr(str(APP)) + ");from trt_compile import watch_parent;watch_parent()"
        child = subprocess.Popen([sys.executable, "-B", "-c", code], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
        child.stdin.close()
        self.assertEqual(child.wait(timeout=5), 125)
        child.stdout.close()
        child.stderr.close()

    def test_parent_watchdog_does_not_deadlock_numpy_import_with_open_stdin(self):
        code = ("import sys,threading;sys.path.insert(0," + repr(str(APP)) + ");"
                "from trt_compile import watch_parent;"
                "threading.Thread(target=watch_parent,daemon=True).start();"
                "import numpy;print('NUMPY_READY',flush=True)")
        child = subprocess.Popen([sys.executable, "-B", "-u", "-c", code], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
        result = queue.Queue()
        threading.Thread(target=lambda: result.put(child.stdout.readline()), daemon=True).start()
        try:
            self.assertEqual(result.get(timeout=10).strip(), b"NUMPY_READY")
        finally:
            child.stdin.close()
            child.wait(timeout=5)
            child.stdout.close()
            child.stderr.close()

    @unittest.skipUnless(sys.platform == "win32", "production cache locking uses Windows msvcrt")
    def test_builder_lock_serializes_separate_processes(self):
        from trt_compile import builder_lock
        identity = {"gpu": "test", "gpu_uuid": "test-uuid", "compute_capability": [8, 9]}
        with tempfile.TemporaryDirectory() as temporary:
            script = ("import sys;sys.path.insert(0," + repr(str(APP)) + ");"
                      "from pathlib import Path;from trt_compile import builder_lock;"
                      "context=builder_lock(Path(" + repr(temporary) + ")," + repr(identity) + ",timeout=.01);"
                      "context.__enter__();print('ACQUIRED',flush=True);context.__exit__(None,None,None)")
            with builder_lock(Path(temporary), identity):
                blocked = subprocess.run([sys.executable, "-B", "-c", script], capture_output=True, timeout=5)
                self.assertNotEqual(blocked.returncode, 0)
                self.assertIn(b"builder is busy", blocked.stderr)
            acquired = subprocess.run([sys.executable, "-B", "-c", script], capture_output=True, timeout=5)
            self.assertEqual(acquired.returncode, 0, acquired.stderr)
            self.assertIn(b"ACQUIRED", acquired.stdout)


if __name__ == "__main__":
    unittest.main()
