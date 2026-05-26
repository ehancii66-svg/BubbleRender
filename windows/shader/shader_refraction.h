//
// Desktop OpenGL 3.3 version of refraction shader
//

#ifndef WINDOWS_SHADER_REFRACTION_H
#define WINDOWS_SHADER_REFRACTION_H

const char* refractionVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform vec3 uCameraPos;

out vec3 vEyeVector;
out vec3 vWorldNormal;
out vec2 vUV;
out mat4 vView;
out mat4 vProj;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
    vEyeVector = normalize(worldPos.xyz - uCameraPos);
    vWorldNormal = normalize(uModel * vec4(aNormal, 0.0)).xyz;
    vUV = aTexCoord;
    vView = uView;
    vProj = uProj;
}
)";

const char* refractionFragmentShader = R"(#version 330 core

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
uniform sampler2D uBackgroundTexture;

in vec3 vEyeVector;
in vec3 vWorldNormal;
in vec2 vUV;
in mat4 vView;
in mat4 vProj;

out vec4 FragColor;

float fresnel(vec3 eye, vec3 normal, float power) {
    float fresnelFactor = abs(dot(eye, normal));
    return pow(1.0 - fresnelFactor, power);
}

float specular(vec3 light, vec3 eye, vec3 normal, float shininess, float diffuseness) {
    vec3 lightVector = normalize(-light);
    vec3 halfVector = normalize(eye + lightVector);
    float NdotL = max(0.0, dot(normal, lightVector));
    float NdotH = max(0.0, dot(normal, halfVector));
    float kSpecular = pow(NdotH * NdotH, shininess);
    return kSpecular + NdotL * diffuseness;
}

void main() {
    float iorRatio = 1.0/1.33;

    vec3 normal = normalize(vWorldNormal);
    vec3 eye = vEyeVector;

    vec3 refractVec = mat3(vView) * refract(eye, normal, iorRatio);

    // Edge factor: 0 at center, 1 at silhouette
    float edge = 1.0 - abs(dot(eye, normal));
    float edgeBoost = mix(1.0, uEdgeDistortionBoost, pow(edge, 1.5));

    vec2 offsetPixels = refractVec.xy * uRefractionStrength * uSpherePixelRadius * edgeBoost;

    // Clamp to a ratio of sphere radius to prevent sampling beyond FBO margin
    float maxOffset = uSpherePixelRadius * uMaxOffsetRatio;
    offsetPixels = clamp(offsetPixels, vec2(-maxOffset), vec2(maxOffset));

    // Map screen pixel to FBO pixel (FBO is larger, content is centered)
    vec2 screenPixel = gl_FragCoord.xy;
    vec2 pad = (uFBOSize - uWinResolution) * 0.5;
    vec2 fboPixel = screenPixel + pad;

    vec2 samplePixel = clamp(fboPixel + offsetPixels, vec2(0.0), uFBOSize);
    vec2 sampleUV = samplePixel / uFBOSize;

    vec3 color = texture(uBackgroundTexture, sampleUV).rgb;

    float specularLight = specular(uLight, eye, normal, uShininess, uDiffuseness);
    color += specularLight;

    float f = fresnel(eye, normal, uFresnelPower);
    color += f * vec3(1.0);

    FragColor = vec4(color, 1.0);
}
)";

#endif // WINDOWS_SHADER_REFRACTION_H
