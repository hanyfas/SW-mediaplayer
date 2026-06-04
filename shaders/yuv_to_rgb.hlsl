// NV12 → RGB pixel shader (BT.709 limited range)
// Used as a reference; the renderer compiles the embedded string directly.
// Compile: fxc /T ps_5_0 /E main /Fo yuv_to_rgb.cso yuv_to_rgb.hlsl

Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

cbuffer CB : register(b0) {
    float alpha;
    float3 pad;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float  y  = texY.Sample(samp, uv);
    float2 uv2 = texUV.Sample(samp, uv);
    float  cb = uv2.x - 0.5;
    float  cr = uv2.y - 0.5;

    // BT.709 limited range (16-235 luma, 16-240 chroma)
    y  = (y  - 16.0/255.0) * (255.0/219.0);
    float r = saturate(y + 1.5748 * cr);
    float g = saturate(y - 0.1873 * cb - 0.4681 * cr);
    float b = saturate(y + 1.8556 * cb);

    return float4(r, g, b, alpha);
}
