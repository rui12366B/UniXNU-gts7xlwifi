// gts7xlwifi: probe of RGB10A8_2P_XR10 (MTLPixelFormat 550) sampling on the A650 (the stripes, 2026-11-05). The plugin fills a 64x2
// texture whose red code is 384 + 8x (R = 8x/510), runs this once and writes the numbers into a BootLog row.
#include <metal_stdlib>
using namespace metal;
kernel void xr10_probe(texture2d<float, access::sample> t [[texture(0)]], device float *out [[buffer(0)]],
                       uint i [[thread_position_in_grid]]) {
    constexpr sampler lin(coord::normalized, filter::linear, address::clamp_to_edge);
    constexpr sampler near(coord::normalized, filter::nearest, address::clamp_to_edge);
    const uint xs[4] = {4, 20, 36, 60};
    if (i >= 4) return;
    const float u = (float(xs[i]) + 0.5f) / 64.0f;
    out[i * 4 + 0] = t.sample(lin, float2(u, 0.25f)).r;
    out[i * 4 + 1] = t.sample(near, float2(u, 0.25f)).r;
    out[i * 4 + 2] = t.read(uint2(xs[i], 0)).r;
    out[i * 4 + 3] = i == 0 ? float(t.get_width()) : i == 1 ? float(t.get_height()) : 0.0f;
}

// Round 2 (periph8): what CoreAnimation does and the compute probe did not. A full-screen triangle; gradient_fs renders R = x/64 at the
// pixel centre (x 4 20 36 60 -> 0.070 0.320 0.570 0.945) into a private 550 target, read back by xr10_probe; sample_fs samples a 550
// texture in the fragment stage at the pixel's normalized position (the implicit, derivative-based level), into an RGBA32Float target.
struct T970Out {float4 position [[position]];};
vertex T970Out fullscreen_vs(uint vid [[vertex_id]]) {
    const float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    T970Out o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}
fragment float4 gradient_fs(T970Out in [[stage_in]]) {
    return float4(in.position.x / 64.0f, 0.5f, 0.25f, 1.0f);
}
fragment float4 sample_fs(T970Out in [[stage_in]], texture2d<float, access::sample> t [[texture(0)]]) {
    constexpr sampler lin(coord::normalized, filter::linear, address::clamp_to_edge);
    return t.sample(lin, in.position.xy / float2(64.0f, 2.0f));
}

// Round 3 (periph9): the sizes and placement CoreAnimation uses. xr10_read_at reads four texels of row p.y (read, and linear/nearest
// samples at their centres) of a texture of p.size; gradient_w renders R = x / w at the pixel centre into any size.
struct T970At {uint4 xs; float2 size; uint y; uint pad;};
kernel void xr10_read_at(texture2d<float, access::sample> t [[texture(0)]], device float *out [[buffer(0)]],
                         constant T970At &p [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    constexpr sampler lin(coord::normalized, filter::linear, address::clamp_to_edge);
    constexpr sampler near(coord::normalized, filter::nearest, address::clamp_to_edge);
    if (i >= 4) return;
    const uint x = p.xs[i];
    const float2 uv = (float2(float(x), float(p.y)) + 0.5f) / p.size;
    out[i * 4 + 0] = t.sample(lin, uv).r;
    out[i * 4 + 1] = t.sample(near, uv).r;
    out[i * 4 + 2] = t.read(uint2(x, p.y)).r;
    out[i * 4 + 3] = i == 0 ? float(t.get_width()) : i == 1 ? float(t.get_height()) : 0.0f;
}
fragment float4 gradient_w(T970Out in [[stage_in]], constant float &w [[buffer(0)]]) {
    return float4(in.position.x / w, 0.5f, 0.25f, 1.0f);
}
