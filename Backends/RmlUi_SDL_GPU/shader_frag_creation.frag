#include "shader_common.hlsli"

// "Creation" by Danilo Guanabara, based on: https://www.shadertoy.com/view/XsXXDn
cbuffer UniformBlockCreation : register(b0, space3) {
    float2 Dimensions : packoffset(c0);
    float Value : packoffset(c0.z);
};

float4 main(Varyings input) : SV_Target0 {
    float t = Value;
    float3 c;
    float l;
    for (int i = 0; i < 3; i++) {
        float2 p = input.TexCoord;
        float2 uv = p;
        p -= .5;
        p.x *= Dimensions.x / Dimensions.y;
        float z = t + float(i) * .07;
        l = length(p);
        uv += p / l * (sin(z) + 1.) * abs(sin(l * 9. - z - z));
        // frac() is HLSL's x - floor(x), which is what glsl_mod() computes for a divisor of one, minus the divide.
        c[i] = .01 / length(frac(uv) - .5);
    }
    return float4(c / l, input.Color.a);
}
