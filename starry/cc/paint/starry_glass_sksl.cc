// Copyright 2026 The Starry Browser Authors

#include "starry/cc/paint/starry_glass_sksl.h"

namespace cc::starry_shaders {

// Liquid Glass 的 SkSL 着色器：一遍绘制阴影、折射与模糊后的表面及边缘高光，最多支持四个
// 形状融合。
//
// 背景输入与坐标约定：
// - 背景来自合成器输入：src 是背后的原始画面，blur0…blur4 是不同模糊宽度的背景。默认由
//   共享 mip 金字塔近似，高斯路径用于回退和对照。
//   blurHi0…4 / blurMix0…4 在 1 倍时插值相邻 mip；高倍率提前在低分辨率混合。
//     blur0  基础背景模糊（regular 是 1/4，双线性放大后约等于 2 px 的高斯）
//     blur1、blur2  模糊的「柔和的尾」，sigma 是 wideSigma.x、wideSigma.y（设备像素）；
//                   半径落在两者之间时按方差插值
//     blur3  阴影取的周围颜色
//     blur4  溢光取的周围颜色，也用来量平均亮度
// - 没有 fwidth：用 SDF 法线和 render surface 的倍率算一个像素的宽度。
// - 用作背景滤镜时（backdropMode 1），输出要包含玻璃外面原来的画面：阴影叠在 src 上。
//   玻璃连同它后面的底一起画进图层时（backdropMode 0），玻璃外面只输出阴影。
//
// 长度单位都是图层坐标（pt，y 向下），原点是图层左上角。滤镜是按物理像素构建的
// （StarryShaderPaintFilter::Spec::scale）：main 收到的坐标和给输入取样的坐标是物理
// 像素，进来除以 pixelScale、取样时乘回去，中间的计算都用 pt。
// pixelScale 包含合成器的动画缩放；dpr 只决定材质，缩放动画不改变材质。
const char kGlass[] = R"SKSL(
uniform shader src;
uniform shader blur0;
uniform shader blur1;
uniform shader blur2;
uniform shader blur3;
uniform shader blur4;
uniform shader blurHi0;
uniform float blurMix0;
uniform shader blurHi1;
uniform float blurMix1;
uniform shader blurHi2;
uniform float blurMix2;
uniform shader blurHi3;
uniform float blurMix3;
uniform shader blurHi4;
uniform float blurMix4;


uniform float dpr;              // device pixels per point
uniform float2 pixelScale;      // render-surface pixels per point, including animation
uniform float2 wideSigma;       // sigma of blur1 and blur2, device pixels
uniform float4 lumaRect;        // the shapes' bounds: where the appearance is measured
uniform float lumaOverride;     // >= 0: the mean luminance behind the glass, measured elsewhere
uniform float backdropMode;     // 1: a backdrop filter, keeps what is outside the glass; 0: only the shadow

// Shapes: centre x, y and half size w, h; corner: radius, continuous (0/1).
uniform float4 shape0;
uniform float4 shape1;
uniform float4 shape2;
uniform float4 shape3;
uniform float2 corner0;
uniform float2 corner1;
uniform float2 corner2;
uniform float2 corner3;
uniform float shapeCount;
uniform float smoothness;       // shape merge distance

// Refraction. Inner pulls content in from the rim, outer pushes it out at the edge.
uniform float innerAmount;
uniform float innerHeight;
uniform float outerAmount;
uniform float outerHeight;
uniform float refractionOpacity;
// Blur radius in backdrop texels, scaled by a piecewise-linear profile over
// distance.
uniform float blurRadius;
uniform float4 blurDist;
uniform float blurDist4;
uniform float4 blurAlpha;
uniform float blurAlpha4;

// Shadow: offset, Gaussian radius, opacity. Large glass colours it with what lies around it
// (vibrancy 0..1), read shadowAmount out; the SDR shadow opacity deepens every shadow.
uniform float2 shadowOffset;
uniform float shadowRadius;
uniform float shadowOpacity;
uniform float shadowVibrancy;
uniform float sdrShadowOpacity;
uniform float shadowAmount;
uniform float shadowHeight;
// Edge bleed (large glass): what lies beyond the rim bleeds into the face.
uniform float bleedStrength;
uniform float bleedAmount;
uniform float bleedHeight;

uniform float dark;             // 0 light appearance, 1 dark
uniform float largeClass;       // 0 small (adaptive), 1 large
uniform float clearGlass;
// Rim highlight.
uniform float highlight;
uniform float2 keyDir;
uniform float keyCos;
uniform float keyBoost;
uniform float2 fillDir;
uniform float fillCos;
uniform float fillBoost;
uniform float rimWidth;
uniform float rimCurvature;
// Extra uniform tint wash, and a colour layered over the finished glass (premultiplied).
uniform float4 tint;
uniform float4 overlay;
// 0 normal; 1 mean luminance; 2 the backdrop, unrefracted; 3..7 blur0..blur4; 8 the SDF.
uniform float debugMode;
uniform float sampleCulling;    // 0 disables culling and evaluates every branch

// The compositor's direct path (StarryShaderPaintFilter::DrawMaterial) shades the material at
// half resolution first and composites it at full resolution, see materialStage in main().
uniform shader material;        // the half-resolution atlas: a face cell and a shadow cell
uniform float materialStage;    // 0 one pass; 1 write the face cell; 2 the shadow cell; 3 composite
uniform float4 materialFace;    // atlas texel = coord * xy + zw
uniform float4 materialFaceCell;    // the cell's texel centres: left, top, right, bottom
uniform float4 materialShadow;
uniform float4 materialShadowCell;
uniform float materialEdge;     // pt: deeper than this the face comes from the atlas
uniform float materialReach;    // pt: how far inside the rim the shadow cell is filled

const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

// ---- Shapes ----

// Rounded box with circular corners: distance and outward gradient, p relative to the centre.
float3 circularBox(float2 p, float2 hs, float r) {
    float2 s = sign(p + 1e-6);
    float2 q = abs(p) - hs + r;
    float2 qp = max(q, 0.0);
    float outside = length(qp);
    float d = min(max(q.x, q.y), 0.0) + outside - r;
    float2 g = outside > 1e-5 ? qp / outside : (q.x > q.y ? float2(1.0, 0.0) : float2(0.0, 1.0));
    return float3(d, g * s);
}

// Continuous ("squircle") corners: the curve starts 1.5287 r from the corner and its distance is
// the circular one corrected by a polynomial in the corner's aspect. Near-capsule shapes fall
// back to circular corners.
float3 continuousBox(float2 p, float2 hs, float r) {
    const float K = 1.5286649;
    float side = min(hs.x, hs.y);
    float circular = clamp((K - side / max(r, 1e-4)) / 0.5286649, 0.0, 1.0);
    float3 c = circularBox(p, hs, r);
    if (circular >= 1.0) {
        return c;
    }
    float2 s = sign(p + 1e-6);
    float R = r * K;
    float2 q = abs(p) - hs + R;
    // Straight sides and the interior agree with the circular box exactly.
    // Only the corner squares need the continuous-curvature approximation.
    if (q.x <= 0.0 || q.y <= 0.0) {
        return c;
    }
    float2 u = max(q / R, 0.0);
    float len = length(u);
    float hi = max(u.x, u.y);
    float lo = min(u.x, u.y);
    float t = hi > 0.0 ? clamp(lo / hi, 0.0, 1.0) : 0.0;
    float poly = 0.268531 + t * (1.26803 + t * (-3.64122 + t * (3.15601 - 0.926054 * t)));
    float squircle = len + 1.0 - 1.0 / (1.0 - t * t * clamp(len, 0.0, 1.0) * poly);
    float d = min(max(q.x, q.y), 0.0) + R * (squircle - 1.0);
    d = mix(d, c.x, circular);
    float2 qp = max(q, 0.0);
    float2 g = dot(qp, qp) > 1e-10 ? normalize(qp) : (q.x > q.y ? float2(1.0, 0.0) : float2(0.0, 1.0));
    return float3(d, g * s);
}

float3 shapeSdf(float2 p, float4 shape, float2 corner) {
    float2 local = p - shape.xy;
    float r = min(corner.x, min(shape.z, shape.w));
    if (corner.y > 0.5) {
        return continuousBox(local, shape.zw, r);
    }
    return circularBox(local, shape.zw, r);
}

// Smooth union that only rounds where the two surfaces face apart, so merging blobs bridge
// smoothly while parallel edges keep their exact distance.
float3 smoothUnion(float3 a, float3 b, float k) {
    float kk = k * clamp(0.5 - 0.5 * dot(a.yz, b.yz), 0.0, 1.0);
    if (kk < 1e-4) {
        return a.x < b.x ? a : b;
    }
    float h = clamp(0.5 + 0.5 * (b.x - a.x) / kk, 0.0, 1.0);
    float d = mix(b.x, a.x, h) - kk * h * (1.0 - h);
    float2 g = mix(b.yz, a.yz, h);
    return float3(d, normalize(g + 1e-6));
}

float3 sceneSdf(float2 p) {
    float3 d = shapeSdf(p, shape0, corner0);
    if (shapeCount > 1.5) { d = smoothUnion(d, shapeSdf(p, shape1, corner1), smoothness); }
    if (shapeCount > 2.5) { d = smoothUnion(d, shapeSdf(p, shape2, corner2), smoothness); }
    if (shapeCount > 3.5) { d = smoothUnion(d, shapeSdf(p, shape3, corner3), smoothness); }
    return d;
}

// ---- Backdrop ----

half4 sampleBlur0(float2 p) {
    half4 lo = blur0.eval(p);
    if (blurMix0 <= 0.0) { return lo; }
    return mix(lo, blurHi0.eval(p), half(blurMix0));
}

half4 sampleBlur1(float2 p) {
    half4 lo = blur1.eval(p);
    if (blurMix1 <= 0.0) { return lo; }
    return mix(lo, blurHi1.eval(p), half(blurMix1));
}

half4 sampleBlur2(float2 p) {
    half4 lo = blur2.eval(p);
    if (blurMix2 <= 0.0) { return lo; }
    return mix(lo, blurHi2.eval(p), half(blurMix2));
}

half4 sampleBlur3(float2 p) {
    half4 lo = blur3.eval(p);
    if (blurMix3 <= 0.0) { return lo; }
    return mix(lo, blurHi3.eval(p), half(blurMix3));
}

half4 sampleBlur4(float2 p) {
    half4 lo = blur4.eval(p);
    if (blurMix4 <= 0.0) { return lo; }
    return mix(lo, blurHi4.eval(p), half(blurMix4));
}

float3 unpremultiply(float4 c) {
    return c.a > 1e-5 ? c.rgb / c.a : float3(0.0);
}

// Wide blur tail: Gaussian sigma in device pixels for a blur radius of `px`
// pixels, stepping up once the sharp core saturates.
float wideSigmaFor(float px) {
    if (px <= 4.76) { return 9.0; }
    if (px <= 5.66) { return mix(9.0, 15.75, (px - 4.76) / 0.9); }
    if (px <= 8.0) { return mix(15.75, 18.25, (px - 5.66) / 2.34); }
    if (px <= 11.3) { return mix(18.25, 21.5, (px - 8.0) / 3.3); }
    if (px <= 16.0) { return mix(21.5, 29.75, (px - 11.3) / 4.7); }
    return 29.75 + 1.755 * (px - 16.0);
}

// The backdrop at p blurred by `radius` (in backdrop texels), premultiplied: a mix
// of the sharp core blur and a wide Gaussian tail, using the
// Gaussian alone past 4.76 px.
float4 blurredBackdrop(float2 p, float radius) {
    float4 core = float4(sampleBlur0(p * pixelScale));
    if (clearGlass > 0.5) {
        return core;
    }
    float px = radius * dpr;
    float sigma = wideSigmaFor(px);
    float lo = wideSigma.x;
    float hi = wideSigma.y;
    float t = hi > lo + 1e-3 ? clamp((sigma * sigma - lo * lo) / (hi * hi - lo * lo), 0.0, 1.0) : 0.0;
    float4 wide = float4(sampleBlur1(p * pixelScale));
    if (t > 0.0) {
        wide = mix(wide, float4(sampleBlur2(p * pixelScale)), t);
    }
    float w = px <= 4.76 ? min(0.85 * log2(px * 0.25 + 1.0), 0.96)
                         : mix(0.96, 1.0, clamp((px - 4.76) / 0.9, 0.0, 1.0));
    return mix(core, wide, w);
}

// Quarter-circle bevel: 0 at the rim, 1 once `height` inside.
float bevel(float depth, float height) {
    float t = clamp(depth / max(height, 1e-4), 0.0, 1.0);
    return sqrt(t * (2.0 - t));
}

float blurProfile(float d) {
    if (d <= blurDist.x) { return blurAlpha.x; }
    if (d <= blurDist.y) { return mix(blurAlpha.x, blurAlpha.y, (d - blurDist.x) / max(blurDist.y - blurDist.x, 1e-4)); }
    if (d <= blurDist.z) { return mix(blurAlpha.y, blurAlpha.z, (d - blurDist.y) / max(blurDist.z - blurDist.y, 1e-4)); }
    if (d <= blurDist.w) { return mix(blurAlpha.z, blurAlpha.w, (d - blurDist.z) / max(blurDist.w - blurDist.z, 1e-4)); }
    if (d <= blurDist4) { return mix(blurAlpha.w, blurAlpha4, (d - blurDist.w) / max(blurDist4 - blurDist.w, 1e-4)); }
    return blurAlpha4;
}

// Saturation, then a levels move along the grey axis (black->b, white->w), then a source-over
// fill, applied consistently across the face, bleed and shadow stages.
float3 grade(float3 c, float white, float black, float saturation, float4 fill) {
    float y = dot(c, LUMA);
    c = mix(float3(y), c, saturation);
    c += black + (white - black - 1.0) * dot(c, LUMA);
    return mix(c, fill.rgb, fill.a);
}

// Mean luminance behind the shapes: a 4x3 grid of reads of the widest blur.
float meanLuma() {
    if (lumaOverride >= 0.0) {
        return lumaOverride;
    }
    float2 cell = lumaRect.zw / float2(4.0, 3.0);
    float3 sum = float3(0.0);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j) {
            float2 at = lumaRect.xy + cell * (float2(float(i), float(j)) + 0.5);
            sum += unpremultiply(float4(sampleBlur4(at * pixelScale)));
        }
    }
    return clamp(dot(sum / 12.0, LUMA), 0.0, 1.0);
}

float erfApprox(float x) {
    // Abramowitz-Stegun 7.1.26, max error 1.5e-7.
    float s = sign(x);
    x = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * x);
    float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
    return s * y;
}

// The shadow at p, premultiplied: outside the shape and under its antialiased rim.
float4 shadowAt(float2 p, float d, float2 n, float lightStyle, float4 shadowFill) {
    float ds = sceneSdf(p - shadowOffset).x;
    float falloff = shadowOpacity * (0.5 - 0.5 * erfApprox(clamp(ds / max(shadowRadius, 1e-4), -2.0, 2.0)));
    float a = shadowFill.a;
    float3 around = float3(0.0);
    if (shadowVibrancy > 0.0) {
        float reach = shadowAmount * (1.0 - bevel(-d, shadowHeight));
        around = grade(unpremultiply(float4(sampleBlur3((p + reach * n) * pixelScale))), mix(0.5, 1.0, lightStyle), 0.0,
                       mix(1.0, 1.8, lightStyle), float4(0.0));
    }
    return falloff * float4(shadowVibrancy * (1.0 - a - sdrShadowOpacity) * around,
                            mix(a + sdrShadowOpacity, 1.0, shadowVibrancy));
}

// The finished colour of the glass face at p (opaque).
float3 faceAt(float2 p, float d, float2 n, float aa, float lightStyle,
              float white, float black, float saturation, float4 fill) {
    float depth = -d;
    // Inner refraction: displacement along the outward normal, strongest at the rim. The
    // two refractions mix premultiplied.
    float inner = innerAmount * (1.0 - bevel(depth, innerHeight));
    float blur = blurProfile(d + inner) * blurRadius;
    float4 c4 = blurredBackdrop(p + inner * n, blur);
    float outerWeight = clamp(d + 1.0, 0.0, 1.0) * refractionOpacity;
    if (refractionOpacity > 0.0 && (sampleCulling < 0.5 || outerWeight > 0.0)) {
        float outer = outerAmount * (1.0 - bevel(depth, outerHeight));
        float blur2r = blurProfile(d + outer) * blurRadius;
        c4 = mix(c4, blurredBackdrop(p + outer * n, blur2r), outerWeight);
    }
    float3 c = grade(unpremultiply(c4), white, black, saturation, fill);
    // Edge bleed: what lies out past the rim, graded, weighted by the face's own brightness
    // to the fourth power (its darkness in the dark style).
    if (bleedStrength > 0.0) {
        float y = clamp(dot(c, LUMA), 0.0, 1.0);
        float k = mix(1.0 - y, y, lightStyle);
        k *= k;
        float bleedWeight = k * k * bleedStrength * mix(0.8, 0.5, lightStyle);
        if (sampleCulling < 0.5 || bleedWeight > 0.0) {
            float reach = bleedAmount * (1.0 - bevel(depth, bleedHeight));
            float3 bleed = grade(unpremultiply(float4(sampleBlur4((p + reach * n) * pixelScale))), mix(0.5, 1.0, lightStyle),
                                 mix(0.0, 0.9, lightStyle), mix(1.0, 1.2, lightStyle), float4(0.0));
            c = mix(c, bleed, bleedWeight);
        }
    }
    if (sampleCulling < 0.5 || tint.a != 0.0) {
        c = mix(c, tint.rgb / max(tint.a, 1e-4), tint.a);
    }

    // Rim highlight: a band rimWidth deep with a hard edge softened by rimCurvature, lit
    // from the key and fill directions, then blended toward a vibrant grade of the face.
    float band = 0.0;
    if (highlight > 0.0 && depth > -1.0) {
        float t = clamp(depth / rimWidth, 0.0, 1.0);
        float profile = mix(t < 1.0 ? 1.0 : 0.0, 1.0 - t, rimCurvature);
        band = clamp(depth / aa + 0.5, 0.0, 1.0) * clamp((rimWidth - depth) / aa + 0.5, 0.0, 1.0) * profile;
    }
    if (sampleCulling < 0.5 || band != 0.0) {
        float key = band * clamp((dot(n, keyDir) - keyCos) / max(1.0 - keyCos, 1e-4), 0.0, 1.0);
        float fil = band * clamp((dot(n, fillDir) - fillCos) / max(1.0 - fillCos, 1e-4), 0.0, 1.0);
        key /= max(1.0 + (1.0 - key) * keyBoost, 1e-4);
        fil /= max(1.0 + (1.0 - fil) * fillBoost, 1e-4);
        float h = clamp((key + fil) * highlight, 0.0, 1.0);
        if (sampleCulling < 0.5 || h > 0.0) {
            float3 vibrantLight = grade(c, 1.0, 0.9, 1.5, float4(0.0));
            float3 vibrantDark = grade(c, -0.5, 0.15, 3.0, float4(0.0));
            float3 rim = clamp(mix(vibrantDark, vibrantLight, lightStyle), 0.0, 1.0);
            c = mix(c, rim, h);
        }
    }

    // SDR holding tone: the interior is held at 0.97 white, the outer 1-2 pt keeps full
    // brightness, which reads as a fine bright edge.
    c *= mix(1.0, 0.97, clamp(-(d + 1.0), 0.0, 1.0));
    return clamp(c, 0.0, 1.0) * (1.0 - overlay.a) + overlay.rgb;
}

half4 main(float2 coord) {
    float2 p = coord / max(pixelScale, float2(1e-3));
    float3 sdf = sceneSdf(p);
    float d = sdf.x;
    float2 n = sdf.yz;
    float aa = max(dot(abs(n), 1.0 / max(pixelScale, float2(1e-3))), 1e-4);
    float coverage = clamp(0.5 - d / aa, 0.0, 1.0);
    float4 base = float4(0.0);
    // The face is opaque. Only the shadow/antialiased rim and debug views need
    // the original backdrop, which otherwise costs a full-resolution read.
    if (sampleCulling < 0.5 || debugMode > 0.5 || (backdropMode > 0.0 && coverage < 1.0)) {
        base = float4(src.eval(coord));
    }

    // Appearance from the backdrop's mean luminance. Light glass over a near-black backdrop
    // flips to dark and dark glass over a near-white one flips to light.
    float Y = meanLuma();
    float lightStyle = dark > 0.5 ? smoothstep(0.82, 0.88, Y) : smoothstep(0.17, 0.2, Y);

    float white;
    float black;
    float4 fill;
    float4 shadowFill;
    if (largeClass > 0.5) {
        white = mix(0.6, 1.03, lightStyle);
        black = mix(0.2, 0.5, lightStyle);
        fill = mix(float4(0.0, 0.0, 0.0, 0.4), float4(1.0, 1.0, 1.0, 0.4), lightStyle);
        shadowFill = float4(0.0, 0.0, 0.0, mix(0.0, 0.12, lightStyle));
    } else {
        black = max(Y - 0.15, 0.1);
        white = min(Y + 0.45, mix(0.9, 1.03, lightStyle));
        float4 lightFill = float4(1.0, 1.0, 1.0, min(0.75 - 0.5 * Y, 0.6));
        float4 darkFill = float4(0.0, 0.0, 0.0, min(0.25 + 0.335 * Y, 0.5));
        fill = mix(darkFill, lightFill, lightStyle);
        shadowFill = float4(0.0, 0.0, 0.0, mix(0.0, clamp(0.38 - 0.2 * Y, 0.05, 0.3), lightStyle));
    }
    if (clearGlass > 0.5) {
        white = 1.15;
        black = 0.075;
        fill = float4(0.0);
    }
    float saturation = clearGlass > 0.5 ? 1.06 : 1.0;

    if (debugMode > 0.5) {
        float3 v = float3(Y);
        if (debugMode > 1.5) { v = unpremultiply(base); }
        if (debugMode > 2.5) { v = unpremultiply(float4(sampleBlur0(coord))); }
        if (debugMode > 3.5) { v = unpremultiply(float4(sampleBlur1(coord))); }
        if (debugMode > 4.5) { v = unpremultiply(float4(sampleBlur2(coord))); }
        if (debugMode > 5.5) { v = unpremultiply(float4(sampleBlur3(coord))); }
        if (debugMode > 6.5) { v = unpremultiply(float4(sampleBlur4(coord))); }
        if (debugMode > 7.5) { v = float3(clamp(0.5 - d / 40.0, 0.0, 1.0), n * 0.5 + 0.5); }
        return half4(mix(base * backdropMode, float4(v, 1.0), coverage));
    }

    // What this pass shades itself. In one pass (stage 0) that is the shadow outside and under
    // the antialiased rim, and the face wherever the shape covers. With the half-resolution
    // material, stage 1 fills the face cell inside the shape and stage 2 the shadow cell
    // outside it plus the few pixels inside that the composite's bilinear read can reach;
    // stage 3 then shades only the rim band itself, where the highlight, the outer refraction
    // and the holding tone change within a point, and reads the rest from the atlas. The
    // face and the shadow have their own cells so that neither bleeds into the other's edge.
    bool cellFace = materialStage > 0.5 && materialStage < 1.5;
    bool cellShadow = materialStage > 1.5 && materialStage < 2.5;
    bool composite = materialStage > 2.5;
    bool needShadow = shadowOpacity > 0.0 && !cellFace && (cellShadow ? d > -materialReach : coverage < 1.0);
    bool needFace = !cellShadow && (cellFace ? d < 0.0 : coverage > 0.0);

    float4 shadow = float4(0.0);
    if (needShadow) {
        if (composite) {
            shadow = float4(material.eval(clamp(coord * materialShadow.xy + materialShadow.zw,
                                                materialShadowCell.xy, materialShadowCell.zw)));
        } else {
            shadow = shadowAt(p, d, n, lightStyle, shadowFill);
        }
    }
    float3 c = float3(0.0);
    if (needFace) {
        if (composite && -d > materialEdge) {
            c = float4(material.eval(clamp(coord * materialFace.xy + materialFace.zw,
                                           materialFaceCell.xy, materialFaceCell.zw))).rgb;
        } else {
            c = faceAt(p, d, n, aa, lightStyle, white, black, saturation, fill);
        }
    }
    if (cellShadow) {
        return half4(shadow);
    }
    if (cellFace) {
        return half4(float4(c, 1.0));
    }

    float4 under = shadow + base * (1.0 - shadow.a) * backdropMode;
    return half4(mix(under, float4(c, 1.0), coverage));
}
)SKSL";

}  // namespace cc::starry_shaders
