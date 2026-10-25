// SPDX-License-Identifier: GPL-2.0-only
#include <metal_stdlib>
using namespace metal;
// 10-bit presentation (BGR10A2Unorm, sRGB-tagged layer; macOS maps it to the
// panel's P3 gamut exactly). That format has no hardware sRGB encode, so the
// shader encodes; 8-bit sRGB targets (the self-test) keep hardware encoding.
constant bool encodeOutput [[function_constant(0)]];
constant bool encodeOutputEnabled=is_function_constant_defined(encodeOutput) && encodeOutput;
static float3 srgb_encode(float3 c) {
    c=clamp(c,0.0,1.0);
    return select(1.055*pow(c,1.0/2.4)-.055,c*12.92,c<=.0031308);
}
struct Raster { float4 position [[position]]; float2 uv; };
vertex Raster doom_vertex(uint v [[vertex_id]]) {
    const float2 p[] = {float2(-1,-1),float2(3,-1),float2(-1,3)};
    Raster r; r.position=float4(p[v],0,1); r.uv=float2((p[v].x+1)*.5,(1-p[v].y)*.5); return r;
}
// Linear color of a palette index under its stored light value.
static float3 doom_lit(uint index, uint shade, constant float4 *palette, constant uchar *colormap) {
    float3 linear=palette[index].rgb;
    if(shade&0x8000u) {
        // RGB lighting: unlit texel times the continuous COLORMAP brightness
        // (quarter levels), used only if the indexed pixel still matches it.
        uint texel=shade&255u,q=(shade>>8)&127u;
        if(colormap[(q>>2)*256u+texel]==index) linear=palette[texel].rgb*pow(max(1.0-float(q)/128.0,0.0),2.2);
    } else if((shade>>8)&127u) {
        // Blend only adjacent original light-table colors in linear SDR.
        linear=mix(linear,palette[shade&255u].rgb,float((shade>>8)&127u)/127.0);
    }
    return linear;
}
// Weapon art over the view at this drawable position, if any: its palette
// index (always unlit), with p the view pixel underneath.
static bool doom_viewmodel(uint2 p, float2 position, texture2d<uint> indices, texture2d<uint> viewmodel,
                           constant float4 &viewmodelRect, constant uchar *fuzzMap, thread uint &index) {
    float2 model=(position-viewmodelRect.xy)/viewmodelRect.zw;
    if(!all(model>=0) || !all(model<1)) return false;
    uint2 q=uint2(model*float2(320,200)); uint art=viewmodel.read(q).r;
    // Weapon art: palette index, or 0x100 for fuzz.
    if(art<256) { index=art; return true; }
    if(art==256) {
        int offset=((q.x+q.y)%3)-1;
        uint2 neighbor=uint2(p.x,uint(clamp(int(p.y)+offset,0,int(indices.get_height())-1)));
        index=fuzzMap[indices.read(neighbor).r];
        return true;
    }
    return false;
}
// Lit linear color of one framebuffer pixel; position is in drawable pixels.
static float3 doom_pixel(uint2 p, float2 position, texture2d<uint> indices, constant float4 *palette,
                         texture2d<uint> viewmodel, constant float4 &viewmodelRect, constant uchar *fuzzMap,
                         texture2d<uint> lighting, constant uchar *colormap) {
    uint index=indices.read(p).r;
    uint shade=lighting.read(p).r;
    if(doom_viewmodel(p,position,indices,viewmodel,viewmodelRect,fuzzMap,index)) shade=0;
    return doom_lit(index,shade,palette,colormap);
}
// Adaptive resolution: a view rendered below the drawable's size is resampled
// bilinearly from four lit texels instead of repeating whole texels, so it
// stays smooth at any scale; the weapon art keeps the drawable's resolution.
static float3 doom_filtered(float2 uv, float2 position, texture2d<uint> indices, constant float4 *palette,
                            texture2d<uint> viewmodel, constant float4 &viewmodelRect, constant uchar *fuzzMap,
                            texture2d<uint> lighting, constant uchar *colormap) {
    float2 size=float2(indices.get_width(),indices.get_height());
    float2 t=clamp(uv*size-.5,float2(0),size-1);
    uint2 a=uint2(t),b=min(a+1,uint2(size)-1);
    float2 f=t-float2(a);
    float3 c00=doom_lit(indices.read(a).r,lighting.read(a).r,palette,colormap);
    float3 c10=doom_lit(indices.read(uint2(b.x,a.y)).r,lighting.read(uint2(b.x,a.y)).r,palette,colormap);
    float3 c01=doom_lit(indices.read(uint2(a.x,b.y)).r,lighting.read(uint2(a.x,b.y)).r,palette,colormap);
    float3 c11=doom_lit(indices.read(b).r,lighting.read(b).r,palette,colormap);
    uint index;
    uint2 p=uint2(clamp(uv,float2(0),float2(.999999))*size);
    if(doom_viewmodel(p,position,indices,viewmodel,viewmodelRect,fuzzMap,index)) return doom_lit(index,0,palette,colormap);
    return mix(mix(c00,c10,f.x),mix(c01,c11,f.x),f.y);
}
fragment float4 doom_fragment(Raster in [[stage_in]], texture2d<uint> indices [[texture(0)]],
                              constant float4 *palette [[buffer(0)]],
                              texture2d<uint> viewmodel [[texture(1)]],
                              constant float4 &viewmodelRect [[buffer(1)]],
                              constant uchar *fuzzMap [[buffer(2)]],
                              texture2d<uint> lighting [[texture(2)]],
                              constant uchar *colormap [[buffer(3)]],
                              constant uint &filtered [[buffer(4)]]) {
    uint2 p=uint2(clamp(in.uv,float2(0),float2(.999999))*float2(indices.get_width(),indices.get_height()));
    float3 linear=filtered?doom_filtered(in.uv,in.position.xy,indices,palette,viewmodel,viewmodelRect,fuzzMap,lighting,colormap)
                          :doom_pixel(p,in.position.xy,indices,palette,viewmodel,viewmodelRect,fuzzMap,lighting,colormap);
    // Sub-step ordered noise in display space hides quantization bands.
    float2 cell=floor(in.position.xy);
    float noise=fract(52.9829189*fract(dot(cell,float2(.06711056,.00583715))))-.5;
    if(encodeOutputEnabled) return float4(srgb_encode(linear)+noise/1023.0,1);
    float3 display=pow(max(linear,0.0),1.0/2.2)+noise/255.0;
    return float4(pow(max(display,0.0),2.2),1);
}
fragment float4 doom_status_fragment(Raster in [[stage_in]], texture2d<uint> indices [[texture(0)]],
                                     constant float4 *palette [[buffer(0)]],constant float4 &shape [[buffer(1)]]) {
    uint2 p=uint2(clamp(in.uv,float2(0),float2(.999999))*float2(indices.get_width(),indices.get_height()));
    float2 edge=abs((in.uv-.5)*shape.xy)-(shape.xy*.5-shape.z);
    float distance=length(max(edge,0.0))+min(max(edge.x,edge.y),0.0)-shape.z;
    float3 color=palette[indices.read(p).r].rgb;
    return float4(encodeOutputEnabled?srgb_encode(color):color,1-smoothstep(-.5,.5,distance));
}
// Native menu panel: premultiplied, rasterized at drawable resolution.
fragment float4 menu_panel_fragment(Raster in [[stage_in]], texture2d<float> panel [[texture(0)]], constant float &opacity [[buffer(0)]]) {
    uint2 p=uint2(clamp(in.uv,float2(0),float2(.999999))*float2(panel.get_width(),panel.get_height()));
    return panel.read(p)*opacity;
}
fragment float4 menu_shade_fragment(Raster in [[stage_in]], constant float4 &shade [[buffer(0)]]) { return shade; }
