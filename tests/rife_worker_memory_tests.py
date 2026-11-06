"""Exercise the real memory command without loading CUDA or running inference."""
import ast
from pathlib import Path
from types import SimpleNamespace
import unittest


tree = ast.parse((Path(__file__).parents[1]/'python/rife/worker.py').read_text(encoding='utf-8'))
runner = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'Runner')
command = next(node for node in runner.body if isinstance(node, ast.FunctionDef) and node.name == 'command')
reserved_devices = []
namespace = {'torch': SimpleNamespace(cuda=SimpleNamespace(memory_reserved=lambda device: reserved_devices.append(device) or 64))}
exec(compile(ast.Module(body=[command], type_ignores=[]), '<real Runner.command>', 'exec'), namespace)


class MemoryCommandTests(unittest.TestCase):
    def setUp(self):
        reserved_devices.clear()

    def query(self, device=None, usage=None, charges=None, available=True):
        state = SimpleNamespace(device=device,
            memory=SimpleNamespace(usage=lambda: usage) if available else None,
            import_charges=charges or {})
        return namespace['command'](state, {'op': 'memory'})

    def test_uninitialized_worker_is_measured_zero_without_cuda(self):
        self.assertEqual(self.query(), {'vram_private': 0, 'torch_reserved': 0})
        self.assertEqual(reserved_devices, [])

    def test_imported_images_are_not_counted_twice(self):
        self.assertEqual(self.query(2, 1024, {1: 128, 2: 256}),
                         {'vram_private': 640, 'torch_reserved': 64})
        self.assertEqual(reserved_devices, [2])

    def test_unavailable_dxgi_is_not_invented_as_zero(self):
        self.assertEqual(self.query(0, available=False), {})
        self.assertEqual(self.query(0, usage=None), {})
        self.assertEqual(reserved_devices, [])

    def test_import_charge_rounding_does_not_underflow(self):
        self.assertEqual(self.query(0, 10, {1: 20})['vram_private'], 0)


if __name__ == '__main__':
    unittest.main()
