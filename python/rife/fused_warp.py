"""RIFE4.4 FP16 samples with the original FP32 coordinate calculation.

This combines the tested intermediate warp operations; it does not quantize
coordinates to FP16 or change the final native-colour D3D12 reconstruction.
"""
import ctypes as C
import os
from pathlib import Path
import sys

import torch
from cuda.bindings import driver as cu, nvrtc

KERNEL = r'''
extern "C" __device__ __forceinline__ float half_read(const unsigned short* p,int k){float x;asm("cvt.f32.f16 %0, %1;":"=f"(x):"h"(p[k]));return x;}
extern "C" __device__ __forceinline__ unsigned short half_write(float x){unsigned short h;asm("cvt.rn.f16.f32 %0, %1;":"=h"(h):"f"(x));return h;}
extern "C" __global__ void fused_warp(const unsigned short* src,const unsigned short* flow,const float* grid,const float* div,unsigned short* out,int N,int C,int H,int W,int sn,int sc,int sh,int sw){
 int idx=blockIdx.x*blockDim.x+threadIdx.x;int count=N*H*W;if(idx>=count)return;
 int wh=W*H;int b=idx/wh;int xy=idx%wh;
 float fx=half_read(flow,b*2*wh+xy)/div[0];
 float fy=half_read(flow,b*2*wh+wh+xy)/div[1];
 float gx=grid[xy]+fx,gy=grid[wh+xy]+fy;
 float x=((gx+1.f)/2.f)*(W-1),y=((gy+1.f)/2.f)*(H-1);
 x=fminf(fmaxf(x,0.f),W-1.f);y=fminf(fmaxf(y,0.f),H-1.f);
 int x0=(int)floorf(x),y0=(int)floorf(y),x1=min(x0+1,W-1),y1=min(y0+1,H-1);
 float xw=x-x0,yw=y-y0;
 float w00=(1.f-xw)*(1.f-yw),w10=xw*(1.f-yw),w01=(1.f-xw)*yw,w11=xw*yw;
 for(int c=0;c<C;c++){
 int base=b*sn+c*sc;
 float v=half_read(src,base+y0*sh+x0*sw)*w00;
 v+=half_read(src,base+y0*sh+x1*sw)*w10;
 v+=half_read(src,base+y1*sh+x0*sw)*w01;
 v+=half_read(src,base+y1*sh+x1*sw)*w11;
 out[(b*C+c)*wh+xy]=half_write(v);
 }
}
'''


def check(result):
    if int(result[0]) != 0:
        raise RuntimeError("CUDA warp operation failed: " + str(result[0]))
    return result[1] if len(result) == 2 else result[1:]


class FusedWarp:
    def __init__(self):
        self._dll = self._dll_directory = None
        if os.name == "nt":
            library = Path(sys.executable).resolve().parent / "Lib" / "site-packages" / "torch" / "lib"
            if library.is_dir():
                self._dll_directory = os.add_dll_directory(str(library))
                nvrtc_dll = library / "nvrtc64_120_0.dll"
                if nvrtc_dll.is_file():
                    self._dll = C.WinDLL(str(nvrtc_dll))
        properties = torch.cuda.get_device_properties(torch.cuda.current_device())
        architecture = f"--gpu-architecture=compute_{properties.major}{properties.minor}".encode("ascii")
        program = check(nvrtc.nvrtcCreateProgram(KERNEL.encode("utf-8"), b"ofxr-rife-warp.cu", 0, [], []))
        try:
            options = [architecture, b"--fmad=false", b"--std=c++14"]
            compiled = nvrtc.nvrtcCompileProgram(program, len(options), options)
            size = check(nvrtc.nvrtcGetProgramLogSize(program))
            # Writable bytearrays are required by the CUDA Python bindings.
            # Never pass a bytes/interned-string buffer to an output function.
            log = bytearray(size)
            check(nvrtc.nvrtcGetProgramLog(program, log))
            if int(compiled[0]) != 0:
                raise RuntimeError(log.decode("utf-8", errors="replace"))
            size = check(nvrtc.nvrtcGetPTXSize(program))
            ptx = bytearray(size)
            check(nvrtc.nvrtcGetPTX(program, ptx))
        finally:
            check(nvrtc.nvrtcDestroyProgram(program))
        self.module = check(cu.cuModuleLoadData(ptx))
        self.function = check(cu.cuModuleGetFunction(self.module, b"fused_warp"))

    def __call__(self, source, flow, divisor, grid):
        if source.dtype != torch.float16:
            raise ValueError("RIFE fused warp requires FP16 samples")
        flow = flow.contiguous()
        output = torch.empty_like(source, memory_format=torch.contiguous_format)
        n, channels, height, width = source.shape
        arguments = [C.c_void_p(source.data_ptr()), C.c_void_p(flow.data_ptr()),
                     C.c_void_p(grid.data_ptr()), C.c_void_p(divisor.data_ptr()), C.c_void_p(output.data_ptr()),
                     C.c_int(n), C.c_int(channels), C.c_int(height), C.c_int(width),
                     *[C.c_int(stride) for stride in source.stride()]]
        pointers = (C.c_void_p * len(arguments))(*[C.addressof(value) for value in arguments])
        check(cu.cuLaunchKernel(self.function, (n * height * width + 255) // 256, 1, 1, 256, 1, 1, 0,
                               cu.CUstream(torch.cuda.current_stream().cuda_stream), C.addressof(pointers), 0))
        return output
