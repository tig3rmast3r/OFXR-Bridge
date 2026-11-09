"""Static RIFE4.4 flow/mask export and device-local Vulkan preparation.

Torch is used on CPU by this offline compiler only. The native worker never
loads CUDA/Torch and always composes original colours in the existing D3D12 path.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil
import time
import uuid

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).resolve().parents[1]
APP = PACKAGE / "python/rife" if (PACKAGE / "python/rife").is_dir() else PACKAGE / "app"
sys.path.insert(0, str(APP))


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def atomic_json(path, value):
    path = Path(path)
    part = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    part.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    for attempt in range(6):
        try:
            os.replace(part, path)
            break
        except OSError as error:
            if getattr(error, "winerror", None) not in (5, 32, 33) or attempt == 5:
                raise
            time.sleep(.02 * (attempt + 1))


def dimensions(width, height):
    if not 32 <= width <= 16384 or not 32 <= height <= 16384:
        raise ValueError("Vulkan RIFE dimensions must be between 32 and 16384")
    return (width + 31) // 32 * 32, (height + 31) // 32 * 32


def fuse_warps(path, width, height):
    """Replace the exact pinned border/bilinear warp graph with one GPU dispatch.

    Keep weight-bearing nodes in order so ncnn's sequential weight file remains
    valid. Unused coordinate arithmetic is pruned, never executed on the CPU.
    """
    lines = path.read_text(encoding="utf-8").splitlines()
    nodes = []
    for line in lines[2:]:
        fields = line.split()
        if not fields:
            continue
        ni, no = int(fields[2]), int(fields[3])
        nodes.append(dict(kind=fields[0], name=fields[1], inputs=fields[4:4+ni],
                          outputs=fields[4+ni:4+ni+no], params=fields[4+ni+no:]))
    producers = {blob: node for node in nodes for blob in node["outputs"]}
    def source(blob):
        node = producers[blob]
        return source(node["inputs"][0]) if node["kind"] == "Split" else node
    fused = 0
    for node in nodes:
        if node["kind"] != "GridSample":
            continue
        if set(node["params"]) != {"0=1", "1=2", "2=1", "3=1"}:
            raise RuntimeError("Unsupported warp semantics; refusing graph fusion")
        add = source(node["inputs"][1])
        if add["kind"] != "BinaryOp" or add["params"] != ["0=0"]:
            raise RuntimeError("Unexpected warp coordinate sum")
        candidates = [source(blob) for blob in add["inputs"]]
        concat = next((n for n in candidates if n["kind"] == "Concat"), None)
        grid = next((n for n in candidates if n["kind"] == "MemoryData"), None)
        if concat is None or grid is None or len(concat["inputs"]) != 2:
            raise RuntimeError("Unexpected warp grid")
        divisions = [source(blob) for blob in concat["inputs"]]
        for division, divisor in zip(divisions, ((width-1)/2, (height-1)/2)):
            params = dict(p.split("=", 1) for p in division["params"])
            if division["kind"] != "BinaryOp" or params.get("0") != "3" or params.get("1") != "1" or float(params.get("2", "nan")) != divisor:
                raise RuntimeError("Unexpected warp normalization")
        slices = [source(n["inputs"][0]) for n in divisions]
        if slices[0] is not slices[1] or slices[0]["kind"] != "Slice" or slices[0]["params"] != ["-23300=2,1,-233", "1=0"]:
            raise RuntimeError("Unexpected warp flow slices")
        node.update(kind="OFXRWarp", name="ofxr_warp_" + str(fused),
                    inputs=[node["inputs"][0], slices[0]["inputs"][0]], params=[])
        fused += 1
    if fused != 6:
        raise RuntimeError("Expected the six RIFE4.4 warps")
    # Pnnx may reuse names for independent Split nodes, use object identity.
    needed_nodes = set()
    def retain(blob):
        node = producers[blob]
        if id(node) in needed_nodes:
            return
        needed_nodes.add(id(node))
        for value in node["inputs"]:
            retain(value)
    retain("out0")
    nodes = [n for n in nodes if id(n) in needed_nodes or n["kind"] == "MemoryData"]
    blobs = {b for n in nodes for b in n["inputs"] + n["outputs"]}
    output = ["7767517", f"{len(nodes)} {len(blobs)}"]
    for node in nodes:
        output.append(" ".join([node["kind"], node["name"], str(len(node["inputs"])), str(len(node["outputs"]))]
                               + node["inputs"] + node["outputs"] + node["params"]))
    path.write_text("\n".join(output) + "\n", encoding="utf-8", newline="\n")
    return fused


def mixed_precision_graph(path, output, first_half_block):
    """Keep geometry/flow, heads and masks FP32; cast only feature convolutions.

    ncnn's Vulkan feature mask changes shaders but does not cast storage when
    packing stays unchanged. Explicit Cast nodes are required on every boundary.
    Weight-bearing nodes keep their original order and share the original .bin.
    """
    if first_half_block not in (0, 2, 3):
        raise ValueError('Unsupported mixed-precision block policy')
    lines = path.read_text(encoding='utf-8').splitlines()
    nodes = []
    for line in lines[2:]:
        f = line.split()
        if not f:
            continue
        ni, no = int(f[2]), int(f[3])
        nodes.append(dict(kind=f[0], name=f[1], inputs=f[4:4+ni], outputs=f[4+ni:4+ni+no], params=f[4+ni+no:]))
    counts = {kind: sum(n['kind'] == kind for n in nodes) for kind in ('Convolution', 'Deconvolution', 'OFXRWarp')}
    if counts != dict(Convolution=40, Deconvolution=4, OFXRWarp=6):
        raise RuntimeError('Mixed precision requires the pinned four-block RIFE4.4 graph')
    if any(n['kind'] == 'Cast' or any(p.startswith('31=') for p in n['params']) for n in nodes):
        raise RuntimeError('Unexpected existing precision annotation')
    blobs = {b for n in nodes for b in n['inputs'] + n['outputs']}
    if any(b.startswith('ofxr_mp_') for b in blobs):
        raise RuntimeError('Reserved mixed-precision blob name')
    precision = {}
    casts = {}
    result = []
    block = 0
    convs = 0
    half_convs = 0
    for n in nodes:
        half = n['kind'] == 'Convolution' and block >= first_half_block
        dtype = 2 if half else 1
        inputs = []
        for blob in n['inputs']:
            if blob not in precision:
                raise RuntimeError('Graph is not in producer order: ' + blob)
            if precision[blob] == dtype:
                inputs.append(blob)
                continue
            key = (blob, dtype)
            if key not in casts:
                name = 'ofxr_mp_' + str(len(casts))
                casts[key] = name
                result.append(dict(kind='Cast', name=name, inputs=[blob], outputs=[name],
                                   params=[f'0={precision[blob]}', f'1={dtype}']))
            inputs.append(casts[key])
        params = n['params'] + ([] if half else ['31=3'])
        result.append({**n, 'inputs': inputs, 'params': params})
        for blob in n['outputs']:
            precision[blob] = dtype
        if n['kind'] == 'Convolution':
            convs += 1
            half_convs += half
        if n['kind'] == 'Deconvolution':
            if convs != 10:
                raise RuntimeError('Unexpected RIFE feature/head boundary')
            block += 1
            convs = 0
    if block != 4 or precision.get('out0') != 1:
        raise RuntimeError('Mixed graph must produce FP32 flow/mask')
    blobs = {b for n in result for b in n['inputs'] + n['outputs']}
    rows = ['7767517', f'{len(result)} {len(blobs)}']
    for n in result:
        rows.append(' '.join([n['kind'], n['name'], str(len(n['inputs'])), str(len(n['outputs']))]
                             + n['inputs'] + n['outputs'] + n['params']))
    output.write_text('\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
    return dict(first_half_block=first_half_block, fp16_convolutions=half_convs,
                fp32_convolutions=40-half_convs, fp32_heads=4, casts=len(casts),
                fp32_geometry=True, fp32_output=True)


def export(root, width, height, directory, pnnx, app=APP):
    import torch
    from models import IFNet_HDv3_v4_4 as code
    from models.IFNet_HDv3_v4_4 import IFNet
    torch.set_num_threads(1)
    net = IFNet(1, False).float().eval()
    net.ofxr_flow_output = True
    weights = torch.load(root / "models/flownet_v4.4.pkl", map_location="cpu", weights_only=True, mmap=True)
    weights = {k.removeprefix("module."): v for k, v in weights.items()}
    net.load_state_dict({k: weights[k] for k in net.state_dict()}, strict=True)

    class Flow(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.net = net
            self.register_buffer("div", torch.tensor([(width - 1) / 2, (height - 1) / 2]))
            gx = torch.linspace(-1, 1, width).view(1, 1, 1, width).expand(1, 1, height, width)
            gy = torch.linspace(-1, 1, height).view(1, 1, height, 1).expand(1, 1, height, width)
            self.register_buffer("grid", torch.cat((gx, gy), 1))

        def forward(self, a, b, timestep):
            return self.net(a, b, timestep, self.div, self.grid)

    network = Flow().eval()
    # Every exported tensor is FP32. Removing identity dtype casts avoids
    # unsupported aten::to nodes in pnnx; numerical warp semantics are retained.
    def export_warp(image, flow, div, grid):
        normalized = torch.cat((flow[:, 0:1] / div[0], flow[:, 1:2] / div[1]), 1)
        return torch.nn.functional.grid_sample(image, (grid + normalized).permute(0, 2, 3, 1),
                                               mode="bilinear", padding_mode="border", align_corners=True)
    original_warp = code.warp
    # Reproducible CPU reference is retained for cross-vendor numerical checks.
    generator = torch.Generator().manual_seed(919)
    # The D3D12 bridge supplies FP16 planes on every backend. Quantize the CPU
    # fixture identically so input rounding is not misdiagnosed as model error.
    a = torch.rand((1, 3, height, width), generator=generator).half().float()
    b = torch.roll(a, shifts=2, dims=3)
    t = torch.full((1, 1, height, width), .5)
    with torch.inference_mode():
        for name, value in (("a", a), ("b", b), ("reference", network(a, b, t))):
            (directory / (name + ".f32")).write_bytes(value.contiguous().numpy().tobytes())
        for fraction, name in ((.25, "reference25"), (.75, "reference75")):
            value = network(a, b, torch.full_like(t, fraction))
            (directory / (name + ".f32")).write_bytes(value.contiguous().numpy().tobytes())
        for fraction, name in ((.25, "reference-eye1-25"), (.5, "reference-eye1-50"), (.75, "reference-eye1-75")):
            value = network(b, a, torch.full_like(t, fraction))
            (directory / (name + ".f32")).write_bytes(value.contiguous().numpy().tobytes())
        try:
            code.warp = export_warp
            traced = torch.jit.trace(network, (a, b, t), check_trace=False)
            traced.save(str(directory / "flow.pt"))
        finally:
            code.warp = original_warp
    shape = f"[1,3,{height},{width}],[1,3,{height},{width}],[1,1,{height},{width}]"
    with (directory / "export.log").open("w", encoding="utf-8") as log:
        subprocess.run([str(pnnx), "flow.pt", "inputshape=" + shape, "fp16=0"], cwd=directory,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    param = (directory / "flow.ncnn.param").read_text(encoding="utf-8")
    if "pnnx." in param or "aten::" in param or not (directory / "flow.ncnn.bin").is_file():
        raise RuntimeError("pnnx produced unsupported operations; inspect export.log")
    fuse_warps(directory / "flow.ncnn.param", width, height)


def prepare(root, cache, worker, pnnx, width, height, gpu=0, eyes=2, rounds=5):
    from runtime_lease import RuntimeLease
    width, height = dimensions(width, height)
    if eyes not in (1, 2) or not 1 <= rounds <= 30:
        raise ValueError("Invalid eye count or tuning iterations")
    root, cache = root.resolve(), cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    probe = json.loads(subprocess.check_output([str(worker), "--probe", str(gpu)], text=True, timeout=30))
    if not probe.get("ok"):
        raise RuntimeError(probe.get("error", "Vulkan probe failed"))
    gpu = probe["gpu"]
    spec = {"schema": 1, "model": "4.4", "width": width, "height": height, "eyes": eyes,
            "weights": sha(root / "models/flownet_v4.4.pkl"), "network": sha(APP / "models/IFNet_HDv3_v4_4.py"),
            "warp": sha(APP / "models/warplayer.py"), "compiler": sha(__file__), "worker": sha(worker), "pnnx": sha(pnnx),
            "device": probe["device"]}
    key = hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest()
    directory = cache / key
    directory.mkdir(exist_ok=True)
    # The OS lock serializes publishers and protects live plans from replacement.
    with RuntimeLease(directory, exclusive=True):
        index = cache / f"{width}x{height}-{eyes}-{probe['device']['uuid']}.json"
        manifest = directory / "plan.json"
        if manifest.is_file():
            try:
                previous = json.loads(manifest.read_text(encoding="utf-8"))
                valid = previous.get("spec") == spec and all(sha(directory / name) == value for name, value in previous["files"].items())
            except (OSError, ValueError, KeyError):
                valid = False
            if valid:
                atomic_json(index, {"schema": 1, "plan": key + "/plan.json"})
                return manifest
        # Publish fresh immutable files in a new subdirectory; an interrupted
        # export/tune never makes a partially prepared plan visible to a worker.
        stage = directory / ("b-" + uuid.uuid4().hex[:8])
        stage.mkdir()
        # Upstream pnnx creates long intermediate filenames and is not fully
        # long-path aware. Export in a short, owned temp folder then retain the
        # runtime artifacts and numerical evidence under the immutable key.
        with tempfile.TemporaryDirectory(prefix="ofxr-vk-") as temporary:
            temporary = Path(temporary)
            export(root, width, height, temporary, pnnx)
            for name in ("flow.ncnn.param", "flow.ncnn.bin", "a.f32", "b.f32", "reference.f32", "reference25.f32", "reference75.f32",
                         "reference-eye1-25.f32", "reference-eye1-50.f32", "reference-eye1-75.f32", "export.log"):
                shutil.copy2(temporary / name, stage / name)
        mixed = {name: mixed_precision_graph(stage / 'flow.ncnn.param', stage / f'flow.{name}.param', first)
                 for name, first in (('features', 0), ('refine', 2), ('final', 3))}
        atomic_json(stage / 'mixed-precision.json', mixed)
        request = {"directory": str(stage), "width": width, "height": height, "eyes": eyes,
                   "gpu": gpu, "rounds": rounds, "device": probe["device"]}
        atomic_json(stage / "request.json", request)
        with (stage / "tuning.log").open("w", encoding="utf-8") as log:
            subprocess.run([str(worker), "--prepare", str(stage / "request.json")], stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=1200)
        result = json.loads((stage / "result.json").read_text(encoding="utf-8"))
        if not result.get("ok") or not result.get("reference_pass"):
            raise RuntimeError("Vulkan numerical verification failed; inspect " + str(stage))
        files = {str(path.relative_to(directory)).replace("\\", "/"): sha(path)
                 for path in (stage / name for name in ("flow.ncnn.param", "flow.ncnn.bin", "pipeline.cache", "warp.cache",
                              "a.f32", "b.f32", "reference.f32", "reference25.f32", "reference75.f32",
                              "reference-eye1-25.f32", "reference-eye1-50.f32", "reference-eye1-75.f32"))}
        for name in ('flow.features.param', 'flow.refine.param', 'flow.final.param', 'mixed-precision.json'):
            files[str((stage / name).relative_to(directory)).replace('\\', '/')] = sha(stage / name)
        atomic_json(manifest, {"schema": 1, "spec": spec, "directory": stage.name, "files": files, **result})
        atomic_json(index, {"schema": 1, "plan": key + "/plan.json"})
        return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--pnnx", type=Path, required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--eyes", type=int, default=2)
    parser.add_argument("--rounds", type=int, default=5)
    args = parser.parse_args()
    from runtime_lease import RuntimeLease
    with RuntimeLease(args.root):
        print(prepare(args.root, args.cache, args.worker.resolve(), args.pnnx.resolve(), args.width, args.height,
                      args.gpu, args.eyes, args.rounds))


if __name__ == "__main__":
    main()
