"""Current-process DXGI dedicated usage on the same adapter as CUDA.

Uses the Windows SDK IDXGIFactory4/IDXGIAdapter3 ABI. No GPU synchronization,
NVML installation, performance counter service, or global free-memory guess.
"""
import ctypes as C
import uuid


class Luid(C.Structure):
    _fields_ = [("low", C.c_uint32), ("high", C.c_int32)]


class MemoryInfo(C.Structure):
    _fields_ = [("budget", C.c_uint64), ("usage", C.c_uint64),
                ("available", C.c_uint64), ("reservation", C.c_uint64)]


def guid(text):
    return (C.c_ubyte * 16).from_buffer_copy(uuid.UUID(text).bytes_le)


def method(pointer, index, result, *args):
    table = C.cast(pointer, C.POINTER(C.POINTER(C.c_void_p))).contents
    return C.WINFUNCTYPE(result, C.c_void_p, *args)(table[index])


class GpuMemory:
    def __init__(self, luid):
        self.adapter = C.c_void_p()
        factory = C.c_void_p()
        dxgi = C.WinDLL("dxgi.dll")
        create = dxgi.CreateDXGIFactory1
        create.argtypes = (C.c_void_p, C.POINTER(C.c_void_p))
        create.restype = C.c_long
        if create(guid("1bc6ea02-ef36-464f-bf0c-21ca39e5168a"), C.byref(factory)) < 0:
            raise RuntimeError("CreateDXGIFactory1 failed")
        try:
            enum = method(factory, 26, C.c_long, Luid, C.c_void_p, C.POINTER(C.c_void_p))
            identity = Luid(luid & 0xffffffff, C.c_int32(luid >> 32).value)
            if enum(factory, identity, guid("645967a4-1392-4310-a798-8053ce3e93fd"), C.byref(self.adapter)) < 0:
                raise RuntimeError("DXGI cannot match CUDA adapter LUID")
        finally:
            method(factory, 2, C.c_ulong)(factory)

    def usage(self):
        info = MemoryInfo()
        query = method(self.adapter, 14, C.c_long, C.c_uint32, C.c_uint32, C.POINTER(MemoryInfo))
        if query(self.adapter, 0, 0, C.byref(info)) < 0:
            return None
        return info.usage

    def close(self):
        if self.adapter:
            method(self.adapter, 2, C.c_ulong)(self.adapter)
            self.adapter = C.c_void_p()
