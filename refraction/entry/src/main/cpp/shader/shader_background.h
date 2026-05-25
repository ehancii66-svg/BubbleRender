//
// Created on 2026/4/27.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#ifndef REFRACTIONMODEL_SHADER_BACKGROUND_H
#define REFRACTIONMODEL_SHADER_BACKGROUND_H

const char* backgroundVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
uniform mat4 uMVP;
out vec2 vUV;
out vec3 vNormal;
void main() {
    vUV = aTexCoord;
    vNormal = normalize(aNormal);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* backgroundFragmentShader = R"(#version 300 es
precision highp float;
in vec2 vUV;
in vec3 vNormal;
out vec4 FragColor;
void main() {
    FragColor = vec4(1.0, 1.0, 1.0, 1.0);
}
)";

#endif //REFRACTIONMODEL_SHADER_BACKGROUND_H
