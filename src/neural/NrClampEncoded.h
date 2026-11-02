#pragma once
// Adapted from wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass v0.8.10,
// 6125a75b37f7e2a408e31934f4827e5660429203:
// OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl, lines 665-670 (mode 8).
// https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/blob/6125a75b37f7e2a408e31934f4827e5660429203/OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl#L665
// GPL-3.0; the repository's OptiScaler licence remains in src/neural/licenses/.
// No colour transform or resize: sanitize each encoded RGB channel, preserve alpha.
inline constexpr char kNrClampEncodedShader[]=R"(
cbuffer Params:register(b0) {
 uint mode;float white;uint width,height;
};
Texture2D<float4> source:register(t0);
RWTexture2D<float4> target:register(u0);
float bounded(float value) { return isfinite(value)?saturate(value):0.5; }
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 if(id.x>=width||id.y>=height)return;
 float4 raw=source.Load(int3(id.xy,0));
 target[id.xy]=float4(bounded(raw.r),bounded(raw.g),bounded(raw.b),raw.a);
}
)";
