Texture2D<float4> VideoTexture : register(t0, space2);
SamplerState     PointSampler : register(s0, space2);

struct FragmentInput {
    float2 TexCoord : TEXCOORD0;
};

float4 cubic(float x) {
    float x2 = x * x;
    float x3 = x2 * x;
    float4 w;
    w.x = -x3 + 2.0 * x2 - x;
    w.y = 3.0 * x3 - 5.0 * x2 + 2.0;
    w.z = -3.0 * x3 + 4.0 * x2 + x;
    w.w = x3 - x2;
    return w * 0.5;
}

// 4-Lookup Fast Bicubic Shader
float4 main(FragmentInput input) : SV_Target0 {
    float2 texSize;
    VideoTexture.GetDimensions(texSize.x, texSize.y);
    float2 invTexSize = 1.0 / texSize;

    float2 texCoord = input.TexCoord * texSize - 0.5;
    float2 fxy = frac(texCoord);
    texCoord -= fxy;

    float4 xcount = cubic(fxy.x);
    float4 ycount = cubic(fxy.y);

    float4 w = float4(xcount.x + xcount.y, xcount.z + xcount.w,
                      ycount.x + ycount.y, ycount.z + ycount.w);

    float4 offset = float4(xcount.y / w.x, xcount.w / w.y,
                           ycount.y / w.z, ycount.w / w.w);

    float4 samplePos = float4(
        (texCoord.x - 1.0 + offset.x) * invTexSize.x,
        (texCoord.x + 1.0 + offset.y) * invTexSize.x,
        (texCoord.y - 1.0 + offset.z) * invTexSize.y,
        (texCoord.y + 1.0 + offset.w) * invTexSize.y
    );

    return (VideoTexture.Sample(PointSampler, float2(samplePos.x, samplePos.z)) * w.x * w.z +
            VideoTexture.Sample(PointSampler, float2(samplePos.y, samplePos.z)) * w.y * w.z +
            VideoTexture.Sample(PointSampler, float2(samplePos.x, samplePos.w)) * w.x * w.w +
            VideoTexture.Sample(PointSampler, float2(samplePos.y, samplePos.w)) * w.y * w.w) /
           (w.x * w.z + w.y * w.z + w.x * w.w + w.y * w.w);
}