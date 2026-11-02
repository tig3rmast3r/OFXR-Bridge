"""Persistent OFXR RIFE worker. GPU buffers only; stdout is a JSON protocol.

This interpreter, Torch and model weights live in one shared portable runtime.
Only the selected model lives on the GPU. Temporal state belongs to the caller,
never to an eye-agnostic feature cache. Both eyes execute serially on one stream.
"""
import ctypes as C
import importlib
import json
from pathlib import Path
import sys

import torch
import torch.nn.functional as F
from cuda.bindings import driver as cu

ROOT = Path(sys.executable).resolve().parent.parent
MODELS = (("4.25.lite", "4_25_lite"), ("4.16.lite", "4_16_lite"), ("4.6", "4_6"))


def cuda(result):
    if result[0] != cu.CUresult.CUDA_SUCCESS:
        raise RuntimeError(f"CUDA: {result[0]}")
    return result[1] if len(result) == 2 else result[1:]


class Device(C.Structure):
    _fields_ = [("device_type", C.c_int), ("device_id", C.c_int)]


class DataType(C.Structure):
    _fields_ = [("code", C.c_uint8), ("bits", C.c_uint8), ("lanes", C.c_uint16)]


class Tensor(C.Structure):
    _fields_ = [("data", C.c_void_p), ("device", Device), ("ndim", C.c_int),
                ("dtype", DataType), ("shape", C.POINTER(C.c_int64)),
                ("strides", C.POINTER(C.c_int64)), ("byte_offset", C.c_uint64)]


class ManagedTensor(C.Structure):
    pass


Deleter = C.CFUNCTYPE(None, C.POINTER(ManagedTensor))
ManagedTensor._fields_ = [("tensor", Tensor), ("context", C.c_void_p), ("deleter", Deleter)]
NO_DELETE = Deleter(lambda _: None)
capsule_new = C.pythonapi.PyCapsule_New
capsule_new.restype = C.py_object
capsule_new.argtypes = (C.c_void_p, C.c_char_p, C.c_void_p)


class SharedBuffer:
    def __init__(self, handle, allocated_bytes, shape, device):
        self.memory = self.ptr = None
        self.tensor = None
        desc = cu.CUDA_EXTERNAL_MEMORY_HANDLE_DESC()
        desc.type = cu.CUexternalMemoryHandleType.CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE
        desc.handle.win32.handle = handle
        desc.size = allocated_bytes
        desc.flags = 1  # CUDA_EXTERNAL_MEMORY_DEDICATED: committed D3D12 resource
        try:
            self.memory = cuda(cu.cuImportExternalMemory(desc))
        finally:
            C.windll.kernel32.CloseHandle(C.c_void_p(handle))
        mapping = cu.CUDA_EXTERNAL_MEMORY_BUFFER_DESC()
        mapping.size = 2
        for dimension in shape:
            mapping.size *= dimension
        self.ptr = cuda(cu.cuExternalMemoryGetMappedBuffer(self.memory, mapping))
        self.shape = (C.c_int64 * len(shape))(*shape)
        self.managed = ManagedTensor(Tensor(int(self.ptr), Device(2, device), len(shape),
                                             DataType(2, 16, 1), self.shape, None, 0), None, NO_DELETE)
        capsule = capsule_new(C.addressof(self.managed), b"dltensor", None)
        self.tensor = torch.utils.dlpack.from_dlpack(capsule)

    def close(self):
        self.tensor = None
        if self.ptr is not None:
            cuda(cu.cuMemFree(self.ptr))
            self.ptr = None
        if self.memory is not None:
            cuda(cu.cuDestroyExternalMemory(self.memory))
            self.memory = None


class Runner:
    def __init__(self):
        self.model_id = None
        self.net = self.head = None
        self.configs = {}
        self.grids = {}
        self.graph_state = None
        self.device = None
        torch.set_num_threads(1)
        torch.backends.cudnn.benchmark = False  # no per-frame search stalls

    def select_device(self, luid):
        if self.device is not None:
            return
        cuda(cu.cuInit(0))
        for index in range(torch.cuda.device_count()):
            device = cuda(cu.cuDeviceGet(index))
            identity, _ = cuda(cu.cuDeviceGetLuid(device))
            if int.from_bytes(identity, "little") == luid:
                self.device = index
                torch.cuda.set_device(index)
                torch.cuda.init()
                return
        raise RuntimeError("No CUDA device matches the OpenXR D3D12 adapter LUID")

    def select_model(self, index):
        if index == self.model_id:
            return
        if index not in range(len(MODELS)):
            raise ValueError("Unsupported RIFE model")
        torch.cuda.synchronize()
        self.net = self.head = None
        self.graph_state = None
        self.grids.clear()
        torch.cuda.empty_cache()
        version, module = MODELS[index]
        code = importlib.import_module("models.IFNet_HDv3_v" + module)
        weights = torch.load(ROOT / "models" / f"flownet_v{version}.pkl",
                             map_location="cpu", weights_only=True, mmap=True)
        weights = {k.removeprefix("module."): v for k, v in weights.items()}
        self.net = code.IFNet(1, False).eval()
        # Training checkpoints also contain teacher/calibration-only modules.
        # Every inference parameter must still be present and shape-correct.
        net_weights = {k: weights[k] for k in self.net.state_dict()}
        self.net.load_state_dict(net_weights, strict=True)
        self.net.ofxr_flow_output = True
        self.net.to(device=self.device, dtype=torch.float16)
        if hasattr(code, "Head"):
            self.head = self.net.encode
        self.model_id = index

    @torch.inference_mode()
    def infer(self, a, b):
        h, w = a.shape[-2:]
        modulo = 128 if self.model_id == 0 else 32
        pw, ph = ((w + modulo - 1) // modulo * modulo, (h + modulo - 1) // modulo * modulo)
        key = (pw, ph)
        if key not in self.grids:
            horizontal = torch.linspace(-1, 1, pw, device=self.device).view(1, 1, 1, pw).expand(1, 1, ph, pw)
            vertical = torch.linspace(-1, 1, ph, device=self.device).view(1, 1, ph, 1).expand(1, 1, ph, pw)
            self.grids[key] = (torch.tensor([(pw-1)/2, (ph-1)/2], device=self.device),
                              torch.cat((horizontal, vertical), 1),
                              torch.full((1, 1, ph, pw), .5, device=self.device, dtype=torch.float16))
        div, grid, timestep = self.grids[key]
        a, b = F.pad(a, (0, pw-w, 0, ph-h)), F.pad(b, (0, pw-w, 0, ph-h))
        args = (a, b, timestep, div, grid)
        result = self.net(*args, self.head(a), self.head(b)) if self.head else self.net(*args)
        return result[:, :, :h, :w]

    def command(self, request):
        op = request["op"]
        if op == "configure":
            self.select_device(request["luid"])
            ident = request["id"]
            self.release(ident)
            buffers = []
            try:
                for index, desc in enumerate(request["buffers"]):
                    shape = (request["eyes"], 5 if index == 2 else 3, request["height"], request["width"])
                    buffers.append(SharedBuffer(desc[0], desc[1], shape, self.device))
                self.configs[ident] = buffers
            except Exception:
                for buffer in buffers:
                    buffer.close()
                raise
            return {}
        if op == "release":
            self.release(request["id"])
            return {}
        if op == "infer":
            self.select_model(request["model"])
            a, b, out = [buffer.tensor for buffer in self.configs[request["id"]]]
            for eye in range(a.shape[0]):
                out[eye:eye+1].copy_(self.infer_cached(a[eye:eye+1], b[eye:eye+1], request.get("fraction", .5)))
            # The reply is a completion receipt. D3D12 never waits an output
            # semaphore that a failed or terminated worker might not signal.
            torch.cuda.current_stream().synchronize()
            return {}
        raise ValueError("Unknown worker operation")

    @torch.inference_mode()
    def infer_cached(self, a, b, fraction=.5):
        # Hundreds of small Torch launches are otherwise CPU-bound even on a
        # fast GPU. Capture one fixed-shape graph and reuse it for both eyes.
        shape = tuple(a.shape)
        if self.graph_state is None or self.graph_state[0] != shape:
            self.graph_state = None
            x, y = torch.empty_like(a), torch.empty_like(b)
            x.copy_(a); y.copy_(b)
            stream = torch.cuda.Stream()
            stream.wait_stream(torch.cuda.current_stream())
            with torch.cuda.stream(stream):
                for _ in range(3):
                    self.infer(x, y)
            torch.cuda.current_stream().wait_stream(stream)
            graph = torch.cuda.CUDAGraph()
            with torch.cuda.graph(graph, stream=stream):
                output = self.infer(x, y)
            self.graph_state = (shape, x, y, graph, output)
        _, x, y, graph, output = self.graph_state
        x.copy_(a); y.copy_(b)
        if not .05 <= fraction <= .95:
            raise ValueError("RIFE interpolation fraction is outside the pair")
        for _, _, timestep in self.grids.values():
            timestep.fill_(fraction)
        graph.replay()
        return output

    def release(self, ident):
        buffers = self.configs.pop(ident, [])
        if buffers:
            torch.cuda.synchronize()
            for buffer in buffers:
                buffer.close()


def main():
    runner = Runner()
    print(json.dumps({"ok": True, "protocol": 2}), flush=True)
    for line in sys.stdin:
        try:
            result = runner.command(json.loads(line))
            print(json.dumps({"ok": True, **result}), flush=True)
        except Exception as error:
            print(json.dumps({"ok": False, "error": str(error)}), flush=True)
    for ident in list(runner.configs):
        runner.release(ident)


if __name__ == "__main__":
    main()
