// A D3D11 game's packed depth-stencil (R24G8, R32G8X24) has no format D3D11
// can share with D3D12, so its depth plane goes over as one float per texel.
// Runs on the game's immediate context inside its DLSS evaluation. 0.2 V521.
Texture2D<float> Depth : register(t0);
RWTexture2D<float> Unpacked : register(u0);

[numthreads(8, 8, 1)]
void UnpackDepth(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    Unpacked.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    Unpacked[id.xy] = Depth.Load(int3(id.xy, 0));
}
