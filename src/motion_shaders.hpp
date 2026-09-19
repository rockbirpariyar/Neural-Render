#pragma once

// Offline-compiled Shader Model 3 sources. Keep the runtime free of HLSL compilation.
namespace enr::motion_shader_source
{
    // A complete eight-by-eight box avoids parity aliasing on one-pixel motion.
    // This work runs only on the one-eighth-resolution grid. Color stays in the game's encoded space;
    // matching does not require an sRGB encode/decode round trip.
    constexpr char downsample_source[] = R"(
sampler2D source_image : register(s0);
float4 color_size : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 sum = 0;
    [unroll] for (int y = 0; y < 8; ++y)
    [unroll] for (int x = 0; x < 8; ++x)
        sum += tex2D(source_image, uv + (float2(x,y)-3.5)*color_size.zw).rgb;
    return float4(sum * (1.0/64.0), 1);
}
)";

    constexpr char coarse_source[] = R"(
sampler2D current_image : register(s0);
sampler2D previous_image : register(s1);
float4 color_size : register(c0);
float4 grid_size : register(c1);

float difference(float3 a, float3 b) { return dot(abs(a-b), float3(0.333333,0.333333,0.333333)); }
float cost(float2 p, float3 a, float3 b, float3 c, float3 d, float3 e,
    float3 f, float3 g, float3 i, float3 j)
{
    float2 h = float2(grid_size.z,0), v = float2(0,grid_size.w);
    return (difference(a,tex2Dlod(previous_image,float4(p,0,0)).rgb) +
        difference(b,tex2Dlod(previous_image,float4(p-h,0,0)).rgb) +
        difference(c,tex2Dlod(previous_image,float4(p+h,0,0)).rgb) +
        difference(d,tex2Dlod(previous_image,float4(p-v,0,0)).rgb) +
        difference(e,tex2Dlod(previous_image,float4(p+v,0,0)).rgb) +
        difference(f,tex2Dlod(previous_image,float4(p-h-v,0,0)).rgb) +
        difference(g,tex2Dlod(previous_image,float4(p+h-v,0,0)).rgb) +
        difference(i,tex2Dlod(previous_image,float4(p-h+v,0,0)).rgb) +
        difference(j,tex2Dlod(previous_image,float4(p+h+v,0,0)).rgb)) * (1.0/9.0);
}
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 h = float2(grid_size.z,0), v = float2(0,grid_size.w);
    float2 margin = grid_size.zw * 1.51;
    if (any(uv <= margin) || any(uv >= 1-margin)) return float4(0,0,0,1);
    float3 a=tex2D(current_image,uv).rgb, b=tex2D(current_image,uv-h).rgb;
    float3 c=tex2D(current_image,uv+h).rgb, d=tex2D(current_image,uv-v).rgb;
    float3 e=tex2D(current_image,uv+v).rgb;
    float3 f=tex2D(current_image,uv-h-v).rgb, g=tex2D(current_image,uv+h-v).rgb;
    float3 i=tex2D(current_image,uv-h+v).rgb, j=tex2D(current_image,uv+h+v).rgb;
    float variation=dot((b-a)*(b-a)+(c-a)*(c-a)+(d-a)*(d-a)+(e-a)*(e-a),float3(0.083333,0.083333,0.083333));
    if (variation < 0.00005) return float4(0,0,0,1);
    float center = cost(uv,a,b,c,d,e,f,g,i,j);
    if (center < 0.0003) return float4(0,0,1,1);
    float best=center, second=1, ranked=center;
    float2 motion=0;
    [loop] for (int y=-4; y<=4; ++y)
    [loop] for (int x=-4; x<=4; ++x)
    {
        if (x==0 && y==0) continue;
        float2 delta=float2(x,y), p=uv+delta*grid_size.zw;
        if (any(p<=margin) || any(p>=1-margin)) continue;
        float error=cost(p,a,b,c,d,e,f,g,i,j);
        float score=error+0.00004*dot(abs(delta),float2(1,1));
        if (score<ranked) { second=best; best=error; ranked=score; motion=delta; }
        else second=min(second,error);
    }
    bool identical=center<0.0003;
    if (identical) { motion=0; best=center; }
    float gap=max(0,second-best);
    if (best>0.12 || (!identical && gap<0.0003)) return float4(0,0,0,1);
    float confidence=identical ? 1 : saturate(gap/(0.01+best));
    return float4(motion*grid_size.zw*color_size.xy,max(0.02,confidence),1);
}
)";

    // Refine each block in original color samples, rather than repeatedly
    // warping history. Two-pixel search followed by a one-pixel search yields
    // integer-pixel correspondences while keeping the candidate count bounded.
    constexpr char refine_source[] = R"(
sampler2D current_image : register(s0);
sampler2D previous_image : register(s1);
sampler2D current_depth : register(s2);
sampler2D previous_depth : register(s3);
sampler2D coarse_motion : register(s4);
float4 color_size : register(c0);
float4 grid_size : register(c1);
float4 depth_options : register(c2);

void order(inout float2 a, inout float2 b)
{
    float2 low=min(a,b); b=max(a,b); a=low;
}
float2 neighbor_motion(float2 uv, float2 fallback)
{
    float4 value=tex2Dlod(coarse_motion,float4(uv,0,0));
    return value.b>0 ? value.rg : fallback;
}
float2 coherent_origin(float2 uv, float2 center)
{
    float2 h=float2(grid_size.z,0), v=float2(0,grid_size.w);
    float2 p0=neighbor_motion(uv-h-v,center), p1=neighbor_motion(uv-v,center);
    float2 p2=neighbor_motion(uv+h-v,center), p3=neighbor_motion(uv-h,center);
    float2 p4=center, p5=neighbor_motion(uv+h,center);
    float2 p6=neighbor_motion(uv-h+v,center), p7=neighbor_motion(uv+v,center);
    float2 p8=neighbor_motion(uv+h+v,center);
    // Fixed 25-comparator sorting network: component-wise median of nine.
    // Coherent camera/region motion suppresses unrelated low-cost patch matches.
    // The final full-resolution patch and matched-depth tests still decide validity.
    order(p0,p3); order(p1,p7); order(p2,p5); order(p4,p8);
    order(p0,p7); order(p2,p4); order(p3,p8); order(p5,p6);
    order(p0,p2); order(p1,p3); order(p4,p5); order(p7,p8);
    order(p1,p4); order(p3,p6); order(p5,p7);
    order(p0,p1); order(p2,p4); order(p3,p5); order(p6,p8);
    order(p2,p3); order(p4,p5); order(p6,p7);
    order(p1,p2); order(p3,p4); order(p5,p6);
    return round(p4);
}
float difference(float3 a, float3 b) { return dot(abs(a-b),float3(0.333333,0.333333,0.333333)); }
float cost(float2 p, float3 a, float3 b, float3 c, float3 d, float3 e)
{
    float2 h=float2(color_size.z*3,0), v=float2(0,color_size.w*3);
    return (difference(a,tex2Dlod(previous_image,float4(p,0,0)).rgb) +
        difference(b,tex2Dlod(previous_image,float4(p-h,0,0)).rgb) +
        difference(c,tex2Dlod(previous_image,float4(p+h,0,0)).rgb) +
        difference(d,tex2Dlod(previous_image,float4(p-v,0,0)).rgb) +
        difference(e,tex2Dlod(previous_image,float4(p+v,0,0)).rgb)) * 0.2;
}
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 coarse=tex2D(coarse_motion,uv);
    if (coarse.b<=0) return float4(0,0,0,1);
    float2 margin=color_size.zw*3.51;
    if (any(uv<=margin) || any(uv>=1-margin)) return float4(0,0,0,1);
    float2 h=float2(color_size.z*3,0), v=float2(0,color_size.w*3);
    float3 a=tex2D(current_image,uv).rgb, b=tex2D(current_image,uv-h).rgb;
    float3 c=tex2D(current_image,uv+h).rgb, d=tex2D(current_image,uv-v).rgb;
    float3 e=tex2D(current_image,uv+v).rgb;
    float variation=dot((b-a)*(b-a)+(c-a)*(c-a)+(d-a)*(d-a)+(e-a)*(e-a),float3(0.083333,0.083333,0.083333));
    if (variation<0.0002) return float4(0,0,0,1);
    float2 origin=coherent_origin(uv,coarse.rg), motion=origin;
    float best=1, ranked=1, second=1;
    float center=cost(uv,a,b,c,d,e);
    bool identical=center<0.0003;
    if (!identical)
    {
    [loop] for (int y=-2; y<=2; ++y)
    [loop] for (int x=-2; x<=2; ++x)
    {
        float2 delta=origin+float2(x,y)*2, p=uv+delta*color_size.zw;
        if (any(p<=margin) || any(p>=1-margin)) continue;
        float error=cost(p,a,b,c,d,e);
        float score=error+0.00001*dot(abs(delta),float2(1,1));
        if (score<ranked) { second=best; best=error; ranked=score; motion=delta; }
        else second=min(second,error);
    }
    origin=motion;
    [loop] for (int fine_y=-1; fine_y<=1; ++fine_y)
    [loop] for (int fine_x=-1; fine_x<=1; ++fine_x)
    {
        if (fine_x==0 && fine_y==0) continue;
        float2 delta=origin+float2(fine_x,fine_y), p=uv+delta*color_size.zw;
        if (any(p<=margin) || any(p>=1-margin)) continue;
        float error=cost(p,a,b,c,d,e);
        float score=error+0.00001*dot(abs(delta),float2(1,1));
        if (score<ranked) { second=best; best=error; ranked=score; motion=delta; }
        else second=min(second,error);
    }
    }
    if (identical) { motion=0; best=center; }
    if (best>0.12 || (!identical && second-best<0.0002)) return float4(0,0,0,1);
    float2 previous_uv=uv+motion*color_size.zw;
    float current_z=tex2Dlod(current_depth,float4(uv,0,0)).r;
    float previous_z=tex2Dlod(previous_depth,float4(previous_uv,0,0)).r;
    if (depth_options.z>0.5) { current_z=1-current_z; previous_z=1-previous_z; }
    // For perspective D3D9 depth, 1-z is approximately proportional to inverse
    // view distance. This comparison is much more useful than a fixed raw-depth
    // epsilon near the far plane; the floor accommodates depth precision noise.
    if (!(current_z>0 && current_z<1 && previous_z>0 && previous_z<1)) return float4(0,0,0,1);
    float tolerance=max(0.0002,0.2*min(1-current_z,1-previous_z));
    if (abs(current_z-previous_z)>tolerance) return float4(0,0,0,1);
    float confidence=identical ? 1 : saturate((second-best)/(0.005+best));
    return float4(motion,max(0.02,min(coarse.b,confidence)),1);
}
)";
}
