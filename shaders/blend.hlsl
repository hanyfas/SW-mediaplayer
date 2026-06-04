// BGRA blend pass – images and software-decoded BGRA frames
Texture2D<float4> tex  : register(t0);
SamplerState      samp : register(s0);

cbuffer CB : register(b0) {
    float alpha;
    float3 pad;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = tex.Sample(samp, uv);
    c.a *= alpha;
    return c;
}
