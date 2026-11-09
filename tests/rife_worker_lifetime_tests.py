"""Failure-inject the real owned worker methods without loading CUDA/Torch."""
import ast
import ctypes
from pathlib import Path
from types import SimpleNamespace as NS
import unittest

TREE=ast.parse((Path(__file__).parents[1]/'python/rife/worker.py').read_text(encoding='utf-8'))

class LifetimeTests(unittest.TestCase):
    def fixture(self, fail_map=0, fail_tensor=False):
        state=NS(imports=set(), mappings=set(), closed=[], calls=0)
        def imported(desc):
            state.calls+=1; ident=state.calls;state.imports.add(ident);return (0,ident)
        def mapped(ident,desc):
            if ident==fail_map:return (1,None)
            state.mappings.add(ident+100);return (0,ident+100)
        def freed(ptr):state.mappings.remove(ptr);return (0,None)
        def destroyed(mem):state.imports.remove(mem);return (0,None)
        def tensor(capsule):
            if fail_tensor:raise RuntimeError('injected tensor construction failure')
            return object()
        def cuda(result):
            if result[0]:raise RuntimeError('injected CUDA mapping failure')
            return result[1]
        cu=NS(CUDA_EXTERNAL_MEMORY_HANDLE_DESC=lambda:NS(handle=NS(win32=NS())),
            CUexternalMemoryHandleType=NS(CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE=1),
            CUDA_EXTERNAL_MEMORY_BUFFER_DESC=lambda:NS(),cuImportExternalMemory=imported,
            cuExternalMemoryGetMappedBuffer=mapped,cuMemFree=freed,cuDestroyExternalMemory=destroyed)
        c=NS(c_int64=ctypes.c_int64,c_void_p=lambda x:x,addressof=lambda _:0,
            windll=NS(kernel32=NS(CloseHandle=lambda handle:state.closed.append(handle))))
        namespace=dict(cu=cu,C=c,cuda=cuda,torch=NS(cuda=NS(synchronize=lambda:None),utils=NS(dlpack=NS(from_dlpack=tensor))),
            ManagedTensor=lambda *a:NS(),Tensor=lambda *a:None,Device=lambda *a:None,DataType=lambda *a:None,
            NO_DELETE=None,capsule_new=lambda *a:None)
        shared=next(n for n in TREE.body if isinstance(n,ast.ClassDef) and n.name=='SharedBuffer')
        runner=next(n for n in TREE.body if isinstance(n,ast.ClassDef) and n.name=='Runner')
        methods=[n for n in runner.body if isinstance(n,ast.FunctionDef) and n.name in ('command','release')]
        isolated=ast.ClassDef(name='Runner',bases=[],keywords=[],body=methods,decorator_list=[])
        module=ast.fix_missing_locations(ast.Module(body=[shared,isolated],type_ignores=[]))
        exec(compile(module,'<real worker lifetime methods>','exec'),namespace)
        worker=namespace['Runner']();worker.configs={};worker.import_charges={};worker.device=0
        worker.optimization_enabled=False;worker.trt=None;worker.memory=None;worker.select_device=lambda _:None
        return state,namespace,worker

    def test_partial_mapping_failure_releases_import(self):
        state,code,_=self.fixture(fail_map=1)
        with self.assertRaises(RuntimeError):code['SharedBuffer'](11,4096,(1,3,4,4),0)
        self.assertFalse(state.imports);self.assertFalse(state.mappings);self.assertEqual(state.closed,[11])

    def test_tensor_failure_releases_mapping_and_import(self):
        state,code,_=self.fixture(fail_tensor=True)
        with self.assertRaises(RuntimeError):code['SharedBuffer'](12,4096,(1,3,4,4),0)
        self.assertFalse(state.imports);self.assertFalse(state.mappings);self.assertEqual(state.closed,[12])

    def test_failed_configuration_closes_unattempted_handles(self):
        state,_,worker=self.fixture(fail_map=2)
        with self.assertRaises(RuntimeError):worker.command(dict(op='configure',id=1,luid=0,width=4,height=4,eyes=1,
            buffers=[[11,4096],[12,4096],[13,4096]]))
        self.assertFalse(state.imports);self.assertFalse(state.mappings)
        self.assertFalse(worker.configs);self.assertFalse(worker.import_charges)
        self.assertEqual(sorted(state.closed),[11,12,13])

    def test_repeated_configuration_release_returns_to_baseline(self):
        state,_,worker=self.fixture()
        for epoch in range(100):
            worker.command(dict(op='configure',id=epoch,luid=0,width=4,height=4,eyes=1,
                buffers=[[11,4096],[12,4096],[13,4096]]))
            self.assertEqual(len(state.imports),3)
            worker.command(dict(op='release',id=epoch))
            self.assertFalse(state.imports);self.assertFalse(state.mappings)
            self.assertFalse(worker.configs);self.assertFalse(worker.import_charges)
        self.assertEqual(len(state.closed),300)

if __name__=='__main__':unittest.main()
