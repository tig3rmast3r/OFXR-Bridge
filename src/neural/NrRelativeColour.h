#pragma once
// NR-only measured-HDR resolve. Colour helpers retain RenoDX/clshortfuse and
// Bjorn Ottosson attribution from DlssNr_Codec.h and licenses/.
// Main legacy codec bytecode remains unchanged for other paths/debug/Colour0.
inline constexpr char kNrRelativeColourShader[]=R"(
cbuffer Params:register(b0) {
 uint gMode;float gWhitePoint;uint gWidth,gHeight;
 float gTransferStrength,gColourStrength;uint gDebugView;float gMaxRatio;
 uint gPassthrough;float gMvScaleX,gMvScaleY;uint gGuideWidth;
 uint gGuideHeight,gCompareMode;float gCompareSplit,gCompareZoom;uint gCompareSwap;
};
static const float3 kLuma=float3(.2126,.7152,.0722);
float3 ClampAp1(float3 color)
{
    const float3x3 bt709_to_ap1 = { 0.613097, 0.339523, 0.047379,
                                    0.070194, 0.916354, 0.013452,
                                    0.020616, 0.109570, 0.869815 };
    const float3x3 ap1_to_bt709 = { 1.705051, -0.621792, -0.083259,
                                    -0.130256, 1.140805, -0.010548,
                                    -0.024003, -0.128969, 1.152972 };
    return mul(ap1_to_bt709, max(mul(bt709_to_ap1, color), float3(0.0, 0.0, 0.0)));
}

// ---------------------------------------------------------------------------------------------
// The composition below (UpgradeToneMap's two-branch ratio, the OkLab hue correction, and the blend
// between a luminance-only result and the model's own colour) is taken from RenoDX's DLSS 5 addon by
// clshortfuse -- https://github.com/clshortfuse/renodx. It is their design, not ours; see
// Licenses/RenoDX_LICENSE.txt. The OkLab matrices are Bjorn Ottosson's published constants and the
// AP1, sRGB and PQ transforms are standard colour science.
// ---------------------------------------------------------------------------------------------

// OkLab, so the model's colour can be reached without its hue being invented on the way. A ratio
// applied to an RGB triple does not move hue, but a difference added to one does -- which is what the
// old composition did, and why a warm subject could come back green. Here the result's chroma is
// rebuilt in the model's own hue direction and only its magnitude is taken from the scaled colour.
float3 CbrtSigned(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3.0); }

float3 ToOkLab(float3 color)
{
    const float3x3 rgb_to_lms = { 0.4122214708, 0.5363325363, 0.0514459929,
                                  0.2119034982, 0.6806995451, 0.1073969566,
                                  0.0883024619, 0.2817188376, 0.6299787005 };
    const float3x3 lms_to_lab = { 0.2104542553, 0.7936177850, -0.0040720468,
                                  1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, color)));
}

float3 FromOkLab(float3 lab)
{
    const float3x3 lab_to_lms = { 1.0, 0.3963377774, 0.2158037573,
                                  1.0, -0.1055613458, -0.0638541728,
                                  1.0, -0.0894841775, -1.2914855480 };
    const float3x3 lms_to_rgb = { 4.0767416621, -3.3077115913, 0.2309699292,
                                  -1.2684380046, 2.6097574011, -0.3413193965,
                                  -0.0041960863, -0.7034186147, 1.7076147010 };
    float3 lms = mul(lab_to_lms, lab);
    return mul(lms_to_rgb, lms * lms * lms);
}

// Takes the hue and the chroma direction from `correct`, and only the chroma magnitude from
// `incorrect`. Scaling a colour by a luminance ratio changes how saturated it reads; this puts the
// saturation back where the model meant it without letting the hue drift.
float3 HueOkLab(float3 incorrect, float3 correct)
{
    float3 incorrectLab = ToOkLab(incorrect);
    const float3 correctLab = ToOkLab(correct);
    const float incorrectChroma = length(incorrectLab.yz);
    const float correctChroma = length(correctLab.yz);
    incorrectLab.yz = correctLab.yz * (correctChroma == 0.0 ? 1.0 : incorrectChroma / correctChroma);
    return ClampAp1(FromOkLab(incorrectLab));
}


Texture2D<float4> gSource:register(t0);
Texture2D<float4> gModel:register(t1);
Texture2D<float4> gOriginal:register(t2);
RWTexture2D<float4> gTarget:register(u0);
SamplerState gLinear:register(s0);
float3 Decode(float3 v) {
 v=saturate(v);return lerp(v/12.92,pow((v+.055)/1.055,2.4),step(.04045,v));
}
float3 Resolve(float3 raw,float3 p,float3 m) {
 // Exact neutral-model invariant, including clipped/quantized proxies.
 // A model that changes nothing must not import proxy tone mapping as colour.
 if(all(p==m)||gTransferStrength<=0)return raw;
 float scale=gPassthrough?1:max(gWhitePoint,1e-4);
 float3 original=raw/scale;
 float oy=dot(original,kLuma),py=dot(p,kLuma),my=dot(m,kLuma);
 if(my<=1e-5)return raw;
 // Preserve accepted codec luminance response and symmetric gain ceiling.
 float ratio=oy<py?oy/max(py,1e-6):(my+max(0,oy-py))/my;
 float3 upgraded=lerp(original,HueOkLab(m*ratio,m),saturate(gTransferStrength));
 float gain=(dot(upgraded,kLuma)+1.0/512)/(oy+1.0/512);
 float ceiling=max(gMaxRatio,1);gain=clamp(gain,1/ceiling,ceiling);
 float3 base=original*gain;
 // Apply only the MODEL'S chromaticity change relative to its own input.
 // Normalizing all colours to equal luminance separates this from tone edits.
 if(oy<=1e-6||py<=1e-5)return max(base*scale,0);
 float3 olab=ToOkLab(original/oy);
 float3 plab=ToOkLab(p/py),mlab=ToOkLab(m/my);
 float amount=saturate(gColourStrength)*saturate(gTransferStrength);
 olab.yz+=(mlab.yz-plab.yz)*amount;
 float3 changed=max(FromOkLab(olab),0);
 float cy=dot(changed,kLuma);
 float3 result=cy>1e-6?changed*(oy*gain/cy):base;
 // This PSO is selected only for the packed R11G11B10 measured-HDR path.
 return min(max(result*scale,0),float3(65024,65024,64512));
}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID) {
 if(id.x>=gWidth||id.y>=gHeight)return;
 float2 screen=(float2(id.xy)+.5)/float2(gWidth,gHeight),uv=screen;
 bool originalSide=false,separator=false,outside=false;
 if(gCompareMode==1) {
  bool left=screen.x<.5;
  uv=float2(left?screen.x*2:(screen.x-.5)*2,screen.y);
  uv=float2((uv.x-.5)/gCompareZoom+.5,(uv.y-.5)*2/gCompareZoom+.5);
  originalSide=(gCompareSwap!=0)!=left;
  separator=abs(screen.x-.5)<1.0/max(gWidth,1);
  outside=any(uv<0)||any(uv>1);
 }else if(gCompareMode==2) {
  originalSide=(gCompareSwap!=0)!=(screen.x<gCompareSplit);
  separator=abs(screen.x-gCompareSplit)<1.0/max(gWidth,1);
 }
 float4 raw=gCompareMode==1?gOriginal.SampleLevel(gLinear,uv,0):gOriginal.Load(int3(id.xy,0));
 float3 p=gSource.SampleLevel(gLinear,uv,0).rgb,m=gModel.SampleLevel(gLinear,uv,0).rgb;
 if(!gPassthrough){p=Decode(p);m=Decode(m);}
 float3 result=originalSide?raw.rgb:Resolve(raw.rgb,p,m);
 if(outside)result=0;
 if(separator)result=gWhitePoint.xxx;
 gTarget[id.xy]=float4(max(result,0),raw.a);
}
)";
