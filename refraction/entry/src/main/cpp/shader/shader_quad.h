//
// Created on 2026/4/27.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#ifndef REFRACTIONMODEL_SHADER_QUAD_H
#define REFRACTIONMODEL_SHADER_QUAD_H

const char* quadVertexShader = R"(#version 300 es
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aPos * 0.5 + 0.5;
}
)";

const char* quadFragmentShader = R"(#version 300 es
precision highp float;
uniform sampler2D uTexture;
in vec2 vUV;
out vec4 FragColor;
void main() {
    FragColor = texture(uTexture, vUV);
}
)";

#endif //REFRACTIONMODEL_SHADER_QUAD_H
