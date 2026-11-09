"""CPU graph-contract checks; numerical and speed checks run on actual Vulkan."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('vulkan_compile', SOURCE / 'tools/vulkan_compile.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)


def graph():
    rows = ['Input a 0 1 a', 'Input b 0 1 b', 'Input t 0 1 t']
    previous = 'a'
    for block in range(4):
        rows.append(f'Concat cat{block} 3 1 {previous} b t feature{block}')
        previous = f'feature{block}'
        for i in range(10):
            name = f'conv{block}_{i}'
            rows.append(f'Convolution {name} 1 1 {previous} {name} 0=64 1=3 6=36864')
            previous = name
        rows.append(f'Deconvolution head{block} 1 1 {previous} head{block} 0=5 1=4 6=5120')
        previous = f'head{block}'
        if block < 3:
            rows.append(f'Split split{block} 1 2 {previous} flow{block}a flow{block}b')
            for eye, image in (('a', 'a'), ('b', 'b')):
                rows.append(f'OFXRWarp warp{block}{eye} 2 1 {image} flow{block}{eye} warp{block}{eye}')
            rows.append(f'BinaryOp sum{block} 2 1 warp{block}a warp{block}b sum{block} 0=0')
            previous = f'sum{block}'
    rows.append(f'Sigmoid mask 1 1 {previous} out0')
    blobs = {p for row in rows for p in row.split()[4:4+int(row.split()[2])+int(row.split()[3])]}
    return '7767517\n' + f'{len(rows)} {len(blobs)}\n' + '\n'.join(rows) + '\n'


def nodes(path):
    return [line.split() for line in path.read_text().splitlines()[2:]]


class PrecisionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='RIFE precision spaces ')
        self.root = Path(self.temp.name)
        self.base = self.root / 'flow.ncnn.param'
        self.output = self.root / 'mixed.param'
        self.base.write_text(graph())

    def tearDown(self):
        self.temp.cleanup()

    def verify(self, first, expected_half, expected_casts):
        original = self.base.read_bytes()
        report = compiler.mixed_precision_graph(self.base, self.output, first)
        dtype = {}
        halves = 0
        casts = 0
        weights = []
        for f in nodes(self.output):
            ni, no = int(f[2]), int(f[3])
            inputs, outputs, params = f[4:4+ni], f[4+ni:4+ni+no], f[4+ni+no:]
            if f[0] == 'Cast':
                casts += 1
                values = dict(p.split('=', 1) for p in params)
                self.assertEqual(dtype[inputs[0]], int(values['0']))
                current = int(values['1'])
            else:
                current = 2 if f[0] == 'Convolution' and '31=3' not in params else 1
                for blob in inputs:
                    self.assertEqual(dtype[blob], current, f'{f[1]} reads the wrong tensor storage')
                if f[0] != 'Convolution':
                    self.assertEqual(current, 1, 'geometry/head left FP32')
                halves += current == 2
                if f[0] in ('Convolution', 'Deconvolution'):
                    weights.append(f[1])
            for out in outputs:
                dtype[out] = current
        self.assertEqual(halves, expected_half)
        self.assertEqual(casts, expected_casts)
        self.assertEqual(dtype['out0'], 1)
        self.assertEqual(weights, [f[1] for f in nodes(self.base) if f[0] in ('Convolution', 'Deconvolution')])
        self.assertEqual(self.base.read_bytes(), original)
        self.assertEqual(report['fp16_convolutions'], expected_half)
        self.assertEqual(report['fp32_heads'], 4)

    def test_all_features_preserve_fp32_geometry_and_heads(self):
        self.verify(0, 40, 8)

    def test_refinement_preserves_first_two_blocks(self):
        self.verify(2, 20, 4)

    def test_final_preserves_first_three_blocks(self):
        self.verify(3, 10, 2)

    def test_policy_is_bounded(self):
        with self.assertRaises(ValueError):
            compiler.mixed_precision_graph(self.base, self.output, 1)

    def test_unreviewed_graph_is_rejected(self):
        self.base.write_text(graph().replace('Convolution conv0_0', 'Unknown conv0_0'))
        with self.assertRaisesRegex(RuntimeError, 'pinned'):
            compiler.mixed_precision_graph(self.base, self.output, 0)

    def test_existing_masks_are_rejected(self):
        self.base.write_text(graph().replace('0=64 1=3', '31=3 0=64 1=3', 1))
        with self.assertRaisesRegex(RuntimeError, 'annotation'):
            compiler.mixed_precision_graph(self.base, self.output, 0)

    def test_bad_block_boundaries_are_rejected(self):
        self.base.write_text(graph().replace('Convolution conv0_9 1 1 conv0_8 conv0_9 0=64 1=3 6=36864\n', '')
                             .replace('Sigmoid mask', 'Convolution replacement 1 1 head3 unused 0=64\nSigmoid mask'))
        with self.assertRaises(RuntimeError):
            compiler.mixed_precision_graph(self.base, self.output, 0)

    def test_serialization_is_reproducible(self):
        compiler.mixed_precision_graph(self.base, self.output, 2)
        first = self.output.read_bytes()
        compiler.mixed_precision_graph(self.base, self.output, 2)
        self.assertEqual(first, self.output.read_bytes())

    def test_reserved_names_are_rejected(self):
        self.base.write_text(graph().replace('feature0', 'ofxr_mp_reserved'))
        with self.assertRaisesRegex(RuntimeError, 'Reserved'):
            compiler.mixed_precision_graph(self.base, self.output, 0)


if __name__ == '__main__':
    unittest.main()
