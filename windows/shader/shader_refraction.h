//
// BubbleRender - Iridescent soap bubble based on Kim2012
// Desktop OpenGL 3.3 version
//

#ifndef WINDOWS_SHADER_REFRACTION_H
#define WINDOWS_SHADER_REFRACTION_H

const char *refractionVertexShader = R"(#version 330 core
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
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
    vEyeVector = normalize(worldPos.xyz - uCameraPos);
    vWorldNormal = normalize(uModel * vec4(aNormal, 0.0)).xyz;
    vWorldPos = worldPos.xyz;
    vUV = aTexCoord;
    vView = uView;
    vProj = uProj;
}
)";

const char *refractionFragmentShader = R"(#version 330 core

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
uniform sampler2D uBackgroundTexture;
uniform samplerCube uEnvironmentMap;
uniform float uEnvironmentReflectionStrength;

uniform float uThickness;       // film thickness d (nm)
uniform float uThicknessVar;    // thickness variation amplitude (nm)
uniform float uTime;            // time for animation

in vec3 vEyeVector;
in vec3 vWorldNormal;
in vec3 vWorldPos;
in vec2 vUV;
in mat4 vView;
in mat4 vProj;

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

    // ---- 1. Iridescence (Kim2012 thin-film interference) ----
    // Dynamic thickness using Simplex noise for sloshing effect
    vec3 filmDir = normalize(vWorldNormal);
    float flowSpeed = 1.25;
    vec3 slowFlow = vec3(uTime * 0.09, -uTime * 0.17, uTime * 0.07) * flowSpeed;
    vec3 warp = vec3(
        snoise(filmDir * 1.15 + slowFlow),
        snoise(filmDir * 1.25 + slowFlow.yzx + vec3(4.1, 1.3, 2.7)),
        snoise(filmDir * 1.05 + slowFlow.zxy + vec3(8.2, 5.4, 0.9))
    ) * 0.12;
    float broadNoise = filmNoise(filmDir * 2.0 + warp + slowFlow);
    float flowNoise = filmNoise(filmDir * 3.4 + warp * 0.7 + slowFlow.yzx * 0.8);
    float fineNoise = filmNoise(filmDir * 7.0 + slowFlow.zxy * 0.45);
    float drainage = smoothstep(-0.85, 0.85, -filmDir.y);
    float thicknessPattern = (broadNoise - 0.5) * 0.52
        + (flowNoise - 0.5) * 0.30
        + (fineNoise - 0.5) * 0.06
        + (drainage - 0.5) * 0.42;
    float dynamicThickness = uThickness + thicknessPattern * uThicknessVar;
    vec3 filmReflectance = kim2012Iridescence(NdotV, dynamicThickness);
    vec3 airyReflectance = belcourAiryIridescence(NdotV, dynamicThickness);

    // ---- 2. Refraction (screen-space FBO distortion) ----
    // Use eye (camera->surface) as incident direction for refraction
    vec3 refractVec = mat3(vView) * refract(eye, normal, iorRatio);
    float edge = 1.0 - NdotV;
    float edgeBoost = mix(1.0, uEdgeDistortionBoost, pow(edge, 1.5));
    float surfaceRefractionScale = (uIsBackFace == 1) ? 0.35 : 1.0;
    vec2 offsetPixels = refractVec.xy * uRefractionStrength * surfaceRefractionScale * uSpherePixelRadius * edgeBoost;
    float maxOffset = uSpherePixelRadius * uMaxOffsetRatio;
    offsetPixels = clamp(offsetPixels, vec2(-maxOffset), vec2(maxOffset));
    vec2 screenPixel = gl_FragCoord.xy;
    vec2 pad = (uFBOSize - uWinResolution) * 0.5;
    vec2 fboPixel = screenPixel + pad;
    vec2 samplePixel = clamp(fboPixel + offsetPixels, vec2(0.0), uFBOSize);
    vec2 sampleUV = samplePixel / uFBOSize;
    vec3 bgColor = texture(uBackgroundTexture, sampleUV).rgb;

    // ---- 3. Color compositing (soap bubble appearance) ----
    // Kim2012: film reflectance R_lambda gives reflected fraction per wavelength.
    // Transmitted light = 1 - R_lambda (complementary colors).
    // Reflected light creates visible iridescence; transmitted light shows background.

    // Moderate boost: raw reflectance values are 0.01-0.8, need extra visibility.
    float surfaceColorScale = (uIsBackFace == 1) ? 0.18 : 1.0;
    vec3 iridescence = filmReflectance * 1.55 * surfaceColorScale;

    // Fresnel edge factor: 0 at center, 1 at silhouette
    float fresnelTerm = pow(1.0 - NdotV, uFresnelPower);

    // Iridescence gets stronger at glancing angles (Fresnel enhancement)
    iridescence *= (1.0 + fresnelTerm * 1.8);

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
    vec3 filmReflectionTint = airyReflectance * (1.0 + fresnelTerm * 2.4);
    filmReflectionTint = filmReflectionTint / (1.0 + filmReflectionTint);
    float envReflectionWeight = uEnvironmentReflectionStrength
        * surfaceColorScale
        * mix(0.06, 0.55, fresnelTerm);
    color += envColor * filmReflectionTint * envReflectionWeight;

    // Specular highlight (bright spot from light source)
    float specularLight = specular(uLight, eye, normal, uShininess, uDiffuseness);
    color += specularLight * 0.35 * surfaceColorScale;

    // Fresnel white edge glow (environment reflection at grazing angles)
    color = mix(color, vec3(1.0), fresnelTerm * 0.06 * surfaceColorScale);

    FragColor = vec4(color, 1.0);
}
)";

#endif // WINDOWS_SHADER_REFRACTION_H
