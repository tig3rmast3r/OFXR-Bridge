"""Optional RIFE4.4 TensorRT backend; asynchronous one-at-a-time compilation.

The protocol worker remains responsive and uses its normal Torch CUDA Graph
while a dedicated child prepares a static engine. No model/image CPU transfers
occur in the hot inference path. Compilers never print to protocol stdout.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import torch

from trt_cache import ADAPTER_VERSION, SCHEMA, atomic_json, cache_directory, cache_key, file_sha256, validate_entry

_dll_directories = []
_trt = None
_logger = None


def load_backend(runtime_root):
    global _trt
    if _trt is not None:
        return _trt
    dependencies = Path(runtime_root) / "backends" / "tensorrt"
    if not dependencies.is_dir():
        raise RuntimeError("TensorRT backend is not installed")
    if str(dependencies) not in sys.path:
        sys.path.insert(0, str(dependencies))
    if os.name == "nt":
        for path in (dependencies / "tensorrt_libs", dependencies / "tensorrt_cu12_libs",
                     Path(runtime_root) / "python" / "Lib" / "site-packages" / "torch" / "lib"):
            if path.is_dir():
                _dll_directories.append(os.add_dll_directory(str(path)))
    import tensorrt
    _trt = tensorrt
    return _trt


def backend_logger(trt):
    global _logger
    if _logger is None:
        class Logger(trt.ILogger):
            def __init__(self):
                super().__init__()

            def log(self, severity, message):
                if severity <= trt.Logger.WARNING:
                    print("RIFE TensorRT: " + message, file=sys.stderr, flush=True)
        # TensorRT may retain the first logger across multiple runtimes. Keep
        # one instance alive for the entire worker, including shape switches.
        _logger = Logger()
    return _logger


class Engine:
    """One fixed-shape batched engine and one private CUDA Graph."""
    @torch.inference_mode()
    def __init__(self, path, spec, runtime_root):
        trt = load_backend(runtime_root)

        self.logger = backend_logger(trt)
        self.runtime = trt.Runtime(self.logger)
        self.engine = self.runtime.deserialize_cuda_engine(Path(path).read_bytes())
        if self.engine is None:
            raise RuntimeError("TensorRT rejected the cached engine")
        expected = {"a": (spec["eyes"], 3, spec["height"], spec["width"]),
                    "b": (spec["eyes"], 3, spec["height"], spec["width"]),
                    "timestep": (1, 1, 1, 1),
                    "flow_mask": (spec["eyes"], 5, spec["height"], spec["width"])}
        if self.engine.num_io_tensors != 4:
            raise RuntimeError("Unexpected TensorRT engine interface")
        for name, shape in expected.items():
            if tuple(self.engine.get_tensor_shape(name)) != shape or self.engine.get_tensor_dtype(name) != trt.DataType.HALF:
                raise RuntimeError("Cached TensorRT tensor contract mismatch: " + name)
        self.context = self.engine.create_execution_context()
        if self.context is None:
            raise RuntimeError("TensorRT could not create an execution context")
        self.x = torch.zeros(expected["a"], device="cuda", dtype=torch.float16)
        self.y = torch.zeros_like(self.x)
        self.timestep = torch.full(expected["timestep"], .5, device="cuda", dtype=torch.float16)
        self.output = torch.empty(expected["flow_mask"], device="cuda", dtype=torch.float16)
        for name, tensor in (("a", self.x), ("b", self.y), ("timestep", self.timestep), ("flow_mask", self.output)):
            if not self.context.set_tensor_address(name, tensor.data_ptr()):
                raise RuntimeError("TensorRT rejected an input/output address")
        # Warm on an explicit stream; capture only after all allocation/initial
        # execution is complete. This does not wait for an engine compilation.
        stream = torch.cuda.Stream()
        stream.wait_stream(torch.cuda.current_stream())
        with torch.cuda.stream(stream):
            for _ in range(3):
                if not self.context.execute_async_v3(stream.cuda_stream):
                    raise RuntimeError("TensorRT warm-up failed")
        torch.cuda.current_stream().wait_stream(stream)
        stream.synchronize()
        self.graph = torch.cuda.CUDAGraph()
        with torch.cuda.graph(self.graph, stream=stream):
            if not self.context.execute_async_v3(stream.cuda_stream):
                raise RuntimeError("TensorRT CUDA Graph capture failed")

    @torch.inference_mode()
    def infer(self, a, b, fraction):
        if not .05 <= fraction <= .95:
            raise ValueError("RIFE interpolation fraction is outside the pair")
        self.x.copy_(a)
        self.y.copy_(b)
        self.timestep.fill_(fraction)
        self.graph.replay()
        return self.output


class Backend:
    BUILD_TIMEOUT = 180.0
    FAILURE_BACKOFF = 3600.0
    SETTLE_SECONDS = .5

    def __init__(self, runtime_root, app_root=None, directory=None):
        self.runtime_root = Path(runtime_root)
        self.app_root = Path(app_root or Path(__file__).parent)
        self.directory = Path(directory) if directory else cache_directory()
        self.device = None
        self.identity = None
        self.unavailable = None
        self._model_identity = None
        self._specs = {}
        self._failures = {}
        self._engine_key = None
        self._engine = None
        self._job = None
        self._pending_key = None
        self._pending_since = 0.0
        self._validated_missing = set()
        self.note = ""

    def set_device(self, index):
        self.device = index

    def _initialize(self):
        if self.identity is not None:
            return True
        if self.unavailable is not None:
            return False
        try:
            import torch
            trt = load_backend(self.runtime_root)
            properties = torch.cuda.get_device_properties(self.device)
            model = self.runtime_root / "models" / "flownet_v4.4.pkl"
            self._model_identity = {"weights": file_sha256(model),
                                    "network": file_sha256(self.app_root / "models" / "IFNet_HDv3_v4_4.py"),
                                    "warp": file_sha256(self.app_root / "models" / "warplayer.py"),
                                    "compiler": file_sha256(self.app_root / "trt_compile.py")}
            self.identity = {"schema": SCHEMA, "adapter": ADAPTER_VERSION, "model": "4.4",
                             "model_sha256": self._model_identity, "trt": trt.__version__,
                             "cuda": torch.version.cuda, "gpu": properties.name,
                             "gpu_uuid": str(getattr(properties, "uuid", "")),
                             "compute_capability": [properties.major, properties.minor],
                             "opset": 19, "precision": "fp16-fp32-coordinates", "tf32": False}
            self.directory.mkdir(parents=True, exist_ok=True)
            return True
        except Exception as error:
            self.unavailable = str(error)
            self.note = self.unavailable
            return False

    def _spec(self, a):
        eyes, channels, height, width = map(int, a.shape)
        if eyes not in (1, 2) or channels != 3:
            raise ValueError("TensorRT supports one or two RGB eyes")
        return {**self.identity, "eyes": eyes, "width": width, "height": height,
                "padded_width": (width + 31) // 32 * 32,
                "padded_height": (height + 31) // 32 * 32}

    def _failed(self, key):
        until = self._failures.get(key)
        if until is not None:
            return time.time() < until
        path = self.directory / (key + ".failed.json")
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            until = float(data["retry_after"])
            self._failures[key] = until
            if time.time() < until:
                self.note = str(data.get("reason", "TensorRT optimization is temporarily unavailable"))
            return time.time() < until
        except (OSError, ValueError, KeyError, TypeError):
            return False

    def _failure(self, key, reason):
        until = time.time() + self.FAILURE_BACKOFF
        self._failures[key] = until
        self.note = str(reason)
        try:
            atomic_json(self.directory / (key + ".failed.json"),
                        {"key": key, "reason": self.note, "retry_after": until})
        except OSError:
            pass

    def _stop_job(self):
        if self._job is None:
            return
        job = self._job
        self._job = None
        process = job["process"]
        try:
            if process.stdin:
                process.stdin.close()
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=1)
        except OSError:
            # Closing stdin can make the parent-watchdog terminate the child
            # between poll() and terminate(). It is already safely stopped.
            if process.poll() is None:
                process.kill()
                process.wait(timeout=1)
        finally:
            job["log"].close()
            self._cleanup_job_files(job)

    def _cleanup_job_files(self, job):
        prefix = job["key"] + "." + str(job["process"].pid) + "."
        for suffix in ("*.onnx.tmp", "*.engine.tmp"):
            for path in self.directory.glob(prefix + suffix):
                try:
                    if path.parent.resolve() == self.directory.resolve():
                        path.unlink()
                except OSError:
                    pass

    def _poll(self):
        if self._job is None:
            return
        job = self._job
        code = job["process"].poll()
        if code is None and time.monotonic() - job["started"] <= self.BUILD_TIMEOUT:
            return
        key = job["key"]
        if code is None:
            self._stop_job()
            self._failure(key, "TensorRT optimization timed out")
            return
        if job["process"].stdin:
            job["process"].stdin.close()
        job["log"].close()
        self._job = None
        if code != 0:
            reason = "TensorRT optimization failed; using PyTorch"
            try:
                result = json.loads(job["result"].read_text(encoding="utf-8"))
                reason = result.get("error", reason)
            except (OSError, ValueError):
                pass
            self._failure(key, reason)
        else:
            if validate_entry(self.directory, key, job["spec"]) is None:
                self._failure(key, "TensorRT optimization produced an invalid cache")
            else:
                self._validated_missing.discard(key)
        self._cleanup_job_files(job)

    def _start(self, key, spec):
        request_path = self.directory / (key + ".request.json")
        result_path = self.directory / (key + ".result.json")
        request = {"schema": SCHEMA, "key": key, "spec": spec,
                   "runtime_root": str(self.runtime_root.resolve()),
                   "app_root": str(self.app_root.resolve()), "cache_directory": str(self.directory.resolve()),
                   "device_index": self.device}
        atomic_json(request_path, request)
        log = (self.directory / (key + ".build.log")).open("ab", buffering=0)
        flags = 0
        if os.name == "nt":
            flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
        try:
            process = subprocess.Popen([sys.executable, "-B", str(self.app_root / "trt_compile.py"),
                                        "--request", str(request_path), "--result", str(result_path), "--parent-watchdog"],
                                       stdin=subprocess.PIPE, stdout=log, stderr=log,
                                       creationflags=flags, close_fds=True)
        except Exception:
            log.close()
            raise
        self._job = {"key": key, "spec": spec, "process": process, "started": time.monotonic(),
                     "result": result_path, "log": log}

    def try_infer(self, a, b, fraction):
        """Return (output-or-None, optimizing). Never wait for compilation."""
        if not .05 <= fraction <= .95:
            raise ValueError("RIFE interpolation fraction is outside the pair")
        self._poll()
        if not self._initialize():
            return None, False
        spec = self._spec(a)
        if tuple(b.shape) != tuple(a.shape):
            raise ValueError("RIFE temporal inputs differ in shape")
        key = cache_key(spec)
        if self._engine_key == key and self._engine is not None:
            try:
                return self._engine.infer(a, b, fraction), False
            except Exception as error:
                self._engine = None
                self._engine_key = None
                self._failure(key, "TensorRT execution failed: " + str(error))
                return None, False
        if self._engine_key != key:
            # The last command completed before the next request is accepted.
            # Do not keep an unused engine/graph resident across a size change.
            self._engine = None
            self._engine_key = None
        if key not in self._validated_missing:
            path = validate_entry(self.directory, key, spec)
            if path is not None:
                try:
                    engine = Engine(path, spec, self.runtime_root)
                    self._engine = engine
                    self._engine_key = key
                    self.note = ""
                    return engine.infer(a, b, fraction), False
                except Exception as error:
                    self._engine = None
                    self._engine_key = None
                    self._failure(key, "TensorRT cache could not be loaded: " + str(error))
            self._validated_missing.add(key)
        if self._failed(key):
            return None, False
        now = time.monotonic()
        if self._pending_key != key:
            self._pending_key = key
            self._pending_since = now
            self.note = ""
        if self._job is None and now - self._pending_since >= self.SETTLE_SECONDS:
            try:
                self._start(key, spec)
            except Exception as error:
                self._failure(key, "TensorRT optimization could not start: " + str(error))
                return None, False
        return None, True

    def suspend(self):
        self._stop_job()
        self._pending_key = None
        self._engine = None
        self._engine_key = None

    def close(self):
        self.suspend()
