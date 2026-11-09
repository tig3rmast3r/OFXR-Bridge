"""CPU fixtures for sealed Vulkan sources and synchronized installer packaging."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SOURCES = ('native/rife_vulkan/worker.cpp', 'native/rife_vulkan/fused_warp.hpp',
           'native/rife_vulkan/CMakeLists.txt', 'tools/prepare_rife_vulkan_deps.py',
           'tools/vulkan_compile.py', 'tools/install_rife_vulkan.py')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='RIFE package spaces ')
        self.root = Path(self.temp.name)
        self.payload = self.root / 'rife-vulkan'
        self.payload.mkdir()
        for relative in SOURCES + ('native/rife_vulkan/dependencies.json',):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture')
        for name in ('dependencies.json', 'ofxr_rife_vulkan.exe', 'pnnx.exe'):
            (self.payload / name).write_bytes(b'fixture')
        self.manifest = dict(schema=1, backend='vulkan',
                             files={p.name: sha(p) for p in self.payload.iterdir()},
                             source={p: sha(self.root / p) for p in SOURCES})

    def tearDown(self):
        self.temp.cleanup()

    def check(self, expected=True, write=True):
        if write:
            (self.payload / 'manifest.json').write_text(json.dumps(self.manifest))
        # Execute the real production functions, without invoking packaging/builds.
        runner = self.root / 'guard.ps1'
        runner.write_text('''param([string]$Script,[string]$Root)
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($Script,[ref]$tokens,[ref]$errors)
if($errors.Count){throw $errors[0]}
foreach($name in @('Hash','Assert-RifeVulkanPayload')){
 $node=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
 Invoke-Expression $node.Extent.Text
}
$null=Assert-RifeVulkanPayload $Root
''', encoding='utf-8')
        result = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(runner), '-Script', str(SOURCE / 'tools/Package-RifeBuild.ps1'),
                                 '-Root', str(self.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, expected, result.stdout + result.stderr)
        return result

    def test_current_sources_and_payload_pass(self):
        self.check()

    def test_missing_payload_is_rejected(self):
        self.check(False, write=False)

    def test_changed_source_is_rejected(self):
        (self.root / SOURCES[0]).write_bytes(b'changed source')
        self.assertIn('rebuild/reseal', self.check(False).stderr)

    def test_changed_binary_is_rejected(self):
        (self.payload / 'ofxr_rife_vulkan.exe').write_bytes(b'changed binary')
        self.check(False)

    def test_changed_dependency_pins_are_rejected(self):
        (self.root / 'native/rife_vulkan/dependencies.json').write_bytes(b'changed pins')
        self.check(False)

    def test_omitted_source_pin_is_rejected(self):
        del self.manifest['source'][SOURCES[0]]
        self.check(False)

    def test_payload_path_cannot_escape_root(self):
        (self.root / 'outside').write_bytes(b'outside')
        self.manifest['files']['../outside'] = sha(self.root / 'outside')
        self.check(False)

    def make_stage(self):
        for name in ('tools/same.ps1', 'python/overlay.py', 'licenses/license.txt',
                     'LICENSE', 'Install-RIFE-Vulkan.cmd', 'docs/RIFE_VULKAN.md'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'sealed stage version')
        (self.root / 'BUILD_PROVENANCE.json').write_text(json.dumps(dict(version=951, source_commit='sealed-commit')))
        entries = sorted(p for p in self.root.rglob('*') if p.is_file())
        (self.root / 'SHA256SUMS.txt').write_text(''.join(sha(p) + '  ' + p.relative_to(self.root).as_posix() + '\n' for p in entries))
        spec = importlib.util.spec_from_file_location('package_rife_installer', SOURCE / 'tools/package_rife_installer.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_standalone_uses_sealed_stage_only(self):
        builder = self.make_stage()
        output = self.root / 'standalone'
        builder.package(self.root, output)
        for name in ('tools/same.ps1', 'Install-RIFE-Vulkan.cmd', 'LICENSE'):
            self.assertEqual((output / name).read_bytes(), (self.root / name).read_bytes())
        self.assertEqual((output / 'RIFE_VULKAN.md').read_bytes(), (self.root / 'docs/RIFE_VULKAN.md').read_bytes())
        self.assertIn('V951', (output / 'LEGGIMI.txt').read_text(encoding='utf-8'))
        self.assertEqual(json.loads((output / 'INSTALLER_PROVENANCE.json').read_text())['installer_tools_commit'], 'sealed-commit')

    def test_standalone_rejects_tampered_stage(self):
        builder = self.make_stage()
        (self.root / 'tools/same.ps1').write_bytes(b'tampered')
        with self.assertRaisesRegex(RuntimeError, 'integrity mismatch'):
            builder.package(self.root, self.root / 'standalone')

    def test_standalone_preserves_previous_zip(self):
        builder = self.make_stage()
        output = self.root / 'standalone'
        archive = Path(str(output) + '.zip')
        archive.write_bytes(b'prior sealed package')
        with self.assertRaisesRegex(RuntimeError, 'preserved'):
            builder.package(self.root, output)
        self.assertEqual(archive.read_bytes(), b'prior sealed package')
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
