"""Windows same-runtime reader/writer lease; no polling or persistent process."""
import ctypes
from pathlib import Path


class RuntimeBusy(RuntimeError):
    pass


class RuntimeLease:
    def __init__(self, root, exclusive=False):
        self.handle = None
        self.path = Path(root).resolve().parent / ("." + Path(root).resolve().name + ".lease")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.kernel = kernel
        handle = kernel.CreateFileW(str(self.path), 0xC0000000 if exclusive else 0x80000000,
                                    0 if exclusive else 1, None, 4, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            error = ctypes.get_last_error()
            if error in (32, 33):
                raise RuntimeBusy("RIFE is in use or another installation/check is running. Close the game and try again.")
            raise OSError(error, "Unable to acquire RIFE runtime lease", str(self.path))
        self.handle = handle

    def close(self):
        if self.handle is not None:
            self.kernel.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()

    def __del__(self):
        self.close()
