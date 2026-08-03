
//
// BubbleRender - Iridescent soap bubble based on Kim2012
// OpenGL ES 3.0 version for HarmonyOS
//

#ifndef REFRACTIONMODEL_SHADER_REFRACTION_H
#define REFRACTIONMODEL_SHADER_REFRACTION_H

const char* refractionVertexShader = R"(#version 300 es
precision highp float;
precision highp int;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 5) in vec3 aFilmDirection;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform vec3 uCameraPos;
uniform float uTime;
uniform float uGeometryWobbleStrength;
uniform float uContactDeformStrength;
uniform int uVisualContactCount;
uniform vec3 uVisualContactPlanePoints[4];
uniform vec3 uVisualContactPlaneNormals[4];
uniform float uVisualContactBlendWidths[4];
uniform float uVisualContactStrengths[4];
uniform int uForceSharedFilm;

out vec3 vEyeVector;
out vec3 vWorldNormal;
out vec3 vFilmDirection;
out vec3 vWorldPos;
out vec2 vUV;
out mat4 vView;
out mat4 vProj;
out float vContactMask;
out float vContactRim;
out float vSharedFilmMask;
out float vShellCoverage;

void main() {
    vec3 localNormal = normalize(aNormal);
    float phase = dot(uModel[3].xyz, vec3(0.73, 1.37, 0.51));
    float t = uTime + phase;

    // Low-order surface modes: broad, slow deformations similar to
    // surface-tension-damped bubble oscillation, not high-frequency noise.
    float quadrupoleY = localNormal.y * localNormal.y - 0.333333;
    float quadrupoleXZ = localNormal.x * localNormal.x - localNormal.z * localNormal.z;
    float saddle = localNormal.x * localNormal.z;
    float skew = localNormal.y * (localNormal.x - localNormal.z);
    float wobble = 0.95 * quadrupoleY * sin(t * 0.42);
    wobble += 0.72 * quadrupoleXZ * sin(t * 0.31 + 1.7);
    wobble += 0.58 * saddle * cos(t * 0.37 + 0.9);
    wobble += 0.38 * skew * sin(t * 0.26 + 2.4);

    // A very small capillary ripple rides on top of the large-scale shape.
    wobble += 0.10 * sin(dot(localNormal, vec3(1.7, 0.6, 1.1)) * 3.0 + t * 0.18);
    vec3 deformedPos = aPos + localNormal * wobble * uGeometryWobbleStrength;

    // Contact-side deformation. The C++ model matrix aligns local +X with
    // the direction toward the other bubble, so this flattens only the side
    // that is actually touching and slightly bulges the surrounding rim.
    float contactMask = exp(-pow((1.0 - localNormal.x) / 0.34, 2.0)) * uContactDeformStrength;
    float rimMask = exp(-pow((localNormal.x - 0.62) / 0.20, 2.0)) * uContactDeformStrength;
    deformedPos.x -= contactMask * 0.280;
    deformedPos.yz += localNormal.yz * rimMask * 0.070;
    vec4 worldPos = uModel * vec4(deformedPos, 1.0);
    gl_Position = uProj * uView * worldPos;
    vEyeVector = normalize(worldPos.xyz - uCameraPos);
    mat3 normalMatrix = transpose(inverse(mat3(uModel)));
    vWorldNormal = normalize(normalMatrix * aNormal);
    vec3 localFilmDirection = length(aFilmDirection) > 1e-5
        ? normalize(aFilmDirection)
        : localNormal;
    vFilmDirection = normalize(normalMatrix * localFilmDirection);
    vWorldPos = worldPos.xyz;
    vUV = aTexCoord;
    vView = uView;
    vProj = uProj;
    vContactMask = contactMask;
    vContactRim = rimMask;
    vSharedFilmMask = max(1.0 - smoothstep(-0.98, -0.92, aTexCoord.x),
                          float(uForceSharedFilm));
    float shellCoverage = 1.0;
    for (int i = 0; i < 4; ++i) {
        if (i >= uVisualContactCount) {
            break;
        }
        float signedDistance = dot(worldPos.xyz - uVisualContactPlanePoints[i],
                                   normalize(uVisualContactPlaneNormals[i]));
        float blendWidth = max(uVisualContactBlendWidths[i], 0.0001);
        float clippedCoverage = 1.0 - smoothstep(-blendWidth, blendWidth,
                                                 signedDistance);
        shellCoverage *= mix(1.0, clippedCoverage,
                             uVisualContactStrengths[i]);
    }
    vShellCoverage = clamp(shellCoverage, 0.0, 1.0);
}
)";

const char* refractionFragmentShader = R"(#version 300 es
precision highp float;
precision highp int;

uniform float uFresnelPower;
uniform float uShininess;
uniform float uDiffuseness;
uniform float uRefractionStrength;
uniform float uEdgeDistortionBoost;
uniform float uMaxOffsetRatio;
uniform vec3 uLight;

uniform vec2 uWinResolution;
uniform vec2 uFBOSize;
uniform float uSpherePixelRadius;
uniform int uIsBackFace;
uniform int uRenderToFBO;
uniform int uIridescenceMode;
uniform sampler2D uBackgroundTexture;
uniform samplerCube uEnvironmentMap;
uniform sampler2D uThinFilmLUT;
uniform float uEnvironmentReflectionStrength;
uniform float uOutputAlpha;

uniform float uThickness;       // film thickness d (nm)
uniform float uThicknessVar;    // thickness variation amplitude (nm)
uniform float uTime;            // time for animation
uniform vec2 uTouchPoint;       // normalized screen-space touch point
uniform float uTouchStrength;   // press/drag strength
uniform float uTouchVelocity;   // normalized drag speed

in vec3 vEyeVector;
in vec3 vWorldNormal;
in vec3 vFilmDirection;
in vec3 vWorldPos;
in vec2 vUV;
in mat4 vView;
in mat4 vProj;
in float vContactMask;
in float vContactRim;
in float vSharedFilmMask;
in float vShellCoverage;

out vec4 FragColor;

// ============================================================
// Kim2012 Eq (4): Fresnel amplitude coefficients Rs, Rp
// cos(theta_t) = sqrt(n^2 - sin^2(theta_i)) / n
// ============================================================
vec2 fresnelCoefficients(float cosTheta, float n) {
    float sinTheta2 = max(0.0, 1.0 - cosTheta * cosTheta);
    float cosT = sqrt(max(0.0, n * n - sinTheta2)) / n;

    float Rs = (cosTheta - cosT) / (cosTheta + cosT);
    float Rp = (n * n * cosTheta - cosT) / (n * n * cosTheta + cosT);
    return vec2(Rs, Rp);
}

// ============================================================
// Kim2012 Eq (5): interference phase difference phi
// ============================================================
float interferencePhase(float cosTheta, float thickness, float wavelength, float n) {
    float sinTheta2 = max(0.0, 1.0 - cosTheta * cosTheta);
    float sinT = sqrt(sinTheta2) / n;
    float cosT = sqrt(max(0.0, 1.0 - sinT * sinT));
    return (4.0 * 3.14159265 * n * thickness * cosT) / wavelength;
}

// ============================================================
// Kim2012 Eq (3): single-wavelength reflectance R_lambda
// ============================================================
float thinFilmReflectance(float cosTheta, float thickness, float wavelength) {
    float n = 1.33;  // soap film refractive index (water)
    vec2 r = fresnelCoefficients(cosTheta, n);
    float Rs = r.x;
    float Rp = r.y;
    float phi = interferencePhase(cosTheta, thickness, wavelength, n);

    float Rs2 = Rs * Rs;
    float Rp2 = Rp * Rp;
    float cosPhi = cos(phi);

    float denom_s = 1.0 + Rs2 * Rs2 - 2.0 * Rs2 * cosPhi;
    float denom_p = 1.0 + Rp2 * Rp2 - 2.0 * Rp2 * cosPhi;

    float Rs_term = (denom_s > 1e-6) ? Rs2 * (1.0 - cosPhi) / denom_s : 0.0;
    float Rp_term = (denom_p > 1e-6) ? Rp2 * (1.0 - cosPhi) / denom_p : 0.0;

    return Rs_term + Rp_term;
}

// ============================================================
// Iridescence color from thin-film interference (unrolled loop)
// ============================================================
vec3 kim2012Iridescence(float NdotV, float thickness) {
    // Sample at R=615nm, G=535nm, B=465nm wavelengths
    float r = thinFilmReflectance(NdotV, thickness, 615.0);
    float g = thinFilmReflectance(NdotV, thickness, 535.0);
    float b = thinFilmReflectance(NdotV, thickness, 465.0);
    return vec3(r, g, b);
}

vec2 complexMul(vec2 a, vec2 b) {
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

vec2 complexDiv(vec2 a, vec2 b) {
    float d = max(dot(b, b), 1e-6);
    return vec2(a.x * b.x + a.y * b.y, a.y * b.x - a.x * b.y) / d;
}

float fresnelAmplitudeS(float etaA, float etaB, float cosA, float cosB) {
    return (etaA * cosA - etaB * cosB) / (etaA * cosA + etaB * cosB);
}

float fresnelAmplitudeP(float etaA, float etaB, float cosA, float cosB) {
    return (etaB * cosA - etaA * cosB) / (etaB * cosA + etaA * cosB);
}

float transmissionAmplitudeS(float etaA, float etaB, float cosA, float cosB) {
    return (2.0 * etaA * cosA) / (etaA * cosA + etaB * cosB);
}

float transmissionAmplitudeP(float etaA, float etaB, float cosA, float cosB) {
    return (2.0 * etaA * cosA) / (etaB * cosA + etaA * cosB);
}

float airyPolarizedReflectance(float cosTheta, float thickness, float wavelength, bool parallel) {
    float eta1 = 1.0;   // air
    float eta2 = 1.33;  // soap film
    float eta3 = 1.0;   // air inside the bubble

    float sin1Sq = max(0.0, 1.0 - cosTheta * cosTheta);
    float sin2 = sqrt(sin1Sq) * eta1 / eta2;
    float cos2 = sqrt(max(0.0, 1.0 - sin2 * sin2));
    float cos3 = cosTheta;

    float r12 = parallel
        ? fresnelAmplitudeP(eta1, eta2, cosTheta, cos2)
        : fresnelAmplitudeS(eta1, eta2, cosTheta, cos2);
    float r21 = parallel
        ? fresnelAmplitudeP(eta2, eta1, cos2, cosTheta)
        : fresnelAmplitudeS(eta2, eta1, cos2, cosTheta);
    float r23 = parallel
        ? fresnelAmplitudeP(eta2, eta3, cos2, cos3)
        : fresnelAmplitudeS(eta2, eta3, cos2, cos3);

    float t12 = parallel
        ? transmissionAmplitudeP(eta1, eta2, cosTheta, cos2)
        : transmissionAmplitudeS(eta1, eta2, cosTheta, cos2);
    float t21 = parallel
        ? transmissionAmplitudeP(eta2, eta1, cos2, cosTheta)
        : transmissionAmplitudeS(eta2, eta1, cos2, cosTheta);

    float phase = (4.0 * 3.14159265 * eta2 * thickness * cos2) / wavelength;
    vec2 phaseShift = vec2(cos(phase), sin(phase));
    vec2 numerator = complexMul(vec2(t12 * r23 * t21, 0.0), phaseShift);
    vec2 denominator = vec2(1.0, 0.0) - complexMul(vec2(r21 * r23, 0.0), phaseShift);
    vec2 amplitude = vec2(r12, 0.0) + complexDiv(numerator, denominator);
    return clamp(dot(amplitude, amplitude), 0.0, 1.0);
}

// Belcour2017-style Airy reflectance: sum all internal reflection orders
// for an air-soap-air thin film. Used to tint environment reflection.
float airyThinFilmReflectance(float cosTheta, float thickness, float wavelength) {
    float rs = airyPolarizedReflectance(cosTheta, thickness, wavelength, false);
    float rp = airyPolarizedReflectance(cosTheta, thickness, wavelength, true);
    return 0.5 * (rs + rp);
}

vec3 belcourAiryIridescence(float NdotV, float thickness) {
    float r = airyThinFilmReflectance(NdotV, thickness, 615.0);
    float g = airyThinFilmReflectance(NdotV, thickness, 535.0);
    float b = airyThinFilmReflectance(NdotV, thickness, 465.0);
    return vec3(r, g, b);
}

vec3 belcourAiryWeightedIridescence(float NdotV, float thickness) {
    vec3 color = vec3(0.0);
    color += airyThinFilmReflectance(NdotV, thickness, 430.0) * vec3(0.05, 0.00, 0.30);
    color += airyThinFilmReflectance(NdotV, thickness, 470.0) * vec3(0.00, 0.10, 0.75);
    color += airyThinFilmReflectance(NdotV, thickness, 500.0) * vec3(0.00, 0.40, 0.42);
    color += airyThinFilmReflectance(NdotV, thickness, 535.0) * vec3(0.10, 0.75, 0.10);
    color += airyThinFilmReflectance(NdotV, thickness, 575.0) * vec3(0.72, 0.66, 0.04);
    color += airyThinFilmReflectance(NdotV, thickness, 615.0) * vec3(0.85, 0.18, 0.02);
    color += airyThinFilmReflectance(NdotV, thickness, 650.0) * vec3(0.45, 0.02, 0.00);
    return color / 2.7;
}

vec3 spectralLUTIridescence(float NdotV, float thickness) {
    float thicknessNorm = clamp((thickness - 100.0) / 800.0, 0.0, 1.0);
    vec3 encodedReflectance = texture(uThinFilmLUT, vec2(clamp(NdotV, 0.0, 0.99), thicknessNorm)).rgb;
    return encodedReflectance * encodedReflectance;
}

// ============================================================
// 3D Simplex Noise (for film sloshing simulation)
// Fixed gradient computation
// ============================================================
vec3 mod289_s(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 mod289_s(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 permute_s(vec4 x) { return mod289_s(((x * 34.0) + 1.0) * x); }
vec4 taylorInvSqrt_s(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

float snoise(vec3 v) {
    const vec2 C = vec2(1.0 / 6.0, 1.0 / 3.0);
    const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);

    vec3 i = floor(v + dot(v, C.yyy));
    vec3 x0 = v - i + dot(i, C.xxx);

    vec3 g = step(x0.yzx, x0.xyz);
    vec3 l = 1.0 - g;
    vec3 i1 = min(g.xyz, l.zxy);
    vec3 i2 = max(g.xyz, l.zxy);

    vec3 x1 = x0 - i1 + C.xxx;
    vec3 x2 = x0 - i2 + C.yyy;
    vec3 x3 = x0 - D.yyy;

    i = mod289_s(i);
    vec4 p = permute_s(permute_s(permute_s(
        i.z + vec4(0.0, i1.z, i2.z, 1.0))
        + i.y + vec4(0.0, i1.y, i2.y, 1.0))
        + i.x + vec4(0.0, i1.x, i2.x, 1.0));

    float n_ = 0.142857142857;
    vec3 ns = n_ * D.wyz - D.xzx;

    vec4 j = p - 49.0 * floor(p * ns.z * ns.z);

    vec4 x_ = floor(j * ns.z);
    vec4 y_ = floor(j - 7.0 * x_);

    vec4 x = x_ * ns.x + ns.yyyy;
    vec4 y = y_ * ns.x + ns.yyyy;
    vec4 h = 1.0 - abs(x) - abs(y);

    vec4 b0 = vec4(x.xy, y.xy);
    vec4 b1 = vec4(x.zw, y.zw);

    vec4 s0 = floor(b0) * 2.0 + 1.0;
    vec4 s1 = floor(b1) * 2.0 + 1.0;
    vec4 sh = -step(h, vec4(0.0));

    vec4 a0 = b0.xzyw + s0.xzyw * sh.xxyy;
    vec4 a1 = b1.xzyw + s1.xzyw * sh.zzww;

    vec3 p0 = vec3(a0.xy, h.x);
    vec3 p1 = vec3(a0.zw, h.y);
    vec3 p2 = vec3(a1.xy, h.z);
    vec3 p3 = vec3(a1.zw, h.w);

    vec4 norm = taylorInvSqrt_s(vec4(dot(p0, p0), dot(p1, p1), dot(p2, p2), dot(p3, p3)));
    p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;

    vec4 m = max(0.6 - vec4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    m = m * m;

    // Fixed: use dot(p_i, x_i) for gradient, not dot(p_i, p_i)
    return 42.0 * dot(m * m, vec4(dot(p0, x0), dot(p1, x1), dot(p2, x2), dot(p3, x3)));
}

float filmNoise(vec3 p) {
    float n = 0.0;
    float amp = 0.5;
    n += amp * snoise(p);
    p = p * 2.03 + vec3(13.1, 7.7, 5.3);
    amp *= 0.5;
    n += amp * snoise(p);
    p = p * 2.07 + vec3(3.9, 17.2, 11.5);
    amp *= 0.5;
    n += amp * snoise(p);
    p = p * 2.11 + vec3(19.4, 2.6, 23.8);
    amp *= 0.5;
    n += amp * snoise(p);
    return n * 0.53 + 0.5;
}

// ============================================================
// Fresnel reflection term (Kim2012 Eq 9 approximation)
// ============================================================
float fresnel(vec3 eye, vec3 normal, float power) {
    float fresnelFactor = abs(dot(eye, normal));
    return pow(1.0 - fresnelFactor, power);
}

// ============================================================
// Blinn-Phong specular highlight
// ============================================================
float specular(vec3 light, vec3 eye, vec3 normal, float shininess, float diffuseness) {
    vec3 lightVector = normalize(-light);
    vec3 halfVector = normalize(eye + lightVector);
    float NdotL = max(0.0, dot(normal, lightVector));
    float NdotH = max(0.0, dot(normal, halfVector));
    float kSpecular = pow(NdotH * NdotH, shininess);
    return kSpecular + NdotL * diffuseness;
}
)"
R"(

// ============================================================
// Main
// ============================================================
void main() {
    float iorRatio = 1.0 / 1.33;

    vec3 normal = normalize(vWorldNormal);
    if (uIsBackFace == 1) {
        normal = -normal;
    }

    vec3 eye = vEyeVector;
    float NdotV = abs(dot(eye, normal));
    vec2 screenUVForTouch = gl_FragCoord.xy / uWinResolution.xy;
    vec2 touchDelta = screenUVForTouch - uTouchPoint;
    touchDelta.x *= uWinResolution.x / max(uWinResolution.y, 1.0);
    float touchDistance = length(touchDelta);
    float touchMask = exp(-touchDistance * touchDistance / 0.0065) * uTouchStrength;
    float touchRing = sin(touchDistance * 78.0 - uTime * 18.0) * exp(-touchDistance * 9.0);
    float touchRipple = touchRing * uTouchStrength * (0.35 + uTouchVelocity * 0.65);

    // ---- 1. Iridescence (Kim2012 thin-film interference) ----
    // Dynamic thickness using Simplex noise for sloshing effect
    float filmDirectionLengthSquared = dot(vFilmDirection, vFilmDirection);
    vec3 filmDir = filmDirectionLengthSquared > 1e-10
        ? vFilmDirection * inversesqrt(filmDirectionLengthSquared)
        : normalize(vWorldNormal);
    float flowSpeed = 1.45;
    vec3 slowFlow = vec3(uTime * 0.10, -uTime * 0.20, uTime * 0.08) * flowSpeed;
    vec3 warp = vec3(
        snoise(filmDir * 1.15 + slowFlow),
        snoise(filmDir * 1.25 + slowFlow.yzx + vec3(4.1, 1.3, 2.7)),
        snoise(filmDir * 1.05 + slowFlow.zxy + vec3(8.2, 5.4, 0.9))
    ) * 0.12;
    float broadNoise = filmNoise(filmDir * 2.0 + warp + slowFlow);
    float flowNoise = filmNoise(filmDir * 3.4 + warp * 0.7 + slowFlow.yzx * 0.8);
    float fineNoise = filmNoise(filmDir * 6.2 + slowFlow.zxy * 0.38);
    float drainage = smoothstep(-0.85, 0.85, -filmDir.y);
    float thicknessPattern = (broadNoise - 0.5) * 0.52
        + (flowNoise - 0.5) * 0.30
        + (fineNoise - 0.5) * 0.04
        + (drainage - 0.5) * 0.46;
    float dynamicThickness = uThickness
        + thicknessPattern * uThicknessVar
        + touchMask * 190.0
        + touchRipple * 95.0;
    vec3 kimReflectance = kim2012Iridescence(NdotV, dynamicThickness);
    vec3 lutReflectance = spectralLUTIridescence(NdotV, dynamicThickness);
    lutReflectance *= 0.85;
    vec3 airyReflectance = belcourAiryWeightedIridescence(NdotV, dynamicThickness);
    airyReflectance *= 1.35;
    vec3 filmReflectance = kimReflectance;
    if (uIridescenceMode == 1) {
        filmReflectance = lutReflectance;
    } else if (uIridescenceMode == 2) {
        filmReflectance = airyReflectance;
    }

    // ---- 2. Refraction (screen-space FBO distortion) ----
    // Use eye (camera->surface) as incident direction for refraction
    vec3 refractVec = mat3(vView) * refract(eye, normal, iorRatio);
    float edge = 1.0 - NdotV;
    float edgeProfile = smoothstep(0.18, 0.92, edge);
    float edgeBoost = mix(1.0, uEdgeDistortionBoost, pow(edgeProfile, 0.85));
    float touchBoost = 1.0 + touchMask * 2.4 + abs(touchRipple) * 1.1;
    float surfaceRefractionScale = (uIsBackFace == 1) ? 0.16 : 1.0;
    float thicknessRefraction = mix(0.08, 0.45, clamp(dynamicThickness / 1000.0, 0.0, 1.0));
    float thinFilmRefraction = mix(0.08, 1.28, pow(edgeProfile, 1.15)) * thicknessRefraction;
    vec2 offsetPixels = refractVec.xy
        * uRefractionStrength
        * surfaceRefractionScale
        * thinFilmRefraction
        * uSpherePixelRadius
        * edgeBoost
        * touchBoost;
    offsetPixels += normalize(touchDelta + vec2(1e-4, 0.0))
        * touchRipple
        * uSpherePixelRadius
        * 0.055
        * surfaceRefractionScale;
    float maxOffset = uSpherePixelRadius * uMaxOffsetRatio;
    offsetPixels = clamp(offsetPixels, vec2(-maxOffset), vec2(maxOffset));
    vec2 screenPixel = gl_FragCoord.xy;
    vec2 fboPixel = (uRenderToFBO == 1)
        ? screenPixel
        : screenPixel * (uFBOSize / uWinResolution);
    vec2 samplePixel = clamp(fboPixel + offsetPixels, vec2(0.0), uFBOSize - vec2(1.0));
    vec2 sampleUV = samplePixel / uFBOSize;
    vec3 bgColor = texture(uBackgroundTexture, sampleUV).rgb;

    // ---- 3. Color compositing (soap bubble appearance) ----
    // Kim2012: film reflectance R_lambda gives reflected fraction per wavelength.
    // Transmitted light = 1 - R_lambda (complementary colors).
    // Reflected light creates visible iridescence; transmitted light shows background.

    // Moderate boost: raw reflectance values are 0.01-0.8, need extra visibility.
    float surfaceColorScale = (uIsBackFace == 1) ? 0.18 : 1.0;
    float colorBoost = (uIridescenceMode == 1) ? 1.05 : ((uIridescenceMode == 2) ? 1.35 : 1.55);
    vec3 iridescence = filmReflectance * colorBoost * surfaceColorScale;
    iridescence *= 1.0 + touchMask * 1.5 + abs(touchRipple) * 0.55;

    // Fresnel edge factor: 0 at center, 1 at silhouette
    float fresnelTerm = pow(1.0 - NdotV, uFresnelPower);

    // Iridescence gets stronger at glancing angles (Fresnel enhancement)
    float fresnelColorBoost = (uIridescenceMode == 1) ? 0.95 : ((uIridescenceMode == 2) ? 1.35 : 1.8);
    iridescence *= (1.0 + fresnelTerm * fresnelColorBoost);

    // Soft clamp to prevent neon colors at edges: x/(1+x) maps [0,inf) to [0,1)
    iridescence = iridescence / (1.0 + iridescence);

    // A soap film is much thinner than a glass sphere: keep the center nearly clear
    // and let the silhouette carry most of the visible surface response.
    float transparency = mix(0.97, 0.78, fresnelTerm);

    // Transmitted background + local thin-film color.
    vec3 color = bgColor * transparency + iridescence;

    // Environment reflection: the cubemap reflection is tinted by thin-film
    // reflectance, so iridescence behaves like colored reflected light.
    vec3 reflectDir = normalize(reflect(eye, normal));
    vec3 envColor = texture(uEnvironmentMap, reflectDir).rgb;

    if (uIridescenceMode == 2) {
        vec3 R = clamp(airyReflectance * 3.5, vec3(0.0), vec3(0.9));
        float luma = dot(R, vec3(0.2126, 0.7152, 0.0722));
        R = clamp(mix(vec3(luma), R, 2.2), vec3(0.0), vec3(0.9));

        float edgeW = mix(0.25, 1.3, fresnelTerm);
        vec3 reflected = envColor * R * edgeW;
        vec3 transmitted = bgColor * (1.0 - R * 0.55);

        float specularLight = specular(uLight, eye, normal, uShininess, uDiffuseness);
        vec3 airyColor = transmitted + reflected * 1.8;
        airyColor += specularLight * 0.08 * surfaceColorScale;
        airyColor = mix(airyColor, vec3(1.0), fresnelTerm * 0.035 * surfaceColorScale);

        float localAlpha = uOutputAlpha * mix(1.0, 0.65, vSharedFilmMask) * vShellCoverage;
        FragColor = vec4(airyColor, localAlpha);
        return;
    }

    float reflectionTintBoost = (uIridescenceMode == 1) ? 1.25 : ((uIridescenceMode == 2) ? 1.75 : 2.4);
    vec3 filmReflectionTint = filmReflectance * (1.0 + fresnelTerm * reflectionTintBoost);
    filmReflectionTint = filmReflectionTint / (1.0 + filmReflectionTint);
    float reflectionWeightBoost = (uIridescenceMode == 1) ? 0.78 : ((uIridescenceMode == 2) ? 1.18 : 1.0);
    float envReflectionWeight = uEnvironmentReflectionStrength
        * surfaceColorScale
        * reflectionWeightBoost
        * mix(0.06, 0.55, fresnelTerm);
    color += envColor * filmReflectionTint * envReflectionWeight;

    // Specular highlight (bright spot from light source)
    float specularLight = specular(uLight, eye, normal, uShininess, uDiffuseness);
    color += specularLight * 0.35 * surfaceColorScale;

    // Fresnel white edge glow (environment reflection at grazing angles)
    color = mix(color, vec3(1.0), fresnelTerm * 0.06 * surfaceColorScale);

    float localAlpha = uOutputAlpha * mix(1.0, 0.65, vSharedFilmMask) * vShellCoverage;
    FragColor = vec4(color, localAlpha);
}
)";

#endif // WINDOWS_SHADER_REFRACTION_H
