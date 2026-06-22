//
// BubbleRender — Full refraction shader with thin-film iridescence
// OpenGL ES 3.0 (GLSL 300 es) version for HarmonyOS
//

#ifndef REFRACTIONMODEL_SHADER_REFRACTION_H
#define REFRACTIONMODEL_SHADER_REFRACTION_H

const char* refractionVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform vec3 uCameraPos;

out vec3 vEyeVector;
out vec3 vWorldNormal;
out vec3 vWorldPos;
out vec2 vUV;
out mat4 vView;
out mat4 vProj;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    gl_Position = uProj * uView * worldPos;
    vEyeVector = normalize(worldPos.xyz - uCameraPos);
    vWorldNormal = normalize(transpose(inverse(mat3(uModel))) * aNormal);
    vWorldPos = worldPos.xyz;
    vUV = aTexCoord;
    vView = uView;
    vProj = uProj;
}
)";

// ---- Fragment shader helper functions (shared by all modes) ----
const char* refractionFragmentCommon = R"(#version 300 es
precision highp float;

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

uniform float uThickness;
uniform float uThicknessVar;
uniform float uTime;
uniform vec2 uTouchPoint;
uniform float uTouchStrength;
uniform float uTouchVelocity;

in vec3 vEyeVector;
in vec3 vWorldNormal;
in vec3 vWorldPos;
in vec2 vUV;
in mat4 vView;
in mat4 vProj;

out vec4 FragColor;

// ============================================================
// Fresnel amplitude coefficients Rs, Rp
// ============================================================
vec2 fresnelCoefficients(float cosTheta, float n) {
    float sinTheta2 = max(0.0, 1.0 - cosTheta * cosTheta);
    float cosT = sqrt(max(0.0, n * n - sinTheta2)) / n;
    float Rs = (cosTheta - cosT) / (cosTheta + cosT);
    float Rp = (n * n * cosTheta - cosT) / (n * n * cosTheta + cosT);
    return vec2(Rs, Rp);
}

// ============================================================
// Interference phase difference
// ============================================================
float interferencePhase(float cosTheta, float thickness, float wavelength, float n) {
    float sinTheta2 = max(0.0, 1.0 - cosTheta * cosTheta);
    float sinT = sqrt(sinTheta2) / n;
    float cosT = sqrt(max(0.0, 1.0 - sinT * sinT));
    return (4.0 * 3.14159265 * n * thickness * cosT) / wavelength;
}

// ============================================================
// Kim2012 single-wavelength reflectance
// ============================================================
float thinFilmReflectance(float cosTheta, float thickness, float wavelength) {
    float n = 1.33;
    vec2 r = fresnelCoefficients(cosTheta, n);
    float Rs = r.x, Rp = r.y;
    float phi = interferencePhase(cosTheta, thickness, wavelength, n);
    float Rs2 = Rs * Rs, Rp2 = Rp * Rp;
    float cosPhi = cos(phi);
    float denom_s = 1.0 + Rs2 * Rs2 - 2.0 * Rs2 * cosPhi;
    float denom_p = 1.0 + Rp2 * Rp2 - 2.0 * Rp2 * cosPhi;
    float Rs_term = (denom_s > 1e-6) ? Rs2 * (1.0 - cosPhi) / denom_s : 0.0;
    float Rp_term = (denom_p > 1e-6) ? Rp2 * (1.0 - cosPhi) / denom_p : 0.0;
    return Rs_term + Rp_term;
}

vec3 kim2012Iridescence(float NdotV, float thickness) {
    float r = thinFilmReflectance(NdotV, thickness, 615.0);
    float g = thinFilmReflectance(NdotV, thickness, 535.0);
    float b = thinFilmReflectance(NdotV, thickness, 465.0);
    return vec3(r, g, b);
}

// ============================================================
// Belcour Airy: complex arithmetic helpers
// ============================================================
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
    float eta1 = 1.0, eta2 = 1.33, eta3 = 1.0;
    float sin1Sq = max(0.0, 1.0 - cosTheta * cosTheta);
    float sin2 = sqrt(sin1Sq) * eta1 / eta2;
    float cos2 = sqrt(max(0.0, 1.0 - sin2 * sin2));
    float cos3 = cosTheta;
    float r12 = parallel ? fresnelAmplitudeP(eta1, eta2, cosTheta, cos2) : fresnelAmplitudeS(eta1, eta2, cosTheta, cos2);
    float r21 = parallel ? fresnelAmplitudeP(eta2, eta1, cos2, cosTheta) : fresnelAmplitudeS(eta2, eta1, cos2, cosTheta);
    float r23 = parallel ? fresnelAmplitudeP(eta2, eta3, cos2, cos3) : fresnelAmplitudeS(eta2, eta3, cos2, cos3);
    float t12 = parallel ? transmissionAmplitudeP(eta1, eta2, cosTheta, cos2) : transmissionAmplitudeS(eta1, eta2, cosTheta, cos2);
    float t21 = parallel ? transmissionAmplitudeP(eta2, eta1, cos2, cosTheta) : transmissionAmplitudeS(eta2, eta1, cos2, cosTheta);
    float phase = (4.0 * 3.14159265 * eta2 * thickness * cos2) / wavelength;
    vec2 phaseShift = vec2(cos(phase), sin(phase));
    vec2 numerator = complexMul(vec2(t12 * r23 * t21, 0.0), phaseShift);
    vec2 denominator = vec2(1.0, 0.0) - complexMul(vec2(r21 * r23, 0.0), phaseShift);
    vec2 amplitude = vec2(r12, 0.0) + complexDiv(numerator, denominator);
    return clamp(dot(amplitude, amplitude), 0.0, 1.0);
}

float airyThinFilmReflectance(float cosTheta, float thickness, float wavelength) {
    float rs = airyPolarizedReflectance(cosTheta, thickness, wavelength, false);
    float rp = airyPolarizedReflectance(cosTheta, thickness, wavelength, true);
    return 0.5 * (rs + rp);
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

// ============================================================
// Spectral LUT lookup
// ============================================================
vec3 spectralLUTIridescence(float NdotV, float thickness) {
    float thicknessNorm = clamp((thickness - 100.0) / 800.0, 0.0, 1.0);
    vec3 encodedReflectance = texture(uThinFilmLUT, vec2(clamp(NdotV, 0.0, 0.99), thicknessNorm)).rgb;
    return encodedReflectance * encodedReflectance;
}

// ============================================================
// 3D Simplex Noise — for film sloshing
// ============================================================
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289(((x * 34.0) + 1.0) * x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

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
    i = mod289(i);
    vec4 p = permute(permute(permute(
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
    vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));
    p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
    vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
    m = m * m;
    return 42.0 * dot(m * m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}

float filmNoise(vec3 p) {
    float n = 0.0, amp = 0.5;
    n += amp * snoise(p); p = p * 2.03 + vec3(13.1, 7.7, 5.3); amp *= 0.5;
    n += amp * snoise(p); p = p * 2.07 + vec3(3.9, 17.2, 11.5);   amp *= 0.5;
    n += amp * snoise(p); p = p * 2.11 + vec3(19.4, 2.6, 23.8);    amp *= 0.5;
    n += amp * snoise(p);
    return n * 0.53 + 0.5;
}

// ============================================================
// Fresnel + specular
// ============================================================
float fresnel(vec3 eye, vec3 normal, float power) {
    return pow(1.0 - abs(dot(eye, normal)), power);
}
float specular(vec3 light, vec3 eye, vec3 normal, float shininess, float diffuseness) {
    vec3 lightVector = normalize(-light);
    vec3 halfVector = normalize(eye + lightVector);
    float NdotL = max(0.0, dot(normal, lightVector));
    float NdotH = max(0.0, dot(normal, halfVector));
    return pow(NdotH * NdotH, shininess) + NdotL * diffuseness;
}
)";

// ---- Main function ----
const char* refractionFragmentMain = R"(
void main() {
    float iorRatio = 1.0 / 1.33;

    vec3 normal = normalize(vWorldNormal);
    if (uIsBackFace == 1) normal = -normal;

    vec3 eye = vEyeVector;
    float NdotV = abs(dot(eye, normal));

    // ---- Touch ripple ----
    vec2 screenUVForTouch = gl_FragCoord.xy / uWinResolution.xy;
    vec2 touchDelta = screenUVForTouch - uTouchPoint;
    touchDelta.x *= uWinResolution.x / max(uWinResolution.y, 1.0);
    float touchDistance = length(touchDelta);
    float touchMask = exp(-touchDistance * touchDistance / 0.0065) * uTouchStrength;
    float touchRing = sin(touchDistance * 78.0 - uTime * 18.0) * exp(-touchDistance * 9.0);
    float touchRipple = touchRing * uTouchStrength * (0.35 + uTouchVelocity * 0.65);

    // ---- Dynamic film thickness ----
    vec3 filmDir = normalize(vWorldNormal);
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

    // ---- Iridescence ----
    vec3 kimReflectance = kim2012Iridescence(NdotV, dynamicThickness);
    vec3 lutReflectance = spectralLUTIridescence(NdotV, dynamicThickness) * 0.85;
    vec3 airyReflectance = belcourAiryWeightedIridescence(NdotV, dynamicThickness) * 1.35;
    vec3 filmReflectance = kimReflectance;
    if (uIridescenceMode == 1) filmReflectance = lutReflectance;
    else if (uIridescenceMode == 2) filmReflectance = airyReflectance;

    // ---- Refraction offset ----
    vec3 refractVec = mat3(vView) * refract(eye, normal, iorRatio);
    float edge = 1.0 - NdotV;
    float edgeProfile = smoothstep(0.18, 0.92, edge);
    float edgeBoost = mix(1.0, uEdgeDistortionBoost, pow(edgeProfile, 0.85));
    float touchBoost = 1.0 + touchMask * 2.4 + abs(touchRipple) * 1.1;
    float surfaceRefractionScale = (uIsBackFace == 1) ? 0.16 : 1.0;
    float thicknessRefraction = mix(0.08, 0.45, clamp(dynamicThickness / 1000.0, 0.0, 1.0));
    float thinFilmRefraction = mix(0.08, 1.28, pow(edgeProfile, 1.15)) * thicknessRefraction;
    vec2 offsetPixels = refractVec.xy
        * uRefractionStrength * surfaceRefractionScale * thinFilmRefraction
        * uSpherePixelRadius * edgeBoost * touchBoost;
    offsetPixels += normalize(touchDelta + vec2(1e-4, 0.0)) * touchRipple * uSpherePixelRadius * 0.055 * surfaceRefractionScale;
    float maxOffset = uSpherePixelRadius * uMaxOffsetRatio;
    offsetPixels = clamp(offsetPixels, vec2(-maxOffset), vec2(maxOffset));
    vec2 samplePixel = (uRenderToFBO == 1)
        ? gl_FragCoord.xy + offsetPixels
        : gl_FragCoord.xy * (uFBOSize / uWinResolution) + offsetPixels;
    vec2 sampleUV = clamp(samplePixel / uFBOSize, vec2(0.0), vec2(1.0));
    vec3 bgColor = texture(uBackgroundTexture, sampleUV).rgb;

    // ---- Color compositing ----
    float surfaceColorScale = (uIsBackFace == 1) ? 0.18 : 1.0;
    float colorBoost = (uIridescenceMode == 1) ? 1.05 : ((uIridescenceMode == 2) ? 1.35 : 1.55);
    vec3 iridescence = filmReflectance * colorBoost * surfaceColorScale;
    iridescence *= 1.0 + touchMask * 1.5 + abs(touchRipple) * 0.55;

    float fresnelTerm = pow(1.0 - NdotV, uFresnelPower);
    float fresnelColorBoost = (uIridescenceMode == 1) ? 0.95 : ((uIridescenceMode == 2) ? 1.35 : 1.8);
    iridescence *= (1.0 + fresnelTerm * fresnelColorBoost);
    iridescence = iridescence / (1.0 + iridescence);

    float transparency = mix(0.97, 0.78, fresnelTerm);
    vec3 color = bgColor * transparency + iridescence;

    // ---- Environment reflection ----
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
        FragColor = vec4(airyColor, uOutputAlpha);
        return;
    }

    float reflectionTintBoost = (uIridescenceMode == 1) ? 1.25 : 2.4;
    vec3 filmReflectionTint = filmReflectance * (1.0 + fresnelTerm * reflectionTintBoost);
    filmReflectionTint = filmReflectionTint / (1.0 + filmReflectionTint);
    float reflectionWeightBoost = (uIridescenceMode == 1) ? 0.78 : 1.0;
    float envReflectionWeight = uEnvironmentReflectionStrength * surfaceColorScale * reflectionWeightBoost * mix(0.06, 0.55, fresnelTerm);
    color += envColor * filmReflectionTint * envReflectionWeight;

    float specularLight = specular(uLight, eye, normal, uShininess, uDiffuseness);
    color += specularLight * 0.35 * surfaceColorScale;
    color = mix(color, vec3(1.0), fresnelTerm * 0.06 * surfaceColorScale);

    FragColor = vec4(color, uOutputAlpha);
}
)";

#endif //REFRACTIONMODEL_SHADER_REFRACTION_H
