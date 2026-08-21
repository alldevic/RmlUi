#include "shader_common.hlsli"

// Resolves a multisampled layer by hand, so that it can be done over a region rather than over the whole attachment.
//
// SDL resolves a layer as the store operation of a render pass, and a pass covers its attachment entirely -- there is
// no rectangle to give it. The reference backend has no such trouble: it resolves with glBlitFramebuffer under
// glScissor, and so pays only for the area of the element being composited. This shader is how that is matched here:
// the resolve becomes an ordinary draw, and the active scissor confines it.
//
// The layer is bound as a storage texture rather than through a sampler. A multisample texture holds samples rather
// than pixels, so there is nothing for a sampler to filter and the sample index has to be given explicitly; reading
// one at all requires an SDL that allows it, which is what supports_shader_resolve in the renderer decides.
Texture2DMS<float4> Source : register(t0, space2);

float4 main(PostVaryings input) : SV_Target0 {
    // Both the size and the sample count come from the texture rather than from a constant buffer. The sample count
    // is chosen by the device and differs between them, and this is one less thing that has to agree with the CPU.
    uint width, height, num_samples;
    Source.GetDimensions(width, height, num_samples);

    // The layer and the target it resolves into are the same size, so the texture coordinate lands on the texel this
    // fragment stands for. Sampled at pixel centres, the product is x + 0.5, which truncates to the texel index.
    int2 coord = int2(input.TexCoord * float2(width, height));

    // A plain average, which is what a fixed-function resolve of a non-sRGB format does. The colours are
    // premultiplied, and averaging premultiplied colours is the same operation, so no unpremultiply is needed.
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (uint i = 0; i < num_samples; i++)
        sum += Source.Load(coord, int(i));
    return sum / float(num_samples);
}
