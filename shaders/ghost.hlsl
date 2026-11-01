// ============================================================================
// StarCraft: Ghost (GhostWin11) — Unified D3D11 Vertex & Pixel Shaders
// ============================================================================
// Emulates the Original Xbox NV2A fixed-function pipeline, 4-stage texture
// combiners, 8-stage register combiners (all 28 engine .xpu pixel shaders),
// hardware alpha test, atmospheric distance fog, and gamma/brightness ramp.

cbuffer PerDrawCB : register(b0) {
    float4 uWvpRow0;
    float4 uWvpRow1;
    float4 uWvpRow2;
    float4 uWvpRow3;
    float4 uViewport;
    float4 uTFactor;
    uint4  uStage0Ops;
    uint4  uStage0Alpha;
    uint4  uStage1Ops;
    uint4  uStage1Alpha;
    uint4  uStage2Ops;
    uint4  uStage2Alpha;
    uint4  uStage3Ops;
    uint4  uStage3Alpha;
    uint4  uFlags;
    float4 uAlphaTest;
    float4 uUvScale01;
    float4 uUvScale23;
    float4 uBumpParams;
    float4 uFogParams;
    float4 uFogColor;
    float4 uPsConst0;
    float4 uPsConst1;
    float4 uPsConst2;
    float4 uPsConst3;
    float4 uPsConst4;
    float4 uPsConst5;
    float4 uPsConst6;
    float4 uPsConst7;
    float4 uWorldRow0;
    float4 uWorldRow1;
    float4 uWorldRow2;
    float4 uWorldRow3;
    uint4  uLightCount;
    float4 uLightPosType[8];
    float4 uLightDirRange[8];
    float4 uLightAttenSpot[8];
    float4 uLightSpotParams[8];
    float4 uLightDiffuse[8];
    float4 uLightAmbient[8];
};

Texture2D    tex0 : register(t0);
Texture2D    tex1 : register(t1);
Texture2D    tex2 : register(t2);
Texture2D    tex3 : register(t3);
SamplerState smp0 : register(s0);
SamplerState smp1 : register(s1);
SamplerState smp2 : register(s2);
SamplerState smp3 : register(s3);

struct VSIn {
    float4 pos   : POSITION;
    float3 norm  : NORMAL;
    float4 color : COLOR0;
    float2 uv0   : TEXCOORD0;
    float2 uv1   : TEXCOORD1;
    float2 uv2   : TEXCOORD2;
    float2 uv3   : TEXCOORD3;
};

struct PSIn {
    float4 pos    : SV_POSITION;
    float4 color  : COLOR0;
    float2 uv0    : TEXCOORD0;
    float2 uv1    : TEXCOORD1;
    float2 uv2    : TEXCOORD2;
    float2 uv3    : TEXCOORD3;
    float2 eyeDist: TEXCOORD4;
    float3 norm   : TEXCOORD5;
};

PSIn VSMain(VSIn input) {
    PSIn o;
    if (uFlags.x != 0u) {
        float vpW = max(uViewport.z, 1.0f);
        float vpH = max(uViewport.w, 1.0f);
        float ndcX = ((input.pos.x - uViewport.x) / vpW) * 2.0f - 1.0f;
        float ndcY = 1.0f - ((input.pos.y - uViewport.y) / vpH) * 2.0f;
        o.pos = float4(ndcX, ndcY, saturate(input.pos.z), 1.0f);
        o.eyeDist = float2(0.0f, 1.0f);
    } else {
        float4 clipPos = input.pos.x * uWvpRow0 +
                         input.pos.y * uWvpRow1 +
                         input.pos.z * uWvpRow2 +
                         uWvpRow3;
        o.pos = clipPos;
        o.eyeDist = float2(max(clipPos.w, 0.0f), input.pos.w);
    }
    if (uLightCount.y != 0u && uFlags.x == 0u) {
        float3 wPos = input.pos.x * uWorldRow0.xyz +
                      input.pos.y * uWorldRow1.xyz +
                      input.pos.z * uWorldRow2.xyz +
                      uWorldRow3.xyz;
        float3 wNorm = input.norm.x * uWorldRow0.xyz +
                       input.norm.y * uWorldRow1.xyz +
                       input.norm.z * uWorldRow2.xyz;
        float nLen = length(wNorm);
        if (nLen > 0.0001f) wNorm /= nLen;

        float3 totRgb = input.color.rgb;
        uint numLights = min(uLightCount.x, 8u);
        for (uint l = 0u; l < numLights; ++l) {
            float4 posType = uLightPosType[l];
            float lType = posType.w;
            float3 L = float3(0.0f, 0.0f, 0.0f);
            float atten = 1.0f;
            if (lType > 2.5f) { // D3DLIGHT_DIRECTIONAL (3)
                L = posType.xyz;
            } else { // POINT (1) or SPOT (2)
                float3 toLight = posType.xyz - wPos;
                float dist = length(toLight);
                float range = uLightDirRange[l].w;
                if (range > 0.0f && dist > range) continue;
                if (dist > 0.0001f) L = toLight / dist;
                float4 attSpot = uLightAttenSpot[l];
                float denom = attSpot.x + attSpot.y * dist + attSpot.z * dist * dist;
                if (denom > 0.0001f) atten = 1.0f / denom;
                if (lType > 1.5f) { // D3DLIGHT_SPOT (2)
                    float cosAlpha = -dot(L, uLightDirRange[l].xyz);
                    float cosHalfPhi = attSpot.w;
                    if (cosAlpha <= cosHalfPhi) continue;
                    float cosHalfTheta = uLightSpotParams[l].x;
                    if (cosAlpha < cosHalfTheta && (cosHalfTheta - cosHalfPhi) > 0.0001f) {
                        atten *= pow(max((cosAlpha - cosHalfPhi) / (cosHalfTheta - cosHalfPhi), 0.0f), uLightSpotParams[l].y);
                    }
                }
            }
            float ndotl = max(dot(wNorm, L), 0.0f);
            totRgb += atten * (uLightAmbient[l].rgb + ndotl * uLightDiffuse[l].rgb);
        }
        o.color = float4(saturate(totRgb), input.color.a);
    } else {
        o.color = input.color;
    }
    o.uv0   = input.uv0 * uUvScale01.xy;
    o.uv1   = input.uv1 * uUvScale01.zw;
    o.uv2   = input.uv2 * uUvScale23.xy;
    o.uv3   = input.uv3 * uUvScale23.zw;
    o.norm  = input.norm;
    return o;
}

float4 SampleCubeAtlas(Texture2D tex, SamplerState smp, float3 dir) {
    float3 a = abs(dir);
    float ma = max(a.x, max(a.y, a.z));
    float face = 4.0f;
    float sc = dir.x;
    float tc = -dir.y;
    if (a.x >= a.y && a.x >= a.z) {
        ma = a.x;
        if (dir.x >= 0.0f) { face = 0.0f; sc = -dir.z; tc = -dir.y; }
        else               { face = 1.0f; sc =  dir.z; tc = -dir.y; }
    } else if (a.y >= a.x && a.y >= a.z) {
        ma = a.y;
        if (dir.y >= 0.0f) { face = 2.0f; sc =  dir.x; tc =  dir.z; }
        else               { face = 3.0f; sc =  dir.x; tc = -dir.z; }
    } else {
        ma = a.z;
        if (dir.z >= 0.0f) { face = 4.0f; sc =  dir.x; tc = -dir.y; }
        else               { face = 5.0f; sc = -dir.x; tc = -dir.y; }
    }
    float invMa = 1.0f / max(ma, 0.0001f);
    float u = clamp(0.5f * (sc * invMa + 1.0f), 0.004f, 0.996f);
    float v = clamp(0.5f * (tc * invMa + 1.0f), 0.004f, 0.996f);
    return tex.Sample(smp, float2((face + u) * (1.0f / 6.0f), v));
}

float4 SampleStage2D(Texture2D tex, SamplerState smp, float2 uv, bool isCubeAtlas) {
    float2 sampleUv = isCubeAtlas
        ? float2(clamp(frac(uv.x), 0.004f, 0.996f) * (1.0f / 6.0f), clamp(frac(uv.y), 0.004f, 0.996f))
        : uv;
    return tex.Sample(smp, sampleUv);
}

float4 EvalArg(uint arg, float4 diffuse, float4 current, float4 tex, float4 tfactor, float4 spec) {
    uint src = arg & 0x0Fu;
    float4 v = current;
    if (src == 0u)      v = diffuse;
    else if (src == 1u) v = current;
    else if (src == 2u) v = tex;
    else if (src == 3u) v = tfactor;
    else if (src == 4u) v = spec;
    if ((arg & 0x10u) != 0u) v = 1.0f - v;
    if ((arg & 0x20u) != 0u) v = v.aaaa;
    return v;
}

float3 EvalColorOp(uint op, uint arg2Src, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 current, float4 tex, float4 tfactor) {
    if (op == 2u)  return a1.rgb;
    if (op == 3u)  return a2.rgb;
    if (op == 4u)  return a1.rgb * a2.rgb;
    if (op == 5u)  return saturate(a1.rgb * a2.rgb * 2.0f);
    if (op == 6u)  return saturate(a1.rgb * a2.rgb * 4.0f);
    if (op == 7u)  return saturate(a1.rgb + a2.rgb);
    if (op == 8u)  return saturate(a1.rgb + a2.rgb - 0.5f);
    if (op == 9u)  return saturate((a1.rgb + a2.rgb - 0.5f) * 2.0f);
    if (op == 10u) return saturate(a1.rgb - a2.rgb);
    if (op == 11u) return saturate(a1.rgb + a2.rgb - a1.rgb * a2.rgb);
    if (op == 12u) return lerp(a2.rgb, a1.rgb, diffuse.a);
    if (op == 13u) return lerp(a2.rgb, a1.rgb, current.a);
    if (op == 14u) return lerp(a2.rgb, a1.rgb, tex.a);
    if (op == 15u) return lerp(a2.rgb, a1.rgb, tfactor.a);
    if (op == 16u) return saturate(a1.rgb + a2.rgb * (1.0f - tex.a));
    if (op == 17u) return saturate(a1.rgb + a2.rgb * (1.0f - current.a));
    if (op == 18u) return saturate(a1.rgb + a1.a * a2.rgb);
    if (op == 19u) return saturate(a1.rgb * a2.rgb + a1.a);
    if (op == 20u) return saturate(a1.rgb * a2.rgb + (1.0f - a1.rgb) * a2.a);
    if (op == 21u) return saturate(a1.rgb * a2.rgb + (1.0f - a1.a) * a2.rgb);
    if (op == 22u) {
        if ((arg2Src & 0x0Fu) == 4u) {
            return saturate(dot((a1.rgb - 0.5f) * 2.0f, (a2.rgb - 0.5f) * 2.0f)).xxx;
        }
        return current.rgb;
    }
    if (op == 23u) return saturate(a0.rgb + a1.rgb * a2.rgb);
    if (op == 24u) return lerp(a2.rgb, a1.rgb, a0.rgb);
    if (op == 25u) return saturate(dot((a1.rgb - 0.5f) * 2.0f, (a2.rgb - 0.5f) * 2.0f)).xxx;
    return a1.rgb * a2.rgb;
}

float EvalAlphaOp(uint op, float a1, float a2, float a0, float4 diffuse, float4 current, float4 tex, float4 tfactor) {
    if (op == 2u)  return a1;
    if (op == 3u)  return a2;
    if (op == 4u)  return a1 * a2;
    if (op == 5u)  return saturate(a1 * a2 * 2.0f);
    if (op == 6u)  return saturate(a1 * a2 * 4.0f);
    if (op == 7u)  return saturate(a1 + a2);
    if (op == 8u)  return saturate(a1 + a2 - 0.5f);
    if (op == 9u)  return saturate((a1 + a2 - 0.5f) * 2.0f);
    if (op == 10u) return saturate(a1 - a2);
    if (op == 11u) return saturate(a1 + a2 - a1 * a2);
    if (op == 12u) return lerp(a2, a1, diffuse.a);
    if (op == 13u) return lerp(a2, a1, current.a);
    if (op == 14u) return lerp(a2, a1, tex.a);
    if (op == 15u) return lerp(a2, a1, tfactor.a);
    if (op == 16u) return saturate(a1 + a2 * (1.0f - tex.a));
    if (op == 17u) return saturate(a1 + a2 * (1.0f - current.a));
    if (op == 23u) return saturate(a0 + a1 * a2);
    if (op == 24u) return lerp(a2, a1, a0);
    return a1 * a2;
}

void EvalStage(uint4 colorOps, uint4 alphaOps, float4 diffuse, inout float4 current, float4 tex, float4 tfactor, float4 spec) {
    float4 c1 = EvalArg(colorOps.y, diffuse, current, tex, tfactor, spec);
    float4 c2 = EvalArg(colorOps.z, diffuse, current, tex, tfactor, spec);
    float4 c0 = EvalArg(colorOps.w, diffuse, current, tex, tfactor, spec);
    current.rgb = EvalColorOp(colorOps.x, colorOps.z, c1, c2, c0, diffuse, current, tex, tfactor);
    if (alphaOps.x > 1u) {
        float4 a1 = EvalArg(alphaOps.y, diffuse, current, tex, tfactor, spec);
        float4 a2 = EvalArg(alphaOps.z, diffuse, current, tex, tfactor, spec);
        float4 a0 = EvalArg(alphaOps.w, diffuse, current, tex, tfactor, spec);
        current.a = EvalAlphaOp(alphaOps.x, a1.a, a2.a, a0.a, diffuse, current, tex, tfactor);
    }
}

float4 PSMain(PSIn input) : SV_TARGET {
    float4 diffuse = input.color;
    float4 specCol = float4(input.norm, 1.0f);
    float4 current = diffuse;
    uint texMask  = uFlags.y & 0x0Fu;
    uint cubeMask = (uFlags.y >> 4u) & 0x0Fu;
    uint projMask = (uFlags.y >> 8u) & 0x0Fu;
#ifdef FIXED_PS_MODE
    const uint psMode = (uint)FIXED_PS_MODE;
#else
    uint psMode   = uFlags.w;
#endif

    float invProjQ = (projMask != 0u && abs(input.eyeDist.y) > 0.0001f) ? (1.0f / input.eyeDist.y) : 1.0f;
    float2 uv0 = ((projMask & 1u) != 0u) ? (input.uv0 * invProjQ) : input.uv0;
    float2 uv1 = ((projMask & 2u) != 0u) ? (input.uv1 * invProjQ) : input.uv1;
    float2 uv2 = ((projMask & 4u) != 0u) ? (input.uv2 * invProjQ) : input.uv2;
    float2 uv3 = ((projMask & 8u) != 0u) ? (input.uv3 * invProjQ) : input.uv3;

    float4 t0 = ((texMask & 1u) != 0u) ? SampleStage2D(tex0, smp0, uv0, (cubeMask & 1u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float4 t1 = ((texMask & 2u) != 0u) ? SampleStage2D(tex1, smp1, uv1, (cubeMask & 2u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);

    // Apply BumpEnv UV offset if stage 0 / stage 1 uses BUMPENVMAP (22 without SPECULAR arg2) or texbem t1, t0 pixel shaders (emBump=1, lava=2, heatShimmer=3)
    if ((uStage0Ops.x == 22u && (uStage0Ops.z & 0x0Fu) != 4u) || psMode == 1u || psMode == 2u || psMode == 3u) {
        float2 d = t0.rg * 2.0f - 1.0f;
        uv1 += float2(d.x * uBumpParams.x + d.y * uBumpParams.z, d.x * uBumpParams.y + d.y * uBumpParams.w);
        if ((texMask & 2u) != 0u) t1 = SampleStage2D(tex1, smp1, uv1, (cubeMask & 2u) != 0u);
    }
    if (uStage1Ops.x == 22u && (uStage1Ops.z & 0x0Fu) != 4u) {
        float2 d = t1.rg * 2.0f - 1.0f;
        uv2 += float2(d.x * uBumpParams.x + d.y * uBumpParams.z, d.x * uBumpParams.y + d.y * uBumpParams.w);
    }

    float4 t2 = ((texMask & 4u) != 0u) ? SampleStage2D(tex2, smp2, uv2, (cubeMask & 4u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float4 t3 = ((texMask & 8u) != 0u) ? SampleStage2D(tex3, smp3, uv3, (cubeMask & 8u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);

    if (psMode > 0u) {
        uint pt = psMode - 1u;
        if (pt == 0u) { // emBump.xpu: tex t0; texbem t1, t0; mov r0, t1
            current = t1;
        } else if (pt == 1u) { // lava.xpu: tex t0; texbem t1, t0; tex t2; mad r0, t1.a, t2, t1
            current.rgb = saturate(t1.a * t2.rgb + t1.rgb);
            current.a   = saturate(t1.a * t2.b   + t1.b);
        } else if (pt == 2u) { // heatShimmer.xpu: tex t0; texbem t1, t0; tex t2; mul r0.rgb, t1, t2; mul r0.a, t2.a, v0.a
            current.rgb = t1.rgb * t2.rgb;
            current.a   = t2.a * diffuse.a;
        } else if (pt == 3u) { // terrain_fog.xpu: outputs fog.rgb and diffuse.a
            current.rgb = uFogColor.rgb;
            current.a   = saturate(diffuse.a);
        } else if (pt == 4u) { // ghostsuit.xpu: add r1.rgb, t1, c1; mov r0.a, t0.a; xmmc r0, t0, v0, c0, r1 (r0.a>=0.5 ? CD : AB)
            float3 r1 = saturate(t1.rgb + uPsConst1.rgb);
            current.rgb = (t0.a >= 0.5f) ? (uPsConst0.rgb * r1)   : (t0.rgb * diffuse.rgb);
            current.a   = (t0.a >= 0.5f) ? (uPsConst0.a   * r1.b) : (t0.a   * diffuse.a);
        } else if (pt == 5u) { // selfglow.xpu: mad r0, c0, t0.a, v0; mul r0, r0, t0
            float4 r0 = saturate(uPsConst0 * t0.a + diffuse);
            current = r0 * t0;
        } else if (pt == 6u) { // bumpreflect.xpu: tex t0; texm3x3pad t1, t0; texm3x3pad t2, t0; texm3x3vspec t3, t0; mov r0, t3
            float3 nTs = t0.rgb * 2.0f - 1.0f;
            float3 Tw  = float3(uv1.x, uv1.y, uv2.x);
            float3 Bw  = float3(uv2.y, uv3.x, uv3.y);
            float3 Nw  = cross(Tw, Bw);
            float3 Np  = (dot(Nw, Nw) > 0.0001f) ? normalize(nTs.x * Tw + nTs.y * Bw + nTs.z * Nw) : normalize(nTs);
            float3 Vw  = (dot(input.norm, input.norm) > 0.0001f) ? normalize(input.norm) : float3(0.0f, 0.0f, 1.0f);
            float3 R   = reflect(-Vw, Np);
            float4 refl = t3;
            if ((texMask & 8u) != 0u) {
                refl = ((cubeMask & 8u) != 0u)
                    ? SampleCubeAtlas(tex3, smp3, R)
                    : tex3.Sample(smp3, float2(R.x * 0.5f + 0.5f, 0.5f - R.y * 0.5f));
            }
            current = refl;
        } else if (pt == 7u) { // waterNM.xpu: add_d2 r0, t0, t1
            current = (t0 + t1) * 0.5f;
        } else if (pt == 8u) { // screenHaze.xpu: 7-stage register combiner (MUX: r0.a>=0.5 ? CD : AB)
            float r0a = dot(t1.rgb, uPsConst0.aaa);
            float r1a = dot(t2.rgb, uPsConst0.aaa);
            float4 r0 = (r0a >= 0.5f) ? t1 : float4(uPsConst0.rgb, uPsConst0.b);
            float4 r1 = (r1a >= 0.5f) ? t2 : float4(uPsConst0.rgb, uPsConst0.b);
            r1.rgb = saturate(r0.rgb * uPsConst1.rgb + r1.rgb * uPsConst1.rgb);
            r1.a   = saturate(r0.a   * uPsConst1.b   + r1.a   * uPsConst1.b);
            r0a = dot(t3.rgb, uPsConst0.aaa);
            r0 = (r0a >= 0.5f) ? t3 : float4(uPsConst0.rgb, uPsConst0.b);
            current.rgb = saturate(uPsConst1.rgb * r0.rgb + r1.rgb);
            current.a   = saturate(uPsConst1.a);
        } else if (pt == 9u) { // terrain.xpu
            float3 r0 = lerp(t2.rgb, t1.rgb, t0.a);
            r0 = saturate(r0 * t0.rgb * 2.0f);
            r0 = saturate(r0 * t3.rgb * 2.0f);
            current.rgb = r0 * diffuse.rgb;
            current.a   = 1.0f;
        } else if (pt == 10u) { // terrain_nodetail.xpu
            float3 r0 = lerp(t2.rgb, t1.rgb, t0.a);
            current.rgb = saturate(r0 * t0.rgb * 2.0f) * diffuse.rgb;
            current.a   = 1.0f;
        } else if (pt == 11u || pt == 14u) { // gaussBlurSource.xpu / gaussBlur_Source_ms.xpu
            float4 r0 = saturate(t0 - uPsConst0);
            float4 r1 = saturate(r0 * 2.0f);
            r1.rgb = saturate(uPsConst2.rgb - r1.rgb);
            r1.a   = saturate(uPsConst2.b   - r1.a);
            r1 = saturate((r1 - uPsConst2.a) * 2.0f);
            r1 = t1 * r1 * uPsConst1;
            current = saturate(r0 + r1);
        } else if (pt == 12u || pt == 13u) { // gaussBlur.xpu / gaussBlur_ms.xpu
            current = saturate(saturate(t0 * uPsConst0 + t1 * uPsConst1) + saturate(t2 * uPsConst2 + t3 * uPsConst3));
        } else if (pt == 15u) { // sightFinal.xpu: tex t0; texreg2gb t1, t0; mov r0, t1
            current = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.gb) : t0;
        } else if (pt == 16u) { // cloak.xpu: t0=backbuffer, t1=membrane, t2=mask, t3=base
            float3 r1 = saturate(t1.a * t1.rgb + t0.rgb);
            float3 r0 = lerp(t3.rgb, t3.rgb * diffuse.rgb, uPsConst1.a);
            r0 = lerp(r1, r0, uPsConst0.rgb);
            current.rgb = (t2.a >= 0.5f) ? r0 : r1;
            current.a   = uPsConst0.a;
        } else if (pt == 17u) { // cloakDissolve.xpu: t0=backbuffer, t1=membrane, t2=dissolve, t3=base
            float3 r0 = saturate(t1.a * t1.rgb + t0.rgb);
            float3 r1 = lerp(t3.rgb, t3.rgb * diffuse.rgb, uPsConst1.a);
            r1 = ((t2.b - (uPsConst2.a - 0.5f)) >= 0.5f) ? r1 : uPsConst2.rgb;
            float3 blended = ((uPsConst2.a - (t2.b - 0.5f)) >= 0.5f) ? r0 : r1;
            current.rgb = lerp(blended, uPsConst3.rgb, uPsConst3.a);
            current.a   = uPsConst0.a;
        } else if (pt == 18u) { // cloakDissolveShadow.xpu: t0=dissolve
            float r0a = t0.b - (uPsConst1.a - 0.5f);
            current.rgb = ((r0a >= 0.5f) ? uPsConst1.rgb : uPsConst0.rgb) * uPsConst0.a;
            current.a   = saturate(r0a * uPsConst0.a);
        } else if (pt == 19u) { // desaturate.xpu: max(r,g,b) + c1, then * c0
            float m = max(t0.r, max(t0.g, t0.b));
            current.rgb = saturate(m + uPsConst1.rgb) * uPsConst0.rgb;
            current.a   = m;
        } else if (pt == 20u) { // shield.xpu: texreg2ar t1, t0 + membrane t2 + dissolve t3 (cnd r1, r0.a, r0, c1)
            float4 s1 = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.ar) : t1;
            float4 r1 = t2 * uPsConst0.a;
            float4 r0 = saturate(s1 + r1) * r1.a;
            float r0a = saturate(t3.b - uPsConst0.b);
            r1 = (r0a >= 0.5f) ? float4(r0.rgb, r0a) : uPsConst1;
            current.rgb = r1.rgb;
            current.a   = r0a;
        } else if (pt == 21u) { // shieldBlur.xpu: 4-tap signed alpha blur + c0.a bias
            float r0a = ((t0.a - 0.5f) + (t1.a - 0.5f)) * 0.5f;
            float r1a = ((t2.a - 0.5f) + (t3.a - 0.5f)) * 0.5f;
            r0a = saturate((r0a + r1a) * 0.5f + uPsConst0.a);
            current = float4(r0a, r0a, r0a, r0a);
        } else if (pt == 22u) { // shield_part1.xpu: 4-tap wave Laplacian propagation
            float r0a = clamp((t1.a - t0.a) + (t2.a - t0.a) + (t3.a - t0.a), -1.0f, 1.0f);
            float r1a = (uPsConst0.a - t0.a) * uPsConst1.a + r0a;
            r0a = saturate(r1a + uPsConst0.a);
            current = float4(r0a, r0a, r0a, 1.0f);
        } else if (pt == 23u) { // shield_part2.xpu: wave height/velocity update
            float d = dot(t0.rgb, uPsConst2.rgb);
            float r0a = t1.a - t0.a;
            float4 r0 = saturate(float4(d + r0a, d + r0a, d + r0a, d + r0a));
            float4 r1 = clamp((float4(t0.rgb, t0.b) - uPsConst1) * 2.0f, -1.0f, 1.0f);
            r1.rgb = ((2.0f * r0.rgb - 1.0f) * uPsConst0.rgb + r1.rgb * uPsConst3.a) * 0.5f;
            r1.a   = ((2.0f * r0.a   - 1.0f) * uPsConst0.b   + r1.a   * uPsConst3.a) * 0.5f;
            r1.rgb = saturate(r1.rgb + uPsConst1.rgb);
            r1.a   = saturate(r1.a   + uPsConst1.b);
            float r0a2 = clamp((t0.a - uPsConst1.a) * 2.0f, -1.0f, 1.0f);
            r0a2 = ((2.0f * r1.a - 1.0f) * uPsConst0.a + r0a2) * 0.5f;
            current.rgb = r1.rgb;
            current.a   = saturate(r0a2 + uPsConst1.a);
        } else if (pt == 24u) { // dof.xpu: texreg2ar t1, t0 + xmmc r0.a, t1.a, c1.a, 1, r0.b (r0.a>=0.5 ? CD : AB)
            float4 s1 = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.ar) : t1;
            float r0_rgb = saturate(((2.0f * saturate(s1.a) - 1.0f) * uPsConst0.a + uPsConst1.a) * 0.5f);
            float r1_rgb = saturate((2.0f * saturate(s1.a) - 1.0f) * uPsConst1.a);
            float r0_a   = (s1.a >= 0.5f) ? r0_rgb : saturate(s1.a * uPsConst1.a);
            current.rgb  = lerp(t2.rgb, t3.rgb, r1_rgb);
            current.a    = saturate(r0_a);
        } else if (pt == 25u) { // alphadissolve.xpu: sub r0.a, t0.a, c0_bias.a; cnd r0.rgb, r0.a, t0, t1
            float r0a = t0.a - (saturate(uPsConst0.a) - 0.5f);
            current.rgb = (r0a >= 0.5f) ? t0.rgb : t1.rgb;
            current.a   = saturate(r0a);
        } else if (pt == 26u) { // alphadissolve_sep.xpu: sub r0.a, t1.b, c0_bias.a; cnd r0.rgb, r0.a, t0, c1
            float r0a = t1.b - (saturate(uPsConst0.a) - 0.5f);
            current.rgb = (r0a >= 0.5f) ? t0.rgb : uPsConst1.rgb;
            current.a   = saturate(r0a);
        } else { // pt == 27u: alphadissolve_sep_glo.xpu: add r0, t1.b, v0; mul r1.rgb, r0, t0; cnd r1, r0.a, r1, c1; cnd r0, r0.a, r1, c2
            float4 r1 = float4(saturate(t1.bbb + diffuse.rgb) * t0.rgb, t0.a);
            float r0a = t2.b - (saturate(uPsConst0.a) - 0.5f);
            r1 = (r0a >= 0.5f) ? r1 : uPsConst1;
            r0a = t2.b - (saturate(uPsConst0.b) - 0.5f);
            current = (r0a >= 0.5f) ? r1 : uPsConst2;
        }
    } else if (uStage0Ops.x > 1u) {
        EvalStage(uStage0Ops, uStage0Alpha, diffuse, current, t0, uTFactor, specCol);
        if (uStage1Ops.x > 1u) {
            EvalStage(uStage1Ops, uStage1Alpha, diffuse, current, t1, uTFactor, specCol);
            if (uStage2Ops.x > 1u) {
                EvalStage(uStage2Ops, uStage2Alpha, diffuse, current, t2, uTFactor, specCol);
                if (uStage3Ops.x > 1u) {
                    EvalStage(uStage3Ops, uStage3Alpha, diffuse, current, t3, uTFactor, specCol);
                }
            }
        }
    }

    if (uFlags.z != 0u) {
        uint fn = (uint)uAlphaTest.x;
        float refVal = uAlphaTest.y;
        bool passAlpha = true;
        if (fn == 1u)      passAlpha = false;
        else if (fn == 2u) passAlpha = (current.a <  refVal - 0.001f);
        else if (fn == 3u) passAlpha = (abs(current.a - refVal) < 0.004f);
        else if (fn == 4u) passAlpha = (current.a <= refVal + 0.001f);
        else if (fn == 5u) passAlpha = (current.a >  refVal + 0.001f);
        else if (fn == 6u) passAlpha = (abs(current.a - refVal) >= 0.004f);
        else if (fn == 7u) passAlpha = (current.a >= refVal - 0.001f);
        if (!passAlpha) discard;
    }

    // Hardware Distance & Atmospheric Fog (3D geometry only)
    if (uFogParams.x > 0.5f && uFlags.x == 0u) {
        float fogMode = uFogParams.x;
        float d = input.eyeDist.x;
        float fogFactor = 1.0f;
        if (fogMode > 2.5f) { // D3DFOG_LINEAR (3)
            float span = max(uFogParams.z - uFogParams.y, 0.001f);
            fogFactor = saturate((uFogParams.z - d) / span);
        } else if (fogMode < 1.5f) { // D3DFOG_EXP (1)
            fogFactor = saturate(exp(-d * uFogParams.w));
        } else { // D3DFOG_EXP2 (2)
            float df = d * uFogParams.w;
            fogFactor = saturate(exp(-(df * df)));
        }
        float fogAmt = 1.0f - fogFactor;
        if (uFogColor.w > 0.5f) {
            current.rgb *= fogFactor;
        } else {
            current.rgb = lerp(current.rgb, uFogColor.rgb, fogAmt);
        }
    }

    // Gamma & Brightness adjustment
    if (uAlphaTest.z > 0.1f && abs(uAlphaTest.z - 1.0f) > 0.01f) {
        current.rgb = pow(max(current.rgb, 0.0f), 1.0f / uAlphaTest.z) * uAlphaTest.w;
    }

    return current;
}
