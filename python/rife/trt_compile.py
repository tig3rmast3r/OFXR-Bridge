"""Single-use isolated TensorRT compiler. Invoked only by the protocol worker.

All output is redirected by its parent to a cache build log. A stdin watchdog
also exits if the game kills its worker, including abrupt TerminateProcess.
"""
import argparse
import contextlib
import ctypes as C
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from trt_cache import BUILD_TIMEOUT, SCHEMA, atomic_json, cache_key, file_sha256, validate_entry

if os.name == "nt":
    import msvcrt
    _kernel = C.WinDLL("kernel32", use_last_error=True)
    _peek_pipe = _kernel.PeekNamedPipe
    _peek_pipe.argtypes = [C.c_void_p, C.c_void_p, C.c_ulong, C.c_void_p, C.c_void_p, C.c_void_p]
    _peek_pipe.restype = C.c_int


def phase(message):
    print(f"RIFE optimizer [{time.monotonic():.3f}] {message}", file=sys.stderr, flush=True)


def watch_parent():
    try:
        if os.name == "nt":
            handle = C.c_void_p(msvcrt.get_osfhandle(sys.stdin.fileno()))
            # A blocking CRT/Python stdin read can deadlock NumPy's Windows
            # extension initialization. Peek is nonblocking and holds no CRT
            # stdio lock while Torch/NumPy/TensorRT libraries initialize.
            while _peek_pipe(handle, None, 0, None, None, None):
                time.sleep(.25)
        else:
            os.read(sys.stdin.fileno(), 1)
    finally:
        os._exit(125)


def deadline(result_path):
    time.sleep(BUILD_TIMEOUT)
    try:
        atomic_json(result_path, {"ok": False, "error": "TensorRT optimization timed out"})
    finally:
        os._exit(124)


@contextlib.contextmanager
def builder_lock(directory, identity, timeout=BUILD_TIMEOUT - 5.0):
    """An OS-owned lock serializes builders from different games/workers."""
    import msvcrt
    gpu = cache_key({k: identity[k] for k in ("gpu", "gpu_uuid", "compute_capability")})
    stream = (directory / ("builder-" + gpu + ".lock")).open("a+b")
    try:
        stream.seek(0, os.SEEK_END)
        if stream.tell() == 0:
            stream.write(b"\0")
            stream.flush()
        started = time.monotonic()
        while True:
            try:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                break
            except OSError:
                if time.monotonic() - started > timeout:
                    raise RuntimeError("TensorRT builder is busy; optimization timed out")
                time.sleep(.25)
        try:
            yield
        finally:
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
    finally:
        stream.close()


def compile_engine(request):
    spec, key = request["spec"], request["key"]
    if request.get("schema") != SCHEMA or cache_key(spec) != key:
        raise ValueError("Invalid TensorRT optimization request")
    directory = Path(request["cache_directory"])
    directory.mkdir(parents=True, exist_ok=True)
    phase("waiting for shared builder lock")
    with builder_lock(directory, spec):
        phase("builder lock acquired")
        cached = validate_entry(directory, key, spec)
        if cached is not None:
            return {"ok": True, "key": key, "cached": True}
        runtime_root, app_root = Path(request["runtime_root"]), Path(request["app_root"])
        if str(app_root) not in sys.path:
            sys.path.insert(0, str(app_root))
        phase("loading Torch")
        import torch
        import torch.nn.functional as F
        from trt_backend import backend_logger, load_backend
        phase("loading TensorRT")
        trt = load_backend(runtime_root)
        phase("loading ONNX")
        import onnx
        from models.IFNet_HDv3_v4_4 import IFNet
        if trt.__version__ != spec["trt"] or torch.version.cuda != spec["cuda"]:
            raise RuntimeError("TensorRT/CUDA version changed during optimization")
        model_path = runtime_root / "models" / "flownet_v4.4.pkl"
        if file_sha256(model_path) != spec["model_sha256"]["weights"]:
            raise RuntimeError("RIFE model changed during optimization")
        if file_sha256(app_root / "models" / "IFNet_HDv3_v4_4.py") != spec["model_sha256"]["network"]:
            raise RuntimeError("RIFE network changed during optimization")
        if file_sha256(app_root / "models" / "warplayer.py") != spec["model_sha256"]["warp"]:
            raise RuntimeError("RIFE warp changed during optimization")
        if file_sha256(app_root / "trt_compile.py") != spec["model_sha256"]["compiler"]:
            raise RuntimeError("RIFE compiler changed during optimization")
        n, w, h = int(spec["eyes"]), int(spec["width"]), int(spec["height"])
        if n not in (1, 2) or not (1 <= w <= 16384 and 1 <= h <= 16384):
            raise ValueError("Unsupported RIFE engine dimensions")
        pw, ph = int(spec["padded_width"]), int(spec["padded_height"])
        if pw != (w + 31) // 32 * 32 or ph != (h + 31) // 32 * 32:
            raise ValueError("RIFE engine padding mismatch")
        torch.set_num_threads(1)

        class Flow(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.net = IFNet(1, False).eval()
                self.net.ofxr_flow_output = True
                weights = torch.load(model_path, map_location="cpu", weights_only=True, mmap=True)
                weights = {k.removeprefix("module."): v for k, v in weights.items()}
                self.net.load_state_dict({k: weights[k] for k in self.net.state_dict()}, strict=True)
                self.net.half()
                self.register_buffer("div", torch.tensor([(pw - 1) / 2, (ph - 1) / 2], dtype=torch.float32))
                gx = torch.linspace(-1, 1, pw).view(1, 1, 1, pw).expand(1, 1, ph, pw)
                gy = torch.linspace(-1, 1, ph).view(1, 1, ph, 1).expand(1, 1, ph, pw)
                self.register_buffer("grid", torch.cat((gx, gy), 1))

            def forward(self, a, b, timestep):
                a = F.pad(a, (0, pw - w, 0, ph - h))
                b = F.pad(b, (0, pw - w, 0, ph - h))
                t = timestep.expand(n, 1, ph, pw)
                return self.net(a, b, t, self.div, self.grid)[:, :, :h, :w]

        prefix = key + "." + str(os.getpid()) + "."
        fd, temporary_onnx = tempfile.mkstemp(prefix=prefix, suffix=".onnx.tmp", dir=directory)
        os.close(fd)
        fd, temporary_engine = tempfile.mkstemp(prefix=prefix, suffix=".engine.tmp", dir=directory)
        os.close(fd)
        try:
            phase("exporting RIFE4.4 flow/mask ONNX19")
            network = Flow().eval()
            a = torch.zeros((n, 3, h, w), dtype=torch.float16)
            b = torch.zeros_like(a)
            timestep = torch.full((1, 1, 1, 1), .5, dtype=torch.float16)
            with torch.inference_mode():
                # Opset20 silently changes GridSample behaviour in supported
                # TensorRT versions. Op19 preserves the validated bilinear op.
                torch.onnx.export(network, (a, b, timestep), temporary_onnx, input_names=["a", "b", "timestep"],
                                  output_names=["flow_mask"], opset_version=19, dynamo=False, do_constant_folding=True)
            onnx.checker.check_model(onnx.load(temporary_onnx))
            phase("ONNX validated; initializing matching CUDA adapter")
            torch.cuda.set_device(request["device_index"])
            properties = torch.cuda.get_device_properties(request["device_index"])
            if properties.name != spec["gpu"] or [properties.major, properties.minor] != spec["compute_capability"]:
                raise RuntimeError("CUDA adapter changed during optimization")
            uuid = str(getattr(properties, "uuid", ""))
            if spec["gpu_uuid"] and uuid != spec["gpu_uuid"]:
                raise RuntimeError("CUDA adapter identity changed during optimization")
            logger = backend_logger(trt)
            builder = trt.Builder(logger)
            definition = builder.create_network(1 << int(trt.NetworkDefinitionCreationFlag.STRONGLY_TYPED))
            parser = trt.OnnxParser(definition, logger)
            if not parser.parse(Path(temporary_onnx).read_bytes()):
                raise RuntimeError("; ".join(str(parser.get_error(i)) for i in range(parser.num_errors)))
            if definition.num_inputs != 3 or definition.num_outputs != 1:
                raise RuntimeError("TensorRT exporter produced an unexpected interface")
            configuration = builder.create_builder_config()
            configuration.clear_flag(trt.BuilderFlag.TF32)
            configuration.builder_optimization_level = 3
            configuration.max_aux_streams = 0
            configuration.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 512 * 1024 * 1024)
            # A lower-priority profiling stream reduces contention with the
            # game while the normal worker continues its Torch fallback.
            profiling_stream = torch.cuda.Stream(priority=1)
            configuration.profile_stream = profiling_stream.cuda_stream
            phase("building static TensorRT engine")
            serialized = builder.build_serialized_network(definition, configuration)
            if serialized is None:
                raise RuntimeError("TensorRT could not optimize RIFE4.4")
            Path(temporary_engine).write_bytes(bytes(serialized))
            engine_path = directory / (key + ".engine")
            metadata = {"schema": SCHEMA, "key": key, "spec": spec,
                        "engine_bytes": Path(temporary_engine).stat().st_size,
                        "engine_sha256": file_sha256(temporary_engine)}
            os.replace(temporary_engine, engine_path)
            atomic_json(directory / (key + ".json"), metadata)
            phase("engine committed to cache")
            try:
                (directory / (key + ".failed.json")).unlink()
            except FileNotFoundError:
                pass
            return {"ok": True, "key": key, "cached": False}
        finally:
            for path in (temporary_onnx, temporary_engine):
                try:
                    os.unlink(path)
                except FileNotFoundError:
                    pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--request", required=True)
    parser.add_argument("--result", required=True)
    parser.add_argument("--parent-watchdog", action="store_true")
    arguments = parser.parse_args()
    phase("compiler started")
    if arguments.parent_watchdog:
        threading.Thread(target=watch_parent, daemon=True).start()
    threading.Thread(target=deadline, args=(arguments.result,), daemon=True).start()
    try:
        request = json.loads(Path(arguments.request).read_text(encoding="utf-8"))
        result = compile_engine(request)
        atomic_json(arguments.result, result)
    except Exception as error:
        atomic_json(arguments.result, {"ok": False, "error": str(error)})
        print("RIFE TensorRT optimization failed: " + str(error), file=sys.stderr, flush=True)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
