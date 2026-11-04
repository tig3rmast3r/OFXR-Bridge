"""Drive only our own progress-window fixture; no download, headset or GPU."""
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
user = C.WinDLL("user32", use_last_error=True)
CALLBACK = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
user.EnumWindows.argtypes = [CALLBACK, W.LPARAM]
user.EnumChildWindows.argtypes = [W.HWND, CALLBACK, W.LPARAM]
user.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
user.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
user.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
user.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
user.SendMessageW.restype = W.LPARAM
user.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]


def text(window, cls=False):
    buffer = C.create_unicode_buffer(1024)
    (user.GetClassNameW if cls else user.GetWindowTextW)(window, buffer, len(buffer))
    return buffer.value


def windows(pid):
    out = []
    @CALLBACK
    def visit(window, unused):
        owner = W.DWORD();user.GetWindowThreadProcessId(window, C.byref(owner))
        if owner.value == pid and text(window).startswith("OFXR 0.3.3 - Pre-compile"):
            out.append(window)
        return True
    user.EnumWindows(visit, 0)
    return out


@unittest.skipUnless(os.name == "nt", "Windows Forms")
class ProgressTests(unittest.TestCase):
    def run_fixture(self, cancel):
        with tempfile.TemporaryDirectory(prefix="OFXR precompile UI spaces ") as temporary:
            folder = Path(temporary);tools = folder / "package/tools";tools.mkdir(parents=True)
            shutil.copy2(SOURCE / "tools/Precompile-Rife.ps1", tools)
            root = folder / "runtime";root.mkdir()
            subprocess.run(["cmd", "/c", "mklink", "/J", str(root / "python"), str(Path(sys.executable).parent)], capture_output=True, check=True)
            (tools / "precompile_rife.py").write_text('''import argparse,json,time
from pathlib import Path
p=argparse.ArgumentParser()
for n in ('root','result','progress','cancel','log'):p.add_argument('--'+n,required=True)
p.add_argument('--parent-watchdog',action='store_true')
a=p.parse_args()
def progress(current,done):
 dest=Path(a.progress);tmp=dest.with_suffix('.tmp');tmp.write_text(json.dumps(dict(status='running',current=current,completed=done,total=2,message='fixture compilation')));tmp.replace(dest)
progress(1,0)
for i in range(30):
 if Path(a.cancel).exists():break
 time.sleep(.1)
cancelled=Path(a.cancel).exists()
if not cancelled:
 progress(2,1);time.sleep(.3)
result=dict(schema=1,status='error' if cancelled else 'ready',message='Cancelled fixture' if cancelled else 'Ready fixture',root=a.root,backend='fixture',installed_bytes=0,required_free_bytes=0,log_path=a.log)
Path(a.result).write_text(json.dumps(result))
raise SystemExit(1 if cancelled else 0)
''')
            result = folder / "result.json"
            command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(tools / "Precompile-Rife.ps1"), "-Root", str(root), "-ResultJson", str(result)]
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 15;window = None;saw_count = False
                while time.monotonic() < deadline and process.poll() is None:
                    found = windows(process.pid)
                    if found:
                        window = found[0];labels = []
                        @CALLBACK
                        def visit(child, unused):
                            labels.append(text(child));return True
                        user.EnumChildWindows(window, visit, 0)
                        if any("1/2" in label for label in labels):saw_count = True;break
                    time.sleep(.05)
                self.assertTrue(window and saw_count, "actual form shows the 1/2 combination counter")
                if cancel:
                    user.PostMessageW(window, 0x10, 0, 0)
                while not result.exists() and time.monotonic() < deadline:time.sleep(.05)
                self.assertTrue(result.is_file(), "owned worker completes or acknowledges cancel")
                time.sleep(.4)
                if not cancel:
                    positions = []
                    @CALLBACK
                    def bar(child, unused):
                        if "msctls_progress32" in text(child, True).lower():
                            positions.append(user.SendMessageW(child, 0x408, 0, 0))
                        return True
                    user.EnumChildWindows(window, bar, 0)
                    self.assertIn(2, positions, "real progress bar reaches its maximum")
                user.PostMessageW(window, 0x10, 0, 0)
                stdout, stderr = process.communicate(timeout=8)
                self.assertEqual(process.returncode, 1 if cancel else 0, stderr.decode(errors="replace"))
                self.assertEqual(json.loads(result.read_text())["status"], "error" if cancel else "ready")
            finally:
                if process.poll() is None:
                    # Only this test-owned process tree; never a user process.
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
                    process.wait(timeout=5)

    def test_actual_progress_window(self):self.run_fixture(False)
    def test_actual_cancel_window(self):self.run_fixture(True)


if __name__ == "__main__":unittest.main()
